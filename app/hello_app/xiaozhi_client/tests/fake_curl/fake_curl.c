#include "fake_curl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fake_curl
{
  long verify_peer;
  long verify_host;
  bool capath_configured;
  const char *capath;
  bool cainfo_configured;
  const char *cainfo;
  bool blob_configured;
  struct curl_blob blob;
  long response_code;
};

static bool fake_ascii_equal(char left, char right)
{
  return tolower((unsigned char)left) == tolower((unsigned char)right);
}

static bool fake_word_equal(const char *left, const char *right)
{
  while (*left != '\0' && *right != '\0')
    {
      if (!fake_ascii_equal(*left++, *right++))
        {
          return false;
        }
    }

  return *left == '\0' && *right == '\0';
}

static int fake_month_number(const char *month)
{
  static const char *const names[] =
  {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  int index;

  for (index = 0; index < 12; index++)
    {
      if (fake_word_equal(month, names[index]))
        {
          return index + 1;
        }
    }

  return 0;
}

static bool fake_leap_year(int year)
{
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int fake_days_in_month(int year, int month)
{
  static const unsigned char days[] =
  {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };

  return month == 2 && fake_leap_year(year) ? 29 : days[month - 1];
}

static int64_t fake_days_from_civil(int year, int month, int day)
{
  int adjusted_year = year - (month <= 2);
  int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) /
            400;
  unsigned int year_of_era =
      (unsigned int)(adjusted_year - era * 400);
  unsigned int day_of_year =
      (unsigned int)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 +
                     day - 1);
  unsigned int day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
      day_of_year;

  return (int64_t)era * 146097 + day_of_era - 719468;
}

CURLcode curl_global_init(long flags)
{
  (void)flags;
  return CURLE_OK;
}

void curl_global_cleanup(void)
{
}

CURL *curl_easy_init(void)
{
  CURL *curl = calloc(1, sizeof(*curl));

  if (curl != NULL)
    {
      curl->response_code = 200;
    }

  return curl;
}

void curl_easy_cleanup(CURL *curl)
{
  free(curl);
}

CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
  va_list arguments;

  if (curl == NULL)
    {
      return CURLE_BAD_FUNCTION_ARGUMENT;
    }

  va_start(arguments, option);
  switch (option)
    {
      case CURLOPT_SSL_VERIFYPEER:
        curl->verify_peer = va_arg(arguments, long);
        break;
      case CURLOPT_SSL_VERIFYHOST:
        curl->verify_host = va_arg(arguments, long);
        break;
      case CURLOPT_CAPATH:
        curl->capath_configured = true;
        curl->capath = va_arg(arguments, const char *);
        break;
      case CURLOPT_CAINFO:
        curl->cainfo_configured = true;
        curl->cainfo = va_arg(arguments, const char *);
        break;
      case CURLOPT_CAINFO_BLOB:
        {
          const struct curl_blob *blob =
              va_arg(arguments, const struct curl_blob *);

          curl->blob_configured = blob != NULL;
          if (blob == NULL)
            {
              memset(&curl->blob, 0, sizeof(curl->blob));
            }
          else
            {
              curl->blob = *blob;
            }
        }
        break;
      default:
        break;
    }

  va_end(arguments);
  return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *curl)
{
  return curl == NULL ? CURLE_BAD_FUNCTION_ARGUMENT : CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
  va_list arguments;
  long *value;

  if (curl == NULL || info != CURLINFO_RESPONSE_CODE)
    {
      return CURLE_BAD_FUNCTION_ARGUMENT;
    }

  va_start(arguments, info);
  value = va_arg(arguments, long *);
  va_end(arguments);
  if (value == NULL)
    {
      return CURLE_BAD_FUNCTION_ARGUMENT;
    }

  *value = curl->response_code;
  return CURLE_OK;
}

const char *curl_easy_strerror(CURLcode code)
{
  switch (code)
    {
      case CURLE_OK:
        return "No error";
      case CURLE_OPERATION_TIMEDOUT:
        return "Operation timed out";
      case CURLE_OUT_OF_MEMORY:
        return "Out of memory";
      default:
        return "Fake curl error";
    }
}

time_t curl_getdate(const char *date, const time_t *now)
{
  char weekday[4];
  char month_text[4];
  char zone[4];
  int day;
  int month;
  int year;
  int hour;
  int minute;
  int second;
  int consumed = 0;
  int64_t seconds;
  time_t result;

  (void)now;
  if (date == NULL ||
      sscanf(date, "%3[A-Za-z], %d %3[A-Za-z] %d %d:%d:%d "
                   "%3[A-Za-z] %n",
             weekday, &day, month_text, &year, &hour, &minute,
             &second, zone, &consumed) != 8)
    {
      return (time_t)-1;
    }

  while (date[consumed] != '\0' &&
         isspace((unsigned char)date[consumed]))
    {
      consumed++;
    }

  month = fake_month_number(month_text);
  if (date[consumed] != '\0' || !fake_word_equal(zone, "GMT") ||
      month == 0 || year < 1970 || day < 1 ||
      day > fake_days_in_month(year, month) || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 60)
    {
      return (time_t)-1;
    }

  seconds = fake_days_from_civil(year, month, day) * 86400 +
            hour * 3600 + minute * 60 + second;
  result = (time_t)seconds;
  return (int64_t)result == seconds ? result : (time_t)-1;
}

long fake_curl_verify_peer(const CURL *curl)
{
  return curl->verify_peer;
}

long fake_curl_verify_host(const CURL *curl)
{
  return curl->verify_host;
}

bool fake_curl_capath_configured(const CURL *curl)
{
  return curl->capath_configured;
}

const char *fake_curl_capath(const CURL *curl)
{
  return curl->capath;
}

bool fake_curl_cainfo_configured(const CURL *curl)
{
  return curl->cainfo_configured;
}

const char *fake_curl_cainfo(const CURL *curl)
{
  return curl->cainfo;
}

bool fake_curl_blob_configured(const CURL *curl)
{
  return curl->blob_configured;
}

const void *fake_curl_blob_data(const CURL *curl)
{
  return curl->blob.data;
}

size_t fake_curl_blob_length(const CURL *curl)
{
  return curl->blob.len;
}

unsigned int fake_curl_blob_flags(const CURL *curl)
{
  return curl->blob.flags;
}
