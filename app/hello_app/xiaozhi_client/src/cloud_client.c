#include "cloud_client.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <curl/curl.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha1.h>

#include "xiaozhi_tls.h"

#define CLOUD_DEFAULT_CONNECT_TIMEOUT_MS 10000
#define CLOUD_DEFAULT_IO_TIMEOUT_MS 10000
#define CLOUD_DEFAULT_MAX_MESSAGE_SIZE 65536
#define CLOUD_HANDSHAKE_MAX 8192
#define CLOUD_WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define CLOUD_MASK_CHUNK 512

struct cloud_client
{
  struct cloud_client_config config;
  char *url;
  char *bearer_token;
  char *device_id;
  char *client_id;
  char *user_agent;
  char *ca_file;
  CURL *easy;
  curl_socket_t socket_fd;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  char error_text[192];
  bool random_ready;
  bool connected;
  bool close_sent;
};

static unsigned int g_cloud_global_references;

static char *cloud_strdup(const char *source)
{
  size_t length;
  char *copy;

  if (source == NULL)
    {
      return NULL;
    }

  length = strlen(source);
  copy = malloc(length + 1);
  if (copy != NULL)
    {
      memcpy(copy, source, length + 1);
    }

  return copy;
}

static void cloud_secure_free(char *text)
{
  volatile char *cursor;
  size_t length;

  if (text == NULL)
    {
      return;
    }

  length = strlen(text);
  cursor = text;
  while (length-- > 0)
    {
      *cursor++ = 0;
    }

  free(text);
}

static void cloud_set_error(struct cloud_client *client,
                            const char *format, ...)
{
  va_list arguments;

  if (client == NULL)
    {
      return;
    }

  va_start(arguments, format);
  vsnprintf(client->error_text, sizeof(client->error_text),
            format, arguments);
  va_end(arguments);
}

static bool cloud_header_value_valid(const char *value)
{
  if (value == NULL)
    {
      return true;
    }

  return strchr(value, '\r') == NULL && strchr(value, '\n') == NULL;
}

static uint64_t cloud_monotonic_ms(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000u +
         (uint64_t)now.tv_nsec / 1000000u;
}

static int cloud_random(struct cloud_client *client, void *buffer,
                        size_t length)
{
  int ret;

  if (!client->random_ready)
    {
      static const unsigned char personal[] = "openvela-xiaozhi-websocket";

      ret = mbedtls_ctr_drbg_seed(&client->drbg, mbedtls_entropy_func,
                                  &client->entropy, personal,
                                  sizeof(personal) - 1);
      if (ret != 0)
        {
          cloud_set_error(client, "random seed failed: -0x%x", -ret);
          return -EIO;
        }

      client->random_ready = true;
    }

  ret = mbedtls_ctr_drbg_random(&client->drbg, buffer, length);
  if (ret != 0)
    {
      cloud_set_error(client, "random generation failed: -0x%x", -ret);
      return -EIO;
    }

  return 0;
}

static int cloud_poll_socket(struct cloud_client *client, short events,
                             uint32_t timeout_ms)
{
  struct pollfd descriptor;
  int ret;

  descriptor.fd = client->socket_fd;
  descriptor.events = events;
  descriptor.revents = 0;

  do
    {
      ret = poll(&descriptor, 1, (int)timeout_ms);
    }
  while (ret < 0 && errno == EINTR);

  if (ret == 0)
    {
      return -EAGAIN;
    }

  if (ret < 0)
    {
      cloud_set_error(client, "socket poll failed: %d", errno);
      return -errno;
    }

  if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
      cloud_set_error(client, "socket closed during poll: 0x%x",
                      descriptor.revents);
      return -ECONNRESET;
    }

  return 0;
}

static uint32_t cloud_remaining_timeout(uint64_t started_ms,
                                        uint32_t timeout_ms)
{
  uint64_t now;
  uint64_t elapsed;

  if (timeout_ms == 0)
    {
      return 0;
    }

  now = cloud_monotonic_ms();
  elapsed = now >= started_ms ? now - started_ms : 0;
  if (elapsed >= timeout_ms)
    {
      return 0;
    }

  return (uint32_t)(timeout_ms - elapsed);
}

