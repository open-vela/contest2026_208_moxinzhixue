#include "xiaozhi_audio.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>
#include <opus.h>

#define XIAOZHI_AUDIO_DEFAULT_RATE 16000
#define XIAOZHI_AUDIO_DEFAULT_FRAME_MS 60
#define XIAOZHI_AUDIO_DEFAULT_BITRATE 24000
#define XIAOZHI_AUDIO_DEFAULT_COMPLEXITY 5
#define XIAOZHI_AUDIO_DEFAULT_UPLINK_PACKETS 8
#define XIAOZHI_AUDIO_DEFAULT_DOWNLINK_PACKETS 16
#define XIAOZHI_AUDIO_THREAD_STACK_SIZE 65536

struct xiaozhi_audio_packet
{
  size_t length;
  uint8_t data[XIAOZHI_OPUS_PACKET_MAX];
};

struct xiaozhi_audio_ring
{
  struct xiaozhi_audio_packet *packets;
  size_t capacity;
  size_t head;
  size_t tail;
  size_t count;
};

struct xiaozhi_audio
{
  struct xiaozhi_audio_config config;
  char *capture_device;
  char *playback_device;
  struct xiaozhi_audio_ring uplink;
  struct xiaozhi_audio_ring downlink;
  pthread_mutex_t capture_lock;
  pthread_cond_t capture_condition;
  pthread_mutex_t playback_lock;
  pthread_cond_t playback_condition;
  pthread_mutex_t status_lock;
  pthread_t capture_thread;
  pthread_t playback_thread;
  struct xiaozhi_audio_stats stats;
  char error_text[192];
  uint32_t playback_sample_rate;
  uint16_t playback_frame_duration_ms;
  uint8_t playback_channels;
  uint32_t playback_generation;
  bool capture_thread_created;
  bool playback_thread_created;
  bool capture_active;
  bool playback_busy;
  bool playback_finish_requested;
  bool playback_draining;
  bool playback_reset;
  bool shutdown;
};

static char *xiaozhi_audio_strdup(const char *source)
{
  size_t length;
  char *copy;

  if (source == NULL)
    {
      return NULL;
    }

  length = strlen(source);
  copy = malloc(length + 1);
  if (copy != NULL)
    {
      memcpy(copy, source, length + 1);
    }

  return copy;
}

static void xiaozhi_audio_set_error(struct xiaozhi_audio *audio,
                                    bool capture, int error,
                                    const char *format, ...)
{
  va_list arguments;

  pthread_mutex_lock(&audio->status_lock);
  if (capture)
    {
      audio->stats.capture_error = error;
    }
  else
    {
      audio->stats.playback_error = error;
    }

  va_start(arguments, format);
  vsnprintf(audio->error_text, sizeof(audio->error_text), format, arguments);
  va_end(arguments);
  pthread_mutex_unlock(&audio->status_lock);
}

static void xiaozhi_audio_add_frames(struct xiaozhi_audio *audio,
                                     bool capture, uint64_t frames)
{
  pthread_mutex_lock(&audio->status_lock);
  if (capture)
    {
      audio->stats.captured_frames += frames;
    }
  else
    {
      audio->stats.played_frames += frames;
    }

  pthread_mutex_unlock(&audio->status_lock);
}

static void xiaozhi_audio_add_drop(struct xiaozhi_audio *audio,
                                   bool uplink)
{
  pthread_mutex_lock(&audio->status_lock);
  if (uplink)
    {
      audio->stats.uplink_dropped++;
    }
  else
    {
      audio->stats.downlink_dropped++;
    }

  pthread_mutex_unlock(&audio->status_lock);
}

static void xiaozhi_ring_clear(struct xiaozhi_audio_ring *ring)
{
  ring->head = 0;
  ring->tail = 0;
  ring->count = 0;
}

static void xiaozhi_ring_drop_oldest(struct xiaozhi_audio_ring *ring)
{
  if (ring->count == 0)
    {
      return;
    }

  ring->head = (ring->head + 1) % ring->capacity;
  ring->count--;
}

