#include "xiaozhi_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#define TEST_QUEUE_MAX 32
#define TEST_MESSAGE_MAX 8192

#define CHECK(condition)                                                     \
  do                                                                         \
    {                                                                        \
      if (!(condition))                                                      \
        {                                                                    \
          fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,          \
                  __LINE__, #condition);                                     \
          return -1;                                                         \
        }                                                                    \
    }                                                                        \
  while (0)

struct mock_frame
{
  enum cloud_frame_type type;
  size_t length;
  unsigned char data[TEST_MESSAGE_MAX];
};

struct mock_transport
{
  struct mock_frame frames[TEST_QUEUE_MAX];
  char sent_text[TEST_QUEUE_MAX][TEST_MESSAGE_MAX];
  size_t sent_text_length[TEST_QUEUE_MAX];
  unsigned char sent_binary[TEST_MESSAGE_MAX];
  size_t sent_binary_length;
  size_t frame_head;
  size_t frame_tail;
  size_t frame_count;
  size_t sent_text_count;
  uint64_t now_ms;
  int connect_count;
  int close_count;
  int connect_failures;
  uint32_t last_receive_timeout_ms;
};

struct mock_audio
{
  unsigned char uplink[64];
  size_t uplink_length;
  size_t downlink_count;
  uint32_t sample_rate;
  uint16_t frame_duration_ms;
  uint8_t channels;
  int start_count;
  int stop_count;
  int push_count;
  int push_error;
  int finish_count;
  int flush_count;
  int capture_error;
  int playback_error;
  bool capture_started;
  bool playback_is_idle;
};

struct observer
{
  enum xiaozhi_state last_state;
  enum xiaozhi_event_type last_event;
  enum xiaozhi_error_code last_error;
  int state_changes;
  int event_count;
  int error_count;
  int json_count;
  int audio_packets;
  char last_text[XIAOZHI_EVENT_TEXT_MAX];
};

static int mock_connect(void *context)
{
  struct mock_transport *transport = context;

  transport->connect_count++;
  if (transport->connect_failures > 0)
    {
      transport->connect_failures--;
      return -ECONNREFUSED;
    }

  return 0;
}

static int mock_send_text(void *context, const void *data, size_t length)
{
  struct mock_transport *transport = context;
  size_t index = transport->sent_text_count;

  if (index >= TEST_QUEUE_MAX || length >= TEST_MESSAGE_MAX)
    {
      return -EMSGSIZE;
    }

  memcpy(transport->sent_text[index], data, length);
  transport->sent_text[index][length] = '\0';
  transport->sent_text_length[index] = length;
  transport->sent_text_count++;
  return 0;
}

static int mock_send_binary(void *context, const void *data, size_t length)
{
  struct mock_transport *transport = context;

  if (length > sizeof(transport->sent_binary))
    {
      return -EMSGSIZE;
    }

  memcpy(transport->sent_binary, data, length);
  transport->sent_binary_length = length;
  return 0;
}

static int mock_receive(void *context, void *buffer, size_t capacity,
                        size_t *length, enum cloud_frame_type *type,
                        uint32_t timeout_ms)
{
  struct mock_transport *transport = context;
  struct mock_frame *frame;

  transport->last_receive_timeout_ms = timeout_ms;
  if (transport->frame_count == 0)
    {
      return -EAGAIN;
    }

  frame = &transport->frames[transport->frame_head];
  if (frame->length > capacity)
    {
      return -EMSGSIZE;
    }

  memcpy(buffer, frame->data, frame->length);
  *length = frame->length;
  *type = frame->type;
  transport->frame_head = (transport->frame_head + 1) % TEST_QUEUE_MAX;
  transport->frame_count--;
  return 0;
}

static void mock_close(void *context)
{
  struct mock_transport *transport = context;
  transport->close_count++;
}

static uint64_t mock_now(void *context)
{
  struct mock_transport *transport = context;
  return transport->now_ms;
}

static int mock_audio_start(void *context)
{
  struct mock_audio *audio = context;
  audio->capture_started = true;
  audio->start_count++;
  return 0;
}

