#include "xiaozhi_provisioning.h"
#include "xiaozhi_defaults.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__NuttX__)
#  include <netutils/cJSON.h>
#  include <netutils/netlib.h>
#else
#  include <cJSON.h>
#  include <sys/random.h>
#endif

#define XIAOZHI_PROVISIONING_PATH_MAX 256
#define XIAOZHI_PROVISIONING_TEXT_MAX 128
#define XIAOZHI_PROVISIONING_DEFAULT_POLL_MS 3000
#define XIAOZHI_PROVISIONING_DEFAULT_ATTEMPTS 10
#define XIAOZHI_PROVISIONING_DEFAULT_ACTIVATION_MS 30000
#define XIAOZHI_PROVISIONING_MAX_FILE_SIZE 4096

struct xiaozhi_provisioning
{
  struct xiaozhi_provisioning_config config;
  struct xiaozhi_pairing_snapshot snapshot;
  struct xiaozhi_cloud_credentials credentials;
  struct xiaozhi_ota_result ota_result;
  xiaozhi_pairing_callback_t callback;
  void *callback_user;
  enum xiaozhi_pairing_state retry_target;
  unsigned int failures;
  char ota_url[XIAOZHI_OTA_URL_MAX];
  char language[XIAOZHI_PROVISIONING_TEXT_MAX];
  char wifi_ifname[XIAOZHI_PROVISIONING_TEXT_MAX];
  char identity_path[XIAOZHI_PROVISIONING_PATH_MAX];
  char credentials_path[XIAOZHI_PROVISIONING_PATH_MAX];
  char ca_file[XIAOZHI_PROVISIONING_PATH_MAX];
  char firmware_version[XIAOZHI_OTA_VERSION_MAX];
  char application_name[XIAOZHI_PROVISIONING_TEXT_MAX];
  char board_name[XIAOZHI_PROVISIONING_TEXT_MAX];
  char chip_model_name[XIAOZHI_PROVISIONING_TEXT_MAX];
  char user_agent[XIAOZHI_PROVISIONING_TEXT_MAX];
};

static int xiaozhi_copy(char *destination, size_t capacity,
                        const char *source)
{
  size_t length;

  if (destination == NULL || capacity == 0 || source == NULL)
    {
      return -EINVAL;
    }

  length = strlen(source);
  if (length >= capacity)
    {
      return -EMSGSIZE;
    }

  memcpy(destination, source, length + 1);
  return 0;
}

static bool xiaozhi_has_scheme(const char *url, const char *scheme)
{
  return url != NULL && strncmp(url, scheme, strlen(scheme)) == 0;
}

static bool xiaozhi_ota_url_allowed(const char *url, bool allow_plaintext)
{
  return xiaozhi_has_scheme(url, "https://") ||
         (allow_plaintext && xiaozhi_has_scheme(url, "http://"));
}

static bool xiaozhi_websocket_url_allowed(const char *url,
                                           bool allow_plaintext)
{
  return xiaozhi_has_scheme(url, "wss://") ||
         (allow_plaintext && xiaozhi_has_scheme(url, "ws://"));
}

static bool xiaozhi_device_id_valid(const char *device_id)
{
  bool all_zero = true;
  unsigned int value;
  int i;

  if (device_id == NULL || strlen(device_id) != 17)
    {
      return false;
    }

  for (i = 0; i < 6; i++)
    {
      if (sscanf(device_id + i * 3, "%2x", &value) != 1)
        {
          return false;
        }

      if (value != 0)
        {
          all_zero = false;
        }

      if (i != 5 && device_id[i * 3 + 2] != ':')
        {
          return false;
        }
    }

  return !all_zero;
}

static bool xiaozhi_client_id_valid(const char *client_id)
{
  size_t i;

  if (client_id == NULL || strlen(client_id) != 36 ||
      client_id[8] != '-' || client_id[13] != '-' ||
      client_id[18] != '-' || client_id[23] != '-' ||
      client_id[14] != '4' ||
      strchr("89ab", client_id[19]) == NULL)
    {
      return false;
    }

  for (i = 0; i < 36; i++)
    {
      if (i == 8 || i == 13 || i == 18 || i == 23)
        {
          continue;
        }

      if (strchr("0123456789abcdef", client_id[i]) == NULL)
        {
          return false;
        }
    }

  return true;
}

