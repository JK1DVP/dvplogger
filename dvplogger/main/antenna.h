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
#ifndef FILE_ANTENNA_H
#define FILE_ANTENNA_H

#include <Arduino.h>
#include "decl.h"

#define ANTENNA_RADIOS 2
#define ANTENNA_MAX_ID 9
#define ANTENNA_PREF_ROWS 3

extern int antenna_control_enable;
extern char antenna_host[64];
extern int antenna_port;
extern char antenna_pref[ANTENNA_PREF_ROWS][N_BAND + 1];
extern char antenna_name[ANTENNA_MAX_ID][24];

void antenna_process();
void antenna_force_resend();
void antenna_settings_changed();
String antenna_status_json();
const char *antenna_controller_state();

#endif
