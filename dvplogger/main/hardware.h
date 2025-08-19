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

#ifndef FILE_HARDWARE_H
#define FILE_HARDWARE_H

//#define SERIAL1_RX 16
#define SERIAL1_RX 34
#define SERIAL1_TX 4

//#define SERIAL2_RX 32
#define SERIAL2_RX 35
#define SERIAL2_TX 33

// Serial3 port hardware assignment
#define CATRX 15  // this is  used for subcpu EN(reset) in the ver2 hardware 25/4/20
#define CATTX 27  // this is used for subcpu BOOT(GP0)  in the ver2 hardware 25/4/20

#if JK1DVPLOG_HWVER >= 2
#define CW_KEY1 2 // PSRAM enabled --> KEY1 wiring moved to IO2
#else
#define CW_KEY1 16 // 
#endif

#define CW_KEY2 32
#define LED 2

#endif
