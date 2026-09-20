#ifndef XIAOZHI_AUDIO_H
#define XIAOZHI_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define XIAOZHI_OPUS_PACKET_MAX 1500

struct xiaozhi_audio_config
{
  const char *capture_device;
  const char *playback_device;
  uint32_t capture_sample_rate;
  uint8_t capture_channels;
  uint16_t frame_duration_ms;
  uint32_t capture_period_time_us;
  uint32_t capture_buffer_time_us;
  uint32_t playback_period_time_us;
  uint32_t playback_buffer_time_us;
  int opus_bitrate;
  int opus_complexity;
  size_t uplink_queue_packets;
  size_t downlink_queue_packets;
};

struct xiaozhi_audio_stats
{
  uint64_t captured_frames;
  uint64_t played_frames;
  uint32_t uplink_dropped;
  uint32_t downlink_dropped;
  int capture_error;
  int playback_error;
};

struct xiaozhi_audio_ops
{
  int (*start_capture)(void *context);
  int (*stop_capture)(void *context);
  int (*read_uplink_opus)(void *context, void *buffer, size_t capacity,
                          size_t *length);
  int (*configure_downlink)(void *context, uint32_t sample_rate,
                            uint8_t channels, uint16_t frame_duration_ms);
  int (*push_downlink_opus)(void *context, const void *data, size_t length);
  int (*finish_downlink)(void *context);
  bool (*playback_idle)(void *context);
  int (*get_error)(void *context, bool capture);
  void (*flush)(void *context);
};

struct xiaozhi_audio;

struct xiaozhi_audio *
xiaozhi_audio_create(const struct xiaozhi_audio_config *config);
void xiaozhi_audio_destroy(struct xiaozhi_audio *audio);
void xiaozhi_audio_get_stats(struct xiaozhi_audio *audio,
                             struct xiaozhi_audio_stats *stats);
const char *xiaozhi_audio_last_error(struct xiaozhi_audio *audio);
const struct xiaozhi_audio_ops *xiaozhi_audio_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_AUDIO_H */
