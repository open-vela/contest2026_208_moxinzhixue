/****************************************************************************
 * Contest 2026 team 208 - Wi-Fi service
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <netutils/netlib.h>
#include <wireless/wapi.h>

#ifdef CONFIG_KVDB
#  include <kvdb.h>
#endif

#include "gs1_wifi.h"

#define GS1_WIFI_LAST_SSID_KEY "persist.gs1.wifi.last_ssid"
#define GS1_WIFI_POLL_MS       250
#define GS1_WIFI_ASSOCIATE_MS  8000
#define GS1_WIFI_SCAN_POLL_MS  200
#define GS1_WIFI_SCAN_TRIES    25

static bool gs1_wifi_ap_present(FAR const struct ether_addr *ap)
{
  static const uint8_t zero[ETHER_ADDR_LEN];
  return memcmp(ap->ether_addr_octet, zero, sizeof(zero)) != 0;
}

static int gs1_wifi_network_find(FAR const struct gs1_wifi_scan_status_s *status,
                                 FAR const char *ssid)
{
  unsigned int i;

  for (i = 0; i < status->count; i++)
    {
      if (strcmp(status->networks[i].ssid, ssid) == 0)
        {
          return (int)i;
        }
    }

  return -1;
}

static void gs1_wifi_network_sort(FAR struct gs1_wifi_scan_status_s *status)
{
  struct gs1_wifi_network_s temporary;
  unsigned int i;
  unsigned int j;

  for (i = 1; i < status->count; i++)
    {
      temporary = status->networks[i];
      j = i;
      while (j > 0 && status->networks[j - 1].rssi < temporary.rssi)
        {
          status->networks[j] = status->networks[j - 1];
          j--;
        }

      status->networks[j] = temporary;
    }
}

static void gs1_wifi_network_add(FAR struct gs1_wifi_scan_status_s *status,
                                 FAR const struct wapi_scan_info_s *info)
{
  FAR struct gs1_wifi_network_s *network;
  int weakest = 0;
  int index;
  int rssi;
  unsigned int i;

  if (!info->has_essid || info->essid[0] == '\0' ||
      strlen(info->essid) > GS1_SSID_MAX)
    {
      return;
    }

  rssi = info->has_rssi ? info->rssi : INT16_MIN;
  index = gs1_wifi_network_find(status, info->essid);
  if (index < 0 && status->count < GS1_WIFI_SCAN_MAX)
    {
      index = status->count++;
    }
  else if (index < 0)
    {
      for (i = 1; i < status->count; i++)
        {
          if (status->networks[i].rssi < status->networks[weakest].rssi)
            {
              weakest = (int)i;
            }
        }

      if (rssi <= status->networks[weakest].rssi)
        {
          return;
        }

      index = weakest;
    }
  else if (rssi <= status->networks[index].rssi)
    {
      return;
    }

  network = &status->networks[index];
  memset(network, 0, sizeof(*network));
  snprintf(network->ssid, sizeof(network->ssid), "%s", info->essid);
  network->rssi = rssi < INT16_MIN ? INT16_MIN :
                  rssi > INT16_MAX ? INT16_MAX : (int16_t)rssi;
  network->secured = !info->has_encode ||
                     (info->encode & IW_ENCODE_DISABLED) == 0;
}

static int gs1_wifi_persist(FAR const struct wpa_wconfig_s *configuration)
{
#ifdef CONFIG_WIRELESS_WAPI_INITCONF
  char temporary[GS1_PATH_MAX];
  char parent[GS1_PATH_MAX];
  FAR char *slash;
  int fd;
  int ret;

  if (snprintf(temporary, sizeof(temporary), "%s.new",
               CONFIG_WIRELESS_WAPI_CONFIG_PATH) >= (int)sizeof(temporary))
    {
      return -ENAMETOOLONG;
    }

  snprintf(parent, sizeof(parent), "%s", CONFIG_WIRELESS_WAPI_CONFIG_PATH);
  slash = strrchr(parent, '/');
  if (slash != NULL)
    {
      *slash = '\0';
      for (slash = parent + 1; *slash != '\0'; slash++)
        {
          if (*slash == '/')
            {
              *slash = '\0';
              if (mkdir(parent, 0750) < 0 && errno != EEXIST)
                {
                  return -errno;
                }

              *slash = '/';
            }
        }

      if (mkdir(parent, 0750) < 0 && errno != EEXIST)
        {
          return -errno;
        }
    }

  ret = wapi_save_config(configuration->ifname, temporary, configuration);
  if (ret < 0)
    {
      unlink(temporary);
      return ret;
    }

  chmod(temporary, 0600);

  fd = open(temporary, O_RDONLY);
  if (fd >= 0)
    {
      fsync(fd);
      close(fd);
    }

  if (rename(temporary, CONFIG_WIRELESS_WAPI_CONFIG_PATH) < 0)
    {
      ret = -errno;
      unlink(temporary);
      return ret;
    }

  sync();
  return 0;
#else
  return -ENOSYS;
#endif
}

int gs1_wifi_query(FAR struct gs1_wifi_status_s *status)
{
  struct ether_addr ap;
  struct in_addr address;
  enum wapi_essid_flag_e essid_flag;
  int interface_up = 0;
  int socket_fd;
  int ret;

  if (status == NULL)
    {
      return -EINVAL;
    }

  memset(&ap, 0, sizeof(ap));
  memset(&address, 0, sizeof(address));
  memset(status->ssid, 0, sizeof(status->ssid));
  memset(status->ipv4, 0, sizeof(status->ipv4));
  snprintf(status->ifname, sizeof(status->ifname), "%s",
           CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_WIFI_IFNAME);

  socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0)
    {
      status->state = GS1_WIFI_ERROR;
      status->last_error = -errno;
      return status->last_error;
    }

  ret = wapi_get_ifup(socket_fd, status->ifname, &interface_up);
  if (ret < 0)
    {
      close(socket_fd);
      status->state = GS1_WIFI_ERROR;
      status->last_error = ret;
      return ret;
    }

  status->interface_up = interface_up != 0;
  if (!status->interface_up)
    {
      close(socket_fd);
      status->associated = false;
      status->has_ipv4 = false;
      status->state = GS1_WIFI_DOWN;
      status->last_error = 0;
      return 0;
    }

  if (wapi_get_essid(socket_fd, status->ifname, status->ssid,
                     &essid_flag) < 0)
    {
      status->ssid[0] = '\0';
    }

  status->associated = wapi_get_ap(socket_fd, status->ifname, &ap) == 0 &&
                       gs1_wifi_ap_present(&ap);
  status->has_ipv4 = wapi_get_ip(socket_fd, status->ifname, &address) == 0 &&
                     address.s_addr != INADDR_ANY;
  if (status->has_ipv4)
    {
      inet_ntop(AF_INET, &address, status->ipv4, sizeof(status->ipv4));
    }

  close(socket_fd);
  if (status->associated && status->has_ipv4)
    {
      status->state = GS1_WIFI_CONNECTED;
    }
  else if (status->ssid[0] != '\0' || status->associated)
    {
      status->state = GS1_WIFI_CONNECTING;
    }
  else
    {
      status->state = GS1_WIFI_DISCONNECTED;
    }

  status->last_error = 0;
  return 0;
}

int gs1_wifi_scan(FAR struct gs1_wifi_scan_status_s *status)
{
  struct wapi_list_s list;
  FAR struct wapi_scan_info_s *info;
  FAR const char *ifname =
    CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_WIFI_IFNAME;
  int interface_up = 0;
  int socket_fd;
  int tries;
  int ret;

  if (status == NULL)
    {
      return -EINVAL;
    }

  memset(status->networks, 0, sizeof(status->networks));
  status->count = 0;
  status->last_error = 0;
  status->state = GS1_WIFI_SCANNING;

  socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0)
    {
      ret = -errno;
      goto failed;
    }

  ret = wapi_get_ifup(socket_fd, ifname, &interface_up);
  if (ret < 0)
    {
      goto close_failed;
    }

  if (!interface_up)
    {
      ret = wapi_set_ifup(socket_fd, ifname);
      if (ret < 0)
        {
          goto close_failed;
        }
    }

  ret = wapi_escan_init(socket_fd, ifname, IW_SCAN_TYPE_ACTIVE, NULL);
  if (ret < 0)
    {
      goto close_failed;
    }

  for (tries = 0; tries < GS1_WIFI_SCAN_TRIES; tries++)
    {
      ret = wapi_scan_stat(socket_fd, ifname);
      if (ret <= 0)
        {
          break;
        }

      usleep(GS1_WIFI_SCAN_POLL_MS * 1000);
    }

  if (ret > 0)
    {
      ret = -ETIMEDOUT;
      goto close_failed;
    }

  if (ret < 0)
    {
      goto close_failed;
    }

  memset(&list, 0, sizeof(list));
  ret = wapi_scan_coll(socket_fd, ifname, &list);
  if (ret < 0)
    {
      goto close_failed;
    }

  for (info = list.head.scan; info != NULL; info = info->next)
    {
      gs1_wifi_network_add(status, info);
    }

  wapi_scan_coll_free(&list);
  close(socket_fd);
  gs1_wifi_network_sort(status);
  status->generation++;
  status->state = GS1_WIFI_SCAN_READY;
  return 0;

close_failed:
  close(socket_fd);
failed:
  status->generation++;
  status->last_error = ret;
  status->state = GS1_WIFI_SCAN_ERROR;
  return ret;
}

int gs1_wifi_connect(FAR const struct gs1_wifi_connect_request_s *request,
                     FAR struct gs1_wifi_status_s *status)
{
  struct wpa_wconfig_s configuration;
  int socket_fd;
  int elapsed;
  int ret;

  if (request == NULL || status == NULL || request->ssid[0] == '\0' ||
      strlen(request->ssid) > GS1_SSID_MAX ||
      strlen(request->passphrase) > GS1_PASSPHRASE_MAX)
    {
      return -EINVAL;
    }

  memset(&configuration, 0, sizeof(configuration));
  configuration.ifname =
    CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_WIFI_IFNAME;
  configuration.sta_mode = CONFIG_NETINIT_WAPI_STAMODE;
  configuration.ssid = request->ssid;
  configuration.ssidlen = strlen(request->ssid);
  configuration.passphrase = request->passphrase;
  configuration.phraselen = strlen(request->passphrase);
  configuration.flag = WAPI_FREQ_AUTO;
  if (configuration.phraselen == 0)
    {
      configuration.auth_wpa = IW_AUTH_WPA_VERSION_DISABLED;
      configuration.cipher_mode = IW_AUTH_CIPHER_NONE;
      configuration.alg = WPA_ALG_NONE;
    }
  else
    {
      configuration.auth_wpa = CONFIG_NETINIT_WAPI_AUTHWPA;
      configuration.cipher_mode = CONFIG_NETINIT_WAPI_CIPHERMODE;
      configuration.alg = CONFIG_NETINIT_WAPI_ALG;
    }

  status->state = GS1_WIFI_CONNECTING;
  status->last_error = 0;
  socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0)
    {
      ret = -errno;
      goto failed;
    }

  ret = wapi_set_ifup(socket_fd, configuration.ifname);
  close(socket_fd);
  if (ret < 0)
    {
      goto failed;
    }

  ret = wpa_driver_wext_associate(&configuration);
  if (ret < 0)
    {
      goto failed;
    }

  for (elapsed = 0; elapsed < GS1_WIFI_ASSOCIATE_MS;
       elapsed += GS1_WIFI_POLL_MS)
    {
      usleep(GS1_WIFI_POLL_MS * 1000);
      ret = gs1_wifi_query(status);
      if (ret == 0 && status->associated)
        {
          break;
        }
    }

  if (!status->associated)
    {
      ret = -ETIMEDOUT;
      goto failed;
    }

  ret = netlib_obtain_ipv4addr(configuration.ifname);
  if (ret < 0)
    {
      goto failed;
    }

  ret = gs1_wifi_query(status);
  if (ret < 0 || !status->has_ipv4)
    {
      ret = ret < 0 ? ret : -ENETUNREACH;
      goto failed;
    }

  if (request->persist)
    {
      ret = gs1_wifi_persist(&configuration);
      if (ret < 0)
        {
          goto failed;
        }
    }

#ifdef CONFIG_KVDB
  property_set_oneway(GS1_WIFI_LAST_SSID_KEY, request->ssid);
  property_commit();
#endif

  status->last_error = 0;
  return 0;

failed:
  if (status->state != GS1_WIFI_CONNECTED)
    {
      status->state = GS1_WIFI_ERROR;
    }

  status->last_error = ret;
  return ret;
}

int gs1_wifi_disconnect(FAR struct gs1_wifi_status_s *status)
{
  struct in_addr address;
  int socket_fd;

  if (status == NULL)
    {
      return -EINVAL;
    }

  socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0)
    {
      status->last_error = -errno;
      return status->last_error;
    }

  wpa_driver_wext_disconnect(
    socket_fd, CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_WIFI_IFNAME);
  address.s_addr = INADDR_ANY;
  wapi_set_ip(socket_fd,
              CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_WIFI_IFNAME,
              &address);
  close(socket_fd);
  usleep(GS1_WIFI_POLL_MS * 1000);
  return gs1_wifi_query(status);
}
