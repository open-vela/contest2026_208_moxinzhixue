/****************************************************************************
 * Contest 2026 team 208 - AI service boundary
 ****************************************************************************/

#ifndef __MOXINZHI_AI_SERVICE_H
#define __MOXINZHI_AI_SERVICE_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>

#define MZ_PROMPT_COUNT 3
#define MZ_QUESTION_MAX_CHARS 64
#define MZ_QUESTION_MAX       257
#define MZ_ANSWER_MAX         769
#define MZ_TOPIC_MAX          64

struct mz_ai_result_s
{
  char question[MZ_QUESTION_MAX];
  char answer[MZ_ANSWER_MAX];
  char topic[MZ_TOPIC_MAX];
};

typedef int (*mz_ai_query_t)(FAR void *context, FAR const char *question,
                             FAR struct mz_ai_result_s *result);

struct mz_ai_service_s
{
  mz_ai_query_t query;
  FAR void *context;
};

void mz_ai_service_init_local(FAR struct mz_ai_service_s *service);
void mz_ai_service_set_provider(FAR struct mz_ai_service_s *service,
                                mz_ai_query_t query, FAR void *context);
int mz_ai_service_ask(FAR struct mz_ai_service_s *service,
                      FAR const char *question,
                      FAR struct mz_ai_result_s *result);
FAR const char *mz_ai_service_prompt(unsigned int index, bool chinese);

#endif /* __MOXINZHI_AI_SERVICE_H */
