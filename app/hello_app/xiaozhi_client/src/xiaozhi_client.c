#include "xiaozhi_client.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__NuttX__)
#  include <netutils/cJSON.h>
#else
#  include <cJSON.h>
#endif

#define XIAOZHI_DEFAULT_PROTOCOL_VERSION 1
#define XIAOZHI_DEFAULT_INPUT_RATE 16000
#define XIAOZHI_DEFAULT_FRAME_MS 20
#define XIAOZHI_DEFAULT_HELLO_TIMEOUT_MS 10000
#define XIAOZHI_DEFAULT_REPLY_TIMEOUT_MS 120000
#define XIAOZHI_DEFAULT_IDLE_TIMEOUT_MS 120000
#define XIAOZHI_DEFAULT_DRAIN_TIMEOUT_MS 5000
#define XIAOZHI_DEFAULT_RECONNECT_INITIAL_MS 1000
#define XIAOZHI_DEFAULT_RECONNECT_MAX_MS 30000
#define XIAOZHI_DEFAULT_RX_BUFFER_SIZE 16384
#define XIAOZHI_DEFAULT_UPLINK_BURST 4

struct xiaozhi_client
{
  struct xiaozhi_client_config config;
  struct cloud_transport_ops transport;
  struct xiaozhi_audio_ops audio;
  struct xiaozhi_callbacks callbacks;
  void *transport_context;
  void *audio_context;
  void *callback_user;
  enum xiaozhi_state state;
  enum xiaozhi_listening_mode listening_mode;
  char session_id[XIAOZHI_SESSION_ID_MAX];
  uint8_t *receive_buffer;
  uint64_t state_deadline_ms;
  uint64_t reconnect_at_ms;
  uint64_t last_receive_ms;
  uint32_t reconnect_attempt;
  uint32_t server_sample_rate;
  uint16_t server_frame_duration_ms;
  uint8_t server_channels;
  bool started;
  bool resume_listening_after_tts;
  bool audio_error_reported;
  bool playback_error_reported;
};

static size_t xiaozhi_strnlen(const char *text, size_t maximum)
{
  size_t length = 0;

  if (text == NULL)
    {
      return 0;
    }

  while (length < maximum && text[length] != '\0')
    {
      length++;
    }

  return length;
}

static int xiaozhi_copy(char *destination, size_t capacity,
                        const char *source)
{
  size_t length;

  if (destination == NULL || capacity == 0)
    {
      return -EINVAL;
    }

  destination[0] = '\0';
  if (source == NULL)
    {
      return 0;
    }

  length = xiaozhi_strnlen(source, capacity);
  if (length >= capacity)
    {
      return -EMSGSIZE;
    }

  memcpy(destination, source, length + 1);
  return 0;
}

static int xiaozhi_json_string(const cJSON *object, const char *name,
                               char *destination, size_t capacity,
                               bool required)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

  if (item == NULL && !required)
    {
      destination[0] = '\0';
      return 0;
    }

  if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
      return -EINVAL;
    }

  return xiaozhi_copy(destination, capacity, item->valuestring);
}

static uint64_t xiaozhi_now(struct xiaozhi_client *client)
{
  return client->transport.now_ms(client->transport_context);
}

static void xiaozhi_set_state(struct xiaozhi_client *client,
                              enum xiaozhi_state state)
{
  enum xiaozhi_state old_state;

  if (client->state == state)
    {
      return;
    }

  old_state = client->state;
  client->state = state;
  if (client->callbacks.on_state != NULL)
    {
      client->callbacks.on_state(client->callback_user, old_state, state);
    }
}

static void xiaozhi_report_error(struct xiaozhi_client *client,
                                 enum xiaozhi_error_code code,
                                 int detail, const char *phase,
                                 bool reconnecting,
                                 uint32_t retry_delay_ms)
{
  struct xiaozhi_error error;

  if (client->callbacks.on_error == NULL)
    {
      return;
    }

  error.code = code;
  error.detail = detail;
  error.phase = phase;
  error.reconnecting = reconnecting;
  error.retry_delay_ms = retry_delay_ms;
  client->callbacks.on_error(client->callback_user, &error);
}

static void xiaozhi_stop_audio(struct xiaozhi_client *client)
{
  if (client->audio.stop_capture != NULL)
    {
      client->audio.stop_capture(client->audio_context);
    }

  if (client->audio.flush != NULL)
    {
      client->audio.flush(client->audio_context);
    }
}

static void xiaozhi_prepare_tts_audio(struct xiaozhi_client *client,
                                      bool resume_listening)
{
  client->resume_listening_after_tts = resume_listening;
  if (client->audio.stop_capture != NULL)
    {
      client->audio.stop_capture(client->audio_context);
    }

  if (client->audio.flush != NULL)
    {
      client->audio.flush(client->audio_context);
    }
}

static uint32_t xiaozhi_reconnect_delay(struct xiaozhi_client *client)
{
  uint64_t delay = client->config.reconnect_initial_ms;
  uint32_t shift = client->reconnect_attempt;

  if (shift > 20)
    {
      shift = 20;
    }

  delay <<= shift;
  if (delay > client->config.reconnect_max_ms)
    {
      delay = client->config.reconnect_max_ms;
    }

  return (uint32_t)delay;
}

