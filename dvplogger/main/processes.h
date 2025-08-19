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

#ifndef FILE_PROCESSES_H
#define FILE_PROCESSES_H
void interval_process();
//
//void check_repeat_function(); // obsolete ->void sequence_manager_tx_status_updated();


// repeat function status 
#define REPEAT_FUNC_STAT_DEFAULT  0
#define REPEAT_FUNC_STAT_SENDING_MSG  1
#define REPEAT_FUNC_STAT_SENDING_MSG_FINISHED  3
#define REPEAT_FUNC_STAT_TIMER_STARTED  4
#define REPEAT_FUNC_STAT_TIMER_EXPIRED  5
#define REPEAT_FUNC_STAT_SENDING_CQ_0 6
#define REPEAT_FUNC_STAT_SENDING_CQ_0_CALLNUMREQ_1 7
#define REPEAT_FUNC_STAT_SENDING_CQ_0_TUREQ_1 8
#define REPEAT_FUNC_STAT_SENDING_CQ_1 9
#define REPEAT_FUNC_STAT_SENDING_CQ_1_CALLNUMREQ_0 10
#define REPEAT_FUNC_STAT_SENDING_CQ_1_TUREQ_0 11

#endif