static int xiaozhi_default_get_mac(void *user, const char *ifname,
                                   uint8_t mac[6])
{
  (void)user;
#if defined(__NuttX__)
  if (netlib_getmacaddr(ifname, mac) < 0)
    {
      return errno == 0 ? -EIO : -errno;
    }

  return 0;
#else
  (void)ifname;
  (void)mac;
  return -ENOSYS;
#endif
}

static int xiaozhi_default_random_bytes(void *user, uint8_t *buffer,
                                        size_t length)
{
  (void)user;
#if defined(__NuttX__)
  arc4random_buf(buffer, length);
  return 0;
#else
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t count = getrandom(buffer + offset, length - offset, 0);
      if (count < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      offset += (size_t)count;
    }

  return 0;
#endif
}

static int xiaozhi_mkdir_parents(const char *path)
{
  char copy[XIAOZHI_PROVISIONING_PATH_MAX];
  char *separator;
  int ret;

  ret = xiaozhi_copy(copy, sizeof(copy), path);
  if (ret < 0)
    {
      return ret;
    }

  separator = strrchr(copy, '/');
  if (separator == NULL || separator == copy)
    {
      return 0;
    }

  *separator = '\0';
  for (separator = copy + 1; *separator != '\0'; separator++)
    {
      if (*separator != '/')
        {
          continue;
        }

      *separator = '\0';
      if (mkdir(copy, 0700) < 0 && errno != EEXIST)
        {
          return -errno;
        }

      *separator = '/';
    }

  if (mkdir(copy, 0700) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return 0;
}

static int xiaozhi_write_secure(const char *path, const char *data,
                                size_t length)
{
  char temporary[XIAOZHI_PROVISIONING_PATH_MAX];
  FILE *file;
  int ret;

  ret = xiaozhi_mkdir_parents(path);
  if (ret < 0)
    {
      return ret;
    }

  ret = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  if (ret < 0 || ret >= (int)sizeof(temporary))
    {
      return -EMSGSIZE;
    }

  file = fopen(temporary, "wb");
  if (file == NULL)
    {
      return -errno;
    }

  ret = 0;
  if (fwrite(data, 1, length, file) != length || fflush(file) != 0)
    {
      ret = errno == 0 ? -EIO : -errno;
    }
  else if (fsync(fileno(file)) < 0)
    {
      ret = -errno;
    }

  if (fclose(file) != 0 && ret == 0)
    {
      ret = -errno;
    }

  if (ret == 0 && chmod(temporary, 0600) < 0)
    {
      ret = -errno;
    }

  if (ret == 0 && rename(temporary, path) < 0)
    {
      ret = -errno;
    }

  if (ret < 0)
    {
      unlink(temporary);
    }

  return ret;
}

static int xiaozhi_read_file(const char *path, char *buffer,
                             size_t capacity)
{
  FILE *file;
  size_t length;

  if (capacity < 2)
    {
      return -EINVAL;
    }

  file = fopen(path, "rb");
  if (file == NULL)
    {
      return errno == ENOENT ? -ENOENT : -errno;
    }

  length = fread(buffer, 1, capacity - 1, file);
  if (ferror(file))
    {
      int ret = errno == 0 ? -EIO : -errno;
      fclose(file);
      return ret;
    }

  if (!feof(file))
    {
      fclose(file);
      return -EMSGSIZE;
    }

  buffer[length] = '\0';
  fclose(file);
  return (int)length;
}

static int xiaozhi_load_or_create_identity(
    struct xiaozhi_provisioning *provisioning)
{
  uint8_t uuid[16];
  uint8_t mac[6];
  char stored[XIAOZHI_CLIENT_ID_MAX + 4];
  size_t length;
  int ret;

  ret = provisioning->config.ops.get_mac(
      provisioning->config.ops_user, provisioning->config.wifi_ifname, mac);
  if (ret < 0)
    {
      return ret;
    }

  snprintf(provisioning->snapshot.identity.device_id,
           sizeof(provisioning->snapshot.identity.device_id),
           "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  if (!xiaozhi_device_id_valid(provisioning->snapshot.identity.device_id))
    {
      return -EINVAL;
    }

  ret = xiaozhi_read_file(provisioning->config.identity_path, stored,
                          sizeof(stored));
  if (ret >= 0)
    {
      length = strcspn(stored, "\r\n");
      stored[length] = '\0';
      if (xiaozhi_client_id_valid(stored))
        {
          return xiaozhi_copy(provisioning->snapshot.identity.client_id,
                              sizeof(provisioning->snapshot.identity.client_id),
                              stored);
        }
    }
  else if (ret != -ENOENT)
    {
      return ret;
    }

  ret = provisioning->config.ops.random_bytes(
      provisioning->config.ops_user, uuid, sizeof(uuid));
  if (ret < 0)
    {
      return ret;
    }

  uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40);
  uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80);
  snprintf(provisioning->snapshot.identity.client_id,
           sizeof(provisioning->snapshot.identity.client_id),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5],
           uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
           uuid[12], uuid[13], uuid[14], uuid[15]);

  return xiaozhi_write_secure(
      provisioning->config.identity_path,
      provisioning->snapshot.identity.client_id,
      strlen(provisioning->snapshot.identity.client_id));
}

