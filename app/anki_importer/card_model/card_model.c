/****************************************************************************
 * app/anki_importer/card_model/card_model.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "card_model.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unqlite.h>

#define CARD_MODEL_SCHEMA_KEY "mx/schema"
#define CARD_MODEL_SCHEMA_VALUE "moxinzhi-card-model/1"
#define CARD_MODEL_KEY_MAX 512
#define CARD_MODEL_CARD_MAGIC 0x3143584dU /* MXC1 */
#define CARD_MODEL_IMPORT_MAGIC 0x3149584dU /* MXI1 */
#define CARD_MODEL_MEDIA_MAGIC 0x314d584dU /* MXM1 */
#define CARD_MODEL_CARD_HEADER_SIZE 84U
#define CARD_MODEL_IMPORT_HEADER_SIZE 40U
#define CARD_MODEL_MEDIA_HEADER_SIZE 24U

struct card_model_s
{
  unqlite *db;
  bool transaction_active;
};

static void card_put_u32(uint8_t *buf, uint32_t value)
{
  buf[0] = (uint8_t)value;
  buf[1] = (uint8_t)(value >> 8);
  buf[2] = (uint8_t)(value >> 16);
  buf[3] = (uint8_t)(value >> 24);
}

static uint32_t card_get_u32(const uint8_t *buf)
{
  return (uint32_t)buf[0] |
         (uint32_t)buf[1] << 8 |
         (uint32_t)buf[2] << 16 |
         (uint32_t)buf[3] << 24;
}

static void card_put_u64(uint8_t *buf, uint64_t value)
{
  card_put_u32(buf, (uint32_t)value);
  card_put_u32(buf + 4, (uint32_t)(value >> 32));
}

static uint64_t card_get_u64(const uint8_t *buf)
{
  return (uint64_t)card_get_u32(buf) |
         (uint64_t)card_get_u32(buf + 4) << 32;
}

static int card_model_result(int result)
{
  if (result == UNQLITE_OK)
    {
      return 0;
    }

  if (result == UNQLITE_NOTFOUND)
    {
      return -ENOENT;
    }

  if (result == UNQLITE_NOMEM)
    {
      return -ENOMEM;
    }

  if (result == UNQLITE_BUSY || result == UNQLITE_LOCKED)
    {
      return -EBUSY;
    }

  return -EIO;
}

static bool card_model_valid_fingerprint(const char *fingerprint)
{
  size_t i;

  if (fingerprint == NULL ||
      strlen(fingerprint) != CARD_MODEL_FINGERPRINT_HEX_LEN)
    {
      return false;
    }

  for (i = 0; i < CARD_MODEL_FINGERPRINT_HEX_LEN; i++)
    {
      char ch = fingerprint[i];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
        {
          return false;
        }
    }

  return true;
}

static int card_model_key(char *key, size_t key_size, const char *kind,
                          const char *fingerprint, const char *suffix)
{
  int written;

  if (!card_model_valid_fingerprint(fingerprint) || kind == NULL)
    {
      return -EINVAL;
    }

  written = suffix == NULL ?
    snprintf(key, key_size, "mx/v1/%s/%s", kind, fingerprint) :
    snprintf(key, key_size, "mx/v1/%s/%s/%s", kind, fingerprint, suffix);
  if (written < 0 || (size_t)written >= key_size)
    {
      return -ENAMETOOLONG;
    }

  return written;
}

static int card_model_fetch(struct card_model_s *model, const char *key,
                            uint8_t **data, size_t *size)
{
  unqlite_int64 bytes = 0;
  uint8_t *buffer;
  int result;

  result = unqlite_kv_fetch(model->db, key, -1, NULL, &bytes);
  if (result != UNQLITE_OK)
    {
      return card_model_result(result);
    }

  if (bytes < 0 || (uint64_t)bytes > SIZE_MAX)
    {
      return -EFBIG;
    }

  buffer = malloc((size_t)bytes + 1);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  result = unqlite_kv_fetch(model->db, key, -1, buffer, &bytes);
  if (result != UNQLITE_OK)
    {
      free(buffer);
      return card_model_result(result);
    }

  buffer[(size_t)bytes] = '\0';
  *data = buffer;
  *size = (size_t)bytes;
  return 0;
}

