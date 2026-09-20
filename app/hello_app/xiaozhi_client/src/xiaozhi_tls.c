#include "xiaozhi_tls.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__NuttX__)
#  include <nuttx/config.h>
#endif

#define XIAOZHI_TLS_CLOCK_URL \
  "http://api.tenclass.net/xiaozhi/ota/"
#define XIAOZHI_TLS_CLOCK_TIMEOUT_MS 15000
#define XIAOZHI_TLS_DATE_MAX 96
#define XIAOZHI_TLS_MAX_BOOTSTRAP_EPOCH ((time_t)2147169600)

#if defined(__NuttX__) && \
    defined(CONFIG_ARCH_BOARD_R528S3_GEMINI_S1)
#  define XIAOZHI_TLS_REUSE_STATIC_CRT 1
#endif

/* DigiCert Global Root G2
 * SHA-256 CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:
 *        47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F
 * Valid through 2038-01-15.  The server supplies its intermediate CA.
 */

static const char g_xiaozhi_digicert_global_root_g2[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
  "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
  "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
  "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
  "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
  "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
  "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
  "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
  "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
  "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
  "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
  "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
  "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
  "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
  "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
  "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
  "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
  "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
  "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
  "MrY=\n"
  "-----END CERTIFICATE-----\n";

#if defined(XIAOZHI_TLS_REUSE_STATIC_CRT)
/* The target's mbedTLS curl backend keeps one process-wide parsed CA chain.
 * Supplying the same blob on every new easy handle would append duplicates
 * indefinitely, so later handles reuse it after the first successful TLS
 * connection.  Pairing and dialogue connections are serialized by the app.
 */
static volatile bool g_xiaozhi_embedded_ca_loaded;
#endif

struct xiaozhi_tls_clock_response
{
  time_t realtime;
  bool has_realtime;
};

static void xiaozhi_tls_error(char *buffer, size_t capacity,
                              const char *format, ...)
{
  va_list arguments;

  if (buffer == NULL || capacity == 0)
    {
      return;
    }

  va_start(arguments, format);
  vsnprintf(buffer, capacity, format, arguments);
  va_end(arguments);
}

static bool xiaozhi_tls_ascii_equal(char left, char right)
{
  if (left >= 'A' && left <= 'Z')
    {
      left = (char)(left - 'A' + 'a');
    }

  if (right >= 'A' && right <= 'Z')
    {
      right = (char)(right - 'A' + 'a');
    }

  return left == right;
}

static bool xiaozhi_tls_header_name_is_date(const char *header,
                                            size_t length)
{
  static const char name[] = "Date:";
  size_t index;

  if (length < sizeof(name) - 1)
    {
      return false;
    }

  for (index = 0; index < sizeof(name) - 1; index++)
    {
      if (!xiaozhi_tls_ascii_equal(header[index], name[index]))
        {
          return false;
        }
    }

  return true;
}

enum xiaozhi_tls_ca_source
xiaozhi_tls_select_ca(const char *ca_file, bool allow_insecure_tls)
{
  if (allow_insecure_tls)
    {
      return XIAOZHI_TLS_CA_NONE;
    }

  return ca_file != NULL && ca_file[0] != '\0' ?
         XIAOZHI_TLS_CA_FILE : XIAOZHI_TLS_CA_EMBEDDED;
}

bool xiaozhi_tls_time_valid(time_t realtime)
{
  return realtime >= XIAOZHI_TLS_VALID_TIME_EPOCH;
}

int xiaozhi_tls_parse_date_header(const char *header, size_t length,
                                  time_t *realtime)
{
  char date[XIAOZHI_TLS_DATE_MAX];
  const char *begin;
  const char *end;
  time_t parsed;
  size_t date_length;

  if (header == NULL || realtime == NULL)
    {
      return -EINVAL;
    }

  if (!xiaozhi_tls_header_name_is_date(header, length))
    {
      return -ENOENT;
    }

  begin = header + 5;
  end = header + length;
  while (begin < end && (*begin == ' ' || *begin == '\t'))
    {
      begin++;
    }

  while (end > begin &&
         (end[-1] == '\r' || end[-1] == '\n' ||
          end[-1] == ' ' || end[-1] == '\t'))
    {
      end--;
    }

  date_length = (size_t)(end - begin);
  if (date_length == 0)
    {
      return -EINVAL;
    }

  if (date_length >= sizeof(date))
    {
      return -EMSGSIZE;
    }

  if (memchr(begin, '\0', date_length) != NULL)
    {
      return -EINVAL;
    }

  memcpy(date, begin, date_length);
  date[date_length] = '\0';
  parsed = curl_getdate(date, NULL);
  if (parsed == (time_t)-1)
    {
      return -EINVAL;
    }

  if (!xiaozhi_tls_time_valid(parsed) ||
      parsed > XIAOZHI_TLS_MAX_BOOTSTRAP_EPOCH)
    {
      return -ERANGE;
    }

  *realtime = parsed;
  return 0;
}

static size_t xiaozhi_tls_clock_header(void *data, size_t size,
                                       size_t count, void *user)
{
  struct xiaozhi_tls_clock_response *response = user;
  size_t length = size * count;
  time_t realtime;

  if (size != 0 && length / size != count)
    {
      return 0;
    }

  if (xiaozhi_tls_parse_date_header(data, length, &realtime) == 0)
    {
      response->realtime = realtime;
      response->has_realtime = true;
    }

  return length;
}

static size_t xiaozhi_tls_discard(void *data, size_t size,
                                  size_t count, void *user)
{
  size_t length = size * count;

  (void)data;
  (void)user;
  return size != 0 && length / size != count ? 0 : length;
}

static int xiaozhi_tls_set_realtime(time_t realtime)
{
#if !defined(__NuttX__)
  (void)realtime;
  return -ENOTSUP;
#else
  struct timespec value;

  value.tv_sec = realtime;
  value.tv_nsec = 0;
  if (clock_settime(CLOCK_REALTIME, &value) < 0)
    {
      return -errno;
    }

  return 0;
#endif
}

int xiaozhi_tls_ensure_realtime(uint32_t timeout_ms,
                                char *error_text,
                                size_t error_capacity)
{
  struct xiaozhi_tls_clock_response response;
  CURL *curl;
  CURLcode code;
  long http_status = 0;
  time_t now;
  int ret;

  if (error_text != NULL && error_capacity != 0)
    {
      error_text[0] = '\0';
    }

  now = time(NULL);
  if (xiaozhi_tls_time_valid(now))
    {
      return 0;
    }

  memset(&response, 0, sizeof(response));
  curl = curl_easy_init();
  if (curl == NULL)
    {
      xiaozhi_tls_error(error_text, error_capacity,
                        "HTTP clock bootstrap initialization failed");
      return -ENOMEM;
    }

  if (timeout_ms == 0)
    {
      timeout_ms = XIAOZHI_TLS_CLOCK_TIMEOUT_MS;
    }

#define XIAOZHI_TLS_SETOPT(option, value)                                \
  do                                                                     \
    {                                                                    \
      code = curl_easy_setopt(curl, option, value);                      \
      if (code != CURLE_OK)                                              \
        {                                                                \
          goto option_error;                                             \
        }                                                                \
    }                                                                    \
  while (0)

  XIAOZHI_TLS_SETOPT(CURLOPT_URL, XIAOZHI_TLS_CLOCK_URL);
  XIAOZHI_TLS_SETOPT(CURLOPT_HTTPGET, 1L);
  XIAOZHI_TLS_SETOPT(CURLOPT_HEADERFUNCTION, xiaozhi_tls_clock_header);
  XIAOZHI_TLS_SETOPT(CURLOPT_HEADERDATA, &response);
  XIAOZHI_TLS_SETOPT(CURLOPT_WRITEFUNCTION, xiaozhi_tls_discard);
  XIAOZHI_TLS_SETOPT(CURLOPT_WRITEDATA, NULL);
  XIAOZHI_TLS_SETOPT(CURLOPT_TIMEOUT_MS, (long)timeout_ms);
  XIAOZHI_TLS_SETOPT(CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
  XIAOZHI_TLS_SETOPT(CURLOPT_NOSIGNAL, 1L);
  XIAOZHI_TLS_SETOPT(CURLOPT_PROTOCOLS_STR, "http");
  XIAOZHI_TLS_SETOPT(CURLOPT_REDIR_PROTOCOLS_STR, "http");
  XIAOZHI_TLS_SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
  XIAOZHI_TLS_SETOPT(CURLOPT_USERAGENT, "moxinzhi-openvela-clock/1");

#undef XIAOZHI_TLS_SETOPT

  code = curl_easy_perform(curl);
  if (code != CURLE_OK)
    {
      curl_easy_cleanup(curl);
      xiaozhi_tls_error(error_text, error_capacity,
                        "HTTP clock bootstrap failed: %s",
                        curl_easy_strerror(code));
      return code == CURLE_OPERATION_TIMEDOUT ? -ETIMEDOUT : -EIO;
    }

  code = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_easy_cleanup(curl);
  if (code != CURLE_OK)
    {
      xiaozhi_tls_error(error_text, error_capacity,
                        "unable to read HTTP clock bootstrap status: %s",
                        curl_easy_strerror(code));
      return -EIO;
    }

  if (http_status != 200)
    {
      xiaozhi_tls_error(error_text, error_capacity,
                        "HTTP clock bootstrap returned HTTP %ld",
                        http_status);
      return -EPROTO;
    }

  if (!response.has_realtime)
    {
      xiaozhi_tls_error(error_text, error_capacity,
                        "HTTP clock bootstrap returned no Date header");
      return -ENODATA;
    }

  if (!xiaozhi_tls_time_valid(response.realtime))
    {
      xiaozhi_tls_error(error_text, error_capacity,
                        "HTTP clock bootstrap returned an invalid time");
      return -ERANGE;
    }

  ret = xiaozhi_tls_set_realtime(response.realtime);
  if (ret < 0 && !xiaozhi_tls_time_valid(time(NULL)))
    {
      xiaozhi_tls_error(error_text, error_capacity,
                        "unable to set realtime clock: %d", ret);
      return ret;
    }

  return 0;

option_error:
#undef XIAOZHI_TLS_SETOPT
  curl_easy_cleanup(curl);
  xiaozhi_tls_error(error_text, error_capacity,
                    "unable to configure HTTP clock bootstrap: %s",
                    curl_easy_strerror(code));
  return -EIO;
}

CURLcode xiaozhi_tls_apply(CURL *curl, const char *ca_file,
                           bool allow_insecure_tls)
{
  struct curl_blob blob;
  enum xiaozhi_tls_ca_source source;
  CURLcode code;

  if (curl == NULL)
    {
      return CURLE_BAD_FUNCTION_ARGUMENT;
    }

  code = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,
                          allow_insecure_tls ? 0L : 1L);
  if (code != CURLE_OK)
    {
      return code;
    }

  code = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,
                          allow_insecure_tls ? 0L : 2L);
  if (code != CURLE_OK)
    {
      return code;
    }

  source = xiaozhi_tls_select_ca(ca_file, allow_insecure_tls);
  code = curl_easy_setopt(curl, CURLOPT_CAPATH, NULL);
  if (code != CURLE_OK && code != CURLE_NOT_BUILT_IN)
    {
      return code;
    }

  code = curl_easy_setopt(curl, CURLOPT_CAINFO, NULL);
  if (code != CURLE_OK)
    {
      return code;
    }

  code = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, NULL);
  if (code != CURLE_OK && code != CURLE_NOT_BUILT_IN)
    {
      return code;
    }

  if (source == XIAOZHI_TLS_CA_NONE)
    {
      return CURLE_OK;
    }

  if (source == XIAOZHI_TLS_CA_FILE)
    {
      return curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    }

  if (source == XIAOZHI_TLS_CA_EMBEDDED)
    {
#if defined(XIAOZHI_TLS_REUSE_STATIC_CRT)
      if (g_xiaozhi_embedded_ca_loaded)
        {
          return CURLE_OK;
        }
#endif

      blob.data = (void *)g_xiaozhi_digicert_global_root_g2;
      blob.len = sizeof(g_xiaozhi_digicert_global_root_g2) - 1;
      blob.flags = CURL_BLOB_NOCOPY;
      return curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    }

  return CURLE_OK;
}

void xiaozhi_tls_note_connection_ready(const char *ca_file,
                                       bool allow_insecure_tls)
{
#if defined(XIAOZHI_TLS_REUSE_STATIC_CRT)
  if (xiaozhi_tls_select_ca(ca_file, allow_insecure_tls) ==
      XIAOZHI_TLS_CA_EMBEDDED)
    {
      g_xiaozhi_embedded_ca_loaded = true;
    }
#else
  (void)ca_file;
  (void)allow_insecure_tls;
#endif
}
