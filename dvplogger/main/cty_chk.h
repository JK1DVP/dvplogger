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

#ifndef FILE_CTY_CHK_H
#define FILE_CTY_CHK_H
#include "cty_chk.h"
#include "cty.h"

int get_entity_info(char *callsign,char *entity, char *entity_desc,
		    int *cqzone, int *ituzone, char *continent,
		    char *lat, char *lon, char *tz);
void show_entity_info(char *callsign);
float calc_azimuth(float to_longitude,float to_latitude,float from_longitude,float from_latitude);
#endif