static int xiaozhi_schedule_reconnect(struct xiaozhi_client *client,
                                      enum xiaozhi_error_code code,
                                      int detail, const char *phase)
{
  uint32_t delay;

  xiaozhi_stop_audio(client);
  client->transport.close(client->transport_context);
  client->session_id[0] = '\0';
  client->state_deadline_ms = 0;
  client->resume_listening_after_tts = false;
  client->audio_error_reported = false;
  client->playback_error_reported = false;

  delay = xiaozhi_reconnect_delay(client);
  if (client->reconnect_attempt < UINT32_MAX)
    {
      client->reconnect_attempt++;
    }

  client->reconnect_at_ms = xiaozhi_now(client) + delay;
  xiaozhi_set_state(client, XIAOZHI_STATE_BACKOFF);
  xiaozhi_report_error(client, code, detail, phase, true, delay);
  return detail < 0 ? detail : -EIO;
}

static void xiaozhi_apply_defaults(struct xiaozhi_client_config *config)
{
  if (config->protocol_version == 0)
    {
      config->protocol_version = XIAOZHI_DEFAULT_PROTOCOL_VERSION;
    }

  if (config->input_sample_rate == 0)
    {
      config->input_sample_rate = XIAOZHI_DEFAULT_INPUT_RATE;
    }

  if (config->input_channels == 0)
    {
      config->input_channels = 1;
    }

  if (config->frame_duration_ms == 0)
    {
      config->frame_duration_ms = XIAOZHI_DEFAULT_FRAME_MS;
    }

  if (config->hello_timeout_ms == 0)
    {
      config->hello_timeout_ms = XIAOZHI_DEFAULT_HELLO_TIMEOUT_MS;
    }

  if (config->reply_timeout_ms == 0)
    {
      config->reply_timeout_ms = XIAOZHI_DEFAULT_REPLY_TIMEOUT_MS;
    }

  if (config->idle_timeout_ms == 0)
    {
      config->idle_timeout_ms = XIAOZHI_DEFAULT_IDLE_TIMEOUT_MS;
    }

  if (config->playback_drain_timeout_ms == 0)
    {
      config->playback_drain_timeout_ms = XIAOZHI_DEFAULT_DRAIN_TIMEOUT_MS;
    }

  if (config->reconnect_initial_ms == 0)
    {
      config->reconnect_initial_ms = XIAOZHI_DEFAULT_RECONNECT_INITIAL_MS;
    }

  if (config->reconnect_max_ms == 0)
    {
      config->reconnect_max_ms = XIAOZHI_DEFAULT_RECONNECT_MAX_MS;
    }

  if (config->reconnect_max_ms < config->reconnect_initial_ms)
    {
      config->reconnect_max_ms = config->reconnect_initial_ms;
    }

  if (config->receive_buffer_size == 0)
    {
      config->receive_buffer_size = XIAOZHI_DEFAULT_RX_BUFFER_SIZE;
    }

  if (config->max_uplink_packets_per_poll == 0)
    {
      config->max_uplink_packets_per_poll = XIAOZHI_DEFAULT_UPLINK_BURST;
    }
}

static bool xiaozhi_opus_rate_valid(uint32_t rate)
{
  return rate == 8000 || rate == 12000 || rate == 16000 ||
         rate == 24000 || rate == 48000;
}

static bool xiaozhi_opus_frame_duration_valid(uint16_t duration_ms)
{
  return duration_ms == 5 || duration_ms == 10 || duration_ms == 20 ||
         duration_ms == 40 || duration_ms == 60;
}

static bool xiaozhi_config_valid(const struct xiaozhi_client_config *config)
{
  return config->protocol_version == 1 &&
         xiaozhi_opus_rate_valid(config->input_sample_rate) &&
         config->input_channels > 0 && config->input_channels <= 2 &&
         xiaozhi_opus_frame_duration_valid(config->frame_duration_ms) &&
         config->receive_buffer_size > 0;
}

static int xiaozhi_send_json(struct xiaozhi_client *client, cJSON *root)
{
  char *json;
  size_t length;
  int ret;

  json = cJSON_PrintUnformatted(root);
  if (json == NULL)
    {
      return -ENOMEM;
    }

  length = strlen(json);
  ret = client->transport.send_text(client->transport_context,
                                    json, length);
  cJSON_free(json);
  return ret;
}

static int xiaozhi_send_hello(struct xiaozhi_client *client)
{
  cJSON *root;
  cJSON *audio;
  int ret;

  root = cJSON_CreateObject();
  audio = cJSON_CreateObject();
  if (root == NULL || audio == NULL)
    {
      cJSON_Delete(root);
      cJSON_Delete(audio);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "type", "hello");
  cJSON_AddNumberToObject(root, "version", client->config.protocol_version);
  cJSON_AddStringToObject(root, "transport", "websocket");
  cJSON_AddStringToObject(audio, "format", "opus");
  cJSON_AddNumberToObject(audio, "sample_rate",
                          client->config.input_sample_rate);
  cJSON_AddNumberToObject(audio, "channels",
                          client->config.input_channels);
  cJSON_AddNumberToObject(audio, "frame_duration",
                          client->config.frame_duration_ms);
  cJSON_AddItemToObject(root, "audio_params", audio);

  ret = xiaozhi_send_json(client, root);
  cJSON_Delete(root);
  return ret;
}

