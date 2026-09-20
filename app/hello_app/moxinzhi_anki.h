/****************************************************************************
 * Contest 2026 team 208 - Anki card catalog bridge
 ****************************************************************************/

#ifndef __MOXINZHI_ANKI_H
#define __MOXINZHI_ANKI_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "moxinzhi_model_types.h"

#ifdef CONFIG_MOXINZHI_ANKI_IMPORTER
#  include "card_model.h"
#endif

#define MZ_ANKI_CACHE_CAPACITY 32

struct mz_anki_ref_s
{
#ifdef CONFIG_MOXINZHI_ANKI_IMPORTER
  char fingerprint[CARD_MODEL_FINGERPRINT_HEX_LEN + 1];
#else
  char fingerprint[65];
#endif
  int64_t source_card_id;
  uint32_t review_count;
  uint32_t correct_count;
};

struct mz_anki_catalog_s
{
  struct mz_anki_ref_s refs[MZ_ANKI_CACHE_CAPACITY];
  struct mz_card_s view;
  uint16_t cached_count;
  uint16_t current;
  int last_error;
  bool truncated;
  bool view_ready;
};

int mz_anki_init(FAR struct mz_anki_catalog_s *catalog);
int mz_anki_reload(FAR struct mz_anki_catalog_s *catalog);
int mz_anki_rate_current(FAR struct mz_anki_catalog_s *catalog,
                         bool mastered);
int mz_anki_advance(FAR struct mz_anki_catalog_s *catalog);
FAR const struct mz_card_s *mz_anki_current(
  FAR const struct mz_anki_catalog_s *catalog);

#endif /* __MOXINZHI_ANKI_H */
