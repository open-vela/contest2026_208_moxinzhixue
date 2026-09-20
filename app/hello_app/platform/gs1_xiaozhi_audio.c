/****************************************************************************
 * Contest 2026 team 208 - Gemini-S1 XiaoZhi audio adapter
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <opus.h>

#include "gs1_platform.h"
#include "gs1_xiaozhi_audio.h"

#define GS1_XIAOZHI_CAPTURE_RATE       16000u
#define GS1_XIAOZHI_CAPTURE_CHANNELS   1u
#define GS1_XIAOZHI_CAPTURE_BITS       16u
#define GS1_XIAOZHI_FRAME_MS           20u
#define GS1_XIAOZHI_FRAME_SAMPLES \
  (GS1_XIAOZHI_CAPTURE_RATE * GS1_XIAOZHI_FRAME_MS / 1000u)
#define GS1_XIAOZHI_RECORD_LIMIT_MS    60000u
#define GS1_XIAOZHI_PLAY_LIMIT_MS      60000u
#define GS1_XIAOZHI_OPUS_BITRATE       24000
#define GS1_XIAOZHI_OPUS_COMPLEXITY    5
#define GS1_XIAOZHI_DECODE_MAX_MS      120u
#define GS1_XIAOZHI_PLAYBACK_RATE \
  CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_SAMPLE_RATE
#define GS1_XIAOZHI_PIPE_BYTES          65535
#define GS1_XIAOZHI_STREAM_BUFFER_BYTES 262144
#define GS1_XIAOZHI_STREAM_WAIT_US      1000
#define GS1_XIAOZHI_STREAM_WAIT_COUNT   50
#define GS1_XIAOZHI_DATA_DIR           "/data/gemini-s1"
#define GS1_XIAOZHI_REPLY_PATH \
  GS1_XIAOZHI_DATA_DIR "/xiaozhi_reply.pcm.part"
#define GS1_XIAOZHI_DIAGNOSTIC_PATH \
  GS1_XIAOZHI_DATA_DIR "/xiaozhi_audio.log"

struct gs1_xiaozhi_audio_s
{
  FAR OpusEncoder *encoder;
  FAR OpusDecoder *decoder;
  FAR int16_t *decode_pcm;
  FAR uint8_t *stream_buffer;
  size_t decode_capacity_samples;
  size_t stream_head;
  size_t stream_size;
  int16_t capture_pcm[GS1_XIAOZHI_FRAME_SAMPLES];
  size_t capture_bytes;
  int reply_fd;
  size_t reply_bytes;
  uint32_t capture_request_id;
  uint32_t play_request_id;
  uint32_t playback_rate;
  uint16_t playback_frame_ms;
  uint8_t playback_channels;
  int capture_error;
  int playback_error;
  bool capture_requested;
  bool capture_started;
  bool play_request_pending;
  bool play_started;
  bool finish_requested;
  char error_text[192];
};

static bool gs1_xiaozhi_opus_rate_valid(uint32_t rate)
{
  return rate == 8000 || rate == 12000 || rate == 16000 ||
         rate == 24000 || rate == 48000;
}

static bool gs1_xiaozhi_frame_ms_valid(uint16_t frame_ms)
{
  return frame_ms == 5 || frame_ms == 10 || frame_ms == 20 ||
         frame_ms == 40 || frame_ms == 60;
}

static bool gs1_xiaozhi_request_completed(uint32_t completed,
                                          uint32_t request_id)
{
  return completed != 0 && request_id != 0 &&
         (int32_t)(completed - request_id) >= 0;
}

static bool gs1_xiaozhi_reply_playing(
    FAR const struct gs1_platform_status_s *status)
{
  return status->audio.state == GS1_AUDIO_PLAYING &&
         strcmp(status->audio.current_path, GS1_XIAOZHI_REPLY_PATH) == 0;
}

static void gs1_xiaozhi_diag(FAR const char *format, ...)
{
  char text[256];
  va_list arguments;
  int fd;
  int length;

  va_start(arguments, format);
  length = vsnprintf(text, sizeof(text) - 1, format, arguments);
  va_end(arguments);
  if (length < 0)
    {
      return;
    }

  if ((size_t)length >= sizeof(text) - 1)
    {
      length = sizeof(text) - 2;
    }

  text[length++] = '\n';
  fd = open(GS1_XIAOZHI_DIAGNOSTIC_PATH,
            O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd >= 0)
    {
      (void)write(fd, text, length);
      (void)close(fd);
    }
}

static void gs1_xiaozhi_set_error(FAR struct gs1_xiaozhi_audio_s *audio,
                                  bool capture, int error,
                                  FAR const char *format, ...)
{
  va_list arguments;

  if (capture)
    {
      audio->capture_error = error;
    }
  else
    {
      audio->playback_error = error;
    }

  va_start(arguments, format);
  vsnprintf(audio->error_text, sizeof(audio->error_text), format, arguments);
  va_end(arguments);
  gs1_xiaozhi_diag("%s error=%d %s", capture ? "capture" : "playback",
                   error, audio->error_text);
}

static int gs1_xiaozhi_stream_pump(
    FAR struct gs1_xiaozhi_audio_s *audio)
{
  size_t contiguous;
  ssize_t written;

  if (audio->reply_fd < 0)
    {
      return audio->stream_size == 0 ? 0 : -EPIPE;
    }

  while (audio->stream_size > 0)
    {
      contiguous = GS1_XIAOZHI_STREAM_BUFFER_BYTES - audio->stream_head;
      if (contiguous > audio->stream_size)
        {
          contiguous = audio->stream_size;
        }

      written = write(audio->reply_fd,
                      audio->stream_buffer + audio->stream_head,
                      contiguous);
      if (written < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              return 0;
            }

          return -errno;
        }

      if (written == 0)
        {
          return 0;
        }

      audio->stream_head = (audio->stream_head + written) %
                           GS1_XIAOZHI_STREAM_BUFFER_BYTES;
      audio->stream_size -= written;
    }

  return 0;
}

static int gs1_xiaozhi_stream_append(
    FAR struct gs1_xiaozhi_audio_s *audio, FAR const void *buffer,
    size_t length)
{
  FAR const uint8_t *source = buffer;
  size_t tail;
  size_t first;
  unsigned int wait_count;
  int ret;

  if (length > GS1_XIAOZHI_STREAM_BUFFER_BYTES)
    {
      return -EMSGSIZE;
    }

  for (wait_count = 0;
       length > GS1_XIAOZHI_STREAM_BUFFER_BYTES - audio->stream_size;
       wait_count++)
    {
      ret = gs1_xiaozhi_stream_pump(audio);
      if (ret < 0)
        {
          return ret;
        }

      if (length <= GS1_XIAOZHI_STREAM_BUFFER_BYTES - audio->stream_size)
        {
          break;
        }

      if (wait_count >= GS1_XIAOZHI_STREAM_WAIT_COUNT)
        {
          return -ENOBUFS;
        }

      usleep(GS1_XIAOZHI_STREAM_WAIT_US);
    }

  tail = (audio->stream_head + audio->stream_size) %
         GS1_XIAOZHI_STREAM_BUFFER_BYTES;
  first = length;
  if (first > GS1_XIAOZHI_STREAM_BUFFER_BYTES - tail)
    {
      first = GS1_XIAOZHI_STREAM_BUFFER_BYTES - tail;
    }

  memcpy(audio->stream_buffer + tail, source, first);
  if (first < length)
    {
      memcpy(audio->stream_buffer, source + first, length - first);
    }

  audio->stream_size += length;
  return gs1_xiaozhi_stream_pump(audio);
}

static int gs1_xiaozhi_close_reply_fd(
    FAR struct gs1_xiaozhi_audio_s *audio)
{
  int ret = 0;

  if (audio->reply_fd >= 0)
    {
      if (close(audio->reply_fd) < 0)
        {
          ret = -errno;
        }

      audio->reply_fd = -1;
    }

  return ret;
}

static void gs1_xiaozhi_close_reply(FAR struct gs1_xiaozhi_audio_s *audio,
                                    bool remove_file)
{
  if (remove_file)
    {
      unlink(GS1_XIAOZHI_REPLY_PATH);
      audio->reply_bytes = 0;
    }

  (void)gs1_xiaozhi_close_reply_fd(audio);
  audio->stream_head = 0;
  audio->stream_size = 0;
  audio->finish_requested = false;
}

static void gs1_xiaozhi_reset_playback(
  FAR struct gs1_xiaozhi_audio_s *audio, bool remove_file)
{
  gs1_xiaozhi_close_reply(audio, remove_file);
  audio->play_request_pending = false;
  audio->play_started = false;
  audio->play_request_id = 0;
  if (audio->decoder != NULL)
    {
      opus_decoder_ctl(audio->decoder, OPUS_RESET_STATE);
    }
}

static int gs1_xiaozhi_request_stop(FAR struct gs1_xiaozhi_audio_s *audio,
                                    bool capture_error)
{
  struct gs1_platform_request_s request;
  int ret;

  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_AUDIO_STOP;
  ret = gs1_platform_request(&request, NULL);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, capture_error, ret,
                            "queue audio stop failed: %d", ret);
    }

  return ret;
}

static int gs1_xiaozhi_start_capture(void *context)
{
  FAR struct gs1_xiaozhi_audio_s *audio = context;
  struct gs1_platform_request_s request;
  struct gs1_platform_status_s status;
  bool cancel_playback;
  int ret;

  if (audio == NULL)
    {
      return -EINVAL;
    }

  audio->capture_error = 0;
  audio->capture_bytes = 0;
  audio->capture_requested = false;
  audio->capture_started = false;
  audio->capture_request_id = 0;
  opus_encoder_ctl(audio->encoder, OPUS_RESET_STATE);
  cancel_playback = audio->reply_fd >= 0 || audio->stream_size > 0 ||
                    audio->play_request_pending || audio->play_started;
  gs1_xiaozhi_reset_playback(audio, true);

  memset(&status, 0, sizeof(status));
  ret = gs1_platform_get_status(&status);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, true, ret,
                            "read platform audio status failed: %d", ret);
      return ret;
    }

  if (cancel_playback || status.audio.state != GS1_AUDIO_IDLE)
    {
      ret = gs1_xiaozhi_request_stop(audio, true);
      if (ret < 0)
        {
          return ret;
        }
    }

  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_AUDIO_RECORD_START;
  request.data.audio_record.sample_rate = GS1_XIAOZHI_CAPTURE_RATE;
  request.data.audio_record.limit_ms = GS1_XIAOZHI_RECORD_LIMIT_MS;
  request.data.audio_record.retain_file = false;
  request.data.audio_record.channels = GS1_XIAOZHI_CAPTURE_CHANNELS;
  request.data.audio_record.bits_per_sample = GS1_XIAOZHI_CAPTURE_BITS;
  request.data.audio_record.channel_map = 0;
  ret = gs1_platform_request(&request, &audio->capture_request_id);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, true, ret,
                            "queue capture start failed: %d", ret);
      return ret;
    }

  audio->capture_requested = true;
  return 0;
}

static int gs1_xiaozhi_stop_capture(void *context)
{
  FAR struct gs1_xiaozhi_audio_s *audio = context;
  struct gs1_platform_request_s request;
  int ret;

  if (audio == NULL)
    {
      return -EINVAL;
    }

  audio->capture_bytes = 0;
  if (!audio->capture_requested && !audio->capture_started)
    {
      return 0;
    }

  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_AUDIO_RECORD_STOP;
  ret = gs1_platform_request(&request, NULL);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, true, ret,
                            "queue capture stop failed: %d", ret);
      return ret;
    }

  audio->capture_requested = false;
  audio->capture_started = false;
  return 0;
}

static int gs1_xiaozhi_capture_status(
    FAR struct gs1_xiaozhi_audio_s *audio)
{
  struct gs1_platform_status_s status;
  bool capture_started;
  int error;
  int ret;

  memset(&status, 0, sizeof(status));
  ret = gs1_platform_get_status(&status);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, true, ret,
                            "read capture status failed: %d", ret);
      return ret;
    }

  if (!audio->capture_requested)
    {
      return -EAGAIN;
    }

  if (status.audio.state == GS1_AUDIO_RECORDING)
    {
      audio->capture_started = true;
      return 0;
    }

  if (!gs1_xiaozhi_request_completed(
        status.runtime.last_completed_request_id,
        audio->capture_request_id))
    {
      return -EAGAIN;
    }

  error = status.audio.last_error;
  if (error == 0 &&
      status.runtime.last_completed_request_id == audio->capture_request_id)
    {
      error = status.runtime.last_error;
    }

  if (error == 0)
    {
      error = audio->capture_started ? -ETIMEDOUT : -EIO;
    }

  capture_started = audio->capture_started;
  audio->capture_requested = false;
  audio->capture_started = false;
  gs1_xiaozhi_set_error(audio, true, error,
                        capture_started ?
                        "capture stopped before push-to-talk release: %d" :
                        "capture did not start: %d", error);
  return error;
}

static int gs1_xiaozhi_read_uplink(void *context, void *buffer,
                                   size_t capacity, size_t *length)
{
  FAR struct gs1_xiaozhi_audio_s *audio = context;
  FAR uint8_t *capture = (FAR uint8_t *)audio->capture_pcm;
  size_t bytes_read;
  int encoded;
  int ret;

  if (audio == NULL || buffer == NULL || length == NULL)
    {
      return -EINVAL;
    }

  *length = 0;
  ret = gs1_xiaozhi_capture_status(audio);
  if (ret < 0)
    {
      return ret;
    }

  while (audio->capture_bytes < sizeof(audio->capture_pcm))
    {
      bytes_read = 0;
      ret = gs1_platform_audio_stream_read(
        capture + audio->capture_bytes,
        sizeof(audio->capture_pcm) - audio->capture_bytes, &bytes_read);
      if (ret < 0)
        {
          gs1_xiaozhi_set_error(audio, true, ret,
                                "read capture PCM failed: %d", ret);
          return ret;
        }

      if (bytes_read == 0)
        {
          return -EAGAIN;
        }

      audio->capture_bytes += bytes_read;
    }

  if (capacity > INT_MAX)
    {
      capacity = INT_MAX;
    }

  encoded = opus_encode(audio->encoder, audio->capture_pcm,
                        GS1_XIAOZHI_FRAME_SAMPLES, buffer,
                        (opus_int32)capacity);
  audio->capture_bytes = 0;
  if (encoded < 0)
    {
      ret = encoded == OPUS_BUFFER_TOO_SMALL ? -EMSGSIZE : -EIO;
      gs1_xiaozhi_set_error(audio, true, ret,
                            "Opus capture encode failed: %s",
                            opus_strerror(encoded));
      return ret;
    }

  *length = (size_t)encoded;
  return 0;
}

static int gs1_xiaozhi_configure_downlink(void *context,
                                          uint32_t sample_rate,
                                          uint8_t channels,
                                          uint16_t frame_ms)
{
  FAR struct gs1_xiaozhi_audio_s *audio = context;
  FAR OpusDecoder *decoder;
  FAR int16_t *decode_pcm;
  size_t capacity_samples;
  uint32_t maximum_frames;
  uint32_t playback_rate;
  int opus_error;

  if (audio == NULL || !gs1_xiaozhi_opus_rate_valid(sample_rate) ||
      channels == 0 || channels > 2 ||
      !gs1_xiaozhi_frame_ms_valid(frame_ms) ||
      !gs1_xiaozhi_opus_rate_valid(GS1_XIAOZHI_PLAYBACK_RATE))
    {
      return -EINVAL;
    }

  /* The server rate describes the encoded stream.  Decode directly to the
   * board's native PCM rate so nxplayer is never asked to configure 24 kHz.
   */

  playback_rate = GS1_XIAOZHI_PLAYBACK_RATE;
  maximum_frames = playback_rate * GS1_XIAOZHI_DECODE_MAX_MS / 1000;
  capacity_samples = (size_t)maximum_frames * channels;
  decode_pcm = malloc(capacity_samples * sizeof(*decode_pcm));
  if (decode_pcm == NULL)
    {
      gs1_xiaozhi_set_error(audio, false, -ENOMEM,
                            "allocate playback PCM failed");
      return -ENOMEM;
    }

  decoder = opus_decoder_create((opus_int32)playback_rate, channels,
                                &opus_error);
  if (decoder == NULL || opus_error != OPUS_OK)
    {
      free(decode_pcm);
      gs1_xiaozhi_set_error(audio, false, -EIO,
                            "create Opus playback decoder failed: %s",
                            opus_strerror(opus_error));
      return -EIO;
    }

  gs1_xiaozhi_reset_playback(audio, true);
  if (audio->decoder != NULL)
    {
      opus_decoder_destroy(audio->decoder);
    }

  free(audio->decode_pcm);
  audio->decoder = decoder;
  audio->decode_pcm = decode_pcm;
  audio->decode_capacity_samples = capacity_samples;
  audio->playback_rate = playback_rate;
  audio->playback_channels = channels;
  audio->playback_frame_ms = frame_ms;
  audio->playback_error = 0;
  gs1_xiaozhi_diag("downlink source=%lu output=%lu channels=%u frame=%u",
                   (unsigned long)sample_rate,
                   (unsigned long)playback_rate, channels, frame_ms);
  return 0;
}

