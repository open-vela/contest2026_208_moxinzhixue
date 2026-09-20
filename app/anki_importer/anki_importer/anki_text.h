/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef MOXINZHI_ANKI_TEXT_H
#define MOXINZHI_ANKI_TEXT_H

#include <stddef.h>
#include <stdint.h>

#define ANKI_TEXT_FLAG_SCRIPT_REMOVED (1U << 0)
#define ANKI_TEXT_FLAG_LATEX_PRESENT  (1U << 1)
#define ANKI_TEXT_FLAG_EXTERNAL_MEDIA (1U << 2)
#define ANKI_TEXT_FLAG_BAD_MEDIA_NAME (1U << 3)

int anki_text_clean(const char *input, size_t input_size,
                    char *output, size_t output_size,
                    char *media_refs, size_t media_refs_size,
                    uint32_t *flags);

#endif