static int xiaozhi_ring_push(struct xiaozhi_audio_ring *ring,
                             const void *data, size_t length)
{
  struct xiaozhi_audio_packet *packet;

  if (length > XIAOZHI_OPUS_PACKET_MAX)
    {
      return -EMSGSIZE;
    }

  packet = &ring->packets[ring->tail];
  packet->length = length;
  memcpy(packet->data, data, length);
  ring->tail = (ring->tail + 1) % ring->capacity;
  ring->count++;
  return 0;
}

static int xiaozhi_ring_pop(struct xiaozhi_audio_ring *ring,
                            void *data, size_t capacity, size_t *length)
{
  struct xiaozhi_audio_packet *packet;

  if (ring->count == 0)
    {
      return -EAGAIN;
    }

  packet = &ring->packets[ring->head];
  if (packet->length > capacity)
    {
      return -EMSGSIZE;
    }

  memcpy(data, packet->data, packet->length);
  *length = packet->length;
  ring->head = (ring->head + 1) % ring->capacity;
  ring->count--;
  return 0;
}

static bool xiaozhi_opus_rate_valid(uint32_t rate)
{
  return rate == 8000 || rate == 12000 || rate == 16000 ||
         rate == 24000 || rate == 48000;
}

static bool xiaozhi_opus_frame_duration_valid(uint16_t duration_ms)
{
  return duration_ms == 5 || duration_ms == 10 || duration_ms == 20 ||
         duration_ms == 40 || duration_ms == 60;
}

static int xiaozhi_pcm_open(snd_pcm_t **handle, const char *device,
                            snd_pcm_stream_t stream, uint32_t rate,
                            uint8_t channels, uint32_t period_time_us,
                            uint32_t buffer_time_us)
{
  snd_pcm_hw_params_t *parameters;
  int ret;

  ret = snd_pcm_open(handle, device, stream, 0);
  if (ret < 0)
    {
      return ret;
    }

  snd_pcm_hw_params_alloca(&parameters);
  ret = snd_pcm_hw_params_any(*handle, parameters);
  if (ret < 0)
    {
      goto error;
    }

  ret = snd_pcm_hw_params_set_access(*handle, parameters,
                                     SND_PCM_ACCESS_RW_INTERLEAVED);
  if (ret < 0)
    {
      goto error;
    }

  ret = snd_pcm_hw_params_set_format(*handle, parameters,
                                     SND_PCM_FORMAT_S16_LE);
  if (ret < 0)
    {
      goto error;
    }

  ret = snd_pcm_hw_params_set_channels(*handle, parameters, channels);
  if (ret < 0)
    {
      goto error;
    }

  ret = snd_pcm_hw_params_set_rate(*handle, parameters, rate, 0);
  if (ret < 0)
    {
      goto error;
    }

  ret = snd_pcm_hw_params_set_period_time(*handle, parameters,
                                          period_time_us, 0);
  if (ret < 0)
    {
      goto error;
    }

  ret = snd_pcm_hw_params_set_buffer_time(*handle, parameters,
                                          buffer_time_us, 0);
  if (ret < 0)
    {
      goto error;
    }

  ret = snd_pcm_hw_params(*handle, parameters);
  if (ret < 0)
    {
      goto error;
    }

  return 0;

error:
  snd_pcm_close(*handle);
  *handle = NULL;
  return ret;
}

static bool xiaozhi_capture_enabled(struct xiaozhi_audio *audio)
{
  bool enabled;

  pthread_mutex_lock(&audio->capture_lock);
  enabled = audio->capture_active && !audio->shutdown;
  pthread_mutex_unlock(&audio->capture_lock);
  return enabled;
}

static void xiaozhi_capture_failed(struct xiaozhi_audio *audio, int error,
                                   const char *operation)
{
  xiaozhi_audio_set_error(audio, true, error, "%s failed: %s",
                          operation, snd_strerror(error));
  pthread_mutex_lock(&audio->capture_lock);
  audio->capture_active = false;
  pthread_cond_broadcast(&audio->capture_condition);
  pthread_mutex_unlock(&audio->capture_lock);
}

