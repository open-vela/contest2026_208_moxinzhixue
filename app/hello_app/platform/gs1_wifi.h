/****************************************************************************
 * Contest 2026 team 208 - Wi-Fi service
 ****************************************************************************/

#ifndef __GS1_WIFI_H
#define __GS1_WIFI_H

#include "gs1_platform.h"

int gs1_wifi_query(FAR struct gs1_wifi_status_s *status);
int gs1_wifi_scan(FAR struct gs1_wifi_scan_status_s *status);
int gs1_wifi_connect(FAR const struct gs1_wifi_connect_request_s *request,
                     FAR struct gs1_wifi_status_s *status);
int gs1_wifi_disconnect(FAR struct gs1_wifi_status_s *status);

#endif /* __GS1_WIFI_H */
