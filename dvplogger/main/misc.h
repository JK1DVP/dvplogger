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

#ifndef FILE_MISC_H
#define FILE_MISC_H
#define N_TIME_MEASURE_BANK 20
void copy_token(char *dest,char *src,int idx,char *sep) ;
void time_measure_clear(int bank);
void time_measure_start(int bank);
void time_measure_start_name(int bank,char *name);
void time_measure_stop(int bank);
int time_measure_get(int bank);
unsigned int reverse_bits(unsigned int bin,int digits);
void print_bin(char *print_to, unsigned int bin, int digits) ;
void set_location_gl_calc(char *locator) ;
void release_memory() ;
void print_memory();
void i2c_scan();
#endif