static int xiaozhi_enqueue_uplink(struct xiaozhi_audio *audio,
                                  const void *data, size_t length)
{
  int ret;

  pthread_mutex_lock(&audio->capture_lock);
  if (audio->uplink.count == audio->uplink.capacity)
    {
      xiaozhi_ring_drop_oldest(&audio->uplink);
      xiaozhi_audio_add_drop(audio, true);
    }

  ret = xiaozhi_ring_push(&audio->uplink, data, length);
  pthread_mutex_unlock(&audio->capture_lock);
  return ret;
}

static void *xiaozhi_capture_thread(void *argument)
{
  struct xiaozhi_audio *audio = argument;
  snd_pcm_t *pcm = NULL;
  OpusEncoder *encoder = NULL;
  int16_t *samples = NULL;
  uint8_t opus[XIAOZHI_OPUS_PACKET_MAX];
  uint32_t frame_samples;
  uint32_t read_frames;
  int opus_error;
  int ret;

  frame_samples = audio->config.capture_sample_rate *
                  audio->config.frame_duration_ms / 1000;
  samples = malloc((size_t)frame_samples * audio->config.capture_channels *
                   sizeof(*samples));
  if (samples == NULL)
    {
      xiaozhi_capture_failed(audio, -ENOMEM, "capture allocation");
      return NULL;
    }

  for (;;)
    {
      pthread_mutex_lock(&audio->capture_lock);
      while (!audio->capture_active && !audio->shutdown)
        {
          pthread_cond_wait(&audio->capture_condition,
                            &audio->capture_lock);
        }

      if (audio->shutdown)
        {
          pthread_mutex_unlock(&audio->capture_lock);
          break;
        }

      xiaozhi_ring_clear(&audio->uplink);
      pthread_mutex_unlock(&audio->capture_lock);

      ret = xiaozhi_pcm_open(&pcm, audio->capture_device,
                             SND_PCM_STREAM_CAPTURE,
                             audio->config.capture_sample_rate,
                             audio->config.capture_channels,
                             audio->config.capture_period_time_us,
                             audio->config.capture_buffer_time_us);
      if (ret < 0)
        {
          xiaozhi_capture_failed(audio, ret, "open capture PCM");
          continue;
        }

      encoder = opus_encoder_create((opus_int32)audio->config.capture_sample_rate,
                                    audio->config.capture_channels,
                                    OPUS_APPLICATION_VOIP, &opus_error);
      if (encoder == NULL || opus_error != OPUS_OK)
        {
          xiaozhi_audio_set_error(audio, true, -EIO,
                                  "opus encoder create failed: %s",
                                  opus_strerror(opus_error));
          snd_pcm_close(pcm);
          pcm = NULL;
          pthread_mutex_lock(&audio->capture_lock);
          audio->capture_active = false;
          pthread_mutex_unlock(&audio->capture_lock);
          continue;
        }

      opus_encoder_ctl(encoder, OPUS_SET_BITRATE(audio->config.opus_bitrate));
      opus_encoder_ctl(encoder,
                       OPUS_SET_COMPLEXITY(audio->config.opus_complexity));
      opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
      opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

      while (xiaozhi_capture_enabled(audio))
        {
          read_frames = 0;
          while (read_frames < frame_samples &&
                 xiaozhi_capture_enabled(audio))
            {
              ret = (int)snd_pcm_readi(
                  pcm,
                  samples + (size_t)read_frames *
                            audio->config.capture_channels,
                  frame_samples - read_frames);
              if (ret < 0)
                {
                  ret = snd_pcm_recover(pcm, ret, 0);
                  if (ret < 0)
                    {
                      xiaozhi_capture_failed(audio, ret, "capture read");
                      break;
                    }

                  continue;
                }

              read_frames += (uint32_t)ret;
            }

          if (read_frames != frame_samples ||
              !xiaozhi_capture_enabled(audio))
            {
              continue;
            }

          ret = opus_encode(encoder, samples, (int)frame_samples,
                            opus, sizeof(opus));
          if (ret < 0)
            {
              xiaozhi_audio_set_error(audio, true, -EIO,
                                      "opus encode failed: %s",
                                      opus_strerror(ret));
              continue;
            }

          if (xiaozhi_enqueue_uplink(audio, opus, (size_t)ret) == 0)
            {
              xiaozhi_audio_add_frames(audio, true, frame_samples);
            }
        }

      opus_encoder_destroy(encoder);
      encoder = NULL;
      snd_pcm_close(pcm);
      pcm = NULL;
    }

  free(samples);
  return NULL;
}

