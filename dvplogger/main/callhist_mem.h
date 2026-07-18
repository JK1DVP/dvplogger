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

#ifndef FILE_CALLHIST_MEM_H
#define FILE_CALLHIST_MEM_H
extern int size_callhist_list;
extern char **callhist_list;
extern char *callhist_list_mem;
extern int n_callhist_list;
int release_callhist_list_contents();
int init_callhist_list();
char *callhist_call(const char *callsign);
char *callsign_body(const char *callsign);
char *exch_callhist(const char *callsign);
int count_callhist(const char **callhist_list);
int search_callhist_list_exch(const char **callhist_list,const char *callsign, int match_body,char **exch_history) ;
#endif
