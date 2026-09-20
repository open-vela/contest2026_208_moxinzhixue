/****************************************************************************
 * Contest 2026 team 208 - product UI platform service boundary
 ****************************************************************************/

#ifndef __MOXINZHI_PLATFORM_H
#define __MOXINZHI_PLATFORM_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#define MZ_PLATFORM_SSID_MAX          33
#define MZ_PLATFORM_PASSWORD_MAX      65
#define MZ_PLATFORM_IP_MAX            40
#define MZ_PLATFORM_PAIR_CODE_MAX     24
#define MZ_PLATFORM_DEVICE_ID_MAX     48
#define MZ_PLATFORM_MODEL_MAX         32
#define MZ_PLATFORM_FIRMWARE_MAX      40
#define MZ_PLATFORM_AI_DETAIL_MAX     96
#define MZ_PLATFORM_WIFI_SCAN_MAX     10

enum mz_wifi_state_e
{
  MZ_WIFI_UNAVAILABLE = 0,
  MZ_WIFI_DISCONNECTED,
  MZ_WIFI_CONNECTING,
  MZ_WIFI_CONNECTED,
  MZ_WIFI_ERROR
};

enum mz_wifi_scan_state_e
{
  MZ_WIFI_SCAN_IDLE = 0,
  MZ_WIFI_SCANNING,
  MZ_WIFI_SCAN_READY,
  MZ_WIFI_SCAN_ERROR
};

struct mz_wifi_network_s
{
  char ssid[MZ_PLATFORM_SSID_MAX];
  int16_t rssi;
  bool secured;
};

enum mz_pair_state_e
{
  MZ_PAIR_UNAVAILABLE = 0,
  MZ_PAIR_UNPAIRED,
  MZ_PAIR_REQUESTING,
  MZ_PAIR_CODE_READY,
  MZ_PAIR_PAIRED,
  MZ_PAIR_ERROR
};

enum mz_ai_state_e
{
  MZ_AI_UNAVAILABLE = 0,
  MZ_AI_PAIR_REQUIRED,
  MZ_AI_CONNECTING,
  MZ_AI_READY,
  MZ_AI_STARTING,
  MZ_AI_LISTENING,
  MZ_AI_THINKING,
  MZ_AI_SPEAKING,
  MZ_AI_ERROR
};

struct mz_platform_status_s
{
  enum mz_wifi_state_e wifi_state;
  enum mz_wifi_scan_state_e wifi_scan_state;
  enum mz_pair_state_e pair_state;
  enum mz_ai_state_e ai_state;
  int ai_error;
  char wifi_ssid[MZ_PLATFORM_SSID_MAX];
  char wifi_ip[MZ_PLATFORM_IP_MAX];
  char pair_code[MZ_PLATFORM_PAIR_CODE_MAX];
  char device_id[MZ_PLATFORM_DEVICE_ID_MAX];
  char model[MZ_PLATFORM_MODEL_MAX];
  char firmware[MZ_PLATFORM_FIRMWARE_MAX];
  char ai_detail[MZ_PLATFORM_AI_DETAIL_MAX];
  struct mz_wifi_network_s wifi_networks[MZ_PLATFORM_WIFI_SCAN_MAX];
  uint32_t wifi_scan_generation;
  int wifi_scan_error;
  uint8_t volume;
  uint8_t brightness;
  uint8_t wifi_network_count;
  bool chinese;
  bool dark_theme;
};

/*
 * All callbacks are invoked from the LVGL/UI thread and must return quickly.
 * A network implementation should start asynchronous work and expose its
 * latest snapshot through refresh().  When that snapshot changes, schedule
 * mz_ui_platform_changed() on the LVGL thread.
 */

struct mz_platform_ops_s
{
  int (*refresh)(FAR void *context,
                 FAR struct mz_platform_status_s *status);
  int (*wifi_scan)(FAR void *context);
  int (*wifi_connect)(FAR void *context, FAR const char *ssid,
                      FAR const char *password);
  int (*set_volume)(FAR void *context, uint8_t volume);
  int (*set_brightness)(FAR void *context, uint8_t brightness);
  int (*set_language)(FAR void *context, bool chinese);
  int (*set_theme)(FAR void *context, bool dark_theme);
  int (*request_pairing)(FAR void *context);
  int (*forget_pairing)(FAR void *context);
  int (*ptt_begin)(FAR void *context);
  int (*ptt_end)(FAR void *context);
  int (*ptt_cancel)(FAR void *context);
};

struct mz_platform_binding_s
{
  FAR const struct mz_platform_ops_s *ops;
  FAR void *context;
};

#endif /* __MOXINZHI_PLATFORM_H */