static int xiaozhi_send_listen(struct xiaozhi_client *client,
                               const char *state,
                               const char *mode,
                               const char *text)
{
  cJSON *root = cJSON_CreateObject();
  int ret;

  if (root == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "session_id", client->session_id);
  cJSON_AddStringToObject(root, "type", "listen");
  cJSON_AddStringToObject(root, "state", state);
  if (mode != NULL)
    {
      cJSON_AddStringToObject(root, "mode", mode);
    }

  if (text != NULL)
    {
      cJSON_AddStringToObject(root, "text", text);
    }

  ret = xiaozhi_send_json(client, root);
  cJSON_Delete(root);
  return ret;
}

static int xiaozhi_parse_audio_params(const cJSON *root,
                                      struct xiaozhi_event *event)
{
  const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root,
                                                        "audio_params");
  const cJSON *item;

  if (!cJSON_IsObject(audio))
    {
      return 0;
    }

  item = cJSON_GetObjectItemCaseSensitive(audio, "format");
  if (item != NULL && (!cJSON_IsString(item) ||
                       item->valuestring == NULL ||
                       strcmp(item->valuestring, "opus") != 0))
    {
      return -ENOTSUP;
    }

  item = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
  if (item != NULL)
    {
      if (!cJSON_IsNumber(item) || item->valuedouble <= 0 ||
          item->valuedouble > UINT32_MAX ||
          !xiaozhi_opus_rate_valid((uint32_t)item->valuedouble))
        {
          return -EINVAL;
        }

      event->sample_rate = (uint32_t)item->valuedouble;
    }

  item = cJSON_GetObjectItemCaseSensitive(audio, "channels");
  if (item != NULL)
    {
      if (!cJSON_IsNumber(item) || item->valueint <= 0 ||
          item->valueint > 2)
        {
          return -EINVAL;
        }

      event->channels = (uint8_t)item->valueint;
    }

  item = cJSON_GetObjectItemCaseSensitive(audio, "frame_duration");
  if (item != NULL)
    {
      if (!cJSON_IsNumber(item) || item->valueint <= 0 ||
          item->valueint > UINT16_MAX ||
          !xiaozhi_opus_frame_duration_valid((uint16_t)item->valueint))
        {
          return -EINVAL;
        }

      event->frame_duration_ms = (uint16_t)item->valueint;
    }

  return 0;
}

int xiaozhi_parse_server_message(const char *json, size_t length,
                                 struct xiaozhi_event *event)
{
  cJSON *root;
  char *copy;
  char type[XIAOZHI_EVENT_NAME_MAX];
  char state[XIAOZHI_EVENT_NAME_MAX];
  int ret;

  if (json == NULL || event == NULL || length == 0)
    {
      return -EINVAL;
    }

  memset(event, 0, sizeof(*event));
  copy = malloc(length + 1);
  if (copy == NULL)
    {
      return -ENOMEM;
    }

  memcpy(copy, json, length);
  copy[length] = '\0';
  root = cJSON_Parse(copy);
  free(copy);
  if (!cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return -EINVAL;
    }

  ret = xiaozhi_json_string(root, "type", type, sizeof(type), false);
  if (ret == -EINVAL || (ret == 0 && type[0] == '\0'))
    {
      event->type = XIAOZHI_EVENT_UNKNOWN;
      cJSON_Delete(root);
      return 0;
    }

  if (ret < 0)
    {
      cJSON_Delete(root);
      return ret;
    }

  ret = xiaozhi_copy(event->name, sizeof(event->name), type);
  if (ret < 0)
    {
      cJSON_Delete(root);
      return ret;
    }

  ret = xiaozhi_json_string(root, "session_id", event->session_id,
                            sizeof(event->session_id), false);
  if (ret == -EINVAL && strcmp(type, "hello") != 0)
    {
      event->type = XIAOZHI_EVENT_UNKNOWN;
      cJSON_Delete(root);
      return 0;
    }

  if (ret < 0)
    {
      cJSON_Delete(root);
      return ret;
    }

