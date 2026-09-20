/****************************************************************************
 * Contest 2026 team 208 - Xiaozhi cloud dialogue smoke-test CLI
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cloud_client.h"
#include "xiaozhi_audio.h"
#include "xiaozhi_client.h"
#include "xiaozhi_ota.h"

#define XIAOZHI_CLI_DEFAULT_TIMEOUT_SECONDS 120
#define XIAOZHI_CLI_MAX_TIMEOUT_SECONDS     3600
#define XIAOZHI_CLI_POLL_MS                 50
#define XIAOZHI_CLI_CONNECT_TIMEOUT_MS      15000
#define XIAOZHI_CLI_HELLO_TIMEOUT_MS        10000
#define XIAOZHI_CLI_DRAIN_TIMEOUT_MS        5000
#define XIAOZHI_CLI_USER_AGENT \
  "openvela-gemini-s1-xiaozhi-cli/1.0"

struct xiaozhi_cli_options
{
  FAR const char *url;
  FAR const char *token;
  FAR const char *device_id;
  FAR const char *client_id;
  FAR const char *ca_file;
  FAR const char *capture_device;
  FAR const char *playback_device;
  FAR const char *text;
  uint32_t timeout_ms;
  bool insecure;
  bool allow_plaintext;
};

struct xiaozhi_cli_context
{
  bool reply_started;
  bool reply_finished;
  unsigned int errors;
};

static uint64_t xiaozhi_cli_now_ms(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static uint32_t xiaozhi_cli_min_timeout(uint32_t requested,
                                        uint32_t maximum)
{
  return requested < maximum ? requested : maximum;
}

static bool xiaozhi_cli_has_scheme(FAR const char *url,
                                    FAR const char *scheme)
{
  return strncmp(url, scheme, strlen(scheme)) == 0;
}

static bool xiaozhi_cli_is_websocket_url(FAR const char *url)
{
  return xiaozhi_cli_has_scheme(url, "ws://") ||
         xiaozhi_cli_has_scheme(url, "wss://");
}

static bool xiaozhi_cli_is_ota_url(FAR const char *url)
{
  return xiaozhi_cli_has_scheme(url, "http://") ||
         xiaozhi_cli_has_scheme(url, "https://");
}

static bool xiaozhi_cli_is_plaintext_url(FAR const char *url)
{
  return xiaozhi_cli_has_scheme(url, "ws://") ||
         xiaozhi_cli_has_scheme(url, "http://");
}

static void xiaozhi_cli_usage(FAR const char *program)
{
  printf("Usage:\n");
  printf("  %s --url <ws(s)://...|http(s)://...> \\\n", program);
  printf("    --device-id <id> --client-id <id> [options]\n");
  printf("Options:\n");
  printf("  --token <token>       WebSocket bearer token (optional)\n");
  printf("  --ca <path>           CA certificate bundle\n");
  printf("  --capture <pcm>       ALSA capture device (default: default)\n");
  printf("  --playback <pcm>      ALSA playback device (default: default)\n");
  printf("  --text <question>     Send text; omit for one voice turn\n");
  printf("  --insecure            Disable TLS peer verification\n");
  printf("  --allow-plaintext     Permit ws/http for isolated testing only\n");
  printf("  --timeout <seconds>   Overall dialogue timeout (default: %u)\n",
         XIAOZHI_CLI_DEFAULT_TIMEOUT_SECONDS);
  printf("  --help                Show this help\n");
  printf("\nhttp(s) URLs use Xiaozhi OTA discovery; "
         "ws(s) URLs connect directly.\n");
}

static int xiaozhi_cli_option_value(int argc, FAR char *argv[],
                                    FAR int *index, FAR const char *name,
                                    FAR const char **value)
{
  FAR const char *argument = argv[*index];
  size_t name_length = strlen(name);

  if (strcmp(argument, name) == 0)
    {
      if (*index + 1 >= argc || argv[*index + 1][0] == '\0')
        {
          return -EINVAL;
        }

      *value = argv[++*index];
      return 1;
    }

  if (strncmp(argument, name, name_length) == 0 &&
      argument[name_length] == '=' && argument[name_length + 1] != '\0')
    {
      *value = argument + name_length + 1;
      return 1;
    }

  return 0;
}

static int xiaozhi_cli_parse_timeout(FAR const char *text,
                                     FAR uint32_t *timeout_ms)
{
  FAR char *end;
  unsigned long seconds;

  errno = 0;
  seconds = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || seconds == 0 ||
      seconds > XIAOZHI_CLI_MAX_TIMEOUT_SECONDS)
    {
      return -EINVAL;
    }

  *timeout_ms = (uint32_t)seconds * 1000;
  return 0;
}

static int xiaozhi_cli_parse_options(int argc, FAR char *argv[],
                                     FAR struct xiaozhi_cli_options *options)
{
  FAR const char *value;
  int matched;
  int index;

  memset(options, 0, sizeof(*options));
  options->timeout_ms = XIAOZHI_CLI_DEFAULT_TIMEOUT_SECONDS * 1000;

  for (index = 1; index < argc; index++)
    {
      if (strcmp(argv[index], "--help") == 0 ||
          strcmp(argv[index], "-h") == 0)
        {
          return 1;
        }

      if (strcmp(argv[index], "--insecure") == 0)
        {
          options->insecure = true;
          continue;
        }

      if (strcmp(argv[index], "--allow-plaintext") == 0)
        {
          options->allow_plaintext = true;
          continue;
        }

#define XIAOZHI_CLI_PARSE_OPTION(option_name, field_name)                 \
      do                                                                  \
        {                                                                 \
          matched = xiaozhi_cli_option_value(argc, argv, &index,          \
                                              option_name, &value);        \
          if (matched < 0)                                                \
            {                                                             \
              fprintf(stderr, "xiaozhi_chat: %s needs a value\n",        \
                      option_name);                                       \
              return matched;                                             \
            }                                                             \
          else if (matched > 0)                                           \
            {                                                             \
              options->field_name = value;                                \
              goto option_parsed;                                         \
            }                                                             \
        }                                                                 \
      while (0)

      XIAOZHI_CLI_PARSE_OPTION("--url", url);
      XIAOZHI_CLI_PARSE_OPTION("--token", token);
      XIAOZHI_CLI_PARSE_OPTION("--device-id", device_id);
      XIAOZHI_CLI_PARSE_OPTION("--client-id", client_id);
      XIAOZHI_CLI_PARSE_OPTION("--ca", ca_file);
      XIAOZHI_CLI_PARSE_OPTION("--capture", capture_device);
      XIAOZHI_CLI_PARSE_OPTION("--playback", playback_device);
      XIAOZHI_CLI_PARSE_OPTION("--text", text);

#undef XIAOZHI_CLI_PARSE_OPTION

      matched = xiaozhi_cli_option_value(argc, argv, &index,
                                          "--timeout", &value);
      if (matched < 0)
        {
          fprintf(stderr, "xiaozhi_chat: --timeout needs a value\n");
          return matched;
        }
      else if (matched > 0)
        {
          if (xiaozhi_cli_parse_timeout(value, &options->timeout_ms) < 0)
            {
              fprintf(stderr,
                      "xiaozhi_chat: timeout must be 1..%u seconds\n",
                      XIAOZHI_CLI_MAX_TIMEOUT_SECONDS);
              return -EINVAL;
            }

          continue;
        }

      fprintf(stderr, "xiaozhi_chat: unknown option: %s\n", argv[index]);
      return -EINVAL;

option_parsed:
      continue;
    }

  if (options->url == NULL || options->device_id == NULL ||
      options->client_id == NULL)
    {
      fprintf(stderr,
              "xiaozhi_chat: --url, --device-id and --client-id "
              "are required\n");
      return -EINVAL;
    }

  if (!xiaozhi_cli_is_websocket_url(options->url) &&
      !xiaozhi_cli_is_ota_url(options->url))
    {
      fprintf(stderr,
              "xiaozhi_chat: URL must use ws://, wss://, http:// "
              "or https://\n");
      return -EINVAL;
    }

  if (!options->allow_plaintext &&
      xiaozhi_cli_is_plaintext_url(options->url))
    {
      fprintf(stderr,
              "xiaozhi_chat: plaintext ws/http is disabled; use TLS or "
              "--allow-plaintext for an isolated test\n");
      return -EPERM;
    }

  return 0;
}

static FAR const char *xiaozhi_cli_event_name(enum xiaozhi_event_type type)
{
  switch (type)
    {
      case XIAOZHI_EVENT_HELLO:
        return "hello";
      case XIAOZHI_EVENT_STT:
        return "stt";
      case XIAOZHI_EVENT_LLM:
        return "llm";
      case XIAOZHI_EVENT_TTS_START:
        return "tts_start";
      case XIAOZHI_EVENT_TTS_SENTENCE:
        return "tts_sentence";
      case XIAOZHI_EVENT_TTS_STOP:
        return "tts_stop";
      case XIAOZHI_EVENT_MCP:
        return "mcp";
      case XIAOZHI_EVENT_SYSTEM:
        return "system";
      case XIAOZHI_EVENT_ALERT:
        return "alert";
      case XIAOZHI_EVENT_PONG:
        return "pong";
      default:
        return "unknown";
    }
}

static void xiaozhi_cli_on_state(FAR void *user,
                                 enum xiaozhi_state old_state,
                                 enum xiaozhi_state new_state)
{
  (void)user;
  printf("xiaozhi_chat: state %s -> %s\n",
         xiaozhi_state_name(old_state), xiaozhi_state_name(new_state));
}

static void xiaozhi_cli_on_event(FAR void *user,
                                 FAR const struct xiaozhi_event *event)
{
  FAR struct xiaozhi_cli_context *context = user;

  switch (event->type)
    {
      case XIAOZHI_EVENT_HELLO:
        printf("xiaozhi_chat: server hello, audio=%lu Hz/%u ch/%u ms\n",
               (unsigned long)event->sample_rate,
               (unsigned int)event->channels,
               (unsigned int)event->frame_duration_ms);
        break;

      case XIAOZHI_EVENT_STT:
        printf("xiaozhi_chat: STT: %s\n", event->text);
        break;

      case XIAOZHI_EVENT_LLM:
        context->reply_started = true;
        printf("xiaozhi_chat: LLM: %s", event->text);
        if (event->emotion[0] != '\0')
          {
            printf(" [emotion=%s]", event->emotion);
          }

        printf("\n");
        break;

      case XIAOZHI_EVENT_TTS_START:
        context->reply_started = true;
        printf("xiaozhi_chat: TTS started\n");
        break;

      case XIAOZHI_EVENT_TTS_SENTENCE:
        context->reply_started = true;
        printf("xiaozhi_chat: TTS: %s\n", event->text);
        break;

      case XIAOZHI_EVENT_TTS_STOP:
        context->reply_finished = true;
        printf("xiaozhi_chat: TTS finished\n");
        break;

      case XIAOZHI_EVENT_SYSTEM:
      case XIAOZHI_EVENT_ALERT:
        printf("xiaozhi_chat: %s status=%s text=%s\n",
               xiaozhi_cli_event_name(event->type), event->status,
               event->text);
        break;

      case XIAOZHI_EVENT_MCP:
        printf("xiaozhi_chat: MCP command=%s\n", event->command);
        break;

      default:
        printf("xiaozhi_chat: event %s\n",
               xiaozhi_cli_event_name(event->type));
        break;
    }
}

static void xiaozhi_cli_on_error(FAR void *user,
                                 FAR const struct xiaozhi_error *error)
{
  FAR struct xiaozhi_cli_context *context = user;

  context->errors++;
  fprintf(stderr,
          "xiaozhi_chat: error code=%d detail=%d phase=%s reconnect=%u",
          error->code, error->detail,
          error->phase == NULL ? "unknown" : error->phase,
          (unsigned int)error->reconnecting);
  if (error->reconnecting)
    {
      fprintf(stderr, " retry_ms=%lu", (unsigned long)error->retry_delay_ms);
    }

  fprintf(stderr, "\n");
}

static const struct xiaozhi_callbacks g_xiaozhi_cli_callbacks =
{
  .on_state = xiaozhi_cli_on_state,
  .on_event = xiaozhi_cli_on_event,
  .on_error = xiaozhi_cli_on_error,
};

static int xiaozhi_cli_discover(
    FAR const struct xiaozhi_cli_options *options,
    FAR struct xiaozhi_ota_result *result,
    FAR const char **websocket_url, FAR const char **token)
{
  struct xiaozhi_ota_config ota_config;
  char error_text[192];
  int ret;

  *websocket_url = options->url;
  *token = options->token;
  if (!xiaozhi_cli_is_ota_url(options->url))
    {
      printf("xiaozhi_chat: using direct WebSocket endpoint\n");
      return 0;
    }

  memset(&ota_config, 0, sizeof(ota_config));
  memset(result, 0, sizeof(*result));
  ota_config.url = options->url;
  ota_config.device_id = options->device_id;
  ota_config.client_id = options->client_id;
  ota_config.user_agent = XIAOZHI_CLI_USER_AGENT;
  ota_config.ca_file = options->ca_file;
  ota_config.timeout_ms = options->timeout_ms;
  ota_config.allow_insecure_tls = options->insecure;

  printf("xiaozhi_chat: discovering WebSocket endpoint through OTA\n");
  ret = xiaozhi_ota_fetch(&ota_config, result, error_text,
                          sizeof(error_text));
  if (ret < 0)
    {
      fprintf(stderr, "xiaozhi_chat: OTA discovery failed: %d (%s)\n",
              ret, error_text[0] == '\0' ? "no detail" : error_text);
      return ret;
    }

  if (result->activation_required)
    {
      printf("xiaozhi_chat: activation required");
      if (result->activation_code[0] != '\0')
        {
          printf(", code=%s", result->activation_code);
        }

      printf("\n");
      if (result->activation_message[0] != '\0')
        {
          printf("xiaozhi_chat: activation: %s\n",
                 result->activation_message);
        }
    }

  if (!result->has_websocket || result->websocket_url[0] == '\0')
    {
      fprintf(stderr,
              "xiaozhi_chat: OTA response has no WebSocket endpoint%s\n",
              result->has_mqtt ? " (MQTT-only is unsupported)" : "");
      return -ENOTSUP;
    }

  *websocket_url = result->websocket_url;
  if (options->token == NULL && result->websocket_token[0] != '\0')
    {
      *token = result->websocket_token;
    }

  printf("xiaozhi_chat: OTA WebSocket configuration ready\n");
  return 0;
}

int main(int argc, FAR char *argv[])
{
  struct xiaozhi_cli_options options;
  struct xiaozhi_cli_context context;
  struct xiaozhi_ota_result ota_result;
  struct cloud_client_config cloud_config;
  struct xiaozhi_audio_config audio_config;
  struct xiaozhi_client_config client_config;
  struct xiaozhi_audio_stats audio_stats;
  FAR const char *websocket_url = NULL;
  FAR const char *token = NULL;
  FAR struct cloud_client *cloud = NULL;
  FAR struct xiaozhi_audio *audio = NULL;
  FAR struct xiaozhi_client *client = NULL;
  enum xiaozhi_state state;
  uint64_t deadline_ms;
  bool operation_started = false;
  bool completed = false;
  bool global_initialized = false;
  int exit_status = EXIT_FAILURE;
  int ret;

  ret = xiaozhi_cli_parse_options(argc, argv, &options);
  if (ret > 0)
    {
      xiaozhi_cli_usage(argv[0]);
      return EXIT_SUCCESS;
    }
  else if (ret < 0)
    {
      xiaozhi_cli_usage(argv[0]);
      return EXIT_FAILURE;
    }

  memset(&context, 0, sizeof(context));
  ret = cloud_client_global_init();
  if (ret < 0)
    {
      fprintf(stderr, "xiaozhi_chat: curl global init failed: %d\n", ret);
      goto cleanup;
    }

  global_initialized = true;
  ret = xiaozhi_cli_discover(&options, &ota_result, &websocket_url,
                             &token);
  if (ret < 0)
    {
      goto cleanup;
    }

  memset(&cloud_config, 0, sizeof(cloud_config));
  cloud_config.url = websocket_url;
  cloud_config.bearer_token = token;
  cloud_config.device_id = options.device_id;
  cloud_config.client_id = options.client_id;
  cloud_config.user_agent = XIAOZHI_CLI_USER_AGENT;
  cloud_config.ca_file = options.ca_file;
  cloud_config.protocol_version = 1;
  cloud_config.connect_timeout_ms =
      xiaozhi_cli_min_timeout(options.timeout_ms,
                              XIAOZHI_CLI_CONNECT_TIMEOUT_MS);
  cloud_config.io_timeout_ms = cloud_config.connect_timeout_ms;
  cloud_config.allow_insecure_tls = options.insecure;

  memset(&audio_config, 0, sizeof(audio_config));
  audio_config.capture_device = options.capture_device;
  audio_config.playback_device = options.playback_device;
  audio_config.capture_sample_rate = 16000;
  audio_config.capture_channels = 1;
  audio_config.frame_duration_ms = 20;

  memset(&client_config, 0, sizeof(client_config));
  client_config.protocol_version = 1;
  client_config.input_sample_rate = 16000;
  client_config.input_channels = 1;
  client_config.frame_duration_ms = 20;
  client_config.hello_timeout_ms =
      xiaozhi_cli_min_timeout(options.timeout_ms,
                              XIAOZHI_CLI_HELLO_TIMEOUT_MS);
  client_config.reply_timeout_ms = options.timeout_ms;
  client_config.idle_timeout_ms = options.timeout_ms;
  client_config.playback_drain_timeout_ms =
      XIAOZHI_CLI_DRAIN_TIMEOUT_MS;

  cloud = cloud_client_create(&cloud_config);
  audio = xiaozhi_audio_create(&audio_config);
  if (cloud == NULL || audio == NULL)
    {
      fprintf(stderr,
              "xiaozhi_chat: failed to create cloud/audio runtime\n");
      goto cleanup;
    }

  client = xiaozhi_client_create(
      &client_config, cloud_client_transport_ops(), cloud,
      xiaozhi_audio_ops(), audio, &g_xiaozhi_cli_callbacks, &context);
  if (client == NULL)
    {
      fprintf(stderr, "xiaozhi_chat: failed to create protocol client\n");
      goto cleanup;
    }

  ret = xiaozhi_client_start(client);
  if (ret < 0)
    {
      fprintf(stderr, "xiaozhi_chat: client start failed: %d\n", ret);
      goto cleanup;
    }

  deadline_ms = xiaozhi_cli_now_ms() + options.timeout_ms;
  while (xiaozhi_cli_now_ms() < deadline_ms)
    {
      state = xiaozhi_client_state(client);
      if (!operation_started && state == XIAOZHI_STATE_READY)
        {
          if (options.text != NULL)
            {
              ret = xiaozhi_client_send_text_query(client, options.text);
            }
          else
            {
              ret = xiaozhi_client_start_listening(
                  client, XIAOZHI_LISTEN_AUTO);
            }

          if (ret < 0)
            {
              fprintf(stderr,
                      "xiaozhi_chat: unable to start dialogue: %d\n", ret);
              goto cleanup;
            }

          if (options.text != NULL)
            {
              printf("xiaozhi_chat: sending text query\n");
            }
          else
            {
              printf("xiaozhi_chat: starting one automatic voice turn\n");
            }

          operation_started = true;
        }

      ret = xiaozhi_client_poll(client, XIAOZHI_CLI_POLL_MS);
      if (ret < 0 && xiaozhi_client_state(client) == XIAOZHI_STATE_STOPPED)
        {
          fprintf(stderr, "xiaozhi_chat: client stopped unexpectedly: %d\n",
                  ret);
          goto cleanup;
        }

      state = xiaozhi_client_state(client);
      if (operation_started && state == XIAOZHI_STATE_BACKOFF)
        {
          fprintf(stderr,
                  "xiaozhi_chat: dialogue interrupted; refusing to report "
                  "success after reconnect\n");
          goto cleanup;
        }

      if (operation_started && context.reply_finished &&
          (state == XIAOZHI_STATE_READY ||
           state == XIAOZHI_STATE_LISTENING))
        {
          completed = true;
          break;
        }

      if (state == XIAOZHI_STATE_BACKOFF)
        {
          usleep(XIAOZHI_CLI_POLL_MS * 1000);
        }
    }

  if (!completed)
    {
      fprintf(stderr,
              "xiaozhi_chat: timed out before a complete reply (started=%u, "
              "reply=%u)\n",
              (unsigned int)operation_started,
              (unsigned int)context.reply_started);
      state = xiaozhi_client_state(client);
      if (state >= XIAOZHI_STATE_READY)
        {
          (void)xiaozhi_client_abort(client, "cli_timeout");
        }

      goto cleanup;
    }

  state = xiaozhi_client_state(client);
  if (options.text == NULL && state == XIAOZHI_STATE_LISTENING)
    {
      (void)xiaozhi_client_abort(client, "cli_complete");
    }

  memset(&audio_stats, 0, sizeof(audio_stats));
  xiaozhi_audio_get_stats(audio, &audio_stats);
  printf("xiaozhi_chat: complete, captured=%llu played=%llu "
         "uplink_drop=%lu downlink_drop=%lu errors=%u\n",
         (unsigned long long)audio_stats.captured_frames,
         (unsigned long long)audio_stats.played_frames,
         (unsigned long)audio_stats.uplink_dropped,
         (unsigned long)audio_stats.downlink_dropped, context.errors);
  if (audio_stats.capture_error != 0 || audio_stats.playback_error != 0)
    {
      fprintf(stderr,
              "xiaozhi_chat: audio error capture=%d playback=%d (%s)\n",
              audio_stats.capture_error, audio_stats.playback_error,
              xiaozhi_audio_last_error(audio));
      goto cleanup;
    }

  if (context.errors != 0)
    {
      fprintf(stderr,
              "xiaozhi_chat: completed with %u protocol/runtime errors\n",
              context.errors);
      goto cleanup;
    }

  exit_status = EXIT_SUCCESS;

cleanup:
  if (exit_status != EXIT_SUCCESS && cloud != NULL)
    {
      FAR const char *cloud_error = cloud_client_last_error(cloud);
      if (cloud_error != NULL && cloud_error[0] != '\0')
        {
          fprintf(stderr, "xiaozhi_chat: cloud detail: %s\n", cloud_error);
        }
    }

  xiaozhi_client_destroy(client);
  xiaozhi_audio_destroy(audio);
  cloud_client_destroy(cloud);
  if (global_initialized)
    {
      cloud_client_global_cleanup();
    }

  memset(&ota_result, 0, sizeof(ota_result));

  return exit_status;
}
