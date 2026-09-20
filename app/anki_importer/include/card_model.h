/****************************************************************************
 * app/anki_importer/include/card_model.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef MOXINZHI_CARD_MODEL_H
#define MOXINZHI_CARD_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define CARD_MODEL_FINGERPRINT_HEX_LEN 64
#define CARD_MODEL_MAX_MEDIA_NAME       180
#define CARD_MODEL_MAX_LOCAL_PATH       320

enum card_model_import_state_e
{
  CARD_MODEL_IMPORT_STAGING = 1,
  CARD_MODEL_IMPORT_COMPLETE = 2
};

enum card_model_source_format_e
{
  CARD_MODEL_SOURCE_ANKI20 = 20,
  CARD_MODEL_SOURCE_ANKI21 = 21
};

struct card_model_s;

struct card_model_import_s
{
  enum card_model_import_state_e state;
  enum card_model_source_format_e format;
  uint64_t imported_at_ms;
  uint32_t card_count;
  uint32_t deck_count;
  uint32_t media_count;
  uint32_t card_skipped_count;
  uint32_t media_skipped_count;
  char source_name[192];
};

struct card_model_card_s
{
  uint64_t imported_at_ms;
  int64_t source_card_id;
  int64_t source_note_id;
  int64_t source_deck_id;
  int64_t source_model_id;
  uint64_t due_at_ms;
  uint32_t flags;
  uint32_t review_count;
  uint32_t correct_count;
  const char *front;
  const char *back;
  const char *deck_name;
  const char *tags;
  const char *media_refs;
};

struct card_model_media_s
{
  uint32_t zip_index;
  uint64_t size;
  char name[CARD_MODEL_MAX_MEDIA_NAME + 1];
  char local_path[CARD_MODEL_MAX_LOCAL_PATH + 1];
};

typedef int (*card_model_visit_card_t)(
  const char *fingerprint,
  const struct card_model_card_s *card, void *arg);

int card_model_open(const char *path, struct card_model_s **out_model);
void card_model_close(struct card_model_s *model);

int card_model_begin(struct card_model_s *model);
int card_model_commit(struct card_model_s *model);
int card_model_rollback(struct card_model_s *model);

int card_model_get_import(
  struct card_model_s *model,
  const char *fingerprint,
  struct card_model_import_s *record);
int card_model_put_import(
  struct card_model_s *model,
  const char *fingerprint,
  const struct card_model_import_s *record);
int card_model_delete_import(
  struct card_model_s *model,
  const char *fingerprint);

int card_model_put_deck(
  struct card_model_s *model,
  const char *fingerprint,
  int64_t source_deck_id, const char *name);

int card_model_put_card(
  struct card_model_s *model,
  const char *fingerprint,
  const struct card_model_card_s *card);
int card_model_get_card(
  struct card_model_s *model,
  const char *fingerprint,
  int64_t source_card_id, struct card_model_card_s *card,
  void **allocation);
int card_model_foreach_card(struct card_model_s *model,
                            card_model_visit_card_t visit, void *arg);
int card_model_update_review(
  struct card_model_s *model,
  const char *fingerprint,
  int64_t source_card_id, bool correct, uint64_t next_due_at_ms);

int card_model_mark_media_reference(
  struct card_model_s *model,
  const char *fingerprint,
  const char *name);
int card_model_has_media_reference(
  struct card_model_s *model,
  const char *fingerprint,
  const char *name, bool *referenced);
int card_model_put_media(
  struct card_model_s *model,
  const char *fingerprint,
  const struct card_model_media_s *media);
int card_model_delete_media(
  struct card_model_s *model,
  const char *fingerprint,
  const struct card_model_media_s *media);
int card_model_get_media_by_zip_index(
  struct card_model_s *model,
  const char *fingerprint,
  uint32_t zip_index, struct card_model_media_s *media);
int card_model_get_media_by_name(
  struct card_model_s *model,
  const char *fingerprint,
  const char *name, struct card_model_media_s *media);

#ifdef __cplusplus
}
#endif

#endif /* MOXINZHI_CARD_MODEL_H */