static int xiaozhi_json_string(const cJSON *root, const char *name,
                               char *destination, size_t capacity)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

  if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
      return -EINVAL;
    }

  return xiaozhi_copy(destination, capacity, item->valuestring);
}

static int xiaozhi_load_credentials(struct xiaozhi_provisioning *provisioning)
{
  char data[XIAOZHI_PROVISIONING_MAX_FILE_SIZE];
  char device_id[XIAOZHI_DEVICE_ID_MAX];
  char client_id[XIAOZHI_CLIENT_ID_MAX];
  cJSON *root;
  int ret;

  ret = xiaozhi_read_file(provisioning->config.credentials_path, data,
                          sizeof(data));
  if (ret < 0)
    {
      return ret;
    }

  root = cJSON_Parse(data);
  if (!cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return -EINVAL;
    }

  ret = xiaozhi_json_string(root, "device_id", device_id,
                            sizeof(device_id));
  if (ret == 0)
    {
      ret = xiaozhi_json_string(root, "client_id", client_id,
                                sizeof(client_id));
    }

  if (ret == 0)
    {
      ret = xiaozhi_json_string(root, "websocket_url",
                                provisioning->credentials.websocket_url,
                                sizeof(provisioning->credentials.websocket_url));
    }

  if (ret == 0)
    {
      const cJSON *token = cJSON_GetObjectItemCaseSensitive(
          root, "websocket_token");
      if (token == NULL)
        {
          provisioning->credentials.websocket_token[0] = '\0';
        }
      else if (!cJSON_IsString(token) || token->valuestring == NULL)
        {
          ret = -EINVAL;
        }
      else
        {
          ret = xiaozhi_copy(provisioning->credentials.websocket_token,
                             sizeof(provisioning->credentials.websocket_token),
                             token->valuestring);
        }
    }

  if (ret == 0 &&
      (strcmp(device_id, provisioning->snapshot.identity.device_id) != 0 ||
       strcmp(client_id, provisioning->snapshot.identity.client_id) != 0 ||
       !xiaozhi_websocket_url_allowed(
           provisioning->credentials.websocket_url,
           provisioning->config.allow_plaintext)))
    {
      ret = -EINVAL;
    }

  cJSON_Delete(root);
  if (ret < 0)
    {
      memset(&provisioning->credentials, 0,
             sizeof(provisioning->credentials));
    }

  return ret;
}

static int xiaozhi_save_credentials(struct xiaozhi_provisioning *provisioning)
{
  cJSON *root = cJSON_CreateObject();
  char *json;
  int ret;

  if (root == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddNumberToObject(root, "version", 1);
  cJSON_AddStringToObject(root, "device_id",
                          provisioning->snapshot.identity.device_id);
  cJSON_AddStringToObject(root, "client_id",
                          provisioning->snapshot.identity.client_id);
  cJSON_AddStringToObject(root, "websocket_url",
                          provisioning->credentials.websocket_url);
  cJSON_AddStringToObject(root, "websocket_token",
                          provisioning->credentials.websocket_token);
  json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json == NULL)
    {
      return -ENOMEM;
    }

  ret = xiaozhi_write_secure(provisioning->config.credentials_path, json,
                             strlen(json));
  cJSON_free(json);
  return ret;
}

