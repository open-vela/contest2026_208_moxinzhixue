#include "xiaozhi_ota.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

#include "xiaozhi_tls.h"

#if defined(__NuttX__)
#  include <netutils/cJSON.h>
#else
#  include <cJSON.h>
#endif

#define XIAOZHI_OTA_DEFAULT_TIMEOUT_MS 15000
#define XIAOZHI_OTA_DEFAULT_RESPONSE_SIZE 32768
#define XIAOZHI_OTA_ACTIVATE_RESPONSE_SIZE 2048

struct xiaozhi_ota_header_storage
{
  char device[256];
  char client[256];
  char user_agent[384];
  char language[192];
};

struct xiaozhi_ota_buffer
{
  char *data;
  size_t length;
  size_t capacity;
  bool overflow;
};

static void xiaozhi_ota_error(char *buffer, size_t capacity,
                              const char *format, ...)
{
  va_list arguments;

  if (buffer == NULL || capacity == 0)
    {
      return;
    }

  va_start(arguments, format);
  vsnprintf(buffer, capacity, format, arguments);
  va_end(arguments);
}

static bool xiaozhi_ota_header_valid(const char *value)
{
  return value == NULL ||
         (strchr(value, '\r') == NULL && strchr(value, '\n') == NULL);
}

static int xiaozhi_ota_add_header(struct curl_slist **headers,
                                  const char *value)
{
  struct curl_slist *updated;

  updated = curl_slist_append(*headers, value);
  if (updated == NULL)
    {
      return -ENOMEM;
    }

  *headers = updated;
  return 0;
}

static size_t xiaozhi_ota_write(void *data, size_t size, size_t count,
                                void *user)
{
  struct xiaozhi_ota_buffer *buffer = user;
  size_t length = size * count;

  if (size != 0 && length / size != count)
    {
      buffer->overflow = true;
      return 0;
    }

  if (length > buffer->capacity - buffer->length - 1)
    {
      buffer->overflow = true;
      return 0;
    }

  memcpy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
  return length;
}

static int xiaozhi_ota_build_headers(
    const struct xiaozhi_ota_config *config,
    struct curl_slist **headers,
    struct xiaozhi_ota_header_storage *storage)
{
  const char *language = config->language == NULL ? "zh-CN" :
                                                    config->language;
  int ret;

  if (!xiaozhi_ota_header_valid(config->device_id) ||
      !xiaozhi_ota_header_valid(config->client_id) ||
      !xiaozhi_ota_header_valid(config->user_agent) ||
      !xiaozhi_ota_header_valid(language))
    {
      return -EINVAL;
    }

  ret = snprintf(storage->device, sizeof(storage->device),
                 "Device-Id: %s", config->device_id);
  if (ret < 0 || ret >= (int)sizeof(storage->device))
    {
      return -EMSGSIZE;
    }

  ret = snprintf(storage->client, sizeof(storage->client),
                 "Client-Id: %s", config->client_id);
  if (ret < 0 || ret >= (int)sizeof(storage->client))
    {
      return -EMSGSIZE;
    }

  ret = snprintf(storage->language, sizeof(storage->language),
                 "Accept-Language: %s", language);
  if (ret < 0 || ret >= (int)sizeof(storage->language))
    {
      return -EMSGSIZE;
    }

  ret = xiaozhi_ota_add_header(headers, "Activation-Version: 1");
  if (ret < 0 ||
      (ret = xiaozhi_ota_add_header(headers,
                                    "Content-Type: application/json")) < 0 ||
      (ret = xiaozhi_ota_add_header(headers, storage->device)) < 0 ||
      (ret = xiaozhi_ota_add_header(headers, storage->client)) < 0 ||
      (ret = xiaozhi_ota_add_header(headers, storage->language)) < 0)
    {
      return ret;
    }

  if (config->user_agent != NULL)
    {
      ret = snprintf(storage->user_agent, sizeof(storage->user_agent),
                     "User-Agent: %s", config->user_agent);
      if (ret < 0 || ret >= (int)sizeof(storage->user_agent))
        {
          return -EMSGSIZE;
        }

      ret = xiaozhi_ota_add_header(headers, storage->user_agent);
    }

  return ret;
}

static CURLcode xiaozhi_ota_apply_curl_options(
    CURL *curl, const struct xiaozhi_ota_config *config,
    struct curl_slist *headers, struct xiaozhi_ota_buffer *response,
    uint32_t timeout_ms)
{
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, xiaozhi_ota_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                   config->allow_insecure_tls ? "http,https" : "https");
  return xiaozhi_tls_apply(curl, config->ca_file,
                           config->allow_insecure_tls);
}

static int xiaozhi_ota_ensure_realtime(
    const struct xiaozhi_ota_config *config, uint32_t timeout_ms,
    char *error_text, size_t error_capacity)
{
  if (config->allow_insecure_tls ||
      strncasecmp(config->url, "https://", 8) != 0)
    {
      return 0;
    }

  return xiaozhi_tls_ensure_realtime(timeout_ms, error_text,
                                     error_capacity);
}