static int cloud_send_raw(struct cloud_client *client, const void *data,
                          size_t length, uint32_t timeout_ms)
{
  const uint8_t *cursor = data;
  uint64_t started_ms = cloud_monotonic_ms();
  size_t sent;
  uint32_t remaining;
  CURLcode code;
  int ret;

  while (length > 0)
    {
      sent = 0;
      code = curl_easy_send(client->easy, cursor, length, &sent);
      if (code == CURLE_OK && sent > 0)
        {
          cursor += sent;
          length -= sent;
          continue;
        }

      if (code != CURLE_AGAIN)
        {
          cloud_set_error(client, "send failed: %s",
                          curl_easy_strerror(code));
          return -EIO;
        }

      remaining = cloud_remaining_timeout(started_ms, timeout_ms);
      if (remaining == 0)
        {
          cloud_set_error(client, "send timeout");
          return -ETIMEDOUT;
        }

      ret = cloud_poll_socket(client, POLLOUT, remaining);
      if (ret < 0)
        {
          return ret == -EAGAIN ? -ETIMEDOUT : ret;
        }
    }

  return 0;
}

static int cloud_receive_exact(struct cloud_client *client, void *data,
                               size_t length, uint32_t timeout_ms,
                               bool no_data_is_again)
{
  uint8_t *cursor = data;
  uint64_t started_ms = cloud_monotonic_ms();
  size_t received_total = 0;
  size_t received;
  uint32_t remaining;
  CURLcode code;
  int ret;

  while (received_total < length)
    {
      received = 0;
      code = curl_easy_recv(client->easy, cursor + received_total,
                            length - received_total, &received);
      if (code == CURLE_OK && received > 0)
        {
          received_total += received;
          continue;
        }

      if (code == CURLE_OK && received == 0)
        {
          cloud_set_error(client, "peer closed the connection");
          return -ECONNRESET;
        }

      if (code != CURLE_AGAIN)
        {
          cloud_set_error(client, "receive failed: %s",
                          curl_easy_strerror(code));
          return -EIO;
        }

      remaining = cloud_remaining_timeout(started_ms, timeout_ms);
      if (remaining == 0)
        {
          if (received_total == 0 && no_data_is_again)
            {
              return -EAGAIN;
            }

          cloud_set_error(client, "receive timeout in frame");
          return -ETIMEDOUT;
        }

      ret = cloud_poll_socket(client, POLLIN, remaining);
      if (ret < 0)
        {
          if (ret == -EAGAIN && received_total == 0 && no_data_is_again)
            {
              return -EAGAIN;
            }

          return ret == -EAGAIN ? -ETIMEDOUT : ret;
        }
    }

  return 0;
}

static int cloud_base64(const unsigned char *input, size_t input_length,
                        char *output, size_t output_capacity)
{
  size_t written = 0;
  int ret;

  ret = mbedtls_base64_encode((unsigned char *)output, output_capacity,
                              &written, input, input_length);
  if (ret != 0 || written >= output_capacity)
    {
      return -EIO;
    }

  output[written] = '\0';
  return 0;
}

static int cloud_expected_accept(const char *key, char *output,
                                 size_t output_capacity)
{
  unsigned char digest[20];
  char combined[96];
  int length;
  int ret;

  length = snprintf(combined, sizeof(combined), "%s%s", key,
                    CLOUD_WEBSOCKET_GUID);
  if (length < 0 || (size_t)length >= sizeof(combined))
    {
      return -EMSGSIZE;
    }

  ret = mbedtls_sha1((const unsigned char *)combined,
                     (size_t)length, digest);
  if (ret != 0)
    {
      return -EIO;
    }

  return cloud_base64(digest, sizeof(digest), output, output_capacity);
}

static char *cloud_trim(char *text)
{
  char *end;

  while (*text == ' ' || *text == '\t')
    {
      text++;
    }

  end = text + strlen(text);
  while (end > text && (end[-1] == ' ' || end[-1] == '\t'))
    {
      *--end = '\0';
    }

  return text;
}