static int mock_audio_stop(void *context)
{
  struct mock_audio *audio = context;
  audio->capture_started = false;
  audio->stop_count++;
  return 0;
}

static int mock_audio_read(void *context, void *buffer, size_t capacity,
                           size_t *length)
{
  struct mock_audio *audio = context;

  if (audio->uplink_length == 0)
    {
      return -EAGAIN;
    }

  if (audio->uplink_length > capacity)
    {
      return -EMSGSIZE;
    }

  memcpy(buffer, audio->uplink, audio->uplink_length);
  *length = audio->uplink_length;
  audio->uplink_length = 0;
  return 0;
}

static int mock_audio_configure(void *context, uint32_t sample_rate,
                                uint8_t channels,
                                uint16_t frame_duration_ms)
{
  struct mock_audio *audio = context;
  audio->sample_rate = sample_rate;
  audio->channels = channels;
  audio->frame_duration_ms = frame_duration_ms;
  return 0;
}

static int mock_audio_push(void *context, const void *data, size_t length)
{
  struct mock_audio *audio = context;
  (void)data;
  (void)length;
  audio->push_count++;
  if (audio->push_error != 0)
    {
      return audio->push_error;
    }

  audio->downlink_count++;
  return 0;
}

static int mock_audio_finish(void *context)
{
  struct mock_audio *audio = context;
  audio->finish_count++;
  return 0;
}

static bool mock_audio_idle(void *context)
{
  struct mock_audio *audio = context;
  return audio->playback_is_idle;
}

static int mock_audio_error(void *context, bool capture)
{
  struct mock_audio *audio = context;
  return capture ? audio->capture_error : audio->playback_error;
}

static void mock_audio_flush(void *context)
{
  struct mock_audio *audio = context;
  audio->flush_count++;
}

static void observe_state(void *user, enum xiaozhi_state old_state,
                          enum xiaozhi_state new_state)
{
  struct observer *observer = user;
  (void)old_state;
  observer->last_state = new_state;
  observer->state_changes++;
}

static void observe_event(void *user, const struct xiaozhi_event *event)
{
  struct observer *observer = user;
  observer->last_event = event->type;
  observer->event_count++;
  snprintf(observer->last_text, sizeof(observer->last_text), "%s",
           event->text);
}

static void observe_error(void *user, const struct xiaozhi_error *error)
{
  struct observer *observer = user;
  observer->last_error = error->code;
  observer->error_count++;
}

static void observe_json(void *user, const char *json, size_t length,
                         const struct xiaozhi_event *event)
{
  struct observer *observer = user;
  (void)json;
  (void)length;
  (void)event;
  observer->json_count++;
}

static void observe_audio(void *user, const void *opus, size_t length,
                          uint32_t sample_rate,
                          uint16_t frame_duration_ms)
{
  struct observer *observer = user;
  (void)opus;
  (void)length;
  (void)sample_rate;
  (void)frame_duration_ms;
  observer->audio_packets++;
}

static const struct cloud_transport_ops g_transport_ops =
{
  .connect = mock_connect,
  .send_text = mock_send_text,
  .send_binary = mock_send_binary,
  .receive = mock_receive,
  .close = mock_close,
  .now_ms = mock_now,
};

static const struct xiaozhi_audio_ops g_audio_ops =
{
  .start_capture = mock_audio_start,
  .stop_capture = mock_audio_stop,
  .read_uplink_opus = mock_audio_read,
  .configure_downlink = mock_audio_configure,
  .push_downlink_opus = mock_audio_push,
  .finish_downlink = mock_audio_finish,
  .playback_idle = mock_audio_idle,
  .get_error = mock_audio_error,
  .flush = mock_audio_flush,
};

static const struct xiaozhi_callbacks g_callbacks =
{
  .on_state = observe_state,
  .on_event = observe_event,
  .on_error = observe_error,
  .on_json_message = observe_json,
  .on_audio_packet = observe_audio,
};