int card_model_open(const char *path, struct card_model_s **out_model)
{
  struct card_model_s *model;
  unqlite_int64 schema_size;
  char schema[sizeof(CARD_MODEL_SCHEMA_VALUE)];
  int result;

  if (path == NULL || out_model == NULL)
    {
      return -EINVAL;
    }

  model = calloc(1, sizeof(*model));
  if (model == NULL)
    {
      return -ENOMEM;
    }

  result = unqlite_open(&model->db, path,
                        UNQLITE_OPEN_READWRITE | UNQLITE_OPEN_CREATE);
  if (result != UNQLITE_OK)
    {
      free(model);
      return card_model_result(result);
    }

  schema_size = sizeof(schema);
  result = unqlite_kv_fetch(model->db, CARD_MODEL_SCHEMA_KEY, -1,
                            schema, &schema_size);
  if (result == UNQLITE_NOTFOUND)
    {
      result = unqlite_kv_store(model->db, CARD_MODEL_SCHEMA_KEY, -1,
                                CARD_MODEL_SCHEMA_VALUE,
                                sizeof(CARD_MODEL_SCHEMA_VALUE) - 1);
      if (result == UNQLITE_OK)
        {
          result = unqlite_commit(model->db);
        }
    }
  else if (result == UNQLITE_OK &&
           (schema_size != sizeof(CARD_MODEL_SCHEMA_VALUE) - 1 ||
            memcmp(schema, CARD_MODEL_SCHEMA_VALUE,
                   sizeof(CARD_MODEL_SCHEMA_VALUE) - 1) != 0))
    {
      result = UNQLITE_INVALID;
    }

  if (result != UNQLITE_OK)
    {
      unqlite_close(model->db);
      free(model);
      return result == UNQLITE_INVALID ? -EPROTONOSUPPORT :
                                        card_model_result(result);
    }

  *out_model = model;
  return 0;
}

void card_model_close(struct card_model_s *model)
{
  if (model != NULL)
    {
      if (model->transaction_active)
        {
          unqlite_rollback(model->db);
        }

      unqlite_close(model->db);
      free(model);
    }
}

int card_model_begin(struct card_model_s *model)
{
  int result;

  if (model == NULL || model->transaction_active)
    {
      return -EINVAL;
    }

  result = unqlite_begin(model->db);
  if (result == UNQLITE_OK)
    {
      model->transaction_active = true;
    }

  return card_model_result(result);
}

int card_model_commit(struct card_model_s *model)
{
  int result;

  if (model == NULL || !model->transaction_active)
    {
      return -EINVAL;
    }

  result = unqlite_commit(model->db);
  if (result == UNQLITE_OK)
    {
      model->transaction_active = false;
    }

  return card_model_result(result);
}

int card_model_rollback(struct card_model_s *model)
{
  int result;

  if (model == NULL || !model->transaction_active)
    {
      return -EINVAL;
    }

  result = unqlite_rollback(model->db);
  model->transaction_active = false;
  return card_model_result(result);
}

int card_model_put_import(struct card_model_s *model, const char *fingerprint,
                          const struct card_model_import_s *record)
{
  uint8_t *wire;
  char key[CARD_MODEL_KEY_MAX];
  size_t source_len;
  size_t wire_size;
  int result;

  if (model == NULL || record == NULL)
    {
      return -EINVAL;
    }

  result = card_model_key(key, sizeof(key), "import", fingerprint, NULL);
  if (result < 0)
    {
      return result;
    }

  source_len = strnlen(record->source_name, sizeof(record->source_name));
  wire_size = CARD_MODEL_IMPORT_HEADER_SIZE + source_len;
  wire = calloc(1, wire_size);
  if (wire == NULL)
    {
      return -ENOMEM;
    }

  card_put_u32(wire, CARD_MODEL_IMPORT_MAGIC);
  card_put_u32(wire + 4, (uint32_t)record->state);
  card_put_u32(wire + 8, (uint32_t)record->format);
  card_put_u64(wire + 12, record->imported_at_ms);
  card_put_u32(wire + 20, record->card_count);
  card_put_u32(wire + 24, record->deck_count);
  card_put_u32(wire + 28, record->media_count);
  card_put_u32(wire + 32, record->card_skipped_count);
  card_put_u32(wire + 36, record->media_skipped_count);
  if (source_len > 0)
    {
      memcpy(wire + CARD_MODEL_IMPORT_HEADER_SIZE, record->source_name,
             source_len);
    }

  result = unqlite_kv_store(model->db, key, -1, wire,
                            (unqlite_int64)wire_size);
  free(wire);
  return card_model_result(result);
}

