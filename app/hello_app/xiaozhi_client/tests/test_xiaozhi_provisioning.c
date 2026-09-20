#include "xiaozhi_provisioning.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                   \
  do                                                                      \
    {                                                                     \
      if (!(condition))                                                   \
        {                                                                 \
          fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                  __LINE__, #condition);                                  \
          return 1;                                                       \
        }                                                                 \
    }                                                                     \
  while (0)

struct fake_server
{
  unsigned int fetch_count;
  unsigned int activate_count;
  unsigned int callback_count;
  enum xiaozhi_pairing_state last_state;
};

static struct fake_server *g_server;

int xiaozhi_ota_fetch(const struct xiaozhi_ota_config *config,
                      struct xiaozhi_ota_result *result,
                      char *error_text, size_t error_capacity)
{
  (void)config;
  (void)result;
  (void)error_text;
  (void)error_capacity;
  return -ENOSYS;
}

int xiaozhi_ota_activate(const struct xiaozhi_ota_config *config,
                         enum xiaozhi_activation_status *status,
                         char *error_text, size_t error_capacity)
{
  (void)config;
  (void)status;
  (void)error_text;
  (void)error_capacity;
  return -ENOSYS;
}

static int fake_get_mac(void *user, const char *ifname, uint8_t mac[6])
{
  static const uint8_t expected[6] = {0x02, 0x12, 0x34, 0x56, 0x78, 0x9a};

  (void)user;
  CHECK(strcmp(ifname, "wlan-test") == 0);
  memcpy(mac, expected, sizeof(expected));
  return 0;
}

static int fake_random(void *user, uint8_t *buffer, size_t length)
{
  size_t i;

  (void)user;
  for (i = 0; i < length; i++)
    {
      buffer[i] = (uint8_t)i;
    }

  return 0;
}

static int fake_fetch(const struct xiaozhi_ota_config *config,
                      struct xiaozhi_ota_result *result,
                      char *error_text, size_t error_capacity)
{
  struct fake_server *server = g_server;

  (void)config;
  (void)error_text;
  (void)error_capacity;
  CHECK(strcmp(config->url, "https://api.tenclass.net/xiaozhi/ota/") == 0);
  CHECK(strcmp(config->device_id, "02:12:34:56:78:9a") == 0);
  CHECK(strlen(config->client_id) == 36);
  memset(result, 0, sizeof(*result));
  server->fetch_count++;
  if (server->fetch_count == 1)
    {
      snprintf(result->activation_code, sizeof(result->activation_code),
               "123456");
      snprintf(result->activation_message,
               sizeof(result->activation_message),
               "https://xiaozhi.me\n123456");
      snprintf(result->activation_challenge,
               sizeof(result->activation_challenge),
               "02:12:34:56:78:9a");
      result->activation_timeout_ms = 10000;
      result->activation_required = true;
    }
  else
    {
      snprintf(result->websocket_url, sizeof(result->websocket_url),
               "wss://api.tenclass.net/xiaozhi/v1/");
      snprintf(result->websocket_token, sizeof(result->websocket_token),
               "secret-test-token");
      result->has_websocket = true;
    }

  return 0;
}

static int fake_activate(const struct xiaozhi_ota_config *config,
                         enum xiaozhi_activation_status *status,
                         char *error_text, size_t error_capacity)
{
  struct fake_server *server = g_server;

  (void)config;
  (void)error_text;
  (void)error_capacity;
  server->activate_count++;
  *status = server->activate_count == 1 ? XIAOZHI_ACTIVATION_PENDING :
                                         XIAOZHI_ACTIVATION_COMPLETE;
  return 0;
}

static void observe_pairing(void *user,
                            const struct xiaozhi_pairing_snapshot *snapshot)
{
  struct fake_server *server = user;

  server->callback_count++;
  server->last_state = snapshot->state;
}

static int test_parser(void)
{
  static const char activation_json[] =
      "{\"server_time\":{\"timestamp\":1757567894012,"
      "\"timezone_offset\":480},"
      "\"activation\":{\"code\":\"460609\","
      "\"message\":\"https://xiaozhi.me\\n460609\","
      "\"challenge\":\"11:22:33:44:55:66\","
      "\"timeout_ms\":30000},"
      "\"websocket\":{\"url\":\"wss://example.test/v1/\","
      "\"token\":\"fixture-token\"},"
      "\"firmware\":{\"version\":\"1.2.3\",\"url\":\"\"}}";
  static const char ready_json[] =
      "{\"websocket\":{\"url\":\"wss://example.test/v1/\","
      "\"token\":\"ready-token\"}}";
  struct xiaozhi_ota_config config;
  struct xiaozhi_ota_result result;

  memset(&config, 0, sizeof(config));
  config.firmware_version = "1.2.3";
  CHECK(xiaozhi_ota_parse_response(&config, activation_json, &result) == 0);
  CHECK(result.activation_required);
  CHECK(strcmp(result.activation_code, "460609") == 0);
  CHECK(result.activation_timeout_ms == 30000);
  CHECK(result.has_websocket);
  CHECK(!result.has_firmware_update);
  CHECK(result.server_timestamp_ms == 1757567894012LL);
  CHECK(xiaozhi_ota_parse_response(&config, ready_json, &result) == 0);
  CHECK(!result.activation_required);
  CHECK(result.has_websocket);
  CHECK(strcmp(result.websocket_token, "ready-token") == 0);
  CHECK(xiaozhi_ota_parse_response(
            &config, "{\"activation\":{\"timeout_ms\":-1}}", &result) ==
        -ERANGE);
  CHECK(xiaozhi_ota_parse_response(&config, "[]", &result) == -EINVAL);
  return 0;
}