static int queue_frame(struct mock_transport *transport,
                       enum cloud_frame_type type,
                       const void *data, size_t length)
{
  struct mock_frame *frame;

  if (transport->frame_count == TEST_QUEUE_MAX ||
      length > TEST_MESSAGE_MAX)
    {
      return -1;
    }

  frame = &transport->frames[transport->frame_tail];
  frame->type = type;
  frame->length = length;
  memcpy(frame->data, data, length);
  transport->frame_tail = (transport->frame_tail + 1) % TEST_QUEUE_MAX;
  transport->frame_count++;
  return 0;
}

static int queue_text(struct mock_transport *transport, const char *json)
{
  return queue_frame(transport, CLOUD_FRAME_TEXT, json, strlen(json));
}

static int check_sent_json(const char *json, const char *type,
                           const char *state, const char *text)
{
  cJSON *root = cJSON_Parse(json);
  cJSON *item;

  CHECK(cJSON_IsObject(root));
  item = cJSON_GetObjectItemCaseSensitive(root, "type");
  CHECK(cJSON_IsString(item));
  CHECK(strcmp(item->valuestring, type) == 0);
  if (state != NULL)
    {
      item = cJSON_GetObjectItemCaseSensitive(root, "state");
      CHECK(cJSON_IsString(item));
      CHECK(strcmp(item->valuestring, state) == 0);
    }

  if (text != NULL)
    {
      item = cJSON_GetObjectItemCaseSensitive(root, "text");
      CHECK(cJSON_IsString(item));
      CHECK(strcmp(item->valuestring, text) == 0);
    }

  cJSON_Delete(root);
  return 0;
}

static struct xiaozhi_client *make_client(struct mock_transport *transport,
                                          struct mock_audio *audio,
                                          struct observer *observer,
                                          uint32_t timeout_ms)
{
  struct xiaozhi_client_config config;

  memset(&config, 0, sizeof(config));
  config.hello_timeout_ms = timeout_ms;
  config.reply_timeout_ms = timeout_ms;
  config.idle_timeout_ms = timeout_ms * 10;
  config.playback_drain_timeout_ms = timeout_ms;
  config.reconnect_initial_ms = 10;
  config.reconnect_max_ms = 40;
  config.receive_buffer_size = TEST_MESSAGE_MAX;

  return xiaozhi_client_create(&config, &g_transport_ops, transport,
                               &g_audio_ops, audio, &g_callbacks, observer);
}

static int connect_and_hello(struct xiaozhi_client *client,
                             struct mock_transport *transport)
{
  const char *hello =
      "{\"type\":\"hello\",\"transport\":\"websocket\","
      "\"session_id\":\"session-1\",\"audio_params\":{"
      "\"format\":\"opus\",\"sample_rate\":24000,"
      "\"channels\":1,\"frame_duration\":60}}";

  CHECK(xiaozhi_client_start(client) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_WAITING_HELLO);
  CHECK(transport->sent_text_count == 1);
  CHECK(check_sent_json(transport->sent_text[0], "hello", NULL, NULL) == 0);
  CHECK(queue_text(transport, hello) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_READY);
  CHECK(strcmp(xiaozhi_client_session_id(client), "session-1") == 0);
  return 0;
}