static int xiaozhi_write_pcm(snd_pcm_t *pcm, const int16_t *samples,
                             uint32_t frames, uint8_t channels)
{
  uint32_t written = 0;
  int ret;

  while (written < frames)
    {
      ret = (int)snd_pcm_writei(pcm,
                                samples + (size_t)written * channels,
                                frames - written);
      if (ret < 0)
        {
          ret = snd_pcm_recover(pcm, ret, 0);
          if (ret < 0)
            {
              return ret;
            }

          continue;
        }

      written += (uint32_t)ret;
    }

  return 0;
}

static int xiaozhi_playback_close(snd_pcm_t **pcm,
                                  OpusDecoder **decoder,
                                  int16_t **samples,
                                  bool drain)
{
  int ret = 0;

  if (*pcm != NULL)
    {
      ret = drain ? snd_pcm_drain(*pcm) : snd_pcm_drop(*pcm);
      snd_pcm_close(*pcm);
      *pcm = NULL;
    }

  if (*decoder != NULL)
    {
      opus_decoder_destroy(*decoder);
      *decoder = NULL;
    }

  free(*samples);
  *samples = NULL;
  return ret;
}

static int xiaozhi_playback_configure(struct xiaozhi_audio *audio,
                                      snd_pcm_t **pcm,
                                      OpusDecoder **decoder,
                                      int16_t **samples,
                                      uint32_t rate, uint8_t channels)
{
  uint32_t maximum_frames;
  int opus_error;
  int ret;

  xiaozhi_playback_close(pcm, decoder, samples, false);
  ret = xiaozhi_pcm_open(pcm, audio->playback_device,
                         SND_PCM_STREAM_PLAYBACK, rate, channels,
                         audio->config.playback_period_time_us,
                         audio->config.playback_buffer_time_us);
  if (ret < 0)
    {
      return ret;
    }

  *decoder = opus_decoder_create((opus_int32)rate, channels, &opus_error);
  if (*decoder == NULL || opus_error != OPUS_OK)
    {
      snd_pcm_close(*pcm);
      *pcm = NULL;
      return -EIO;
    }

  maximum_frames = rate * 120 / 1000;
  *samples = malloc((size_t)maximum_frames * channels * sizeof(**samples));
  if (*samples == NULL)
    {
      xiaozhi_playback_close(pcm, decoder, samples, false);
      return -ENOMEM;
    }

  return 0;
}

