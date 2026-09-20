/****************************************************************************
 * Contest 2026 team 208 - low-latency streaming audio service
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/audio/audio.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <system/nxplayer.h>
#include <system/nxrecorder.h>

#if defined(CONFIG_ARCH_BOARD_R528S3_GEMINI_S1) && \
    defined(CONFIG_COMPONENTS_AW_TINY_ALSA_LIB)
/* This public tiny-ALSA control entry point is supplied by the R528 board
 * audio component.  Its header is not exported through the app include path.
 */

extern int snd_ctl_set(FAR const char *name, FAR const char *element,
                       unsigned int value);
extern int snd_ctl_set_bynum(FAR const char *name,
                             const unsigned int element,
                             unsigned int value);
#  define GS1_AUDIO_HAVE_CODEC_CONTROLS 1
#endif

#include "gs1_audio.h"
#include "gs1_storage.h"

#define GS1_NX_STATE_IDLE       0
#define GS1_AUDIO_PUMP_SLEEP_US 5000
#define GS1_AUDIO_PUMP_CHUNK    2048
#define GS1_AUDIO_PIPE_BYTES    16384
#define GS1_AUDIO_BACKEND_START_TIMEOUT_MS 1500
#define GS1_AUDIO_BACKEND_START_POLL_US    5000
#ifdef CONFIG_NXPLAYER_MSG_PRIO
#  define GS1_AUDIO_NXPLAYER_MSG_PRIO CONFIG_NXPLAYER_MSG_PRIO
#else
#  define GS1_AUDIO_NXPLAYER_MSG_PRIO 1
#endif

#ifdef GS1_AUDIO_HAVE_CODEC_CONTROLS
#  define GS1_AUDIO_CODEC_CARD          "audiocodec"
#  define GS1_AUDIO_CODEC_HP_ROUTE      3u
#  define GS1_AUDIO_CODEC_HPOUT_GAIN    2u
#  define GS1_AUDIO_CODEC_MIC_GAIN      19u
#  define GS1_AUDIO_CODEC_ADC_VOLUME    160u
#  define GS1_AUDIO_CODEC_DACL_ID        6u
#  define GS1_AUDIO_CODEC_DACR_ID        7u
#endif

#ifdef GS1_AUDIO_HAVE_CODEC_CONTROLS
static void gs1_audio_codec_set(FAR const char *control, unsigned int value)
{
  /* The platform worker serializes stream changes.  These controls configure
   * the codec before nxplayer/nxrecorder opens its PCM endpoint; an older
   * codec without one of the controls must not make the audio request fail.
   */

  (void)snd_ctl_set(GS1_AUDIO_CODEC_CARD, control, value);
}

static void gs1_audio_apply_output_volume(uint8_t percent)
{
  unsigned int codec_value;

  if (percent > 100)
    {
      percent = 100;
    }

  codec_value = (unsigned int)percent * 0xffu / 100u;
  (void)snd_ctl_set_bynum(GS1_AUDIO_CODEC_CARD,
                          GS1_AUDIO_CODEC_DACL_ID, codec_value);
  (void)snd_ctl_set_bynum(GS1_AUDIO_CODEC_CARD,
                          GS1_AUDIO_CODEC_DACR_ID, codec_value);
}
#else
static void gs1_audio_apply_output_volume(uint8_t percent)
{
  (void)percent;
}
#endif

static void gs1_audio_configure_capture_route(void)
{
#ifdef GS1_AUDIO_HAVE_CODEC_CONTROLS
  /* Match the mono setup in the R528 lower half, but establish the mixer
   * state before nxrecorder starts the stream.
   */

  gs1_audio_codec_set("MIC1 gain volume", GS1_AUDIO_CODEC_MIC_GAIN);
  gs1_audio_codec_set("MIC1 input switch", 1u);
  gs1_audio_codec_set("ADC1_2 digital volume switch", 1u);
  gs1_audio_codec_set("ADC1 digital volume", GS1_AUDIO_CODEC_ADC_VOLUME);
#endif
}