static int test_parser(void)
{
  struct xiaozhi_event event;
  const char *message =
      "{\"type\":\"tts\",\"state\":\"sentence_start\","
      "\"session_id\":\"abc\",\"text\":\"hello\"}";
  const char *unknown_tts =
      "{\"type\":\"tts\",\"state\":\"sentence_end\","
      "\"session_id\":\"abc\"}";
  const char *missing_optional =
      "{\"type\":\"system\",\"session_id\":\"abc\"}";
  const char *missing_sentence_text =
      "{\"type\":\"tts\",\"state\":\"sentence_start\","
      "\"session_id\":\"abc\"}";
  const char *invalid_extension_field =
      "{\"type\":\"stt\",\"session_id\":\"abc\",\"text\":42}";
  const char *oversize_type =
      "{\"type\":\"abcdefghijklmnopqrstuvwxyz0123456789\"}";
  const char *invalid_audio =
      "{\"type\":\"hello\",\"transport\":\"websocket\","
      "\"audio_params\":{\"format\":\"opus\","
      "\"sample_rate\":24000,\"channels\":1,"
      "\"frame_duration\":30}}";

  CHECK(xiaozhi_parse_server_message(message, strlen(message), &event) == 0);
  CHECK(event.type == XIAOZHI_EVENT_TTS_SENTENCE);
  CHECK(strcmp(event.session_id, "abc") == 0);
  CHECK(strcmp(event.text, "hello") == 0);
  CHECK(xiaozhi_parse_server_message("{}", 2, &event) == 0);
  CHECK(event.type == XIAOZHI_EVENT_UNKNOWN);
  CHECK(xiaozhi_parse_server_message(unknown_tts, strlen(unknown_tts),
                                      &event) == 0);
  CHECK(event.type == XIAOZHI_EVENT_UNKNOWN);
  CHECK(strcmp(event.name, "tts") == 0);
  CHECK(xiaozhi_parse_server_message(missing_optional,
                                      strlen(missing_optional), &event) == 0);
  CHECK(event.type == XIAOZHI_EVENT_SYSTEM);
  CHECK(event.command[0] == '\0');
  CHECK(xiaozhi_parse_server_message(missing_sentence_text,
                                      strlen(missing_sentence_text),
                                      &event) == 0);
  CHECK(event.type == XIAOZHI_EVENT_TTS_SENTENCE);
  CHECK(event.text[0] == '\0');
  CHECK(xiaozhi_parse_server_message(invalid_extension_field,
                                      strlen(invalid_extension_field),
                                      &event) == 0);
  CHECK(event.type == XIAOZHI_EVENT_UNKNOWN);
  CHECK(xiaozhi_parse_server_message("{", 1, &event) == -EINVAL);
  CHECK(xiaozhi_parse_server_message("[]", 2, &event) == -EINVAL);
  CHECK(xiaozhi_parse_server_message(oversize_type,
                                      strlen(oversize_type), &event) ==
        -EMSGSIZE);
  CHECK(xiaozhi_parse_server_message(invalid_audio, strlen(invalid_audio),
                                      &event) == -EINVAL);
  return 0;
}

static int test_config_validation(void)
{
  struct xiaozhi_client_config config;
  struct mock_transport transport;

  memset(&config, 0, sizeof(config));
  memset(&transport, 0, sizeof(transport));
  config.protocol_version = 2;
  CHECK(xiaozhi_client_create(&config, &g_transport_ops, &transport,
                              NULL, NULL, NULL, NULL) == NULL);
  config.protocol_version = 1;
  config.frame_duration_ms = 30;
  CHECK(xiaozhi_client_create(&config, &g_transport_ops, &transport,
                              NULL, NULL, NULL, NULL) == NULL);
  return 0;
}