static void *xiaozhi_playback_thread(void *argument)
{
  struct xiaozhi_audio *audio = argument;
  uint8_t opus[XIAOZHI_OPUS_PACKET_MAX];
  snd_pcm_t *pcm = NULL;
  OpusDecoder *decoder = NULL;
  int16_t *samples = NULL;
  uint32_t generation = 0;
  uint32_t rate = 0;
  uint32_t maximum_frames;
  uint16_t frame_duration_ms;
  uint8_t channels = 0;
  size_t opus_length;
  bool finish;
  bool reset;
  int decoded_frames;
  int ret;

  for (;;)
    {
      pthread_mutex_lock(&audio->playback_lock);
      while (!audio->shutdown && audio->downlink.count == 0 &&
             generation == audio->playback_generation &&
             !audio->playback_reset &&
             !audio->playback_finish_requested)
        {
          pthread_cond_wait(&audio->playback_condition,
                            &audio->playback_lock);
        }

      if (audio->shutdown)
        {
          pthread_mutex_unlock(&audio->playback_lock);
          break;
        }

      if (generation != audio->playback_generation)
        {
          generation = audio->playback_generation;
          rate = audio->playback_sample_rate;
          channels = audio->playback_channels;
          frame_duration_ms = audio->playback_frame_duration_ms;
          reset = true;
        }
      else
        {
          frame_duration_ms = audio->playback_frame_duration_ms;
          reset = audio->playback_reset;
        }

      audio->playback_reset = false;
      if (audio->downlink.count == 0)
        {
          finish = audio->playback_finish_requested;
          if (finish)
            {
              audio->playback_finish_requested = false;
              audio->playback_draining = true;
            }

          pthread_mutex_unlock(&audio->playback_lock);
          if (finish)
            {
              ret = xiaozhi_playback_close(&pcm, &decoder, &samples, true);
              if (ret < 0)
                {
                  xiaozhi_audio_set_error(audio, false, ret,
                                          "playback drain failed: %s",
                                          snd_strerror(ret));
                }

              pthread_mutex_lock(&audio->playback_lock);
              audio->playback_draining = false;
              pthread_cond_broadcast(&audio->playback_condition);
              pthread_mutex_unlock(&audio->playback_lock);
            }
          else if (rate != 0 && channels != 0 &&
              (pcm == NULL || reset))
            {
              ret = xiaozhi_playback_configure(audio, &pcm, &decoder,
                                               &samples, rate, channels);
              if (ret < 0)
                {
                  xiaozhi_audio_set_error(audio, false, ret,
                                          "configure playback failed: %s",
                                          snd_strerror(ret));
                }
            }
          else if (reset && decoder != NULL)
            {
              opus_decoder_ctl(decoder, OPUS_RESET_STATE);
            }

          continue;
        }

      ret = xiaozhi_ring_pop(&audio->downlink, opus, sizeof(opus),
                             &opus_length);
      audio->playback_busy = ret == 0;
      pthread_mutex_unlock(&audio->playback_lock);
      if (ret < 0)
        {
          continue;
        }

      if (pcm == NULL || decoder == NULL || samples == NULL || reset)
        {
          ret = xiaozhi_playback_configure(audio, &pcm, &decoder,
                                           &samples, rate, channels);
          if (ret < 0)
            {
              xiaozhi_audio_set_error(audio, false, ret,
                                      "configure playback failed: %s",
                                      snd_strerror(ret));
              goto packet_done;
            }
        }

      maximum_frames = rate * 120 / 1000;
      decoded_frames = opus_decode(decoder, opus, (opus_int32)opus_length,
                                   samples, (int)maximum_frames, 0);
      if (decoded_frames < 0)
        {
          xiaozhi_audio_set_error(audio, false, -EIO,
                                  "opus decode failed: %s",
                                  opus_strerror(decoded_frames));
          goto packet_done;
        }

      (void)frame_duration_ms;
      ret = xiaozhi_write_pcm(pcm, samples, (uint32_t)decoded_frames,
                              channels);
      if (ret < 0)
        {
          xiaozhi_audio_set_error(audio, false, ret,
                                  "playback write failed: %s",
                                  snd_strerror(ret));
          goto packet_done;
        }

      xiaozhi_audio_add_frames(audio, false, (uint32_t)decoded_frames);

packet_done:
      pthread_mutex_lock(&audio->playback_lock);
      audio->playback_busy = false;
      pthread_cond_broadcast(&audio->playback_condition);
      pthread_mutex_unlock(&audio->playback_lock);
    }

  xiaozhi_playback_close(&pcm, &decoder, &samples, false);
  return NULL;
}

