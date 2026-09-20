/****************************************************************************
 * Small SHA-256 implementation used for duplicate-import fingerprints.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "anki_sha256.h"

#include <string.h>

#define ROR32(v, n) (((v) >> (n)) | ((v) << (32 - (n))))

static const uint32_t g_sha256_k[64] =
{
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t anki_sha256_load_be(const uint8_t *data)
{
  return (uint32_t)data[0] << 24 |
         (uint32_t)data[1] << 16 |
         (uint32_t)data[2] << 8 |
         (uint32_t)data[3];
}

static void anki_sha256_store_be(uint8_t *data, uint32_t value)
{
  data[0] = (uint8_t)(value >> 24);
  data[1] = (uint8_t)(value >> 16);
  data[2] = (uint8_t)(value >> 8);
  data[3] = (uint8_t)value;
}

static void anki_sha256_transform(struct anki_sha256_s *ctx,
                                  const uint8_t block[64])
{
  uint32_t words[64];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t f;
  uint32_t g;
  uint32_t h;
  uint32_t s0;
  uint32_t s1;
  uint32_t ch;
  uint32_t maj;
  uint32_t t1;
  uint32_t t2;
  size_t i;

  for (i = 0; i < 16; i++)
    {
      words[i] = anki_sha256_load_be(block + i * 4);
    }

  for (i = 16; i < 64; i++)
    {
      s0 = ROR32(words[i - 15], 7) ^ ROR32(words[i - 15], 18) ^
           (words[i - 15] >> 3);
      s1 = ROR32(words[i - 2], 17) ^ ROR32(words[i - 2], 19) ^
           (words[i - 2] >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];
  f = ctx->state[5];
  g = ctx->state[6];
  h = ctx->state[7];

  for (i = 0; i < 64; i++)
    {
      s1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
      ch = (e & f) ^ (~e & g);
      t1 = h + s1 + ch + g_sha256_k[i] + words[i];
      s0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
      maj = (a & b) ^ (a & c) ^ (b & c);
      t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

void anki_sha256_init(struct anki_sha256_s *ctx)
{
  static const uint32_t initial[8] =
  {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
  };

  memcpy(ctx->state, initial, sizeof(initial));
  ctx->bit_count = 0;
  ctx->block_used = 0;
}

void anki_sha256_update(struct anki_sha256_s *ctx,
                        const void *data, size_t size)
{
  const uint8_t *input = data;
  size_t copy_size;

  ctx->bit_count += (uint64_t)size * 8;
  while (size > 0)
    {
      copy_size = sizeof(ctx->block) - ctx->block_used;
      if (copy_size > size)
        {
          copy_size = size;
        }

      memcpy(ctx->block + ctx->block_used, input, copy_size);
      ctx->block_used += copy_size;
      input += copy_size;
      size -= copy_size;

      if (ctx->block_used == sizeof(ctx->block))
        {
          anki_sha256_transform(ctx, ctx->block);
          ctx->block_used = 0;
        }
    }
}

void anki_sha256_final(struct anki_sha256_s *ctx, uint8_t digest[32])
{
  uint64_t bit_count = ctx->bit_count;
  size_t i;

  ctx->block[ctx->block_used++] = 0x80;
  if (ctx->block_used > 56)
    {
      memset(ctx->block + ctx->block_used, 0,
             sizeof(ctx->block) - ctx->block_used);
      anki_sha256_transform(ctx, ctx->block);
      ctx->block_used = 0;
    }

  memset(ctx->block + ctx->block_used, 0, 56 - ctx->block_used);
  for (i = 0; i < 8; i++)
    {
      ctx->block[63 - i] = (uint8_t)(bit_count >> (i * 8));
    }

  anki_sha256_transform(ctx, ctx->block);
  for (i = 0; i < 8; i++)
    {
      anki_sha256_store_be(digest + i * 4, ctx->state[i]);
    }

  memset(ctx, 0, sizeof(*ctx));
}
