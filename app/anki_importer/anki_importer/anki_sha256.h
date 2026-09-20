/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef MOXINZHI_ANKI_SHA256_H
#define MOXINZHI_ANKI_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct anki_sha256_s
{
  uint32_t state[8];
  uint64_t bit_count;
  uint8_t block[64];
  size_t block_used;
};

void anki_sha256_init(struct anki_sha256_s *ctx);
void anki_sha256_update(struct anki_sha256_s *ctx,
                        const void *data, size_t size);
void anki_sha256_final(struct anki_sha256_s *ctx, uint8_t digest[32]);

#endif
