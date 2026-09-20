/****************************************************************************
 * Contest 2026 team 208 - local learning model
 ****************************************************************************/

#ifndef __MOXINZHI_MODEL_H
#define __MOXINZHI_MODEL_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "moxinzhi_anki.h"
#include "moxinzhi_ai_service.h"
#include "moxinzhi_model_types.h"

#define MZ_CARD_CAPACITY 6

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
  uint32_t active_cards;
  uint32_t mastered_cards;
  uint32_t review_cards;
  uint16_t imported_cards;
  bool imported_truncated;
  uint8_t daily_goal;
};

struct mz_model_s
{
  struct mz_ai_service_s ai;
  struct mz_anki_catalog_s anki;
  struct mz_model_meta_s meta;
  struct mz_card_s cards[MZ_CARD_CAPACITY];
  struct mz_ai_result_s latest;
  bool has_latest;
  bool latest_saved;
  bool ai_pending;
  bool ai_voice_pending;
  int storage_status;
  int anki_status;
  int ai_status;
};

int mz_model_init(FAR struct mz_model_s *model);
int mz_model_ask(FAR struct mz_model_s *model, FAR const char *question);
int mz_model_begin_voice(FAR struct mz_model_s *model);
void mz_model_cancel_ai(FAR struct mz_model_s *model);
int mz_model_complete_ai(FAR struct mz_model_s *model, int status,
                         FAR const struct mz_ai_result_s *result);
int mz_model_save_latest(FAR struct mz_model_s *model);
int mz_model_rate_current(FAR struct mz_model_s *model, bool mastered);
int mz_model_advance_card(FAR struct mz_model_s *model);
int mz_model_set_daily_goal(FAR struct mz_model_s *model, uint8_t goal);
int mz_model_reload_anki(FAR struct mz_model_s *model);
int mz_model_reset(FAR struct mz_model_s *model);
FAR const struct mz_card_s *mz_model_current_card(
  FAR const struct mz_model_s *model);
void mz_model_get_stats(FAR const struct mz_model_s *model,
                        FAR struct mz_stats_s *stats);

#endif /* __MOXINZHI_MODEL_H */
