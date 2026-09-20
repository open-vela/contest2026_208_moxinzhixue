/****************************************************************************
 * Contest 2026 team 208 - Gemini-S1 hardware baseline self-test
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "platform/gs1_platform.h"

#define GS1_HWCHECK_POLL_MS 100

static FAR const char *gs1_hwcheck_storage_state(enum gs1_storage_state_e state)
{
  static FAR const char *names[] =
  {
    "unknown", "checking", "ready", "degraded", "unavailable"
  };

  return state < sizeof(names) / sizeof(names[0]) ? names[state] : "invalid";
}

static FAR const char *gs1_hwcheck_wifi_state(enum gs1_wifi_state_e state)
{
  static FAR const char *names[] =
  {
    "unknown", "down", "disconnected", "connecting", "connected", "error"
  };

  return state < sizeof(names) / sizeof(names[0]) ? names[state] : "invalid";
}

static FAR const char *gs1_hwcheck_audio_state(enum gs1_audio_state_e state)
{
  static FAR const char *names[] =
  {
    "unknown", "idle", "recording", "playing", "error"
  };

  return state < sizeof(names) / sizeof(names[0]) ? names[state] : "invalid";
}

static FAR const char *gs1_hwcheck_time_state(enum gs1_time_state_e state)
{
  static FAR const char *names[] =
  {
    "unknown", "invalid", "local", "syncing", "synced", "error"
  };

  return state < sizeof(names) / sizeof(names[0]) ? names[state] : "invalid";
}

static void gs1_hwcheck_print(FAR const struct gs1_platform_status_s *status)
{
  printf("runtime.state=%u queue=%lu queued=%lu completed=%lu error=%d(%s)\n",
         status->runtime.state,
         (unsigned long)status->runtime.queue_depth,
         (unsigned long)status->runtime.last_queued_request_id,
         (unsigned long)status->runtime.last_completed_request_id,
         status->runtime.last_error,
         gs1_platform_error_string(status->runtime.last_error));
  printf("storage.state=%s root=%s writable=%u kvdb=%u free=%llu "
         "recordings=%lu bytes=%llu error=%d\n",
         gs1_hwcheck_storage_state(status->storage.state),
         status->storage.root, status->storage.writable,
         status->storage.kvdb_available,
         (unsigned long long)status->storage.free_bytes,
         (unsigned long)status->storage.recording_count,
         (unsigned long long)status->storage.recording_bytes,
         status->storage.last_error);
  printf("wifi.state=%s if=%s up=%u associated=%u ipv4=%s ssid=%s "
         "error=%d\n", gs1_hwcheck_wifi_state(status->wifi.state),
         status->wifi.ifname, status->wifi.interface_up,
         status->wifi.associated,
         status->wifi.has_ipv4 ? status->wifi.ipv4 : "-",
         status->wifi.ssid[0] != '\0' ? status->wifi.ssid : "-",
         status->wifi.last_error);
  printf("audio.state=%s devices=%u format=%lu/%u/%u elapsed_ms=%lu "
         "bytes=%llu stream=%u buffered=%lu dropped=%llu current=%s "
         "last=%s error=%d\n",
         gs1_hwcheck_audio_state(status->audio.state),
         status->audio.device_count, (unsigned long)status->audio.sample_rate,
         status->audio.channels, status->audio.bits_per_sample,
         (unsigned long)status->audio.elapsed_ms,
         (unsigned long long)status->audio.bytes,
         status->audio.streaming,
         (unsigned long)status->audio.stream_buffered_bytes,
         (unsigned long long)status->audio.stream_dropped_bytes,
         status->audio.current_path[0] != '\0' ?
           status->audio.current_path : "-",
         status->audio.last_recording_path[0] != '\0' ?
           status->audio.last_recording_path : "-",
         status->audio.last_error);
  printf("time.state=%s valid=%u network=%u network_error=%d pid=%d "
         "owned=%u starts=%lu restarts=%lu elapsed_ms=%lu samples=%lu "
         "server=%s offset_raw=%lld delay_raw=%lld realtime=%lld "
         "monotonic=%lld last_sync=%lld start_error=%d error=%d\n",
         gs1_hwcheck_time_state(status->time.state),
         status->time.realtime_valid,
         status->time.network_ready, status->time.network_error,
         status->time.ntp_pid, status->time.daemon_owned,
         (unsigned long)status->time.ntp_start_count,
         (unsigned long)status->time.ntp_restart_count,
         (unsigned long)status->time.sync_elapsed_ms,
         (unsigned long)status->time.ntp_samples,
         status->time.first_server[0] != '\0' ?
           status->time.first_server : "-",
         (long long)status->time.first_sample_offset,
         (long long)status->time.first_sample_delay,
         (long long)status->time.realtime_sec,
         (long long)status->time.monotonic_sec,
         (long long)status->time.last_sync_sec,
         status->time.last_start_error,
         status->time.last_error);
}

static int gs1_hwcheck_wait_request(uint32_t request_id, uint32_t timeout_ms,
                                    FAR struct gs1_platform_status_s *status)
{
  uint32_t elapsed;
  int ret;

  for (elapsed = 0; elapsed <= timeout_ms; elapsed += GS1_HWCHECK_POLL_MS)
    {
      ret = gs1_platform_get_status(status);
      if (ret < 0)
        {
          return ret;
        }

      if ((int32_t)(status->runtime.last_completed_request_id - request_id) >= 0)
        {
          return status->runtime.last_error;
        }

      usleep(GS1_HWCHECK_POLL_MS * 1000);
    }

  return GS1_ERROR_TIMEOUT;
}

static int gs1_hwcheck_submit(
    FAR const struct gs1_platform_request_s *request, uint32_t timeout_ms,
    FAR struct gs1_platform_status_s *status)
{
  uint32_t request_id;
  int ret;

  ret = gs1_platform_request(request, &request_id);
  return ret < 0 ? ret : gs1_hwcheck_wait_request(request_id, timeout_ms,
                                                  status);
}

static int gs1_hwcheck_wait_audio_idle(uint32_t timeout_ms,
                                       bool require_recording,
                                       FAR struct gs1_platform_status_s *status)
{
  uint32_t elapsed;
  int ret;

  for (elapsed = 0; elapsed <= timeout_ms; elapsed += GS1_HWCHECK_POLL_MS)
    {
      ret = gs1_platform_get_status(status);
      if (ret < 0)
        {
          return ret;
        }

      if (status->audio.state == GS1_AUDIO_ERROR)
        {
          return status->audio.last_error != 0 ? status->audio.last_error :
                                                -EIO;
        }

      if (status->audio.state == GS1_AUDIO_IDLE &&
          (!require_recording || status->audio.last_recording_path[0] != '\0'))
        {
          return 0;
        }

      usleep(GS1_HWCHECK_POLL_MS * 1000);
    }

  return GS1_ERROR_TIMEOUT;
}

static int gs1_hwcheck_seconds(FAR const char *text, uint32_t maximum,
                               FAR uint32_t *seconds)
{
  FAR char *end;
  unsigned long value;

  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value == 0 ||
      value > maximum)
    {
      return -EINVAL;
    }

  *seconds = value;
  return 0;
}

static void gs1_hwcheck_usage(FAR const char *program)
{
  printf("Usage:\n");
  printf("  %s status\n", program);
  printf("  %s storage\n", program);
  printf("  %s wifi-connect <ssid> [passphrase]\n", program);
  printf("  %s wifi-disconnect\n", program);
  printf("  %s ntp [timeout-seconds]\n", program);
  printf("  %s record [seconds]\n", program);
  printf("  %s stream [seconds]\n", program);
  printf("  %s play <pcm-path> [seconds]\n", program);
  printf("  %s audio-loopback [record-seconds]\n", program);
}

int main(int argc, FAR char *argv[])
{
  struct gs1_platform_request_s request;
  struct gs1_platform_status_s status;
  char recording_path[GS1_PATH_MAX];
  FAR const char *command;
  uint32_t seconds = 5;
  int ret;

  command = argc > 1 ? argv[1] : "status";
  if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0)
    {
      gs1_hwcheck_usage(argv[0]);
      return EXIT_SUCCESS;
    }

  ret = gs1_platform_init();
  if (ret < 0)
    {
      fprintf(stderr, "gs1_hwcheck: init failed: %d (%s)\n", ret,
              gs1_platform_error_string(ret));
      return EXIT_FAILURE;
    }

  memset(&request, 0, sizeof(request));
  memset(&status, 0, sizeof(status));

  if (strcmp(command, "status") == 0)
    {
      request.op = GS1_REQUEST_REFRESH;
      ret = gs1_hwcheck_submit(&request, 5000, &status);
    }
  else if (strcmp(command, "storage") == 0)
    {
      request.op = GS1_REQUEST_STORAGE_PROBE;
      ret = gs1_hwcheck_submit(&request, 10000, &status);
    }
  else if (strcmp(command, "wifi-connect") == 0 && argc >= 3)
    {
      request.op = GS1_REQUEST_WIFI_CONNECT;
      snprintf(request.data.wifi_connect.ssid,
               sizeof(request.data.wifi_connect.ssid), "%s", argv[2]);
      if (argc >= 4)
        {
          snprintf(request.data.wifi_connect.passphrase,
                   sizeof(request.data.wifi_connect.passphrase), "%s",
                   argv[3]);
        }

      request.data.wifi_connect.persist = true;
      ret = gs1_hwcheck_submit(&request, 30000, &status);
    }
  else if (strcmp(command, "wifi-disconnect") == 0)
    {
      request.op = GS1_REQUEST_WIFI_DISCONNECT;
      ret = gs1_hwcheck_submit(&request, 5000, &status);
    }
  else if (strcmp(command, "ntp") == 0)
    {
      if (argc < 3)
        {
          seconds = 45;
        }
      else if (gs1_hwcheck_seconds(argv[2], 120, &seconds) < 0)
        {
          ret = -EINVAL;
          goto finished;
        }

      request.op = GS1_REQUEST_TIME_SYNC;
      ret = gs1_hwcheck_submit(&request, 5000, &status);
      if (ret == 0)
        {
          uint32_t elapsed;
          for (elapsed = 0; elapsed <= seconds * 1000;
               elapsed += GS1_HWCHECK_POLL_MS)
            {
              gs1_platform_get_status(&status);
              if (status.time.state == GS1_TIME_SYNCED &&
                  status.time.ntp_samples > 0)
                {
                  break;
                }

              if (status.time.state == GS1_TIME_ERROR)
                {
                  ret = status.time.last_error != 0 ?
                        status.time.last_error : -EIO;
                  break;
                }

              usleep(GS1_HWCHECK_POLL_MS * 1000);
            }

          if (ret == 0 &&
              (status.time.state != GS1_TIME_SYNCED ||
               status.time.ntp_samples == 0))
            {
              ret = GS1_ERROR_TIMEOUT;
            }
        }
    }
  else if (strcmp(command, "record") == 0 ||
           strcmp(command, "stream") == 0 ||
           strcmp(command, "audio-loopback") == 0)
    {
      if (argc >= 3 && gs1_hwcheck_seconds(argv[2], 30, &seconds) < 0)
        {
          ret = -EINVAL;
          goto finished;
        }

      request.op = GS1_REQUEST_AUDIO_RECORD_START;
      snprintf(request.data.audio_record.basename,
               sizeof(request.data.audio_record.basename), "hwcheck");
      request.data.audio_record.sample_rate =
        CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_SAMPLE_RATE;
      request.data.audio_record.channels =
        CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_CHANNELS;
      request.data.audio_record.bits_per_sample =
        CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_BITS;
      request.data.audio_record.channel_map =
        CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_CHANNEL_MAP;
      request.data.audio_record.limit_ms = strcmp(command, "stream") == 0 ?
                                           0 : seconds * 1000;
      request.data.audio_record.retain_file =
        strcmp(command, "stream") != 0;
      ret = gs1_hwcheck_submit(&request, 5000, &status);
      if (ret == 0 && strcmp(command, "stream") == 0)
        {
          struct timespec start;
          struct timespec now;
          uint8_t pcm[1280];
          size_t stream_bytes;
          uint64_t total = 0;
          uint32_t first_chunk_ms = UINT32_MAX;

          clock_gettime(CLOCK_MONOTONIC, &start);
          do
            {
              stream_bytes = 0;
              ret = gs1_platform_audio_stream_read(pcm, sizeof(pcm),
                                                   &stream_bytes);
              if (ret < 0)
                {
                  break;
                }

              total += stream_bytes;
              clock_gettime(CLOCK_MONOTONIC, &now);
              if (stream_bytes > 0 && first_chunk_ms == UINT32_MAX)
                {
                  first_chunk_ms =
                    (uint32_t)((now.tv_sec - start.tv_sec) * 1000 +
                    (now.tv_nsec - start.tv_nsec) / 1000000);
                }

              usleep(GS1_HWCHECK_POLL_MS * 1000);
            }
          while ((uint32_t)(now.tv_sec - start.tv_sec) < seconds);

          memset(&request, 0, sizeof(request));
          request.op = GS1_REQUEST_AUDIO_RECORD_STOP;
          if (ret == 0)
            {
              ret = gs1_hwcheck_submit(&request, 5000, &status);
            }

          printf("stream.read_bytes=%llu first_chunk_ms=%lu\n",
                 (unsigned long long)total,
                 first_chunk_ms == UINT32_MAX ? 0 :
                 (unsigned long)first_chunk_ms);
        }
      else if (ret == 0)
        {
          ret = gs1_hwcheck_wait_audio_idle(seconds * 1000 + 10000, true,
                                            &status);
        }

      if (ret == 0 && strcmp(command, "audio-loopback") == 0)
        {
          snprintf(recording_path, sizeof(recording_path), "%s",
                   status.audio.last_recording_path);
          memset(&request, 0, sizeof(request));
          request.op = GS1_REQUEST_AUDIO_PLAY;
          snprintf(request.data.audio_play.path,
                   sizeof(request.data.audio_play.path), "%s",
                   recording_path);
          request.data.audio_play.sample_rate =
            CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_SAMPLE_RATE;
          request.data.audio_play.channels =
            CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_CHANNELS;
          request.data.audio_play.bits_per_sample =
            CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_BITS;
          request.data.audio_play.channel_map =
            CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_CHANNEL_MAP;
          request.data.audio_play.limit_ms = seconds * 1000 + 2000;
          ret = gs1_hwcheck_submit(&request, 5000, &status);
          if (ret == 0)
            {
              ret = gs1_hwcheck_wait_audio_idle(seconds * 1000 + 10000,
                                                false, &status);
            }
        }
    }
  else if (strcmp(command, "play") == 0 && argc >= 3)
    {
      if (argc >= 4 && gs1_hwcheck_seconds(argv[3], 120, &seconds) < 0)
        {
          ret = -EINVAL;
          goto finished;
        }

      request.op = GS1_REQUEST_AUDIO_PLAY;
      snprintf(request.data.audio_play.path,
               sizeof(request.data.audio_play.path), "%s", argv[2]);
      request.data.audio_play.sample_rate =
        CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_SAMPLE_RATE;
      request.data.audio_play.channels =
        CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_CHANNELS;
      request.data.audio_play.bits_per_sample =
        CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_BITS;
      request.data.audio_play.channel_map =
        CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_AUDIO_CHANNEL_MAP;
      request.data.audio_play.limit_ms = seconds * 1000;
      ret = gs1_hwcheck_submit(&request, 5000, &status);
      if (ret == 0)
        {
          ret = gs1_hwcheck_wait_audio_idle(seconds * 1000 + 10000, false,
                                            &status);
        }
    }
  else
    {
      gs1_hwcheck_usage(argv[0]);
      ret = -EINVAL;
    }

finished:
  gs1_platform_get_status(&status);
  gs1_hwcheck_print(&status);
  if (ret < 0)
    {
      fprintf(stderr, "gs1_hwcheck: %s failed: %d (%s)\n", command, ret,
              gs1_platform_error_string(ret));
    }

  gs1_platform_shutdown();
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
