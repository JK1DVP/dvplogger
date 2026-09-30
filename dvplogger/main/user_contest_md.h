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
#ifndef FILE_USER_CONTEST_MD_H
#define FILE_USER_CONTEST_MD_H

#include <stddef.h>

#define USER_MD_CONTEST_ID 44
#define USER_MD_RUNTIME_ID_FIRST 128
#define USER_MD_RUNTIME_ID_LAST  254

bool is_user_md_contest_name(const char *contest_name);
bool canonicalize_user_md_contest_name(const char *contest_name,
                                       char *out, size_t out_size);
bool start_user_md_contest(const char *contest_name);
// Used when the selected User contest has no .MD file.  The logger remains
// active with dupe checking but without multiplier validation/counting.
void set_user_md_fallback_dupe_mask(int mask);
void process_user_md_contest();
bool user_md_contest_loading();
int user_md_multi_check(const char *exchange, int bandid);
int user_md_multi_check_for(const char *contest_name,
                            const char *exchange, int bandid);
// Persistent one-byte identity used by the shared DUPE pool.  create=true is
// accepted only after the corresponding MD file has been opened successfully.
uint8_t user_md_runtime_id(const char *contest_name, bool create);
void release_user_md_contest();

#endif