static int test_text_and_audio_flow(void)
{
  struct mock_transport transport;
  struct mock_audio audio;
  struct observer observer;
  struct xiaozhi_client *client;
  const unsigned char opus[] = {1, 2, 3, 4};
  const char *query = "hello \"xiaozhi\"";

  memset(&transport, 0, sizeof(transport));
  memset(&audio, 0, sizeof(audio));
  memset(&observer, 0, sizeof(observer));
  audio.playback_is_idle = true;
  client = make_client(&transport, &audio, &observer, 100);
  CHECK(client != NULL);
  CHECK(connect_and_hello(client, &transport) == 0);
  CHECK(audio.sample_rate == 24000);
  CHECK(audio.channels == 1);
  CHECK(audio.frame_duration_ms == 60);

  CHECK(xiaozhi_client_send_text_query(client, query) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_THINKING);
  CHECK(check_sent_json(transport.sent_text[1], "listen", "detect",
                       query) == 0);
  CHECK(queue_text(&transport,
                   "{\"type\":\"stt\",\"session_id\":\"session-1\","
                   "\"text\":\"hello xiaozhi\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(observer.last_event == XIAOZHI_EVENT_STT);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"start\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_SPEAKING);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"sentence_start\","
                   "\"session_id\":\"session-1\","
                   "\"text\":\"reply text\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(strcmp(observer.last_text, "reply text") == 0);
  CHECK(queue_frame(&transport, CLOUD_FRAME_BINARY, opus, sizeof(opus)) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(audio.downlink_count == 1);
  CHECK(observer.audio_packets == 1);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"stop\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_READY);
  CHECK(audio.finish_count == 1);
  CHECK(observer.json_count == 5);

  CHECK(xiaozhi_client_start_listening(client,
                                       XIAOZHI_LISTEN_MANUAL) == 0);
  CHECK(audio.capture_started);
  memcpy(audio.uplink, opus, sizeof(opus));
  audio.uplink_length = sizeof(opus);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(transport.sent_binary_length == sizeof(opus));
  CHECK(memcmp(transport.sent_binary, opus, sizeof(opus)) == 0);
  CHECK(xiaozhi_client_stop_listening(client) == 0);
  CHECK(!audio.capture_started);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_THINKING);

  xiaozhi_client_destroy(client);
  return 0;
}

static int test_binary_before_tts_start(void)
{
  struct mock_transport transport;
  struct mock_audio audio;
  struct observer observer;
  struct xiaozhi_client *client;
  const unsigned char opus[] = {1, 2, 3, 4};
  int flush_count;
  int start_count;

  memset(&transport, 0, sizeof(transport));
  memset(&audio, 0, sizeof(audio));
  memset(&observer, 0, sizeof(observer));
  audio.playback_is_idle = true;
  client = make_client(&transport, &audio, &observer, 100);
  CHECK(client != NULL);
  CHECK(connect_and_hello(client, &transport) == 0);

  CHECK(queue_frame(&transport, CLOUD_FRAME_BINARY, opus, sizeof(opus)) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_READY);
  CHECK(audio.downlink_count == 0);
  CHECK(observer.audio_packets == 0);

  CHECK(xiaozhi_client_start_listening(client, XIAOZHI_LISTEN_AUTO) == 0);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"start\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_SPEAKING);
  CHECK(xiaozhi_client_abort(client, "replace auto reply") == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_READY);

  CHECK(xiaozhi_client_start_listening(client,
                                       XIAOZHI_LISTEN_MANUAL) == 0);
  CHECK(queue_frame(&transport, CLOUD_FRAME_BINARY, opus, sizeof(opus)) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_LISTENING);
  CHECK(audio.downlink_count == 0);
  CHECK(observer.audio_packets == 0);
  CHECK(xiaozhi_client_stop_listening(client) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_THINKING);

  flush_count = audio.flush_count;
  CHECK(queue_frame(&transport, CLOUD_FRAME_BINARY, opus, 0) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_THINKING);
  CHECK(audio.flush_count == flush_count);
  CHECK(audio.push_count == 0);
  CHECK(observer.audio_packets == 0);
  CHECK(xiaozhi_client_poll(client, 1000) == 0);
  CHECK(transport.last_receive_timeout_ms == 100);

  CHECK(queue_frame(&transport, CLOUD_FRAME_BINARY, opus, sizeof(opus)) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_SPEAKING);
  CHECK(audio.flush_count == flush_count + 1);
  CHECK(audio.push_count == 1);
  CHECK(audio.downlink_count == 1);
  CHECK(observer.audio_packets == 1);
  CHECK(xiaozhi_client_poll(client, 1000) == 0);
  CHECK(transport.last_receive_timeout_ms == 1000);

  flush_count++;
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"start\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_SPEAKING);
  CHECK(audio.flush_count == flush_count);
  CHECK(audio.downlink_count == 1);

  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"stop\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_READY);
  CHECK(audio.finish_count == 1);
  CHECK(!audio.capture_started);
  start_count = audio.start_count;
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(audio.start_count == start_count);

  xiaozhi_client_destroy(client);
  return 0;
}

static int test_binary_push_failure_stays_thinking(void)
{
  struct mock_transport transport;
  struct mock_audio audio;
  struct observer observer;
  struct xiaozhi_client *client;
  const unsigned char opus[] = {1, 2, 3, 4};
  int flush_count;

  memset(&transport, 0, sizeof(transport));
  memset(&audio, 0, sizeof(audio));
  memset(&observer, 0, sizeof(observer));
  audio.playback_is_idle = true;
  audio.push_error = -EIO;
  client = make_client(&transport, &audio, &observer, 100);
  CHECK(client != NULL);
  CHECK(connect_and_hello(client, &transport) == 0);
  CHECK(xiaozhi_client_send_text_query(client, "reject first packet") == 0);
  flush_count = audio.flush_count;

  CHECK(queue_frame(&transport, CLOUD_FRAME_BINARY, opus, sizeof(opus)) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_THINKING);
  CHECK(audio.flush_count == flush_count + 1);
  CHECK(audio.push_count == 1);
  CHECK(audio.downlink_count == 0);
  CHECK(observer.audio_packets == 0);
  CHECK(observer.last_error == XIAOZHI_ERROR_AUDIO);
  CHECK(xiaozhi_client_poll(client, 1000) == 0);
  CHECK(transport.last_receive_timeout_ms == 100);

  xiaozhi_client_destroy(client);
  return 0;
}

static int test_drain_and_auto_resume(void)
{
  struct mock_transport transport;
  struct mock_audio audio;
  struct observer observer;
  struct xiaozhi_client *client;

  memset(&transport, 0, sizeof(transport));
  memset(&audio, 0, sizeof(audio));
  memset(&observer, 0, sizeof(observer));
  audio.playback_is_idle = true;
  client = make_client(&transport, &audio, &observer, 100);
  CHECK(client != NULL);
  CHECK(connect_and_hello(client, &transport) == 0);
  CHECK(xiaozhi_client_start_listening(client,
                                       XIAOZHI_LISTEN_REALTIME) ==
        -ENOTSUP);
  CHECK(xiaozhi_client_start_listening(client, XIAOZHI_LISTEN_AUTO) == 0);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"start\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(!audio.capture_started);

  audio.playback_is_idle = false;
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"stop\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_DRAINING);
  audio.playback_is_idle = true;
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_LISTENING);
  CHECK(audio.capture_started);
  CHECK(audio.start_count == 2);

  xiaozhi_client_destroy(client);
  return 0;
}