static bool cloud_header_has_token(const char *value, const char *token)
{
  const char *cursor = value;
  const char *start;
  size_t length;

  while (*cursor != '\0')
    {
      while (*cursor == ' ' || *cursor == '\t' || *cursor == ',')
        {
          cursor++;
        }

      start = cursor;
      while (*cursor != '\0' && *cursor != ',')
        {
          cursor++;
        }

      length = (size_t)(cursor - start);
      while (length > 0 && (start[length - 1] == ' ' ||
                            start[length - 1] == '\t'))
        {
          length--;
        }

      if (strlen(token) == length && strncasecmp(start, token, length) == 0)
        {
          return true;
        }
    }

  return false;
}

static int cloud_validate_handshake(struct cloud_client *client,
                                    char *response,
                                    const char *expected_accept)
{
  char *line;
  char *next;
  char *colon;
  char *name;
  char *value;
  bool upgrade_ok = false;
  bool connection_ok = false;
  bool accept_ok = false;
  int http_major;
  int http_minor;
  int status;

  line = response;
  next = strstr(line, "\r\n");
  if (next == NULL)
    {
      return -EPROTO;
    }

  *next = '\0';
  if (sscanf(line, "HTTP/%d.%d %d", &http_major, &http_minor,
             &status) != 3 || status != 101)
    {
      cloud_set_error(client, "websocket upgrade rejected: %s", line);
      return -EACCES;
    }

  line = next + 2;
  while (*line != '\0')
    {
      next = strstr(line, "\r\n");
      if (next == NULL)
        {
          break;
        }

      *next = '\0';
      if (*line == '\0')
        {
          break;
        }

      colon = strchr(line, ':');
      if (colon != NULL)
        {
          *colon = '\0';
          name = cloud_trim(line);
          value = cloud_trim(colon + 1);
          if (strcasecmp(name, "Upgrade") == 0 &&
              strcasecmp(value, "websocket") == 0)
            {
              upgrade_ok = true;
            }
          else if (strcasecmp(name, "Connection") == 0 &&
                   cloud_header_has_token(value, "Upgrade"))
            {
              connection_ok = true;
            }
          else if (strcasecmp(name, "Sec-WebSocket-Accept") == 0 &&
                   strcmp(value, expected_accept) == 0)
            {
              accept_ok = true;
            }
        }

      line = next + 2;
    }

  if (!upgrade_ok || !connection_ok || !accept_ok)
    {
      cloud_set_error(client, "invalid websocket upgrade response");
      return -EPROTO;
    }

  return 0;
}

static int cloud_read_handshake(struct cloud_client *client,
                                char *response, size_t capacity)
{
  size_t length = 0;
  int ret;

  while (length + 1 < capacity)
    {
      ret = cloud_receive_exact(client, response + length, 1,
                                client->config.io_timeout_ms, false);
      if (ret < 0)
        {
          return ret;
        }

      length++;
      if (length >= 4 && memcmp(response + length - 4,
                                "\r\n\r\n", 4) == 0)
        {
          response[length] = '\0';
          return 0;
        }
    }

  cloud_set_error(client, "websocket handshake headers too large");
  return -EMSGSIZE;
}