int card_model_get_import(struct card_model_s *model, const char *fingerprint,
                          struct card_model_import_s *record)
{
  uint8_t *wire;
  char key[CARD_MODEL_KEY_MAX];
  size_t wire_size;
  size_t source_len;
  int result;

  if (model == NULL || record == NULL)
    {
      return -EINVAL;
    }

  result = card_model_key(key, sizeof(key), "import", fingerprint, NULL);
  if (result < 0)
    {
      return result;
    }

  result = card_model_fetch(model, key, &wire, &wire_size);
  if (result < 0)
    {
      return result;
    }

  if (wire_size < CARD_MODEL_IMPORT_HEADER_SIZE ||
      card_get_u32(wire) != CARD_MODEL_IMPORT_MAGIC)
    {
      free(wire);
      return -EBADMSG;
    }

  memset(record, 0, sizeof(*record));
  record->state = (enum card_model_import_state_e)card_get_u32(wire + 4);
  record->format = (enum card_model_source_format_e)card_get_u32(wire + 8);
  record->imported_at_ms = card_get_u64(wire + 12);
  record->card_count = card_get_u32(wire + 20);
  record->deck_count = card_get_u32(wire + 24);
  record->media_count = card_get_u32(wire + 28);
  record->card_skipped_count = card_get_u32(wire + 32);
  record->media_skipped_count = card_get_u32(wire + 36);
  source_len = wire_size - CARD_MODEL_IMPORT_HEADER_SIZE;
  if (source_len >= sizeof(record->source_name))
    {
      source_len = sizeof(record->source_name) - 1;
    }

  memcpy(record->source_name, wire + CARD_MODEL_IMPORT_HEADER_SIZE,
         source_len);
  record->source_name[source_len] = '\0';
  free(wire);
  return 0;
}

static int card_model_delete_prefix(struct card_model_s *model,
                                    const char *prefix)
{
  unqlite_kv_cursor *cursor;
  char *key = NULL;
  size_t prefix_len = strlen(prefix);
  int key_len;
  int result;
  bool deleted;

  do
    {
      deleted = false;
      result = unqlite_kv_cursor_init(model->db, &cursor);
      if (result != UNQLITE_OK)
        {
          return card_model_result(result);
        }

      result = unqlite_kv_cursor_first_entry(cursor);
      while (result == UNQLITE_OK &&
             unqlite_kv_cursor_valid_entry(cursor) != 0)
        {
          key_len = 0;
          result = unqlite_kv_cursor_key(cursor, NULL, &key_len);
          if (result != UNQLITE_OK || key_len < 0)
            {
              break;
            }

          key = malloc((size_t)key_len + 1);
          if (key == NULL)
            {
              unqlite_kv_cursor_release(model->db, cursor);
              return -ENOMEM;
            }

          result = unqlite_kv_cursor_key(cursor, key, &key_len);
          if (result != UNQLITE_OK)
            {
              free(key);
              key = NULL;
              break;
            }

          key[key_len] = '\0';
          if ((size_t)key_len >= prefix_len &&
              memcmp(key, prefix, prefix_len) == 0)
            {
              free(key);
              key = NULL;
              result = unqlite_kv_cursor_delete_entry(cursor);
              deleted = result == UNQLITE_OK;
              break;
            }

          free(key);
          key = NULL;
          result = unqlite_kv_cursor_next_entry(cursor);
        }

      free(key);
      key = NULL;
      unqlite_kv_cursor_release(model->db, cursor);
      if (result != UNQLITE_OK && result != UNQLITE_DONE &&
          result != UNQLITE_EOF &&
          result != UNQLITE_NOTFOUND)
        {
          return card_model_result(result);
        }
    }
  while (deleted);

  return 0;
}

