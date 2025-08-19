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

#ifndef FILE_ESP32_FLASHER_H
#define FILE_ESP32_FLASHER_H

#ifdef __cplusplus
extern "C" {
#endif
void esp_flasher_sd(char *which);
void esp_flasher(void);
void loader_boot_init_func() ;
void loader_reset_init_func() ;
void loader_port_reset_target_func() ;
void loader_port_enter_bootloader_func() ;
int flash_bin_file_streaming(const char *filepath, uint32_t flash_address);
void flash_file_to_partition(const char *filepath) ;

  
#ifdef __cplusplus
}
#endif

#endif