static int cloud_url_parts(struct cloud_client *client,
                           char **connect_url_out,
                           char **host_header_out,
                           char **request_target_out)
{
  CURLU *url_handle = NULL;
  CURLUcode url_code;
  char *connect_url = NULL;
  char *host = NULL;
  char *port = NULL;
  char *path = NULL;
  char *query = NULL;
  char *user = NULL;
  char *host_header = NULL;
  char *target = NULL;
  const char *default_port;
  const char *suffix;
  bool ipv6;
  bool port_from_curl = false;
  bool path_from_curl = false;
  size_t length;
  int ret = -EINVAL;

  if (strncmp(client->url, "ws://", 5) == 0)
    {
      suffix = client->url + 5;
      default_port = "80";
      length = strlen(suffix) + strlen("http://") + 1;
      connect_url = malloc(length);
      if (connect_url != NULL)
        {
          snprintf(connect_url, length, "http://%s", suffix);
        }
    }
  else if (strncmp(client->url, "wss://", 6) == 0)
    {
      suffix = client->url + 6;
      default_port = "443";
      length = strlen(suffix) + strlen("https://") + 1;
      connect_url = malloc(length);
      if (connect_url != NULL)
        {
          snprintf(connect_url, length, "https://%s", suffix);
        }
    }
  else
    {
      cloud_set_error(client, "URL must use ws:// or wss://");
      return -EINVAL;
    }

  if (connect_url == NULL)
    {
      return -ENOMEM;
    }

  url_handle = curl_url();
  if (url_handle == NULL)
    {
      ret = -ENOMEM;
      goto cleanup;
    }

  url_code = curl_url_set(url_handle, CURLUPART_URL, connect_url, 0);
  if (url_code != CURLUE_OK)
    {
      cloud_set_error(client, "invalid URL: %s", curl_url_strerror(url_code));
      goto cleanup;
    }

  if (curl_url_get(url_handle, CURLUPART_USER, &user, 0) == CURLUE_OK)
    {
      cloud_set_error(client, "userinfo is not allowed in websocket URL");
      goto cleanup;
    }

  if (curl_url_get(url_handle, CURLUPART_HOST, &host, 0) != CURLUE_OK)
    {
      cloud_set_error(client, "websocket URL has no host");
      goto cleanup;
    }

  if (curl_url_get(url_handle, CURLUPART_PORT, &port, 0) != CURLUE_OK)
    {
      port = cloud_strdup(default_port);
      if (port == NULL)
        {
          ret = -ENOMEM;
          goto cleanup;
        }
    }
  else
    {
      port_from_curl = true;
    }

  if (curl_url_get(url_handle, CURLUPART_PATH, &path, 0) != CURLUE_OK)
    {
      path = cloud_strdup("/");
      if (path == NULL)
        {
          ret = -ENOMEM;
          goto cleanup;
        }
    }
  else
    {
      path_from_curl = true;
    }

  curl_url_get(url_handle, CURLUPART_QUERY, &query, 0);
  ipv6 = strchr(host, ':') != NULL;
  length = strlen(host) + strlen(port) + 5;
  host_header = malloc(length);
  if (host_header == NULL)
    {
      ret = -ENOMEM;
      goto cleanup;
    }

  if (strcmp(port, default_port) == 0)
    {
      snprintf(host_header, length, ipv6 ? "[%s]" : "%s", host);
    }
  else
    {
      snprintf(host_header, length, ipv6 ? "[%s]:%s" : "%s:%s",
               host, port);
    }

  length = strlen(path) + (query == NULL ? 0 : strlen(query) + 1) + 1;
  target = malloc(length);
  if (target == NULL)
    {
      ret = -ENOMEM;
      goto cleanup;
    }

  if (query == NULL)
    {
      snprintf(target, length, "%s", path);
    }
  else
    {
      snprintf(target, length, "%s?%s", path, query);
    }

  *connect_url_out = connect_url;
  *host_header_out = host_header;
  *request_target_out = target;
  connect_url = NULL;
  host_header = NULL;
  target = NULL;
  ret = 0;

cleanup:
  if (url_handle != NULL)
    {
      curl_url_cleanup(url_handle);
    }

  curl_free(host);
  if (port_from_curl)
    {
      curl_free(port);
    }
  else
    {
      free(port);
    }

  if (path_from_curl)
    {
      curl_free(path);
    }
  else
    {
      free(path);
    }
  curl_free(query);
  curl_free(user);
  free(connect_url);
  free(host_header);
  free(target);
  return ret;
}

