/****************************************************************************
 * app/anki_importer/anki_importer/anki_importer.c
 *
 * Board-side Anki 2.0/2.1 package importer for Gemini-S1 / R528 / OpenVela.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "anki_importer.h"

#include "anki_sha256.h"
#include "anki_text.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef ANKI_HOST_BUILD
#  include <cJSON.h>
#else
#  include <netutils/cJSON.h>
#endif

#include <contrib/minizip/unzip.h>
#include <sqlite3.h>

#ifndef PATH_MAX
#  define PATH_MAX 1024
#endif

#ifndef NAME_MAX
#  define NAME_MAX 255
#endif

#ifndef O_DIRECTORY
#  define O_DIRECTORY 0
#endif

#define ANKI_IO_BUFFER_SIZE 16384
#define ANKI_MEDIA_REFS_SIZE 4096
#define ANKI_ZIP_NAME_SIZE 512
#define ANKI_COLLECTION20 "collection.anki2"
#define ANKI_COLLECTION21 "collection.anki21"
#define ANKI_COLLECTION_LATEST "collection.anki21b"
#define ANKI_STAGING_PREFIX ".staging-"

#if 9 + ANKI_IMPORT_FS_ID_HEX_LEN > NAME_MAX
#  error "Anki filesystem ID does not fit NAME_MAX"
#endif

#if ANKI_IMPORT_FS_ID_HEX_LEN + 9 > NAME_MAX
#  error "Anki report filename does not fit NAME_MAX"
#endif

#ifdef CONFIG_MOXINZHI_ANKI_MAX_PACKAGE_MIB
#  define ANKI_DEFAULT_MAX_PACKAGE \
    ((uint64_t)CONFIG_MOXINZHI_ANKI_MAX_PACKAGE_MIB * 1024 * 1024)
#  define ANKI_DEFAULT_MAX_COLLECTION \
    ((uint64_t)CONFIG_MOXINZHI_ANKI_MAX_COLLECTION_MIB * 1024 * 1024)
#  define ANKI_DEFAULT_MAX_TOTAL \
    ((uint64_t)CONFIG_MOXINZHI_ANKI_MAX_TOTAL_MIB * 1024 * 1024)
#  define ANKI_DEFAULT_MAX_MEDIA \
    ((uint64_t)CONFIG_MOXINZHI_ANKI_MAX_MEDIA_FILE_MIB * 1024 * 1024)
#  define ANKI_DEFAULT_MAX_ENTRIES CONFIG_MOXINZHI_ANKI_MAX_ENTRIES
#  define ANKI_DEFAULT_MAX_CARDS CONFIG_MOXINZHI_ANKI_MAX_CARDS
#  define ANKI_DEFAULT_MAX_FIELD CONFIG_MOXINZHI_ANKI_MAX_FIELD_BYTES
#  define ANKI_DEFAULT_MAX_META CONFIG_MOXINZHI_ANKI_MAX_METADATA_KIB * 1024U
#  define ANKI_DEFAULT_MAX_MEDIA_MAP \
    CONFIG_MOXINZHI_ANKI_MAX_MEDIA_MAP_KIB * 1024U
#  define ANKI_DEFAULT_MAX_RATIO CONFIG_MOXINZHI_ANKI_MAX_COMPRESSION_RATIO
#else
#  define ANKI_DEFAULT_MAX_PACKAGE (512ULL * 1024 * 1024)
#  define ANKI_DEFAULT_MAX_COLLECTION (64ULL * 1024 * 1024)
#  define ANKI_DEFAULT_MAX_TOTAL (768ULL * 1024 * 1024)
#  define ANKI_DEFAULT_MAX_MEDIA (32ULL * 1024 * 1024)
#  define ANKI_DEFAULT_MAX_ENTRIES 20000U
#  define ANKI_DEFAULT_MAX_CARDS 100000U
#  define ANKI_DEFAULT_MAX_FIELD 32768U
#  define ANKI_DEFAULT_MAX_META (1024U * 1024)
#  define ANKI_DEFAULT_MAX_MEDIA_MAP (2048U * 1024)
#  define ANKI_DEFAULT_MAX_RATIO 250U
#endif

struct anki_import_ctx_s
{
  const struct anki_import_options_s *options;
  struct anki_import_report_s *report;
  struct card_model_s *model;
  unzFile archive;
  sqlite3 *source_db;
  cJSON *models;
  cJSON *decks;
  cJSON *media_map;
  bool transaction_active;
  bool staging_record_written;
  bool media_renamed;
  uint64_t imported_at_ms;
  uint32_t media_selected;
  char collection_entry[32];
  char source_basename[192];
  char work_dir[PATH_MAX];
  char collection_path[PATH_MAX];
  char media_staging[PATH_MAX];
  char media_final[PATH_MAX];
};

static void anki_report_message(struct anki_import_report_s *report,
                                const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vsnprintf(report->message, sizeof(report->message), format, args);
  va_end(args);
}

int anki_import_filesystem_id(
  const char *fingerprint,
  char filesystem_id[ANKI_IMPORT_FS_ID_HEX_LEN + 1])
{
  size_t half = ANKI_IMPORT_FS_ID_HEX_LEN / 2;
  size_t index;

  if (fingerprint == NULL || filesystem_id == NULL ||
      strlen(fingerprint) != CARD_MODEL_FINGERPRINT_HEX_LEN)
    {
      return -EINVAL;
    }

  for (index = 0; index < CARD_MODEL_FINGERPRINT_HEX_LEN; index++)
    {
      if (strchr("0123456789abcdefABCDEF", fingerprint[index]) == NULL)
        {
          return -EINVAL;
        }
    }

  memcpy(filesystem_id, fingerprint, half);
  memcpy(filesystem_id + half,
         fingerprint + CARD_MODEL_FINGERPRINT_HEX_LEN - half, half);
  filesystem_id[ANKI_IMPORT_FS_ID_HEX_LEN] = '\0';
  return 0;
}

static uint64_t anki_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
    {
      return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    }

  return (uint64_t)time(NULL) * 1000;
}

static int anki_join_path(char *output, size_t output_size,
                          const char *left, const char *right)
{
  int written;
  size_t left_size;

  if (output == NULL || output_size == 0 || left == NULL || right == NULL)
    {
      return -EINVAL;
    }

  left_size = strlen(left);
  written = snprintf(output, output_size,
                     left_size > 0 && left[left_size - 1] == '/' ?
                     "%s%s" : "%s/%s", left, right);
  if (written < 0 || (size_t)written >= output_size)
    {
      return -ENAMETOOLONG;
    }

  return 0;
}

static int anki_mkdirs(const char *path)
{
  char buffer[PATH_MAX];
  char *cursor;
  size_t length;

  if (path == NULL || path[0] == '\0')
    {
      return -EINVAL;
    }

  length = strlen(path);
  if (length >= sizeof(buffer))
    {
      return -ENAMETOOLONG;
    }

  memcpy(buffer, path, length + 1);
  for (cursor = buffer + 1; *cursor != '\0'; cursor++)
    {
      if (*cursor == '/')
        {
          *cursor = '\0';
          if (mkdir(buffer, 0700) < 0 && errno != EEXIST)
            {
              return -errno;
            }

          *cursor = '/';
        }
    }

  if (mkdir(buffer, 0700) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return 0;
}

static int anki_parent_dir(const char *path, char *parent, size_t parent_size)
{
  const char *slash;
  size_t size;

  slash = strrchr(path, '/');
  if (slash == NULL || slash == path)
    {
      if (slash == path)
        {
          if (parent_size < 2)
            {
              return -ENOSPC;
            }

          strcpy(parent, "/");
          return 0;
        }

      if (parent_size < 2)
        {
          return -ENOSPC;
        }

      strcpy(parent, ".");
      return 0;
    }

  size = (size_t)(slash - path);
  if (size + 1 > parent_size)
    {
      return -ENAMETOOLONG;
    }

  memcpy(parent, path, size);
  parent[size] = '\0';
  return 0;
}

static int anki_fsync_dir(const char *path)
{
  int fd;
  int result;

  fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    {
      return errno == ENOTSUP || errno == EINVAL ? 0 : -errno;
    }

  result = fsync(fd);
  if (result < 0 && errno != ENOTSUP && errno != EINVAL)
    {
      result = -errno;
    }
  else
    {
      result = 0;
    }

  close(fd);
  return result;
}

static int anki_remove_tree(const char *path)
{
  struct stat status;
  struct dirent *entry;
  DIR *directory;
  char child[PATH_MAX];
  int result = 0;

  if (lstat(path, &status) < 0)
    {
      return errno == ENOENT ? 0 : -errno;
    }

  if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode))
    {
      return unlink(path) < 0 ? -errno : 0;
    }

  directory = opendir(path);
  if (directory == NULL)
    {
      return -errno;
    }

  while ((entry = readdir(directory)) != NULL)
    {
      if (strcmp(entry->d_name, ".") == 0 ||
          strcmp(entry->d_name, "..") == 0)
        {
          continue;
        }

      result = anki_join_path(child, sizeof(child), path, entry->d_name);
      if (result < 0)
        {
          break;
        }

      result = anki_remove_tree(child);
      if (result < 0)
        {
          break;
        }
    }

  closedir(directory);
  if (result < 0)
    {
      return result;
    }

  return rmdir(path) < 0 && errno != ENOENT ? -errno : 0;
}

static bool anki_has_suffix(const char *text, const char *suffix)
{
  size_t text_size = strlen(text);
  size_t suffix_size = strlen(suffix);

  return text_size >= suffix_size &&
         strcmp(text + text_size - suffix_size, suffix) == 0;
}

static int anki_validate_source_path(const struct anki_import_options_s *options,
                                     char *basename, size_t basename_size)
{
  size_t root_size;
  const char *relative;

  if (options->source_path == NULL || options->input_root == NULL ||
      options->input_root[0] == '\0')
    {
      return -EINVAL;
    }

  root_size = strlen(options->input_root);
  if (strncmp(options->source_path, options->input_root, root_size) != 0 ||
      (options->source_path[root_size] != '/' &&
       !(root_size > 0 && options->input_root[root_size - 1] == '/')))
    {
      return -EACCES;
    }

  relative = options->source_path + root_size;
  if (*relative == '/')
    {
      relative++;
    }

  if (*relative == '\0' || strchr(relative, '/') != NULL ||
      strchr(relative, '\\') != NULL || strstr(relative, "..") != NULL ||
      !anki_has_suffix(relative, ".apkg"))
    {
      return -EACCES;
    }

  if (strlen(relative) >= basename_size)
    {
      return -ENAMETOOLONG;
    }

  strcpy(basename, relative);
  return 0;
}

static int anki_fingerprint(const struct anki_import_options_s *options,
                            char output[CARD_MODEL_FINGERPRINT_HEX_LEN + 1])
{
  struct anki_sha256_s sha256;
  struct stat status;
  uint8_t digest[32];
  uint8_t buffer[ANKI_IO_BUFFER_SIZE];
  static const char hex[] = "0123456789abcdef";
  FILE *file;
  size_t count;
  size_t i;

  if (lstat(options->source_path, &status) < 0)
    {
      return -errno;
    }

  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      (uint64_t)status.st_size > options->limits.max_package_bytes)
    {
      return -EFBIG;
    }

  file = fopen(options->source_path, "rb");
  if (file == NULL)
    {
      return -errno;
    }

  anki_sha256_init(&sha256);
  while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
      anki_sha256_update(&sha256, buffer, count);
    }

  if (ferror(file))
    {
      int error = errno == 0 ? EIO : errno;
      fclose(file);
      return -error;
    }

  fclose(file);
  anki_sha256_final(&sha256, digest);
  for (i = 0; i < sizeof(digest); i++)
    {
      output[i * 2] = hex[digest[i] >> 4];
      output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }

  output[CARD_MODEL_FINGERPRINT_HEX_LEN] = '\0';
  return 0;
}

static bool anki_zip_name_has_traversal(const char *name)
{
  const char *segment = name;
  const char *cursor;
  size_t size;

  if (name[0] == '/' || name[0] == '\\' || strchr(name, '\\') != NULL ||
      strchr(name, ':') != NULL)
    {
      return true;
    }

  for (cursor = name; ; cursor++)
    {
      if (*cursor == '/' || *cursor == '\0')
        {
          size = (size_t)(cursor - segment);
          if (size == 0 || (size == 1 && segment[0] == '.') ||
              (size == 2 && segment[0] == '.' && segment[1] == '.'))
            {
              return true;
            }

          if (*cursor == '\0')
            {
              break;
            }

          segment = cursor + 1;
        }
    }

  return false;
}

static int anki_zip_current_info(unzFile archive, unz_file_info64 *info,
                                 char *name, size_t name_size)
{
  int result;

  memset(info, 0, sizeof(*info));
  result = unzGetCurrentFileInfo64(archive, info, name,
                                   (uLong)name_size, NULL, 0, NULL, 0);
  if (result != UNZ_OK)
    {
      return -EBADMSG;
    }

  if (info->size_filename == 0 || info->size_filename >= name_size)
    {
      return -ENAMETOOLONG;
    }

  name[info->size_filename] = '\0';
  if (strlen(name) != info->size_filename)
    {
      return -EBADMSG;
    }

  return 0;
}

static int anki_zip_scan(struct anki_import_ctx_s *ctx)
{
  unz_global_info64 global;
  unz_file_info64 info;
  char name[ANKI_ZIP_NAME_SIZE];
  uint64_t total = 0;
  uint64_t ratio;
  ZPOS64_T i;
  int result;

  if (unzGetGlobalInfo64(ctx->archive, &global) != UNZ_OK ||
      global.number_entry == 0 ||
      global.number_entry > ctx->options->limits.max_archive_entries)
    {
      return -EFBIG;
    }

  ctx->report->archive_entries = (uint32_t)global.number_entry;
  result = unzGoToFirstFile(ctx->archive);
  for (i = 0; i < global.number_entry && result == UNZ_OK; i++)
    {
      result = anki_zip_current_info(ctx->archive, &info, name, sizeof(name));
      if (result < 0)
        {
          return result;
        }

      if (anki_zip_name_has_traversal(name) || (info.flag & 1U) != 0 ||
          (info.compression_method != 0 && info.compression_method != 8))
        {
          return -EPERM;
        }

      if (UINT64_MAX - total < info.uncompressed_size)
        {
          return -EFBIG;
        }

      total += info.uncompressed_size;
      if (total > ctx->options->limits.max_total_uncompressed_bytes)
        {
          return -EFBIG;
        }

      if (info.uncompressed_size > 0)
        {
          if (info.compressed_size == 0)
            {
              return -EFBIG;
            }

          ratio = (uint64_t)info.uncompressed_size /
                  (uint64_t)info.compressed_size;
          if (ctx->options->limits.max_compression_ratio == 0 ||
              ratio > ctx->options->limits.max_compression_ratio ||
              (ratio == ctx->options->limits.max_compression_ratio &&
               info.uncompressed_size % info.compressed_size != 0))
            {
              return -EFBIG;
            }
        }

      if (i + 1 < global.number_entry)
        {
          result = unzGoToNextFile(ctx->archive);
        }
    }

  return result == UNZ_OK ? 0 : -EBADMSG;
}

static int anki_zip_has_entry(unzFile archive, const char *name)
{
  return unzLocateFile(archive, name, 1) == UNZ_OK;
}

static int anki_detect_format(struct anki_import_ctx_s *ctx)
{
  if (anki_zip_has_entry(ctx->archive, ANKI_COLLECTION_LATEST))
    {
      anki_report_message(ctx->report,
                          "new-style collection.anki21b/zstd package is not "
                          "supported by the minimal board importer");
      return -EPROTONOSUPPORT;
    }

  if (anki_zip_has_entry(ctx->archive, ANKI_COLLECTION21))
    {
      strcpy(ctx->collection_entry, ANKI_COLLECTION21);
      ctx->report->format = CARD_MODEL_SOURCE_ANKI21;
      return 0;
    }

  if (anki_zip_has_entry(ctx->archive, ANKI_COLLECTION20))
    {
      strcpy(ctx->collection_entry, ANKI_COLLECTION20);
      ctx->report->format = CARD_MODEL_SOURCE_ANKI20;
      return 0;
    }

  anki_report_message(ctx->report, "collection.anki2/collection.anki21 missing");
  return -EBADMSG;
}

static int anki_write_all(int fd, const uint8_t *data, size_t size)
{
  ssize_t written;

  while (size > 0)
    {
      written = write(fd, data, size);
      if (written < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (written == 0)
        {
          return -EIO;
        }

      data += written;
      size -= (size_t)written;
    }

  return 0;
}

static int anki_extract_current(struct anki_import_ctx_s *ctx,
                                const char *path, uint64_t limit,
                                uint64_t *written_bytes)
{
  unz_file_info64 info;
  char name[ANKI_ZIP_NAME_SIZE];
  uint8_t buffer[ANKI_IO_BUFFER_SIZE];
  uint64_t total = 0;
  int fd = -1;
  int count = 0;
  int result;
  int close_result;

  result = anki_zip_current_info(ctx->archive, &info, name, sizeof(name));
  if (result < 0)
    {
      return result;
    }

  if (info.uncompressed_size > limit)
    {
      return -EFBIG;
    }

  result = unzOpenCurrentFile(ctx->archive);
  if (result != UNZ_OK)
    {
      return -EBADMSG;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    {
      unzCloseCurrentFile(ctx->archive);
      return -errno;
    }

  while ((count = unzReadCurrentFile(ctx->archive, buffer,
                                     sizeof(buffer))) > 0)
    {
      if (total + (uint64_t)count > limit)
        {
          result = -EFBIG;
          break;
        }

      result = anki_write_all(fd, buffer, (size_t)count);
      if (result < 0)
        {
          break;
        }

      total += (uint64_t)count;
    }

  if (count < 0 && result >= 0)
    {
      result = -EBADMSG;
    }

  if (result >= 0 && fsync(fd) < 0)
    {
      result = -errno;
    }

  close(fd);
  close_result = unzCloseCurrentFile(ctx->archive);
  if (result >= 0 && close_result != UNZ_OK)
    {
      result = -EBADMSG;
    }

  if (result < 0 || total != info.uncompressed_size)
    {
      unlink(path);
      return result < 0 ? result : -EBADMSG;
    }

  if (written_bytes != NULL)
    {
      *written_bytes = total;
    }

  return 0;
}

static int anki_read_current(struct anki_import_ctx_s *ctx, uint64_t limit,
                             uint8_t **data, size_t *size)
{
  unz_file_info64 info;
  char name[ANKI_ZIP_NAME_SIZE];
  uint8_t *buffer;
  size_t used = 0;
  int count = 0;
  int result;
  int close_result;

  result = anki_zip_current_info(ctx->archive, &info, name, sizeof(name));
  if (result < 0)
    {
      return result;
    }

  if (info.uncompressed_size > limit || info.uncompressed_size > SIZE_MAX - 1)
    {
      return -EFBIG;
    }

  buffer = malloc((size_t)info.uncompressed_size + 1);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  result = unzOpenCurrentFile(ctx->archive);
  if (result != UNZ_OK)
    {
      free(buffer);
      return -EBADMSG;
    }

  while (used < (size_t)info.uncompressed_size &&
         (count = unzReadCurrentFile(ctx->archive, buffer + used,
                                     (unsigned int)
                                     ((size_t)info.uncompressed_size -
                                      used))) > 0)
    {
      used += (size_t)count;
    }

  close_result = unzCloseCurrentFile(ctx->archive);
  if (used != (size_t)info.uncompressed_size || count < 0 ||
      close_result != UNZ_OK)
    {
      free(buffer);
      return -EBADMSG;
    }

  buffer[used] = '\0';
  *data = buffer;
  *size = used;
  return 0;
}

static int anki_prepare_paths(struct anki_import_ctx_s *ctx)
{
  char filesystem_id[ANKI_IMPORT_FS_ID_HEX_LEN + 1];
  char staging_name[sizeof(ANKI_STAGING_PREFIX) +
                    ANKI_IMPORT_FS_ID_HEX_LEN];
  char parent[PATH_MAX];
  int result;

  result = anki_import_filesystem_id(ctx->report->fingerprint,
                                     filesystem_id);
  if (result < 0)
    {
      return result;
    }

  result = anki_mkdirs(ctx->options->work_root);
  if (result < 0)
    {
      return result;
    }

  result = anki_mkdirs(ctx->options->media_root);
  if (result < 0)
    {
      return result;
    }

  result = anki_mkdirs(ctx->options->report_root);
  if (result < 0)
    {
      return result;
    }

  result = anki_parent_dir(ctx->options->store_path, parent, sizeof(parent));
  if (result < 0)
    {
      return result;
    }

  result = anki_mkdirs(parent);
  if (result < 0)
    {
      return result;
    }

  result = anki_join_path(ctx->work_dir, sizeof(ctx->work_dir),
                          ctx->options->work_root,
                          filesystem_id);
  if (result < 0)
    {
      return result;
    }

  result = anki_join_path(ctx->collection_path, sizeof(ctx->collection_path),
                          ctx->work_dir, "collection.sqlite");
  if (result < 0)
    {
      return result;
    }

  snprintf(staging_name, sizeof(staging_name), ANKI_STAGING_PREFIX "%s",
           filesystem_id);
  result = anki_join_path(ctx->media_staging, sizeof(ctx->media_staging),
                          ctx->options->media_root, staging_name);
  if (result < 0)
    {
      return result;
    }

  result = anki_join_path(ctx->media_final, sizeof(ctx->media_final),
                          ctx->options->media_root,
                          filesystem_id);
  if (result < 0)
    {
      return result;
    }

  return 0;
}

static int anki_reset_import_paths(struct anki_import_ctx_s *ctx)
{
  int result;

  result = anki_remove_tree(ctx->work_dir);
  if (result == 0)
    {
      result = anki_remove_tree(ctx->media_staging);
    }

  if (result == 0)
    {
      result = anki_remove_tree(ctx->media_final);
    }

  if (result == 0)
    {
      result = anki_mkdirs(ctx->work_dir);
    }

  if (result == 0)
    {
      result = anki_mkdirs(ctx->media_staging);
    }

  return result;
}

static int anki_source_db_open(struct anki_import_ctx_s *ctx)
{
  sqlite3_stmt *statement = NULL;
  const unsigned char *check;
  int result;

  result = sqlite3_open_v2(ctx->collection_path, &ctx->source_db,
                           SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
  if (result != SQLITE_OK)
    {
      return -EBADMSG;
    }

  sqlite3_busy_timeout(ctx->source_db, 1000);
  result = sqlite3_exec(ctx->source_db,
                        "PRAGMA query_only=ON;"
                        "PRAGMA trusted_schema=OFF;"
                        "PRAGMA cache_size=-256;"
                        "PRAGMA mmap_size=0;",
                        NULL, NULL, NULL);
  if (result != SQLITE_OK)
    {
      return -EBADMSG;
    }

  result = sqlite3_prepare_v2(ctx->source_db, "PRAGMA quick_check(1)", -1,
                              &statement, NULL);
  if (result != SQLITE_OK || sqlite3_step(statement) != SQLITE_ROW)
    {
      sqlite3_finalize(statement);
      return -EBADMSG;
    }

  check = sqlite3_column_text(statement, 0);
  result = check != NULL && strcmp((const char *)check, "ok") == 0 ?
           0 : -EBADMSG;
  sqlite3_finalize(statement);
  return result;
}

static cJSON *anki_parse_json(const char *data, size_t size)
{
  return cJSON_ParseWithLengthOpts(data, size + 1, NULL, true);
}

static int anki_load_collection_json(struct anki_import_ctx_s *ctx)
{
  sqlite3_stmt *statement = NULL;
  const char *models;
  const char *decks;
  int models_size;
  int decks_size;
  int result;

  result = sqlite3_prepare_v2(ctx->source_db,
                              "SELECT models, decks FROM col LIMIT 1", -1,
                              &statement, NULL);
  if (result != SQLITE_OK || sqlite3_step(statement) != SQLITE_ROW)
    {
      sqlite3_finalize(statement);
      return -EBADMSG;
    }

  models = (const char *)sqlite3_column_text(statement, 0);
  decks = (const char *)sqlite3_column_text(statement, 1);
  models_size = sqlite3_column_bytes(statement, 0);
  decks_size = sqlite3_column_bytes(statement, 1);
  if (models == NULL || decks == NULL || models_size <= 0 || decks_size <= 0 ||
      (uint32_t)models_size > ctx->options->limits.max_metadata_json_bytes ||
      (uint32_t)decks_size > ctx->options->limits.max_metadata_json_bytes)
    {
      sqlite3_finalize(statement);
      return -EFBIG;
    }

  ctx->models = anki_parse_json(models, (size_t)models_size);
  ctx->decks = anki_parse_json(decks, (size_t)decks_size);
  sqlite3_finalize(statement);
  if (!cJSON_IsObject(ctx->models) || !cJSON_IsObject(ctx->decks))
    {
      return -EBADMSG;
    }

  return 0;
}

static cJSON *anki_json_id(cJSON *root, int64_t id)
{
  char id_string[32];

  snprintf(id_string, sizeof(id_string), "%" PRId64, id);
  return cJSON_GetObjectItemCaseSensitive(root, id_string);
}

static bool anki_model_supported(struct anki_import_ctx_s *ctx, int64_t mid)
{
  cJSON *model = anki_json_id(ctx->models, mid);
  cJSON *type;
  cJSON *fields;
  cJSON *templates;
  cJSON *first_template;
  cJSON *question;
  cJSON *answer;
  int template_count;

  if (!cJSON_IsObject(model))
    {
      return false;
    }

  type = cJSON_GetObjectItemCaseSensitive(model, "type");
  fields = cJSON_GetObjectItemCaseSensitive(model, "flds");
  templates = cJSON_GetObjectItemCaseSensitive(model, "tmpls");
  if (!cJSON_IsNumber(type) || type->valueint != 0 ||
      !cJSON_IsArray(fields) || cJSON_GetArraySize(fields) != 2 ||
      !cJSON_IsArray(templates) || cJSON_GetArraySize(templates) < 1)
    {
      return false;
    }

  template_count = cJSON_GetArraySize(templates);
  if (template_count > 1)
    {
      ctx->report->warnings |= ANKI_IMPORT_WARN_EXTRA_TEMPLATE;
    }

  first_template = cJSON_GetArrayItem(templates, 0);
  question = cJSON_GetObjectItemCaseSensitive(first_template, "qfmt");
  answer = cJSON_GetObjectItemCaseSensitive(first_template, "afmt");
  if (!cJSON_IsString(question) || !cJSON_IsString(answer) ||
      strstr(question->valuestring, "{{cloze:") != NULL ||
      strstr(answer->valuestring, "{{cloze:") != NULL ||
      strstr(question->valuestring, "<script") != NULL ||
      strstr(answer->valuestring, "<script") != NULL)
    {
      return false;
    }

  return true;
}

static const char *anki_deck_name(struct anki_import_ctx_s *ctx, int64_t did)
{
  cJSON *deck = anki_json_id(ctx->decks, did);
  cJSON *name;

  if (!cJSON_IsObject(deck))
    {
      return "Imported Anki";
    }

  name = cJSON_GetObjectItemCaseSensitive(deck, "name");
  return cJSON_IsString(name) && name->valuestring[0] != '\0' &&
         strlen(name->valuestring) <=
         ctx->options->limits.max_field_bytes ?
         name->valuestring : "Imported Anki";
}

static int anki_import_decks(struct anki_import_ctx_s *ctx)
{
  cJSON *deck;
  cJSON *name;
  char *end;
  int64_t did;
  int result;

  cJSON_ArrayForEach(deck, ctx->decks)
    {
      if (deck->string == NULL)
        {
          continue;
        }

      errno = 0;
      did = (int64_t)strtoll(deck->string, &end, 10);
      name = cJSON_GetObjectItemCaseSensitive(deck, "name");
      if (errno != 0 || end == deck->string || *end != '\0' ||
          !cJSON_IsString(name) ||
          strlen(name->valuestring) >
          ctx->options->limits.max_field_bytes)
        {
          continue;
        }

      result = card_model_put_deck(ctx->model, ctx->report->fingerprint,
                                   did, name->valuestring);
      if (result < 0)
        {
          return result;
        }

      ctx->report->decks_imported++;
    }

  return 0;
}

static int anki_merge_refs(char *output, size_t output_size,
                           const char *front_refs, const char *back_refs)
{
  const char *sets[2] = {front_refs, back_refs};
  const char *cursor;
  const char *newline;
  size_t used = 0;
  size_t size;
  size_t i;
  size_t existing_offset;
  size_t existing_size;
  bool present;

  output[0] = '\0';
  for (i = 0; i < 2; i++)
    {
      cursor = sets[i];
      while (*cursor != '\0')
        {
          newline = strchr(cursor, '\n');
          size = newline == NULL ? strlen(cursor) :
                                  (size_t)(newline - cursor);
          present = false;
          existing_offset = 0;
          while (existing_offset < used)
            {
              existing_size = strcspn(output + existing_offset, "\n");
              if (existing_size == size &&
                  memcmp(output + existing_offset, cursor, size) == 0)
                {
                  present = true;
                  break;
                }

              existing_offset += existing_size + 1;
            }

          if (size > 0 && !present)
            {
              if (used + size + (used == 0 ? 1 : 2) > output_size)
                {
                  return -ENOSPC;
                }

              if (used > 0)
                {
                  output[used++] = '\n';
                }

              memcpy(output + used, cursor, size);
              used += size;
              output[used] = '\0';
            }

          if (newline == NULL)
            {
              break;
            }

          cursor = newline + 1;
        }
    }

  return 0;
}

static int anki_mark_refs(struct anki_import_ctx_s *ctx, const char *refs)
{
  const char *cursor = refs;
  const char *newline;
  char name[CARD_MODEL_MAX_MEDIA_NAME + 1];
  size_t size;
  int result;

  while (*cursor != '\0')
    {
      newline = strchr(cursor, '\n');
      size = newline == NULL ? strlen(cursor) : (size_t)(newline - cursor);
      if (size > CARD_MODEL_MAX_MEDIA_NAME)
        {
          return -ENAMETOOLONG;
        }

      memcpy(name, cursor, size);
      name[size] = '\0';
      result = card_model_mark_media_reference(ctx->model,
                                               ctx->report->fingerprint,
                                               name);
      if (result < 0)
        {
          return result;
        }

      if (newline == NULL)
        {
          break;
        }

      cursor = newline + 1;
    }

  return 0;
}

static int anki_import_cards(struct anki_import_ctx_s *ctx)
{
  static const char query[] =
    "SELECT cards.id, notes.id, notes.mid, cards.did, notes.tags, notes.flds "
    "FROM cards JOIN notes ON notes.id=cards.nid "
    "WHERE cards.ord=0 ORDER BY cards.id";
  struct card_model_card_s card;
  sqlite3_stmt *statement = NULL;
  const char *fields;
  const char *separator;
  const char *second_separator;
  const char *tags;
  const char *deck_name;
  char *front = NULL;
  char *back = NULL;
  char front_refs[ANKI_MEDIA_REFS_SIZE];
  char back_refs[ANKI_MEDIA_REFS_SIZE];
  char refs[ANKI_MEDIA_REFS_SIZE];
  size_t front_size;
  size_t back_size;
  int fields_size;
  int tags_size;
  int step;
  int result = 0;
  uint32_t front_flags;
  uint32_t back_flags;

  front = malloc((size_t)ctx->options->limits.max_field_bytes + 1);
  back = malloc((size_t)ctx->options->limits.max_field_bytes + 1);
  if (front == NULL || back == NULL)
    {
      result = -ENOMEM;
      goto done;
    }

  if (sqlite3_prepare_v2(ctx->source_db, query, -1, &statement, NULL) !=
      SQLITE_OK)
    {
      result = -EBADMSG;
      goto done;
    }

  while ((step = sqlite3_step(statement)) == SQLITE_ROW)
    {
      ctx->report->cards_seen++;
      if (ctx->report->cards_seen > ctx->options->limits.max_cards)
        {
          result = -EFBIG;
          goto done;
        }

      memset(&card, 0, sizeof(card));
      card.source_card_id = sqlite3_column_int64(statement, 0);
      card.source_note_id = sqlite3_column_int64(statement, 1);
      card.source_model_id = sqlite3_column_int64(statement, 2);
      card.source_deck_id = sqlite3_column_int64(statement, 3);
      tags = (const char *)sqlite3_column_text(statement, 4);
      fields = (const char *)sqlite3_column_text(statement, 5);
      tags_size = sqlite3_column_bytes(statement, 4);
      fields_size = sqlite3_column_bytes(statement, 5);

      if (!anki_model_supported(ctx, card.source_model_id) ||
          fields == NULL || fields_size <= 0 || tags_size < 0 ||
          (uint32_t)tags_size > ctx->options->limits.max_field_bytes)
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_COMPLEX_MODEL;
          ctx->report->cards_skipped++;
          continue;
        }

      separator = memchr(fields, 0x1f, (size_t)fields_size);
      if (separator == NULL)
        {
          ctx->report->cards_skipped++;
          continue;
        }

      front_size = (size_t)(separator - fields);
      back_size = (size_t)fields_size - front_size - 1;
      second_separator = memchr(separator + 1, 0x1f, back_size);
      if (second_separator != NULL ||
          front_size > ctx->options->limits.max_field_bytes ||
          back_size > ctx->options->limits.max_field_bytes)
        {
          ctx->report->cards_skipped++;
          continue;
        }

      result = anki_text_clean(fields, front_size, front,
                               (size_t)ctx->options->limits.max_field_bytes + 1,
                               front_refs, sizeof(front_refs), &front_flags);
      if (result == 0)
        {
          result = anki_text_clean(separator + 1, back_size, back,
                                   (size_t)ctx->options->limits.max_field_bytes +
                                   1, back_refs, sizeof(back_refs),
                                   &back_flags);
        }

      if (result == -ENOSPC)
        {
          result = 0;
          ctx->report->cards_skipped++;
          continue;
        }
      else if (result < 0)
        {
          goto done;
        }

      if (front[0] == '\0' && back[0] == '\0')
        {
          ctx->report->cards_skipped++;
          continue;
        }

      result = anki_merge_refs(refs, sizeof(refs), front_refs, back_refs);
      if (result < 0)
        {
          ctx->report->cards_skipped++;
          result = 0;
          continue;
        }

      result = anki_mark_refs(ctx, refs);
      if (result < 0)
        {
          goto done;
        }

      if ((front_flags | back_flags) & ANKI_TEXT_FLAG_SCRIPT_REMOVED)
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_SCRIPT_REMOVED;
        }

      if ((front_flags | back_flags) & ANKI_TEXT_FLAG_LATEX_PRESENT)
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_LATEX_PRESENT;
        }

      if ((front_flags | back_flags) & ANKI_TEXT_FLAG_EXTERNAL_MEDIA)
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_EXTERNAL_MEDIA;
        }

      if ((front_flags | back_flags) & ANKI_TEXT_FLAG_BAD_MEDIA_NAME)
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_MEDIA_INVALID;
        }

      deck_name = anki_deck_name(ctx, card.source_deck_id);
      card.imported_at_ms = ctx->imported_at_ms;
      card.front = front;
      card.back = back;
      card.deck_name = deck_name;
      card.tags = tags == NULL ? "" : tags;
      card.media_refs = refs;
      result = card_model_put_card(ctx->model, ctx->report->fingerprint,
                                   &card);
      if (result < 0)
        {
          goto done;
        }

      ctx->report->cards_imported++;
    }

  if (step != SQLITE_DONE)
    {
      result = -EBADMSG;
    }

done:
  sqlite3_finalize(statement);
  free(front);
  free(back);
  return result;
}

static bool anki_parse_zip_index(const char *text, uint32_t *index)
{
  unsigned long value;
  char *end;
  const char *cursor;

  if (text == NULL || text[0] == '\0' ||
      (text[0] == '0' && text[1] != '\0'))
    {
      return false;
    }

  for (cursor = text; *cursor != '\0'; cursor++)
    {
      if (*cursor < '0' || *cursor > '9')
        {
          return false;
        }
    }

  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || *end != '\0' || value > UINT32_MAX)
    {
      return false;
    }

  *index = (uint32_t)value;
  return true;
}

static int anki_import_media_map(struct anki_import_ctx_s *ctx)
{
  struct card_model_media_s media;
  struct card_model_media_s existing;
  cJSON *map;
  cJSON *entry;
  uint8_t *data;
  size_t data_size;
  uint32_t zip_index;
  bool referenced;
  int result;

  if (!anki_zip_has_entry(ctx->archive, "media"))
    {
      return 0;
    }

  result = anki_read_current(ctx, ctx->options->limits.max_media_map_bytes,
                             &data, &data_size);
  if (result < 0)
    {
      return result;
    }

  map = anki_parse_json((const char *)data, data_size);
  free(data);
  if (!cJSON_IsObject(map))
    {
      cJSON_Delete(map);
      return -EBADMSG;
    }

  cJSON_ArrayForEach(entry, map)
    {
      if (entry->string == NULL || !cJSON_IsString(entry) ||
          !anki_parse_zip_index(entry->string, &zip_index))
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_MEDIA_INVALID;
          ctx->report->media_skipped++;
          continue;
        }

      result = card_model_has_media_reference(ctx->model,
                                              ctx->report->fingerprint,
                                              entry->valuestring,
                                              &referenced);
      if (result == -EINVAL)
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_MEDIA_INVALID;
          ctx->report->media_skipped++;
          continue;
        }
      else if (result < 0)
        {
          cJSON_Delete(map);
          return result;
        }

      if (!referenced)
        {
          ctx->report->media_skipped++;
          continue;
        }

      result = card_model_get_media_by_zip_index(ctx->model,
                                                  ctx->report->fingerprint,
                                                  zip_index, &existing);
      if (result == 0)
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_MEDIA_INVALID;
          ctx->report->media_skipped++;
          continue;
        }
      else if (result != -ENOENT)
        {
          cJSON_Delete(map);
          return result;
        }

      result = card_model_get_media_by_name(ctx->model,
                                            ctx->report->fingerprint,
                                            entry->valuestring, &existing);
      if (result == 0)
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_MEDIA_INVALID;
          ctx->report->media_skipped++;
          continue;
        }
      else if (result != -ENOENT && result != -EINVAL)
        {
          cJSON_Delete(map);
          return result;
        }

      memset(&media, 0, sizeof(media));
      media.zip_index = zip_index;
      media.size = UINT64_MAX;
      if (strlen(entry->valuestring) > CARD_MODEL_MAX_MEDIA_NAME)
        {
          ctx->report->warnings |= ANKI_IMPORT_WARN_MEDIA_INVALID;
          ctx->report->media_skipped++;
          continue;
        }

      strcpy(media.name, entry->valuestring);
      result = snprintf(media.local_path, sizeof(media.local_path),
                        "%s/%" PRIu32, ctx->media_final, zip_index);
      if (result < 0 || (size_t)result >= sizeof(media.local_path))
        {
          cJSON_Delete(map);
          return -ENAMETOOLONG;
        }

      result = card_model_put_media(ctx->model, ctx->report->fingerprint,
                                    &media);
      if (result < 0)
        {
          cJSON_Delete(map);
          return result;
        }

      ctx->media_selected++;
      ctx->report->media_mapped++;
    }

  ctx->media_map = map;
  return 0;
}

static int anki_prune_missing_media(struct anki_import_ctx_s *ctx)
{
  struct card_model_media_s media;
  cJSON *entry;
  uint32_t zip_index;
  int result;

  if (ctx->media_map == NULL)
    {
      return 0;
    }

  cJSON_ArrayForEach(entry, ctx->media_map)
    {
      if (entry->string == NULL || !cJSON_IsString(entry) ||
          !anki_parse_zip_index(entry->string, &zip_index))
        {
          continue;
        }

      result = card_model_get_media_by_zip_index(ctx->model,
                                                  ctx->report->fingerprint,
                                                  zip_index, &media);
      if (result == -ENOENT)
        {
          continue;
        }
      else if (result < 0)
        {
          return result;
        }

      if (media.size == UINT64_MAX)
        {
          result = card_model_delete_media(ctx->model,
                                           ctx->report->fingerprint,
                                           &media);
          if (result < 0)
            {
              return result;
            }

          ctx->report->media_skipped++;
          ctx->report->warnings |= ANKI_IMPORT_WARN_MEDIA_MISSING;
        }
    }

  return 0;
}

static int anki_extract_media(struct anki_import_ctx_s *ctx)
{
  struct card_model_media_s media;
  unz_global_info64 global;
  unz_file_info64 info;
  char name[ANKI_ZIP_NAME_SIZE];
  char path[PATH_MAX];
  uint64_t size;
  uint32_t zip_index;
  ZPOS64_T i;
  int result;

  if (ctx->media_selected == 0)
    {
      return 0;
    }

  if (unzGetGlobalInfo64(ctx->archive, &global) != UNZ_OK ||
      unzGoToFirstFile(ctx->archive) != UNZ_OK)
    {
      return -EBADMSG;
    }

  for (i = 0; i < global.number_entry; i++)
    {
      result = anki_zip_current_info(ctx->archive, &info, name, sizeof(name));
      if (result < 0)
        {
          return result;
        }

      if (anki_parse_zip_index(name, &zip_index))
        {
          result = card_model_get_media_by_zip_index(
            ctx->model, ctx->report->fingerprint, zip_index, &media);
          if (result == 0)
            {
              if (info.uncompressed_size >
                  ctx->options->limits.max_media_file_bytes)
                {
                  return -EFBIG;
                }

              result = anki_join_path(path, sizeof(path), ctx->media_staging,
                                      name);
              if (result < 0)
                {
                  return result;
                }

              result = anki_extract_current(ctx, path,
                                            ctx->options->limits.
                                            max_media_file_bytes, &size);
              if (result < 0)
                {
                  return result;
                }

              media.size = size;
              result = card_model_put_media(ctx->model,
                                            ctx->report->fingerprint,
                                            &media);
              if (result < 0)
                {
                  return result;
                }

              ctx->report->media_imported++;
              ctx->report->media_bytes += size;
            }
          else if (result != -ENOENT)
            {
              return result;
            }
        }

      if (i + 1 < global.number_entry &&
          unzGoToNextFile(ctx->archive) != UNZ_OK)
        {
          return -EBADMSG;
        }
    }

  return anki_prune_missing_media(ctx);
}

static int anki_commit_media(struct anki_import_ctx_s *ctx)
{
  int result;

  result = anki_fsync_dir(ctx->media_staging);
  if (result < 0)
    {
      return result;
    }

  if (rename(ctx->media_staging, ctx->media_final) < 0)
    {
      return -errno;
    }

  ctx->media_renamed = true;
  return anki_fsync_dir(ctx->options->media_root);
}

static int anki_write_staging_record(struct anki_import_ctx_s *ctx)
{
  struct card_model_import_s record;
  int result;

  memset(&record, 0, sizeof(record));
  record.state = CARD_MODEL_IMPORT_STAGING;
  record.format = ctx->report->format;
  record.imported_at_ms = ctx->imported_at_ms;
  strncpy(record.source_name, ctx->source_basename,
          sizeof(record.source_name) - 1);

  result = card_model_begin(ctx->model);
  if (result == 0)
    {
      result = card_model_put_import(ctx->model, ctx->report->fingerprint,
                                     &record);
    }

  if (result == 0)
    {
      result = card_model_commit(ctx->model);
    }
  else
    {
      card_model_rollback(ctx->model);
    }

  if (result == 0)
    {
      ctx->staging_record_written = true;
    }

  return result;
}

static int anki_finish_import_record(struct anki_import_ctx_s *ctx)
{
  struct card_model_import_s record;

  memset(&record, 0, sizeof(record));
  record.state = CARD_MODEL_IMPORT_COMPLETE;
  record.format = ctx->report->format;
  record.imported_at_ms = ctx->imported_at_ms;
  record.card_count = ctx->report->cards_imported;
  record.deck_count = ctx->report->decks_imported;
  record.media_count = ctx->report->media_imported;
  record.card_skipped_count = ctx->report->cards_skipped;
  record.media_skipped_count = ctx->report->media_skipped;
  strncpy(record.source_name, ctx->source_basename,
          sizeof(record.source_name) - 1);
  return card_model_put_import(ctx->model, ctx->report->fingerprint, &record);
}

static void anki_cleanup_ctx(struct anki_import_ctx_s *ctx, bool failed)
{
  if (ctx->source_db != NULL)
    {
      sqlite3_close(ctx->source_db);
      ctx->source_db = NULL;
    }

  cJSON_Delete(ctx->models);
  cJSON_Delete(ctx->decks);
  cJSON_Delete(ctx->media_map);
  ctx->models = NULL;
  ctx->decks = NULL;
  ctx->media_map = NULL;

  if (ctx->archive != NULL)
    {
      unzClose(ctx->archive);
      ctx->archive = NULL;
    }

  if (ctx->transaction_active)
    {
      card_model_rollback(ctx->model);
      ctx->transaction_active = false;
    }

  unlink(ctx->collection_path);
  if (failed)
    {
      if (ctx->staging_record_written && ctx->model != NULL)
        {
          if (card_model_begin(ctx->model) == 0)
            {
              if (card_model_delete_import(ctx->model,
                                           ctx->report->fingerprint) == 0)
                {
                  card_model_commit(ctx->model);
                }
              else
                {
                  card_model_rollback(ctx->model);
                }
            }
        }

      anki_remove_tree(ctx->media_staging);
      if (ctx->media_renamed)
        {
          anki_remove_tree(ctx->media_final);
        }
    }

  anki_remove_tree(ctx->work_dir);
  card_model_close(ctx->model);
  ctx->model = NULL;
}

void anki_import_default_options(struct anki_import_options_s *options)
{
  memset(options, 0, sizeof(*options));
#ifdef CONFIG_MOXINZHI_ANKI_INPUT_ROOT
  options->input_root = CONFIG_MOXINZHI_ANKI_INPUT_ROOT;
  options->store_path = CONFIG_MOXINZHI_CARD_STORE_PATH;
  options->media_root = CONFIG_MOXINZHI_CARD_MEDIA_ROOT;
  options->work_root = CONFIG_MOXINZHI_ANKI_WORK_ROOT;
  options->report_root = CONFIG_MOXINZHI_ANKI_REPORT_ROOT;
#else
  options->input_root = "/data/import";
  options->store_path = "/data/moxinzhi/cards.unqlite";
  options->media_root = "/data/moxinzhi/media";
  options->work_root = "/data/moxinzhi/.import-work";
  options->report_root = "/data/moxinzhi/import-reports";
#endif
  options->limits.max_package_bytes = ANKI_DEFAULT_MAX_PACKAGE;
  options->limits.max_collection_bytes = ANKI_DEFAULT_MAX_COLLECTION;
  options->limits.max_total_uncompressed_bytes = ANKI_DEFAULT_MAX_TOTAL;
  options->limits.max_media_file_bytes = ANKI_DEFAULT_MAX_MEDIA;
  options->limits.max_archive_entries = ANKI_DEFAULT_MAX_ENTRIES;
  options->limits.max_cards = ANKI_DEFAULT_MAX_CARDS;
  options->limits.max_field_bytes = ANKI_DEFAULT_MAX_FIELD;
  options->limits.max_metadata_json_bytes = ANKI_DEFAULT_MAX_META;
  options->limits.max_media_map_bytes = ANKI_DEFAULT_MAX_MEDIA_MAP;
  options->limits.max_compression_ratio = ANKI_DEFAULT_MAX_RATIO;
}

int anki_import_package(const struct anki_import_options_s *options,
                        struct anki_import_report_s *report)
{
  struct anki_import_ctx_s ctx;
  struct card_model_import_s existing;
  int result;

  if (options == NULL || report == NULL || options->store_path == NULL ||
      options->media_root == NULL || options->work_root == NULL ||
      options->report_root == NULL)
    {
      return -EINVAL;
    }

  memset(&ctx, 0, sizeof(ctx));
  memset(report, 0, sizeof(*report));
  ctx.options = options;
  ctx.report = report;
  ctx.imported_at_ms = anki_now_ms();
  report->result = ANKI_IMPORT_RESULT_FAILED;

  result = anki_validate_source_path(options, ctx.source_basename,
                                     sizeof(ctx.source_basename));
  if (result < 0)
    {
      report->result = ANKI_IMPORT_RESULT_REJECTED;
      anki_report_message(report, "source must be a direct .apkg child of %s",
                          options->input_root);
      return result;
    }

  result = anki_fingerprint(options, report->fingerprint);
  if (result < 0)
    {
      report->result = result == -EFBIG ? ANKI_IMPORT_RESULT_REJECTED :
                                         ANKI_IMPORT_RESULT_FAILED;
      anki_report_message(report, "failed to fingerprint package: %d",
                          -result);
      return result;
    }

  result = anki_prepare_paths(&ctx);
  if (result < 0)
    {
      anki_report_message(report, "failed to prepare import directories: %d",
                          -result);
      goto done;
    }

  result = card_model_open(options->store_path, &ctx.model);
  if (result < 0)
    {
      anki_report_message(report, "failed to open card store: %d", -result);
      goto done;
    }

  result = card_model_get_import(ctx.model, report->fingerprint, &existing);
  if (result == 0 && existing.state == CARD_MODEL_IMPORT_COMPLETE)
    {
      report->result = ANKI_IMPORT_RESULT_DUPLICATE;
      report->format = existing.format;
      report->cards_imported = existing.card_count;
      report->decks_imported = existing.deck_count;
      report->media_imported = existing.media_count;
      report->cards_skipped = existing.card_skipped_count;
      report->media_skipped = existing.media_skipped_count;
      anki_report_message(report, "package already imported");
      result = 0;
      goto done;
    }
  else if (result == 0)
    {
      result = card_model_begin(ctx.model);
      if (result == 0)
        {
          result = card_model_delete_import(ctx.model,
                                            report->fingerprint);
          if (result == 0)
            {
              result = card_model_commit(ctx.model);
            }
          else
            {
              card_model_rollback(ctx.model);
            }
        }

      if (result < 0)
        {
          anki_report_message(report,
                              "failed to recover interrupted import: %d",
                              -result);
          goto done;
        }
    }
  else if (result != -ENOENT)
    {
      anki_report_message(report, "failed to inspect import state: %d",
                          -result);
      goto done;
    }

  result = anki_reset_import_paths(&ctx);
  if (result < 0)
    {
      anki_report_message(report, "failed to reset import staging: %d",
                          -result);
      goto done;
    }

  ctx.archive = unzOpen64(options->source_path);
  if (ctx.archive == NULL)
    {
      result = -EBADMSG;
      report->result = ANKI_IMPORT_RESULT_REJECTED;
      anki_report_message(report, "not a readable ZIP/APKG archive");
      goto done;
    }

  result = anki_zip_scan(&ctx);
  if (result < 0)
    {
      report->result = ANKI_IMPORT_RESULT_REJECTED;
      anki_report_message(report,
                          "archive violates entry, size, encryption, "
                          "compression, or path safety limits");
      goto done;
    }

  result = anki_detect_format(&ctx);
  if (result < 0)
    {
      report->result = ANKI_IMPORT_RESULT_REJECTED;
      goto done;
    }

  result = anki_write_staging_record(&ctx);
  if (result < 0)
    {
      anki_report_message(report, "failed to record import transaction: %d",
                          -result);
      goto done;
    }

  if (unzLocateFile(ctx.archive, ctx.collection_entry, 1) != UNZ_OK)
    {
      result = -EBADMSG;
      goto done;
    }

  result = anki_extract_current(&ctx, ctx.collection_path,
                                options->limits.max_collection_bytes, NULL);
  if (result < 0)
    {
      if (result == -EFBIG || result == -EBADMSG)
        {
          report->result = ANKI_IMPORT_RESULT_REJECTED;
        }

      anki_report_message(report, "collection extraction failed: %d",
                          -result);
      goto done;
    }

  result = anki_source_db_open(&ctx);
  if (result < 0)
    {
      report->result = ANKI_IMPORT_RESULT_REJECTED;
      anki_report_message(report, "collection SQLite is corrupt or unsupported");
      goto done;
    }

  result = anki_load_collection_json(&ctx);
  if (result < 0)
    {
      report->result = ANKI_IMPORT_RESULT_REJECTED;
      anki_report_message(report, "collection metadata is invalid or too large");
      goto done;
    }

  result = card_model_begin(ctx.model);
  if (result < 0)
    {
      goto done;
    }

  ctx.transaction_active = true;
  result = anki_import_decks(&ctx);
  if (result == 0)
    {
      result = anki_import_cards(&ctx);
    }

  if (result == 0)
    {
      result = anki_import_media_map(&ctx);
    }

  if (result == 0)
    {
      result = anki_extract_media(&ctx);
    }

  if (result == 0)
    {
      result = anki_commit_media(&ctx);
    }

  if (result == 0)
    {
      result = anki_finish_import_record(&ctx);
    }

  if (result == 0)
    {
      result = card_model_commit(ctx.model);
      if (result == 0)
        {
          ctx.transaction_active = false;
        }
    }

  if (result < 0)
    {
      if (result == -EFBIG || result == -EBADMSG ||
          result == -EPROTONOSUPPORT || result == -EPERM)
        {
          report->result = ANKI_IMPORT_RESULT_REJECTED;
        }

      anki_report_message(report, "import aborted safely: %d", -result);
      goto done;
    }

  report->result = ANKI_IMPORT_RESULT_IMPORTED;
  anki_report_message(report, "import complete");

done:
  anki_cleanup_ctx(&ctx, result < 0);
  if (anki_import_write_report(options, report) < 0)
    {
      report->warnings |= ANKI_IMPORT_WARN_REPORT_IO;
    }

  return result;
}

static int anki_json_escape(FILE *file, const char *text)
{
  const unsigned char *cursor = (const unsigned char *)text;

  while (*cursor != '\0')
    {
      if (*cursor == '"' || *cursor == '\\')
        {
          if (fputc('\\', file) == EOF || fputc(*cursor, file) == EOF)
            {
              return -EIO;
            }
        }
      else if (*cursor < 0x20)
        {
          if (fprintf(file, "\\u%04x", *cursor) < 0)
            {
              return -EIO;
            }
        }
      else if (fputc(*cursor, file) == EOF)
        {
          return -EIO;
        }

      cursor++;
    }

  return 0;
}

int anki_import_write_report(const struct anki_import_options_s *options,
                             const struct anki_import_report_s *report)
{
  char filesystem_id[ANKI_IMPORT_FS_ID_HEX_LEN + 1];
  char name[ANKI_IMPORT_FS_ID_HEX_LEN + sizeof(".json")];
  char path[PATH_MAX];
  char temporary[PATH_MAX];
  FILE *file;
  int fd;
  int result;

  if (options == NULL || report == NULL || options->report_root == NULL)
    {
      return -EINVAL;
    }

  result = anki_mkdirs(options->report_root);
  if (result < 0)
    {
      return result;
    }

  if (report->fingerprint[0] != '\0')
    {
      result = anki_import_filesystem_id(report->fingerprint,
                                         filesystem_id);
      if (result < 0)
        {
          return result;
        }

      snprintf(name, sizeof(name), "%s.json", filesystem_id);
    }
  else
    {
      strcpy(name, "last-failed.json");
    }

  result = anki_join_path(path, sizeof(path), options->report_root, name);
  if (result < 0)
    {
      return result;
    }

  if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) < 0 ||
      strlen(path) + 4 >= sizeof(temporary))
    {
      return -ENAMETOOLONG;
    }

  file = fopen(temporary, "wb");
  if (file == NULL)
    {
      return -errno;
    }

  if (fprintf(file,
              "{\n"
              "  \"schema\": \"moxinzhi-anki-import-report/1\",\n"
              "  \"result\": %u,\n"
              "  \"format\": %u,\n"
              "  \"fingerprint\": \"%s\",\n"
              "  \"warnings\": %" PRIu32 ",\n"
              "  \"archive_entries\": %" PRIu32 ",\n"
              "  \"cards_seen\": %" PRIu32 ",\n"
              "  \"cards_imported\": %" PRIu32 ",\n"
              "  \"cards_skipped\": %" PRIu32 ",\n"
              "  \"decks_imported\": %" PRIu32 ",\n"
              "  \"media_mapped\": %" PRIu32 ",\n"
              "  \"media_imported\": %" PRIu32 ",\n"
              "  \"media_skipped\": %" PRIu32 ",\n"
              "  \"media_bytes\": %" PRIu64 ",\n"
              "  \"message\": \"",
              (unsigned int)report->result, (unsigned int)report->format,
              report->fingerprint, report->warnings,
              report->archive_entries, report->cards_seen,
              report->cards_imported, report->cards_skipped,
              report->decks_imported, report->media_mapped,
              report->media_imported, report->media_skipped,
              report->media_bytes) < 0)
    {
      result = -EIO;
    }

  if (result == 0)
    {
      result = anki_json_escape(file, report->message);
    }
  if (result == 0 && fprintf(file, "\"\n}\n") < 0)
    {
      result = -EIO;
    }

  if (result == 0 && fflush(file) < 0)
    {
      result = -errno;
    }

  fd = fileno(file);
  if (result == 0 && fd >= 0 && fsync(fd) < 0)
    {
      result = -errno;
    }

  if (fclose(file) < 0 && result == 0)
    {
      result = -errno;
    }

  if (result == 0 && rename(temporary, path) < 0)
    {
      result = -errno;
    }

  if (result == 0)
    {
      result = anki_fsync_dir(options->report_root);
    }
  else
    {
      unlink(temporary);
    }

  return result;
}
