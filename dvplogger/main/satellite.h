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

#ifndef FILE_SATELLITE_H
#define FILE_SATELLITE_H
extern int satidx_sort[N_SATELLITES];
void load_satinfo() ;
void readtlefile() ;
void set_sat_index(char *sat_name) ;
void adjust_sat_offset(int dfreq) ;
void set_sat_info2(char *sat_name) ;
void set_sat_info_calc() ;
void set_vfo_frequency_rig(int freq, int vfo, struct radio *radio);
void set_sat_freq_to_rig_vfo(int freq, int tx);
void set_sat_freq_up() ;
void set_sat_freq_dn() ;
void set_sat_freq_to_rig() ;
void set_sat_freq_calc() ;
void set_sat_center_frequency() ;
void set_sat_beacon_frequency() ;

void set_location_gl_calc(char *locator) ;
void freq2str(char *s, int freq) ;
void upd_display_sat() ;
void print_sat_info_by_index(int i) ;
void print_sat_info(char *sat_name) ;
int compare_datetime(DateTime a, DateTime b) ;
void print_sat_info_aos() ;
void init_sat() ;
void allocate_sat() ;
void release_sat() ;
int find_satname(char *satname) ;
void readtlefile() ;
void getTLE() ;
void plan13_test() ;
void save_satinfo() ;
void print_datetime(DateTime time) ;
DateTime add_datetime(DateTime time, int seconds) ;
void start_calc_nextaos();
void sat_find_nextaos_sequence() ;
void sat_calc_position(int satidx, DateTime time) ;
void sat_process() ;
#endif
