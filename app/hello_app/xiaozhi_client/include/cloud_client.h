#ifndef XIAOZHI_CLOUD_CLIENT_H
#define XIAOZHI_CLOUD_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

enum cloud_frame_type
{
  CLOUD_FRAME_TEXT = 1,
  CLOUD_FRAME_BINARY,
  CLOUD_FRAME_CLOSE
};

struct cloud_client_config
{
  const char *url;
  const char *bearer_token;
  const char *device_id;
  const char *client_id;
  const char *user_agent;
  const char *ca_file;
  unsigned int protocol_version;
  uint32_t connect_timeout_ms;
  uint32_t io_timeout_ms;
  size_t max_message_size;
  bool allow_insecure_tls;
};

struct cloud_transport_ops
{
  int (*connect)(void *context);
  int (*send_text)(void *context, const void *data, size_t length);
  int (*send_binary)(void *context, const void *data, size_t length);
  int (*receive)(void *context, void *buffer, size_t capacity,
                 size_t *length, enum cloud_frame_type *type,
                 uint32_t timeout_ms);
  void (*close)(void *context);
  uint64_t (*now_ms)(void *context);
};

struct cloud_client;

int cloud_client_global_init(void);
void cloud_client_global_cleanup(void);

struct cloud_client *
cloud_client_create(const struct cloud_client_config *config);
void cloud_client_destroy(struct cloud_client *client);

int cloud_client_connect(struct cloud_client *client);
int cloud_client_send_text(struct cloud_client *client,
                           const void *data, size_t length);
int cloud_client_send_binary(struct cloud_client *client,
                             const void *data, size_t length);
int cloud_client_receive(struct cloud_client *client, void *buffer,
                         size_t capacity, size_t *length,
                         enum cloud_frame_type *type,
                         uint32_t timeout_ms);
void cloud_client_close(struct cloud_client *client);
uint64_t cloud_client_now_ms(struct cloud_client *client);
const char *cloud_client_last_error(const struct cloud_client *client);

const struct cloud_transport_ops *cloud_client_transport_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_CLOUD_CLIENT_H */
