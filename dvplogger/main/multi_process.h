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

#ifndef FILE_MULTI_PROCESS_H
#define FILE_MULTI_PROCESS_H
#include "multi.h"
#include "contest.h"

void init_multi(const struct multi_item *multi, int start_band, int stop_band) ;
//void init_multi() ;
void clear_multi_worked() ;
int multi_check_option(char *s,int bandid,int option);   // s: exch (such as in plogw->recv_exch +2)
int multi_check(char *s,int bandid);   // s: exch (such as in plogw->recv_exch +2)
//int multi_check(char *s);
void print_multi_list();
int multi_check_old() ;
extern struct multi_list multi_list;
void entry_multiplier(struct radio *radio);
void reverse_search_multi() ;

#endif
