/****************************************************************************
 * Contest 2026 team 208 - product platform and XiaoZhi runtime
 ****************************************************************************/

#ifndef __MOXINZHI_RUNTIME_H
#define __MOXINZHI_RUNTIME_H

#include <nuttx/config.h>

#include "moxinzhi_ai_service.h"
#include "moxinzhi_platform.h"

struct mz_runtime_s;

int mz_runtime_create(FAR struct mz_runtime_s **out_runtime,
                      FAR struct mz_platform_binding_s *binding);
void mz_runtime_destroy(FAR struct mz_runtime_s *runtime);
int mz_runtime_query(FAR void *context, FAR const char *question,
                     FAR struct mz_ai_result_s *result);
int mz_runtime_take_query_result(FAR struct mz_runtime_s *runtime,
                                 FAR struct mz_ai_result_s *result,
                                 FAR int *status);

#endif /* __MOXINZHI_RUNTIME_H */