static void xiaozhi_audio_apply_defaults(struct xiaozhi_audio_config *config)
{
  if (config->capture_device == NULL)
    {
      config->capture_device = "default";
    }

  if (config->playback_device == NULL)
    {
      config->playback_device = "default";
    }

  if (config->capture_sample_rate == 0)
    {
      config->capture_sample_rate = XIAOZHI_AUDIO_DEFAULT_RATE;
    }

  if (config->capture_channels == 0)
    {
      config->capture_channels = 1;
    }

  if (config->frame_duration_ms == 0)
    {
      config->frame_duration_ms = XIAOZHI_AUDIO_DEFAULT_FRAME_MS;
    }

  if (config->capture_period_time_us == 0)
    {
      config->capture_period_time_us = config->frame_duration_ms * 1000;
    }

  if (config->capture_buffer_time_us == 0)
    {
      config->capture_buffer_time_us = config->capture_period_time_us * 4;
    }

  if (config->playback_period_time_us == 0)
    {
      config->playback_period_time_us = config->frame_duration_ms * 1000;
    }

  if (config->playback_buffer_time_us == 0)
    {
      config->playback_buffer_time_us = config->playback_period_time_us * 4;
    }

  if (config->opus_bitrate == 0)
    {
      config->opus_bitrate = XIAOZHI_AUDIO_DEFAULT_BITRATE;
    }

  if (config->opus_complexity <= 0 || config->opus_complexity > 10)
    {
      config->opus_complexity = XIAOZHI_AUDIO_DEFAULT_COMPLEXITY;
    }

  if (config->uplink_queue_packets == 0)
    {
      config->uplink_queue_packets = XIAOZHI_AUDIO_DEFAULT_UPLINK_PACKETS;
    }

  if (config->downlink_queue_packets == 0)
    {
      config->downlink_queue_packets = XIAOZHI_AUDIO_DEFAULT_DOWNLINK_PACKETS;
    }
}

struct xiaozhi_audio *
xiaozhi_audio_create(const struct xiaozhi_audio_config *config)
{
  struct xiaozhi_audio *audio;
  pthread_attr_t attributes;
  int ret;

  if (config == NULL)
    {
      return NULL;
    }

  audio = calloc(1, sizeof(*audio));
  if (audio == NULL)
    {
      return NULL;
    }

  audio->config = *config;
  xiaozhi_audio_apply_defaults(&audio->config);
  if (!xiaozhi_opus_rate_valid(audio->config.capture_sample_rate) ||
      !xiaozhi_opus_frame_duration_valid(
          audio->config.frame_duration_ms) ||
      audio->config.capture_channels == 0 ||
      audio->config.capture_channels > 2 ||
      audio->config.capture_sample_rate * audio->config.frame_duration_ms %
          1000 != 0)
    {
      free(audio);
      return NULL;
    }

  audio->capture_device = xiaozhi_audio_strdup(audio->config.capture_device);
  audio->playback_device = xiaozhi_audio_strdup(audio->config.playback_device);
  audio->uplink.capacity = audio->config.uplink_queue_packets;
  audio->downlink.capacity = audio->config.downlink_queue_packets;
  audio->uplink.packets = calloc(audio->uplink.capacity,
                                 sizeof(*audio->uplink.packets));
  audio->downlink.packets = calloc(audio->downlink.capacity,
                                   sizeof(*audio->downlink.packets));
  if (audio->capture_device == NULL || audio->playback_device == NULL ||
      audio->uplink.packets == NULL || audio->downlink.packets == NULL)
    {
      free(audio->capture_device);
      free(audio->playback_device);
      free(audio->uplink.packets);
      free(audio->downlink.packets);
      free(audio);
      return NULL;
    }

  audio->config.capture_device = audio->capture_device;
  audio->config.playback_device = audio->playback_device;
  pthread_mutex_init(&audio->capture_lock, NULL);
  pthread_cond_init(&audio->capture_condition, NULL);
  pthread_mutex_init(&audio->playback_lock, NULL);
  pthread_cond_init(&audio->playback_condition, NULL);
  pthread_mutex_init(&audio->status_lock, NULL);

  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, XIAOZHI_AUDIO_THREAD_STACK_SIZE);
  ret = pthread_create(&audio->capture_thread, &attributes,
                       xiaozhi_capture_thread, audio);
  if (ret == 0)
    {
      audio->capture_thread_created = true;
      ret = pthread_create(&audio->playback_thread, &attributes,
                           xiaozhi_playback_thread, audio);
      if (ret == 0)
        {
          audio->playback_thread_created = true;
        }
    }

  pthread_attr_destroy(&attributes);
  if (!audio->capture_thread_created || !audio->playback_thread_created)
    {
      xiaozhi_audio_destroy(audio);
      return NULL;
    }

  return audio;
}

