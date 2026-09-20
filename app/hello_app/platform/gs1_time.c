/****************************************************************************
 * Contest 2026 team 208 - clock/NTP service
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include <netutils/ntpclient.h>

#ifdef CONFIG_KVDB
#  include <kvdb.h>
#endif

#include "gs1_time.h"

#define GS1_VALID_TIME_EPOCH 1704067200LL /* 2024-01-01 UTC */
#define GS1_LAST_SYNC_KEY    "persist.gs1.time.last_sync"

static struct ntpc_status_s g_ntp_baseline;
static bool g_last_sync_loaded;
static bool g_ntp_owned;
static bool g_network_ready;
static bool g_waiting_for_new_sample;
static uint32_t g_restarts_this_sync;
static uint64_t g_sync_started_ms;
static uint64_t g_attempt_started_ms;
static int g_ntp_pid = -1;

static uint64_t gs1_time_now_ms(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000 +
         (uint64_t)now.tv_nsec / 1000000;
}

static uint32_t gs1_time_elapsed(uint64_t started, uint64_t now)
{
  uint64_t elapsed;

  if (started == 0 || now <= started)
    {
      return 0;
    }

  elapsed = now - started;
  return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static bool gs1_time_sample_address_equal(
  FAR const struct ntpc_status_s *left,
  FAR const struct ntpc_status_s *right, unsigned int index)
{
  return memcmp(&left->samples[index]._srv_addr_store,
                &right->samples[index]._srv_addr_store,
                sizeof(struct sockaddr_storage)) == 0;
}

static bool gs1_time_has_fresh_samples(
  FAR const struct ntpc_status_s *current)
{
  unsigned int index;

  if (current->nsamples == 0)
    {
      return false;
    }

  if (!g_waiting_for_new_sample ||
      current->nsamples != g_ntp_baseline.nsamples)
    {
      return true;
    }

  for (index = 0; index < current->nsamples; index++)
    {
      if (current->samples[index].offset !=
            g_ntp_baseline.samples[index].offset ||
          current->samples[index].delay !=
            g_ntp_baseline.samples[index].delay ||
          !gs1_time_sample_address_equal(current, &g_ntp_baseline, index))
        {
          return true;
        }
    }

  return false;
}

static void gs1_time_server_text(FAR const struct sockaddr *address,
                                 FAR char *server, size_t capacity)
{
  if (capacity == 0)
    {
      return;
    }

  server[0] = '\0';
  if (address == NULL)
    {
      return;
    }

  if (address->sa_family == AF_INET)
    {
      FAR const struct sockaddr_in *ipv4 =
        (FAR const struct sockaddr_in *)address;

      inet_ntop(AF_INET, &ipv4->sin_addr, server, capacity);
    }
#ifdef CONFIG_NET_IPv6
  else if (address->sa_family == AF_INET6)
    {
      FAR const struct sockaddr_in6 *ipv6 =
        (FAR const struct sockaddr_in6 *)address;

      inet_ntop(AF_INET6, &ipv6->sin6_addr, server, capacity);
    }
#endif
}

static void gs1_time_capture_baseline(void)
{
  memset(&g_ntp_baseline, 0, sizeof(g_ntp_baseline));
  ntpc_status(&g_ntp_baseline);
  g_waiting_for_new_sample = true;
}

static int gs1_time_start_daemon(FAR struct gs1_time_status_s *status,
                                 bool restart)
{
  uint64_t now;
  int ret;

  gs1_time_capture_baseline();
  ntpc_stop();
  ret = ntpc_start();
  status->ntp_start_count++;
  if (restart)
    {
      status->ntp_restart_count++;
    }

  status->last_start_error = ret < 0 ? ret : 0;
  if (ret < 0)
    {
      status->state = GS1_TIME_ERROR;
      status->daemon_owned = false;
      status->ntp_pid = -1;
      status->last_error = ret;
      g_ntp_owned = false;
      g_ntp_pid = -1;
      return ret;
    }

  now = gs1_time_now_ms();
  if (!restart || g_sync_started_ms == 0)
    {
      g_sync_started_ms = now;
    }

  g_attempt_started_ms = now;
  g_ntp_owned = true;
  g_ntp_pid = ret;
  status->state = GS1_TIME_SYNCING;
  status->daemon_owned = true;
  status->ntp_pid = ret;
  status->ntp_samples = 0;
  status->sync_elapsed_ms = gs1_time_elapsed(g_sync_started_ms, now);
  status->first_sample_offset = 0;
  status->first_sample_delay = 0;
  status->first_server[0] = '\0';
  status->last_error = 0;
  return 0;
}

int gs1_time_query(FAR struct gs1_time_status_s *status)
{
  struct ntpc_status_s ntp_status;
  struct timespec realtime;
  struct timespec monotonic;
  uint64_t now_ms;
  bool fresh_samples;
  bool was_synced;
  int ret;

  if (status == NULL)
    {
      return -EINVAL;
    }

  was_synced = status->state == GS1_TIME_SYNCED &&
               status->last_sync_sec > 0;
  if (clock_gettime(CLOCK_REALTIME, &realtime) < 0 ||
      clock_gettime(CLOCK_MONOTONIC, &monotonic) < 0)
    {
      status->state = GS1_TIME_ERROR;
      status->last_error = -errno;
      return status->last_error;
    }

  now_ms = (uint64_t)monotonic.tv_sec * 1000 +
           (uint64_t)monotonic.tv_nsec / 1000000;
  status->realtime_sec = realtime.tv_sec;
  status->monotonic_sec = monotonic.tv_sec;
  status->realtime_valid = realtime.tv_sec >= GS1_VALID_TIME_EPOCH;
  status->daemon_owned = g_ntp_owned;
  status->network_ready = g_network_ready;
  status->ntp_pid = g_ntp_pid;
  if (g_sync_started_ms != 0 && !was_synced)
    {
      status->sync_elapsed_ms =
        gs1_time_elapsed(g_sync_started_ms, now_ms);
    }

#ifdef CONFIG_KVDB
  if (!g_last_sync_loaded)
    {
      property_get_int64_with_err(GS1_LAST_SYNC_KEY,
                                  &status->last_sync_sec);
      g_last_sync_loaded = true;
    }
#endif

  memset(&ntp_status, 0, sizeof(ntp_status));
  ret = ntpc_status(&ntp_status);
  if (ret < 0)
    {
      status->state = status->realtime_valid ? GS1_TIME_LOCAL :
                                               GS1_TIME_INVALID;
      status->last_error = ret;
      return ret;
    }

  fresh_samples = gs1_time_has_fresh_samples(&ntp_status);
  status->ntp_samples = fresh_samples ? ntp_status.nsamples : 0;
  if (fresh_samples && status->realtime_valid)
    {
      g_waiting_for_new_sample = false;
      status->state = GS1_TIME_SYNCED;
      status->last_sync_sec = realtime.tv_sec;
      status->sync_elapsed_ms =
        gs1_time_elapsed(g_sync_started_ms, now_ms);
      status->first_sample_offset = ntp_status.samples[0].offset;
      status->first_sample_delay = ntp_status.samples[0].delay;
      gs1_time_server_text(ntp_status.samples[0].srv_addr,
                           status->first_server,
                           sizeof(status->first_server));
      status->last_error = 0;

#ifdef CONFIG_KVDB
      if (!was_synced)
        {
          int64_t saved = 0;

          if (property_get_int64_with_err(GS1_LAST_SYNC_KEY, &saved) < 0 ||
              realtime.tv_sec - saved >= 3600)
            {
              property_set_int64(GS1_LAST_SYNC_KEY, realtime.tv_sec);
              property_commit();
            }
        }
#endif
    }
  else if (status->state == GS1_TIME_SYNCING)
    {
      if (g_attempt_started_ms != 0 &&
          now_ms - g_attempt_started_ms >=
            CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_NTP_RESTART_SECONDS *
            1000ull)
        {
          if (g_restarts_this_sync <
              CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_NTP_MAX_RESTARTS)
            {
              g_restarts_this_sync++;
              return gs1_time_start_daemon(status, true);
            }

          status->state = GS1_TIME_ERROR;
          status->last_error = GS1_ERROR_TIMEOUT;
          return status->last_error;
        }

      status->last_error = 0;
    }
  else
    {
      status->state = status->realtime_valid ? GS1_TIME_LOCAL :
                                               GS1_TIME_INVALID;
      status->last_error = 0;
    }

  return 0;
}

int gs1_time_start_sync(FAR struct gs1_time_status_s *status)
{
  if (status == NULL)
    {
      return -EINVAL;
    }

  g_restarts_this_sync = 0;
  g_sync_started_ms = 0;
  return gs1_time_start_daemon(status, false);
}

int gs1_time_network_update(FAR struct gs1_time_status_s *status,
                            bool ready, int error)
{
  bool became_ready;

  if (status == NULL)
    {
      return -EINVAL;
    }

  became_ready = ready && !g_network_ready;
  g_network_ready = ready;
  status->network_ready = ready;
  status->network_error = error;
  if (became_ready && status->state != GS1_TIME_SYNCED)
    {
      return gs1_time_start_sync(status);
    }

  return 0;
}

void gs1_time_shutdown(void)
{
  if (g_ntp_owned)
    {
      ntpc_stop();
    }

  memset(&g_ntp_baseline, 0, sizeof(g_ntp_baseline));
  g_last_sync_loaded = false;
  g_ntp_owned = false;
  g_network_ready = false;
  g_waiting_for_new_sample = false;
  g_restarts_this_sync = 0;
  g_sync_started_ms = 0;
  g_attempt_started_ms = 0;
  g_ntp_pid = -1;
}
