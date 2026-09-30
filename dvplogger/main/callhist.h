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

#ifndef FILE_CALLHIST_H
#define FILE_CALLHIST_H
//int release_callhist_list_contents();
//int init_callhist_list();
int read_callhist_list(char *fn);
void print_callhist_list(const char **callhist_list,int n);
//char *callhist_call(const char *callsign);
//char *callsign_body(const char *callsign);
//char *exch_callhist(const char *callsign);
//int count_callhist(const char **callhist_list);
//int search_callhist_list_exch(const char **callhist_list,const char *callsign, int match_body,char **exch_history);
int search_callhist_list(const char **callhist_list,const char *callsign, int match_body);

extern char **callhist_list;
void copy_tail_character(char *dest, char *s);
void init_callhist() ;
void close_callhist() ;
int search_callhist(char *callsign) ;
int search_callhist_getexch (char *callsign,char *getexch) ;
void open_callhist() ;
void release_callhist() ;
void release_callhist_list() ;
void set_callhistfn(char *fn) ;
void show_callhist_status();
void write_callhist(char *s);
void close_callhistf();
// Packed Call History cache (.PKB).  .PCK remains the authoritative source.
bool ensure_callhist_pkb(const char *source_fn);
void callhist_source_updated(const char *source_fn);

// Runtime packed CALLHIST access.  PKB is preferred; legacy PCK RAM is fallback.
struct callhist_iter {
  size_t offset;
  int index;
  size_t current_offset;
  uint16_t filter_sig;
  bool filter_enabled;
};
bool callhist_pkb_load(const char *source_fn);
void callhist_pkb_release();
bool callhist_pkb_loaded();
size_t callhist_pkb_memory_bytes();
int callhist_pkb_count();
void callhist_iter_begin(struct callhist_iter *it);
bool callhist_iter_next(struct callhist_iter *it, char *call, size_t call_size,
                        char *exch, size_t exch_size);
void callhist_iter_set_filter(struct callhist_iter *it, const char *needle);
bool callhist_iter_next_call(struct callhist_iter *it, char *call, size_t call_size);
bool callhist_iter_current_exch(struct callhist_iter *it, char *exch, size_t exch_size);
bool callhist_lookup_exact(const char *callsign, char *exch, size_t exch_size);

#endif