void xiaozhi_audio_destroy(struct xiaozhi_audio *audio)
{
  if (audio == NULL)
    {
      return;
    }

  pthread_mutex_lock(&audio->capture_lock);
  audio->shutdown = true;
  audio->capture_active = false;
  pthread_cond_broadcast(&audio->capture_condition);
  pthread_mutex_unlock(&audio->capture_lock);

  pthread_mutex_lock(&audio->playback_lock);
  audio->shutdown = true;
  pthread_cond_broadcast(&audio->playback_condition);
  pthread_mutex_unlock(&audio->playback_lock);

  if (audio->capture_thread_created)
    {
      pthread_join(audio->capture_thread, NULL);
    }

  if (audio->playback_thread_created)
    {
      pthread_join(audio->playback_thread, NULL);
    }

  pthread_mutex_destroy(&audio->capture_lock);
  pthread_cond_destroy(&audio->capture_condition);
  pthread_mutex_destroy(&audio->playback_lock);
  pthread_cond_destroy(&audio->playback_condition);
  pthread_mutex_destroy(&audio->status_lock);
  free(audio->capture_device);
  free(audio->playback_device);
  free(audio->uplink.packets);
  free(audio->downlink.packets);
  memset(audio, 0, sizeof(*audio));
  free(audio);
}

static int xiaozhi_audio_start_capture_impl(void *context)
{
  struct xiaozhi_audio *audio = context;

  if (audio == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&audio->status_lock);
  audio->stats.capture_error = 0;
  pthread_mutex_unlock(&audio->status_lock);

  pthread_mutex_lock(&audio->capture_lock);
  xiaozhi_ring_clear(&audio->uplink);
  audio->capture_active = true;
  pthread_cond_broadcast(&audio->capture_condition);
  pthread_mutex_unlock(&audio->capture_lock);
  return 0;
}

static int xiaozhi_audio_stop_capture_impl(void *context)
{
  struct xiaozhi_audio *audio = context;

  if (audio == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&audio->capture_lock);
  audio->capture_active = false;
  xiaozhi_ring_clear(&audio->uplink);
  pthread_cond_broadcast(&audio->capture_condition);
  pthread_mutex_unlock(&audio->capture_lock);
  return 0;
}

static int xiaozhi_audio_read_uplink_impl(void *context, void *buffer,
                                          size_t capacity, size_t *length)
{
  struct xiaozhi_audio *audio = context;
  int capture_error;
  int ret;

  if (audio == NULL || buffer == NULL || length == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&audio->capture_lock);
  ret = xiaozhi_ring_pop(&audio->uplink, buffer, capacity, length);
  pthread_mutex_unlock(&audio->capture_lock);
  if (ret != -EAGAIN)
    {
      return ret;
    }

  pthread_mutex_lock(&audio->status_lock);
  capture_error = audio->stats.capture_error;
  pthread_mutex_unlock(&audio->status_lock);
  return capture_error != 0 ? capture_error : -EAGAIN;
}

static int xiaozhi_audio_configure_downlink_impl(void *context,
                                                 uint32_t sample_rate,
                                                 uint8_t channels,
                                                 uint16_t frame_duration_ms)
{
  struct xiaozhi_audio *audio = context;

  if (audio == NULL || !xiaozhi_opus_rate_valid(sample_rate) ||
      channels == 0 || channels > 2 ||
      !xiaozhi_opus_frame_duration_valid(frame_duration_ms))
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&audio->status_lock);
  audio->stats.playback_error = 0;
  pthread_mutex_unlock(&audio->status_lock);

  pthread_mutex_lock(&audio->playback_lock);
  audio->playback_sample_rate = sample_rate;
  audio->playback_channels = channels;
  audio->playback_frame_duration_ms = frame_duration_ms;
  audio->playback_generation++;
  audio->playback_finish_requested = false;
  audio->playback_reset = true;
  xiaozhi_ring_clear(&audio->downlink);
  pthread_cond_broadcast(&audio->playback_condition);
  pthread_mutex_unlock(&audio->playback_lock);
  return 0;
}

