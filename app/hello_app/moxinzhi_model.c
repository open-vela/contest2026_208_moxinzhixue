/****************************************************************************
 * Contest 2026 team 208 - local learning model
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "moxinzhi_model.h"
#include "moxinzhi_storage.h"

static void mz_model_defaults(FAR struct mz_model_s *model)
{
  memset(&model->meta, 0, sizeof(model->meta));
  memset(model->cards, 0, sizeof(model->cards));
  model->meta.next_id = 1;
  model->meta.daily_goal = 3;
}

static void mz_model_reconcile(FAR struct mz_model_s *model)
{
  uint8_t count = 0;
  uint8_t first = 0;
  uint8_t slot;

  for (slot = 0; slot < MZ_CARD_CAPACITY; slot++)
    {
      if (model->cards[slot].id != 0)
        {
          if (count == 0)
            {
              first = slot;
            }

          count++;
        }
    }

  model->meta.card_count = count;
  if (count == 0 || model->meta.current_slot >= MZ_CARD_CAPACITY ||
      model->cards[model->meta.current_slot].id == 0)
    {
      model->meta.current_slot = first;
    }

  if (model->meta.next_write_slot >= MZ_CARD_CAPACITY)
    {
      model->meta.next_write_slot = 0;
    }

  if (model->meta.next_id == 0)
    {
      model->meta.next_id = 1;
    }

  if (model->meta.daily_goal != 3 && model->meta.daily_goal != 5 &&
      model->meta.daily_goal != 10)
    {
      model->meta.daily_goal = 3;
    }
}

static int mz_model_save_meta(FAR struct mz_model_s *model)
{
  int ret = mz_storage_save_meta(&model->meta);

  model->storage_status = ret;
  return ret;
}

int mz_model_init(FAR struct mz_model_s *model)
{
  int ret;

  if (model == NULL)
    {
      return -EINVAL;
    }

  memset(model, 0, sizeof(*model));
  mz_ai_service_init_local(&model->ai);
  mz_model_defaults(model);

  ret = mz_storage_load(&model->meta, model->cards);
  if (ret == -ENOENT)
    {
      ret = 0;
    }

  model->storage_status = ret;
  mz_model_reconcile(model);
  return ret;
}

int mz_model_ask(FAR struct mz_model_s *model, FAR const char *question)
{
  int ret;

  if (model == NULL || question == NULL)
    {
      return -EINVAL;
    }

  ret = mz_ai_service_ask(&model->ai, question, &model->latest);
  if (ret < 0)
    {
      return ret;
    }

  model->has_latest = true;
  model->latest_saved = false;
  model->meta.ask_count++;
  return mz_model_save_meta(model);
}

int mz_model_save_latest(FAR struct mz_model_s *model)
{
  FAR struct mz_card_s *card;
  uint8_t slot;
  int ret;

  if (model == NULL || !model->has_latest)
    {
      return -ENODATA;
    }

  if (model->latest_saved)
    {
      return -EALREADY;
    }

  slot = model->meta.next_write_slot;
  card = &model->cards[slot];
  memset(card, 0, sizeof(*card));
  card->id = model->meta.next_id++;
  card->state = MZ_CARD_NEW;
  snprintf(card->question, sizeof(card->question), "%s",
           model->latest.question);
  snprintf(card->answer, sizeof(card->answer), "%s", model->latest.answer);
  snprintf(card->topic, sizeof(card->topic), "%s", model->latest.topic);

  ret = mz_storage_save_card(slot, card);
  if (ret < 0)
    {
      model->storage_status = ret;
      memset(card, 0, sizeof(*card));
      return ret;
    }

  if (model->meta.card_count < MZ_CARD_CAPACITY)
    {
      model->meta.card_count++;
    }

  model->meta.current_slot = slot;
  model->meta.next_write_slot = (slot + 1) % MZ_CARD_CAPACITY;
  model->meta.saved_total++;
  model->latest_saved = true;
  return mz_model_save_meta(model);
}

int mz_model_rate_current(FAR struct mz_model_s *model, bool mastered)
{
  FAR struct mz_card_s *card;
  uint8_t slot;
  int ret;

  if (model == NULL)
    {
      return -EINVAL;
    }

  slot = model->meta.current_slot;
  card = slot < MZ_CARD_CAPACITY ? &model->cards[slot] : NULL;
  if (card == NULL || card->id == 0)
    {
      return -ENODATA;
    }

  card->state = mastered ? MZ_CARD_MASTERED : MZ_CARD_REVIEW;
  if (mastered)
    {
      model->meta.mastered_actions++;
    }
  else
    {
      model->meta.review_actions++;
    }

  ret = mz_storage_save_card(slot, card);
  if (ret < 0)
    {
      model->storage_status = ret;
      return ret;
    }

  ret = mz_model_save_meta(model);
  if (ret >= 0)
    {
      mz_model_advance_card(model);
    }

  return ret;
}

int mz_model_advance_card(FAR struct mz_model_s *model)
{
  uint8_t start;
  uint8_t offset;
  uint8_t slot;

  if (model == NULL || model->meta.card_count == 0)
    {
      return -ENODATA;
    }

  start = model->meta.current_slot;
  for (offset = 1; offset <= MZ_CARD_CAPACITY; offset++)
    {
      slot = (start + offset) % MZ_CARD_CAPACITY;
      if (model->cards[slot].id != 0)
        {
          model->meta.current_slot = slot;
          return mz_model_save_meta(model);
        }
    }

  return -ENODATA;
}

int mz_model_set_daily_goal(FAR struct mz_model_s *model, uint8_t goal)
{
  if (model == NULL || (goal != 3 && goal != 5 && goal != 10))
    {
      return -EINVAL;
    }

  model->meta.daily_goal = goal;
  return mz_model_save_meta(model);
}

int mz_model_reset(FAR struct mz_model_s *model)
{
  int ret;

  if (model == NULL)
    {
      return -EINVAL;
    }

  ret = mz_storage_reset();
  mz_model_defaults(model);
  memset(&model->latest, 0, sizeof(model->latest));
  model->has_latest = false;
  model->latest_saved = false;
  model->storage_status = ret;
  return ret;
}

FAR const struct mz_card_s *mz_model_current_card(
  FAR const struct mz_model_s *model)
{
  uint8_t slot;

  if (model == NULL)
    {
      return NULL;
    }

  slot = model->meta.current_slot;
  if (slot >= MZ_CARD_CAPACITY || model->cards[slot].id == 0)
    {
      return NULL;
    }

  return &model->cards[slot];
}

void mz_model_get_stats(FAR const struct mz_model_s *model,
                        FAR struct mz_stats_s *stats)
{
  uint8_t slot;

  memset(stats, 0, sizeof(*stats));
  if (model == NULL)
    {
      return;
    }

  stats->ask_count = model->meta.ask_count;
  stats->saved_total = model->meta.saved_total;
  stats->mastered_actions = model->meta.mastered_actions;
  stats->review_actions = model->meta.review_actions;
  stats->active_cards = model->meta.card_count;
  stats->daily_goal = model->meta.daily_goal;

  for (slot = 0; slot < MZ_CARD_CAPACITY; slot++)
    {
      if (model->cards[slot].state == MZ_CARD_MASTERED &&
          model->cards[slot].id != 0)
        {
          stats->mastered_cards++;
        }
      else if (model->cards[slot].state == MZ_CARD_REVIEW &&
               model->cards[slot].id != 0)
        {
          stats->review_cards++;
        }
    }
}