  if (strcmp(type, "hello") == 0)
    {
      event->type = XIAOZHI_EVENT_HELLO;
      ret = xiaozhi_json_string(root, "transport", event->transport,
                                sizeof(event->transport), true);
      if (ret == 0)
        {
          ret = xiaozhi_parse_audio_params(root, event);
        }
    }
  else if (strcmp(type, "stt") == 0)
    {
      event->type = XIAOZHI_EVENT_STT;
      ret = xiaozhi_json_string(root, "text", event->text,
                                sizeof(event->text), false);
      if (ret == -EINVAL)
        {
          event->type = XIAOZHI_EVENT_UNKNOWN;
          ret = 0;
        }
    }
  else if (strcmp(type, "llm") == 0)
    {
      event->type = XIAOZHI_EVENT_LLM;
      ret = xiaozhi_json_string(root, "text", event->text,
                                sizeof(event->text), false);
      if (ret == 0)
        {
          ret = xiaozhi_json_string(root, "emotion", event->emotion,
                                    sizeof(event->emotion), false);
        }

      if (ret == -EINVAL)
        {
          event->type = XIAOZHI_EVENT_UNKNOWN;
          ret = 0;
        }
    }
  else if (strcmp(type, "tts") == 0)
    {
      ret = xiaozhi_json_string(root, "state", state, sizeof(state), false);
      if (ret == 0 && strcmp(state, "start") == 0)
        {
          event->type = XIAOZHI_EVENT_TTS_START;
        }
      else if (ret == 0 && strcmp(state, "stop") == 0)
        {
          event->type = XIAOZHI_EVENT_TTS_STOP;
        }
      else if (ret == 0 && strcmp(state, "sentence_start") == 0)
        {
          event->type = XIAOZHI_EVENT_TTS_SENTENCE;
          ret = xiaozhi_json_string(root, "text", event->text,
                                    sizeof(event->text), false);
          if (ret == -EINVAL)
            {
              event->type = XIAOZHI_EVENT_UNKNOWN;
              ret = 0;
            }
        }
      else if (ret == 0)
        {
          event->type = XIAOZHI_EVENT_UNKNOWN;
        }
      else if (ret == -EINVAL)
        {
          event->type = XIAOZHI_EVENT_UNKNOWN;
          ret = 0;
        }
    }
  else if (strcmp(type, "mcp") == 0)
    {
      event->type = XIAOZHI_EVENT_MCP;
      ret = 0;
    }
  else if (strcmp(type, "system") == 0)
    {
      event->type = XIAOZHI_EVENT_SYSTEM;
      ret = xiaozhi_json_string(root, "command", event->command,
                                sizeof(event->command), false);
      if (ret == -EINVAL)
        {
          event->type = XIAOZHI_EVENT_UNKNOWN;
          ret = 0;
        }
    }
  else if (strcmp(type, "alert") == 0)
    {
      event->type = XIAOZHI_EVENT_ALERT;
      ret = xiaozhi_json_string(root, "status", event->status,
                                sizeof(event->status), false);
      if (ret == 0)
        {
          ret = xiaozhi_json_string(root, "message", event->text,
                                    sizeof(event->text), false);
        }

      if (ret == 0)
        {
          ret = xiaozhi_json_string(root, "emotion", event->emotion,
                                    sizeof(event->emotion), false);
        }

      if (ret == -EINVAL)
        {
          event->type = XIAOZHI_EVENT_UNKNOWN;
          ret = 0;
        }
    }
  else if (strcmp(type, "pong") == 0)
    {
      event->type = XIAOZHI_EVENT_PONG;
      ret = 0;
    }
  else
    {
      event->type = XIAOZHI_EVENT_UNKNOWN;
      ret = 0;
    }

  cJSON_Delete(root);
  return ret;
}

static bool xiaozhi_session_matches(struct xiaozhi_client *client,
                                    const struct xiaozhi_event *event)
{
  if (client->session_id[0] == '\0' || event->session_id[0] == '\0')
    {
      return true;
    }

  return strcmp(client->session_id, event->session_id) == 0;
}

static void xiaozhi_emit_event(struct xiaozhi_client *client,
                               const struct xiaozhi_event *event)
{
  if (client->callbacks.on_event != NULL)
    {
      client->callbacks.on_event(client->callback_user, event);
    }
}

static int xiaozhi_complete_playback(struct xiaozhi_client *client)
{
  int ret;

  if (client->resume_listening_after_tts)
    {
      client->resume_listening_after_tts = false;
      if (client->audio.start_capture != NULL)
        {
          ret = client->audio.start_capture(client->audio_context);
          if (ret < 0)
            {
              xiaozhi_report_error(client, XIAOZHI_ERROR_AUDIO, ret,
                                   "resume_capture", false, 0);
              xiaozhi_set_state(client, XIAOZHI_STATE_READY);
              return ret;
            }
        }

      xiaozhi_set_state(client, XIAOZHI_STATE_LISTENING);
    }
  else
    {
      xiaozhi_set_state(client, XIAOZHI_STATE_READY);
    }

  client->state_deadline_ms = 0;
  return 0;
}