static void xiaozhi_emit(struct xiaozhi_provisioning *provisioning)
{
  if (provisioning->callback != NULL)
    {
      provisioning->callback(provisioning->callback_user,
                             &provisioning->snapshot);
    }
}

static void xiaozhi_set_state(struct xiaozhi_provisioning *provisioning,
                              enum xiaozhi_pairing_state state)
{
  provisioning->snapshot.state = state;
  xiaozhi_emit(provisioning);
}

static void xiaozhi_set_error(struct xiaozhi_provisioning *provisioning,
                              int error, const char *text)
{
  provisioning->snapshot.last_error = error;
  if (text == NULL || text[0] == '\0')
    {
      snprintf(provisioning->snapshot.last_error_text,
               sizeof(provisioning->snapshot.last_error_text),
               "error %d", error);
    }
  else
    {
      xiaozhi_copy(provisioning->snapshot.last_error_text,
                   sizeof(provisioning->snapshot.last_error_text), text);
    }
}

static void xiaozhi_clear_error(struct xiaozhi_provisioning *provisioning)
{
  provisioning->snapshot.last_error = 0;
  provisioning->snapshot.last_error_text[0] = '\0';
}

static uint64_t xiaozhi_retry_delay(unsigned int failures)
{
  uint64_t delay = 1000;

  while (failures > 1 && delay < 60000)
    {
      delay *= 2;
      failures--;
    }

  return delay > 60000 ? 60000 : delay;
}

static int xiaozhi_schedule_retry(
    struct xiaozhi_provisioning *provisioning,
    enum xiaozhi_pairing_state target, uint64_t now_ms,
    int error, const char *error_text)
{
  provisioning->failures++;
  provisioning->snapshot.attempt = provisioning->failures;
  xiaozhi_set_error(provisioning, error, error_text);
  if (provisioning->failures >= provisioning->config.max_attempts)
    {
      xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_ERROR);
      return error;
    }

  provisioning->retry_target = target;
  provisioning->snapshot.next_action_at_ms =
      now_ms + xiaozhi_retry_delay(provisioning->failures);
  xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_RETRY_WAIT);
  return error;
}

static void xiaozhi_build_ota_config(
    struct xiaozhi_provisioning *provisioning,
    struct xiaozhi_ota_config *config)
{
  memset(config, 0, sizeof(*config));
  config->url = provisioning->config.ota_url;
  config->device_id = provisioning->snapshot.identity.device_id;
  config->client_id = provisioning->snapshot.identity.client_id;
  config->user_agent = provisioning->config.user_agent;
  config->language = provisioning->config.language;
  config->firmware_version = provisioning->config.firmware_version;
  config->application_name = provisioning->config.application_name;
  config->board_name = provisioning->config.board_name;
  config->chip_model_name = provisioning->config.chip_model_name;
  config->ca_file = provisioning->config.ca_file;
  config->timeout_ms = provisioning->config.request_timeout_ms;
  config->allow_insecure_tls = provisioning->config.allow_insecure_tls;
}

