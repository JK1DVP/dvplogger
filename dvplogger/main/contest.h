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

#ifndef FILE_CONTEST_H
#define FILE_CONTEST_H

#define CW_PH_DUPE_NG 0xff-3  // QSO both CW and PHONE is a DUPE
#define CW_PH_DUPE_OK 0xff  // QSO both CW and PHONE counts point

void set_contest_id();
void search_contest_id_from_name();
bool alternate_contest();
bool select_contest_pair(const char *main_name, const char *sub_name);
void process_pending_contest_pair();
void contest_entry_set_single(const char *name);
void contest_commit_single_mode();
bool restore_saved_contest_selection();
bool previous_contest_info(int *id, char *name, size_t name_size);
bool previous_contest_sent_exch(char *out, size_t out_size);
int previous_contest_multi_check(const char *exch, int bandid);
uint8_t current_contest_dupe_id();
uint8_t previous_contest_dupe_id();
uint8_t contest_dupe_id_for_name(const char *name, int built_in_id);
int contest_dupe_mask_for_name(const char *name, int built_in_id,
                               int fallback_mask);
int previous_contest_dupe_mask();
// Keep independent QSO/multiplier statistics for the active/previous contests.
void contest_stats_capture_current();
void contest_stats_capture_rebuilt_current();
bool contest_stats_restore_current();
void contest_stats_record_secondary(uint8_t contest_id, int modetype, int bandid,
                                    int multi, bool is_dupe);
void contest_stats_begin_rebuild();
void contest_stats_record_rebuild(uint8_t contest_id, unsigned char bandmode,
                                  const char *recv_exch);
void contest_stats_finish_rebuild(bool valid);
int contest_definition_count();
int contest_definition_id(int index);
const char *contest_definition_name(int index);
int contest_definition_mask(int index);
#endif