static void gs1_audio_configure_playback_route(uint8_t volume)
{
#ifdef GS1_AUDIO_HAVE_CODEC_CONTROLS
  /* R528 route 3 is the codec's headphone-only path.  It must be selected
   * before the lower half evaluates the DAPM route on stream start.
   */

  gs1_audio_codec_set("HPOUT gain volume", GS1_AUDIO_CODEC_HPOUT_GAIN);
  gs1_audio_codec_set("Playback audio route", GS1_AUDIO_CODEC_HP_ROUTE);
#endif
  gs1_audio_apply_output_volume(volume);
}

static uint32_t gs1_audio_now_ms(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void gs1_audio_backend_arm(FAR struct gs1_audio_context_s *context)
{
  context->started_ms = gs1_audio_now_ms();
  context->backend_start_deadline_ms =
    context->started_ms + GS1_AUDIO_BACKEND_START_TIMEOUT_MS;
  context->backend_started = false;
}

static bool gs1_audio_backend_start_pending(
    FAR struct gs1_audio_context_s *context, int state, uint32_t now)
{
  if (state != GS1_NX_STATE_IDLE)
    {
      context->backend_started = true;
      return false;
    }

  return !context->backend_started &&
         (int32_t)(now - context->backend_start_deadline_ms) < 0;
}

static int gs1_audio_wait_backend_start(
    FAR struct gs1_audio_context_s *context, FAR volatile int *state)
{
  uint32_t now;

  for (;;)
    {
      if (*state != GS1_NX_STATE_IDLE)
        {
          context->backend_started = true;
          return 0;
        }

      now = gs1_audio_now_ms();
      if ((int32_t)(now - context->backend_start_deadline_ms) >= 0)
        {
          return -ETIMEDOUT;
        }

      usleep(GS1_AUDIO_BACKEND_START_POLL_US);
    }
}

static int gs1_audio_cancel_player(FAR struct nxplayer_s *player)
{
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  struct audio_msg_s message;
  int state;
  int ret;

  /* nxplayer_stop() intentionally does nothing while the play thread is
   * still prefilling and reports IDLE.  Waiting for PLAYING here delayed a
   * XiaoZhi barge-in by the full backend-start timeout.  Queue STOP directly
   * during that short window; the play thread consumes it as soon as prefill
   * completes (or the interrupted FIFO reaches EOF).
   */

  pthread_mutex_lock(&player->mutex);
  state = player->state;
  pthread_mutex_unlock(&player->mutex);
  if (state != GS1_NX_STATE_IDLE)
    {
      return nxplayer_stop(player);
    }

  memset(&message, 0, sizeof(message));
  message.msg_id = AUDIO_MSG_STOP;
  ret = mq_send(player->mq, (FAR const char *)&message, sizeof(message),
                GS1_AUDIO_NXPLAYER_MSG_PRIO);
  if (ret < 0)
    {
      ret = -errno;
      if (ret == -EBADF || ret == -ENOENT)
        {
          return 0;
        }
    }

  return ret;
#else
  (void)player;
  return -ENOSYS;
#endif
}

static void gs1_audio_backend_reset(FAR struct gs1_audio_context_s *context)
{
  context->backend_start_deadline_ms = 0;
  context->backend_started = false;
}

static uint8_t gs1_audio_device_count(void)
{
  FAR DIR *directory;
  FAR struct dirent *entry;
  uint8_t count = 0;

  directory = opendir("/dev/audio");
  if (directory == NULL)
    {
      return 0;
    }

  while ((entry = readdir(directory)) != NULL)
    {
      if (entry->d_name[0] != '.' && count < UINT8_MAX)
        {
          count++;
        }
    }

  closedir(directory);
  return count;
}

static bool gs1_audio_data_path_valid(FAR const char *path)
{
  FAR const char *cursor;

  if (path == NULL || strncmp(path, "/data/", 6) != 0)
    {
      return false;
    }

  for (cursor = path; *cursor != '\0'; cursor++)
    {
      if (cursor[0] == '.' && cursor[1] == '.' &&
          (cursor == path || cursor[-1] == '/') &&
          (cursor[2] == '/' || cursor[2] == '\0'))
        {
          return false;
        }
    }

  return true;
}

static int gs1_audio_write_all(int fd, FAR const uint8_t *data, size_t size)
{
  ssize_t written;

  while (size > 0)
    {
      written = write(fd, data, size);
      if (written < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (written == 0)
        {
          return -EIO;
        }

      data += written;
      size -= written;
    }

  return 0;
}

static size_t gs1_audio_frame_bytes(FAR struct gs1_audio_context_s *context)
{
  size_t bytes = context->channels * ((context->bits_per_sample + 7) / 8);
  return bytes > 0 ? bytes : 1;
}

static void gs1_audio_ring_reset(FAR struct gs1_audio_context_s *context)
{
  pthread_mutex_lock(&context->stream_lock);
  context->stream_head = 0;
  context->stream_size = 0;
  context->stream_total_bytes = 0;
  context->stream_dropped_bytes = 0;
  context->retention_error = 0;
  pthread_mutex_unlock(&context->stream_lock);
}

static void gs1_audio_ring_push(FAR struct gs1_audio_context_s *context,
                                FAR const uint8_t *data, size_t size)
{
  size_t capacity = sizeof(context->stream_buffer);
  size_t frame_bytes = gs1_audio_frame_bytes(context);
  size_t drop;
  size_t tail;
  size_t first;

  size -= size % frame_bytes;
  if (size == 0)
    {
      return;
    }

  pthread_mutex_lock(&context->stream_lock);
  context->stream_total_bytes += size;
  if (size >= capacity)
    {
      size_t keep = capacity - capacity % frame_bytes;
      context->stream_dropped_bytes += context->stream_size + size - keep;
      data += size - keep;
      size = keep;
      context->stream_head = 0;
      context->stream_size = 0;
    }
  else if (context->stream_size + size > capacity)
    {
      drop = context->stream_size + size - capacity;
      drop = (drop + frame_bytes - 1) / frame_bytes * frame_bytes;
      if (drop > context->stream_size)
        {
          drop = context->stream_size;
        }

      context->stream_head = (context->stream_head + drop) % capacity;
      context->stream_size -= drop;
      context->stream_dropped_bytes += drop;
    }

  tail = (context->stream_head + context->stream_size) % capacity;
  first = size < capacity - tail ? size : capacity - tail;
  memcpy(context->stream_buffer + tail, data, first);
  if (size > first)
    {
      memcpy(context->stream_buffer, data + first, size - first);
    }

  context->stream_size += size;
  pthread_mutex_unlock(&context->stream_lock);
}

static void gs1_audio_retention_failed(
    FAR struct gs1_audio_context_s *context, int error)
{
  pthread_mutex_lock(&context->stream_lock);
  if (context->retention_error == 0)
    {
      context->retention_error = error;
    }

  pthread_mutex_unlock(&context->stream_lock);
  if (context->retention_fd >= 0)
    {
      close(context->retention_fd);
      context->retention_fd = -1;
    }

  gs1_storage_discard_recording(context->part_path);
}

static void gs1_audio_pump_chunk(FAR struct gs1_audio_context_s *context,
                                 FAR const uint8_t *data, size_t size)
{
  int ret;

  gs1_audio_ring_push(context, data, size);
  if (context->retention_fd >= 0)
    {
      ret = gs1_audio_write_all(context->retention_fd, data, size);
      if (ret < 0)
        {
          gs1_audio_retention_failed(context, ret);
        }
    }
}

static FAR void *gs1_audio_pump(FAR void *arg)
{
  FAR struct gs1_audio_context_s *context = arg;
  uint8_t data[GS1_AUDIO_PUMP_CHUNK];
  ssize_t size;

  for (;;)
    {
      size = read(context->stream_fd, data, sizeof(data));
      if (size > 0)
        {
          gs1_audio_pump_chunk(context, data, size);
          continue;
        }

      if (size < 0 && errno == EINTR)
        {
          continue;
        }

      if (context->pump_stop)
        {
          break;
        }

      usleep(GS1_AUDIO_PUMP_SLEEP_US);
    }

  /* The writer has stopped.  Drain bytes already buffered by the FIFO. */

  for (;;)
    {
      size = read(context->stream_fd, data, sizeof(data));
      if (size <= 0)
        {
          break;
        }

      gs1_audio_pump_chunk(context, data, size);
    }

  return NULL;
}

static void gs1_audio_stream_snapshot(
    FAR struct gs1_audio_context_s *context,
    FAR struct gs1_audio_status_s *status)
{
  pthread_mutex_lock(&context->stream_lock);
  status->stream_buffered_bytes = context->stream_size;
  status->stream_total_bytes = context->stream_total_bytes;
  status->stream_dropped_bytes = context->stream_dropped_bytes;
  status->bytes = context->stream_total_bytes;
  if (context->retention_error < 0)
    {
      status->last_error = context->retention_error;
    }

  pthread_mutex_unlock(&context->stream_lock);
}

static void gs1_audio_close_stream(FAR struct gs1_audio_context_s *context)
{
  if (context->pump_started)
    {
      context->pump_stop = true;
      pthread_join(context->pump_thread, NULL);
      context->pump_started = false;
    }

  if (context->stream_fd >= 0)
    {
      close(context->stream_fd);
      context->stream_fd = -1;
    }

  if (context->fifo_path[0] != '\0')
    {
      unlink(context->fifo_path);
      context->fifo_path[0] = '\0';
    }
}

void gs1_audio_initialize(FAR struct gs1_audio_context_s *context,
                          FAR struct gs1_audio_status_s *status)
{
  memset(context, 0, sizeof(*context));
  memset(status, 0, sizeof(*status));
  context->stream_fd = -1;
  context->retention_fd = -1;
  context->volume = 70;
  pthread_mutex_init(&context->stream_lock, NULL);
  status->state = GS1_AUDIO_IDLE;
}

int gs1_audio_record_start(
    FAR struct gs1_audio_context_s *context,
    FAR const struct gs1_audio_record_request_s *request,
    FAR struct gs1_audio_status_s *status)
{
  int ret;

  if (context == NULL || request == NULL || status == NULL ||
      request->channels == 0 || request->bits_per_sample == 0 ||
      request->sample_rate == 0)
    {
      return -EINVAL;
    }

  if (context->recorder != NULL || context->player != NULL)
    {
      return -EBUSY;
    }

  gs1_audio_configure_capture_route();
  context->sample_rate = request->sample_rate;
  context->channels = request->channels;
  context->bits_per_sample = request->bits_per_sample;
  context->channel_map = request->channel_map;
  context->retain_file = request->retain_file;
  context->part_path[0] = '\0';
  context->final_path[0] = '\0';
  gs1_audio_ring_reset(context);

  if (context->retain_file)
    {
      ret = gs1_storage_new_recording(request->basename, context->part_path,
                                      sizeof(context->part_path),
                                      context->final_path,
                                      sizeof(context->final_path));
      if (ret < 0)
        {
          goto failed;
        }

      context->retention_fd = open(context->part_path,
                                   O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (context->retention_fd < 0)
        {
          ret = -errno;
          goto failed;
        }
    }

  snprintf(context->fifo_path, sizeof(context->fifo_path),
           "/var/gs1cap_%lx", (unsigned long)getpid());
  unlink(context->fifo_path);
  if (mkfifo(context->fifo_path, 0600) < 0)
    {
      ret = -errno;
      goto failed;
    }

  context->stream_fd = open(context->fifo_path, O_RDONLY | O_NONBLOCK);
  if (context->stream_fd < 0)
    {
      ret = -errno;
      goto failed;
    }

#ifdef F_SETPIPE_SZ
  fcntl(context->stream_fd, F_SETPIPE_SZ, GS1_AUDIO_PIPE_BYTES);
#endif

  context->pump_stop = false;
  ret = pthread_create(&context->pump_thread, NULL, gs1_audio_pump, context);
  if (ret != 0)
    {
      ret = -ret;
      goto failed;
    }

  context->pump_started = true;
  context->recorder = nxrecorder_create();
  if (context->recorder == NULL)
    {
      ret = -ENOMEM;
      goto failed;
    }

  ret = nxrecorder_setdevice(
    context->recorder,
    CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_CAPTURE_DEVICE);
  if (ret < 0)
    {
      nxrecorder_release(context->recorder);
      context->recorder = NULL;
      goto failed;
    }

  gs1_audio_backend_arm(context);
  ret = nxrecorder_recordinternal(context->recorder, context->fifo_path,
                                  AUDIO_FMT_PCM, request->channels,
                                  request->bits_per_sample,
                                  request->sample_rate,
                                  request->channel_map);
  if (ret < 0)
    {
      nxrecorder_release(context->recorder);
      context->recorder = NULL;
      goto failed;
    }

  context->deadline_ms = request->limit_ms == 0 ? 0 :
                         context->started_ms + request->limit_ms;
  status->state = GS1_AUDIO_RECORDING;
  status->streaming = true;
  status->sample_rate = request->sample_rate;
  status->channels = request->channels;
  status->bits_per_sample = request->bits_per_sample;
  status->elapsed_ms = 0;
  status->bytes = 0;
  status->stream_total_bytes = 0;
  status->stream_dropped_bytes = 0;
  status->stream_buffered_bytes = 0;
  status->last_error = 0;
  snprintf(status->current_path, sizeof(status->current_path), "%s",
           context->retain_file ? context->part_path : "pcm-stream");
  return 0;

failed:
  gs1_audio_backend_reset(context);
  gs1_audio_close_stream(context);
  if (context->retention_fd >= 0)
    {
      close(context->retention_fd);
      context->retention_fd = -1;
    }

  if (context->part_path[0] != '\0')
    {
      gs1_storage_discard_recording(context->part_path);
    }

  status->state = GS1_AUDIO_ERROR;
  status->streaming = false;
  status->last_error = ret;
  return ret;
}

int gs1_audio_record_stop(FAR struct gs1_audio_context_s *context,
                          FAR struct gs1_audio_status_s *status)
{
  struct stat file_stat;
  int retention_error;
  int ret;

  if (context == NULL || status == NULL)
    {
      return -EINVAL;
    }

  if (context->recorder == NULL)
    {
      return status->state == GS1_AUDIO_RECORDING ?
             GS1_ERROR_INVALID_STATE : 0;
    }

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  ret = gs1_audio_wait_backend_start(context, &context->recorder->state);
  if (ret == 0)
    {
      ret = nxrecorder_stop(context->recorder);
    }
#else
  ret = -ENOSYS;
#endif
  nxrecorder_release(context->recorder);
  context->recorder = NULL;
  gs1_audio_close_stream(context);
  gs1_audio_stream_snapshot(context, status);

  pthread_mutex_lock(&context->stream_lock);
  retention_error = context->retention_error;
  pthread_mutex_unlock(&context->stream_lock);

  if (context->retention_fd >= 0)
    {
      if (fsync(context->retention_fd) < 0 && ret == 0)
        {
          ret = -errno;
        }

      if (close(context->retention_fd) < 0 && ret == 0)
        {
          ret = -errno;
        }

      context->retention_fd = -1;
    }

  if (ret == 0 && retention_error < 0)
    {
      ret = retention_error;
    }

  if (ret == 0 && context->retain_file)
    {
      ret = gs1_storage_publish_recording(context->part_path,
                                          context->final_path);
    }

  if (ret < 0 && context->part_path[0] != '\0')
    {
      gs1_storage_discard_recording(context->part_path);
    }

  status->state = ret < 0 ? GS1_AUDIO_ERROR : GS1_AUDIO_IDLE;
  status->streaming = false;
  status->elapsed_ms = gs1_audio_now_ms() - context->started_ms;
  status->current_path[0] = '\0';
  if (ret == 0 && context->retain_file)
    {
      snprintf(status->last_recording_path,
               sizeof(status->last_recording_path), "%s",
               context->final_path);
      if (stat(context->final_path, &file_stat) == 0)
        {
          status->bytes = file_stat.st_size;
        }
    }

  status->last_error = ret;
  context->deadline_ms = 0;
  gs1_audio_backend_reset(context);
  return ret;
}

int gs1_audio_play(FAR struct gs1_audio_context_s *context,
                   FAR const struct gs1_audio_play_request_s *request,
                   FAR struct gs1_audio_status_s *status)
{
  struct stat file_stat;
  int ret;

  if (context == NULL || request == NULL || status == NULL ||
      !gs1_audio_data_path_valid(request->path) ||
      request->channels == 0 || request->bits_per_sample == 0 ||
      request->sample_rate == 0)
    {
      return !gs1_audio_data_path_valid(request != NULL ? request->path :
                                                          NULL) ?
             GS1_ERROR_UNSAFE_PATH : -EINVAL;
    }

  if (context->recorder != NULL || context->player != NULL)
    {
      return -EBUSY;
    }

  gs1_audio_configure_playback_route(context->volume);
  context->player = nxplayer_create();
  if (context->player == NULL)
    {
      status->state = GS1_AUDIO_ERROR;
      status->last_error = -ENOMEM;
      return -ENOMEM;
    }

  ret = nxplayer_setdevice(
    context->player,
    CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_PLAYBACK_DEVICE);
  if (ret < 0)
    {
      nxplayer_release(context->player);
      context->player = NULL;
      status->state = GS1_AUDIO_ERROR;
      status->last_error = ret;
      return ret;
    }

  gs1_audio_backend_arm(context);
  ret = nxplayer_playraw(context->player, request->path, AUDIO_FMT_PCM,
                         AUDIO_FMT_UNDEF, request->channels,
                         request->bits_per_sample, request->sample_rate,
                         request->channel_map);
  if (ret < 0)
    {
      nxplayer_release(context->player);
      context->player = NULL;
      gs1_audio_backend_reset(context);
      status->state = GS1_AUDIO_ERROR;
      status->last_error = ret;
      return ret;
    }

  context->deadline_ms = request->limit_ms == 0 ? 0 :
                         context->started_ms + request->limit_ms;
  status->state = GS1_AUDIO_PLAYING;
  status->streaming = false;
  status->sample_rate = request->sample_rate;
  status->channels = request->channels;
  status->bits_per_sample = request->bits_per_sample;
  status->elapsed_ms = 0;
  status->last_error = 0;
  snprintf(status->current_path, sizeof(status->current_path), "%s",
           request->path);
  if (stat(request->path, &file_stat) == 0)
    {
      status->bytes = file_stat.st_size;
    }

  return 0;
}

int gs1_audio_stop(FAR struct gs1_audio_context_s *context,
                   FAR struct gs1_audio_status_s *status)
{
  int ret = 0;

  if (context == NULL || status == NULL)
    {
      return -EINVAL;
    }

  if (context->recorder != NULL)
    {
      return gs1_audio_record_stop(context, status);
    }

  if (context->player != NULL)
    {
      ret = gs1_audio_cancel_player(context->player);
      nxplayer_release(context->player);
      context->player = NULL;
    }

  status->state = ret < 0 ? GS1_AUDIO_ERROR : GS1_AUDIO_IDLE;
  status->streaming = false;
  status->elapsed_ms = gs1_audio_now_ms() - context->started_ms;
  status->current_path[0] = '\0';
  status->last_error = ret;
  context->deadline_ms = 0;
  gs1_audio_backend_reset(context);
  return ret;
}

int gs1_audio_set_volume(FAR struct gs1_audio_context_s *context,
                         uint8_t percent)
{
  if (context == NULL || percent > 100)
    {
      return -EINVAL;
    }

  context->volume = percent;
  gs1_audio_apply_output_volume(percent);
  return 0;
}

void gs1_audio_tick(FAR struct gs1_audio_context_s *context,
                    FAR struct gs1_audio_status_s *status)
{
  uint32_t now;

  if (context == NULL || status == NULL)
    {
      return;
    }

  if (!context->device_probe_done)
    {
      status->device_count = gs1_audio_device_count();
      context->device_probe_done = true;
    }

  now = gs1_audio_now_ms();
  if (context->recorder != NULL)
    {
      status->elapsed_ms = now - context->started_ms;
      gs1_audio_stream_snapshot(context, status);
      if (context->recorder->state == GS1_NX_STATE_IDLE)
        {
          if (gs1_audio_backend_start_pending(
                context, context->recorder->state, now))
            {
              return;
            }

          nxrecorder_release(context->recorder);
          context->recorder = NULL;
          gs1_audio_close_stream(context);
          if (context->retention_fd >= 0)
            {
              close(context->retention_fd);
              context->retention_fd = -1;
            }

          if (context->part_path[0] != '\0')
            {
              gs1_storage_discard_recording(context->part_path);
            }

          status->state = GS1_AUDIO_ERROR;
          status->streaming = false;
          status->last_error = context->backend_started ? -EIO :
                                                        -ETIMEDOUT;
          status->current_path[0] = '\0';
          context->deadline_ms = 0;
          gs1_audio_backend_reset(context);
        }
      else
        {
          context->backend_started = true;
        }

      if (context->recorder == NULL)
        {
          return;
        }

      if (context->deadline_ms != 0 &&
          (int32_t)(now - context->deadline_ms) >= 0)
        {
          gs1_audio_record_stop(context, status);
        }
    }
  else if (context->player != NULL)
    {
      status->elapsed_ms = now - context->started_ms;
      if (context->player->state == GS1_NX_STATE_IDLE)
        {
          if (gs1_audio_backend_start_pending(
                context, context->player->state, now))
            {
              return;
            }

          nxplayer_release(context->player);
          context->player = NULL;
          status->state = context->backend_started ? GS1_AUDIO_IDLE :
                                                     GS1_AUDIO_ERROR;
          status->current_path[0] = '\0';
          status->last_error = context->backend_started ? 0 : -ETIMEDOUT;
          context->deadline_ms = 0;
          gs1_audio_backend_reset(context);
        }
      else
        {
          context->backend_started = true;
        }

      if (context->player == NULL)
        {
          return;
        }

      if (context->deadline_ms != 0 &&
          (int32_t)(now - context->deadline_ms) >= 0)
        {
          gs1_audio_stop(context, status);
        }
    }
}

int gs1_audio_stream_read(FAR struct gs1_audio_context_s *context,
                          FAR void *buffer, size_t capacity,
                          FAR size_t *bytes_read)
{
  size_t frame_bytes;
  size_t count;
  size_t first;

  if (context == NULL || buffer == NULL || bytes_read == NULL)
    {
      return -EINVAL;
    }

  frame_bytes = gs1_audio_frame_bytes(context);
  if (capacity < frame_bytes)
    {
      return -EMSGSIZE;
    }

  pthread_mutex_lock(&context->stream_lock);
  count = capacity < context->stream_size ? capacity : context->stream_size;
  count -= count % frame_bytes;
  first = count < sizeof(context->stream_buffer) - context->stream_head ?
          count : sizeof(context->stream_buffer) - context->stream_head;
  memcpy(buffer, context->stream_buffer + context->stream_head, first);
  if (count > first)
    {
      memcpy((FAR uint8_t *)buffer + first, context->stream_buffer,
             count - first);
    }

  context->stream_head = (context->stream_head + count) %
                         sizeof(context->stream_buffer);
  context->stream_size -= count;
  pthread_mutex_unlock(&context->stream_lock);
  *bytes_read = count;
  return 0;
}

void gs1_audio_shutdown(FAR struct gs1_audio_context_s *context,
                        FAR struct gs1_audio_status_s *status)
{
  if (context == NULL || status == NULL)
    {
      return;
    }

  if (context->recorder != NULL || context->player != NULL)
    {
      gs1_audio_stop(context, status);
    }

  pthread_mutex_destroy(&context->stream_lock);
}
