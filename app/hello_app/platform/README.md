# Gemini-S1 platform service

This directory is the non-display hardware baseline for Gemini-S1.  It does
not call LVGL and does not implement any cloud-model protocol.

The UI-facing contract is `gs1_platform.h`: callers enqueue a request with
`gs1_platform_request()` and copy an immutable status snapshot with
`gs1_platform_get_status()`.  File I/O, DHCP, WAPI association, nxrecorder,
nxplayer and NTP all run away from the UI thread.  UI code must never call the
synchronous module helpers directly.

Recording is stream-first.  nxrecorder writes PCM into a private FIFO, a
dedicated pump moves frames into a bounded ring every few milliseconds, and a
network/codec worker can pull available bytes without blocking:

```c
uint8_t pcm[1280];
size_t available;

gs1_platform_audio_stream_read(pcm, sizeof(pcm), &available);
```

The ring drops the oldest complete PCM frames when a consumer falls behind,
so stale audio never increases conversational latency.  `retain_file=false`
is the intended live Xiaozhi path; setting it to true tees the same stream to
the crash-safe recording lifecycle below.  This layer intentionally does not
send the frames to any cloud protocol.

## Persistence and recording lifecycle

- Application data lives below `/data/gemini-s1` by default.
- Small metadata uses KVDB `persist.gs1.*` keys and explicit commits.
- File updates use a same-directory temporary file, `fsync`, atomic `rename`
  and `sync`, which is the power-loss-safe publish sequence used for YAFFS.
- When `retain_file=true`, stream frames are first written as
  `recordings/*.pcm.part`.  Only a successful `nxrecorder_stop()` publishes the
  file as `.pcm`; boot probing removes abandoned `.part` files.
- Retention defaults to 20 recordings / 64 MiB.  The oldest completed file is
  removed only after a newer recording has been safely published.
- Wi-Fi passphrases are never copied into status or KVDB.  If persistence is
  requested, WAPI writes a temporary config that is atomically renamed to the
  configured `/data/etc/wifi/wapi.conf`.

## Enable and build

Enable:

```text
CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_SERVICE=y
```

The existing RTL8733BS/WAPI, DHCP/netlib, R528 Audio, nxrecorder/nxplayer,
NTP client, YAFFS and KVDB options are Kconfig dependencies.

```sh
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh \
  --cmake -j6
```

## Board acceptance commands

```text
gs1_hwcheck status
gs1_hwcheck storage
gs1_hwcheck wifi-connect YOUR_SSID YOUR_PASSWORD
ifconfig wlan0
ping -c 3 1.1.1.1
gs1_hwcheck ntp 30
date
gs1_hwcheck stream 5
gs1_hwcheck record 5
ls -l /data/gemini-s1/recordings
gs1_hwcheck audio-loopback 5
reboot
gs1_hwcheck storage
ls -l /data/gemini-s1/recordings
```

For power-loss testing, start `gs1_hwcheck record 30`, remove power during the
recording, boot again and run `gs1_hwcheck storage`.  No `.part` file should
remain and previously published `.pcm` files must still be readable.