int xiaozhi_ota_fetch(const struct xiaozhi_ota_config *config,
                      struct xiaozhi_ota_result *result,
                      char *error_text, size_t error_capacity)
{
  struct xiaozhi_ota_buffer response;
  struct xiaozhi_ota_header_storage header_storage;
  struct curl_slist *headers = NULL;
  cJSON *body_root = NULL;
  cJSON *application = NULL;
  cJSON *board = NULL;
  const char *application_name;
  const char *board_name;
  const char *chip_model_name;
  const char *language;
  char *body = NULL;
  CURL *curl = NULL;
  CURLcode code;
  long status = 0;
  uint32_t timeout_ms;
  size_t response_size;
  int ret = -EIO;

  if (error_text != NULL && error_capacity != 0)
    {
      error_text[0] = '\0';
    }

  if (config == NULL || result == NULL || config->url == NULL ||
      config->device_id == NULL || config->client_id == NULL ||
      config->device_id[0] == '\0' || config->client_id[0] == '\0' ||
      !xiaozhi_ota_header_valid(config->device_id) ||
      !xiaozhi_ota_header_valid(config->client_id))
    {
      return -EINVAL;
    }

  memset(result, 0, sizeof(*result));
  timeout_ms = config->timeout_ms == 0 ?
               XIAOZHI_OTA_DEFAULT_TIMEOUT_MS : config->timeout_ms;
  response_size = config->max_response_size == 0 ?
                  XIAOZHI_OTA_DEFAULT_RESPONSE_SIZE :
                  config->max_response_size;
  if (response_size == SIZE_MAX)
    {
      return -EOVERFLOW;
    }

  response.data = calloc(1, response_size + 1);
  if (response.data == NULL)
    {
      return -ENOMEM;
    }

  response.length = 0;
  response.capacity = response_size + 1;
  response.overflow = false;
  memset(&header_storage, 0, sizeof(header_storage));

  application_name = config->application_name == NULL ?
                     "moxinzhi-openvela" : config->application_name;
  board_name = config->board_name == NULL ? "gemini-s1" :
                                                config->board_name;
  chip_model_name = config->chip_model_name == NULL ? "r528" :
                                                      config->chip_model_name;
  language = config->language == NULL ? "zh-CN" : config->language;

  body_root = cJSON_CreateObject();
  application = cJSON_CreateObject();
  board = cJSON_CreateObject();
  if (body_root == NULL || application == NULL || board == NULL)
    {
      ret = -ENOMEM;
      goto cleanup;
    }

  cJSON_AddNumberToObject(body_root, "version", 2);
  cJSON_AddStringToObject(body_root, "language", language);
  cJSON_AddStringToObject(body_root, "mac_address", config->device_id);
  cJSON_AddStringToObject(body_root, "uuid", config->client_id);
  cJSON_AddStringToObject(body_root, "chip_model_name", chip_model_name);
  cJSON_AddStringToObject(application, "name", application_name);
  cJSON_AddStringToObject(application, "version",
                         config->firmware_version == NULL ? "0.0.0" :
                                                             config->firmware_version);
  cJSON_AddStringToObject(board, "name", board_name);
  cJSON_AddStringToObject(board, "type", board_name);
  cJSON_AddStringToObject(board, "mac", config->device_id);
  cJSON_AddItemToObject(body_root, "application", application);
  cJSON_AddItemToObject(body_root, "board", board);
  application = NULL;
  board = NULL;
  body = cJSON_PrintUnformatted(body_root);
  if (body == NULL)
    {
      ret = -ENOMEM;
      goto cleanup;
    }

  ret = xiaozhi_ota_build_headers(config, &headers, &header_storage);
  if (ret < 0)
    {
      goto cleanup;
    }

  ret = xiaozhi_ota_ensure_realtime(config, timeout_ms, error_text,
                                    error_capacity);
  if (ret < 0)
    {
      goto cleanup;
    }

  curl = curl_easy_init();
  if (curl == NULL)
    {
      ret = -ENOMEM;
      goto cleanup;
    }

  curl_easy_setopt(curl, CURLOPT_URL, config->url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  code = xiaozhi_ota_apply_curl_options(curl, config, headers, &response,
                                        timeout_ms);
  if (code != CURLE_OK)
    {
      xiaozhi_ota_error(error_text, error_capacity,
                        "TLS configuration failed: %s",
                        curl_easy_strerror(code));
      ret = -EIO;
      goto cleanup;
    }

  code = curl_easy_perform(curl);
  if (code != CURLE_OK)
    {
      xiaozhi_ota_error(error_text, error_capacity, "OTA request failed: %s",
                        curl_easy_strerror(code));
      ret = code == CURLE_OPERATION_TIMEDOUT ? -ETIMEDOUT : -EIO;
      goto cleanup;
    }

  xiaozhi_tls_note_connection_ready(config->ca_file,
                                    config->allow_insecure_tls);

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (response.overflow)
    {
      xiaozhi_ota_error(error_text, error_capacity,
                        "OTA response exceeds configured limit");
      ret = -EMSGSIZE;
      goto cleanup;
    }

  if (status != 200)
    {
      xiaozhi_ota_error(error_text, error_capacity,
                        "OTA server returned HTTP %ld", status);
      ret = status == 401 || status == 403 ? -EACCES : -EPROTO;
      goto cleanup;
    }

  ret = xiaozhi_ota_parse_response(config, response.data, result);
  if (ret < 0)
    {
      if (result->server_error[0] != '\0')
        {
          xiaozhi_ota_error(error_text, error_capacity,
                            "OTA server rejected device: %s",
                            result->server_error);
        }
      else
        {
          xiaozhi_ota_error(error_text, error_capacity,
                            "invalid OTA response JSON");
        }
    }

cleanup:
  if (curl != NULL)
    {
      curl_easy_cleanup(curl);
    }

  curl_slist_free_all(headers);
  cJSON_free(body);
  cJSON_Delete(application);
  cJSON_Delete(board);
  cJSON_Delete(body_root);
  free(response.data);
  return ret;
}

int xiaozhi_ota_activate(const struct xiaozhi_ota_config *config,
                         enum xiaozhi_activation_status *status,
                         char *error_text, size_t error_capacity)
{
  struct xiaozhi_ota_header_storage header_storage;
  struct xiaozhi_ota_buffer response;
  struct curl_slist *headers = NULL;
  char response_data[XIAOZHI_OTA_ACTIVATE_RESPONSE_SIZE + 1];
  char activate_url[XIAOZHI_OTA_URL_MAX];
  CURL *curl = NULL;
  CURLcode code;
  long http_status = 0;
  uint32_t timeout_ms;
  size_t url_length;
  int ret = -EIO;

  if (error_text != NULL && error_capacity != 0)
    {
      error_text[0] = '\0';
    }

  if (config == NULL || status == NULL || config->url == NULL ||
      config->device_id == NULL || config->client_id == NULL ||
      config->url[0] == '\0' || config->device_id[0] == '\0' ||
      config->client_id[0] == '\0')
    {
      return -EINVAL;
    }

  url_length = strlen(config->url);
  ret = snprintf(activate_url, sizeof(activate_url),
                 config->url[url_length - 1] == '/' ? "%sactivate" :
                                                       "%s/activate",
                 config->url);
  if (ret < 0 || ret >= (int)sizeof(activate_url))
    {
      return -EMSGSIZE;
    }

  memset(&header_storage, 0, sizeof(header_storage));
  memset(response_data, 0, sizeof(response_data));
  response.data = response_data;
  response.length = 0;
  response.capacity = sizeof(response_data);
  response.overflow = false;
  *status = XIAOZHI_ACTIVATION_PENDING;
  timeout_ms = config->timeout_ms == 0 ?
               XIAOZHI_OTA_DEFAULT_TIMEOUT_MS : config->timeout_ms;

  ret = xiaozhi_ota_build_headers(config, &headers, &header_storage);
  if (ret < 0)
    {
      goto cleanup;
    }

  ret = xiaozhi_ota_ensure_realtime(config, timeout_ms, error_text,
                                    error_capacity);
  if (ret < 0)
    {
      goto cleanup;
    }

  curl = curl_easy_init();
  if (curl == NULL)
    {
      ret = -ENOMEM;
      goto cleanup;
    }

  curl_easy_setopt(curl, CURLOPT_URL, activate_url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 2L);
  code = xiaozhi_ota_apply_curl_options(curl, config, headers, &response,
                                        timeout_ms);
  if (code != CURLE_OK)
    {
      xiaozhi_ota_error(error_text, error_capacity,
                        "TLS configuration failed: %s",
                        curl_easy_strerror(code));
      ret = -EIO;
      goto cleanup;
    }

  code = curl_easy_perform(curl);
  if (code != CURLE_OK)
    {
      xiaozhi_ota_error(error_text, error_capacity,
                        "activation request failed: %s",
                        curl_easy_strerror(code));
      ret = code == CURLE_OPERATION_TIMEDOUT ? -ETIMEDOUT : -EIO;
      goto cleanup;
    }

  xiaozhi_tls_note_connection_ready(config->ca_file,
                                    config->allow_insecure_tls);

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  if (response.overflow)
    {
      xiaozhi_ota_error(error_text, error_capacity,
                        "activation response exceeds configured limit");
      ret = -EMSGSIZE;
      goto cleanup;
    }

  if (http_status == 202)
    {
      *status = XIAOZHI_ACTIVATION_PENDING;
      ret = 0;
    }
  else if (http_status == 200)
    {
      *status = XIAOZHI_ACTIVATION_COMPLETE;
      ret = 0;
    }
  else
    {
      xiaozhi_ota_error(error_text, error_capacity,
                        "activation server returned HTTP %ld",
                        http_status);
      ret = http_status == 401 || http_status == 403 ? -EACCES :
                                                       -EPROTO;
    }

cleanup:
  if (curl != NULL)
    {
      curl_easy_cleanup(curl);
    }

  curl_slist_free_all(headers);
  return ret;
}
