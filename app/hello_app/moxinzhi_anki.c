/****************************************************************************
 * Contest 2026 team 208 - Anki card catalog bridge
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "moxinzhi_anki.h"
#include "moxinzhi_text.h"

#ifdef CONFIG_MOXINZHI_ANKI_IMPORTER

#define MZ_ANKI_VISIT_STOP 1
#define MZ_ANKI_DAY_MS     (24ull * 60ull * 60ull * 1000ull)
#define MZ_ANKI_RETRY_MS   (10ull * 60ull * 1000ull)

static void mz_anki_copy(FAR char *target, size_t capacity,
                         FAR const char *source)
{
  mz_text_copy_utf8(target, capacity, source);
}

static uint64_t mz_anki_now_ms(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_REALTIME, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000ull +
         (uint64_t)now.tv_nsec / 1000000ull;
}

static int mz_anki_collect(FAR const char *fingerprint,
                           FAR const struct card_model_card_s *card,
                           FAR void *arg)
{
  FAR struct mz_anki_catalog_s *catalog = arg;
  FAR struct mz_anki_ref_s *ref;

  if (catalog->cached_count >= MZ_ANKI_CACHE_CAPACITY)
    {
      catalog->truncated = true;
      return MZ_ANKI_VISIT_STOP;
    }

  ref = &catalog->refs[catalog->cached_count++];
  mz_anki_copy(ref->fingerprint, sizeof(ref->fingerprint), fingerprint);
  ref->source_card_id = card->source_card_id;
  ref->review_count = card->review_count;
  ref->correct_count = card->correct_count;
  return 0;
}

static int mz_anki_load_current(FAR struct mz_anki_catalog_s *catalog)
{
  struct card_model_card_s card;
  FAR struct card_model_s *model = NULL;
  FAR struct mz_anki_ref_s *ref;
  void *allocation = NULL;
  int ret;

  catalog->view_ready = false;
  memset(&catalog->view, 0, sizeof(catalog->view));
  if (catalog->cached_count == 0)
    {
      return -ENODATA;
    }

  if (catalog->current >= catalog->cached_count)
    {
      catalog->current = 0;
    }

  ref = &catalog->refs[catalog->current];
  ret = card_model_open(CONFIG_MOXINZHI_CARD_STORE_PATH, &model);
  if (ret < 0)
    {
      if (ret == -ENOENT)
        {
          catalog->last_error = 0;
          return 0;
        }

      catalog->last_error = ret;
      return ret;
    }

  memset(&card, 0, sizeof(card));
  ret = card_model_get_card(model, ref->fingerprint,
                            ref->source_card_id, &card, &allocation);
  if (ret >= 0)
    {
      catalog->view.id = (uint32_t)(card.source_card_id ^
                                    (card.source_card_id >> 32));
      if (catalog->view.id == 0)
        {
          catalog->view.id = (uint32_t)catalog->current + 1;
        }

      if (card.review_count == 0)
        {
          catalog->view.state = MZ_CARD_NEW;
        }
      else if (card.correct_count == card.review_count)
        {
          catalog->view.state = MZ_CARD_MASTERED;
        }
      else
        {
          catalog->view.state = MZ_CARD_REVIEW;
        }

      mz_anki_copy(catalog->view.question,
                   sizeof(catalog->view.question), card.front);
      mz_anki_copy(catalog->view.answer,
                   sizeof(catalog->view.answer), card.back);
      mz_anki_copy(catalog->view.topic, sizeof(catalog->view.topic),
                   card.deck_name == NULL || card.deck_name[0] == '\0' ?
                     "Anki" : card.deck_name);
      catalog->view_ready = true;
      catalog->last_error = 0;
    }
  else
    {
      catalog->last_error = ret;
    }

  free(allocation);
  card_model_close(model);
  return ret;
}

int mz_anki_init(FAR struct mz_anki_catalog_s *catalog)
{
  if (catalog == NULL)
    {
      return -EINVAL;
    }

  memset(catalog, 0, sizeof(*catalog));
  return mz_anki_reload(catalog);
}

int mz_anki_reload(FAR struct mz_anki_catalog_s *catalog)
{
  FAR struct card_model_s *model = NULL;
  int ret;

  if (catalog == NULL)
    {
      return -EINVAL;
    }

  memset(catalog->refs, 0, sizeof(catalog->refs));
  memset(&catalog->view, 0, sizeof(catalog->view));
  catalog->cached_count = 0;
  catalog->current = 0;
  catalog->truncated = false;
  catalog->view_ready = false;

  ret = card_model_open(CONFIG_MOXINZHI_CARD_STORE_PATH, &model);
  if (ret < 0)
    {
      if (ret == -ENOENT)
        {
          catalog->last_error = 0;
          return 0;
        }

      catalog->last_error = ret;
      return ret;
    }

  ret = card_model_foreach_card(model, mz_anki_collect, catalog);
  card_model_close(model);
  if (ret == MZ_ANKI_VISIT_STOP)
    {
      ret = 0;
    }

  if (ret < 0)
    {
      catalog->last_error = ret;
      return ret;
    }

  if (catalog->cached_count == 0)
    {
      catalog->last_error = 0;
      return 0;
    }

  return mz_anki_load_current(catalog);
}

int mz_anki_rate_current(FAR struct mz_anki_catalog_s *catalog,
                         bool mastered)
{
  FAR struct card_model_s *model = NULL;
  FAR struct mz_anki_ref_s *ref;
  uint64_t next_due_ms;
  int ret;

  if (catalog == NULL || catalog->cached_count == 0 ||
      catalog->current >= catalog->cached_count)
    {
      return -ENODATA;
    }

  ref = &catalog->refs[catalog->current];
  ret = card_model_open(CONFIG_MOXINZHI_CARD_STORE_PATH, &model);
  if (ret < 0)
    {
      catalog->last_error = ret;
      return ret;
    }

  next_due_ms = mz_anki_now_ms() +
                (mastered ? MZ_ANKI_DAY_MS : MZ_ANKI_RETRY_MS);
  ret = card_model_update_review(model, ref->fingerprint,
                                 ref->source_card_id, mastered,
                                 next_due_ms);
  card_model_close(model);
  catalog->last_error = ret;
  if (ret >= 0)
    {
      if (ref->review_count < UINT32_MAX)
        {
          ref->review_count++;
        }

      if (mastered && ref->correct_count < UINT32_MAX)
        {
          ref->correct_count++;
        }

      catalog->view.state = mastered ? MZ_CARD_MASTERED : MZ_CARD_REVIEW;
    }

  return ret;
}

int mz_anki_advance(FAR struct mz_anki_catalog_s *catalog)
{
  if (catalog == NULL || catalog->cached_count == 0)
    {
      return -ENODATA;
    }

  catalog->current = (uint16_t)((catalog->current + 1) %
                                catalog->cached_count);
  return mz_anki_load_current(catalog);
}

FAR const struct mz_card_s *mz_anki_current(
  FAR const struct mz_anki_catalog_s *catalog)
{
  return catalog != NULL && catalog->view_ready ? &catalog->view : NULL;
}

#else

int mz_anki_init(FAR struct mz_anki_catalog_s *catalog)
{
  if (catalog == NULL)
    {
      return -EINVAL;
    }

  memset(catalog, 0, sizeof(*catalog));
  return 0;
}

int mz_anki_reload(FAR struct mz_anki_catalog_s *catalog)
{
  return catalog == NULL ? -EINVAL : 0;
}

int mz_anki_rate_current(FAR struct mz_anki_catalog_s *catalog,
                         bool mastered)
{
  (void)catalog;
  (void)mastered;
  return -ENOSYS;
}

int mz_anki_advance(FAR struct mz_anki_catalog_s *catalog)
{
  (void)catalog;
  return -ENOSYS;
}

FAR const struct mz_card_s *mz_anki_current(
  FAR const struct mz_anki_catalog_s *catalog)
{
  (void)catalog;
  return NULL;
}

#endif