static int xiaozhi_check(struct xiaozhi_provisioning *provisioning,
                         uint64_t now_ms)
{
  struct xiaozhi_ota_config config;
  struct xiaozhi_ota_result result;
  char error_text[XIAOZHI_PAIRING_ERROR_MAX];
  uint32_t timeout_ms;
  int ret;

  memset(&result, 0, sizeof(result));
  memset(error_text, 0, sizeof(error_text));
  xiaozhi_build_ota_config(provisioning, &config);
  xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_CHECKING);
  ret = provisioning->config.ops.fetch(&config, &result, error_text,
                                       sizeof(error_text));
  if (ret < 0)
    {
      return xiaozhi_schedule_retry(provisioning,
                                    XIAOZHI_PAIRING_CHECKING, now_ms,
                                    ret, error_text);
    }

  provisioning->failures = 0;
  provisioning->snapshot.attempt = 0;
  xiaozhi_clear_error(provisioning);
  provisioning->ota_result = result;
  if (result.activation_required)
    {
      xiaozhi_copy(provisioning->snapshot.pairing_code,
                   sizeof(provisioning->snapshot.pairing_code),
                   result.activation_code);
      xiaozhi_copy(provisioning->snapshot.pairing_message,
                   sizeof(provisioning->snapshot.pairing_message),
                   result.activation_message);
      timeout_ms = result.activation_timeout_ms == 0 ?
                   XIAOZHI_PROVISIONING_DEFAULT_ACTIVATION_MS :
                   result.activation_timeout_ms;
      provisioning->snapshot.pairing_deadline_at_ms = now_ms + timeout_ms;
      provisioning->snapshot.next_action_at_ms = now_ms;
      xiaozhi_set_state(
          provisioning, result.activation_code[0] == '\0' ?
          XIAOZHI_PAIRING_WAITING : XIAOZHI_PAIRING_CODE_READY);
      return 0;
    }

  if (!result.has_websocket)
    {
      ret = result.has_mqtt ? -ENOTSUP : -EPROTO;
      xiaozhi_set_error(provisioning, ret,
                        result.has_mqtt ?
                        "official OTA returned MQTT-only configuration" :
                        "official OTA returned no WebSocket configuration");
      xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_ERROR);
      return ret;
    }

  if (!xiaozhi_websocket_url_allowed(result.websocket_url,
                                      provisioning->config.allow_plaintext))
    {
      ret = -EPERM;
      xiaozhi_set_error(provisioning, ret,
                        "plaintext WebSocket endpoint rejected");
      xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_ERROR);
      return ret;
    }

  xiaozhi_copy(provisioning->credentials.websocket_url,
               sizeof(provisioning->credentials.websocket_url),
               result.websocket_url);
  xiaozhi_copy(provisioning->credentials.websocket_token,
               sizeof(provisioning->credentials.websocket_token),
               result.websocket_token);
  ret = xiaozhi_save_credentials(provisioning);
  if (ret < 0)
    {
      xiaozhi_set_error(provisioning, ret,
                        "unable to persist Xiaozhi credentials");
      xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_ERROR);
      return ret;
    }

  provisioning->snapshot.pairing_code[0] = '\0';
  provisioning->snapshot.pairing_message[0] = '\0';
  provisioning->snapshot.pairing_deadline_at_ms = 0;
  provisioning->snapshot.next_action_at_ms = 0;
  provisioning->snapshot.has_credentials = true;
  xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_READY);
  return 0;
}

static int xiaozhi_activate(struct xiaozhi_provisioning *provisioning,
                            uint64_t now_ms)
{
  struct xiaozhi_ota_config config;
  enum xiaozhi_activation_status status;
  char error_text[XIAOZHI_PAIRING_ERROR_MAX];
  int ret;

  if (provisioning->snapshot.pairing_deadline_at_ms != 0 &&
      now_ms >= provisioning->snapshot.pairing_deadline_at_ms)
    {
      provisioning->snapshot.next_action_at_ms = now_ms;
      xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_CHECKING);
      return 0;
    }

  memset(error_text, 0, sizeof(error_text));
  xiaozhi_build_ota_config(provisioning, &config);
  ret = provisioning->config.ops.activate(&config, &status, error_text,
                                          sizeof(error_text));
  if (ret < 0)
    {
      return xiaozhi_schedule_retry(provisioning,
                                    XIAOZHI_PAIRING_WAITING, now_ms,
                                    ret, error_text);
    }

  provisioning->failures = 0;
  provisioning->snapshot.attempt++;
  xiaozhi_clear_error(provisioning);
  if (status == XIAOZHI_ACTIVATION_COMPLETE)
    {
      provisioning->snapshot.next_action_at_ms = now_ms;
      xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_CHECKING);
      return 0;
    }

  provisioning->snapshot.next_action_at_ms =
      now_ms + provisioning->config.activation_poll_ms;
  xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_WAITING);
  return 0;
}

