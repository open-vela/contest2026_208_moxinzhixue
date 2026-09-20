#ifndef XIAOZHI_CLIENT_H
#define XIAOZHI_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cloud_client.h"
#include "xiaozhi_audio.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define XIAOZHI_SESSION_ID_MAX 96
#define XIAOZHI_EVENT_NAME_MAX 32
#define XIAOZHI_EVENT_TEXT_MAX 4096
#define XIAOZHI_EVENT_STATUS_MAX 96
#define XIAOZHI_EVENT_EMOTION_MAX 32

enum xiaozhi_state
{
  XIAOZHI_STATE_STOPPED = 0,
  XIAOZHI_STATE_BACKOFF,
  XIAOZHI_STATE_CONNECTING,
  XIAOZHI_STATE_WAITING_HELLO,
  XIAOZHI_STATE_READY,
  XIAOZHI_STATE_LISTENING,
  XIAOZHI_STATE_THINKING,
  XIAOZHI_STATE_SPEAKING,
  XIAOZHI_STATE_DRAINING
};

enum xiaozhi_listening_mode
{
  XIAOZHI_LISTEN_AUTO = 0,
  XIAOZHI_LISTEN_MANUAL,
  XIAOZHI_LISTEN_REALTIME
};

enum xiaozhi_event_type
{
  XIAOZHI_EVENT_UNKNOWN = 0,
  XIAOZHI_EVENT_HELLO,
  XIAOZHI_EVENT_STT,
  XIAOZHI_EVENT_LLM,
  XIAOZHI_EVENT_TTS_START,
  XIAOZHI_EVENT_TTS_SENTENCE,
  XIAOZHI_EVENT_TTS_STOP,
  XIAOZHI_EVENT_MCP,
  XIAOZHI_EVENT_SYSTEM,
  XIAOZHI_EVENT_ALERT,
  XIAOZHI_EVENT_PONG
};

enum xiaozhi_error_code
{
  XIAOZHI_ERROR_TRANSPORT = 1,
  XIAOZHI_ERROR_PROTOCOL,
  XIAOZHI_ERROR_HELLO_TIMEOUT,
  XIAOZHI_ERROR_REPLY_TIMEOUT,
  XIAOZHI_ERROR_IDLE_TIMEOUT,
  XIAOZHI_ERROR_AUDIO,
  XIAOZHI_ERROR_MESSAGE_TOO_LARGE
};

struct xiaozhi_event
{
  enum xiaozhi_event_type type;
  char name[XIAOZHI_EVENT_NAME_MAX];
  char session_id[XIAOZHI_SESSION_ID_MAX];
  char text[XIAOZHI_EVENT_TEXT_MAX];
  char status[XIAOZHI_EVENT_STATUS_MAX];
  char emotion[XIAOZHI_EVENT_EMOTION_MAX];
  char command[XIAOZHI_EVENT_NAME_MAX];
  char transport[XIAOZHI_EVENT_NAME_MAX];
  uint32_t sample_rate;
  uint16_t frame_duration_ms;
  uint8_t channels;
};

struct xiaozhi_error
{
  enum xiaozhi_error_code code;
  int detail;
  const char *phase;
  bool reconnecting;
  uint32_t retry_delay_ms;
};

struct xiaozhi_client_config
{
  unsigned int protocol_version;
  uint32_t input_sample_rate;
  uint8_t input_channels;
  uint16_t frame_duration_ms;
  uint32_t hello_timeout_ms;
  uint32_t reply_timeout_ms;
  uint32_t idle_timeout_ms;
  uint32_t playback_drain_timeout_ms;
  uint32_t reconnect_initial_ms;
  uint32_t reconnect_max_ms;
  size_t receive_buffer_size;
  unsigned int max_uplink_packets_per_poll;
};

struct xiaozhi_callbacks
{
  void (*on_state)(void *user, enum xiaozhi_state old_state,
                   enum xiaozhi_state new_state);
  void (*on_event)(void *user, const struct xiaozhi_event *event);
  void (*on_error)(void *user, const struct xiaozhi_error *error);
  void (*on_json_message)(void *user, const char *json, size_t length,
                          const struct xiaozhi_event *event);
  void (*on_audio_packet)(void *user, const void *opus, size_t length,
                          uint32_t sample_rate,
                          uint16_t frame_duration_ms);
};

struct xiaozhi_client;

struct xiaozhi_client *
xiaozhi_client_create(const struct xiaozhi_client_config *config,
                      const struct cloud_transport_ops *transport_ops,
                      void *transport_context,
                      const struct xiaozhi_audio_ops *audio_ops,
                      void *audio_context,
                      const struct xiaozhi_callbacks *callbacks,
                      void *callback_user);
void xiaozhi_client_destroy(struct xiaozhi_client *client);

int xiaozhi_client_start(struct xiaozhi_client *client);
void xiaozhi_client_stop(struct xiaozhi_client *client);
int xiaozhi_client_poll(struct xiaozhi_client *client,
                        uint32_t timeout_ms);
int xiaozhi_client_reconnect(struct xiaozhi_client *client);

int xiaozhi_client_send_text_query(struct xiaozhi_client *client,
                                   const char *text);
int xiaozhi_client_start_listening(struct xiaozhi_client *client,
                                   enum xiaozhi_listening_mode mode);
int xiaozhi_client_stop_listening(struct xiaozhi_client *client);
int xiaozhi_client_abort(struct xiaozhi_client *client,
                         const char *reason);
int xiaozhi_client_send_opus(struct xiaozhi_client *client,
                             const void *opus, size_t length);

enum xiaozhi_state
xiaozhi_client_state(const struct xiaozhi_client *client);
const char *xiaozhi_client_session_id(const struct xiaozhi_client *client);

int xiaozhi_parse_server_message(const char *json, size_t length,
                                 struct xiaozhi_event *event);
const char *xiaozhi_state_name(enum xiaozhi_state state);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_CLIENT_H */