int card_model_delete_import(struct card_model_s *model,
                             const char *fingerprint)
{
  static const char *const kinds[] =
  {
    "card", "deck", "ref", "mediazip", "medianame", "import"
  };
  char prefix[CARD_MODEL_KEY_MAX];
  size_t i;
  int result;

  if (model == NULL || !card_model_valid_fingerprint(fingerprint))
    {
      return -EINVAL;
    }

  for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
    {
      result = card_model_key(prefix, sizeof(prefix), kinds[i],
                              fingerprint, NULL);
      if (result < 0)
        {
          return result;
        }

      result = card_model_delete_prefix(model, prefix);
      if (result < 0)
        {
          return result;
        }
    }

  return 0;
}

int card_model_put_deck(struct card_model_s *model, const char *fingerprint,
                        int64_t source_deck_id, const char *name)
{
  char suffix[32];
  char key[CARD_MODEL_KEY_MAX];
  size_t name_len;
  int result;

  if (model == NULL || name == NULL)
    {
      return -EINVAL;
    }

  snprintf(suffix, sizeof(suffix), "%" PRId64, source_deck_id);
  result = card_model_key(key, sizeof(key), "deck", fingerprint, suffix);
  if (result < 0)
    {
      return result;
    }

  name_len = strlen(name);
  return card_model_result(unqlite_kv_store(model->db, key, -1, name,
                                            (unqlite_int64)name_len));
}

static int card_model_card_key(char *key, size_t key_size,
                               const char *fingerprint,
                               int64_t source_card_id)
{
  char suffix[32];

  snprintf(suffix, sizeof(suffix), "%" PRId64, source_card_id);
  return card_model_key(key, key_size, "card", fingerprint, suffix);
}

int card_model_put_card(struct card_model_s *model, const char *fingerprint,
                        const struct card_model_card_s *card)
{
  const char *strings[5];
  uint32_t lengths[5];
  uint8_t *wire;
  char key[CARD_MODEL_KEY_MAX];
  size_t wire_size = CARD_MODEL_CARD_HEADER_SIZE;
  size_t offset;
  size_t i;
  int result;

  if (model == NULL || card == NULL || card->front == NULL ||
      card->back == NULL)
    {
      return -EINVAL;
    }

  result = card_model_card_key(key, sizeof(key), fingerprint,
                               card->source_card_id);
  if (result < 0)
    {
      return result;
    }

  strings[0] = card->front;
  strings[1] = card->back;
  strings[2] = card->deck_name == NULL ? "" : card->deck_name;
  strings[3] = card->tags == NULL ? "" : card->tags;
  strings[4] = card->media_refs == NULL ? "" : card->media_refs;

  for (i = 0; i < 5; i++)
    {
      size_t length = strlen(strings[i]);
      if (length > UINT32_MAX || wire_size > SIZE_MAX - length)
        {
          return -EFBIG;
        }

      lengths[i] = (uint32_t)length;
      wire_size += length;
    }

  wire = calloc(1, wire_size);
  if (wire == NULL)
    {
      return -ENOMEM;
    }

  card_put_u32(wire, CARD_MODEL_CARD_MAGIC);
  card_put_u32(wire + 4, card->flags);
  card_put_u64(wire + 8, card->imported_at_ms);
  card_put_u64(wire + 16, (uint64_t)card->source_card_id);
  card_put_u64(wire + 24, (uint64_t)card->source_note_id);
  card_put_u64(wire + 32, (uint64_t)card->source_deck_id);
  card_put_u64(wire + 40, (uint64_t)card->source_model_id);
  card_put_u64(wire + 48, card->due_at_ms);
  card_put_u32(wire + 56, card->review_count);
  card_put_u32(wire + 60, card->correct_count);
  for (i = 0; i < 5; i++)
    {
      card_put_u32(wire + 64 + i * 4, lengths[i]);
    }

  offset = CARD_MODEL_CARD_HEADER_SIZE;
  for (i = 0; i < 5; i++)
    {
      memcpy(wire + offset, strings[i], lengths[i]);
      offset += lengths[i];
    }

  result = unqlite_kv_store(model->db, key, -1, wire,
                            (unqlite_int64)wire_size);
  free(wire);
  return card_model_result(result);
}

