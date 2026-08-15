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
#ifndef FILE_CALLHIST_REMOTE_H
#define FILE_CALLHIST_REMOTE_H
#include "Arduino.h"
#include "decl.h"
extern int callhist_at; // 0: main CPU, 1: sub CPU
bool load_callhist_subcpu(const char *fn);
bool callhist_subcpu_alive(uint32_t timeout_ms = 350);
void clear_callhist_subcpu_main();
void process_callhist_control_response_main(const char *buf);
void process_callhist_reset_subcpu(const char *s);
void process_callhist_entry_subcpu(char *s);
void process_callhist_end_subcpu();
bool search_callhist_subcpu_local(const char *call, char *exch, size_t exch_size);
int append_callhist_partial_subcpu(const char *call, struct check_entry_list *entry_list,
                                   int max_entries);
int get_callhist_subcpu_count();
size_t get_callhist_subcpu_bytes();
bool get_callhist_subcpu_entry(int index, const char **call, const char **exch);
#endif
