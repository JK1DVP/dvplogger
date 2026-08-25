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

#ifndef FILE_QSO_H
#define FILE_QSO_H
extern File qsologf;            // qso logf
// QSO log access helpers for modules that must not manipulate qsologf directly.
bool qso_log_is_open();
bool open_qso_log_readonly(File *f);
void close_qso_log_readonly(File *f);
int read_qso_log_record(File *f, union qso_union_tag *record);
size_t append_qso_log_record(const union qso_union_tag *record,
                             size_t *size_before, size_t *size_after);

struct qso_repair_stats {
  unsigned long total_records;
  unsigned long kept_records;
  unsigned long removed_records;
  unsigned long duplicate_groups;
  unsigned long groups_restored_by_zmerge;
};

// Rebuild QSO.TXT through a temporary file. The original is preserved as
// QSO.TXT.zbackup until the next successful repair.
bool repair_qso_log(const uint32_t *server_ids, size_t server_count,
                    struct qso_repair_stats *stats);
void init_qsofiles() ;
void init_qso() ;
void makedupe_qso_entry(const union qso_union_tag *record) ;
void process_makedupe_multiplier_maincpu(const char *recv_exch, unsigned char bandmode);
void reformat_qso_entry(union qso_union_tag *qso) ;
void read_qso_log(int option, Stream *out = nullptr) ;
int read_qso_log_to_file() ;
void set_qsodata_from_qso_entry() ;
void create_new_qso_log() ;
bool switch_qso_log(int backup_number);
void list_qso_backup_files();
void process_qso_file_operation();
bool qso_file_operation_busy();
void request_makedupe_rebuild();
void process_pending_makedupe_rebuild();
void open_qsolog() ;
void close_qsolog() ;
void print_qso_entry_file(File *f) ;
void print_qso_entry(union qso_union_tag *qso, Stream *out = nullptr);
void sprint_qso_entry(char *buf,union qso_union_tag *qso);
void sprint_qso_entry_hamlogcsv(char *buf,union qso_union_tag *qso);
void sprint_qso_entry_adif(char *buf,union qso_union_tag *qso) ;
void string_trim_right(char *s, char c);
void print_qso_logfile() ;
bool parse_strings(const char *remarks, const char *parse_str,
                   char *out, size_t out_size);
void print_qso_log() ;
// operation options in read_qso_log  or'ed
#define READQSO_MAKEDUPE 1
#define READQSO_PRINT 2
void expand_sent_exch(char *out, size_t out_size);
//char *expand_sent_exch();
void make_qsolog_entry() ;
void make_zlogqsodata(char *buf);
void dump_qso_current(Stream *out = nullptr) ;
void dump_qso_log(Stream *out = nullptr) ;
void dump_qso_bak(char *numstr, Stream *out = nullptr);
#endif