static int cloud_websocket_handshake(struct cloud_client *client,
                                     const char *host_header,
                                     const char *request_target)
{
  unsigned char nonce[16];
  char key[32];
  char expected_accept[48];
  char authorization[640];
  char request[CLOUD_HANDSHAKE_MAX];
  char response[CLOUD_HANDSHAKE_MAX];
  const char *authorization_line = "";
  unsigned int protocol_version;
  int length;
  int ret;

  ret = cloud_random(client, nonce, sizeof(nonce));
  if (ret < 0)
    {
      return ret;
    }

  ret = cloud_base64(nonce, sizeof(nonce), key, sizeof(key));
  if (ret < 0)
    {
      return ret;
    }

  ret = cloud_expected_accept(key, expected_accept, sizeof(expected_accept));
  if (ret < 0)
    {
      return ret;
    }

  authorization[0] = '\0';
  if (client->bearer_token != NULL && client->bearer_token[0] != '\0')
    {
      const char *prefix = strncasecmp(client->bearer_token, "Bearer ", 7) == 0 ?
                           "" : "Bearer ";
      length = snprintf(authorization, sizeof(authorization),
                        "Authorization: %s%s\r\n", prefix,
                        client->bearer_token);
      if (length < 0 || (size_t)length >= sizeof(authorization))
        {
          return -EMSGSIZE;
        }

      authorization_line = authorization;
    }

  protocol_version = client->config.protocol_version == 0 ?
                     1 : client->config.protocol_version;
  length = snprintf(
      request, sizeof(request),
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: %s\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Protocol-Version: %u\r\n"
      "Device-Id: %s\r\n"
      "Client-Id: %s\r\n"
      "User-Agent: %s\r\n"
      "%s"
      "\r\n",
      request_target, host_header, key, protocol_version,
      client->device_id, client->client_id,
      client->user_agent == NULL ? "openvela-xiaozhi/1.0" :
                                   client->user_agent,
      authorization_line);
  if (length < 0 || (size_t)length >= sizeof(request))
    {
      cloud_set_error(client, "websocket handshake request too large");
      return -EMSGSIZE;
    }

  ret = cloud_send_raw(client, request, (size_t)length,
                       client->config.io_timeout_ms);
  if (ret < 0)
    {
      return ret;
    }

  ret = cloud_read_handshake(client, response, sizeof(response));
  if (ret < 0)
    {
      return ret;
    }

  return cloud_validate_handshake(client, response, expected_accept);
}

static int cloud_send_frame(struct cloud_client *client, uint8_t opcode,
                            const void *data, size_t length)
{
  uint8_t header[14];
  uint8_t mask[4];
  uint8_t chunk[CLOUD_MASK_CHUNK];
  const uint8_t *payload = data;
  size_t header_length;
  size_t offset;
  size_t amount;
  int ret;

  if (!client->connected)
    {
      return -ENOTCONN;
    }

  if (length > client->config.max_message_size)
    {
      return -EMSGSIZE;
    }

  if (opcode >= 0x8 && length > 125)
    {
      return -EMSGSIZE;
    }

  ret = cloud_random(client, mask, sizeof(mask));
  if (ret < 0)
    {
      return ret;
    }

  header[0] = 0x80 | opcode;
  if (length <= 125)
    {
      header[1] = 0x80 | (uint8_t)length;
      header_length = 2;
    }
  else if (length <= UINT16_MAX)
    {
      header[1] = 0x80 | 126;
      header[2] = (uint8_t)(length >> 8);
      header[3] = (uint8_t)length;
      header_length = 4;
    }
  else
    {
      uint64_t value = length;
      int index;

      header[1] = 0x80 | 127;
      for (index = 0; index < 8; index++)
        {
          header[2 + index] = (uint8_t)(value >> (56 - index * 8));
        }

      header_length = 10;
    }

  memcpy(header + header_length, mask, sizeof(mask));
  header_length += sizeof(mask);
  ret = cloud_send_raw(client, header, header_length,
                       client->config.io_timeout_ms);
  if (ret < 0)
    {
      return ret;
    }

  offset = 0;
  while (offset < length)
    {
      amount = length - offset;
      if (amount > sizeof(chunk))
        {
          amount = sizeof(chunk);
        }

      for (size_t index = 0; index < amount; index++)
        {
          chunk[index] = payload[offset + index] ^ mask[(offset + index) & 3];
        }

      ret = cloud_send_raw(client, chunk, amount,
                           client->config.io_timeout_ms);
      if (ret < 0)
        {
          return ret;
        }

      offset += amount;
    }

  return 0;
}

static int cloud_receive_payload(struct cloud_client *client,
                                 void *buffer, size_t length,
                                 uint32_t timeout_ms)
{
  if (length == 0)
    {
      return 0;
    }

  return cloud_receive_exact(client, buffer, length, timeout_ms, false);
}

int cloud_client_global_init(void)
{
  CURLcode code;

  if (g_cloud_global_references++ > 0)
    {
      return 0;
    }

  code = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (code != CURLE_OK)
    {
      g_cloud_global_references = 0;
      return -EIO;
    }

  return 0;
}

void cloud_client_global_cleanup(void)
{
  if (g_cloud_global_references == 0)
    {
      return;
    }

  if (--g_cloud_global_references == 0)
    {
      curl_global_cleanup();
    }
}