static int xiaozhi_handle_event(struct xiaozhi_client *client,
                                const struct xiaozhi_event *event)
{
  int ret;

  if (event->type != XIAOZHI_EVENT_HELLO &&
      event->type != XIAOZHI_EVENT_UNKNOWN &&
      !xiaozhi_session_matches(client, event))
    {
      xiaozhi_report_error(client, XIAOZHI_ERROR_PROTOCOL, -EPROTO,
                           "session_id", false, 0);
      return -EPROTO;
    }

  switch (event->type)
    {
      case XIAOZHI_EVENT_HELLO:
        if (client->state != XIAOZHI_STATE_WAITING_HELLO ||
            strcmp(event->transport, "websocket") != 0)
          {
            return -EPROTO;
          }

        ret = xiaozhi_copy(client->session_id,
                           sizeof(client->session_id), event->session_id);
        if (ret < 0)
          {
            return ret;
          }

        client->server_sample_rate = event->sample_rate != 0 ?
                                     event->sample_rate : 24000;
        client->server_channels = event->channels != 0 ?
                                  event->channels : 1;
        client->server_frame_duration_ms = event->frame_duration_ms != 0 ?
                                           event->frame_duration_ms :
                                           client->config.frame_duration_ms;

        if (client->audio.configure_downlink != NULL)
          {
            ret = client->audio.configure_downlink(
                client->audio_context, client->server_sample_rate,
                client->server_channels,
                client->server_frame_duration_ms);
            if (ret < 0)
              {
                xiaozhi_report_error(client, XIAOZHI_ERROR_AUDIO, ret,
                                     "configure_downlink", false, 0);
              }
          }

        client->reconnect_attempt = 0;
        client->state_deadline_ms = 0;
        xiaozhi_set_state(client, XIAOZHI_STATE_READY);
        break;

      case XIAOZHI_EVENT_TTS_START:
        if (client->state != XIAOZHI_STATE_THINKING &&
            (client->state != XIAOZHI_STATE_LISTENING ||
             client->listening_mode != XIAOZHI_LISTEN_AUTO))
          {
            return 0;
          }

        xiaozhi_prepare_tts_audio(
            client, client->state == XIAOZHI_STATE_LISTENING &&
                    client->listening_mode == XIAOZHI_LISTEN_AUTO);

        client->state_deadline_ms = 0;
        xiaozhi_set_state(client, XIAOZHI_STATE_SPEAKING);
        break;

      case XIAOZHI_EVENT_TTS_STOP:
        if (client->state != XIAOZHI_STATE_SPEAKING)
          {
            return 0;
          }

        if (client->audio.finish_downlink != NULL)
          {
            ret = client->audio.finish_downlink(client->audio_context);
            if (ret < 0)
              {
                xiaozhi_report_error(client, XIAOZHI_ERROR_AUDIO, ret,
                                     "finish_downlink", false, 0);
              }
          }

        if (client->audio.playback_idle == NULL ||
            client->audio.playback_idle(client->audio_context))
          {
            xiaozhi_complete_playback(client);
          }
        else
          {
            client->state_deadline_ms = xiaozhi_now(client) +
                client->config.playback_drain_timeout_ms;
            xiaozhi_set_state(client, XIAOZHI_STATE_DRAINING);
          }
        break;

      case XIAOZHI_EVENT_TTS_SENTENCE:
        if (client->state != XIAOZHI_STATE_SPEAKING &&
            client->state != XIAOZHI_STATE_DRAINING)
          {
            return 0;
          }

        break;

      default:
        break;
    }

  xiaozhi_emit_event(client, event);
  return 0;
}

static int xiaozhi_connect_if_due(struct xiaozhi_client *client)
{
  uint64_t now = xiaozhi_now(client);
  int ret;

  if (client->state != XIAOZHI_STATE_BACKOFF ||
      now < client->reconnect_at_ms)
    {
      return 0;
    }

  xiaozhi_set_state(client, XIAOZHI_STATE_CONNECTING);
  ret = client->transport.connect(client->transport_context);
  if (ret < 0)
    {
      return xiaozhi_schedule_reconnect(client, XIAOZHI_ERROR_TRANSPORT,
                                        ret, "connect");
    }

  ret = xiaozhi_send_hello(client);
  if (ret < 0)
    {
      return xiaozhi_schedule_reconnect(client, XIAOZHI_ERROR_TRANSPORT,
                                        ret, "send_hello");
    }

  now = xiaozhi_now(client);
  client->last_receive_ms = now;
  client->state_deadline_ms = now + client->config.hello_timeout_ms;
  xiaozhi_set_state(client, XIAOZHI_STATE_WAITING_HELLO);
  return 0;
}

static int xiaozhi_drain_uplink(struct xiaozhi_client *client)
{
  uint8_t packet[XIAOZHI_OPUS_PACKET_MAX];
  unsigned int index;
  size_t length;
  int ret;

  if (client->state != XIAOZHI_STATE_LISTENING ||
      client->audio.read_uplink_opus == NULL)
    {
      return 0;
    }

  for (index = 0; index < client->config.max_uplink_packets_per_poll;
       index++)
    {
      length = 0;
      ret = client->audio.read_uplink_opus(client->audio_context,
                                           packet, sizeof(packet), &length);
      if (ret == -EAGAIN)
        {
          return 0;
        }

      if (ret < 0)
        {
          if (!client->audio_error_reported)
            {
              client->audio_error_reported = true;
              xiaozhi_report_error(client, XIAOZHI_ERROR_AUDIO, ret,
                                   "capture", false, 0);
              return ret;
            }

          return 0;
        }

      client->audio_error_reported = false;
      ret = client->transport.send_binary(client->transport_context,
                                          packet, length);
      if (ret < 0)
        {
          return xiaozhi_schedule_reconnect(client,
                                            XIAOZHI_ERROR_TRANSPORT,
                                            ret, "send_audio");
        }
    }

  return 0;
}

static int xiaozhi_check_playback_error(struct xiaozhi_client *client)
{
  int error;

  if (client->audio.get_error == NULL)
    {
      return 0;
    }

  error = client->audio.get_error(client->audio_context, false);
  if (error == 0)
    {
      client->playback_error_reported = false;
      return 0;
    }

  if (client->playback_error_reported)
    {
      return 0;
    }

  client->playback_error_reported = true;
  xiaozhi_report_error(client, XIAOZHI_ERROR_AUDIO, error,
                       "playback", false, 0);
  return error;
}

static uint32_t xiaozhi_effective_timeout(struct xiaozhi_client *client,
                                          uint32_t requested)
{
  uint64_t now = xiaozhi_now(client);
  uint64_t remaining;

  if (client->state_deadline_ms == 0 ||
      client->state_deadline_ms <= now)
    {
      return client->state_deadline_ms == 0 ? requested : 0;
    }

  remaining = client->state_deadline_ms - now;
  if (remaining < requested)
    {
      return (uint32_t)remaining;
    }

  return requested;
}

