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

#ifndef FILE_MULTI_H
#define FILE_MULTI_H
#include "decl.h"
#include "variables.h"

#define MULTI_TYPE_NORMAL 0
#define MULTI_TYPE_JARL_PWR 1
#define MULTI_TYPE_JARL 1
#define MULTI_TYPE_UEC 2  
#define MULTI_TYPE_JARL_PWR_NOMULTICHK 3
#define MULTI_TYPE_NOCHK_LASTCHR 4
#define MULTI_TYPE_GL_NUMBERS 5  // HSWAS
#define MULTI_TYPE_ARRLDX 6 //
#define MULTI_TYPE_ARRL10M 7 //
#define MULTI_TYPE_CQWW 8 // CQWW contest same as MULTI_TYPE_NORMAL but indicates this is DX contest to show entity information
#define MULTI_TYPE_KCWA 9 // check only head two letters 

struct multi_item {
  const char *mul[N_MULTI];
  const char *name[N_MULTI];
};

struct multi_list {
  const struct multi_item *multi[N_BAND]; // multi item definition for each band
  // index is iband-1 (iband=1... N_BAND)
  int n_multi[N_BAND]; // number of multi defined for each band
  bool multi_worked[N_BAND][N_MULTI];
};
//extern void init_multi();
//extern int multi_check(char *s);
//extern int multi_check_old();
//extern void set_arrl_multi();

//extern struct multi_item multi_arrl;
extern struct multi_item multi_test_line ;
extern const struct multi_item multi_musashino_line;
extern const struct multi_item multi_hswas ;
extern const struct multi_item multi_kcj;
extern const struct multi_item multi_saitama_int ;
extern const struct multi_item multi_tokyouhf ;
extern const struct multi_item multi_cqzones ;
extern const struct multi_item multi_tama;
extern const struct multi_item multi_tmtest;
extern const struct multi_item multi_kantou;
extern const struct multi_item multi_allja;
extern const struct multi_item multi_knint;
extern const struct multi_item multi_yk;
extern const struct multi_item multi_ja8int;
extern const struct multi_item multi_yntest;
extern const struct multi_item multi_acag;
extern const struct multi_item multi_arrldx;
extern const struct multi_item multi_arrl10m;
extern const struct multi_item multi_tki;

#endif
