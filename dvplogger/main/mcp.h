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

#ifndef FILE_MCP_H
#define FILE_MCP_H
#include <Arduino.h>
#include <Adafruit_MCP23X17.h>
extern Adafruit_MCP23X17 mcp;

void init_mcp_port() ;
void phone_switch(int left, int right) ;
void write_mcp_gpio(int tmp,int tmp1);
int read_mcp_gpio(int i);
void mic_switch(int sel) ;
void ptt_switch(int sel,int on) ;
#endif