struct cloud_client *
cloud_client_create(const struct cloud_client_config *config)
{
  struct cloud_client *client;

  if (config == NULL || config->url == NULL ||
      config->device_id == NULL || config->client_id == NULL ||
      config->device_id[0] == '\0' || config->client_id[0] == '\0' ||
      (config->protocol_version != 0 && config->protocol_version != 1) ||
      !cloud_header_value_valid(config->bearer_token) ||
      !cloud_header_value_valid(config->device_id) ||
      !cloud_header_value_valid(config->client_id) ||
      !cloud_header_value_valid(config->user_agent))
    {
      return NULL;
    }

  client = calloc(1, sizeof(*client));
  if (client == NULL)
    {
      return NULL;
    }

  client->url = cloud_strdup(config->url);
  client->bearer_token = cloud_strdup(config->bearer_token);
  client->device_id = cloud_strdup(config->device_id);
  client->client_id = cloud_strdup(config->client_id);
  client->user_agent = cloud_strdup(config->user_agent);
  client->ca_file = cloud_strdup(config->ca_file);
  if (client->url == NULL || client->device_id == NULL ||
      client->client_id == NULL ||
      (config->bearer_token != NULL && client->bearer_token == NULL) ||
      (config->user_agent != NULL && client->user_agent == NULL) ||
      (config->ca_file != NULL && client->ca_file == NULL))
    {
      cloud_client_destroy(client);
      return NULL;
    }

  client->config = *config;
  client->config.url = client->url;
  client->config.bearer_token = client->bearer_token;
  client->config.device_id = client->device_id;
  client->config.client_id = client->client_id;
  client->config.user_agent = client->user_agent;
  client->config.ca_file = client->ca_file;
  if (client->config.connect_timeout_ms == 0)
    {
      client->config.connect_timeout_ms = CLOUD_DEFAULT_CONNECT_TIMEOUT_MS;
    }

  if (client->config.io_timeout_ms == 0)
    {
      client->config.io_timeout_ms = CLOUD_DEFAULT_IO_TIMEOUT_MS;
    }

  if (client->config.max_message_size == 0)
    {
      client->config.max_message_size = CLOUD_DEFAULT_MAX_MESSAGE_SIZE;
    }

  client->socket_fd = CURL_SOCKET_BAD;
  mbedtls_entropy_init(&client->entropy);
  mbedtls_ctr_drbg_init(&client->drbg);
  return client;
}

void cloud_client_destroy(struct cloud_client *client)
{
  if (client == NULL)
    {
      return;
    }

  cloud_client_close(client);
  mbedtls_ctr_drbg_free(&client->drbg);
  mbedtls_entropy_free(&client->entropy);
  free(client->url);
  cloud_secure_free(client->bearer_token);
  free(client->device_id);
  free(client->client_id);
  free(client->user_agent);
  free(client->ca_file);
  memset(client, 0, sizeof(*client));
  free(client);
}

