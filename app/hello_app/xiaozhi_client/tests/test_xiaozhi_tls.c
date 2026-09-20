#include "xiaozhi_tls.h"
#include "fake_curl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                   \
  do                                                                      \
    {                                                                     \
      if (!(condition))                                                   \
        {                                                                 \
          fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,     \
                  __LINE__, #condition);                                  \
          return -1;                                                      \
        }                                                                 \
    }                                                                     \
  while (0)

static int test_ca_selection(void)
{
  CHECK(xiaozhi_tls_select_ca(NULL, false) ==
        XIAOZHI_TLS_CA_EMBEDDED);
  CHECK(xiaozhi_tls_select_ca("", false) ==
        XIAOZHI_TLS_CA_EMBEDDED);
  CHECK(xiaozhi_tls_select_ca("/tmp/custom.pem", false) ==
        XIAOZHI_TLS_CA_FILE);
  CHECK(xiaozhi_tls_select_ca(NULL, true) == XIAOZHI_TLS_CA_NONE);
  CHECK(xiaozhi_tls_select_ca("/tmp/custom.pem", true) ==
        XIAOZHI_TLS_CA_NONE);
  return 0;
}

static int test_date_parsing(void)
{
  static const char date_header[] =
      "Date: Sun, 26 Jul 2026 07:11:28 GMT\r\n";
  static const char lowercase_header[] =
      "date:\tSun, 26 Jul 2026 07:11:28 GMT  \r\n";
  static const char invalid_header[] = "Date: not-a-date\r\n";
  static const char unrelated_header[] = "Server: unit-test\r\n";
  static const char old_header[] =
      "Date: Sun, 31 Dec 2023 23:59:59 GMT\r\n";
  static const char too_new_header[] =
      "Date: Sat, 16 Jan 2038 00:00:00 GMT\r\n";
  static const char missing_zone_header[] =
      "Date: Sun, 26 Jul 2026 07:11:28\r\n";
  static const char embedded_nul_header[] =
      "Date: Sun, 26 Jul 2026 07:11:28 GMT\0ignored\r\n";
  char nonterminated[sizeof(date_header) - 1];
  char oversized[128];
  time_t realtime = 0;

  memcpy(nonterminated, date_header, sizeof(nonterminated));
  memset(oversized, 'x', sizeof(oversized));
  memcpy(oversized, "Date: ", 6);

  CHECK(xiaozhi_tls_parse_date_header(date_header,
                                      strlen(date_header),
                                      &realtime) == 0);
  CHECK(realtime == (time_t)1785049888);
  CHECK(xiaozhi_tls_time_valid(realtime));
  CHECK(xiaozhi_tls_parse_date_header(lowercase_header,
                                      strlen(lowercase_header),
                                      &realtime) == 0);
  CHECK(realtime == (time_t)1785049888);
  CHECK(xiaozhi_tls_parse_date_header(nonterminated,
                                      sizeof(nonterminated),
                                      &realtime) == 0);
  CHECK(realtime == (time_t)1785049888);
  CHECK(xiaozhi_tls_parse_date_header(invalid_header,
                                      strlen(invalid_header),
                                      &realtime) == -EINVAL);
  CHECK(xiaozhi_tls_parse_date_header(unrelated_header,
                                      strlen(unrelated_header),
                                      &realtime) == -ENOENT);
  CHECK(xiaozhi_tls_parse_date_header(old_header, strlen(old_header),
                                      &realtime) == -ERANGE);
  CHECK(xiaozhi_tls_parse_date_header(too_new_header,
                                      strlen(too_new_header),
                                      &realtime) == -ERANGE);
  CHECK(xiaozhi_tls_parse_date_header(missing_zone_header,
                                      strlen(missing_zone_header),
                                      &realtime) == -EINVAL);
  CHECK(xiaozhi_tls_parse_date_header(embedded_nul_header,
                                      sizeof(embedded_nul_header) - 1,
                                      &realtime) == -EINVAL);
  CHECK(xiaozhi_tls_parse_date_header(oversized, sizeof(oversized),
                                      &realtime) == -EMSGSIZE);
  CHECK(xiaozhi_tls_parse_date_header(NULL, 0, &realtime) == -EINVAL);
  CHECK(!xiaozhi_tls_time_valid((time_t)0));
  CHECK(xiaozhi_tls_time_valid(XIAOZHI_TLS_VALID_TIME_EPOCH));
  return 0;
}

static int test_curl_options(void)
{
  CURL *curl;
  const char *pem;

  CHECK(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
  curl = curl_easy_init();
  CHECK(curl != NULL);
  CHECK(xiaozhi_tls_apply(curl, "", false) == CURLE_OK);
  CHECK(fake_curl_verify_peer(curl) == 1);
  CHECK(fake_curl_verify_host(curl) == 2);
  CHECK(fake_curl_capath_configured(curl));
  CHECK(fake_curl_capath(curl) == NULL);
  CHECK(fake_curl_cainfo_configured(curl));
  CHECK(fake_curl_cainfo(curl) == NULL);
  CHECK(fake_curl_blob_configured(curl));
  CHECK(fake_curl_blob_length(curl) == 1294);
  CHECK(fake_curl_blob_flags(curl) == CURL_BLOB_NOCOPY);
  pem = fake_curl_blob_data(curl);
  CHECK(pem != NULL);
  CHECK(strncmp(pem, "-----BEGIN CERTIFICATE-----", 27) == 0);
  CHECK(xiaozhi_tls_apply(curl, "/tmp/custom.pem", false) == CURLE_OK);
  CHECK(fake_curl_verify_peer(curl) == 1);
  CHECK(fake_curl_verify_host(curl) == 2);
  CHECK(strcmp(fake_curl_cainfo(curl), "/tmp/custom.pem") == 0);
  CHECK(!fake_curl_blob_configured(curl));
  CHECK(xiaozhi_tls_apply(curl, NULL, true) == CURLE_OK);
  CHECK(fake_curl_verify_peer(curl) == 0);
  CHECK(fake_curl_verify_host(curl) == 0);
  CHECK(fake_curl_cainfo(curl) == NULL);
  CHECK(!fake_curl_blob_configured(curl));
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  return 0;
}

int main(void)
{
  CHECK(test_ca_selection() == 0);
  CHECK(test_date_parsing() == 0);
  CHECK(test_curl_options() == 0);
  printf("xiaozhi TLS host tests: PASS\n");
  return EXIT_SUCCESS;
}
