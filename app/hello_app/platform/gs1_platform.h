/****************************************************************************
 * Contest 2026 team 208 - Gemini-S1 platform service
 ****************************************************************************/

#ifndef __GS1_PLATFORM_H
#define __GS1_PLATFORM_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "gs1_config.h"

#define GS1_SSID_MAX          32
#define GS1_PASSPHRASE_MAX    64
#define GS1_IFNAME_MAX        16
#define GS1_IPV4_MAX          16
#define GS1_PATH_MAX          192
#define GS1_NTP_SERVER_MAX    48
#define GS1_WIFI_SCAN_MAX     10

/* Domain errors are kept outside the errno range.  Native failures are
 * returned as negated errno values.
 */

enum gs1_error_e
{
  GS1_ERROR_NOT_STARTED = -2001,
  GS1_ERROR_ALREADY_STARTED = -2002,
  GS1_ERROR_QUEUE_FULL = -2003,
  GS1_ERROR_INVALID_STATE = -2004,
  GS1_ERROR_TIMEOUT = -2005,
  GS1_ERROR_UNSAFE_PATH = -2006,
  GS1_ERROR_DATA_CORRUPT = -2007
};

enum gs1_worker_state_e
{
  GS1_WORKER_STOPPED = 0,
  GS1_WORKER_RUNNING,
  GS1_WORKER_ERROR
};

enum gs1_storage_state_e
{
  GS1_STORAGE_UNKNOWN = 0,
  GS1_STORAGE_CHECKING,
  GS1_STORAGE_READY,
  GS1_STORAGE_DEGRADED,
  GS1_STORAGE_UNAVAILABLE
};

enum gs1_wifi_state_e
{
  GS1_WIFI_UNKNOWN = 0,
  GS1_WIFI_DOWN,
  GS1_WIFI_DISCONNECTED,
  GS1_WIFI_CONNECTING,
  GS1_WIFI_CONNECTED,
  GS1_WIFI_ERROR
};

enum gs1_wifi_scan_state_e
{
  GS1_WIFI_SCAN_IDLE = 0,
  GS1_WIFI_SCANNING,
  GS1_WIFI_SCAN_READY,
  GS1_WIFI_SCAN_ERROR
};

enum gs1_audio_state_e
{
  GS1_AUDIO_UNKNOWN = 0,
  GS1_AUDIO_IDLE,
  GS1_AUDIO_RECORDING,
  GS1_AUDIO_PLAYING,
  GS1_AUDIO_ERROR
};

enum gs1_time_state_e
{
  GS1_TIME_UNKNOWN = 0,
  GS1_TIME_INVALID,
  GS1_TIME_LOCAL,
  GS1_TIME_SYNCING,
  GS1_TIME_SYNCED,
  GS1_TIME_ERROR
};

struct gs1_runtime_status_s
{
  enum gs1_worker_state_e state;
  uint32_t queue_depth;
  uint32_t last_queued_request_id;
  uint32_t last_completed_request_id;
  int last_error;
};

struct gs1_storage_status_s
{
  enum gs1_storage_state_e state;
  bool writable;
  bool kvdb_available;
  uint32_t recording_count;
  uint64_t free_bytes;
  uint64_t recording_bytes;
  char root[GS1_PATH_MAX];
  int last_error;
};

struct gs1_wifi_status_s
{
  enum gs1_wifi_state_e state;
  bool interface_up;
  bool associated;
  bool has_ipv4;
  char ifname[GS1_IFNAME_MAX];
  char ssid[GS1_SSID_MAX + 1];
  char ipv4[GS1_IPV4_MAX];
  int last_error;
};

struct gs1_audio_status_s
{
  enum gs1_audio_state_e state;
  bool streaming;
  uint32_t sample_rate;
  uint32_t elapsed_ms;
  uint64_t bytes;
  uint64_t stream_total_bytes;
  uint64_t stream_dropped_bytes;
  uint32_t stream_buffered_bytes;
  uint8_t channels;
  uint8_t bits_per_sample;
  uint8_t device_count;
  char current_path[GS1_PATH_MAX];
  char last_recording_path[GS1_PATH_MAX];
  int last_error;
};

