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

#ifndef FILE_MISC_H
#define FILE_MISC_H
#define N_TIME_MEASURE_BANK 62

enum TimeProfileBank {
  PROF_LOOP_TOTAL = 0,
  PROF_MUX_RECV,
  PROF_MEMSTAT,
  PROF_WEB_TERMINAL,
  PROF_WEB_BANDMAP,
  PROF_TCP_SERVER,
  PROF_MORSE_DECODE,
  PROF_MORSE_MONITOR,
  PROF_CARDKEY,
  PROF_KEY_MAIN,
  PROF_KEY_EXTERNAL,
  PROF_QSO_FILE,
  PROF_USER_MD,
  PROF_CONTROL_TX,
  PROF_TIMEKEEP_DISPLAY,
  PROF_TIMEKEEP,
  PROF_SO2R_1,
  PROF_CIV,
  PROF_WIFI,
  PROF_INTERVAL,
  PROF_SIGNAL,
  PROF_ROTATOR,
  PROF_SO2R_2,
  PROF_SATELLITE,
  PROF_CLUSTER,
  PROF_ZSERVER,
  PROF_CW_DISPLAY,
  PROF_CONSOLE,
  PROF_PADDLE_DIAG,
  PROF_MAKEDUPE,
  PROF_MUX_SERVICE,
  PROF_DISPLAY_SERVICE,
  PROF_DISPLAY_REQUEST,
  PROF_DISPLAY_DUPE_STATE,
  PROF_DISPLAY_DUPE_DRAW,
  PROF_DISPLAY_DUPE_FLUSH,
  PROF_DISPLAY_DRAW_MUX_BEFORE,
  PROF_DISPLAY_DRAW_RENDER,
  PROF_DISPLAY_DRAW_MUX_AFTER,
  PROF_DISPLAY_FLUSH_MUX_BEFORE,
  PROF_DISPLAY_FLUSH_OLED,
  PROF_DISPLAY_FLUSH_MUX_AFTER,
  PROF_CIV_RX,
  PROF_CIV_FRAME,
  PROF_CIV_PRINT,
  PROF_CIV_GET,
  PROF_CIV_CLEAR,
  PROF_CIV_QUERY,
  PROF_CIV_TAIL,
  PROF_WEB_BAND_DUPE,
  PROF_WEB_BAND_CMD,
  PROF_WEB_BAND_SNAPSHOT,
  PROF_WEB_BAND_START,
  PROF_MUX_PACKET_HANDLER,
  PROF_MUX_DUPE_ACK,
  PROF_MUX_DUPE_RESULT,
  PROF_DUPE_RESULT_PARSE,
  PROF_DUPE_RESULT_CALLHIST,
  PROF_DUPE_RESULT_COMMIT,
  PROF_DUPE_RESULT_PARTIAL_UI,
  PROF_DUPE_RESULT_DISPLAY,
  PROF_DUPE_RESULT_PENDING_SEND,
  PROF_BANK_COUNT
};
void copy_token(char *dest,char *src,int idx,const char *sep) ;
void time_measure_clear(int bank);
void time_measure_start(int bank);
void time_measure_start_name(int bank, const char *name);
void time_measure_stop(int bank);
int time_measure_get(int bank);
uint64_t time_measure_get_total(int bank);
uint32_t time_measure_get_calls(int bank);
const char *time_measure_get_name(int bank);
unsigned int reverse_bits(unsigned int bin,int digits);
void print_bin(char *print_to, unsigned int bin, int digits) ;
void set_location_gl_calc(char *locator) ;
void release_memory() ;
void print_memory();
void i2c_scan(Stream *out = nullptr);
void memtrace_event(const char *tag);
void memtrace_poll();
#endif
