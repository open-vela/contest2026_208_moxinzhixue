# Xiaozhi protocol compatibility

## Reviewed upstreams

The implementation was checked against these revisions on 2026-07-26:

- `78/xiaozhi-esp32` at `38bc4f443b96da306a4f7c77375042e46d588e64`.
- `xinnan-tech/xiaozhi-esp32-server` at
  `f5ed1aaec88471ba00ac778045331514066d63dc`.
- OpenVela curl at `7d1e03473bb8c5d409a54df9bb522bc7bdb6d9d8`
  (curl 8.4.0-DEV).

Both Xiaozhi repositories are MIT licensed.  The linked components retain their
own licenses: cJSON is MIT, curl uses the curl license, mbedTLS is Apache-2.0,
Opus uses its BSD-style license, and OpenVela's ALSA-compatible layer is
Apache-2.0.  This module is an original implementation of the documented wire
protocol and does not copy upstream source files.

## Selected transport

| Path | Control | Audio | Extra requirements | Decision |
| --- | --- | --- | --- | --- |
| WebSocket | JSON text frames | Binary Opus frames | One TCP/TLS connection | Implemented |
| MQTT + UDP | MQTT JSON | AES-CTR UDP packets | Broker, UDP ports, key/nonce handling and `xiaozhi-mqtt-gateway` | Discovery only |
| OTA HTTP | JSON POST/response | None | curl/TLS and board identity | Implemented for discovery |

WebSocket is the smallest reliable fit for the available curl, mbedTLS, cJSON,
TCP/UDP, ALSA and Opus configuration.  OpenVela's curl headers expose WebSocket
APIs, but that curl build does not enable `USE_WEBSOCKETS`; `cloud_client.c`
therefore uses curl `CONNECT_ONLY` for DNS/TCP/TLS/certificate handling and a
small internal RFC 6455 upgrade/frame implementation.

## WebSocket compatibility

The handshake sends these runtime-provided headers:

```text
Authorization: Bearer <token>   # omitted when no token was provisioned
Protocol-Version: 1
Device-Id: <device identity>
Client-Id: <client UUID>
User-Agent: <runtime/default product agent>
```

Header values containing CR/LF are rejected.  `wss://` verifies the peer and
hostname by default.  `allow_insecure_tls` is an explicit development-only
opt-in; production should provide a valid CA store/path through runtime config.

The application hello is compatible with upstream version 1:

```json
{
  "type": "hello",
  "version": 1,
  "transport": "websocket",
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

The server hello's Opus rate/channels/frame duration are validated and used to
configure the decoder/playback path.  The target server normally replies with
24 kHz, mono, 60 ms downlink audio.

Protocol-Version 1 is intentionally enforced.  Upstream WebSocket versions 2
and 3 wrap binary payloads in extra headers, but the reviewed target server's
direct WebSocket receive path decodes each binary message as raw Opus.  Its
timestamp/AEC packet parsing is currently in the MQTT-gateway path.  Sending a
version-2 wrapper directly to this server would be decoded as corrupt Opus.

Supported client messages:

- `listen/start` with `auto` or `manual`.
- `listen/stop`.
- `listen/detect` with text, used for text questions and wake-word text.
- `abort` with an optional reason.
- Raw binary Opus while listening.

Supported server messages/events:

- `hello`, `stt`, `llm`.
- `tts/start`, `tts/sentence_start`, `tts/stop`.
- `mcp`, `system`, `alert`, `pong`, and unknown extension types.
- Raw binary Opus downlink.

The target server explicitly accepts arbitrary `listen/detect.text` content and
routes it to the LLM, which enables text-only questions.  This is useful but is
not guaranteed by every Xiaozhi-compatible server deployment.

The full audio sequence is:

1. Capture signed 16-bit interleaved PCM at 16 kHz mono.
2. Encode legal Opus frames (default 60 ms, VOIP application).
3. Send each Opus packet as one masked WebSocket binary message.
4. Parse STT and TTS sentence text JSON messages.
5. Decode server binary Opus at the negotiated rate/channels.
6. Write PCM to ALSA and drain the PCM device after `tts/stop` before resuming
   auto capture.

## MQTT + UDP compatibility boundary

OTA responses can advertise MQTT settings, and `xiaozhi_ota_result.has_mqtt`
preserves that fact.  The data plane is not silently substituted because the
reviewed server requires the separate `xiaozhi-mqtt-gateway`; it also adds
broker lifecycle, UDP AES-CTR framing, SSRC/timestamp/sequence management and
network/firewall configuration.  Deployments that return MQTT only must either
enable/configure that gateway and add a dedicated transport implementation or
configure OTA to return a WebSocket endpoint.

## OTA flow

`xiaozhi_ota_fetch()` defaults to the official
`https://api.tenclass.net/xiaozhi/ota/` endpoint through Kconfig. It sends the
official `Activation-Version: 1`, `Device-Id`, `Client-Id`, `User-Agent`,
`Accept-Language` and JSON content headers. The request body uses system-info
schema version 2 and includes application, board, MAC and UUID identity. It
parses:

- `websocket.url` and `websocket.token`.
- Presence of `mqtt` settings.
- Activation code/message/challenge.
- Firmware version/download URL.
- Server timestamp and timezone offset.

For unbound devices, `xiaozhi_provisioning` shows `activation.code` through its
UI-safe snapshot/callback API, polls `POST <ota-url>/activate` (HTTP 202 means
pending; HTTP 200 means paired), then checks OTA again to obtain WSS/token.
Device-Id is the board Wi-Fi MAC and Client-Id is a persistent UUID v4, matching
the official client. Credentials are atomically written with mode 0600 and are
not accepted as CLI arguments.

Only discovery is implemented.  Firmware authenticity, flash writes, rollback
and activation persistence are board/product policy and must use the OpenVela
OTA facilities.

## Known risks and board validation items

- No live authenticated server session was performed because no deployment
  endpoint or credentials were placed in scope.
- No Gemini-S1 microphone/speaker hardware run was performed.  Confirm the
  board's ALSA device names and that the playback path accepts the negotiated
  rate (commonly 24 kHz).  If it requires another fixed rate, add a board audio
  backend with resampling.
- Realtime listening is rejected by the built-in backend.  Enabling it without
  AEC would feed TTS output back into ASR and can cause false barge-in.
- Downlink queue overflow drops the oldest packet and resets Opus decoder state;
  size queues for worst-case scheduler/network stalls.
- TLS depends on a correct clock and CA store.  Do not solve certificate errors
  by shipping `allow_insecure_tls=true`.
- OTA activation schemas and target-server extensions can evolve independently;
  retain the raw JSON callback for forward-compatible handling.

## Verification performed

- Host parser/state-machine executable compiled with cJSON 1.7.12 and passed.
- `cloud_client.c` compiled with curl 8.4 headers and mbedTLS 3.4 headers under
  `-Wall -Wextra -Werror`.
- `xiaozhi_ota.c` compiled with curl 8.4 and cJSON headers under strict warnings.
- `xiaozhi_audio.c` compiled with OpenVela ALSA-compatible headers and Opus 1.3.1
  headers under strict warnings.

These checks validate parsing, state transitions and API-level compilation; they
do not replace a final full OpenVela image build or on-board audio/network test.
