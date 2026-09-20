/****************************************************************************
 * Host integration tests for the Moxinzhixue Anki importer.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "anki_importer.h"
#include "anki_text.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#  define PATH_MAX 1024
#endif

#define CHECK(condition)                                                     \
  do                                                                         \
    {                                                                        \
      if (!(condition))                                                      \
        {                                                                    \
          fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                  #condition);                                               \
          return -1;                                                         \
        }                                                                    \
    }                                                                        \
  while (0)

struct test_env_s
{
  struct anki_import_options_s options;
  char root[PATH_MAX];
  char source[PATH_MAX];
  char store[PATH_MAX];
  char media[PATH_MAX];
  char work[PATH_MAX];
  char reports[PATH_MAX];
};

static int test_join(char *output, size_t output_size,
                     const char *left, const char *right)
{
  int written = snprintf(output, output_size, "%s/%s", left, right);

  return written < 0 || (size_t)written >= output_size ? -1 : 0;
}

static int test_remove_tree(const char *path)
{
  struct stat status;
  struct dirent *entry;
  DIR *directory;
  char child[PATH_MAX];

  if (lstat(path, &status) < 0)
    {
      return errno == ENOENT ? 0 : -1;
    }

  if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode))
    {
      return unlink(path);
    }

  directory = opendir(path);
  if (directory == NULL)
    {
      return -1;
    }

  while ((entry = readdir(directory)) != NULL)
    {
      if (strcmp(entry->d_name, ".") == 0 ||
          strcmp(entry->d_name, "..") == 0)
        {
          continue;
        }

      if (test_join(child, sizeof(child), path, entry->d_name) < 0 ||
          test_remove_tree(child) < 0)
        {
          closedir(directory);
          return -1;
        }
    }

  closedir(directory);
  return rmdir(path);
}

static int test_env_init(struct test_env_s *env, const char *fixtures,
                         const char *fixture)
{
  strcpy(env->root, "/tmp/moxinzhi-anki-test-XXXXXX");
  if (mkdtemp(env->root) == NULL)
    {
      return -1;
    }

  if (test_join(env->source, sizeof(env->source), fixtures, fixture) < 0 ||
      test_join(env->store, sizeof(env->store), env->root, "cards.unqlite") < 0 ||
      test_join(env->media, sizeof(env->media), env->root, "media") < 0 ||
      test_join(env->work, sizeof(env->work), env->root, "work") < 0 ||
      test_join(env->reports, sizeof(env->reports), env->root, "reports") < 0)
    {
      test_remove_tree(env->root);
      return -1;
    }

  anki_import_default_options(&env->options);
  env->options.source_path = env->source;
  env->options.input_root = fixtures;
  env->options.store_path = env->store;
  env->options.media_root = env->media;
  env->options.work_root = env->work;
  env->options.report_root = env->reports;
  return 0;
}

static int test_read_file(const char *path, char *buffer, size_t buffer_size)
{
  FILE *file;
  size_t size;

  file = fopen(path, "rb");
  if (file == NULL)
    {
      return -1;
    }

  size = fread(buffer, 1, buffer_size - 1, file);
  if (ferror(file))
    {
      fclose(file);
      return -1;
    }

  buffer[size] = '\0';
  fclose(file);
  return (int)size;
}

static int test_visit_card(const char *fingerprint,
                           const struct card_model_card_s *card, void *arg)
{
  int *count = arg;

  CHECK(strlen(fingerprint) == CARD_MODEL_FINGERPRINT_HEX_LEN);
  CHECK(card->front != NULL);
  (*count)++;
  return 0;
}

static int test_text_cleanup(void)
{
  const char input[] =
    "A<div>B&nbsp;&amp;</div><SCRIPT>drop()</SCRIPT>"
    "<img src=\"local.png\"> [sound:voice.mp3]";
  char output[128];
  char refs[128];
  uint32_t flags = 0;

  CHECK(anki_text_clean(input, sizeof(input) - 1, output, sizeof(output),
                        refs, sizeof(refs), &flags) == 0);
  CHECK(strcmp(output, "A\nB &\n[图片] [音频]") == 0);
  CHECK(strcmp(refs, "local.png\nvoice.mp3") == 0);
  CHECK((flags & ANKI_TEXT_FLAG_SCRIPT_REMOVED) != 0);
  return 0;
}

static int test_filesystem_id(void)
{
  char fingerprint[CARD_MODEL_FINGERPRINT_HEX_LEN + 1];
  char filesystem_id[ANKI_IMPORT_FS_ID_HEX_LEN + 1];

  memset(fingerprint, 'a', CARD_MODEL_FINGERPRINT_HEX_LEN);
  fingerprint[CARD_MODEL_FINGERPRINT_HEX_LEN] = '\0';
  CHECK(anki_import_filesystem_id(fingerprint, filesystem_id) == 0);
  CHECK(strlen(filesystem_id) == ANKI_IMPORT_FS_ID_HEX_LEN);
  CHECK(strlen(".staging-") + strlen(filesystem_id) <= 32);
  CHECK(strlen(filesystem_id) + strlen(".json.tmp") <= 32);
  fingerprint[0] = 'z';
  CHECK(anki_import_filesystem_id(fingerprint, filesystem_id) == -EINVAL);
  return 0;
}

static int test_valid20_and_duplicate(const char *fixtures)
{
  struct test_env_s env;
  struct anki_import_report_s report;
  struct card_model_import_s import_record;
  struct card_model_card_s card;
  struct card_model_media_s media;
  struct card_model_s *model = NULL;
  void *allocation = NULL;
  char path[PATH_MAX];
  char filesystem_id[ANKI_IMPORT_FS_ID_HEX_LEN + 1];
  char data[256];
  int count = 0;

  CHECK(test_env_init(&env, fixtures, "valid20.apkg") == 0);
  CHECK(anki_import_package(&env.options, &report) == 0);
  CHECK(report.result == ANKI_IMPORT_RESULT_IMPORTED);
  CHECK(report.format == CARD_MODEL_SOURCE_ANKI20);
  CHECK(report.cards_seen == 2);
  CHECK(report.cards_imported == 1);
  CHECK(report.cards_skipped == 1);
  CHECK(report.decks_imported == 1);
  CHECK(report.media_mapped == 3);
  CHECK(report.media_imported == 2);
  CHECK(report.media_skipped == 2);
  CHECK((report.warnings & ANKI_IMPORT_WARN_SCRIPT_REMOVED) != 0);
  CHECK((report.warnings & ANKI_IMPORT_WARN_LATEX_PRESENT) != 0);
  CHECK((report.warnings & ANKI_IMPORT_WARN_COMPLEX_MODEL) != 0);
  CHECK((report.warnings & ANKI_IMPORT_WARN_MEDIA_MISSING) != 0);

  CHECK(card_model_open(env.store, &model) == 0);
  CHECK(card_model_get_import(model, report.fingerprint, &import_record) == 0);
  CHECK(import_record.state == CARD_MODEL_IMPORT_COMPLETE);
  CHECK(import_record.card_count == 1);
  CHECK(import_record.card_skipped_count == 1);
  CHECK(import_record.media_count == 2);
  CHECK(import_record.media_skipped_count == 2);

  CHECK(card_model_get_card(model, report.fingerprint, 3001, &card,
                            &allocation) == 0);
  CHECK(strcmp(card.front, "Intro\nHello World\n[图片][图片]") == 0);
  CHECK(strcmp(card.back,
               "Answer & detail [音频] [latex]x[/latex]") == 0);
  CHECK(strcmp(card.deck_name, "Fixture Deck") == 0);
  CHECK(strcmp(card.tags, " tag1 tag2 ") == 0);
  CHECK(strcmp(card.media_refs,
               "pic.png\nmissing.png\naudio.mp3") == 0);
  free(allocation);
  allocation = NULL;

  CHECK(card_model_get_media_by_name(model, report.fingerprint, "pic.png",
                                     &media) == 0);
  CHECK(media.size == sizeof("PNG-fixture") - 1);
  CHECK(test_read_file(media.local_path, data, sizeof(data)) ==
        (int)(sizeof("PNG-fixture") - 1));
  CHECK(memcmp(data, "PNG-fixture", sizeof("PNG-fixture") - 1) == 0);
  CHECK(card_model_get_media_by_name(model, report.fingerprint,
                                     "missing.png", &media) == -ENOENT);
  CHECK(card_model_foreach_card(model, test_visit_card, &count) == 0);
  CHECK(count == 1);

  CHECK(card_model_begin(model) == 0);
  CHECK(card_model_update_review(model, report.fingerprint, 3001, true,
                                 123456789) == 0);
  CHECK(card_model_commit(model) == 0);
  CHECK(card_model_get_card(model, report.fingerprint, 3001, &card,
                            &allocation) == 0);
  CHECK(card.review_count == 1 && card.correct_count == 1);
  CHECK(card.due_at_ms == 123456789);
  free(allocation);
  allocation = NULL;
  card_model_close(model);
  model = NULL;

  CHECK(anki_import_filesystem_id(report.fingerprint, filesystem_id) == 0);
  CHECK(snprintf(path, sizeof(path), "%s/%s.json", env.reports,
                 filesystem_id) > 0);
  CHECK(test_read_file(path, data, sizeof(data)) > 0);
  CHECK(strstr(data, "moxinzhi-anki-import-report/1") != NULL);

  CHECK(anki_import_package(&env.options, &report) == 0);
  CHECK(report.result == ANKI_IMPORT_RESULT_DUPLICATE);
  CHECK(report.cards_imported == 1);
  CHECK(report.cards_skipped == 1);
  CHECK(report.media_imported == 2);
  CHECK(report.media_skipped == 2);

  CHECK(test_remove_tree(env.root) == 0);
  return 0;
}

static int test_valid21(const char *fixtures)
{
  struct test_env_s env;
  struct anki_import_report_s report;
  struct card_model_card_s card;
  struct card_model_s *model = NULL;
  void *allocation = NULL;

  CHECK(test_env_init(&env, fixtures, "valid21.apkg") == 0);
  CHECK(anki_import_package(&env.options, &report) == 0);
  CHECK(report.result == ANKI_IMPORT_RESULT_IMPORTED);
  CHECK(report.format == CARD_MODEL_SOURCE_ANKI21);
  CHECK(report.cards_imported == 1);
  CHECK(report.media_imported == 0);
  CHECK(card_model_open(env.store, &model) == 0);
  CHECK(card_model_get_card(model, report.fingerprint, 5001, &card,
                            &allocation) == 0);
  CHECK(strcmp(card.front, "2.1 front") == 0);
  CHECK(strcmp(card.back, "2.1 back") == 0);
  free(allocation);
  card_model_close(model);
  CHECK(test_remove_tree(env.root) == 0);
  return 0;
}

static int test_rejected_fixture(const char *fixtures, const char *fixture,
                                 uint64_t max_package,
                                 uint64_t max_collection,
                                 const char *message_fragment)
{
  struct test_env_s env;
  struct anki_import_report_s report;
  int result;

  CHECK(test_env_init(&env, fixtures, fixture) == 0);
  if (max_package != 0)
    {
      env.options.limits.max_package_bytes = max_package;
    }

  if (max_collection != 0)
    {
      env.options.limits.max_collection_bytes = max_collection;
    }

  result = anki_import_package(&env.options, &report);
  CHECK(result < 0);
  CHECK(report.result == ANKI_IMPORT_RESULT_REJECTED);
  if (message_fragment != NULL)
    {
      CHECK(strstr(report.message, message_fragment) != NULL);
    }

  CHECK(test_remove_tree(env.root) == 0);
  return 0;
}

static int test_rejections(const char *fixtures)
{
  CHECK(test_rejected_fixture(fixtures, "corrupt-zip.apkg", 0, 0,
                              "not a readable") == 0);
  CHECK(test_rejected_fixture(fixtures, "corrupt-sqlite.apkg", 0, 0,
                              "SQLite") == 0);
  CHECK(test_rejected_fixture(fixtures, "traversal.apkg", 0, 0,
                              "archive violates") == 0);
  CHECK(test_rejected_fixture(fixtures, "encrypted.apkg", 0, 0,
                              "archive violates") == 0);
  CHECK(test_rejected_fixture(fixtures, "unsupported-compression.apkg", 0, 0,
                              "archive violates") == 0);
  CHECK(test_rejected_fixture(fixtures, "compression-bomb.apkg", 0, 0,
                              "archive violates") == 0);
  CHECK(test_rejected_fixture(fixtures, "oversized-entry.apkg", 0, 128,
                              "extraction failed") == 0);
  CHECK(test_rejected_fixture(fixtures, "valid20.apkg", 64, 0,
                              "fingerprint") == 0);
  CHECK(test_rejected_fixture(fixtures, "latest.apkg", 0, 0,
                              "collection.anki21b/zstd") == 0);
  return 0;
}

static int test_symlink_source_rejected(const char *fixtures)
{
  struct test_env_s env;
  struct anki_import_report_s report;
  char target[PATH_MAX];

  CHECK(test_env_init(&env, fixtures, "valid21.apkg") == 0);
  CHECK(test_join(target, sizeof(target), fixtures, "valid21.apkg") == 0);
  CHECK(test_join(env.source, sizeof(env.source), env.root, "linked.apkg") == 0);
  CHECK(symlink(target, env.source) == 0);
  env.options.input_root = env.root;
  env.options.source_path = env.source;
  CHECK(anki_import_package(&env.options, &report) < 0);
  CHECK(report.result == ANKI_IMPORT_RESULT_REJECTED);
  CHECK(test_remove_tree(env.root) == 0);
  return 0;
}

static int test_staging_recovery(const char *fixtures)
{
  struct test_env_s env;
  struct anki_import_report_s report;
  struct card_model_import_s import_record;
  struct card_model_card_s fake_card;
  struct card_model_card_s card;
  struct card_model_s *model = NULL;
  char media_final[PATH_MAX];
  char orphan[PATH_MAX];
  char filesystem_id[ANKI_IMPORT_FS_ID_HEX_LEN + 1];
  void *allocation = NULL;
  int fd;
  int count = 0;

  CHECK(test_env_init(&env, fixtures, "valid21.apkg") == 0);
  CHECK(anki_import_package(&env.options, &report) == 0);
  CHECK(card_model_open(env.store, &model) == 0);
  CHECK(card_model_get_import(model, report.fingerprint, &import_record) == 0);

  memset(&fake_card, 0, sizeof(fake_card));
  fake_card.source_card_id = 5999;
  fake_card.source_note_id = 5999;
  fake_card.front = "orphan front";
  fake_card.back = "orphan back";
  import_record.state = CARD_MODEL_IMPORT_STAGING;
  CHECK(card_model_begin(model) == 0);
  CHECK(card_model_put_card(model, report.fingerprint, &fake_card) == 0);
  CHECK(card_model_put_import(model, report.fingerprint, &import_record) == 0);
  CHECK(card_model_commit(model) == 0);
  card_model_close(model);
  model = NULL;

  CHECK(anki_import_filesystem_id(report.fingerprint, filesystem_id) == 0);
  CHECK(test_join(media_final, sizeof(media_final), env.media,
                  filesystem_id) == 0);
  CHECK(test_join(orphan, sizeof(orphan), media_final, "orphan") == 0);
  fd = open(orphan, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(fd >= 0);
  CHECK(write(fd, "orphan", 6) == 6);
  close(fd);

  CHECK(anki_import_package(&env.options, &report) == 0);
  CHECK(report.result == ANKI_IMPORT_RESULT_IMPORTED);
  CHECK(access(orphan, F_OK) < 0 && errno == ENOENT);
  CHECK(card_model_open(env.store, &model) == 0);
  CHECK(card_model_get_card(model, report.fingerprint, 5999, &card,
                            &allocation) == -ENOENT);
  CHECK(card_model_foreach_card(model, test_visit_card, &count) == 0);
  CHECK(count == 1);
  CHECK(card_model_get_import(model, report.fingerprint, &import_record) == 0);
  CHECK(import_record.state == CARD_MODEL_IMPORT_COMPLETE);
  card_model_close(model);
  CHECK(test_remove_tree(env.root) == 0);
  return 0;
}

int main(int argc, char **argv)
{
  const char *fixtures;

  if (argc != 2)
    {
      fprintf(stderr, "usage: %s FIXTURE_DIR\n", argv[0]);
      return EXIT_FAILURE;
    }

  fixtures = argv[1];
  if (test_text_cleanup() < 0 ||
      test_filesystem_id() < 0 ||
      test_valid20_and_duplicate(fixtures) < 0 ||
      test_valid21(fixtures) < 0 ||
      test_rejections(fixtures) < 0 ||
      test_symlink_source_rejected(fixtures) < 0 ||
      test_staging_recovery(fixtures) < 0)
    {
      return EXIT_FAILURE;
    }

  printf("all Anki importer host tests passed\n");
  return EXIT_SUCCESS;
}
