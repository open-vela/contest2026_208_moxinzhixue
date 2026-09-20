# OpenVela Xiaozhi cloud client

This directory provides a UI-independent Xiaozhi dialogue client for
Gemini-S1/OpenVela.  The default path is a single WebSocket connection carrying
JSON control/text frames and binary Opus audio frames.

Implemented end-to-end flow:

- OTA configuration discovery for WebSocket endpoint, token, activation,
  firmware metadata and server time.
- TCP/TLS connection and certificate verification through libcurl.
- RFC 6455 upgrade, client masking, fragmented messages, ping/pong and close.
- Xiaozhi hello/session state machine, reconnect backoff and timeouts.
- Text questions and STT/LLM/TTS text callbacks.
- 16 kHz mono PCM capture, 60 ms Opus encoding and binary upload.
- Negotiated Opus downlink decoding, PCM playback and playback drain.
- Manual/auto listening, abort, bounded audio queues and error/statistics APIs.

The production default follows the official firmware and uses
`https://api.tenclass.net/xiaozhi/ota/` (Kconfig-overridable). Device-Id is the
lower-case Wi-Fi MAC, Client-Id is a persistent UUID v4, and returned WSS/token
credentials are stored in a mode-0600 file. TLS peer and hostname verification
remain enabled by default. When no CA file is configured, the client uses an
embedded DigiCert Global Root G2 trust anchor. If the board starts before 2024,
the client obtains the official endpoint's HTTP `Date` header to initialize
`CLOCK_REALTIME`, then performs the normal verified HTTPS/WSS connection.

## Files

- `cloud_client`: curl-assisted TCP/TLS plus the WebSocket framing layer.
- `xiaozhi_client`: protocol JSON, session state and transport/audio orchestration.
- `xiaozhi_audio`: ALSA-compatible PCM capture/playback and libopus workers.
- `xiaozhi_ota`: OTA/configuration discovery; it does not flash firmware.
- `xiaozhi_provisioning`: persistent identity, pairing state machine and the
  UI-safe status/code API. Tokens are available only through the separate
  credentials getter.
- `tests`: host-side parser and state-machine tests with mock transport/audio.

## OpenVela configuration

Enable the existing application and its optional cloud-client sources together
with these dependencies:

```text
CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST=y
CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT=y
CONFIG_LIB_CURL=y
CONFIG_CRYPTO_MBEDTLS=y
CONFIG_NETUTILS_CJSON=y
CONFIG_LIB_OPUS=y
CONFIG_AUDIOUTILS_ALSA_LIB=y
```

The CMake and Makefile integration only adds the module sources and include
directory.  It deliberately does not call the client from `hello_app_main.c`.
The UI/application owner can integrate it on a worker task without coupling
network or audio blocking work to the LVGL thread.

## Runtime construction

The following is a lifecycle outline.  All values in `runtime` are supplied by
the product's provisioning/configuration layer; they are not literals in the
firmware.