int cloud_client_connect(struct cloud_client *client)
{
  char clock_error[192];
  char *connect_url = NULL;
  char *host_header = NULL;
  char *request_target = NULL;
  CURLcode code;
  long response_code;
  int ret;

  if (client == NULL)
    {
      return -EINVAL;
    }

  cloud_client_close(client);
  client->error_text[0] = '\0';
  ret = cloud_url_parts(client, &connect_url, &host_header,
                        &request_target);
  if (ret < 0)
    {
      goto cleanup;
    }

  if (!client->config.allow_insecure_tls &&
      strncasecmp(connect_url, "https://", 8) == 0)
    {
      memset(clock_error, 0, sizeof(clock_error));
      ret = xiaozhi_tls_ensure_realtime(
          client->config.connect_timeout_ms, clock_error,
          sizeof(clock_error));
      if (ret < 0)
        {
          cloud_set_error(client, "%s",
                          clock_error[0] == '\0' ?
                          "unable to establish trusted realtime" :
                          clock_error);
          goto cleanup;
        }
    }

  client->easy = curl_easy_init();
  if (client->easy == NULL)
    {
      ret = -ENOMEM;
      goto cleanup;
    }

  curl_easy_setopt(client->easy, CURLOPT_URL, connect_url);
  curl_easy_setopt(client->easy, CURLOPT_CONNECT_ONLY, 1L);
  curl_easy_setopt(client->easy, CURLOPT_CONNECTTIMEOUT_MS,
                   (long)client->config.connect_timeout_ms);
  curl_easy_setopt(client->easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(client->easy, CURLOPT_PROTOCOLS_STR, "http,https");
  code = xiaozhi_tls_apply(client->easy, client->config.ca_file,
                           client->config.allow_insecure_tls);
  if (code != CURLE_OK)
    {
      cloud_set_error(client, "TLS configuration failed: %s",
                      curl_easy_strerror(code));
      ret = -EIO;
      goto cleanup;
    }

  code = curl_easy_perform(client->easy);
  if (code != CURLE_OK)
    {
      cloud_set_error(client, "connect failed: %s", curl_easy_strerror(code));
      ret = code == CURLE_OPERATION_TIMEDOUT ? -ETIMEDOUT : -ECONNREFUSED;
      goto cleanup;
    }

  xiaozhi_tls_note_connection_ready(client->config.ca_file,
                                    client->config.allow_insecure_tls);

  code = curl_easy_getinfo(client->easy, CURLINFO_ACTIVESOCKET,
                           &client->socket_fd);
  if (code != CURLE_OK || client->socket_fd == CURL_SOCKET_BAD)
    {
      cloud_set_error(client, "unable to obtain connected socket");
      ret = -EIO;
      goto cleanup;
    }

  curl_easy_getinfo(client->easy, CURLINFO_RESPONSE_CODE, &response_code);
  (void)response_code;
  ret = cloud_websocket_handshake(client, host_header, request_target);
  if (ret < 0)
    {
      goto cleanup;
    }

  client->connected = true;
  client->close_sent = false;

cleanup:
  free(connect_url);
  free(host_header);
  free(request_target);
  if (ret < 0 && client->easy != NULL)
    {
      curl_easy_cleanup(client->easy);
      client->easy = NULL;
      client->socket_fd = CURL_SOCKET_BAD;
    }

  return ret;
}

int cloud_client_send_text(struct cloud_client *client,
                           const void *data, size_t length)
{
  if (client == NULL || data == NULL)
    {
      return -EINVAL;
    }

  return cloud_send_frame(client, 0x1, data, length);
}

int cloud_client_send_binary(struct cloud_client *client,
                             const void *data, size_t length)
{
  if (client == NULL || data == NULL)
    {
      return -EINVAL;
    }

  return cloud_send_frame(client, 0x2, data, length);
}

int cloud_client_receive(struct cloud_client *client, void *buffer,
                         size_t capacity, size_t *length,
                         enum cloud_frame_type *type,
                         uint32_t timeout_ms)
{
  uint8_t header[8];
  uint8_t control[125];
  uint8_t opcode;
  uint8_t message_opcode = 0;
  uint64_t payload_length;
  size_t offset = 0;
  bool final;
  int ret;
  int index;

  if (client == NULL || buffer == NULL || length == NULL || type == NULL ||
      !client->connected)
    {
      return -EINVAL;
    }

  for (;;)
    {
      ret = cloud_receive_exact(client, header, 2, timeout_ms,
                                offset == 0 && message_opcode == 0);
      if (ret < 0)
        {
          return ret;
        }

      final = (header[0] & 0x80) != 0;
      opcode = header[0] & 0x0f;
      if ((header[0] & 0x70) != 0 || (header[1] & 0x80) != 0)
        {
          cloud_set_error(client, "unsupported websocket frame flags");
          return -EPROTO;
        }

      payload_length = header[1] & 0x7f;
      if (payload_length == 126)
        {
          ret = cloud_receive_exact(client, header, 2, timeout_ms, false);
          if (ret < 0)
            {
              return ret;
            }

          payload_length = ((uint64_t)header[0] << 8) | header[1];
        }
      else if (payload_length == 127)
        {
          ret = cloud_receive_exact(client, header, 8, timeout_ms, false);
          if (ret < 0)
            {
              return ret;
            }

          if ((header[0] & 0x80) != 0)
            {
              return -EPROTO;
            }

          payload_length = 0;
          for (index = 0; index < 8; index++)
            {
              payload_length = (payload_length << 8) | header[index];
            }
        }

      if (opcode >= 0x8)
        {
          if (!final || payload_length > sizeof(control))
            {
              return -EPROTO;
            }

          if (opcode == 0x8 && payload_length == 1)
            {
              return -EPROTO;
            }

          ret = cloud_receive_payload(client, control,
                                      (size_t)payload_length, timeout_ms);
          if (ret < 0)
            {
              return ret;
            }

          if (opcode == 0x8)
            {
              if (!client->close_sent)
                {
                  cloud_send_frame(client, 0x8, control,
                                   (size_t)payload_length);
                  client->close_sent = true;
                }

              *length = (size_t)payload_length;
              *type = CLOUD_FRAME_CLOSE;
              return 0;
            }

          if (opcode == 0x9)
            {
              ret = cloud_send_frame(client, 0xA, control,
                                     (size_t)payload_length);
              if (ret < 0)
                {
                  return ret;
                }
            }
          else if (opcode != 0xA)
            {
              return -EPROTO;
            }

          continue;
        }

      if (opcode == 0x0)
        {
          if (message_opcode == 0)
            {
              return -EPROTO;
            }
        }
      else if (opcode == 0x1 || opcode == 0x2)
        {
          if (message_opcode != 0)
            {
              return -EPROTO;
            }

          message_opcode = opcode;
        }
      else
        {
          return -EPROTO;
        }

      if (offset > client->config.max_message_size || offset > capacity ||
          payload_length > client->config.max_message_size - offset ||
          payload_length > capacity - offset)
        {
          cloud_set_error(client, "websocket message exceeds configured limit");
          return -EMSGSIZE;
        }

      ret = cloud_receive_payload(client, (uint8_t *)buffer + offset,
                                  (size_t)payload_length, timeout_ms);
      if (ret < 0)
        {
          return ret;
        }

      offset += (size_t)payload_length;
      if (final)
        {
          *length = offset;
          *type = message_opcode == 0x1 ? CLOUD_FRAME_TEXT :
                                          CLOUD_FRAME_BINARY;
          return 0;
        }
    }
}

void cloud_client_close(struct cloud_client *client)
{
  uint8_t close_code[2] = {0x03, 0xe8};

  if (client == NULL)
    {
      return;
    }

  if (client->connected && !client->close_sent)
    {
      cloud_send_frame(client, 0x8, close_code, sizeof(close_code));
      client->close_sent = true;
    }

  client->connected = false;
  if (client->easy != NULL)
    {
      curl_easy_cleanup(client->easy);
      client->easy = NULL;
    }

  client->socket_fd = CURL_SOCKET_BAD;
}

uint64_t cloud_client_now_ms(struct cloud_client *client)
{
  (void)client;
  return cloud_monotonic_ms();
}

const char *cloud_client_last_error(const struct cloud_client *client)
{
  return client == NULL ? "invalid cloud client" : client->error_text;
}

static int cloud_transport_connect(void *context)
{
  return cloud_client_connect(context);
}

static int cloud_transport_send_text(void *context, const void *data,
                                     size_t length)
{
  return cloud_client_send_text(context, data, length);
}

static int cloud_transport_send_binary(void *context, const void *data,
                                       size_t length)
{
  return cloud_client_send_binary(context, data, length);
}

static int cloud_transport_receive(void *context, void *buffer,
                                   size_t capacity, size_t *length,
                                   enum cloud_frame_type *type,
                                   uint32_t timeout_ms)
{
  return cloud_client_receive(context, buffer, capacity, length,
                              type, timeout_ms);
}

static void cloud_transport_close(void *context)
{
  cloud_client_close(context);
}

static uint64_t cloud_transport_now(void *context)
{
  return cloud_client_now_ms(context);
}

const struct cloud_transport_ops *cloud_client_transport_ops(void)
{
  static const struct cloud_transport_ops operations =
  {
    .connect = cloud_transport_connect,
    .send_text = cloud_transport_send_text,
    .send_binary = cloud_transport_send_binary,
    .receive = cloud_transport_receive,
    .close = cloud_transport_close,
    .now_ms = cloud_transport_now,
  };

  return &operations;
}
