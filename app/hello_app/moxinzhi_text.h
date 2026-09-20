/****************************************************************************
 * Contest 2026 team 208 - UTF-8 safe bounded text helpers
 ****************************************************************************/

#ifndef __MOXINZHI_TEXT_H
#define __MOXINZHI_TEXT_H

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

static inline size_t mz_text_utf8_valid_prefix(FAR const char *text,
                                                size_t length)
{
  size_t index = 0;

  while (index < length)
    {
      unsigned char lead = (unsigned char)text[index];
      size_t count;
      size_t offset;

      if (lead < 0x80)
        {
          index++;
          continue;
        }
      else if (lead >= 0xc2 && lead <= 0xdf)
        {
          count = 2;
        }
      else if (lead >= 0xe0 && lead <= 0xef)
        {
          count = 3;
        }
      else if (lead >= 0xf0 && lead <= 0xf4)
        {
          count = 4;
        }
      else
        {
          break;
        }

      if (index + count > length)
        {
          break;
        }

      for (offset = 1; offset < count; offset++)
        {
          if (((unsigned char)text[index + offset] & 0xc0) != 0x80)
            {
              return index;
            }
        }

      if ((lead == 0xe0 && (unsigned char)text[index + 1] < 0xa0) ||
          (lead == 0xed && (unsigned char)text[index + 1] >= 0xa0) ||
          (lead == 0xf0 && (unsigned char)text[index + 1] < 0x90) ||
          (lead == 0xf4 && (unsigned char)text[index + 1] >= 0x90))
        {
          break;
        }

      index += count;
    }

  return index;
}

static inline void mz_text_utf8_trim(FAR char *text)
{
  size_t length;
  size_t valid;

  if (text == NULL)
    {
      return;
    }

  length = strlen(text);
  valid = mz_text_utf8_valid_prefix(text, length);
  text[valid] = '\0';
}

static inline void mz_text_copy_utf8(FAR char *target, size_t capacity,
                                     FAR const char *source)
{
  if (target == NULL || capacity == 0)
    {
      return;
    }

  snprintf(target, capacity, "%s", source == NULL ? "" : source);
  mz_text_utf8_trim(target);
}

#endif /* __MOXINZHI_TEXT_H */