static int gs1_xiaozhi_open_reply(FAR struct gs1_xiaozhi_audio_s *audio)
{
  int ret;

  if (audio->reply_fd >= 0)
    {
      return 0;
    }

  if (mkdir(GS1_XIAOZHI_DATA_DIR, 0700) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  unlink(GS1_XIAOZHI_REPLY_PATH);
  if (mkfifo(GS1_XIAOZHI_REPLY_PATH, 0600) < 0)
    {
      return -errno;
    }

  audio->reply_fd = open(GS1_XIAOZHI_REPLY_PATH,
                          O_RDWR | O_NONBLOCK);
  if (audio->reply_fd < 0)
    {
      ret = -errno;
      unlink(GS1_XIAOZHI_REPLY_PATH);
      return ret;
    }

#ifdef F_SETPIPE_SZ
  (void)fcntl(audio->reply_fd, F_SETPIPE_SZ, GS1_XIAOZHI_PIPE_BYTES);
#endif

  audio->stream_head = 0;
  audio->stream_size = 0;
  audio->reply_bytes = 0;
  audio->playback_error = 0;
  audio->finish_requested = false;
  return 0;
}

static int gs1_xiaozhi_start_playback(
    FAR struct gs1_xiaozhi_audio_s *audio)
{
  struct gs1_platform_request_s request;
  int ret;

  if (audio->play_request_pending || audio->play_started)
    {
      return 0;
    }

  ret = gs1_xiaozhi_open_reply(audio);
  if (ret < 0)
    {
      return ret;
    }

  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_AUDIO_PLAY;
  snprintf(request.data.audio_play.path,
           sizeof(request.data.audio_play.path), "%s",
           GS1_XIAOZHI_REPLY_PATH);
  request.data.audio_play.sample_rate = audio->playback_rate;
  request.data.audio_play.limit_ms = GS1_XIAOZHI_PLAY_LIMIT_MS;
  request.data.audio_play.channels = audio->playback_channels;
  request.data.audio_play.bits_per_sample = GS1_XIAOZHI_CAPTURE_BITS;
  request.data.audio_play.channel_map = 0;
  ret = gs1_platform_request(&request, &audio->play_request_id);
  if (ret < 0)
    {
      gs1_xiaozhi_close_reply(audio, true);
      return ret;
    }

  audio->play_request_pending = true;
  audio->play_started = false;
  gs1_xiaozhi_diag("play queued id=%lu rate=%lu channels=%u",
                   (unsigned long)audio->play_request_id,
                   (unsigned long)audio->playback_rate,
                   audio->playback_channels);
  return 0;
}

static int gs1_xiaozhi_update_playback_start(
    FAR struct gs1_xiaozhi_audio_s *audio)
{
  struct gs1_platform_status_s status;
  int error;
  int ret;

  if (!audio->play_request_pending)
    {
      return 0;
    }

  memset(&status, 0, sizeof(status));
  ret = gs1_platform_get_status(&status);
  if (ret < 0)
    {
      return ret;
    }

  if (gs1_xiaozhi_reply_playing(&status))
    {
      audio->play_request_pending = false;
      audio->play_started = true;
      gs1_xiaozhi_diag("play started id=%lu",
                       (unsigned long)audio->play_request_id);
      return 0;
    }

  if (!gs1_xiaozhi_request_completed(
        status.runtime.last_completed_request_id,
        audio->play_request_id))
    {
      return 0;
    }

  error = status.audio.last_error;
  if (status.runtime.last_completed_request_id == audio->play_request_id)
    {
      error = status.runtime.last_error;
    }

  if (error == 0)
    {
      error = -EIO;
    }

  return error;
}

static int gs1_xiaozhi_push_downlink(void *context, const void *data,
                                     size_t length)
{
  FAR struct gs1_xiaozhi_audio_s *audio = context;
  size_t pcm_bytes;
  uint32_t maximum_frames;
  int decoded_frames;
  int ret;

  if (audio == NULL || data == NULL || length == 0 || length > INT_MAX)
    {
      return -EINVAL;
    }

  if (audio->decoder == NULL || audio->decode_pcm == NULL ||
      audio->playback_rate == 0 || audio->playback_channels == 0)
    {
      return -EAGAIN;
    }

  if (audio->finish_requested)
    {
      return -EBUSY;
    }

  maximum_frames = audio->playback_rate * GS1_XIAOZHI_DECODE_MAX_MS / 1000;
  decoded_frames = opus_decode(audio->decoder, data, (opus_int32)length,
                               audio->decode_pcm, (int)maximum_frames, 0);
  if (decoded_frames < 0)
    {
      gs1_xiaozhi_set_error(audio, false, -EIO,
                            "Opus playback decode failed: %s",
                            opus_strerror(decoded_frames));
      return -EIO;
    }

  pcm_bytes = (size_t)decoded_frames * audio->playback_channels *
              sizeof(*audio->decode_pcm);
  if (pcm_bytes > audio->decode_capacity_samples *
                  sizeof(*audio->decode_pcm))
    {
      gs1_xiaozhi_set_error(audio, false, -EOVERFLOW,
                            "decoded playback frame is too large");
      return -EOVERFLOW;
    }

  ret = gs1_xiaozhi_start_playback(audio);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, false, ret,
                            "start XiaoZhi reply stream failed: %d", ret);
      return ret;
    }

  if (audio->reply_bytes == 0)
    {
      gs1_xiaozhi_diag("first packet bytes=%lu decoded=%d",
                       (unsigned long)length, decoded_frames);
    }

  ret = gs1_xiaozhi_stream_append(audio, audio->decode_pcm, pcm_bytes);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, false, ret,
                            "queue XiaoZhi reply PCM failed: %d", ret);
      (void)gs1_xiaozhi_request_stop(audio, false);
      gs1_xiaozhi_reset_playback(audio, true);
      return ret;
    }

  audio->reply_bytes += pcm_bytes;
  ret = gs1_xiaozhi_update_playback_start(audio);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, false, ret,
                            "start XiaoZhi reply playback failed: %d", ret);
      (void)gs1_xiaozhi_request_stop(audio, false);
      gs1_xiaozhi_reset_playback(audio, true);
      return ret;
    }

  return 0;
}

