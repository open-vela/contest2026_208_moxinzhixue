/****************************************************************************
 * Basic, allocation-free Anki field-to-text conversion.
 *
 * This intentionally does not try to render card templates.  It removes
 * markup from the first two note fields, keeps paragraph boundaries, and
 * records local image/audio references for the importer.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "anki_text.h"

#include "card_model.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct anki_text_writer_s
{
  char *output;
  size_t output_size;
  size_t output_used;
  char *refs;
  size_t refs_size;
  size_t refs_used;
  uint32_t flags;
};

static int anki_ascii_tolower(int ch)
{
  return ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch;
}

static bool anki_ascii_equal(const char *text, size_t size,
                             const char *literal)
{
  size_t literal_size = strlen(literal);
  size_t i;

  if (size != literal_size)
    {
      return false;
    }

  for (i = 0; i < size; i++)
    {
      if (anki_ascii_tolower((unsigned char)text[i]) !=
          anki_ascii_tolower((unsigned char)literal[i]))
        {
          return false;
        }
    }

  return true;
}

static const char *anki_ascii_find(const char *start, const char *end,
                                   const char *needle)
{
  size_t needle_size = strlen(needle);
  const char *cursor;
  size_t i;

  if (needle_size == 0)
    {
      return start;
    }

  for (cursor = start; (size_t)(end - cursor) >= needle_size; cursor++)
    {
      for (i = 0; i < needle_size; i++)
        {
          if (anki_ascii_tolower((unsigned char)cursor[i]) !=
              anki_ascii_tolower((unsigned char)needle[i]))
            {
              break;
            }
        }

      if (i == needle_size)
        {
          return cursor;
        }
    }

  return NULL;
}

static int anki_text_append_raw(struct anki_text_writer_s *writer,
                                const char *data, size_t size)
{
  if (writer->output_used + size + 1 > writer->output_size)
    {
      return -ENOSPC;
    }

  memcpy(writer->output + writer->output_used, data, size);
  writer->output_used += size;
  writer->output[writer->output_used] = '\0';
  return 0;
}

static int anki_text_append_space(struct anki_text_writer_s *writer,
                                  bool newline)
{
  char ch = newline ? '\n' : ' ';

  if (writer->output_used == 0)
    {
      return 0;
    }

  if (writer->output[writer->output_used - 1] == '\n')
    {
      return 0;
    }

  if (!newline && writer->output[writer->output_used - 1] == ' ')
    {
      return 0;
    }

  if (newline && writer->output[writer->output_used - 1] == ' ')
    {
      writer->output[writer->output_used - 1] = '\n';
      return 0;
    }

  return anki_text_append_raw(writer, &ch, 1);
}

static bool anki_media_name_safe(const char *name, size_t size)
{
  size_t i;

  if (size == 0 || size > CARD_MODEL_MAX_MEDIA_NAME ||
      (size == 1 && name[0] == '.') ||
      (size == 2 && name[0] == '.' && name[1] == '.'))
    {
      return false;
    }

  for (i = 0; i < size; i++)
    {
      unsigned char ch = (unsigned char)name[i];
      if (ch < 0x20 || ch == 0x7f || ch == '/' || ch == '\\' || ch == ':')
        {
          return false;
        }
    }

  return true;
}

static bool anki_ref_present(const struct anki_text_writer_s *writer,
                             const char *name, size_t size)
{
  size_t offset = 0;
  size_t item_size;

  while (offset < writer->refs_used)
    {
      item_size = strcspn(writer->refs + offset, "\n");
      if (item_size == size &&
          memcmp(writer->refs + offset, name, size) == 0)
        {
          return true;
        }

      offset += item_size + 1;
    }

  return false;
}

static int anki_add_media_ref(struct anki_text_writer_s *writer,
                              const char *name, size_t size)
{
  while (size > 0 && isspace((unsigned char)*name))
    {
      name++;
      size--;
    }

  while (size > 0 && isspace((unsigned char)name[size - 1]))
    {
      size--;
    }

  if (size >= 5 &&
      (anki_ascii_equal(name, 5, "data:") ||
       anki_ascii_equal(name, 5, "http:")) )
    {
      writer->flags |= ANKI_TEXT_FLAG_EXTERNAL_MEDIA;
      return 0;
    }

  if (size >= 6 && anki_ascii_equal(name, 6, "https:"))
    {
      writer->flags |= ANKI_TEXT_FLAG_EXTERNAL_MEDIA;
      return 0;
    }

  if (!anki_media_name_safe(name, size))
    {
      writer->flags |= ANKI_TEXT_FLAG_BAD_MEDIA_NAME;
      return 0;
    }

  if (anki_ref_present(writer, name, size))
    {
      return 0;
    }

  if (writer->refs_used + size + 2 > writer->refs_size)
    {
      return -ENOSPC;
    }

  memcpy(writer->refs + writer->refs_used, name, size);
  writer->refs_used += size;
  writer->refs[writer->refs_used++] = '\n';
  writer->refs[writer->refs_used] = '\0';
  return 0;
}

static int anki_append_codepoint(struct anki_text_writer_s *writer,
                                 uint32_t codepoint)
{
  char encoded[4];
  size_t size;

  if (codepoint == 0 || codepoint > 0x10ffff ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff))
    {
      return anki_text_append_raw(writer, "?", 1);
    }

  if (codepoint < 0x80)
    {
      encoded[0] = (char)codepoint;
      size = 1;
    }
  else if (codepoint < 0x800)
    {
      encoded[0] = (char)(0xc0 | (codepoint >> 6));
      encoded[1] = (char)(0x80 | (codepoint & 0x3f));
      size = 2;
    }
  else if (codepoint < 0x10000)
    {
      encoded[0] = (char)(0xe0 | (codepoint >> 12));
      encoded[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
      encoded[2] = (char)(0x80 | (codepoint & 0x3f));
      size = 3;
    }
  else
    {
      encoded[0] = (char)(0xf0 | (codepoint >> 18));
      encoded[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
      encoded[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
      encoded[3] = (char)(0x80 | (codepoint & 0x3f));
      size = 4;
    }

  return anki_text_append_raw(writer, encoded, size);
}

static int anki_decode_entity(struct anki_text_writer_s *writer,
                              const char *entity, size_t size)
{
  uint32_t value = 0;
  size_t i = 0;
  int base = 10;
  int digit;

  if (anki_ascii_equal(entity, size, "amp"))
    {
      return anki_text_append_raw(writer, "&", 1);
    }
  else if (anki_ascii_equal(entity, size, "lt"))
    {
      return anki_text_append_raw(writer, "<", 1);
    }
  else if (anki_ascii_equal(entity, size, "gt"))
    {
      return anki_text_append_raw(writer, ">", 1);
    }
  else if (anki_ascii_equal(entity, size, "quot"))
    {
      return anki_text_append_raw(writer, "\"", 1);
    }
  else if (anki_ascii_equal(entity, size, "apos") ||
           anki_ascii_equal(entity, size, "#39"))
    {
      return anki_text_append_raw(writer, "'", 1);
    }
  else if (anki_ascii_equal(entity, size, "nbsp"))
    {
      return anki_text_append_space(writer, false);
    }

  if (size < 2 || entity[0] != '#')
    {
      return anki_text_append_raw(writer, "?", 1);
    }

  i = 1;
  if (i < size && (entity[i] == 'x' || entity[i] == 'X'))
    {
      base = 16;
      i++;
    }

  if (i == size)
    {
      return anki_text_append_raw(writer, "?", 1);
    }

  for (; i < size; i++)
    {
      if (entity[i] >= '0' && entity[i] <= '9')
        {
          digit = entity[i] - '0';
        }
      else if (base == 16 && entity[i] >= 'a' && entity[i] <= 'f')
        {
          digit = entity[i] - 'a' + 10;
        }
      else if (base == 16 && entity[i] >= 'A' && entity[i] <= 'F')
        {
          digit = entity[i] - 'A' + 10;
        }
      else
        {
          return anki_text_append_raw(writer, "?", 1);
        }

      if (value > (0x10ffffU - (uint32_t)digit) / (uint32_t)base)
        {
          return anki_text_append_raw(writer, "?", 1);
        }

      value = value * (uint32_t)base + (uint32_t)digit;
    }

  return anki_append_codepoint(writer, value);
}

static void anki_tag_name(const char *tag, const char *end,
                          const char **name, size_t *name_size)
{
  while (tag < end && isspace((unsigned char)*tag))
    {
      tag++;
    }

  if (tag < end && *tag == '/')
    {
      tag++;
    }

  while (tag < end && isspace((unsigned char)*tag))
    {
      tag++;
    }

  *name = tag;
  while (tag < end && (isalnum((unsigned char)*tag) || *tag == '-'))
    {
      tag++;
    }

  *name_size = (size_t)(tag - *name);
}

static int anki_img_source(struct anki_text_writer_s *writer,
                           const char *tag, const char *end)
{
  const char *cursor = tag;
  const char *value;
  char quote;
  size_t size;

  while (cursor < end)
    {
      if (cursor + 3 <= end &&
          anki_ascii_tolower((unsigned char)cursor[0]) == 's' &&
          anki_ascii_tolower((unsigned char)cursor[1]) == 'r' &&
          anki_ascii_tolower((unsigned char)cursor[2]) == 'c')
        {
          cursor += 3;
          while (cursor < end && isspace((unsigned char)*cursor))
            {
              cursor++;
            }

          if (cursor >= end || *cursor != '=')
            {
              continue;
            }

          cursor++;
          while (cursor < end && isspace((unsigned char)*cursor))
            {
              cursor++;
            }

          if (cursor >= end)
            {
              return 0;
            }

          quote = (*cursor == '\'' || *cursor == '"') ? *cursor++ : '\0';
          value = cursor;
          if (quote != '\0')
            {
              while (cursor < end && *cursor != quote)
                {
                  cursor++;
                }
            }
          else
            {
              while (cursor < end && !isspace((unsigned char)*cursor))
                {
                  cursor++;
                }
            }

          size = (size_t)(cursor - value);
          return anki_add_media_ref(writer, value, size);
        }

      cursor++;
    }

  return 0;
}

static bool anki_block_tag(const char *name, size_t size)
{
  return anki_ascii_equal(name, size, "br") ||
         anki_ascii_equal(name, size, "p") ||
         anki_ascii_equal(name, size, "div") ||
         anki_ascii_equal(name, size, "li") ||
         anki_ascii_equal(name, size, "tr") ||
         anki_ascii_equal(name, size, "h1") ||
         anki_ascii_equal(name, size, "h2") ||
         anki_ascii_equal(name, size, "h3") ||
         anki_ascii_equal(name, size, "h4") ||
         anki_ascii_equal(name, size, "h5") ||
         anki_ascii_equal(name, size, "h6");
}

int anki_text_clean(const char *input, size_t input_size,
                    char *output, size_t output_size,
                    char *media_refs, size_t media_refs_size,
                    uint32_t *flags)
{
  struct anki_text_writer_s writer;
  const char *cursor;
  const char *end;
  const char *close;
  const char *tag_name;
  const char *entity_end;
  size_t tag_name_size;
  size_t entity_size;
  int result;

  if (input == NULL || output == NULL || output_size == 0 ||
      media_refs == NULL || media_refs_size == 0)
    {
      return -EINVAL;
    }

  memset(&writer, 0, sizeof(writer));
  writer.output = output;
  writer.output_size = output_size;
  writer.refs = media_refs;
  writer.refs_size = media_refs_size;
  output[0] = '\0';
  media_refs[0] = '\0';
  cursor = input;
  end = input + input_size;

  if (anki_ascii_find(cursor, end, "[latex]") != NULL ||
      anki_ascii_find(cursor, end, "\\(") != NULL ||
      anki_ascii_find(cursor, end, "[$]") != NULL)
    {
      writer.flags |= ANKI_TEXT_FLAG_LATEX_PRESENT;
    }

  while (cursor < end)
    {
      if (*cursor == '<')
        {
          if (cursor + 4 <= end && memcmp(cursor, "<!--", 4) == 0)
            {
              close = anki_ascii_find(cursor + 4, end, "-->");
              cursor = close == NULL ? end : close + 3;
              continue;
            }

          close = memchr(cursor + 1, '>', (size_t)(end - cursor - 1));
          if (close == NULL)
            {
              result = anki_text_append_raw(&writer, cursor, 1);
              if (result < 0)
                {
                  return result;
                }

              cursor++;
              continue;
            }

          anki_tag_name(cursor + 1, close, &tag_name, &tag_name_size);
          if (anki_ascii_equal(tag_name, tag_name_size, "script") ||
              anki_ascii_equal(tag_name, tag_name_size, "style"))
            {
              const char *after = close + 1;
              const char *terminator = anki_ascii_equal(tag_name,
                                                        tag_name_size,
                                                        "script") ?
                                       "</script" : "</style";
              const char *closing_tag = anki_ascii_find(after, end,
                                                        terminator);
              writer.flags |= ANKI_TEXT_FLAG_SCRIPT_REMOVED;
              if (closing_tag == NULL)
                {
                  cursor = end;
                }
              else
                {
                  close = memchr(closing_tag, '>',
                                 (size_t)(end - closing_tag));
                  cursor = close == NULL ? end : close + 1;
                }

              continue;
            }

          if (anki_ascii_equal(tag_name, tag_name_size, "img"))
            {
              result = anki_img_source(&writer, cursor + 1, close);
              if (result < 0)
                {
                  return result;
                }

              result = anki_text_append_raw(&writer, "[图片]",
                                             sizeof("[图片]") - 1);
              if (result < 0)
                {
                  return result;
                }
            }
          else if (anki_block_tag(tag_name, tag_name_size))
            {
              result = anki_text_append_space(&writer, true);
              if (result < 0)
                {
                  return result;
                }
            }

          cursor = close + 1;
          continue;
        }

      if (cursor + 7 <= end &&
          anki_ascii_equal(cursor, 7, "[sound:"))
        {
          close = memchr(cursor + 7, ']', (size_t)(end - cursor - 7));
          if (close != NULL)
            {
              result = anki_add_media_ref(&writer, cursor + 7,
                                          (size_t)(close - cursor - 7));
              if (result < 0)
                {
                  return result;
                }

              result = anki_text_append_raw(&writer, "[音频]",
                                             sizeof("[音频]") - 1);
              if (result < 0)
                {
                  return result;
                }

              cursor = close + 1;
              continue;
            }
        }

      if (*cursor == '&')
        {
          entity_end = memchr(cursor + 1, ';',
                              (size_t)(end - cursor - 1));
          if (entity_end != NULL && entity_end - cursor <= 12)
            {
              entity_size = (size_t)(entity_end - cursor - 1);
              result = anki_decode_entity(&writer, cursor + 1, entity_size);
              if (result < 0)
                {
                  return result;
                }

              cursor = entity_end + 1;
              continue;
            }
        }

      if (*cursor == '\r' || *cursor == '\n')
        {
          result = anki_text_append_space(&writer, true);
        }
      else if (*cursor == '\t' || *cursor == '\f' || *cursor == ' ')
        {
          result = anki_text_append_space(&writer, false);
        }
      else
        {
          result = anki_text_append_raw(&writer, cursor, 1);
        }

      if (result < 0)
        {
          return result;
        }

      cursor++;
    }

  while (writer.output_used > 0 &&
         (writer.output[writer.output_used - 1] == ' ' ||
          writer.output[writer.output_used - 1] == '\n'))
    {
      writer.output[--writer.output_used] = '\0';
    }

  if (writer.refs_used > 0)
    {
      writer.refs[--writer.refs_used] = '\0';
    }

  if (flags != NULL)
    {
      *flags = writer.flags;
    }

  return 0;
}