static int card_model_decode_card(const uint8_t *wire, size_t wire_size,
                                  struct card_model_card_s *card,
                                  void **allocation)
{
  uint32_t lengths[5];
  char *strings;
  size_t string_bytes = 0;
  size_t input_offset;
  size_t output_offset;
  size_t i;

  if (wire_size < CARD_MODEL_CARD_HEADER_SIZE ||
      card_get_u32(wire) != CARD_MODEL_CARD_MAGIC)
    {
      return -EBADMSG;
    }

  for (i = 0; i < 5; i++)
    {
      lengths[i] = card_get_u32(wire + 64 + i * 4);
      if (string_bytes > SIZE_MAX - lengths[i] - 1)
        {
          return -EFBIG;
        }

      string_bytes += lengths[i] + 1;
    }

  if (string_bytes - 5 != wire_size - CARD_MODEL_CARD_HEADER_SIZE)
    {
      return -EBADMSG;
    }

  strings = malloc(string_bytes);
  if (strings == NULL)
    {
      return -ENOMEM;
    }

  memset(card, 0, sizeof(*card));
  card->flags = card_get_u32(wire + 4);
  card->imported_at_ms = card_get_u64(wire + 8);
  card->source_card_id = (int64_t)card_get_u64(wire + 16);
  card->source_note_id = (int64_t)card_get_u64(wire + 24);
  card->source_deck_id = (int64_t)card_get_u64(wire + 32);
  card->source_model_id = (int64_t)card_get_u64(wire + 40);
  card->due_at_ms = card_get_u64(wire + 48);
  card->review_count = card_get_u32(wire + 56);
  card->correct_count = card_get_u32(wire + 60);

  input_offset = CARD_MODEL_CARD_HEADER_SIZE;
  output_offset = 0;
  for (i = 0; i < 5; i++)
    {
      memcpy(strings + output_offset, wire + input_offset, lengths[i]);
      strings[output_offset + lengths[i]] = '\0';
      input_offset += lengths[i];
      output_offset += lengths[i] + 1;
    }

  card->front = strings;
  card->back = card->front + lengths[0] + 1;
  card->deck_name = card->back + lengths[1] + 1;
  card->tags = card->deck_name + lengths[2] + 1;
  card->media_refs = card->tags + lengths[3] + 1;
  *allocation = strings;
  return 0;
}

int card_model_get_card(struct card_model_s *model, const char *fingerprint,
                        int64_t source_card_id,
                        struct card_model_card_s *card, void **allocation)
{
  uint8_t *wire;
  char key[CARD_MODEL_KEY_MAX];
  size_t wire_size;
  int result;

  if (model == NULL || card == NULL || allocation == NULL)
    {
      return -EINVAL;
    }

  result = card_model_card_key(key, sizeof(key), fingerprint, source_card_id);
  if (result < 0)
    {
      return result;
    }

  result = card_model_fetch(model, key, &wire, &wire_size);
  if (result < 0)
    {
      return result;
    }

  result = card_model_decode_card(wire, wire_size, card, allocation);
  free(wire);
  return result;
}