static int gs1_xiaozhi_finish_downlink(void *context)
{
  FAR struct gs1_xiaozhi_audio_s *audio = context;
  int ret;

  if (audio == NULL)
    {
      return -EINVAL;
    }

  if (audio->reply_bytes == 0)
    {
      gs1_xiaozhi_reset_playback(audio, true);
      return 0;
    }

  audio->finish_requested = true;
  ret = gs1_xiaozhi_stream_pump(audio);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, false, ret,
                            "finish XiaoZhi reply stream failed: %d", ret);
      (void)gs1_xiaozhi_request_stop(audio, false);
      gs1_xiaozhi_reset_playback(audio, true);
      return ret;
    }

  return 0;
}

static bool gs1_xiaozhi_playback_idle(void *context)
{
  FAR struct gs1_xiaozhi_audio_s *audio = context;
  struct gs1_platform_status_s status;
  int error;
  int ret;

  if (audio == NULL)
    {
      return true;
    }

  if (audio->reply_fd < 0 && audio->stream_size == 0 &&
      !audio->play_request_pending && !audio->play_started)
    {
      return true;
    }

  ret = gs1_xiaozhi_stream_pump(audio);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, false, ret,
                            "write XiaoZhi reply stream failed: %d", ret);
      (void)gs1_xiaozhi_request_stop(audio, false);
      gs1_xiaozhi_reset_playback(audio, true);
      return true;
    }

  memset(&status, 0, sizeof(status));
  ret = gs1_platform_get_status(&status);
  if (ret < 0)
    {
      gs1_xiaozhi_set_error(audio, false, ret,
                            "read playback status failed: %d", ret);
      gs1_xiaozhi_reset_playback(audio, true);
      return true;
    }

  if (gs1_xiaozhi_reply_playing(&status))
    {
      audio->play_request_pending = false;
      audio->play_started = true;
      ret = gs1_xiaozhi_stream_pump(audio);
      if (ret < 0)
        {
          gs1_xiaozhi_set_error(audio, false, ret,
                                "write XiaoZhi reply stream failed: %d",
                                ret);
          (void)gs1_xiaozhi_request_stop(audio, false);
          gs1_xiaozhi_reset_playback(audio, true);
          return true;
        }

      if (audio->finish_requested && audio->stream_size == 0 &&
          audio->reply_fd >= 0)
        {
          ret = gs1_xiaozhi_close_reply_fd(audio);
          if (ret < 0)
            {
              gs1_xiaozhi_set_error(
                audio, false, ret,
                "close XiaoZhi reply stream failed: %d", ret);
              (void)gs1_xiaozhi_request_stop(audio, false);
              gs1_xiaozhi_reset_playback(audio, true);
              return true;
            }
        }

      return false;
    }

  if (audio->play_request_pending &&
      !gs1_xiaozhi_request_completed(
        status.runtime.last_completed_request_id,
        audio->play_request_id))
    {
      return false;
    }

  if (audio->play_request_pending)
    {
      error = status.audio.last_error;
      if (status.runtime.last_completed_request_id ==
          audio->play_request_id)
        {
          error = status.runtime.last_error;
        }

      audio->play_request_pending = false;
      if (error < 0 || status.audio.state == GS1_AUDIO_ERROR)
        {
          if (error == 0)
            {
              error = -EIO;
            }

          gs1_xiaozhi_set_error(audio, false, error,
                                "XiaoZhi reply playback failed: %d", error);
          gs1_xiaozhi_reset_playback(audio, true);
          return true;
        }

      if (status.audio.state == GS1_AUDIO_IDLE)
        {
          gs1_xiaozhi_set_error(audio, false, -EIO,
                                "XiaoZhi reply playback did not start");
          gs1_xiaozhi_reset_playback(audio, true);
          return true;
        }

      gs1_xiaozhi_set_error(audio, false, -EIO,
                            "unexpected playback state: %d",
                            status.audio.state);
      gs1_xiaozhi_reset_playback(audio, true);
      return true;
    }

  if (audio->play_started && !gs1_xiaozhi_reply_playing(&status))
    {
      if (status.audio.state == GS1_AUDIO_ERROR)
        {
          error = status.audio.last_error != 0 ?
                  status.audio.last_error : -EIO;
          gs1_xiaozhi_set_error(audio, false, error,
                                 "XiaoZhi reply playback stopped: %d", error);
        }
      else if (!audio->finish_requested)
        {
          gs1_xiaozhi_set_error(
            audio, false, -EIO,
            "XiaoZhi reply playback stopped before TTS finished");
        }

      gs1_xiaozhi_reset_playback(audio, true);
      return true;
    }

  return false;
}

