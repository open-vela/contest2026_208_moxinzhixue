#ifndef XIAOZHI_PROVISIONING_H
#define XIAOZHI_PROVISIONING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiaozhi_ota.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define XIAOZHI_DEVICE_ID_MAX 18
#define XIAOZHI_CLIENT_ID_MAX 37
#define XIAOZHI_PAIRING_ERROR_MAX 192

enum xiaozhi_pairing_state
{
  XIAOZHI_PAIRING_IDLE = 0,
  XIAOZHI_PAIRING_CHECKING,
  XIAOZHI_PAIRING_CODE_READY,
  XIAOZHI_PAIRING_WAITING,
  XIAOZHI_PAIRING_READY,
  XIAOZHI_PAIRING_RETRY_WAIT,
  XIAOZHI_PAIRING_ERROR
};

struct xiaozhi_identity
{
  char device_id[XIAOZHI_DEVICE_ID_MAX];
  char client_id[XIAOZHI_CLIENT_ID_MAX];
};

struct xiaozhi_cloud_credentials
{
  char websocket_url[XIAOZHI_OTA_URL_MAX];
  char websocket_token[XIAOZHI_OTA_TOKEN_MAX];
};

struct xiaozhi_pairing_snapshot
{
  enum xiaozhi_pairing_state state;
  struct xiaozhi_identity identity;
  char pairing_code[XIAOZHI_OTA_VERSION_MAX];
  char pairing_message[XIAOZHI_OTA_ACTIVATION_MAX];
  char last_error_text[XIAOZHI_PAIRING_ERROR_MAX];
  int last_error;
  unsigned int attempt;
  uint64_t next_action_at_ms;
  uint64_t pairing_deadline_at_ms;
  bool has_credentials;
};

struct xiaozhi_provisioning_ops
{
  int (*fetch)(const struct xiaozhi_ota_config *config,
               struct xiaozhi_ota_result *result,
               char *error_text, size_t error_capacity);
  int (*activate)(const struct xiaozhi_ota_config *config,
                  enum xiaozhi_activation_status *status,
                  char *error_text, size_t error_capacity);
  int (*get_mac)(void *user, const char *ifname, uint8_t mac[6]);
  int (*random_bytes)(void *user, uint8_t *buffer, size_t length);
};

struct xiaozhi_provisioning_config
{
  const char *ota_url;
  const char *language;
  const char *wifi_ifname;
  const char *identity_path;
  const char *credentials_path;
  const char *ca_file;
  const char *firmware_version;
  const char *application_name;
  const char *board_name;
  const char *chip_model_name;
  const char *user_agent;
  uint32_t request_timeout_ms;
  uint32_t activation_poll_ms;
  unsigned int max_attempts;
  bool allow_plaintext;
  bool allow_insecure_tls;
  struct xiaozhi_provisioning_ops ops;
  void *ops_user;
};

typedef void (*xiaozhi_pairing_callback_t)(
    void *user, const struct xiaozhi_pairing_snapshot *snapshot);

struct xiaozhi_provisioning;

struct xiaozhi_provisioning *
xiaozhi_provisioning_create(
    const struct xiaozhi_provisioning_config *config,
    xiaozhi_pairing_callback_t callback, void *callback_user);
void xiaozhi_provisioning_destroy(struct xiaozhi_provisioning *provisioning);

int xiaozhi_provisioning_start(struct xiaozhi_provisioning *provisioning,
                               bool force_refresh, uint64_t now_ms);
int xiaozhi_provisioning_poll(struct xiaozhi_provisioning *provisioning,
                              uint64_t now_ms);
int xiaozhi_provisioning_retry(struct xiaozhi_provisioning *provisioning,
                               uint64_t now_ms);
int xiaozhi_provisioning_clear_credentials(
    struct xiaozhi_provisioning *provisioning);

int xiaozhi_provisioning_get_snapshot(
    const struct xiaozhi_provisioning *provisioning,
    struct xiaozhi_pairing_snapshot *snapshot);
int xiaozhi_provisioning_get_credentials(
    const struct xiaozhi_provisioning *provisioning,
    struct xiaozhi_cloud_credentials *credentials);

const char *xiaozhi_pairing_state_name(enum xiaozhi_pairing_state state);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_PROVISIONING_H */