static int xiaozhi_configure(struct xiaozhi_provisioning *provisioning,
                             const struct xiaozhi_provisioning_config *input)
{
#define XIAOZHI_COPY_CONFIG(field, fallback)                              \
  do                                                                     \
    {                                                                    \
      const char *value = input != NULL && input->field != NULL ?        \
                          input->field : fallback;                        \
      int copy_ret = xiaozhi_copy(provisioning->field,                   \
                                  sizeof(provisioning->field), value);    \
      if (copy_ret < 0)                                                   \
        {                                                                 \
          return copy_ret;                                                \
        }                                                                 \
      provisioning->config.field = provisioning->field;                  \
    }                                                                    \
  while (0)

  XIAOZHI_COPY_CONFIG(ota_url, XIAOZHI_DEFAULT_OTA_URL);
  XIAOZHI_COPY_CONFIG(language, XIAOZHI_DEFAULT_LANGUAGE);
  XIAOZHI_COPY_CONFIG(wifi_ifname, XIAOZHI_DEFAULT_WIFI_IFNAME);
  XIAOZHI_COPY_CONFIG(identity_path, XIAOZHI_DEFAULT_IDENTITY_PATH);
  XIAOZHI_COPY_CONFIG(credentials_path, XIAOZHI_DEFAULT_CREDENTIALS_PATH);
  XIAOZHI_COPY_CONFIG(ca_file, XIAOZHI_DEFAULT_CA_FILE);
  XIAOZHI_COPY_CONFIG(firmware_version, XIAOZHI_DEFAULT_FIRMWARE_VERSION);
  XIAOZHI_COPY_CONFIG(application_name, XIAOZHI_DEFAULT_APPLICATION_NAME);
  XIAOZHI_COPY_CONFIG(board_name, XIAOZHI_DEFAULT_BOARD_NAME);
  XIAOZHI_COPY_CONFIG(chip_model_name, XIAOZHI_DEFAULT_CHIP_MODEL);
  XIAOZHI_COPY_CONFIG(user_agent, XIAOZHI_DEFAULT_USER_AGENT);

#undef XIAOZHI_COPY_CONFIG

  if (input != NULL)
    {
      provisioning->config.request_timeout_ms = input->request_timeout_ms;
      provisioning->config.activation_poll_ms = input->activation_poll_ms;
      provisioning->config.max_attempts = input->max_attempts;
      provisioning->config.allow_plaintext = input->allow_plaintext;
      provisioning->config.allow_insecure_tls = input->allow_insecure_tls;
      provisioning->config.ops = input->ops;
      provisioning->config.ops_user = input->ops_user;
    }

  if (provisioning->config.activation_poll_ms == 0)
    {
      provisioning->config.activation_poll_ms =
          XIAOZHI_PROVISIONING_DEFAULT_POLL_MS;
    }

  if (provisioning->config.max_attempts == 0)
    {
      provisioning->config.max_attempts =
          XIAOZHI_PROVISIONING_DEFAULT_ATTEMPTS;
    }

  if (provisioning->config.ops.fetch == NULL)
    {
      provisioning->config.ops.fetch = xiaozhi_ota_fetch;
    }

  if (provisioning->config.ops.activate == NULL)
    {
      provisioning->config.ops.activate = xiaozhi_ota_activate;
    }

  if (provisioning->config.ops.get_mac == NULL)
    {
      provisioning->config.ops.get_mac = xiaozhi_default_get_mac;
    }

  if (provisioning->config.ops.random_bytes == NULL)
    {
      provisioning->config.ops.random_bytes = xiaozhi_default_random_bytes;
    }

  return xiaozhi_ota_url_allowed(provisioning->config.ota_url,
                                 provisioning->config.allow_plaintext) ?
         0 : -EPERM;
}

struct xiaozhi_provisioning *
xiaozhi_provisioning_create(
    const struct xiaozhi_provisioning_config *config,
    xiaozhi_pairing_callback_t callback, void *callback_user)
{
  struct xiaozhi_provisioning *provisioning =
      calloc(1, sizeof(*provisioning));

  if (provisioning == NULL)
    {
      return NULL;
    }

  if (xiaozhi_configure(provisioning, config) < 0)
    {
      free(provisioning);
      return NULL;
    }

  provisioning->callback = callback;
  provisioning->callback_user = callback_user;
  provisioning->snapshot.state = XIAOZHI_PAIRING_IDLE;
  return provisioning;
}

void xiaozhi_provisioning_destroy(struct xiaozhi_provisioning *provisioning)
{
  if (provisioning != NULL)
    {
      memset(&provisioning->credentials, 0,
             sizeof(provisioning->credentials));
      free(provisioning);
    }
}