static int gs1_xiaozhi_get_error(void *context, bool capture)
{
  FAR struct gs1_xiaozhi_audio_s *audio = context;

  if (audio == NULL)
    {
      return -EINVAL;
    }

  if (capture && audio->capture_error == 0 && audio->capture_requested)
    {
      (void)gs1_xiaozhi_capture_status(audio);
    }

  return capture ? audio->capture_error : audio->playback_error;
}

static void gs1_xiaozhi_flush(void *context)
{
  FAR struct gs1_xiaozhi_audio_s *audio = context;
  struct gs1_platform_status_s status;
  bool capture;
  bool stop_needed;
  int ret = 0;

  if (audio == NULL)
    {
      return;
    }

  capture = audio->capture_requested || audio->capture_started;
  stop_needed = capture || audio->reply_fd >= 0 ||
                audio->stream_size > 0 || audio->play_request_pending ||
                audio->play_started;
  memset(&status, 0, sizeof(status));
  if (gs1_platform_get_status(&status) == 0 &&
      (status.audio.state == GS1_AUDIO_RECORDING ||
       status.audio.state == GS1_AUDIO_PLAYING))
    {
      stop_needed = true;
      if (status.audio.state == GS1_AUDIO_RECORDING)
        {
          capture = true;
        }
    }

  audio->capture_bytes = 0;
  if (stop_needed)
    {
      ret = gs1_xiaozhi_request_stop(audio, capture);
    }

  if (ret >= 0)
    {
      audio->capture_requested = false;
      audio->capture_started = false;
      audio->capture_request_id = 0;
    }
  else if (capture)
    {
      audio->capture_requested = true;
    }

  gs1_xiaozhi_reset_playback(audio, true);
}