static int xiaozhi_check_deadlines(struct xiaozhi_client *client)
{
  uint64_t now = xiaozhi_now(client);

  if (client->state == XIAOZHI_STATE_WAITING_HELLO &&
      now >= client->state_deadline_ms)
    {
      return xiaozhi_schedule_reconnect(client,
                                        XIAOZHI_ERROR_HELLO_TIMEOUT,
                                        -ETIMEDOUT, "server_hello");
    }

  if (client->state == XIAOZHI_STATE_THINKING &&
      client->state_deadline_ms != 0 && now >= client->state_deadline_ms)
    {
      return xiaozhi_schedule_reconnect(client,
                                        XIAOZHI_ERROR_REPLY_TIMEOUT,
                                        -ETIMEDOUT, "reply");
    }

  if (client->state == XIAOZHI_STATE_DRAINING)
    {
      if (client->audio.playback_idle == NULL ||
          client->audio.playback_idle(client->audio_context))
        {
          return xiaozhi_complete_playback(client);
        }

      if (now >= client->state_deadline_ms)
        {
          if (client->audio.flush != NULL)
            {
              client->audio.flush(client->audio_context);
            }

          xiaozhi_report_error(client, XIAOZHI_ERROR_AUDIO,
                               -ETIMEDOUT, "playback_drain", false, 0);
          return xiaozhi_complete_playback(client);
        }
    }

  if (client->state >= XIAOZHI_STATE_READY &&
      client->config.idle_timeout_ms != 0 &&
      now - client->last_receive_ms >= client->config.idle_timeout_ms)
    {
      return xiaozhi_schedule_reconnect(client,
                                        XIAOZHI_ERROR_IDLE_TIMEOUT,
                                        -ETIMEDOUT, "idle");
    }

  return 0;
}

struct xiaozhi_client *
xiaozhi_client_create(const struct xiaozhi_client_config *config,
                      const struct cloud_transport_ops *transport_ops,
                      void *transport_context,
                      const struct xiaozhi_audio_ops *audio_ops,
                      void *audio_context,
                      const struct xiaozhi_callbacks *callbacks,
                      void *callback_user)
{
  struct xiaozhi_client *client;

  if (config == NULL || transport_ops == NULL ||
      transport_ops->connect == NULL ||
      transport_ops->send_text == NULL ||
      transport_ops->send_binary == NULL ||
      transport_ops->receive == NULL ||
      transport_ops->close == NULL ||
      transport_ops->now_ms == NULL)
    {
      return NULL;
    }

  client = calloc(1, sizeof(*client));
  if (client == NULL)
    {
      return NULL;
    }

  client->config = *config;
  xiaozhi_apply_defaults(&client->config);
  if (!xiaozhi_config_valid(&client->config))
    {
      free(client);
      return NULL;
    }

  client->transport = *transport_ops;
  client->transport_context = transport_context;
  client->audio_context = audio_context;
  client->callback_user = callback_user;
  client->state = XIAOZHI_STATE_STOPPED;
  client->server_sample_rate = 24000;
  client->server_channels = 1;
  client->server_frame_duration_ms = client->config.frame_duration_ms;

  if (audio_ops != NULL)
    {
      client->audio = *audio_ops;
    }

  if (callbacks != NULL)
    {
      client->callbacks = *callbacks;
    }

  client->receive_buffer = malloc(client->config.receive_buffer_size);
  if (client->receive_buffer == NULL)
    {
      free(client);
      return NULL;
    }

  return client;
}

void xiaozhi_client_destroy(struct xiaozhi_client *client)
{
  if (client == NULL)
    {
      return;
    }

  xiaozhi_client_stop(client);
  free(client->receive_buffer);
  memset(client, 0, sizeof(*client));
  free(client);
}

int xiaozhi_client_start(struct xiaozhi_client *client)
{
  if (client == NULL)
    {
      return -EINVAL;
    }

  if (client->started)
    {
      return 0;
    }

  client->started = true;
  client->reconnect_attempt = 0;
  client->reconnect_at_ms = xiaozhi_now(client);
  xiaozhi_set_state(client, XIAOZHI_STATE_BACKOFF);
  return 0;
}

void xiaozhi_client_stop(struct xiaozhi_client *client)
{
  if (client == NULL)
    {
      return;
    }

  client->started = false;
  xiaozhi_stop_audio(client);
  client->transport.close(client->transport_context);
  client->session_id[0] = '\0';
  client->state_deadline_ms = 0;
  xiaozhi_set_state(client, XIAOZHI_STATE_STOPPED);
}

int xiaozhi_client_reconnect(struct xiaozhi_client *client)
{
  if (client == NULL || !client->started)
    {
      return -EINVAL;
    }

  xiaozhi_stop_audio(client);
  client->transport.close(client->transport_context);
  client->session_id[0] = '\0';
  client->reconnect_attempt = 0;
  client->reconnect_at_ms = xiaozhi_now(client);
  xiaozhi_set_state(client, XIAOZHI_STATE_BACKOFF);
  return 0;
}