int xiaozhi_provisioning_start(struct xiaozhi_provisioning *provisioning,
                               bool force_refresh, uint64_t now_ms)
{
  int ret;

  if (provisioning == NULL)
    {
      return -EINVAL;
    }

  ret = xiaozhi_load_or_create_identity(provisioning);
  if (ret < 0)
    {
      xiaozhi_set_error(provisioning, ret,
                        "unable to obtain device identity");
      xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_ERROR);
      return ret;
    }

  ret = xiaozhi_load_credentials(provisioning);
  provisioning->snapshot.has_credentials = ret == 0;
  xiaozhi_clear_error(provisioning);
  provisioning->failures = 0;
  provisioning->snapshot.attempt = 0;
  if (ret == 0 && !force_refresh)
    {
      xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_READY);
      return 0;
    }

  provisioning->snapshot.next_action_at_ms = now_ms;
  xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_CHECKING);
  return 0;
}

int xiaozhi_provisioning_poll(struct xiaozhi_provisioning *provisioning,
                              uint64_t now_ms)
{
  enum xiaozhi_pairing_state state;

  if (provisioning == NULL)
    {
      return -EINVAL;
    }

  state = provisioning->snapshot.state;
  if (state == XIAOZHI_PAIRING_READY || state == XIAOZHI_PAIRING_IDLE)
    {
      return 0;
    }

  if (state == XIAOZHI_PAIRING_ERROR)
    {
      return provisioning->snapshot.last_error == 0 ? -EIO :
             provisioning->snapshot.last_error;
    }

  if (now_ms < provisioning->snapshot.next_action_at_ms)
    {
      return -EAGAIN;
    }

  if (state == XIAOZHI_PAIRING_RETRY_WAIT)
    {
      state = provisioning->retry_target;
    }

  if (state == XIAOZHI_PAIRING_CHECKING)
    {
      return xiaozhi_check(provisioning, now_ms);
    }

  if (state == XIAOZHI_PAIRING_CODE_READY ||
      state == XIAOZHI_PAIRING_WAITING)
    {
      return xiaozhi_activate(provisioning, now_ms);
    }

  return -EINVAL;
}

int xiaozhi_provisioning_retry(struct xiaozhi_provisioning *provisioning,
                               uint64_t now_ms)
{
  if (provisioning == NULL)
    {
      return -EINVAL;
    }

  provisioning->failures = 0;
  provisioning->snapshot.attempt = 0;
  provisioning->snapshot.next_action_at_ms = now_ms;
  xiaozhi_clear_error(provisioning);
  xiaozhi_set_state(provisioning, XIAOZHI_PAIRING_CHECKING);
  return 0;
}

int xiaozhi_provisioning_clear_credentials(
    struct xiaozhi_provisioning *provisioning)
{
  if (provisioning == NULL)
    {
      return -EINVAL;
    }

  if (unlink(provisioning->config.credentials_path) < 0 && errno != ENOENT)
    {
      return -errno;
    }

  memset(&provisioning->credentials, 0,
         sizeof(provisioning->credentials));
  provisioning->snapshot.has_credentials = false;
  provisioning->snapshot.state = XIAOZHI_PAIRING_IDLE;
  xiaozhi_emit(provisioning);
  return 0;
}

int xiaozhi_provisioning_get_snapshot(
    const struct xiaozhi_provisioning *provisioning,
    struct xiaozhi_pairing_snapshot *snapshot)
{
  if (provisioning == NULL || snapshot == NULL)
    {
      return -EINVAL;
    }

  *snapshot = provisioning->snapshot;
  return 0;
}

int xiaozhi_provisioning_get_credentials(
    const struct xiaozhi_provisioning *provisioning,
    struct xiaozhi_cloud_credentials *credentials)
{
  if (provisioning == NULL || credentials == NULL)
    {
      return -EINVAL;
    }

  if (!provisioning->snapshot.has_credentials)
    {
      return -ENOENT;
    }

  *credentials = provisioning->credentials;
  return 0;
}

const char *xiaozhi_pairing_state_name(enum xiaozhi_pairing_state state)
{
  switch (state)
    {
      case XIAOZHI_PAIRING_IDLE:
        return "idle";
      case XIAOZHI_PAIRING_CHECKING:
        return "checking";
      case XIAOZHI_PAIRING_CODE_READY:
        return "code_ready";
      case XIAOZHI_PAIRING_WAITING:
        return "waiting";
      case XIAOZHI_PAIRING_READY:
        return "ready";
      case XIAOZHI_PAIRING_RETRY_WAIT:
        return "retry_wait";
      case XIAOZHI_PAIRING_ERROR:
        return "error";
      default:
        return "unknown";
    }
}
