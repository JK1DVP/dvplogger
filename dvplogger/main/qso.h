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
enum qso_log_append_mode {
  QSO_LOG_APPEND_NORMAL = 0,
  QSO_LOG_APPEND_MERGE = 1
};
size_t qso_log_append_record(const union qso_union_tag *record,
                             qso_log_append_mode mode);
void qso_log_flush();

struct qso_repair_stats {
  unsigned long total_records;
  unsigned long kept_records;
  unsigned long removed_records;
  unsigned long duplicate_groups;
  unsigned long groups_restored_by_zmerge;
  unsigned long ambiguous_server_groups;
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
bool qso_stream_job_busy();
bool start_read_qso_job(Stream *out = nullptr);
bool start_dump_qso_job(Stream *out = nullptr);
bool start_dump_qso_backup_job(const char *numstr, Stream *out = nullptr);
bool cancel_qso_stream_job();

// Sequential Web export.  The source QSO file is opened once and read only
// forward.  Formatted output is handed to the Web client through two small
// FIFO slots; no source offset/seek is used by the browser.
struct qso_web_export_info {
  bool active;
  bool complete;
  bool failed;
  bool cancelled;
  bool chunk_ready;
  uint32_t records_done;
  uint32_t records_total;
  uint32_t elapsed_ms;
  uint32_t bytes;
  uint32_t q_records;
  uint32_t deleted_records;
  uint32_t other_records;
  uint32_t next_sequence;
};
bool start_read_qso_web_job(const char *numstr = nullptr);
bool start_read_qso_web_file_job(const char *numstr = nullptr,
                                     const char *conteststr = nullptr);
bool cancel_read_qso_web_job();
void get_read_qso_web_export_info(struct qso_web_export_info *info);
void touch_read_qso_web_job();
bool ack_read_qso_web_chunk(uint32_t sequence);
bool acquire_read_qso_web_chunk(const uint8_t **data, size_t *len,
                                uint32_t *sequence, bool *eof);
void release_read_qso_web_chunk(uint32_t sequence);
// Pull already-formatted bytes from the sequential Web producer.  This is for
// one long HTTP response: source QSO.TXT remains owned/read by the main loop.
// When no bytes are ready yet, returns 0 with *eof=false so the Web callback
// can return RESPONSE_TRY_AGAIN instead of terminating the response.
size_t pull_read_qso_web_stream(uint8_t *dst, size_t max_len, bool *eof);
uint32_t qsoid_allocate_local();
void qsoid_reconcile_observed(uint8_t txnum, uint32_t observed_ss);
bool qsoid_extract_from_record(const union qso_union_tag *rec, uint32_t *id);
void request_makedupe_rebuild();
void process_pending_makedupe_rebuild();
void open_qsolog() ;
void close_qsolog() ;
void print_qso_entry_file(File *f) ;
void print_qso_entry(union qso_union_tag *qso, Stream *out = nullptr);
void sprint_qso_entry(char *buf,union qso_union_tag *qso);
void sprint_qso_entry_hamlogcsv(char *buf,union qso_union_tag *qso);
void sprint_qso_entry_adif(char *buf,union qso_union_tag *qso) ;
void sprint_qso_entry_cabrillo(char *buf, union qso_union_tag *qso);
bool qso_contest_name(const union qso_union_tag *qso, char *out, size_t out_size);
void string_trim_right(char *s, char c);
void print_qso_logfile() ;
bool parse_strings(const char *remarks, const char *parse_str,
                   char *out, size_t out_size);
void print_qso_log() ;
bool append_secondary_contest_qso(const char *recv_exch, const char *contest_name, bool multi_ok);
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