```c
struct runtime_xiaozhi_settings
{
  const char *websocket_url;
  const char *bearer_token;
  const char *device_id;
  const char *client_id;
  const char *ca_file;
  const char *capture_pcm;
  const char *playback_pcm;
};

static void run_xiaozhi(const struct runtime_xiaozhi_settings *runtime)
{
  struct cloud_client_config cloud_config = {0};
  struct xiaozhi_audio_config audio_config = {0};
  struct xiaozhi_client_config client_config = {0};
  struct cloud_client *cloud;
  struct xiaozhi_audio *audio;
  struct xiaozhi_client *client;

  cloud_config.url = runtime->websocket_url;
  cloud_config.bearer_token = runtime->bearer_token;
  cloud_config.device_id = runtime->device_id;
  cloud_config.client_id = runtime->client_id;
  cloud_config.ca_file = runtime->ca_file;
  cloud_config.protocol_version = 1;

  audio_config.capture_device = runtime->capture_pcm;
  audio_config.playback_device = runtime->playback_pcm;
  audio_config.capture_sample_rate = 16000;
  audio_config.capture_channels = 1;
  audio_config.frame_duration_ms = 60;

  client_config.protocol_version = 1;
  client_config.input_sample_rate = 16000;
  client_config.input_channels = 1;
  client_config.frame_duration_ms = 60;

  if (cloud_client_global_init() < 0)
    {
      return;
    }

  cloud = cloud_client_create(&cloud_config);
  audio = xiaozhi_audio_create(&audio_config);
  client = cloud != NULL && audio != NULL ?
      xiaozhi_client_create(&client_config,
                            cloud_client_transport_ops(), cloud,
                            xiaozhi_audio_ops(), audio,
                            NULL, NULL) : NULL;

  if (client != NULL && xiaozhi_client_start(client) == 0)
    {
      while (xiaozhi_client_state(client) != XIAOZHI_STATE_STOPPED)
        {
          (void)xiaozhi_client_poll(client, 50);
          /* Exit on an application-owned shutdown flag. */
        }
    }

  xiaozhi_client_destroy(client);
  xiaozhi_audio_destroy(audio);
  cloud_client_destroy(cloud);
  cloud_client_global_cleanup();
}
```

Install callbacks in `struct xiaozhi_callbacks` before creating the client:

- `on_event` receives parsed hello, STT, LLM, TTS, MCP, system and alert events.
- `on_json_message` receives the validated raw JSON for extension payloads.
- `on_audio_packet` observes the original server Opus packet if needed.
- `on_state` and `on_error` are suitable for posting small messages to the UI
  task.  Callbacks execute on the task calling `xiaozhi_client_poll()`.

## Dialogue operations

After the state reaches `XIAOZHI_STATE_READY`:

```c
xiaozhi_client_send_text_query(client, text);
xiaozhi_client_start_listening(client, XIAOZHI_LISTEN_AUTO);
xiaozhi_client_start_listening(client, XIAOZHI_LISTEN_MANUAL);
xiaozhi_client_stop_listening(client);
xiaozhi_client_abort(client, "user_cancelled");
```

`AUTO` and `MANUAL` provide the complete half-duplex capture/upload/reply/playback
path used by the target server.  The built-in ALSA backend returns `-ENOTSUP`
for `XIAOZHI_LISTEN_REALTIME`: realtime barge-in needs acoustic echo cancellation
and a protocol/server combination that preserves render/capture timing.  A
product-specific AEC backend should be integrated before enabling that mode.

The audio worker uses bounded queues.  When overloaded it drops the oldest
packet instead of growing memory without limit.  Read counters and asynchronous
capture/playback errors with `xiaozhi_audio_get_stats()` and
`xiaozhi_audio_last_error()`.

## OTA discovery

Call `xiaozhi_ota_fetch()` before constructing the WebSocket client when the
deployment uses the upstream provisioning flow.  Copy the returned URL/token
into application-owned secure storage or directly into `cloud_client_config`.
An OTA response may contain only MQTT settings; this module reports that with
`has_mqtt` but does not implement the separate MQTT gateway data plane.

Firmware download, signature/rollback policy, partition selection and reboot
remain the responsibility of OpenVela's board-specific OTA subsystem.

## Official pairing

`xiaozhi_provisioning_start()` obtains the board MAC, loads or generates the
persistent Client-Id and optionally reuses cached credentials. A worker calls
`xiaozhi_provisioning_poll()`; state callbacks and
`xiaozhi_provisioning_get_snapshot()` expose only state, identity, pairing code
and user message to the UI. After `/activate` returns HTTP 200, provisioning
checks OTA again and stores `websocket.url` plus `websocket.token`.

For board testing, `xiaozhi_pair` prints the pairing code without accepting a
token on the command line:

```text
xiaozhi_pair --timeout 300
xiaozhi_pair --status
```

`xiaozhi_provisioning_get_credentials()` is intended for the dialogue worker;
the token is never included in pairing callbacks or CLI output.

See `PROTOCOL_COMPATIBILITY.md` for the reviewed revisions, license notes and
known compatibility risks.