struct gs1_time_status_s
{
  enum gs1_time_state_e state;
  bool realtime_valid;
  bool daemon_owned;
  bool network_ready;
  uint32_t ntp_samples;
  uint32_t ntp_start_count;
  uint32_t ntp_restart_count;
  uint32_t sync_elapsed_ms;
  int64_t realtime_sec;
  int64_t monotonic_sec;
  int64_t last_sync_sec;
  int64_t first_sample_offset;
  int64_t first_sample_delay;
  int ntp_pid;
  int last_start_error;
  int network_error;
  char first_server[GS1_NTP_SERVER_MAX];
  int last_error;
};

struct gs1_wifi_network_s
{
  char ssid[GS1_SSID_MAX + 1];
  int16_t rssi;
  bool secured;
};

struct gs1_wifi_scan_status_s
{
  enum gs1_wifi_scan_state_e state;
  struct gs1_wifi_network_s networks[GS1_WIFI_SCAN_MAX];
  uint32_t generation;
  int last_error;
  uint8_t count;
};

struct gs1_platform_status_s
{
  uint32_t generation;
  struct gs1_runtime_status_s runtime;
  struct gs1_storage_status_s storage;
  struct gs1_wifi_status_s wifi;
  struct gs1_wifi_scan_status_s wifi_scan;
  struct gs1_audio_status_s audio;
  struct gs1_time_status_s time;
};

enum gs1_request_op_e
{
  GS1_REQUEST_REFRESH = 1,
  GS1_REQUEST_STORAGE_PROBE,
  GS1_REQUEST_WIFI_SCAN,
  GS1_REQUEST_WIFI_CONNECT,
  GS1_REQUEST_WIFI_DISCONNECT,
  GS1_REQUEST_AUDIO_RECORD_START,
  GS1_REQUEST_AUDIO_RECORD_STOP,
  GS1_REQUEST_AUDIO_PLAY,
  GS1_REQUEST_AUDIO_STOP,
  GS1_REQUEST_AUDIO_SET_VOLUME,
  GS1_REQUEST_TIME_SYNC
};

struct gs1_wifi_connect_request_s
{
  char ssid[GS1_SSID_MAX + 1];
  char passphrase[GS1_PASSPHRASE_MAX + 1];
  bool persist;
};

struct gs1_audio_record_request_s
{
  char basename[64];
  uint32_t sample_rate;
  uint32_t limit_ms;
  bool retain_file;
  uint8_t channels;
  uint8_t bits_per_sample;
  uint8_t channel_map;
};

struct gs1_audio_play_request_s
{
  char path[GS1_PATH_MAX];
  uint32_t sample_rate;
  uint32_t limit_ms;
  uint8_t channels;
  uint8_t bits_per_sample;
  uint8_t channel_map;
};

struct gs1_audio_volume_request_s
{
  uint8_t percent;
};

struct gs1_platform_request_s
{
  enum gs1_request_op_e op;
  union
  {
    struct gs1_wifi_connect_request_s wifi_connect;
    struct gs1_audio_record_request_s audio_record;
    struct gs1_audio_play_request_s audio_play;
    struct gs1_audio_volume_request_s audio_volume;
  } data;
};

/* init() starts only the service worker.  Hardware probing and every
 * potentially blocking operation run on that worker, never the caller/UI
 * thread.  shutdown() joins the worker and may block during audio teardown.
 */

int gs1_platform_init(void);
int gs1_platform_shutdown(void);
int gs1_platform_request(FAR const struct gs1_platform_request_s *request,
                         FAR uint32_t *request_id);
int gs1_platform_get_status(FAR struct gs1_platform_status_s *status);
int gs1_platform_audio_stream_read(FAR void *buffer, size_t capacity,
                                   FAR size_t *bytes_read);
FAR const char *gs1_platform_error_string(int error);

#endif /* __GS1_PLATFORM_H */
