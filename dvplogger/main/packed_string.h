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
#ifndef DVPLOGGER_PACKED_STRING_H
#define DVPLOGGER_PACKED_STRING_H

#include <stddef.h>
#include <stdint.h>

#define PACKED6_CALL_MAX_CHARS 16
#define PACKED6_CALL_MAX_BYTES ((PACKED6_CALL_MAX_CHARS * 6 + 7) / 8)

int packed6_code(char c, bool callsign);
size_t packed6_size(size_t nchars);
size_t packed6_encode(const char *s, bool callsign, uint8_t *out, size_t cap);
bool packed6_decode(const uint8_t *src, size_t src_bytes, uint8_t nchars,
                    bool callsign, char *dst, size_t dst_size);
uint8_t packed6_get(const uint8_t *src, size_t src_bytes, uint8_t char_pos);

void packed_len5_set(uint8_t *p, uint16_t rec, uint8_t v);
uint8_t packed_len5_get(const uint8_t *p, uint16_t rec);

struct packed6_masked_query {
  uint8_t value[PACKED6_CALL_MAX_BYTES];
  uint8_t mask[PACKED6_CALL_MAX_BYTES];
  uint8_t nchars;
  uint8_t nbytes;
};

bool packed6_make_masked_query(const char *pattern, char wildcard,
                               packed6_masked_query *q);
bool packed6_equal(const uint8_t *stored, uint8_t stored_len,
                   const uint8_t *query, uint8_t query_len);
bool packed6_match_masked(const uint8_t *stored, uint8_t stored_len,
                          const packed6_masked_query *q);
bool packed6_contains(const uint8_t *stored, uint8_t stored_len,
                      const uint8_t *pattern, uint8_t pattern_len);

#endif