int card_model_foreach_card(struct card_model_s *model,
                            card_model_visit_card_t visit, void *arg)
{
  unqlite_kv_cursor *cursor;
  struct card_model_card_s card;
  unqlite_int64 data_len;
  uint8_t *wire = NULL;
  char *key = NULL;
  char fingerprint[CARD_MODEL_FINGERPRINT_HEX_LEN + 1];
  const char prefix[] = "mx/v1/card/";
  int key_len;
  int result;
  int visit_result = 0;
  void *allocation = NULL;

  if (model == NULL || visit == NULL)
    {
      return -EINVAL;
    }

  result = unqlite_kv_cursor_init(model->db, &cursor);
  if (result != UNQLITE_OK)
    {
      return card_model_result(result);
    }

  result = unqlite_kv_cursor_first_entry(cursor);
  while (result == UNQLITE_OK &&
         unqlite_kv_cursor_valid_entry(cursor) != 0)
    {
      key_len = 0;
      data_len = 0;
      result = unqlite_kv_cursor_key(cursor, NULL, &key_len);
      if (result != UNQLITE_OK || key_len < 0)
        {
          break;
        }

      key = malloc((size_t)key_len + 1);
      if (key == NULL)
        {
          visit_result = -ENOMEM;
          break;
        }

      result = unqlite_kv_cursor_key(cursor, key, &key_len);
      if (result != UNQLITE_OK)
        {
          break;
        }

      key[key_len] = '\0';
      if ((size_t)key_len > sizeof(prefix) - 1 +
                            CARD_MODEL_FINGERPRINT_HEX_LEN + 1 &&
          memcmp(key, prefix, sizeof(prefix) - 1) == 0)
        {
          memcpy(fingerprint, key + sizeof(prefix) - 1,
                 CARD_MODEL_FINGERPRINT_HEX_LEN);
          fingerprint[CARD_MODEL_FINGERPRINT_HEX_LEN] = '\0';
          result = unqlite_kv_cursor_data(cursor, NULL, &data_len);
          if (result != UNQLITE_OK || data_len < 0 ||
              (uint64_t)data_len > SIZE_MAX)
            {
              break;
            }

          wire = malloc((size_t)data_len);
          if (wire == NULL)
            {
              visit_result = -ENOMEM;
              break;
            }

          result = unqlite_kv_cursor_data(cursor, wire, &data_len);
          if (result != UNQLITE_OK)
            {
              break;
            }

          visit_result = card_model_decode_card(wire, (size_t)data_len,
                                                 &card, &allocation);
          if (visit_result == 0)
            {
              visit_result = visit(fingerprint, &card, arg);
            }

          free(allocation);
          allocation = NULL;
          free(wire);
          wire = NULL;
          if (visit_result != 0)
            {
              break;
            }
        }

      free(key);
      key = NULL;
      result = unqlite_kv_cursor_next_entry(cursor);
    }

  free(key);
  free(wire);
  free(allocation);
  unqlite_kv_cursor_release(model->db, cursor);
  if (visit_result != 0)
    {
      return visit_result;
    }

  if (result == UNQLITE_OK || result == UNQLITE_DONE ||
      result == UNQLITE_EOF ||
      result == UNQLITE_NOTFOUND)
    {
      return 0;
    }

  return card_model_result(result);
}

int card_model_update_review(struct card_model_s *model,
                             const char *fingerprint,
                             int64_t source_card_id, bool correct,
                             uint64_t next_due_at_ms)
{
  struct card_model_card_s card;
  void *allocation = NULL;
  int result;

  result = card_model_get_card(model, fingerprint, source_card_id, &card,
                               &allocation);
  if (result < 0)
    {
      return result;
    }

  if (card.review_count < UINT32_MAX)
    {
      card.review_count++;
    }

  if (correct && card.correct_count < UINT32_MAX)
    {
      card.correct_count++;
    }

  card.due_at_ms = next_due_at_ms;
  result = card_model_put_card(model, fingerprint, &card);
  free(allocation);
  return result;
}