FAR struct gs1_xiaozhi_audio_s *gs1_xiaozhi_audio_create(void)
{
  FAR struct gs1_xiaozhi_audio_s *audio;
  int opus_error;

  audio = calloc(1, sizeof(*audio));
  if (audio == NULL)
    {
      return NULL;
    }

  audio->reply_fd = -1;
  audio->encoder = opus_encoder_create(GS1_XIAOZHI_CAPTURE_RATE,
                                       GS1_XIAOZHI_CAPTURE_CHANNELS,
                                       OPUS_APPLICATION_VOIP, &opus_error);
  if (audio->encoder == NULL || opus_error != OPUS_OK)
    {
      free(audio);
      return NULL;
    }

  audio->stream_buffer = malloc(GS1_XIAOZHI_STREAM_BUFFER_BYTES);
  if (audio->stream_buffer == NULL)
    {
      opus_encoder_destroy(audio->encoder);
      free(audio);
      return NULL;
    }

  opus_encoder_ctl(audio->encoder,
                   OPUS_SET_BITRATE(GS1_XIAOZHI_OPUS_BITRATE));
  opus_encoder_ctl(audio->encoder,
                   OPUS_SET_COMPLEXITY(GS1_XIAOZHI_OPUS_COMPLEXITY));
  opus_encoder_ctl(audio->encoder, OPUS_SET_VBR(1));
  opus_encoder_ctl(audio->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  unlink(GS1_XIAOZHI_REPLY_PATH);
  return audio;
}

void gs1_xiaozhi_audio_destroy(FAR struct gs1_xiaozhi_audio_s *audio)
{
  if (audio == NULL)
    {
      return;
    }

  gs1_xiaozhi_flush(audio);
  opus_encoder_destroy(audio->encoder);
  if (audio->decoder != NULL)
    {
      opus_decoder_destroy(audio->decoder);
    }
  free(audio->decode_pcm);
  free(audio->stream_buffer);
  memset(audio, 0, sizeof(*audio));
  free(audio);
}

FAR const struct xiaozhi_audio_ops *gs1_xiaozhi_audio_ops(void)
{
  static const struct xiaozhi_audio_ops operations =
  {
    .start_capture = gs1_xiaozhi_start_capture,
    .stop_capture = gs1_xiaozhi_stop_capture,
    .read_uplink_opus = gs1_xiaozhi_read_uplink,
    .configure_downlink = gs1_xiaozhi_configure_downlink,
    .push_downlink_opus = gs1_xiaozhi_push_downlink,
    .finish_downlink = gs1_xiaozhi_finish_downlink,
    .playback_idle = gs1_xiaozhi_playback_idle,
    .get_error = gs1_xiaozhi_get_error,
    .flush = gs1_xiaozhi_flush,
  };

  return &operations;
}

FAR const char *
gs1_xiaozhi_audio_last_error(FAR const struct gs1_xiaozhi_audio_s *audio)
{
  return audio == NULL ? "invalid Gemini-S1 XiaoZhi audio adapter" :
                         audio->error_text;
}
