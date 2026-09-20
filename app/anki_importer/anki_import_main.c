/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "anki_importer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void anki_import_usage(const char *program)
{
  printf("Usage: %s [options] /data/import/deck.apkg\n", program);
  printf("Options:\n");
  printf("  --input-root PATH  allowed input directory\n");
  printf("  --store PATH       UnQLite card store\n");
  printf("  --media PATH       imported media root\n");
  printf("  --work PATH        temporary work root\n");
  printf("  --reports PATH     JSON report root\n");
}

static int anki_take_value(int argc, char **argv, int *index,
                           const char **value)
{
  if (*index + 1 >= argc)
    {
      return -EINVAL;
    }

  *value = argv[++*index];
  return 0;
}

static void anki_print_json_string(const char *text)
{
  const unsigned char *cursor = (const unsigned char *)text;

  while (*cursor != '\0')
    {
      if (*cursor == '"' || *cursor == '\\')
        {
          putchar('\\');
          putchar(*cursor);
        }
      else if (*cursor < 0x20)
        {
          printf("\\u%04x", *cursor);
        }
      else
        {
          putchar(*cursor);
        }

      cursor++;
    }
}

int main(int argc, char **argv)
{
  struct anki_import_options_s options;
  struct anki_import_report_s report;
  int result = 0;
  int i;

  anki_import_default_options(&options);
  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
          anki_import_usage(argv[0]);
          return EXIT_SUCCESS;
        }
      else if (strcmp(argv[i], "--input-root") == 0)
        {
          result = anki_take_value(argc, argv, &i, &options.input_root);
        }
      else if (strcmp(argv[i], "--store") == 0)
        {
          result = anki_take_value(argc, argv, &i, &options.store_path);
        }
      else if (strcmp(argv[i], "--media") == 0)
        {
          result = anki_take_value(argc, argv, &i, &options.media_root);
        }
      else if (strcmp(argv[i], "--work") == 0)
        {
          result = anki_take_value(argc, argv, &i, &options.work_root);
        }
      else if (strcmp(argv[i], "--reports") == 0)
        {
          result = anki_take_value(argc, argv, &i, &options.report_root);
        }
      else if (argv[i][0] == '-')
        {
          fprintf(stderr, "anki_import: unknown option: %s\n", argv[i]);
          anki_import_usage(argv[0]);
          return EXIT_FAILURE;
        }
      else if (options.source_path == NULL)
        {
          options.source_path = argv[i];
          result = 0;
        }
      else
        {
          result = -EINVAL;
        }

      if (result < 0)
        {
          fprintf(stderr, "anki_import: missing or invalid argument\n");
          anki_import_usage(argv[0]);
          return EXIT_FAILURE;
        }
    }

  if (options.source_path == NULL)
    {
      anki_import_usage(argv[0]);
      return EXIT_FAILURE;
    }

  result = anki_import_package(&options, &report);
  printf("{\"result\":%u,\"format\":%u,\"fingerprint\":\"%s\","
         "\"cards_imported\":%" PRIu32
         ",\"cards_skipped\":%" PRIu32
         ",\"media_imported\":%" PRIu32
         ",\"warnings\":%" PRIu32 ","
         "\"media_bytes\":%" PRIu64 ",\"message\":\"",
         (unsigned int)report.result, (unsigned int)report.format,
         report.fingerprint, report.cards_imported, report.cards_skipped,
         report.media_imported, report.warnings, report.media_bytes);
  anki_print_json_string(report.message);
  puts("\"}");
  return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