int xiaozhi_client_poll(struct xiaozhi_client *client,
                        uint32_t timeout_ms)
{
  enum cloud_frame_type frame_type;
  struct xiaozhi_event event;
  size_t length = 0;
  uint32_t effective_timeout;
  int ret;

  if (client == NULL || !client->started)
    {
      return -EINVAL;
    }

  ret = xiaozhi_connect_if_due(client);
  if (ret < 0 || client->state == XIAOZHI_STATE_BACKOFF ||
      client->state == XIAOZHI_STATE_CONNECTING)
    {
      return ret;
    }

  ret = xiaozhi_drain_uplink(client);
  if (ret < 0)
    {
      return ret;
    }

  ret = xiaozhi_check_playback_error(client);
  if (ret < 0)
    {
      return ret;
    }

  effective_timeout = xiaozhi_effective_timeout(client, timeout_ms);
  ret = client->transport.receive(client->transport_context,
                                  client->receive_buffer,
                                  client->config.receive_buffer_size,
                                  &length, &frame_type,
                                  effective_timeout);
  if (ret == -EAGAIN)
    {
      return xiaozhi_check_deadlines(client);
    }

  if (ret < 0)
    {
      return xiaozhi_schedule_reconnect(client, XIAOZHI_ERROR_TRANSPORT,
                                        ret, "receive");
    }

  client->last_receive_ms = xiaozhi_now(client);
  if (frame_type == CLOUD_FRAME_CLOSE)
    {
      return xiaozhi_schedule_reconnect(client, XIAOZHI_ERROR_TRANSPORT,
                                        -ECONNRESET, "peer_close");
    }

  if (frame_type == CLOUD_FRAME_BINARY)
    {
      if (client->state == XIAOZHI_STATE_THINKING)
        {
          /* The official server may send the first packet before tts/start. */

          if (length == 0 || client->audio.push_downlink_opus == NULL)
            {
              return xiaozhi_check_deadlines(client);
            }

          xiaozhi_prepare_tts_audio(client, false);
          ret = client->audio.push_downlink_opus(client->audio_context,
                                                 client->receive_buffer,
                                                 length);
          if (ret < 0)
            {
              xiaozhi_report_error(client, XIAOZHI_ERROR_AUDIO, ret,
                                   "playback_queue", false, 0);
              return xiaozhi_check_deadlines(client);
            }

          client->state_deadline_ms = 0;
          xiaozhi_set_state(client, XIAOZHI_STATE_SPEAKING);
          if (client->callbacks.on_audio_packet != NULL)
            {
              client->callbacks.on_audio_packet(
                  client->callback_user, client->receive_buffer, length,
                  client->server_sample_rate,
                  client->server_frame_duration_ms);
            }

          return xiaozhi_check_deadlines(client);
        }

      if ((client->state == XIAOZHI_STATE_SPEAKING ||
           client->state == XIAOZHI_STATE_DRAINING) &&
          client->audio.push_downlink_opus != NULL)
        {
          ret = client->audio.push_downlink_opus(client->audio_context,
                                                 client->receive_buffer,
                                                 length);
          if (ret < 0)
            {
              xiaozhi_report_error(client, XIAOZHI_ERROR_AUDIO, ret,
                                   "playback_queue", false, 0);
            }
        }

      if ((client->state == XIAOZHI_STATE_SPEAKING ||
           client->state == XIAOZHI_STATE_DRAINING) &&
          client->callbacks.on_audio_packet != NULL)
        {
          client->callbacks.on_audio_packet(
              client->callback_user, client->receive_buffer, length,
              client->server_sample_rate,
              client->server_frame_duration_ms);
        }

      return xiaozhi_check_deadlines(client);
    }

  ret = xiaozhi_parse_server_message((const char *)client->receive_buffer,
                                     length, &event);
  if (ret < 0)
    {
      xiaozhi_report_error(client,
                           ret == -EMSGSIZE ?
                           XIAOZHI_ERROR_MESSAGE_TOO_LARGE :
                           XIAOZHI_ERROR_PROTOCOL,
                           ret, "parse_json", false, 0);
      return ret;
    }

  ret = xiaozhi_handle_event(client, &event);
  if (ret < 0)
    {
      xiaozhi_report_error(client, XIAOZHI_ERROR_PROTOCOL,
                           ret, "handle_json", false, 0);
    }

  if (client->callbacks.on_json_message != NULL)
    {
      client->callbacks.on_json_message(client->callback_user,
                                        (const char *)client->receive_buffer,
                                        length, &event);
    }

  return ret;
}

int xiaozhi_client_send_text_query(struct xiaozhi_client *client,
                                   const char *text)
{
  size_t length;
  int ret;

  if (client == NULL || text == NULL ||
      client->state != XIAOZHI_STATE_READY)
    {
      return -EBUSY;
    }

  length = xiaozhi_strnlen(text, XIAOZHI_EVENT_TEXT_MAX);
  if (length == 0)
    {
      return -EINVAL;
    }

  if (length >= XIAOZHI_EVENT_TEXT_MAX)
    {
      return -EMSGSIZE;
    }

  ret = xiaozhi_send_listen(client, "detect", NULL, text);
  if (ret < 0)
    {
      return xiaozhi_schedule_reconnect(client,
                                        XIAOZHI_ERROR_TRANSPORT,
                                        ret, "send_text_query");
    }

  client->state_deadline_ms = xiaozhi_now(client) +
                              client->config.reply_timeout_ms;
  xiaozhi_set_state(client, XIAOZHI_STATE_THINKING);
  return 0;
}

