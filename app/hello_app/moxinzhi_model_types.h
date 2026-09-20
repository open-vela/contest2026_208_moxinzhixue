/****************************************************************************
 * Contest 2026 team 208 - shared learning model value types
 ****************************************************************************/

#ifndef __MOXINZHI_MODEL_TYPES_H
#define __MOXINZHI_MODEL_TYPES_H

#include <stdint.h>

#include "moxinzhi_ai_service.h"

enum mz_card_state_e
{
  MZ_CARD_NEW = 0,
  MZ_CARD_MASTERED,
  MZ_CARD_REVIEW
};

struct mz_card_s
{
  uint32_t id;
  uint8_t state;
  uint8_t reserved[3];
  char question[MZ_QUESTION_MAX];
  char answer[MZ_ANSWER_MAX];
  char topic[MZ_TOPIC_MAX];
};

#endif /* __MOXINZHI_MODEL_TYPES_H */
