#ifndef XIAOZHI_OTA_H
#define XIAOZHI_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define XIAOZHI_OTA_URL_MAX 512
#define XIAOZHI_OTA_TOKEN_MAX 512
#define XIAOZHI_OTA_VERSION_MAX 64
#define XIAOZHI_OTA_ACTIVATION_MAX 512
#define XIAOZHI_OTA_ERROR_MAX 192

enum xiaozhi_activation_status
{
  XIAOZHI_ACTIVATION_PENDING = 0,
  XIAOZHI_ACTIVATION_COMPLETE
};

struct xiaozhi_ota_config
{
  const char *url;
  const char *device_id;
  const char *client_id;
  const char *user_agent;
  const char *language;
  const char *firmware_version;
  const char *application_name;
  const char *board_name;
  const char *chip_model_name;
  const char *ca_file;
  uint32_t timeout_ms;
  size_t max_response_size;
  bool allow_insecure_tls;
};

struct xiaozhi_ota_result
{
  char websocket_url[XIAOZHI_OTA_URL_MAX];
  char websocket_token[XIAOZHI_OTA_TOKEN_MAX];
  char firmware_version[XIAOZHI_OTA_VERSION_MAX];
  char firmware_url[XIAOZHI_OTA_URL_MAX];
  char activation_code[XIAOZHI_OTA_VERSION_MAX];
  char activation_message[XIAOZHI_OTA_ACTIVATION_MAX];
  char activation_challenge[XIAOZHI_OTA_ACTIVATION_MAX];
  char server_error[XIAOZHI_OTA_ERROR_MAX];
  int64_t server_timestamp_ms;
  int timezone_offset_minutes;
  uint32_t activation_timeout_ms;
  bool has_websocket;
  bool has_mqtt;
  bool has_firmware_update;
  bool activation_required;
};

int xiaozhi_ota_fetch(const struct xiaozhi_ota_config *config,
                      struct xiaozhi_ota_result *result,
                      char *error_text, size_t error_capacity);

int xiaozhi_ota_activate(const struct xiaozhi_ota_config *config,
                         enum xiaozhi_activation_status *status,
                         char *error_text, size_t error_capacity);

int xiaozhi_ota_parse_response(const struct xiaozhi_ota_config *config,
                               const char *json,
                               struct xiaozhi_ota_result *result);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_OTA_H */
