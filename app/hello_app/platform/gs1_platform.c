/****************************************************************************
 * Contest 2026 team 208 - Gemini-S1 platform service
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "gs1_audio.h"
#include "gs1_platform.h"
#include "gs1_storage.h"
#include "gs1_thread.h"
#include "gs1_time.h"
#include "gs1_wifi.h"

struct gs1_platform_message_s
{
  uint32_t request_id;
  struct gs1_platform_request_s request;
};

_Static_assert(sizeof(struct gs1_platform_message_s) <=
               GS1_THREAD_PAYLOAD_MAX,
               "platform request exceeds worker queue payload");

struct gs1_platform_context_s
{
  pthread_mutex_t lock;
  struct gs1_thread_queue_s worker;
  struct gs1_audio_context_s audio;
  struct gs1_platform_status_s status;
  uint32_t next_request_id;
  uint16_t time_idle_ticks;
  uint16_t wifi_idle_ticks;
  uint16_t storage_idle_ticks;
  bool initialized;
};

static struct gs1_platform_context_s g_platform;

static void gs1_platform_generation_locked(void)
{
  g_platform.status.generation++;
  gs1_thread_depth(&g_platform.worker,
                   &g_platform.status.runtime.queue_depth);
}

static void gs1_platform_set_storage(
    FAR const struct gs1_storage_status_s *status)
{
  pthread_mutex_lock(&g_platform.lock);
  g_platform.status.storage = *status;
  gs1_platform_generation_locked();
  pthread_mutex_unlock(&g_platform.lock);
}

static void gs1_platform_set_wifi(FAR const struct gs1_wifi_status_s *status)
{
  pthread_mutex_lock(&g_platform.lock);
  g_platform.status.wifi = *status;
  gs1_platform_generation_locked();
  pthread_mutex_unlock(&g_platform.lock);
}

static void gs1_platform_set_wifi_scan(
    FAR const struct gs1_wifi_scan_status_s *status)
{
  pthread_mutex_lock(&g_platform.lock);
  g_platform.status.wifi_scan = *status;
  gs1_platform_generation_locked();
  pthread_mutex_unlock(&g_platform.lock);
}

static void gs1_platform_set_audio(
    FAR const struct gs1_audio_status_s *status)
{
  pthread_mutex_lock(&g_platform.lock);
  g_platform.status.audio = *status;
  gs1_platform_generation_locked();
  pthread_mutex_unlock(&g_platform.lock);
}

static void gs1_platform_set_time(FAR const struct gs1_time_status_s *status)
{
  pthread_mutex_lock(&g_platform.lock);
  g_platform.status.time = *status;
  gs1_platform_generation_locked();
  pthread_mutex_unlock(&g_platform.lock);
}

static void gs1_platform_complete(uint32_t request_id, int error)
{
  pthread_mutex_lock(&g_platform.lock);
  g_platform.status.runtime.last_completed_request_id = request_id;
  g_platform.status.runtime.last_error = error;
  gs1_platform_generation_locked();
  pthread_mutex_unlock(&g_platform.lock);
}

static int gs1_platform_refresh(void)
{
  struct gs1_storage_status_s storage;
  struct gs1_wifi_status_s wifi;
  struct gs1_audio_status_s audio;
  struct gs1_time_status_s time_status;
  int ret = 0;
  int current;

  pthread_mutex_lock(&g_platform.lock);
  storage = g_platform.status.storage;
  wifi = g_platform.status.wifi;
  audio = g_platform.status.audio;
  time_status = g_platform.status.time;
  pthread_mutex_unlock(&g_platform.lock);

  current = gs1_storage_query(&storage);
  if (current < 0 && ret == 0)
    {
      ret = current;
    }

  gs1_platform_set_storage(&storage);
  current = gs1_wifi_query(&wifi);
  if (current < 0 && ret == 0)
    {
      ret = current;
    }

  gs1_platform_set_wifi(&wifi);
  current = gs1_time_network_update(
    &time_status, wifi.state == GS1_WIFI_CONNECTED && wifi.has_ipv4,
    wifi.last_error);
  if (current < 0 && ret == 0)
    {
      ret = current;
    }

  gs1_platform_set_time(&time_status);
  gs1_audio_tick(&g_platform.audio, &audio);
  gs1_platform_set_audio(&audio);
  current = gs1_time_query(&time_status);
  if (current < 0 && ret == 0)
    {
      ret = current;
    }

  gs1_platform_set_time(&time_status);
  return ret;
}

static int gs1_platform_worker_message(FAR void *arg,
                                       FAR const void *payload,
                                       size_t payload_size)
{
  FAR const struct gs1_platform_message_s *message = payload;
  struct gs1_storage_status_s storage;
  struct gs1_wifi_status_s wifi;
  struct gs1_wifi_scan_status_s wifi_scan;
  struct gs1_audio_status_s audio;
  struct gs1_time_status_s time_status;
  int ret = -EINVAL;

  (void)arg;
  if (payload_size != sizeof(*message))
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_platform.lock);
  if (g_platform.status.runtime.queue_depth > 0)
    {
      g_platform.status.runtime.queue_depth--;
    }

  storage = g_platform.status.storage;
  wifi = g_platform.status.wifi;
  wifi_scan = g_platform.status.wifi_scan;
  audio = g_platform.status.audio;
  time_status = g_platform.status.time;
  pthread_mutex_unlock(&g_platform.lock);

  switch (message->request.op)
    {
      case GS1_REQUEST_REFRESH:
        ret = gs1_platform_refresh();
        break;

      case GS1_REQUEST_STORAGE_PROBE:
        storage.state = GS1_STORAGE_CHECKING;
        storage.last_error = 0;
        gs1_platform_set_storage(&storage);
        ret = gs1_storage_probe(&storage);
        gs1_platform_set_storage(&storage);
        break;

      case GS1_REQUEST_WIFI_SCAN:
        wifi_scan.state = GS1_WIFI_SCANNING;
        wifi_scan.count = 0;
        wifi_scan.last_error = 0;
        gs1_platform_set_wifi_scan(&wifi_scan);
        ret = gs1_wifi_scan(&wifi_scan);
        gs1_platform_set_wifi_scan(&wifi_scan);
        break;

      case GS1_REQUEST_WIFI_CONNECT:
        wifi.state = GS1_WIFI_CONNECTING;
        wifi.last_error = 0;
        gs1_platform_set_wifi(&wifi);
        ret = gs1_wifi_connect(&message->request.data.wifi_connect, &wifi);
        gs1_platform_set_wifi(&wifi);
        gs1_time_network_update(
          &time_status, wifi.state == GS1_WIFI_CONNECTED && wifi.has_ipv4,
          wifi.last_error);
        gs1_platform_set_time(&time_status);
        break;

      case GS1_REQUEST_WIFI_DISCONNECT:
        ret = gs1_wifi_disconnect(&wifi);
        gs1_platform_set_wifi(&wifi);
        gs1_time_network_update(&time_status, false, wifi.last_error);
        gs1_platform_set_time(&time_status);
        break;

      case GS1_REQUEST_AUDIO_RECORD_START:
        ret = gs1_audio_record_start(&g_platform.audio,
                                     &message->request.data.audio_record,
                                     &audio);
        gs1_platform_set_audio(&audio);
        break;

      case GS1_REQUEST_AUDIO_RECORD_STOP:
        ret = gs1_audio_record_stop(&g_platform.audio, &audio);
        gs1_platform_set_audio(&audio);
        break;

      case GS1_REQUEST_AUDIO_PLAY:
        ret = gs1_audio_play(&g_platform.audio,
                             &message->request.data.audio_play, &audio);
        gs1_platform_set_audio(&audio);
        break;

      case GS1_REQUEST_AUDIO_STOP:
        ret = gs1_audio_stop(&g_platform.audio, &audio);
        gs1_platform_set_audio(&audio);
        break;

      case GS1_REQUEST_AUDIO_SET_VOLUME:
        ret = gs1_audio_set_volume(
          &g_platform.audio, message->request.data.audio_volume.percent);
        break;

      case GS1_REQUEST_TIME_SYNC:
        time_status.state = GS1_TIME_SYNCING;
        time_status.last_error = 0;
        gs1_platform_set_time(&time_status);
        ret = gs1_time_start_sync(&time_status);
        gs1_platform_set_time(&time_status);
        break;

      default:
        ret = -EINVAL;
        break;
    }

  gs1_platform_complete(message->request_id, ret);
  return ret;
}

static void gs1_platform_worker_idle(FAR void *arg)
{
  struct gs1_storage_status_s storage;
  struct gs1_wifi_status_s wifi;
  struct gs1_audio_status_s audio;
  struct gs1_time_status_s time_status;
  struct gs1_audio_status_s previous_audio;

  (void)arg;
  pthread_mutex_lock(&g_platform.lock);
  storage = g_platform.status.storage;
  wifi = g_platform.status.wifi;
  audio = g_platform.status.audio;
  time_status = g_platform.status.time;
  pthread_mutex_unlock(&g_platform.lock);

  previous_audio = audio;
  gs1_audio_tick(&g_platform.audio, &audio);
  if (memcmp(&previous_audio, &audio, sizeof(audio)) != 0)
    {
      gs1_platform_set_audio(&audio);
    }

  g_platform.time_idle_ticks++;
  if (g_platform.time_idle_ticks >= 50)
    {
      g_platform.time_idle_ticks = 0;
      gs1_time_query(&time_status);
      gs1_platform_set_time(&time_status);
    }

  g_platform.wifi_idle_ticks++;
  if (g_platform.wifi_idle_ticks >= 100)
    {
      g_platform.wifi_idle_ticks = 0;
      gs1_wifi_query(&wifi);
      gs1_platform_set_wifi(&wifi);
      gs1_time_network_update(
        &time_status, wifi.state == GS1_WIFI_CONNECTED && wifi.has_ipv4,
        wifi.last_error);
      gs1_platform_set_time(&time_status);
    }

  g_platform.storage_idle_ticks++;
  if (g_platform.storage_idle_ticks >= 1500)
    {
      g_platform.storage_idle_ticks = 0;
      gs1_storage_query(&storage);
      gs1_platform_set_storage(&storage);
    }
}

static void gs1_platform_worker_stop(FAR void *arg)
{
  struct gs1_audio_status_s audio;

  (void)arg;
  pthread_mutex_lock(&g_platform.lock);
  audio = g_platform.status.audio;
  pthread_mutex_unlock(&g_platform.lock);
  gs1_audio_shutdown(&g_platform.audio, &audio);
  gs1_platform_set_audio(&audio);
  gs1_time_shutdown();

  pthread_mutex_lock(&g_platform.lock);
  g_platform.status.runtime.state = GS1_WORKER_STOPPED;
  g_platform.status.runtime.queue_depth = 0;
  g_platform.status.generation++;
  pthread_mutex_unlock(&g_platform.lock);
}

static bool gs1_platform_request_valid(
    FAR const struct gs1_platform_request_s *request)
{
  if (request == NULL)
    {
      return false;
    }

  switch (request->op)
    {
      case GS1_REQUEST_REFRESH:
      case GS1_REQUEST_STORAGE_PROBE:
      case GS1_REQUEST_WIFI_SCAN:
      case GS1_REQUEST_WIFI_DISCONNECT:
      case GS1_REQUEST_AUDIO_RECORD_STOP:
      case GS1_REQUEST_AUDIO_STOP:
      case GS1_REQUEST_TIME_SYNC:
        return true;

      case GS1_REQUEST_AUDIO_SET_VOLUME:
        return request->data.audio_volume.percent <= 100;

      case GS1_REQUEST_WIFI_CONNECT:
        return memchr(request->data.wifi_connect.ssid, '\0',
                      sizeof(request->data.wifi_connect.ssid)) != NULL &&
               memchr(request->data.wifi_connect.passphrase, '\0',
                      sizeof(request->data.wifi_connect.passphrase)) != NULL &&
               request->data.wifi_connect.ssid[0] != '\0';

      case GS1_REQUEST_AUDIO_RECORD_START:
        return memchr(request->data.audio_record.basename, '\0',
                      sizeof(request->data.audio_record.basename)) != NULL &&
               request->data.audio_record.sample_rate > 0 &&
               request->data.audio_record.channels > 0 &&
               request->data.audio_record.bits_per_sample > 0;

      case GS1_REQUEST_AUDIO_PLAY:
        return memchr(request->data.audio_play.path, '\0',
                      sizeof(request->data.audio_play.path)) != NULL &&
               request->data.audio_play.path[0] != '\0' &&
               request->data.audio_play.sample_rate > 0 &&
               request->data.audio_play.channels > 0 &&
               request->data.audio_play.bits_per_sample > 0;

      default:
        return false;
    }
}

int gs1_platform_init(void)
{
  struct gs1_thread_callbacks_s callbacks;
  struct gs1_platform_request_s request;
  int ret;

  if (g_platform.initialized)
    {
      return GS1_ERROR_ALREADY_STARTED;
    }

  memset(&g_platform, 0, sizeof(g_platform));
  pthread_mutex_init(&g_platform.lock, NULL);
  g_platform.status.runtime.state = GS1_WORKER_STOPPED;
  g_platform.status.storage.state = GS1_STORAGE_UNKNOWN;
  g_platform.status.wifi.state = GS1_WIFI_UNKNOWN;
  g_platform.status.wifi_scan.state = GS1_WIFI_SCAN_IDLE;
  g_platform.status.time.state = GS1_TIME_UNKNOWN;
  snprintf(g_platform.status.storage.root,
           sizeof(g_platform.status.storage.root), "%s",
           CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_ROOT);
  snprintf(g_platform.status.wifi.ifname,
           sizeof(g_platform.status.wifi.ifname), "%s",
           CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_WIFI_IFNAME);
  gs1_audio_initialize(&g_platform.audio, &g_platform.status.audio);

  callbacks.message = gs1_platform_worker_message;
  callbacks.idle = gs1_platform_worker_idle;
  callbacks.stop = gs1_platform_worker_stop;
  ret = gs1_thread_start(
    &g_platform.worker, "gs1svc",
    CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_WORKER_STACKSIZE,
    20, &callbacks, &g_platform);
  if (ret < 0)
    {
      gs1_audio_shutdown(&g_platform.audio, &g_platform.status.audio);
      pthread_mutex_destroy(&g_platform.lock);
      return ret;
    }

  g_platform.initialized = true;
  g_platform.status.runtime.state = GS1_WORKER_RUNNING;
  memset(&request, 0, sizeof(request));
  request.op = GS1_REQUEST_STORAGE_PROBE;
  gs1_platform_request(&request, NULL);
  request.op = GS1_REQUEST_REFRESH;
  gs1_platform_request(&request, NULL);
  return 0;
}

int gs1_platform_shutdown(void)
{
  int ret;

  if (!g_platform.initialized)
    {
      return GS1_ERROR_NOT_STARTED;
    }

  ret = gs1_thread_stop(&g_platform.worker);
  pthread_mutex_destroy(&g_platform.lock);
  memset(&g_platform, 0, sizeof(g_platform));
  return ret;
}

int gs1_platform_request(FAR const struct gs1_platform_request_s *request,
                         FAR uint32_t *request_id)
{
  struct gs1_platform_message_s message;
  int ret;

  if (!g_platform.initialized)
    {
      return GS1_ERROR_NOT_STARTED;
    }

  pthread_mutex_lock(&g_platform.lock);
  if (g_platform.status.runtime.state != GS1_WORKER_RUNNING)
    {
      pthread_mutex_unlock(&g_platform.lock);
      return GS1_ERROR_INVALID_STATE;
    }

  pthread_mutex_unlock(&g_platform.lock);

  if (!gs1_platform_request_valid(request))
    {
      return -EINVAL;
    }

  memset(&message, 0, sizeof(message));
  pthread_mutex_lock(&g_platform.lock);
  message.request_id = ++g_platform.next_request_id;
  if (message.request_id == 0)
    {
      message.request_id = ++g_platform.next_request_id;
    }

  g_platform.status.runtime.last_queued_request_id = message.request_id;
  g_platform.status.runtime.queue_depth++;
  g_platform.status.generation++;
  pthread_mutex_unlock(&g_platform.lock);

  message.request = *request;
  ret = gs1_thread_post(&g_platform.worker, &message, sizeof(message));
  if (ret < 0)
    {
      pthread_mutex_lock(&g_platform.lock);
      if (g_platform.status.runtime.queue_depth > 0)
        {
          g_platform.status.runtime.queue_depth--;
        }

      g_platform.status.runtime.last_error = ret;
      g_platform.status.generation++;
      pthread_mutex_unlock(&g_platform.lock);
      return ret == -EAGAIN ? GS1_ERROR_QUEUE_FULL : ret;
    }

  if (request_id != NULL)
    {
      *request_id = message.request_id;
    }

  return 0;
}

int gs1_platform_get_status(FAR struct gs1_platform_status_s *status)
{
  if (status == NULL)
    {
      return -EINVAL;
    }

  if (!g_platform.initialized)
    {
      return GS1_ERROR_NOT_STARTED;
    }

  pthread_mutex_lock(&g_platform.lock);
  *status = g_platform.status;
  pthread_mutex_unlock(&g_platform.lock);
  return 0;
}

int gs1_platform_audio_stream_read(FAR void *buffer, size_t capacity,
                                   FAR size_t *bytes_read)
{
  if (!g_platform.initialized)
    {
      return GS1_ERROR_NOT_STARTED;
    }

  return gs1_audio_stream_read(&g_platform.audio, buffer, capacity,
                               bytes_read);
}

FAR const char *gs1_platform_error_string(int error)
{
  switch (error)
    {
      case 0:
        return "ok";
      case GS1_ERROR_NOT_STARTED:
        return "platform service not started";
      case GS1_ERROR_ALREADY_STARTED:
        return "platform service already started";
      case GS1_ERROR_QUEUE_FULL:
        return "platform service queue full";
      case GS1_ERROR_INVALID_STATE:
        return "invalid service state";
      case GS1_ERROR_TIMEOUT:
        return "operation timed out";
      case GS1_ERROR_UNSAFE_PATH:
        return "path is outside the allowed data area";
      case GS1_ERROR_DATA_CORRUPT:
        return "persistent data verification failed";
      default:
        return error < 0 && error > -2000 ? strerror(-error) :
                                           "unknown platform error";
    }
}
