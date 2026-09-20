#ifndef XIAOZHI_TEST_FAKE_CURL_INSPECT_H
#define XIAOZHI_TEST_FAKE_CURL_INSPECT_H

#include <stdbool.h>
#include <stddef.h>

#include <curl/curl.h>

long fake_curl_verify_peer(const CURL *curl);
long fake_curl_verify_host(const CURL *curl);
bool fake_curl_capath_configured(const CURL *curl);
const char *fake_curl_capath(const CURL *curl);
bool fake_curl_cainfo_configured(const CURL *curl);
const char *fake_curl_cainfo(const CURL *curl);
bool fake_curl_blob_configured(const CURL *curl);
const void *fake_curl_blob_data(const CURL *curl);
size_t fake_curl_blob_length(const CURL *curl);
unsigned int fake_curl_blob_flags(const CURL *curl);

#endif /* XIAOZHI_TEST_FAKE_CURL_INSPECT_H */
