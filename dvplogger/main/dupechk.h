/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2025 Eiichiro Araki
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

#ifndef FILE_DUPECHK_H
#define FILE_DUPECHK_H

unsigned char bandmode(struct radio *radio) ;
unsigned char bandmode_param(int bandid,int modetype) ;
bool dupe_check_nocallhist(char *call, byte bandmode, byte mask) ;
void process_dupechk_query_subcpu(char *s);
void process_dupechk_partial_query_subcpu(char *s);
int query_dupechk_partial_subcpu(const char *call, byte bandmode, byte mask,
                                 struct check_entry_list *entry_list);
void process_dupechk_partial_response_maincpu(char *s);

bool dupe_check(struct radio *radio,char *call, byte bandmode, byte mask, bool callhist_check) ;
bool dupe_check_get_callhist(char *call, byte bandmode, byte mask, bool callhist_check,char *getexch,bool *f_getexch,bool *f_callhist);
void entry_dupechk_subcpu(char *s);
void entry_dupechk_call_exch_bandmode(char *callsign,char *recv_exch,unsigned char bandmode);
void entry_dupechk_data(const char *callsign, const char *recv_exch, unsigned char bandmode);
void reset_dupechk_subcpu();
void notify_dupechk_subcpu_reset();
void sync_dupechk_mask_subcpu(unsigned char mask);
void set_dupechk_mask_subcpu(unsigned char mask);
unsigned char get_dupechk_mask_subcpu();
void begin_makedupe_bulk_subcpu(unsigned char mask);
void entry_makedupe_bulk_subcpu(char *s);
void finish_makedupe_bulk_subcpu();
void begin_makedupe_subcpu(unsigned char mask);
void finish_makedupe_subcpu();
void process_makedupe_score_maincpu(char *s, int group);
void entry_makedupe_subcpu_data(const char *callsign, const char *recv_exch, unsigned char bandmode);
void entry_dupechk(struct radio *radio) ;
void init_score() ;
//void init_dupechk() ;

void init_dupechk_maincpu();
void init_dupechk_subcpu();
void init_dupechk(int nmaxqso,int iam);
void task_dupechk();
int get_dupechk_nmaxqso();
int get_dupechk_ncallsign();
//extern struct dupechk *dupechk;
#endif
