/****************************************************************************
 * Contest 2026 team 208 - local P0 AI service implementation
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "moxinzhi_ai_service.h"

struct mz_local_answer_s
{
  FAR const char *question_zh;
  FAR const char *question_en;
  FAR const char *answer_zh;
  FAR const char *answer_en;
  FAR const char *topic_zh;
  FAR const char *topic_en;
};

static const struct mz_local_answer_s g_local_answers[MZ_PROMPT_COUNT] =
{
  {
    "为什么天空是蓝色的？",
    "Why is the sky blue?",
    "阳光进入大气后，波长较短的蓝光更容易被空气分子散射，所以天空看起来是蓝色的。",
    "Air molecules scatter short blue wavelengths more strongly than other "
    "visible light, so the sky looks blue.",
    "物理",
    "Physics"
  },
  {
    "用一句话解释光合作用。",
    "Explain photosynthesis.",
    "植物利用光能，把二氧化碳和水转化为有机物，并释放氧气。",
    "Plants use light to turn carbon dioxide and water into stored chemical "
    "energy, releasing oxygen.",
    "生物",
    "Biology"
  },
  {
    "12 和 18 的最大公因数是多少？",
    "What is the GCD of 12 and 18?",
    "答案是 6。12 的因数有 1、2、3、4、6、12，18 的因数中最大的共同项是 6。",
    "The answer is 6. It is the largest number that divides both 12 and 18 without a remainder.",
    "数学",
    "Math"
  }
};

static void mz_copy_text(FAR char *target, size_t target_size,
                         FAR const char *source)
{
  if (target_size > 0)
    {
      snprintf(target, target_size, "%s", source);
    }
}

static int mz_local_query(FAR void *context, FAR const char *question,
                          FAR struct mz_ai_result_s *result)
{
  bool chinese;
  unsigned int i;

  (void)context;

  if (question == NULL || result == NULL)
    {
      return -EINVAL;
    }

  for (i = 0; i < MZ_PROMPT_COUNT; i++)
    {
      if (strcmp(question, g_local_answers[i].question_zh) == 0 ||
          strcmp(question, g_local_answers[i].question_en) == 0)
        {
          chinese = strcmp(question, g_local_answers[i].question_zh) == 0;
          memset(result, 0, sizeof(*result));
          mz_copy_text(result->question, sizeof(result->question), question);
          mz_copy_text(result->answer, sizeof(result->answer),
                       chinese ? g_local_answers[i].answer_zh :
                                 g_local_answers[i].answer_en);
          mz_copy_text(result->topic, sizeof(result->topic),
                       chinese ? g_local_answers[i].topic_zh :
                                 g_local_answers[i].topic_en);
          return 0;
        }
    }

  return -ENODATA;
}

void mz_ai_service_init_local(FAR struct mz_ai_service_s *service)
{
  mz_ai_service_set_provider(service, mz_local_query, NULL);
}

void mz_ai_service_set_provider(FAR struct mz_ai_service_s *service,
                                mz_ai_query_t query, FAR void *context)
{
  if (service != NULL)
    {
      service->query = query;
      service->context = context;
    }
}

int mz_ai_service_ask(FAR struct mz_ai_service_s *service,
                      FAR const char *question,
                      FAR struct mz_ai_result_s *result)
{
  if (service == NULL || service->query == NULL)
    {
      return -ENOSYS;
    }

  return service->query(service->context, question, result);
}

FAR const char *mz_ai_service_prompt(unsigned int index, bool chinese)
{
  if (index >= MZ_PROMPT_COUNT)
    {
      return "";
    }

  return chinese ? g_local_answers[index].question_zh :
                   g_local_answers[index].question_en;
}
