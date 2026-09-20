/****************************************************************************
 * Contest 2026 team 208 - KVDB persistence adapter
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <kvdb.h>

#include "moxinzhi_storage.h"
#include "moxinzhi_text.h"

#define MZ_STORAGE_MAGIC   0x4d5a4c31u
#define MZ_STORAGE_VERSION 1
#define MZ_META_KEY        "persist.moxinzhi.meta"
#define MZ_CARD_KEY_FORMAT "persist.moxinzhi.card.%u"
#define MZ_PERSISTED_QUESTION_MAX 56
#define MZ_PERSISTED_ANSWER_MAX   128
#define MZ_PERSISTED_TOPIC_MAX    20

struct mz_record_header_s
{
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t checksum;
};

struct mz_meta_record_s
{
  struct mz_record_header_s header;
  struct mz_model_meta_s payload;
};

struct mz_persisted_card_s
{
  uint32_t id;
  uint8_t state;
  uint8_t reserved[3];
  char question[MZ_PERSISTED_QUESTION_MAX];
  char answer[MZ_PERSISTED_ANSWER_MAX];
  char topic[MZ_PERSISTED_TOPIC_MAX];
};

struct mz_card_record_s
{
  struct mz_record_header_s header;
  struct mz_persisted_card_s payload;
};

_Static_assert(sizeof(struct mz_card_record_s) < PROP_VALUE_MAX,
               "KVDB card record exceeds PROP_VALUE_MAX");
_Static_assert(sizeof(struct mz_persisted_card_s) == 212,
               "Persistent card ABI changed");
_Static_assert(sizeof(struct mz_card_record_s) == 224,
               "Persistent card record ABI changed");

static uint32_t mz_storage_checksum(FAR const void *data, size_t size)
{
  FAR const uint8_t *bytes = data;
  uint32_t hash = 2166136261u;
  size_t i;

  for (i = 0; i < size; i++)
    {
      hash ^= bytes[i];
      hash *= 16777619u;
    }

  return hash;
}

static bool mz_storage_valid_record(FAR void *record, size_t record_size,
                                    size_t payload_size)
{
  FAR struct mz_record_header_s *header = record;
  uint32_t checksum;
  uint32_t expected;

  if (header->magic != MZ_STORAGE_MAGIC ||
      header->version != MZ_STORAGE_VERSION ||
      header->size != payload_size)
    {
      return false;
    }

  expected = header->checksum;
  header->checksum = 0;
  checksum = mz_storage_checksum(record, record_size);
  header->checksum = expected;
  return checksum == expected;
}

static void mz_storage_card_key(uint8_t slot, FAR char *key,
                                 size_t key_size)
{
  snprintf(key, key_size, MZ_CARD_KEY_FORMAT, (unsigned int)slot);
}

static void mz_storage_decode_card(FAR struct mz_card_s *target,
                                   FAR const struct mz_persisted_card_s *source)
{
  memset(target, 0, sizeof(*target));
  target->id = source->id;
  target->state = source->state;
  mz_text_copy_utf8(target->question, sizeof(target->question),
                    source->question);
  mz_text_copy_utf8(target->answer, sizeof(target->answer), source->answer);
  mz_text_copy_utf8(target->topic, sizeof(target->topic), source->topic);
}

static void mz_storage_encode_card(
  FAR struct mz_persisted_card_s *target, FAR const struct mz_card_s *source)
{
  memset(target, 0, sizeof(*target));
  target->id = source->id;
  target->state = source->state;
  mz_text_copy_utf8(target->question, sizeof(target->question),
                    source->question);
  mz_text_copy_utf8(target->answer, sizeof(target->answer), source->answer);
  mz_text_copy_utf8(target->topic, sizeof(target->topic), source->topic);
}

int mz_storage_load(FAR struct mz_model_meta_s *meta,
                    FAR struct mz_card_s cards[MZ_CARD_CAPACITY])
{
  struct mz_meta_record_s meta_record;
  struct mz_card_record_s card_record;
  char key[32];
  ssize_t size;
  int first_error = 0;
  uint8_t slot;

  memset(meta, 0, sizeof(*meta));
  memset(cards, 0, sizeof(struct mz_card_s) * MZ_CARD_CAPACITY);

  size = property_get_binary(MZ_META_KEY, &meta_record,
                             sizeof(meta_record));
  if (size == -ENOENT)
    {
      return -ENOENT;
    }

  if (size != sizeof(meta_record) ||
      !mz_storage_valid_record(&meta_record, sizeof(meta_record),
                               sizeof(meta_record.payload)))
    {
      return size < 0 ? (int)size : -EBADMSG;
    }

  *meta = meta_record.payload;
  for (slot = 0; slot < MZ_CARD_CAPACITY; slot++)
    {
      mz_storage_card_key(slot, key, sizeof(key));
      size = property_get_binary(key, &card_record, sizeof(card_record));
      if (size == -ENOENT)
        {
          continue;
        }

      if (size != sizeof(card_record) ||
          !mz_storage_valid_record(&card_record, sizeof(card_record),
                                   sizeof(card_record.payload)))
        {
          if (first_error == 0)
            {
              first_error = size < 0 ? (int)size : -EBADMSG;
            }

          continue;
        }

      mz_storage_decode_card(&cards[slot], &card_record.payload);
    }

  return first_error;
}

int mz_storage_save_meta(FAR const struct mz_model_meta_s *meta)
{
  struct mz_meta_record_s record;
  int ret;

  memset(&record, 0, sizeof(record));
  record.header.magic = MZ_STORAGE_MAGIC;
  record.header.version = MZ_STORAGE_VERSION;
  record.header.size = sizeof(record.payload);
  record.payload = *meta;
  record.header.checksum = mz_storage_checksum(&record, sizeof(record));

  ret = property_set_binary(MZ_META_KEY, &record, sizeof(record), false);
  return ret < 0 ? ret : property_commit();
}

int mz_storage_save_card(uint8_t slot, FAR struct mz_card_s *card)
{
  struct mz_card_record_s record;
  char key[32];
  int ret;

  if (slot >= MZ_CARD_CAPACITY)
    {
      return -ERANGE;
    }

  memset(&record, 0, sizeof(record));
  record.header.magic = MZ_STORAGE_MAGIC;
  record.header.version = MZ_STORAGE_VERSION;
  record.header.size = sizeof(record.payload);
  mz_storage_encode_card(&record.payload, card);
  record.header.checksum = mz_storage_checksum(&record, sizeof(record));
  mz_storage_card_key(slot, key, sizeof(key));

  ret = property_set_binary(key, &record, sizeof(record), false);
  if (ret < 0)
    {
      return ret;
    }

  ret = property_commit();
  if (ret >= 0)
    {
      /* Keep the in-memory card identical to what can be restored after a
       * reboot from the version-1 KVDB record.
       */

      mz_storage_decode_card(card, &record.payload);
    }

  return ret;
}

int mz_storage_reset(void)
{
  char key[32];
  int ret;
  int first_error = 0;
  uint8_t slot;

  ret = property_delete(MZ_META_KEY);
  if (ret < 0 && ret != -ENOENT)
    {
      first_error = ret;
    }

  for (slot = 0; slot < MZ_CARD_CAPACITY; slot++)
    {
      mz_storage_card_key(slot, key, sizeof(key));
      ret = property_delete(key);
      if (ret < 0 && ret != -ENOENT && first_error == 0)
        {
          first_error = ret;
        }
    }

  ret = property_commit();
  return first_error != 0 ? first_error : ret;
}

FAR const char *mz_storage_backend_path(void)
{
#ifdef CONFIG_KVDB_PERSIST_PATH
  return CONFIG_KVDB_PERSIST_PATH;
#else
  return "/data/persist.db";
#endif
}
