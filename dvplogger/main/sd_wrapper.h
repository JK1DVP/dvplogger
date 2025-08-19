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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct SDFileHandle SDFileHandle;

SDFileHandle *sd_fopen(const char *path, const char *mode);
size_t sd_fread(void *buf, size_t size, SDFileHandle *handle);
size_t sd_fwrite(const void *buf, size_t size, SDFileHandle *handle);
bool sd_feof(SDFileHandle *handle);
bool sd_fseek(SDFileHandle *handle, size_t pos);
size_t sd_ftell(SDFileHandle *handle);
size_t sd_fsize(SDFileHandle *handle);
void sd_fclose(SDFileHandle *handle);

#ifdef __cplusplus
}
#endif
