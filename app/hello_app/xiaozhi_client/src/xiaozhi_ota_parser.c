#include "xiaozhi_ota.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#if defined(__NuttX__)
#  include <netutils/cJSON.h>
#else
#  include <cJSON.h>
#endif

static int xiaozhi_ota_copy_json(const cJSON *object, const char *name,
                                 char *destination, size_t capacity)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  size_t length;

  destination[0] = '\0';
  if (item == NULL)
    {
      return 0;
    }

  if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
      return -EINVAL;
    }

  length = strlen(item->valuestring);
  if (length >= capacity)
    {
      return -EMSGSIZE;
    }

  memcpy(destination, item->valuestring, length + 1);
  return 0;
}

int xiaozhi_ota_parse_response(const struct xiaozhi_ota_config *config,
                               const char *json,
                               struct xiaozhi_ota_result *result)
{
  const cJSON *section;
  const cJSON *item;
  cJSON *root;
  int ret = -EINVAL;

  if (config == NULL || json == NULL || result == NULL)
    {
      return -EINVAL;
    }

  memset(result, 0, sizeof(*result));
  root = cJSON_Parse(json);
  if (!cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return -EINVAL;
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "error");
  if (item != NULL)
    {
      if (!cJSON_IsString(item) || item->valuestring == NULL)
        {
          goto done;
        }

      if (strlen(item->valuestring) >= sizeof(result->server_error))
        {
          ret = -EMSGSIZE;
          goto done;
        }

      memcpy(result->server_error, item->valuestring,
             strlen(item->valuestring) + 1);
    }

  section = cJSON_GetObjectItemCaseSensitive(root, "server_time");
  if (cJSON_IsObject(section))
    {
      item = cJSON_GetObjectItemCaseSensitive(section, "timestamp");
      if (cJSON_IsNumber(item))
        {
          result->server_timestamp_ms = (int64_t)item->valuedouble;
        }

      item = cJSON_GetObjectItemCaseSensitive(section, "timezone_offset");
      if (cJSON_IsNumber(item))
        {
          result->timezone_offset_minutes = item->valueint;
        }
    }

  section = cJSON_GetObjectItemCaseSensitive(root, "websocket");
  if (cJSON_IsObject(section))
    {
      ret = xiaozhi_ota_copy_json(section, "url", result->websocket_url,
                                  sizeof(result->websocket_url));
      if (ret < 0)
        {
          goto done;
        }

      ret = xiaozhi_ota_copy_json(section, "token",
                                  result->websocket_token,
                                  sizeof(result->websocket_token));
      if (ret < 0)
        {
          goto done;
        }

      result->has_websocket = result->websocket_url[0] != '\0';
    }

  result->has_mqtt = cJSON_IsObject(
      cJSON_GetObjectItemCaseSensitive(root, "mqtt"));

  section = cJSON_GetObjectItemCaseSensitive(root, "firmware");
  if (cJSON_IsObject(section))
    {
      ret = xiaozhi_ota_copy_json(section, "version",
                                  result->firmware_version,
                                  sizeof(result->firmware_version));
      if (ret < 0)
        {
          goto done;
        }

      ret = xiaozhi_ota_copy_json(section, "url", result->firmware_url,
                                  sizeof(result->firmware_url));
      if (ret < 0)
        {
          goto done;
        }

      result->has_firmware_update = result->firmware_url[0] != '\0' &&
          (config->firmware_version == NULL ||
           strcmp(config->firmware_version,
                  result->firmware_version) != 0);
    }

  section = cJSON_GetObjectItemCaseSensitive(root, "activation");
  if (cJSON_IsObject(section))
    {
      ret = xiaozhi_ota_copy_json(section, "code",
                                  result->activation_code,
                                  sizeof(result->activation_code));
      if (ret < 0)
        {
          goto done;
        }

      ret = xiaozhi_ota_copy_json(section, "message",
                                  result->activation_message,
                                  sizeof(result->activation_message));
      if (ret < 0)
        {
          goto done;
        }

      ret = xiaozhi_ota_copy_json(section, "challenge",
                                  result->activation_challenge,
                                  sizeof(result->activation_challenge));
      if (ret < 0)
        {
          goto done;
        }

      item = cJSON_GetObjectItemCaseSensitive(section, "timeout_ms");
      if (item != NULL)
        {
          if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
              item->valuedouble > UINT32_MAX)
            {
              ret = -ERANGE;
              goto done;
            }

          result->activation_timeout_ms = (uint32_t)item->valuedouble;
        }

      result->activation_required = result->activation_code[0] != '\0' ||
                                    result->activation_challenge[0] != '\0';
    }

  ret = result->server_error[0] == '\0' ? 0 : -EACCES;

done:
  cJSON_Delete(root);
  return ret;
}
