/****************************************************************************
 * Contest 2026 team 208 - local learning model
 ****************************************************************************/

#ifndef __MOXINZHI_MODEL_H
#define __MOXINZHI_MODEL_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "moxinzhi_ai_service.h"

#define MZ_CARD_CAPACITY 6

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

struct mz_model_meta_s
{
  uint32_t next_id;
  uint32_t ask_count;
  uint32_t saved_total;
  uint32_t mastered_actions;
  uint32_t review_actions;
  uint8_t card_count;
  uint8_t current_slot;
  uint8_t next_write_slot;
  uint8_t daily_goal;
};

struct mz_stats_s
{
  uint32_t ask_count;
  uint32_t saved_total;
  uint32_t mastered_actions;
  uint32_t review_actions;
  uint8_t active_cards;
  uint8_t mastered_cards;
  uint8_t review_cards;
  uint8_t daily_goal;
};

struct mz_model_s
{
  struct mz_ai_service_s ai;
  struct mz_model_meta_s meta;
  struct mz_card_s cards[MZ_CARD_CAPACITY];
  struct mz_ai_result_s latest;
  bool has_latest;
  bool latest_saved;
  int storage_status;
};

int mz_model_init(FAR struct mz_model_s *model);
int mz_model_ask(FAR struct mz_model_s *model, FAR const char *question);
int mz_model_save_latest(FAR struct mz_model_s *model);
int mz_model_rate_current(FAR struct mz_model_s *model, bool mastered);
int mz_model_advance_card(FAR struct mz_model_s *model);
int mz_model_set_daily_goal(FAR struct mz_model_s *model, uint8_t goal);
int mz_model_reset(FAR struct mz_model_s *model);
FAR const struct mz_card_s *mz_model_current_card(
  FAR const struct mz_model_s *model);
void mz_model_get_stats(FAR const struct mz_model_s *model,
                        FAR struct mz_stats_s *stats);

#endif /* __MOXINZHI_MODEL_H */
