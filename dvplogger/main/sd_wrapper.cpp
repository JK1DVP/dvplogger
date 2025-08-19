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
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <Arduino.h>
#include <SD.h>
#include "sd_wrapper.h"

extern "C" {

struct SDFileHandle {
    fs::File file;
};

SDFileHandle *sd_fopen(const char *path, const char *mode) {
    if (!path || !mode) return nullptr;

    fs::File file = SD.open(path, mode);
    if (!file || file.isDirectory()) return nullptr;

    SDFileHandle *handle = new SDFileHandle;
    handle->file = file;
    return handle;
}

size_t sd_fread(void *buf, size_t size, SDFileHandle *handle) {
    if (!handle || !buf) return 0;
    return handle->file.read((uint8_t *)buf, size);
}

size_t sd_fwrite(const void *buf, size_t size, SDFileHandle *handle) {
    if (!handle || !buf) return 0;
    return handle->file.write((const uint8_t *)buf, size);
}

bool sd_feof(SDFileHandle *handle) {
    if (!handle) return true;
    return !handle->file.available();
}

bool sd_fseek(SDFileHandle *handle, size_t pos) {
    if (!handle) return false;
    return handle->file.seek(pos);
}

size_t sd_ftell(SDFileHandle *handle) {
    if (!handle) return 0;
    return handle->file.position();
}

size_t sd_fsize(SDFileHandle *handle) {
    if (!handle) return 0;
    return handle->file.size();
}

void sd_fclose(SDFileHandle *handle) {
    if (!handle) return;
    handle->file.close();
    delete handle;
}

}

