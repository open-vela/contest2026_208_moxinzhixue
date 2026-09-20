/****************************************************************************
 * Contest 2026 team 208 - clock/NTP service
 ****************************************************************************/

#ifndef __GS1_TIME_H
#define __GS1_TIME_H

#include "gs1_platform.h"

int gs1_time_query(FAR struct gs1_time_status_s *status);
int gs1_time_start_sync(FAR struct gs1_time_status_s *status);
int gs1_time_network_update(FAR struct gs1_time_status_s *status,
                            bool ready, int error);
void gs1_time_shutdown(void);

#endif /* __GS1_TIME_H */
