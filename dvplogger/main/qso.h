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

#ifndef FILE_QSO_H
#define FILE_QSO_H
extern File qsologf;            // qso logf
void init_qsofiles() ;
void init_qso() ;
void makedupe_qso_entry() ;
void reformat_qso_entry(union qso_union_tag *qso) ;
void read_qso_log(int option) ;
int read_qso_log_to_file() ;
void set_qsodata_from_qso_entry() ;
void create_new_qso_log() ;
void open_qsolog() ;
void close_qsolog() ;
void print_qso_entry_file(File *f) ;
void print_qso_entry(union qso_union_tag *qso);
void sprint_qso_entry(char *buf,union qso_union_tag *qso);
void sprint_qso_entry_hamlogcsv(char *buf,union qso_union_tag *qso);
void sprint_qso_entry_adif(char *buf,union qso_union_tag *qso) ;
void string_trim_right(char *s, char c);
void print_qso_logfile() ;

void print_qso_log() ;
// operation options in read_qso_log  or'ed
#define READQSO_MAKEDUPE 1
#define READQSO_PRINT 2
void expand_sent_exch(char *out, size_t out_size);
//char *expand_sent_exch();
void make_qsolog_entry() ;
void make_zlogqsodata(char *buf);
void dump_qso_current() ;
void dump_qso_log() ;
void dump_qso_bak(char *numstr);
#endif