static int xiaozhi_audio_push_downlink_impl(void *context,
                                            const void *data, size_t length)
{
  struct xiaozhi_audio *audio = context;
  int ret;

  if (audio == NULL || data == NULL || length == 0 ||
      length > XIAOZHI_OPUS_PACKET_MAX)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&audio->playback_lock);
  if (audio->playback_sample_rate == 0)
    {
      pthread_mutex_unlock(&audio->playback_lock);
      return -EAGAIN;
    }

  if (audio->downlink.count == audio->downlink.capacity)
    {
      xiaozhi_ring_drop_oldest(&audio->downlink);
      xiaozhi_audio_add_drop(audio, false);
      audio->playback_reset = true;
    }

  ret = xiaozhi_ring_push(&audio->downlink, data, length);
  pthread_cond_broadcast(&audio->playback_condition);
  pthread_mutex_unlock(&audio->playback_lock);
  return ret;
}

static int xiaozhi_audio_finish_downlink_impl(void *context)
{
  struct xiaozhi_audio *audio = context;

  if (audio == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&audio->playback_lock);
  audio->playback_finish_requested = true;
  pthread_cond_broadcast(&audio->playback_condition);
  pthread_mutex_unlock(&audio->playback_lock);
  return 0;
}

static bool xiaozhi_audio_playback_idle_impl(void *context)
{
  struct xiaozhi_audio *audio = context;
  bool idle;

  if (audio == NULL)
    {
      return true;
    }

  pthread_mutex_lock(&audio->playback_lock);
  idle = audio->downlink.count == 0 && !audio->playback_busy &&
         !audio->playback_finish_requested && !audio->playback_draining;
  pthread_mutex_unlock(&audio->playback_lock);
  return idle;
}

static int xiaozhi_audio_get_error_impl(void *context, bool capture)
{
  struct xiaozhi_audio *audio = context;
  int error;

  if (audio == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&audio->status_lock);
  error = capture ? audio->stats.capture_error :
                    audio->stats.playback_error;
  pthread_mutex_unlock(&audio->status_lock);
  return error;
}

static void xiaozhi_audio_flush_impl(void *context)
{
  struct xiaozhi_audio *audio = context;

  if (audio == NULL)
    {
      return;
    }

  pthread_mutex_lock(&audio->capture_lock);
  xiaozhi_ring_clear(&audio->uplink);
  pthread_mutex_unlock(&audio->capture_lock);

  pthread_mutex_lock(&audio->playback_lock);
  xiaozhi_ring_clear(&audio->downlink);
  audio->playback_finish_requested = false;
  audio->playback_reset = true;
  pthread_cond_broadcast(&audio->playback_condition);
  pthread_mutex_unlock(&audio->playback_lock);
}

void xiaozhi_audio_get_stats(struct xiaozhi_audio *audio,
                             struct xiaozhi_audio_stats *stats)
{
  if (audio == NULL || stats == NULL)
    {
      return;
    }

  pthread_mutex_lock(&audio->status_lock);
  *stats = audio->stats;
  pthread_mutex_unlock(&audio->status_lock);
}

const char *xiaozhi_audio_last_error(struct xiaozhi_audio *audio)
{
  return audio == NULL ? "invalid audio client" : audio->error_text;
}

const struct xiaozhi_audio_ops *xiaozhi_audio_ops(void)
{
  static const struct xiaozhi_audio_ops operations =
  {
    .start_capture = xiaozhi_audio_start_capture_impl,
    .stop_capture = xiaozhi_audio_stop_capture_impl,
    .read_uplink_opus = xiaozhi_audio_read_uplink_impl,
    .configure_downlink = xiaozhi_audio_configure_downlink_impl,
    .push_downlink_opus = xiaozhi_audio_push_downlink_impl,
    .finish_downlink = xiaozhi_audio_finish_downlink_impl,
    .playback_idle = xiaozhi_audio_playback_idle_impl,
    .get_error = xiaozhi_audio_get_error_impl,
    .flush = xiaozhi_audio_flush_impl,
  };

  return &operations;
}
