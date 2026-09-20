/****************************************************************************
 * app/anki_importer/include/anki_importer.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef MOXINZHI_ANKI_IMPORTER_H
#define MOXINZHI_ANKI_IMPORTER_H

#include <stdbool.h>
#include <stdint.h>

#include "card_model.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ANKI_IMPORT_MESSAGE_MAX 192
#define ANKI_IMPORT_FS_ID_HEX_LEN 22

enum anki_import_result_e
{
  ANKI_IMPORT_RESULT_IMPORTED = 0,
  ANKI_IMPORT_RESULT_DUPLICATE = 1,
  ANKI_IMPORT_RESULT_REJECTED = 2,
  ANKI_IMPORT_RESULT_FAILED = 3
};

enum anki_import_warning_e
{
  ANKI_IMPORT_WARN_SCRIPT_REMOVED = 1U << 0,
  ANKI_IMPORT_WARN_LATEX_PRESENT = 1U << 1,
  ANKI_IMPORT_WARN_COMPLEX_MODEL = 1U << 2,
  ANKI_IMPORT_WARN_EXTRA_TEMPLATE = 1U << 3,
  ANKI_IMPORT_WARN_MEDIA_MISSING = 1U << 4,
  ANKI_IMPORT_WARN_MEDIA_INVALID = 1U << 5,
  ANKI_IMPORT_WARN_EXTERNAL_MEDIA = 1U << 6,
  ANKI_IMPORT_WARN_REPORT_IO = 1U << 7
};

struct anki_import_limits_s
{
  uint64_t max_package_bytes;
  uint64_t max_collection_bytes;
  uint64_t max_total_uncompressed_bytes;
  uint64_t max_media_file_bytes;
  uint32_t max_archive_entries;
  uint32_t max_cards;
  uint32_t max_field_bytes;
  uint32_t max_metadata_json_bytes;
  uint32_t max_media_map_bytes;
  uint32_t max_compression_ratio;
};

struct anki_import_options_s
{
  const char *source_path;
  const char *input_root;
  const char *store_path;
  const char *media_root;
  const char *work_root;
  const char *report_root;
  struct anki_import_limits_s limits;
};

struct anki_import_report_s
{
  enum anki_import_result_e result;
  enum card_model_source_format_e format;
  uint32_t warnings;
  uint32_t archive_entries;
  uint32_t cards_seen;
  uint32_t cards_imported;
  uint32_t cards_skipped;
  uint32_t decks_imported;
  uint32_t media_mapped;
  uint32_t media_imported;
  uint32_t media_skipped;
  uint64_t media_bytes;
  char fingerprint[CARD_MODEL_FINGERPRINT_HEX_LEN + 1];
  char message[ANKI_IMPORT_MESSAGE_MAX];
};

void anki_import_default_options(struct anki_import_options_s *options);
int anki_import_filesystem_id(
  const char *fingerprint,
  char filesystem_id[ANKI_IMPORT_FS_ID_HEX_LEN + 1]);
int anki_import_package(const struct anki_import_options_s *options,
                        struct anki_import_report_s *report);
int anki_import_write_report(const struct anki_import_options_s *options,
                             const struct anki_import_report_s *report);

#ifdef __cplusplus
}
#endif

#endif /* MOXINZHI_ANKI_IMPORTER_H */