static bool card_model_safe_media_name(const char *name)
{
  size_t length;
  size_t i;

  if (name == NULL)
    {
      return false;
    }

  length = strlen(name);
  if (length == 0 || length > CARD_MODEL_MAX_MEDIA_NAME ||
      strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    {
      return false;
    }

  for (i = 0; i < length; i++)
    {
      unsigned char ch = (unsigned char)name[i];
      if (ch < 0x20 || ch == 0x7f || ch == '/' || ch == '\\' || ch == ':')
        {
          return false;
        }
    }

  return true;
}

int card_model_mark_media_reference(struct card_model_s *model,
                                    const char *fingerprint,
                                    const char *name)
{
  static const uint8_t present = 1;
  char key[CARD_MODEL_KEY_MAX];
  int result;

  if (model == NULL || !card_model_safe_media_name(name))
    {
      return -EINVAL;
    }

  result = card_model_key(key, sizeof(key), "ref", fingerprint, name);
  if (result < 0)
    {
      return result;
    }

  return card_model_result(unqlite_kv_store(model->db, key, -1, &present,
                                            sizeof(present)));
}

int card_model_has_media_reference(struct card_model_s *model,
                                   const char *fingerprint,
                                   const char *name, bool *referenced)
{
  unqlite_int64 size = 0;
  char key[CARD_MODEL_KEY_MAX];
  int result;

  if (model == NULL || referenced == NULL ||
      !card_model_safe_media_name(name))
    {
      return -EINVAL;
    }

  result = card_model_key(key, sizeof(key), "ref", fingerprint, name);
  if (result < 0)
    {
      return result;
    }

  result = unqlite_kv_fetch(model->db, key, -1, NULL, &size);
  if (result == UNQLITE_NOTFOUND)
    {
      *referenced = false;
      return 0;
    }

  if (result == UNQLITE_OK)
    {
      *referenced = true;
      return 0;
    }

  return card_model_result(result);
}

static int card_model_encode_media(const struct card_model_media_s *media,
                                   uint8_t **wire, size_t *wire_size)
{
  size_t name_len;
  size_t path_len;
  uint8_t *buffer;

  if (!card_model_safe_media_name(media->name))
    {
      return -EINVAL;
    }

  name_len = strlen(media->name);
  path_len = strnlen(media->local_path, sizeof(media->local_path));
  if (path_len == 0 || path_len > CARD_MODEL_MAX_LOCAL_PATH)
    {
      return -EINVAL;
    }

  *wire_size = CARD_MODEL_MEDIA_HEADER_SIZE + name_len + path_len;
  buffer = malloc(*wire_size);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  card_put_u32(buffer, CARD_MODEL_MEDIA_MAGIC);
  card_put_u32(buffer + 4, media->zip_index);
  card_put_u64(buffer + 8, media->size);
  card_put_u32(buffer + 16, (uint32_t)name_len);
  card_put_u32(buffer + 20, (uint32_t)path_len);
  memcpy(buffer + CARD_MODEL_MEDIA_HEADER_SIZE, media->name, name_len);
  memcpy(buffer + CARD_MODEL_MEDIA_HEADER_SIZE + name_len,
         media->local_path, path_len);
  *wire = buffer;
  return 0;
}

int card_model_put_media(struct card_model_s *model, const char *fingerprint,
                         const struct card_model_media_s *media)
{
  uint8_t *wire;
  char suffix[32];
  char zip_key[CARD_MODEL_KEY_MAX];
  char name_key[CARD_MODEL_KEY_MAX];
  size_t wire_size;
  int result;

  if (model == NULL || media == NULL)
    {
      return -EINVAL;
    }

  result = card_model_encode_media(media, &wire, &wire_size);
  if (result < 0)
    {
      return result;
    }

  snprintf(suffix, sizeof(suffix), "%" PRIu32, media->zip_index);
  result = card_model_key(zip_key, sizeof(zip_key), "mediazip", fingerprint,
                          suffix);
  if (result >= 0)
    {
      result = card_model_key(name_key, sizeof(name_key), "medianame",
                              fingerprint, media->name);
    }

  if (result >= 0)
    {
      result = card_model_result(unqlite_kv_store(model->db, zip_key, -1,
                                                  wire,
                                                  (unqlite_int64)wire_size));
    }

  if (result >= 0)
    {
      result = card_model_result(unqlite_kv_store(model->db, name_key, -1,
                                                  wire,
                                                  (unqlite_int64)wire_size));
    }

  free(wire);
  return result;
}

int card_model_delete_media(struct card_model_s *model,
                            const char *fingerprint,
                            const struct card_model_media_s *media)
{
  char suffix[32];
  char zip_key[CARD_MODEL_KEY_MAX];
  char name_key[CARD_MODEL_KEY_MAX];
  int result;

  if (model == NULL || media == NULL ||
      !card_model_safe_media_name(media->name))
    {
      return -EINVAL;
    }

  snprintf(suffix, sizeof(suffix), "%" PRIu32, media->zip_index);
  result = card_model_key(zip_key, sizeof(zip_key), "mediazip", fingerprint,
                          suffix);
  if (result >= 0)
    {
      result = card_model_key(name_key, sizeof(name_key), "medianame",
                              fingerprint, media->name);
    }

  if (result >= 0)
    {
      result = unqlite_kv_delete(model->db, zip_key, -1);
      if (result == UNQLITE_NOTFOUND)
        {
          result = UNQLITE_OK;
        }
    }

  if (result >= 0)
    {
      result = unqlite_kv_delete(model->db, name_key, -1);
      if (result == UNQLITE_NOTFOUND)
        {
          result = UNQLITE_OK;
        }
    }

  return result < 0 ? card_model_result(result) : 0;
}

static int card_model_decode_media(const uint8_t *wire, size_t wire_size,
                                   struct card_model_media_s *media)
{
  uint32_t name_len;
  uint32_t path_len;

  if (wire_size < CARD_MODEL_MEDIA_HEADER_SIZE ||
      card_get_u32(wire) != CARD_MODEL_MEDIA_MAGIC)
    {
      return -EBADMSG;
    }

  name_len = card_get_u32(wire + 16);
  path_len = card_get_u32(wire + 20);
  if (name_len == 0 || name_len > CARD_MODEL_MAX_MEDIA_NAME ||
      path_len == 0 || path_len > CARD_MODEL_MAX_LOCAL_PATH ||
      CARD_MODEL_MEDIA_HEADER_SIZE + (size_t)name_len + path_len != wire_size)
    {
      return -EBADMSG;
    }

  memset(media, 0, sizeof(*media));
  media->zip_index = card_get_u32(wire + 4);
  media->size = card_get_u64(wire + 8);
  memcpy(media->name, wire + CARD_MODEL_MEDIA_HEADER_SIZE, name_len);
  memcpy(media->local_path,
         wire + CARD_MODEL_MEDIA_HEADER_SIZE + name_len, path_len);
  return 0;
}

static int card_model_get_media(struct card_model_s *model, const char *key,
                                struct card_model_media_s *media)
{
  uint8_t *wire;
  size_t wire_size;
  int result;

  result = card_model_fetch(model, key, &wire, &wire_size);
  if (result < 0)
    {
      return result;
    }

  result = card_model_decode_media(wire, wire_size, media);
  free(wire);
  return result;
}

int card_model_get_media_by_zip_index(struct card_model_s *model,
                                      const char *fingerprint,
                                      uint32_t zip_index,
                                      struct card_model_media_s *media)
{
  char suffix[32];
  char key[CARD_MODEL_KEY_MAX];
  int result;

  if (model == NULL || media == NULL)
    {
      return -EINVAL;
    }

  snprintf(suffix, sizeof(suffix), "%" PRIu32, zip_index);
  result = card_model_key(key, sizeof(key), "mediazip", fingerprint, suffix);
  if (result < 0)
    {
      return result;
    }

  return card_model_get_media(model, key, media);
}

int card_model_get_media_by_name(struct card_model_s *model,
                                 const char *fingerprint, const char *name,
                                 struct card_model_media_s *media)
{
  char key[CARD_MODEL_KEY_MAX];
  int result;

  if (model == NULL || media == NULL || !card_model_safe_media_name(name))
    {
      return -EINVAL;
    }

  result = card_model_key(key, sizeof(key), "medianame", fingerprint, name);
  if (result < 0)
    {
      return result;
    }

  return card_model_get_media(model, key, media);
}
