#ifndef XIAOZHI_TLS_H
#define XIAOZHI_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <curl/curl.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define XIAOZHI_TLS_VALID_TIME_EPOCH ((time_t)1704067200)

enum xiaozhi_tls_ca_source
{
  XIAOZHI_TLS_CA_NONE = 0,
  XIAOZHI_TLS_CA_FILE,
  XIAOZHI_TLS_CA_EMBEDDED
};

enum xiaozhi_tls_ca_source
xiaozhi_tls_select_ca(const char *ca_file, bool allow_insecure_tls);

bool xiaozhi_tls_time_valid(time_t realtime);

int xiaozhi_tls_parse_date_header(const char *header, size_t length,
                                  time_t *realtime);

int xiaozhi_tls_ensure_realtime(uint32_t timeout_ms,
                                char *error_text,
                                size_t error_capacity);

CURLcode xiaozhi_tls_apply(CURL *curl, const char *ca_file,
                           bool allow_insecure_tls);

void xiaozhi_tls_note_connection_ready(const char *ca_file,
                                       bool allow_insecure_tls);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_TLS_H */
