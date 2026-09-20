#ifndef XIAOZHI_TEST_FAKE_CURL_H
#define XIAOZHI_TEST_FAKE_CURL_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct fake_curl CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;

#define CURLE_OK 0
#define CURLE_UNSUPPORTED_PROTOCOL 1
#define CURLE_FAILED_INIT 2
#define CURLE_NOT_BUILT_IN 4
#define CURLE_OUT_OF_MEMORY 27
#define CURLE_OPERATION_TIMEDOUT 28
#define CURLE_BAD_FUNCTION_ARGUMENT 43

#define CURL_GLOBAL_DEFAULT 3L

#define CURL_BLOB_NOCOPY 0
#define CURL_BLOB_COPY 1

struct curl_blob
{
  void *data;
  size_t len;
  unsigned int flags;
};

enum
{
  CURLOPT_URL = 1,
  CURLOPT_HTTPGET,
  CURLOPT_HEADERFUNCTION,
  CURLOPT_HEADERDATA,
  CURLOPT_WRITEFUNCTION,
  CURLOPT_WRITEDATA,
  CURLOPT_TIMEOUT_MS,
  CURLOPT_CONNECTTIMEOUT_MS,
  CURLOPT_NOSIGNAL,
  CURLOPT_PROTOCOLS_STR,
  CURLOPT_REDIR_PROTOCOLS_STR,
  CURLOPT_FOLLOWLOCATION,
  CURLOPT_USERAGENT,
  CURLOPT_SSL_VERIFYPEER,
  CURLOPT_SSL_VERIFYHOST,
  CURLOPT_CAPATH,
  CURLOPT_CAINFO,
  CURLOPT_CAINFO_BLOB
};

#define CURLINFO_RESPONSE_CODE 1

CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *curl);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *curl);
CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...);
const char *curl_easy_strerror(CURLcode code);
time_t curl_getdate(const char *date, const time_t *now);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_TEST_FAKE_CURL_H */