int xiaozhi_client_start_listening(struct xiaozhi_client *client,
                                   enum xiaozhi_listening_mode mode)
{
  const char *mode_text;
  int ret;

  if (client == NULL || client->state != XIAOZHI_STATE_READY)
    {
      return -EBUSY;
    }

  if (mode == XIAOZHI_LISTEN_REALTIME)
    {
      return -ENOTSUP;
    }
  else if (mode == XIAOZHI_LISTEN_AUTO)
    {
      mode_text = "auto";
    }
  else if (mode == XIAOZHI_LISTEN_MANUAL)
    {
      mode_text = "manual";
    }
  else
    {
      return -EINVAL;
    }

  if (client->audio.start_capture == NULL ||
      client->audio.read_uplink_opus == NULL)
    {
      return -ENOSYS;
    }

  client->audio_error_reported = false;
  ret = client->audio.start_capture(client->audio_context);
  if (ret < 0)
    {
      xiaozhi_report_error(client, XIAOZHI_ERROR_AUDIO, ret,
                           "start_capture", false, 0);
      return ret;
    }

  ret = xiaozhi_send_listen(client, "start", mode_text, NULL);
  if (ret < 0)
    {
      client->audio.stop_capture(client->audio_context);
      return xiaozhi_schedule_reconnect(client,
                                        XIAOZHI_ERROR_TRANSPORT,
                                        ret, "start_listening");
    }

  client->listening_mode = mode;
  client->state_deadline_ms = 0;
  xiaozhi_set_state(client, XIAOZHI_STATE_LISTENING);
  return 0;
}

int xiaozhi_client_stop_listening(struct xiaozhi_client *client)
{
  int ret;

  if (client == NULL || client->state != XIAOZHI_STATE_LISTENING)
    {
      return -EBUSY;
    }

  if (client->audio.stop_capture != NULL)
    {
      client->audio.stop_capture(client->audio_context);
    }

  ret = xiaozhi_send_listen(client, "stop", NULL, NULL);
  if (ret < 0)
    {
      return xiaozhi_schedule_reconnect(client,
                                        XIAOZHI_ERROR_TRANSPORT,
                                        ret, "stop_listening");
    }

  client->state_deadline_ms = xiaozhi_now(client) +
                              client->config.reply_timeout_ms;
  xiaozhi_set_state(client, XIAOZHI_STATE_THINKING);
  return 0;
}

int xiaozhi_client_abort(struct xiaozhi_client *client,
                         const char *reason)
{
  cJSON *root;
  int ret;

  if (client == NULL || client->state < XIAOZHI_STATE_READY)
    {
      return -EINVAL;
    }

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "session_id", client->session_id);
  cJSON_AddStringToObject(root, "type", "abort");
  if (reason != NULL && reason[0] != '\0')
    {
      cJSON_AddStringToObject(root, "reason", reason);
    }

  ret = xiaozhi_send_json(client, root);
  cJSON_Delete(root);
  if (ret < 0)
    {
      return xiaozhi_schedule_reconnect(client,
                                        XIAOZHI_ERROR_TRANSPORT,
                                        ret, "abort");
    }

  xiaozhi_stop_audio(client);
  client->state_deadline_ms = 0;
  xiaozhi_set_state(client, XIAOZHI_STATE_READY);
  return 0;
}

int xiaozhi_client_send_opus(struct xiaozhi_client *client,
                             const void *opus, size_t length)
{
  int ret;

  if (client == NULL || opus == NULL || length == 0 ||
      length > XIAOZHI_OPUS_PACKET_MAX ||
      client->state != XIAOZHI_STATE_LISTENING)
    {
      return -EINVAL;
    }

  ret = client->transport.send_binary(client->transport_context,
                                      opus, length);
  if (ret < 0)
    {
      return xiaozhi_schedule_reconnect(client,
                                        XIAOZHI_ERROR_TRANSPORT,
                                        ret, "send_opus");
    }

  return 0;
}

enum xiaozhi_state
xiaozhi_client_state(const struct xiaozhi_client *client)
{
  return client == NULL ? XIAOZHI_STATE_STOPPED : client->state;
}

const char *xiaozhi_client_session_id(const struct xiaozhi_client *client)
{
  return client == NULL ? "" : client->session_id;
}

const char *xiaozhi_state_name(enum xiaozhi_state state)
{
  switch (state)
    {
      case XIAOZHI_STATE_STOPPED:
        return "stopped";
      case XIAOZHI_STATE_BACKOFF:
        return "backoff";
      case XIAOZHI_STATE_CONNECTING:
        return "connecting";
      case XIAOZHI_STATE_WAITING_HELLO:
        return "waiting_hello";
      case XIAOZHI_STATE_READY:
        return "ready";
      case XIAOZHI_STATE_LISTENING:
        return "listening";
      case XIAOZHI_STATE_THINKING:
        return "thinking";
      case XIAOZHI_STATE_SPEAKING:
        return "speaking";
      case XIAOZHI_STATE_DRAINING:
        return "draining";
      default:
        return "unknown";
    }
}
