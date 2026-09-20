/****************************************************************************
 * Contest 2026 team 208 - audio service
 ****************************************************************************/

#ifndef __GS1_AUDIO_H
#define __GS1_AUDIO_H

#include <pthread.h>

#include "gs1_platform.h"

struct nxrecorder_s;
struct nxplayer_s;

struct gs1_audio_context_s
{
  FAR struct nxrecorder_s *recorder;
  FAR struct nxplayer_s *player;
  pthread_t pump_thread;
  pthread_mutex_t stream_lock;
  uint8_t stream_buffer[
    CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_STREAM_BUFFER_BYTES];
  size_t stream_head;
  size_t stream_size;
  uint64_t stream_total_bytes;
  uint64_t stream_dropped_bytes;
  int stream_fd;
  int retention_fd;
  int retention_error;
  char fifo_path[64];
  char part_path[GS1_PATH_MAX];
  char final_path[GS1_PATH_MAX];
  uint32_t deadline_ms;
  uint32_t started_ms;
  uint32_t backend_start_deadline_ms;
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bits_per_sample;
  uint8_t channel_map;
  uint8_t volume;
  bool retain_file;
  bool device_probe_done;
  bool backend_started;
  volatile bool pump_stop;
  bool pump_started;
};

void gs1_audio_initialize(FAR struct gs1_audio_context_s *context,
                          FAR struct gs1_audio_status_s *status);
int gs1_audio_record_start(
    FAR struct gs1_audio_context_s *context,
    FAR const struct gs1_audio_record_request_s *request,
    FAR struct gs1_audio_status_s *status);
int gs1_audio_record_stop(FAR struct gs1_audio_context_s *context,
                          FAR struct gs1_audio_status_s *status);
int gs1_audio_play(FAR struct gs1_audio_context_s *context,
                   FAR const struct gs1_audio_play_request_s *request,
                   FAR struct gs1_audio_status_s *status);
int gs1_audio_stop(FAR struct gs1_audio_context_s *context,
                   FAR struct gs1_audio_status_s *status);
int gs1_audio_set_volume(FAR struct gs1_audio_context_s *context,
                         uint8_t percent);
void gs1_audio_tick(FAR struct gs1_audio_context_s *context,
                    FAR struct gs1_audio_status_s *status);
int gs1_audio_stream_read(FAR struct gs1_audio_context_s *context,
                          FAR void *buffer, size_t capacity,
                          FAR size_t *bytes_read);
void gs1_audio_shutdown(FAR struct gs1_audio_context_s *context,
                        FAR struct gs1_audio_status_s *status);

#endif /* __GS1_AUDIO_H */