static int test_abort_and_manual_restart_drops_stale_downlink(void)
{
  struct mock_transport transport;
  struct mock_audio audio;
  struct observer observer;
  struct xiaozhi_client *client;
  const unsigned char old_opus[] = {1, 2, 3, 4};
  const unsigned char new_opus[] = {5, 6, 7, 8};
  int stop_count;
  int event_count;
  int flush_count;

  memset(&transport, 0, sizeof(transport));
  memset(&audio, 0, sizeof(audio));
  memset(&observer, 0, sizeof(observer));
  audio.playback_is_idle = true;
  client = make_client(&transport, &audio, &observer, 100);
  CHECK(client != NULL);
  CHECK(connect_and_hello(client, &transport) == 0);
  CHECK(xiaozhi_client_send_text_query(client, "interrupt me") == 0);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"start\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_SPEAKING);
  CHECK(queue_frame(&transport, CLOUD_FRAME_BINARY,
                    old_opus, sizeof(old_opus)) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(audio.downlink_count == 1);
  CHECK(observer.audio_packets == 1);

  flush_count = audio.flush_count;
  CHECK(xiaozhi_client_abort(client, "user_barge_in") == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_READY);
  CHECK(audio.flush_count == flush_count + 1);
  CHECK(xiaozhi_client_start_listening(client,
                                       XIAOZHI_LISTEN_MANUAL) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_LISTENING);
  CHECK(audio.capture_started);

  stop_count = audio.stop_count;
  event_count = observer.event_count;
  CHECK(queue_frame(&transport, CLOUD_FRAME_BINARY,
                    old_opus, sizeof(old_opus)) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(audio.downlink_count == 1);
  CHECK(observer.audio_packets == 1);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"stop\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_LISTENING);
  CHECK(audio.finish_count == 0);
  CHECK(observer.event_count == event_count);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"start\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_LISTENING);
  CHECK(audio.capture_started);
  CHECK(audio.stop_count == stop_count);
  CHECK(observer.event_count == event_count);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"sentence_start\","
                   "\"session_id\":\"session-1\","
                   "\"text\":\"stale reply\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_LISTENING);
  CHECK(observer.event_count == event_count);

  CHECK(xiaozhi_client_stop_listening(client) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_THINKING);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"start\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_SPEAKING);
  CHECK(queue_frame(&transport, CLOUD_FRAME_BINARY,
                    new_opus, sizeof(new_opus)) == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(audio.downlink_count == 2);
  CHECK(observer.audio_packets == 2);
  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"stop\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_READY);
  CHECK(audio.finish_count == 1);

  xiaozhi_client_destroy(client);
  return 0;
}

static int test_extension_compatibility(void)
{
  struct mock_transport transport;
  struct mock_audio audio;
  struct observer observer;
  struct xiaozhi_client *client;

  memset(&transport, 0, sizeof(transport));
  memset(&audio, 0, sizeof(audio));
  memset(&observer, 0, sizeof(observer));
  audio.playback_is_idle = true;
  client = make_client(&transport, &audio, &observer, 100);
  CHECK(client != NULL);
  CHECK(connect_and_hello(client, &transport) == 0);
  CHECK(xiaozhi_client_send_text_query(client, "compatibility") == 0);

  CHECK(queue_text(&transport,
                   "{\"type\":\"future_extension\","
                   "\"session_id\":\"another-session\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_THINKING);
  CHECK(observer.last_event == XIAOZHI_EVENT_UNKNOWN);
  CHECK(observer.error_count == 0);

  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"start\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_SPEAKING);

  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"sentence_start\","
                   "\"session_id\":\"session-1\","
                   "\"text\":\"compatible answer\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(observer.last_event == XIAOZHI_EVENT_TTS_SENTENCE);
  CHECK(strcmp(observer.last_text, "compatible answer") == 0);

  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"sentence_end\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_SPEAKING);
  CHECK(observer.last_event == XIAOZHI_EVENT_UNKNOWN);
  CHECK(observer.error_count == 0);

  CHECK(queue_text(&transport,
                   "{\"type\":\"tts\",\"state\":\"stop\","
                   "\"session_id\":\"session-1\"}") == 0);
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_READY);
  CHECK(observer.error_count == 0);

  xiaozhi_client_destroy(client);
  return 0;
}

static int test_reconnect_and_timeout(void)
{
  struct mock_transport transport;
  struct mock_audio audio;
  struct observer observer;
  struct xiaozhi_client *client;

  memset(&transport, 0, sizeof(transport));
  memset(&audio, 0, sizeof(audio));
  memset(&observer, 0, sizeof(observer));
  transport.connect_failures = 1;
  audio.playback_is_idle = true;
  client = make_client(&transport, &audio, &observer, 50);
  CHECK(client != NULL);
  CHECK(xiaozhi_client_start(client) == 0);
  CHECK(xiaozhi_client_poll(client, 0) < 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_BACKOFF);
  CHECK(observer.last_error == XIAOZHI_ERROR_TRANSPORT);
  transport.now_ms += 10;
  CHECK(xiaozhi_client_poll(client, 0) == 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_WAITING_HELLO);
  transport.now_ms += 50;
  CHECK(xiaozhi_client_poll(client, 0) < 0);
  CHECK(xiaozhi_client_state(client) == XIAOZHI_STATE_BACKOFF);
  CHECK(observer.last_error == XIAOZHI_ERROR_HELLO_TIMEOUT);

  xiaozhi_client_destroy(client);
  return 0;
}

int main(void)
{
  CHECK(test_parser() == 0);
  CHECK(test_config_validation() == 0);
  CHECK(test_text_and_audio_flow() == 0);
  CHECK(test_binary_before_tts_start() == 0);
  CHECK(test_binary_push_failure_stays_thinking() == 0);
  CHECK(test_drain_and_auto_resume() == 0);
  CHECK(test_abort_and_manual_restart_drops_stale_downlink() == 0);
  CHECK(test_extension_compatibility() == 0);
  CHECK(test_reconnect_and_timeout() == 0);
  printf("xiaozhi_client host tests: PASS\n");
  return EXIT_SUCCESS;
}
