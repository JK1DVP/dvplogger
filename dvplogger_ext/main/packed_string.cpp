/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2026 Eiichiro Araki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "packed_string.h"

#include <ctype.h>
#include <string.h>

int packed6_code(char c, bool callsign) {
  c = (char)toupper((unsigned char)c);
  if (c >= '0' && c <= '9') return 1 + (c - '0');
  if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
  if (callsign && c == '/') return 37;
  if (callsign && c == '-') return 38;
  return -1;
}

size_t packed6_size(size_t nchars) {
  return (nchars * 6U + 7U) / 8U;
}

size_t packed6_encode(const char *s, bool callsign, uint8_t *out, size_t cap) {
  if (!s || !out) return 0;
  uint32_t bits = 0;
  unsigned nbits = 0;
  size_t n = 0;
  for (; *s; ++s) {
    int c = packed6_code(*s, callsign);
    if (c < 0) return 0;
    bits = (bits << 6) | (uint32_t)c;
    nbits += 6;
    while (nbits >= 8) {
      nbits -= 8;
      if (n >= cap) return 0;
      out[n++] = (uint8_t)(bits >> nbits);
      if (nbits) bits &= ((1UL << nbits) - 1U);
      else bits = 0;
    }
  }
  if (nbits) {
    if (n >= cap) return 0;
    out[n++] = (uint8_t)(bits << (8 - nbits));
  }
  return n;
}

static char packed6_char(uint8_t code, bool callsign) {
  if (code >= 1 && code <= 10) return (char)('0' + code - 1);
  if (code >= 11 && code <= 36) return (char)('A' + code - 11);
  if (callsign && code == 37) return '/';
  if (callsign && code == 38) return '-';
  return '\0';
}

uint8_t packed6_get(const uint8_t *src, size_t src_bytes, uint8_t char_pos) {
  const size_t bit = (size_t)char_pos * 6U;
  const size_t byte = bit >> 3;
  const unsigned shift = (unsigned)(bit & 7U);
  if (!src || byte >= src_bytes) return 0;
  uint16_t w = (uint16_t)src[byte] << 8;
  if (byte + 1 < src_bytes) w |= src[byte + 1];
  return (uint8_t)((w >> (10U - shift)) & 0x3fU);
}

bool packed6_decode(const uint8_t *src, size_t src_bytes, uint8_t nchars,
                    bool callsign, char *dst, size_t dst_size) {
  if (!src || !dst || dst_size <= nchars) return false;
  if (packed6_size(nchars) > src_bytes) return false;
  for (uint8_t i = 0; i < nchars; ++i) {
    char c = packed6_char(packed6_get(src, src_bytes, i), callsign);
    if (!c) {
      dst[0] = '\0';
      return false;
    }
    dst[i] = c;
  }
  dst[nchars] = '\0';
  return true;
}

void packed_len5_set(uint8_t *p, uint16_t rec, uint8_t v) {
  const uint32_t bit = (uint32_t)rec * 5U;
  const uint32_t byte = bit >> 3;
  const unsigned sh = bit & 7U;
  uint16_t w = p[byte];
  if (sh > 3) w |= (uint16_t)p[byte + 1] << 8;
  w = (uint16_t)((w & ~((uint16_t)31U << sh)) |
                 ((uint16_t)(v & 31U) << sh));
  p[byte] = (uint8_t)w;
  if (sh > 3) p[byte + 1] = (uint8_t)(w >> 8);
}

uint8_t packed_len5_get(const uint8_t *p, uint16_t rec) {
  const uint32_t bit = (uint32_t)rec * 5U;
  const uint32_t byte = bit >> 3;
  const unsigned sh = bit & 7U;
  uint16_t w = p[byte];
  if (sh > 3) w |= (uint16_t)p[byte + 1] << 8;
  return (uint8_t)((w >> sh) & 31U);
}

bool packed6_equal(const uint8_t *stored, uint8_t stored_len,
                   const uint8_t *query, uint8_t query_len) {
  if (!stored || !query || stored_len != query_len) return false;
  const size_t n = packed6_size(stored_len);
  return memcmp(stored, query, n) == 0;
}

bool packed6_make_masked_query(const char *pattern, char wildcard,
                               packed6_masked_query *q) {
  if (!pattern || !q) return false;
  const size_t len = strlen(pattern);
  if (!len || len > PACKED6_CALL_MAX_CHARS) return false;
  memset(q, 0, sizeof(*q));
  q->nchars = (uint8_t)len;
  q->nbytes = (uint8_t)packed6_size(len);

  for (size_t pos = 0; pos < len; ++pos) {
    if (pattern[pos] == wildcard) continue;
    const int code = packed6_code(pattern[pos], true);
    if (code < 0) return false;
    const size_t bit0 = pos * 6U;
    for (unsigned b = 0; b < 6; ++b) {
      const size_t bit = bit0 + b;
      const size_t by = bit >> 3;
      const uint8_t bm = (uint8_t)(0x80U >> (bit & 7U));
      q->mask[by] |= bm;
      if ((code >> (5U - b)) & 1U) q->value[by] |= bm;
    }
  }
  return true;
}

bool packed6_match_masked(const uint8_t *stored, uint8_t stored_len,
                          const packed6_masked_query *q) {
  if (!stored || !q || stored_len != q->nchars) return false;
  for (uint8_t i = 0; i < q->nbytes; ++i)
    if ((stored[i] & q->mask[i]) != (q->value[i] & q->mask[i])) return false;
  return true;
}

bool packed6_contains(const uint8_t *stored, uint8_t stored_len,
                      const uint8_t *pattern, uint8_t pattern_len) {
  if (!stored || !pattern || !pattern_len || pattern_len > stored_len) return false;
  const size_t sb = packed6_size(stored_len);
  const size_t pb = packed6_size(pattern_len);
  for (uint8_t start = 0; start <= (uint8_t)(stored_len - pattern_len); ++start) {
    bool ok = true;
    for (uint8_t j = 0; j < pattern_len; ++j) {
      if (packed6_get(stored, sb, (uint8_t)(start + j)) !=
          packed6_get(pattern, pb, j)) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}