static int test_pairing_flow(void)
{
  struct xiaozhi_provisioning_config config;
  struct xiaozhi_pairing_snapshot snapshot;
  struct xiaozhi_cloud_credentials credentials;
  struct xiaozhi_provisioning *provisioning;
  struct fake_server server;
  struct stat metadata;
  char directory[] = "/tmp/xiaozhi-provisioning-XXXXXX";
  char identity_path[256];
  char credentials_path[256];
  char client_id[XIAOZHI_CLIENT_ID_MAX];

  CHECK(mkdtemp(directory) != NULL);
  CHECK(snprintf(identity_path, sizeof(identity_path), "%s/client_id",
                 directory) < (int)sizeof(identity_path));
  CHECK(snprintf(credentials_path, sizeof(credentials_path), "%s/cloud.json",
                 directory) < (int)sizeof(credentials_path));
  memset(&config, 0, sizeof(config));
  memset(&server, 0, sizeof(server));
  config.ota_url = "https://api.tenclass.net/xiaozhi/ota/";
  config.wifi_ifname = "wlan-test";
  config.identity_path = identity_path;
  config.credentials_path = credentials_path;
  config.activation_poll_ms = 100;
  config.ops.fetch = fake_fetch;
  config.ops.activate = fake_activate;
  config.ops.get_mac = fake_get_mac;
  config.ops.random_bytes = fake_random;

  g_server = &server;
  provisioning = xiaozhi_provisioning_create(&config, observe_pairing,
                                              &server);
  CHECK(provisioning != NULL);
  CHECK(xiaozhi_provisioning_start(provisioning, true, 0) == 0);
  CHECK(xiaozhi_provisioning_get_snapshot(provisioning, &snapshot) == 0);
  CHECK(strcmp(snapshot.identity.device_id, "02:12:34:56:78:9a") == 0);
  CHECK(strcmp(snapshot.identity.client_id,
               "00010203-0405-4607-8809-0a0b0c0d0e0f") == 0);
  snprintf(client_id, sizeof(client_id), "%s", snapshot.identity.client_id);

  CHECK(xiaozhi_provisioning_poll(provisioning, 0) == 0);
  CHECK(xiaozhi_provisioning_get_snapshot(provisioning, &snapshot) == 0);
  CHECK(snapshot.state == XIAOZHI_PAIRING_CODE_READY);
  CHECK(strcmp(snapshot.pairing_code, "123456") == 0);
  CHECK(xiaozhi_provisioning_poll(provisioning, 0) == 0);
  CHECK(xiaozhi_provisioning_get_snapshot(provisioning, &snapshot) == 0);
  CHECK(snapshot.state == XIAOZHI_PAIRING_WAITING);
  CHECK(xiaozhi_provisioning_poll(provisioning, 50) == -EAGAIN);
  CHECK(xiaozhi_provisioning_poll(provisioning, 100) == 0);
  CHECK(xiaozhi_provisioning_poll(provisioning, 100) == 0);
  CHECK(xiaozhi_provisioning_get_snapshot(provisioning, &snapshot) == 0);
  CHECK(snapshot.state == XIAOZHI_PAIRING_READY);
  CHECK(snapshot.has_credentials);
  CHECK(snapshot.pairing_code[0] == '\0');
  CHECK(xiaozhi_provisioning_get_credentials(provisioning,
                                              &credentials) == 0);
  CHECK(strcmp(credentials.websocket_url,
               "wss://api.tenclass.net/xiaozhi/v1/") == 0);
  CHECK(strcmp(credentials.websocket_token, "secret-test-token") == 0);
  CHECK(stat(identity_path, &metadata) == 0);
  CHECK((metadata.st_mode & 0777) == 0600);
  CHECK(stat(credentials_path, &metadata) == 0);
  CHECK((metadata.st_mode & 0777) == 0600);
  xiaozhi_provisioning_destroy(provisioning);

  memset(&server, 0, sizeof(server));
  g_server = &server;
  provisioning = xiaozhi_provisioning_create(&config, observe_pairing,
                                              &server);
  CHECK(provisioning != NULL);
  CHECK(xiaozhi_provisioning_start(provisioning, false, 1000) == 0);
  CHECK(xiaozhi_provisioning_get_snapshot(provisioning, &snapshot) == 0);
  CHECK(snapshot.state == XIAOZHI_PAIRING_READY);
  CHECK(strcmp(snapshot.identity.client_id, client_id) == 0);
  CHECK(server.fetch_count == 0);
  xiaozhi_provisioning_destroy(provisioning);

  CHECK(unlink(credentials_path) == 0);
  CHECK(unlink(identity_path) == 0);
  CHECK(rmdir(directory) == 0);
  return 0;
}

static int test_plaintext_rejected(void)
{
  struct xiaozhi_provisioning_config config;

  memset(&config, 0, sizeof(config));
  config.ota_url = "http://example.test/xiaozhi/ota/";
  CHECK(xiaozhi_provisioning_create(&config, NULL, NULL) == NULL);
  return 0;
}

int main(void)
{
  CHECK(test_parser() == 0);
  CHECK(test_pairing_flow() == 0);
  CHECK(test_plaintext_rejected() == 0);
  printf("xiaozhi provisioning host tests: PASS\n");
  return EXIT_SUCCESS;
}
