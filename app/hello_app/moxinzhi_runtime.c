/****************************************************************************
 * Contest 2026 team 208 - product platform and XiaoZhi runtime
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_KVDB
#  include <kvdb.h>
#endif

#include "moxinzhi_runtime.h"
#include "moxinzhi_text.h"

#if defined(CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_SERVICE) && \
    defined(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT)

#  include "cloud_client.h"
#  include "platform/gs1_platform.h"
#  include "platform/gs1_xiaozhi_audio.h"
#  include "xiaozhi_client.h"
#  include "xiaozhi_defaults.h"
#  include "xiaozhi_provisioning.h"

#define MZ_RUNTIME_WORKER_STACK       65536
#define MZ_RUNTIME_WORKER_SLEEP_US    5000
#define MZ_RUNTIME_CLIENT_POLL_MS     10
#define MZ_RUNTIME_QUERY_TIMEOUT_MS   45000
#define MZ_RUNTIME_CONNECT_TIMEOUT_MS 20000
#define MZ_RUNTIME_PTT_LIMIT_MS       60000
#define MZ_RUNTIME_PLAYBACK_DRAIN_MS  5000
#define MZ_RUNTIME_NTP_RETRY_MS       30000
#define MZ_RUNTIME_VOLUME_KEY         "persist.moxinzhi.volume"
#define MZ_RUNTIME_BRIGHTNESS_KEY     "persist.moxinzhi.brightness"
#define MZ_RUNTIME_LANGUAGE_KEY       "persist.moxinzhi.chinese"
#define MZ_RUNTIME_THEME_KEY          "persist.moxinzhi.dark_theme"

enum mz_dialogue_kind_e
{
  MZ_DIALOGUE_NONE = 0,
  MZ_DIALOGUE_TEXT,
  MZ_DIALOGUE_VOICE
};

struct mz_runtime_s
{
  pthread_mutex_t lock;
  pthread_t worker;
  FAR struct xiaozhi_provisioning *provisioning;
  FAR struct cloud_client *cloud;
  FAR struct xiaozhi_client *client;
  FAR struct gs1_xiaozhi_audio_s *audio;
  struct xiaozhi_pairing_snapshot pairing;
  struct xiaozhi_cloud_credentials credentials;
  struct mz_ai_result_s query_result;
  char query_question[MZ_QUESTION_MAX];
  char dialogue_question[MZ_QUESTION_MAX];
  char dialogue_answer[MZ_ANSWER_MAX];
  char ai_detail[96];
  int query_status;
  int ai_error;
  uint64_t next_ntp_ms;
  uint64_t query_deadline_ms;
  uint64_t recording_started_ms;
  enum mz_dialogue_kind_e dialogue_kind;
  enum mz_ai_state_e ai_state;
  uint8_t volume;
  uint8_t brightness;
  bool chinese;
  bool dark_theme;
  bool stop;
  bool worker_started;
  bool platform_owned;
  bool cloud_initialized;
  bool force_pairing;
  bool clear_pairing;
  bool credentials_ready;
  bool client_rebuild;
  bool query_pending;
  bool query_running;
  bool query_ready;
  bool ptt_begin_pending;
  bool ptt_end_pending;
  bool ptt_cancel_pending;
  bool ptt_listening;
  bool ptt_barge_pending;
  bool client_abort_pending;
  bool answer_from_llm;
};

static uint64_t mz_runtime_now_ms(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000ull +
         (uint64_t)now.tv_nsec / 1000000ull;
}

static void mz_runtime_copy(FAR char *target, size_t capacity,
                            FAR const char *source)
{
  mz_text_copy_utf8(target, capacity, source);
}

static void mz_runtime_pairing_changed(
  FAR void *user, FAR const struct xiaozhi_pairing_snapshot *snapshot)
{
  FAR struct mz_runtime_s *runtime = user;

  pthread_mutex_lock(&runtime->lock);
  runtime->pairing = *snapshot;
  pthread_mutex_unlock(&runtime->lock);
}

static void mz_runtime_load_settings(FAR struct mz_runtime_s *runtime)
{
  int32_t value;

  runtime->volume = 70;
  runtime->brightness = 80;
  runtime->chinese = true;
  runtime->dark_theme = false;

#ifdef CONFIG_KVDB
  if (property_get_int32_with_err(MZ_RUNTIME_VOLUME_KEY, &value) == 0 &&
      value >= 0 && value <= 100)
    {
      runtime->volume = (uint8_t)value;
    }

  if (property_get_int32_with_err(MZ_RUNTIME_BRIGHTNESS_KEY, &value) == 0 &&
      value >= 0 && value <= 100)
    {
      runtime->brightness = (uint8_t)value;
    }

  if (property_get_int32_with_err(MZ_RUNTIME_LANGUAGE_KEY, &value) == 0)
    {
      runtime->chinese = value != 0;
    }

  if (property_get_int32_with_err(MZ_RUNTIME_THEME_KEY, &value) == 0)
    {
      runtime->dark_theme = value != 0;
    }
#endif
}

static int mz_runtime_store_setting(FAR const char *key, int32_t value)
{
#ifdef CONFIG_KVDB
  return property_set_int32(key, value);
#else
  (void)key;
  (void)value;
  return 0;
#endif
}

static void mz_runtime_update_credentials(FAR struct mz_runtime_s *runtime)
{
  struct xiaozhi_cloud_credentials credentials;
  bool changed;

  if (xiaozhi_provisioning_get_credentials(runtime->provisioning,
                                            &credentials) < 0)
    {
      return;
    }

  pthread_mutex_lock(&runtime->lock);
  changed = !runtime->credentials_ready ||
            memcmp(&runtime->credentials, &credentials,
                   sizeof(credentials)) != 0;
  runtime->credentials = credentials;
  runtime->credentials_ready = true;
  runtime->client_rebuild = runtime->client_rebuild || changed;
  if (runtime->ai_state == MZ_AI_PAIR_REQUIRED || changed)
    {
      runtime->ai_state = MZ_AI_CONNECTING;
      runtime->ai_error = 0;
      runtime->ai_detail[0] = '\0';
    }

  pthread_mutex_unlock(&runtime->lock);
}

static void mz_runtime_poll_time(FAR struct mz_runtime_s *runtime,
                                 uint64_t now_ms)
{
  struct gs1_platform_request_s request;
  struct gs1_platform_status_s status;

  if (now_ms < runtime->next_ntp_ms ||
      gs1_platform_get_status(&status) < 0 ||
      status.wifi.state != GS1_WIFI_CONNECTED ||
      status.time.realtime_valid)
    {
      return;
    }

  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_TIME_SYNC;
  (void)gs1_platform_request(&request, NULL);
  runtime->next_ntp_ms = now_ms + MZ_RUNTIME_NTP_RETRY_MS;
}

static void mz_runtime_ai_tick(FAR struct mz_runtime_s *runtime,
                               uint64_t now_ms);
static void mz_runtime_ai_shutdown(FAR struct mz_runtime_s *runtime);

static FAR void *mz_runtime_worker(FAR void *arg)
{
  FAR struct mz_runtime_s *runtime = arg;
  struct xiaozhi_pairing_snapshot snapshot;
  bool clear_pairing;
  bool force_pairing;
  bool stop;
  uint64_t now_ms;
  int ret;

  now_ms = mz_runtime_now_ms();
  (void)xiaozhi_provisioning_start(runtime->provisioning, false, now_ms);

  for (;;)
    {
      pthread_mutex_lock(&runtime->lock);
      stop = runtime->stop;
      clear_pairing = runtime->clear_pairing;
      force_pairing = runtime->force_pairing;
      runtime->clear_pairing = false;
      runtime->force_pairing = false;
      pthread_mutex_unlock(&runtime->lock);

      if (stop)
        {
          break;
        }

      now_ms = mz_runtime_now_ms();
      if (clear_pairing)
        {
          (void)xiaozhi_provisioning_clear_credentials(
            runtime->provisioning);
          pthread_mutex_lock(&runtime->lock);
          memset(&runtime->credentials, 0, sizeof(runtime->credentials));
          runtime->credentials_ready = false;
          runtime->client_rebuild = true;
          runtime->ai_state = MZ_AI_PAIR_REQUIRED;
          runtime->ai_error = -EACCES;
          mz_runtime_copy(runtime->ai_detail,
                          sizeof(runtime->ai_detail),
                          "pairing required");
          pthread_mutex_unlock(&runtime->lock);
          force_pairing = true;
        }

      if (force_pairing)
        {
          (void)xiaozhi_provisioning_start(runtime->provisioning, true,
                                           now_ms);
        }

      mz_runtime_poll_time(runtime, now_ms);
      ret = xiaozhi_provisioning_poll(runtime->provisioning, now_ms);
      if (ret != -EAGAIN)
        {
          (void)xiaozhi_provisioning_get_snapshot(runtime->provisioning,
                                                  &snapshot);
          if (snapshot.state == XIAOZHI_PAIRING_READY &&
              snapshot.has_credentials)
            {
              mz_runtime_update_credentials(runtime);
            }
        }

      mz_runtime_ai_tick(runtime, now_ms);
      usleep(MZ_RUNTIME_WORKER_SLEEP_US);
    }

  mz_runtime_ai_shutdown(runtime);
  return NULL;
}

static enum mz_wifi_state_e mz_runtime_wifi_state(enum gs1_wifi_state_e state)
{
  switch (state)
    {
      case GS1_WIFI_CONNECTED:
        return MZ_WIFI_CONNECTED;
      case GS1_WIFI_CONNECTING:
        return MZ_WIFI_CONNECTING;
      case GS1_WIFI_DOWN:
      case GS1_WIFI_DISCONNECTED:
        return MZ_WIFI_DISCONNECTED;
      case GS1_WIFI_ERROR:
        return MZ_WIFI_ERROR;
      default:
        return MZ_WIFI_UNAVAILABLE;
    }
}

static enum mz_wifi_scan_state_e mz_runtime_wifi_scan_state(
  enum gs1_wifi_scan_state_e state)
{
  switch (state)
    {
      case GS1_WIFI_SCANNING:
        return MZ_WIFI_SCANNING;
      case GS1_WIFI_SCAN_READY:
        return MZ_WIFI_SCAN_READY;
      case GS1_WIFI_SCAN_ERROR:
        return MZ_WIFI_SCAN_ERROR;
      default:
        return MZ_WIFI_SCAN_IDLE;
    }
}

static enum mz_pair_state_e mz_runtime_pair_state(
  enum xiaozhi_pairing_state state)
{
  switch (state)
    {
      case XIAOZHI_PAIRING_CHECKING:
      case XIAOZHI_PAIRING_RETRY_WAIT:
        return MZ_PAIR_REQUESTING;
      case XIAOZHI_PAIRING_CODE_READY:
      case XIAOZHI_PAIRING_WAITING:
        return MZ_PAIR_CODE_READY;
      case XIAOZHI_PAIRING_READY:
        return MZ_PAIR_PAIRED;
      case XIAOZHI_PAIRING_ERROR:
        return MZ_PAIR_ERROR;
      default:
        return MZ_PAIR_UNPAIRED;
    }
}

static int mz_runtime_refresh(FAR void *context,
                              FAR struct mz_platform_status_s *status)
{
  FAR struct mz_runtime_s *runtime = context;
  struct gs1_platform_status_s platform;
  struct xiaozhi_pairing_snapshot pairing;
  unsigned int i;

  if (runtime == NULL || status == NULL)
    {
      return -EINVAL;
    }

  memset(&platform, 0, sizeof(platform));
  memset(status, 0, sizeof(*status));
  if (gs1_platform_get_status(&platform) >= 0)
    {
      status->wifi_state = mz_runtime_wifi_state(platform.wifi.state);
      status->wifi_scan_state =
        mz_runtime_wifi_scan_state(platform.wifi_scan.state);
      mz_runtime_copy(status->wifi_ssid, sizeof(status->wifi_ssid),
                      platform.wifi.ssid);
      mz_runtime_copy(status->wifi_ip, sizeof(status->wifi_ip),
                      platform.wifi.ipv4);
      status->wifi_scan_generation = platform.wifi_scan.generation;
      status->wifi_scan_error = platform.wifi_scan.last_error;
      status->wifi_network_count = platform.wifi_scan.count;
      if (status->wifi_network_count > MZ_PLATFORM_WIFI_SCAN_MAX)
        {
          status->wifi_network_count = MZ_PLATFORM_WIFI_SCAN_MAX;
        }

      for (i = 0; i < status->wifi_network_count; i++)
        {
          mz_runtime_copy(status->wifi_networks[i].ssid,
                          sizeof(status->wifi_networks[i].ssid),
                          platform.wifi_scan.networks[i].ssid);
          status->wifi_networks[i].rssi =
            platform.wifi_scan.networks[i].rssi;
          status->wifi_networks[i].secured =
            platform.wifi_scan.networks[i].secured;
        }
    }
  else
    {
      status->wifi_state = MZ_WIFI_UNAVAILABLE;
    }

  pthread_mutex_lock(&runtime->lock);
  pairing = runtime->pairing;
  status->volume = runtime->volume;
  status->brightness = runtime->brightness;
  status->chinese = runtime->chinese;
  status->dark_theme = runtime->dark_theme;
  status->ai_state = runtime->ai_state;
  status->ai_error = runtime->ai_error;
  mz_runtime_copy(status->ai_detail, sizeof(status->ai_detail),
                  runtime->ai_detail);
  pthread_mutex_unlock(&runtime->lock);

  status->pair_state = mz_runtime_pair_state(pairing.state);
  mz_runtime_copy(status->pair_code, sizeof(status->pair_code),
                  pairing.pairing_code);
  mz_runtime_copy(status->device_id, sizeof(status->device_id),
                  pairing.identity.device_id);
  mz_runtime_copy(status->model, sizeof(status->model), "Gemini-S1");
  mz_runtime_copy(status->firmware, sizeof(status->firmware),
                  XIAOZHI_DEFAULT_FIRMWARE_VERSION);
  return 0;
}

static int mz_runtime_wifi_scan(FAR void *context)
{
  struct gs1_platform_request_s request;

  (void)context;
  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_WIFI_SCAN;
  return gs1_platform_request(&request, NULL);
}

static int mz_runtime_wifi_connect(FAR void *context, FAR const char *ssid,
                                   FAR const char *password)
{
  struct gs1_platform_request_s request;

  (void)context;
  if (ssid == NULL || password == NULL || ssid[0] == '\0')
    {
      return -EINVAL;
    }

  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_WIFI_CONNECT;
  mz_runtime_copy(request.data.wifi_connect.ssid,
                  sizeof(request.data.wifi_connect.ssid), ssid);
  mz_runtime_copy(request.data.wifi_connect.passphrase,
                  sizeof(request.data.wifi_connect.passphrase), password);
  request.data.wifi_connect.persist = true;
  return gs1_platform_request(&request, NULL);
}

static int mz_runtime_set_volume(FAR void *context, uint8_t value)
{
  FAR struct mz_runtime_s *runtime = context;
  struct gs1_platform_request_s request;
  int ret;

  if (runtime == NULL || value > 100)
    {
      return -EINVAL;
    }

  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_AUDIO_SET_VOLUME;
  request.data.audio_volume.percent = value;
  ret = gs1_platform_request(&request, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = mz_runtime_store_setting(MZ_RUNTIME_VOLUME_KEY, value);
  if (ret >= 0)
    {
      pthread_mutex_lock(&runtime->lock);
      runtime->volume = value;
      pthread_mutex_unlock(&runtime->lock);
    }

  return ret;
}

static int mz_runtime_set_brightness(FAR void *context, uint8_t value)
{
  FAR struct mz_runtime_s *runtime = context;
  int ret;

  if (runtime == NULL || value > 100)
    {
      return -EINVAL;
    }

  ret = mz_runtime_store_setting(MZ_RUNTIME_BRIGHTNESS_KEY, value);
  if (ret >= 0)
    {
      pthread_mutex_lock(&runtime->lock);
      runtime->brightness = value;
      pthread_mutex_unlock(&runtime->lock);
    }

  return ret;
}

static int mz_runtime_set_language(FAR void *context, bool chinese)
{
  FAR struct mz_runtime_s *runtime = context;
  int ret;

  if (runtime == NULL)
    {
      return -EINVAL;
    }

  ret = mz_runtime_store_setting(MZ_RUNTIME_LANGUAGE_KEY, chinese);
  if (ret >= 0)
    {
      pthread_mutex_lock(&runtime->lock);
      runtime->chinese = chinese;
      pthread_mutex_unlock(&runtime->lock);
    }

  return ret;
}

static int mz_runtime_set_theme(FAR void *context, bool dark_theme)
{
  FAR struct mz_runtime_s *runtime = context;
  int ret;

  if (runtime == NULL)
    {
      return -EINVAL;
    }

  ret = mz_runtime_store_setting(MZ_RUNTIME_THEME_KEY, dark_theme);
  if (ret >= 0)
    {
      pthread_mutex_lock(&runtime->lock);
      runtime->dark_theme = dark_theme;
      pthread_mutex_unlock(&runtime->lock);
    }

  return ret;
}

static int mz_runtime_request_pairing(FAR void *context)
{
  FAR struct mz_runtime_s *runtime = context;

  if (runtime == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&runtime->lock);
  runtime->force_pairing = true;
  runtime->credentials_ready = false;
  runtime->client_rebuild = true;
  runtime->ai_state = MZ_AI_PAIR_REQUIRED;
  pthread_mutex_unlock(&runtime->lock);
  return 0;
}

static int mz_runtime_forget_pairing(FAR void *context)
{
  FAR struct mz_runtime_s *runtime = context;

  if (runtime == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&runtime->lock);
  runtime->clear_pairing = true;
  pthread_mutex_unlock(&runtime->lock);
  return 0;
}

static int mz_runtime_ptt_begin(FAR void *context)
{
  FAR struct mz_runtime_s *runtime = context;
  bool interrupting;

  if (runtime == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&runtime->lock);
  if (!runtime->credentials_ready)
    {
      pthread_mutex_unlock(&runtime->lock);
      return -EACCES;
    }

  if (runtime->ptt_begin_pending || runtime->ptt_listening)
    {
      pthread_mutex_unlock(&runtime->lock);
      return -EBUSY;
    }

  interrupting = runtime->query_pending || runtime->query_running ||
                 runtime->query_ready ||
                 runtime->dialogue_kind != MZ_DIALOGUE_NONE ||
                 runtime->ai_state == MZ_AI_THINKING ||
                 runtime->ai_state == MZ_AI_SPEAKING;
  if (interrupting && runtime->ai_state != MZ_AI_THINKING &&
      runtime->ai_state != MZ_AI_SPEAKING)
    {
      pthread_mutex_unlock(&runtime->lock);
      return -EBUSY;
    }

  runtime->query_running = true;
  runtime->query_pending = false;
  runtime->ptt_begin_pending = true;
  runtime->ptt_end_pending = false;
  runtime->ptt_cancel_pending = false;
  runtime->ptt_listening = false;
  runtime->ptt_barge_pending = interrupting;
  runtime->client_abort_pending = false;
  runtime->query_ready = false;
  runtime->query_status = -EINPROGRESS;
  memset(&runtime->query_result, 0, sizeof(runtime->query_result));
  runtime->query_question[0] = '\0';
  runtime->dialogue_kind = MZ_DIALOGUE_NONE;
  runtime->dialogue_question[0] = '\0';
  runtime->dialogue_answer[0] = '\0';
  runtime->answer_from_llm = false;
  runtime->query_deadline_ms = mz_runtime_now_ms() +
                               MZ_RUNTIME_CONNECT_TIMEOUT_MS +
                               MZ_RUNTIME_PTT_LIMIT_MS;
  runtime->ai_state = MZ_AI_STARTING;
  runtime->ai_error = 0;
  runtime->ai_detail[0] = '\0';
  pthread_mutex_unlock(&runtime->lock);
  return 0;
}

static int mz_runtime_ptt_end(FAR void *context)
{
  FAR struct mz_runtime_s *runtime = context;

  if (runtime == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&runtime->lock);
  if (!runtime->query_running ||
      (!runtime->ptt_begin_pending && !runtime->ptt_listening &&
       runtime->dialogue_kind != MZ_DIALOGUE_VOICE))
    {
      pthread_mutex_unlock(&runtime->lock);
      return -EINVAL;
    }

  runtime->ptt_end_pending = true;
  if (runtime->ptt_listening)
    {
      runtime->ai_state = MZ_AI_THINKING;
    }

  pthread_mutex_unlock(&runtime->lock);
  return 0;
}

static int mz_runtime_ptt_cancel(FAR void *context)
{
  FAR struct mz_runtime_s *runtime = context;

  if (runtime == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&runtime->lock);
  runtime->ptt_cancel_pending = true;
  pthread_mutex_unlock(&runtime->lock);
  return 0;
}

static const struct mz_platform_ops_s g_mz_runtime_platform_ops =
{
  .refresh = mz_runtime_refresh,
  .wifi_scan = mz_runtime_wifi_scan,
  .wifi_connect = mz_runtime_wifi_connect,
  .set_volume = mz_runtime_set_volume,
  .set_brightness = mz_runtime_set_brightness,
  .set_language = mz_runtime_set_language,
  .set_theme = mz_runtime_set_theme,
  .request_pairing = mz_runtime_request_pairing,
  .forget_pairing = mz_runtime_forget_pairing,
  .ptt_begin = mz_runtime_ptt_begin,
  .ptt_end = mz_runtime_ptt_end,
  .ptt_cancel = mz_runtime_ptt_cancel,
};

static void mz_runtime_append_answer_locked(FAR struct mz_runtime_s *runtime,
                                            FAR const char *text)
{
  size_t used;

  if (text == NULL || text[0] == '\0')
    {
      return;
    }

  used = strlen(runtime->dialogue_answer);
  if (used != 0 && used + 1 < sizeof(runtime->dialogue_answer))
    {
      runtime->dialogue_answer[used++] = ' ';
      runtime->dialogue_answer[used] = '\0';
    }

  snprintf(runtime->dialogue_answer + used,
           sizeof(runtime->dialogue_answer) - used, "%s", text);
  mz_text_utf8_trim(runtime->dialogue_answer);
}

static void mz_runtime_finish_dialogue_locked(
  FAR struct mz_runtime_s *runtime)
{
  FAR const char *question;

  if (!runtime->query_running ||
      runtime->dialogue_kind == MZ_DIALOGUE_NONE)
    {
      return;
    }

  if (runtime->dialogue_answer[0] == '\0')
    {
      runtime->query_status = -ENODATA;
      runtime->query_ready = true;
      runtime->query_running = false;
      runtime->dialogue_kind = MZ_DIALOGUE_NONE;
      runtime->query_pending = false;
      runtime->ptt_begin_pending = false;
      runtime->ptt_end_pending = false;
      runtime->ptt_cancel_pending = false;
      runtime->ptt_listening = false;
      runtime->ptt_barge_pending = false;
      runtime->answer_from_llm = false;
      runtime->query_deadline_ms = 0;
      runtime->recording_started_ms = 0;
      runtime->ai_error = -ENODATA;
      runtime->ai_state = MZ_AI_ERROR;
      mz_runtime_copy(runtime->ai_detail, sizeof(runtime->ai_detail),
                      "server returned no answer");
      return;
    }

  question = runtime->dialogue_question[0] != '\0' ?
             runtime->dialogue_question :
             runtime->query_question[0] != '\0' ?
             runtime->query_question : "Voice question";
  memset(&runtime->query_result, 0, sizeof(runtime->query_result));
  mz_runtime_copy(runtime->query_result.question,
                  sizeof(runtime->query_result.question), question);
  mz_runtime_copy(runtime->query_result.answer,
                  sizeof(runtime->query_result.answer),
                  runtime->dialogue_answer);
  mz_runtime_copy(runtime->query_result.topic,
                  sizeof(runtime->query_result.topic), "XiaoZhi");
  runtime->query_status = 0;
  runtime->query_ready = true;
  runtime->query_running = false;
  runtime->query_pending = false;
  runtime->dialogue_kind = MZ_DIALOGUE_NONE;
  runtime->client_abort_pending = false;
  runtime->ptt_begin_pending = false;
  runtime->ptt_end_pending = false;
  runtime->ptt_cancel_pending = false;
  runtime->ptt_listening = false;
  runtime->ptt_barge_pending = false;
  runtime->answer_from_llm = false;
  runtime->query_deadline_ms = 0;
  runtime->recording_started_ms = 0;
  runtime->ai_error = 0;
  runtime->ai_detail[0] = '\0';
}

static void mz_runtime_fail_dialogue_locked(
  FAR struct mz_runtime_s *runtime, int error, FAR const char *detail)
{
  if (error >= 0)
    {
      error = -EIO;
    }

  memset(&runtime->query_result, 0, sizeof(runtime->query_result));
  runtime->query_status = error;
  runtime->query_ready = runtime->query_running || runtime->query_pending;
  runtime->client_abort_pending = true;
  runtime->query_running = false;
  runtime->query_pending = false;
  runtime->dialogue_kind = MZ_DIALOGUE_NONE;
  runtime->ptt_begin_pending = false;
  runtime->ptt_end_pending = false;
  runtime->ptt_cancel_pending = false;
  runtime->ptt_listening = false;
  runtime->ptt_barge_pending = false;
  runtime->answer_from_llm = false;
  runtime->query_deadline_ms = 0;
  runtime->recording_started_ms = 0;
  runtime->ai_error = error;
  runtime->ai_state = MZ_AI_ERROR;
  mz_runtime_copy(runtime->ai_detail, sizeof(runtime->ai_detail),
                  detail == NULL ? "xiaozhi request failed" : detail);
}

static void mz_runtime_client_state(FAR void *user,
                                    enum xiaozhi_state old_state,
                                    enum xiaozhi_state new_state)
{
  FAR struct mz_runtime_s *runtime = user;

  (void)old_state;
  pthread_mutex_lock(&runtime->lock);
  switch (new_state)
    {
      case XIAOZHI_STATE_BACKOFF:
      case XIAOZHI_STATE_CONNECTING:
      case XIAOZHI_STATE_WAITING_HELLO:
        runtime->ai_state = MZ_AI_CONNECTING;
        break;

      case XIAOZHI_STATE_READY:
        if (!runtime->query_running && !runtime->query_pending)
          {
            runtime->ai_state = MZ_AI_READY;
            runtime->ai_error = 0;
            runtime->ai_detail[0] = '\0';
          }
        break;

      case XIAOZHI_STATE_LISTENING:
        runtime->ai_state = MZ_AI_LISTENING;
        break;

      case XIAOZHI_STATE_THINKING:
        runtime->ai_state = MZ_AI_THINKING;
        break;

      case XIAOZHI_STATE_SPEAKING:
      case XIAOZHI_STATE_DRAINING:
        runtime->ai_state = MZ_AI_SPEAKING;
        break;

      default:
        runtime->ai_state = MZ_AI_UNAVAILABLE;
        break;
    }

  pthread_mutex_unlock(&runtime->lock);
}

static void mz_runtime_client_event(FAR void *user,
                                    FAR const struct xiaozhi_event *event)
{
  FAR struct mz_runtime_s *runtime = user;

  pthread_mutex_lock(&runtime->lock);
  if (runtime->dialogue_kind == MZ_DIALOGUE_NONE)
    {
      pthread_mutex_unlock(&runtime->lock);
      return;
    }

  if (event->type == XIAOZHI_EVENT_STT && event->text[0] != '\0')
    {
      mz_runtime_copy(runtime->dialogue_question,
                      sizeof(runtime->dialogue_question), event->text);
    }
  else if (event->type == XIAOZHI_EVENT_TTS_SENTENCE)
    {
      if (runtime->answer_from_llm)
        {
          runtime->dialogue_answer[0] = '\0';
          runtime->answer_from_llm = false;
        }

      mz_runtime_append_answer_locked(runtime, event->text);
    }
  else if (event->type == XIAOZHI_EVENT_LLM &&
           runtime->dialogue_answer[0] == '\0')
    {
      mz_runtime_append_answer_locked(runtime, event->text);
      runtime->answer_from_llm = runtime->dialogue_answer[0] != '\0';
    }
  else if (event->type == XIAOZHI_EVENT_TTS_STOP)
    {
      mz_runtime_finish_dialogue_locked(runtime);
    }

  pthread_mutex_unlock(&runtime->lock);
}

static void mz_runtime_client_error(FAR void *user,
                                    FAR const struct xiaozhi_error *error)
{
  FAR struct mz_runtime_s *runtime = user;
  bool playback_only;
  char detail[96];

  playback_only = error->code == XIAOZHI_ERROR_AUDIO &&
                  error->phase != NULL &&
                  (strstr(error->phase, "playback") != NULL ||
                   strstr(error->phase, "downlink") != NULL);
  snprintf(detail, sizeof(detail), "%s (%d)",
           error->phase == NULL ? "xiaozhi" : error->phase,
           error->detail);

  pthread_mutex_lock(&runtime->lock);
  runtime->ai_error = error->detail < 0 ? error->detail : -EIO;
  mz_runtime_copy(runtime->ai_detail, sizeof(runtime->ai_detail), detail);
  if (playback_only)
    {
      pthread_mutex_unlock(&runtime->lock);
      return;
    }

  if (runtime->query_running || runtime->query_pending)
    {
      mz_runtime_fail_dialogue_locked(runtime, error->detail, detail);
    }
  else
    {
      runtime->ai_state = error->reconnecting ? MZ_AI_CONNECTING :
                                                MZ_AI_ERROR;
    }

  pthread_mutex_unlock(&runtime->lock);
}

int mz_runtime_create(FAR struct mz_runtime_s **out_runtime,
                      FAR struct mz_platform_binding_s *binding)
{
  struct xiaozhi_provisioning_config config;
  struct gs1_platform_request_s request;
  pthread_attr_t attributes;
  FAR struct mz_runtime_s *runtime;
  int ret;

  if (out_runtime == NULL || binding == NULL)
    {
      return -EINVAL;
    }

  *out_runtime = NULL;
  memset(binding, 0, sizeof(*binding));
  runtime = calloc(1, sizeof(*runtime));
  if (runtime == NULL)
    {
      return -ENOMEM;
    }

  pthread_mutex_init(&runtime->lock, NULL);
  mz_runtime_load_settings(runtime);
  runtime->ai_state = MZ_AI_PAIR_REQUIRED;
  runtime->ai_error = -EACCES;
  mz_runtime_copy(runtime->ai_detail, sizeof(runtime->ai_detail),
                  "pairing required");
  ret = gs1_platform_init();
  if (ret == 0)
    {
      runtime->platform_owned = true;
    }
  else if (ret != GS1_ERROR_ALREADY_STARTED)
    {
      goto failed;
    }

  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_AUDIO_SET_VOLUME;
  request.data.audio_volume.percent = runtime->volume;
  (void)gs1_platform_request(&request, NULL);

  ret = cloud_client_global_init();
  if (ret < 0)
    {
      goto failed;
    }

  runtime->cloud_initialized = true;

  memset(&config, 0, sizeof(config));
  config.ota_url = XIAOZHI_DEFAULT_OTA_URL;
  config.language = XIAOZHI_DEFAULT_LANGUAGE;
  config.wifi_ifname = XIAOZHI_DEFAULT_WIFI_IFNAME;
  config.identity_path = XIAOZHI_DEFAULT_IDENTITY_PATH;
  config.credentials_path = XIAOZHI_DEFAULT_CREDENTIALS_PATH;
  config.ca_file = XIAOZHI_DEFAULT_CA_FILE[0] == '\0' ? NULL :
                   XIAOZHI_DEFAULT_CA_FILE;
  config.firmware_version = XIAOZHI_DEFAULT_FIRMWARE_VERSION;
  config.application_name = XIAOZHI_DEFAULT_APPLICATION_NAME;
  config.board_name = XIAOZHI_DEFAULT_BOARD_NAME;
  config.chip_model_name = XIAOZHI_DEFAULT_CHIP_MODEL;
  config.user_agent = XIAOZHI_DEFAULT_USER_AGENT;
  config.request_timeout_ms = 15000;
  config.activation_poll_ms = 3000;
  config.max_attempts = 8;
  runtime->provisioning = xiaozhi_provisioning_create(
    &config, mz_runtime_pairing_changed, runtime);
  if (runtime->provisioning == NULL)
    {
      ret = -ENOMEM;
      goto failed;
    }

  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, MZ_RUNTIME_WORKER_STACK);
  ret = pthread_create(&runtime->worker, &attributes,
                       mz_runtime_worker, runtime);
  pthread_attr_destroy(&attributes);
  if (ret != 0)
    {
      ret = -ret;
      goto failed;
    }

  runtime->worker_started = true;
  binding->ops = &g_mz_runtime_platform_ops;
  binding->context = runtime;
  *out_runtime = runtime;
  return 0;

failed:
  xiaozhi_provisioning_destroy(runtime->provisioning);
  if (runtime->cloud_initialized)
    {
      cloud_client_global_cleanup();
    }

  if (runtime->platform_owned)
    {
      (void)gs1_platform_shutdown();
    }

  pthread_mutex_destroy(&runtime->lock);
  free(runtime);
  return ret;
}

void mz_runtime_destroy(FAR struct mz_runtime_s *runtime)
{
  if (runtime == NULL)
    {
      return;
    }

  pthread_mutex_lock(&runtime->lock);
  runtime->stop = true;
  pthread_mutex_unlock(&runtime->lock);
  if (runtime->worker_started)
    {
      pthread_join(runtime->worker, NULL);
    }

  xiaozhi_provisioning_destroy(runtime->provisioning);
  if (runtime->cloud_initialized)
    {
      cloud_client_global_cleanup();
    }

  if (runtime->platform_owned)
    {
      (void)gs1_platform_shutdown();
    }

  pthread_mutex_destroy(&runtime->lock);
  memset(runtime, 0, sizeof(*runtime));
  free(runtime);
}

static void mz_runtime_client_destroy(FAR struct mz_runtime_s *runtime)
{
  xiaozhi_client_destroy(runtime->client);
  runtime->client = NULL;
  gs1_xiaozhi_audio_destroy(runtime->audio);
  runtime->audio = NULL;
  cloud_client_destroy(runtime->cloud);
  runtime->cloud = NULL;
}

static int mz_runtime_client_create(FAR struct mz_runtime_s *runtime)
{
  struct xiaozhi_cloud_credentials credentials;
  struct xiaozhi_identity identity;
  struct cloud_client_config cloud_config;
  struct xiaozhi_client_config client_config;
  struct xiaozhi_callbacks callbacks;
  FAR const struct xiaozhi_audio_ops *audio_ops = NULL;
  int ret;

  pthread_mutex_lock(&runtime->lock);
  if (!runtime->credentials_ready)
    {
      pthread_mutex_unlock(&runtime->lock);
      return -EACCES;
    }

  credentials = runtime->credentials;
  identity = runtime->pairing.identity;
  pthread_mutex_unlock(&runtime->lock);

  memset(&cloud_config, 0, sizeof(cloud_config));
  cloud_config.url = credentials.websocket_url;
  cloud_config.bearer_token = credentials.websocket_token;
  cloud_config.device_id = identity.device_id;
  cloud_config.client_id = identity.client_id;
  cloud_config.user_agent = XIAOZHI_DEFAULT_USER_AGENT;
  cloud_config.ca_file = XIAOZHI_DEFAULT_CA_FILE[0] == '\0' ? NULL :
                         XIAOZHI_DEFAULT_CA_FILE;
  cloud_config.protocol_version = 1;
  cloud_config.connect_timeout_ms = 15000;
  cloud_config.io_timeout_ms = 15000;

  memset(&client_config, 0, sizeof(client_config));
  client_config.protocol_version = 1;
  client_config.input_sample_rate = 16000;
  client_config.input_channels = 1;
  client_config.frame_duration_ms = 20;
  client_config.hello_timeout_ms = 10000;
  client_config.reply_timeout_ms = MZ_RUNTIME_QUERY_TIMEOUT_MS;
  client_config.idle_timeout_ms = 300000;
  client_config.playback_drain_timeout_ms = MZ_RUNTIME_PLAYBACK_DRAIN_MS;

  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.on_state = mz_runtime_client_state;
  callbacks.on_event = mz_runtime_client_event;
  callbacks.on_error = mz_runtime_client_error;

  runtime->cloud = cloud_client_create(&cloud_config);
  if (runtime->cloud == NULL)
    {
      ret = -ENOMEM;
      goto failed;
    }

  runtime->audio = gs1_xiaozhi_audio_create();
  if (runtime->audio != NULL)
    {
      audio_ops = gs1_xiaozhi_audio_ops();
    }

  runtime->client = xiaozhi_client_create(
    &client_config, cloud_client_transport_ops(), runtime->cloud,
    audio_ops, runtime->audio, &callbacks, runtime);
  if (runtime->client == NULL)
    {
      ret = -ENOMEM;
      goto failed;
    }

  ret = xiaozhi_client_start(runtime->client);
  if (ret < 0)
    {
      goto failed;
    }

  pthread_mutex_lock(&runtime->lock);
  runtime->ai_state = MZ_AI_CONNECTING;
  runtime->ai_error = 0;
  runtime->ai_detail[0] = '\0';
  pthread_mutex_unlock(&runtime->lock);
  memset(&credentials, 0, sizeof(credentials));
  return 0;

failed:
  mz_runtime_client_destroy(runtime);
  pthread_mutex_lock(&runtime->lock);
  runtime->ai_state = MZ_AI_ERROR;
  runtime->ai_error = ret;
  mz_runtime_copy(runtime->ai_detail, sizeof(runtime->ai_detail),
                  "unable to create xiaozhi client");
  pthread_mutex_unlock(&runtime->lock);
  memset(&credentials, 0, sizeof(credentials));
  return ret;
}

static bool mz_runtime_connection_ready(FAR struct mz_runtime_s *runtime)
{
  struct gs1_platform_status_s platform;
  FAR const char *detail;

  if (gs1_platform_get_status(&platform) < 0)
    {
      return true;
    }

  if (platform.wifi.state != GS1_WIFI_CONNECTED)
    {
      detail = "waiting for Wi-Fi";
    }
  else if (!platform.time.realtime_valid)
    {
      detail = "waiting for time sync";
    }
  else
    {
      return true;
    }

  pthread_mutex_lock(&runtime->lock);
  runtime->ai_state = MZ_AI_CONNECTING;
  runtime->ai_error = 0;
  mz_runtime_copy(runtime->ai_detail, sizeof(runtime->ai_detail), detail);
  pthread_mutex_unlock(&runtime->lock);
  return false;
}

static void mz_runtime_cancel_voice(FAR struct mz_runtime_s *runtime)
{
  enum xiaozhi_state state;

  state = xiaozhi_client_state(runtime->client);
  if (state >= XIAOZHI_STATE_READY)
    {
      (void)xiaozhi_client_abort(runtime->client, "user_cancel");
      state = xiaozhi_client_state(runtime->client);
    }

  pthread_mutex_lock(&runtime->lock);
  runtime->ptt_cancel_pending = false;
  runtime->ptt_begin_pending = false;
  runtime->ptt_end_pending = false;
  runtime->ptt_listening = false;
  runtime->ptt_barge_pending = false;
  runtime->query_pending = false;
  runtime->query_running = false;
  runtime->query_ready = false;
  runtime->query_status = -ECANCELED;
  runtime->dialogue_kind = MZ_DIALOGUE_NONE;
  runtime->dialogue_question[0] = '\0';
  runtime->dialogue_answer[0] = '\0';
  runtime->answer_from_llm = false;
  runtime->query_deadline_ms = 0;
  runtime->recording_started_ms = 0;
  runtime->ai_error = 0;
  runtime->ai_detail[0] = '\0';
  runtime->ai_state = state == XIAOZHI_STATE_READY ? MZ_AI_READY :
                                                    MZ_AI_CONNECTING;
  pthread_mutex_unlock(&runtime->lock);
}

static void mz_runtime_start_text(FAR struct mz_runtime_s *runtime,
                                  uint64_t now_ms)
{
  char question[MZ_QUESTION_MAX];
  int ret;

  pthread_mutex_lock(&runtime->lock);
  mz_runtime_copy(question, sizeof(question), runtime->query_question);
  runtime->query_pending = false;
  runtime->dialogue_kind = MZ_DIALOGUE_TEXT;
  mz_runtime_copy(runtime->dialogue_question,
                  sizeof(runtime->dialogue_question), question);
  runtime->dialogue_answer[0] = '\0';
  runtime->query_deadline_ms = now_ms + MZ_RUNTIME_QUERY_TIMEOUT_MS;
  pthread_mutex_unlock(&runtime->lock);

  ret = xiaozhi_client_send_text_query(runtime->client, question);
  if (ret < 0)
    {
      pthread_mutex_lock(&runtime->lock);
      if (runtime->query_running)
        {
          mz_runtime_fail_dialogue_locked(runtime, ret,
                                          "send text query failed");
        }

      pthread_mutex_unlock(&runtime->lock);
    }
}

static void mz_runtime_start_voice(FAR struct mz_runtime_s *runtime,
                                   uint64_t now_ms)
{
  int ret;

  if (runtime->audio == NULL)
    {
      pthread_mutex_lock(&runtime->lock);
      mz_runtime_fail_dialogue_locked(runtime, -ENODEV,
                                      "microphone unavailable");
      pthread_mutex_unlock(&runtime->lock);
      return;
    }

  pthread_mutex_lock(&runtime->lock);
  runtime->dialogue_kind = MZ_DIALOGUE_VOICE;
  runtime->dialogue_question[0] = '\0';
  runtime->dialogue_answer[0] = '\0';
  runtime->answer_from_llm = false;
  runtime->recording_started_ms = now_ms;
  runtime->query_deadline_ms = now_ms + MZ_RUNTIME_PTT_LIMIT_MS +
                               MZ_RUNTIME_QUERY_TIMEOUT_MS;
  pthread_mutex_unlock(&runtime->lock);

  ret = xiaozhi_client_start_listening(runtime->client,
                                       XIAOZHI_LISTEN_MANUAL);
  pthread_mutex_lock(&runtime->lock);
  if (ret < 0)
    {
      if (runtime->query_running)
        {
          mz_runtime_fail_dialogue_locked(runtime, ret,
                                          "start microphone failed");
        }
    }
  else
    {
      runtime->ptt_begin_pending = false;
      runtime->ptt_listening = true;
      runtime->ai_state = MZ_AI_LISTENING;
    }

  pthread_mutex_unlock(&runtime->lock);
}

static void mz_runtime_stop_voice(FAR struct mz_runtime_s *runtime,
                                  uint64_t now_ms)
{
  int ret;

  ret = xiaozhi_client_stop_listening(runtime->client);
  pthread_mutex_lock(&runtime->lock);
  runtime->ptt_end_pending = false;
  runtime->ptt_listening = false;
  runtime->query_deadline_ms = now_ms + MZ_RUNTIME_QUERY_TIMEOUT_MS;
  if (ret < 0)
    {
      if (runtime->query_running)
        {
          mz_runtime_fail_dialogue_locked(runtime, ret,
                                          "stop microphone failed");
        }
    }
  else
    {
      runtime->ai_state = MZ_AI_THINKING;
    }

  pthread_mutex_unlock(&runtime->lock);
}

static int mz_runtime_barge_in(FAR struct mz_runtime_s *runtime)
{
  int ret;

  ret = xiaozhi_client_abort(runtime->client, "user_barge_in");
  pthread_mutex_lock(&runtime->lock);
  if (ret < 0)
    {
      if (runtime->query_running)
        {
          mz_runtime_fail_dialogue_locked(runtime, ret,
                                          "interrupt playback failed");
        }
    }
  else
    {
      runtime->ptt_barge_pending = false;
      runtime->ai_state = MZ_AI_STARTING;
      runtime->ai_error = 0;
      runtime->ai_detail[0] = '\0';
    }

  pthread_mutex_unlock(&runtime->lock);
  return ret;
}

static void mz_runtime_ai_tick(FAR struct mz_runtime_s *runtime,
                               uint64_t now_ms)
{
  enum xiaozhi_state state;
  bool credentials_ready;
  bool rebuild;
  bool cancel;
  bool abort_client;
  bool barge;
  bool begin_voice;
  bool end_voice;
  bool begin_text;
  bool listening;
  bool running;
  uint64_t started_ms;
  uint64_t deadline_ms;
  int ret;

  pthread_mutex_lock(&runtime->lock);
  credentials_ready = runtime->credentials_ready;
  rebuild = runtime->client_rebuild;
  runtime->client_rebuild = false;
  if ((!credentials_ready || rebuild) &&
      (runtime->query_running || runtime->query_pending))
    {
      mz_runtime_fail_dialogue_locked(
        runtime, -ECONNRESET,
        credentials_ready ? "xiaozhi credentials changed" :
                            "xiaozhi pairing restarted");
      runtime->client_abort_pending = false;
    }

  pthread_mutex_unlock(&runtime->lock);

  if (!credentials_ready || rebuild)
    {
      mz_runtime_client_destroy(runtime);
    }

  if (!credentials_ready)
    {
      pthread_mutex_lock(&runtime->lock);
      runtime->ai_state = MZ_AI_PAIR_REQUIRED;
      pthread_mutex_unlock(&runtime->lock);
      return;
    }

  if (runtime->client == NULL && !mz_runtime_connection_ready(runtime))
    {
      return;
    }

  if (runtime->client == NULL && mz_runtime_client_create(runtime) < 0)
    {
      return;
    }

  pthread_mutex_lock(&runtime->lock);
  abort_client = runtime->client_abort_pending;
  runtime->client_abort_pending = false;
  barge = runtime->ptt_barge_pending;
  cancel = runtime->ptt_cancel_pending &&
           (runtime->ptt_begin_pending || runtime->ptt_listening ||
            runtime->dialogue_kind == MZ_DIALOGUE_VOICE);
  if (runtime->ptt_cancel_pending && !cancel)
    {
      runtime->ptt_cancel_pending = false;
    }

  pthread_mutex_unlock(&runtime->lock);
  if (abort_client &&
      xiaozhi_client_state(runtime->client) >= XIAOZHI_STATE_READY)
    {
      (void)xiaozhi_client_abort(runtime->client, "request_failed");
    }

  if (cancel)
    {
      mz_runtime_cancel_voice(runtime);
      return;
    }

  state = xiaozhi_client_state(runtime->client);
  if (barge && state >= XIAOZHI_STATE_READY)
    {
      if (mz_runtime_barge_in(runtime) < 0)
        {
          return;
        }

      barge = false;
    }

  ret = xiaozhi_client_poll(runtime->client, MZ_RUNTIME_CLIENT_POLL_MS);
  state = xiaozhi_client_state(runtime->client);
  if (ret < 0 && state == XIAOZHI_STATE_BACKOFF)
    {
      pthread_mutex_lock(&runtime->lock);
      if (runtime->query_running || runtime->query_pending)
        {
          FAR const char *detail = cloud_client_last_error(runtime->cloud);
          mz_runtime_fail_dialogue_locked(
            runtime, ret,
            detail == NULL || detail[0] == '\0' ?
            "xiaozhi connection interrupted" : detail);
        }

      pthread_mutex_unlock(&runtime->lock);
    }

  pthread_mutex_lock(&runtime->lock);
  barge = runtime->ptt_barge_pending;
  pthread_mutex_unlock(&runtime->lock);
  state = xiaozhi_client_state(runtime->client);
  if (barge && state >= XIAOZHI_STATE_READY)
    {
      if (mz_runtime_barge_in(runtime) < 0)
        {
          return;
        }
    }

  pthread_mutex_lock(&runtime->lock);
  begin_voice = runtime->ptt_begin_pending;
  end_voice = runtime->ptt_end_pending;
  begin_text = runtime->query_pending;
  listening = runtime->ptt_listening;
  running = runtime->query_running;
  started_ms = runtime->recording_started_ms;
  deadline_ms = runtime->query_deadline_ms;
  pthread_mutex_unlock(&runtime->lock);

  state = xiaozhi_client_state(runtime->client);
  if (state == XIAOZHI_STATE_READY)
    {
      if (begin_voice)
        {
          mz_runtime_start_voice(runtime, now_ms);
        }
      else if (begin_text)
        {
          mz_runtime_start_text(runtime, now_ms);
        }
    }

  pthread_mutex_lock(&runtime->lock);
  end_voice = runtime->ptt_end_pending;
  listening = runtime->ptt_listening;
  started_ms = runtime->recording_started_ms;
  pthread_mutex_unlock(&runtime->lock);
  if (listening && started_ms != 0 &&
      now_ms - started_ms >= MZ_RUNTIME_PTT_LIMIT_MS)
    {
      end_voice = true;
    }

  if (end_voice && xiaozhi_client_state(runtime->client) ==
                   XIAOZHI_STATE_LISTENING)
    {
      mz_runtime_stop_voice(runtime, now_ms);
    }

  pthread_mutex_lock(&runtime->lock);
  running = runtime->query_running;
  deadline_ms = runtime->query_deadline_ms;
  if (running && deadline_ms != 0 && now_ms >= deadline_ms)
    {
      mz_runtime_fail_dialogue_locked(runtime, -ETIMEDOUT,
                                      "xiaozhi request timed out");
    }

  pthread_mutex_unlock(&runtime->lock);
}

static void mz_runtime_ai_shutdown(FAR struct mz_runtime_s *runtime)
{
  if (runtime->client != NULL &&
      xiaozhi_client_state(runtime->client) >= XIAOZHI_STATE_READY)
    {
      (void)xiaozhi_client_abort(runtime->client, "shutdown");
    }

  mz_runtime_client_destroy(runtime);
}

int mz_runtime_query(FAR void *context, FAR const char *question,
                     FAR struct mz_ai_result_s *result)
{
  FAR struct mz_runtime_s *runtime = context;

  (void)result;
  if (runtime == NULL || question == NULL || question[0] == '\0')
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&runtime->lock);
  if (!runtime->credentials_ready)
    {
      pthread_mutex_unlock(&runtime->lock);
      return -EACCES;
    }

  if (runtime->query_pending || runtime->query_running)
    {
      pthread_mutex_unlock(&runtime->lock);
      return -EBUSY;
    }

  mz_runtime_copy(runtime->query_question,
                  sizeof(runtime->query_question), question);
  runtime->query_pending = true;
  runtime->query_running = true;
  runtime->query_ready = false;
  runtime->query_status = -EINPROGRESS;
  runtime->dialogue_kind = MZ_DIALOGUE_NONE;
  runtime->client_abort_pending = false;
  runtime->dialogue_question[0] = '\0';
  runtime->dialogue_answer[0] = '\0';
  runtime->answer_from_llm = false;
  runtime->query_deadline_ms = mz_runtime_now_ms() +
                               MZ_RUNTIME_CONNECT_TIMEOUT_MS +
                               MZ_RUNTIME_QUERY_TIMEOUT_MS;
  if (runtime->ai_state != MZ_AI_READY)
    {
      runtime->ai_state = MZ_AI_CONNECTING;
    }
  else
    {
      runtime->ai_state = MZ_AI_THINKING;
    }

  runtime->ai_error = 0;
  runtime->ai_detail[0] = '\0';
  pthread_mutex_unlock(&runtime->lock);
  return -EINPROGRESS;
}

int mz_runtime_take_query_result(FAR struct mz_runtime_s *runtime,
                                 FAR struct mz_ai_result_s *result,
                                 FAR int *status)
{
  if (runtime == NULL || result == NULL || status == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&runtime->lock);
  if (!runtime->query_ready)
    {
      pthread_mutex_unlock(&runtime->lock);
      return -EAGAIN;
    }

  *result = runtime->query_result;
  *status = runtime->query_status;
  runtime->query_ready = false;
  memset(&runtime->query_result, 0, sizeof(runtime->query_result));
  pthread_mutex_unlock(&runtime->lock);
  return 0;
}

#else

struct mz_runtime_s
{
  int unused;
};

int mz_runtime_create(FAR struct mz_runtime_s **out_runtime,
                      FAR struct mz_platform_binding_s *binding)
{
  if (out_runtime != NULL)
    {
      *out_runtime = NULL;
    }

  if (binding != NULL)
    {
      memset(binding, 0, sizeof(*binding));
    }

  return -ENOSYS;
}

void mz_runtime_destroy(FAR struct mz_runtime_s *runtime)
{
  (void)runtime;
}

int mz_runtime_query(FAR void *context, FAR const char *question,
                     FAR struct mz_ai_result_s *result)
{
  (void)context;
  (void)question;
  (void)result;
  return -ENOSYS;
}

int mz_runtime_take_query_result(FAR struct mz_runtime_s *runtime,
                                 FAR struct mz_ai_result_s *result,
                                 FAR int *status)
{
  (void)runtime;
  (void)result;
  (void)status;
  return -ENOSYS;
}

#endif
