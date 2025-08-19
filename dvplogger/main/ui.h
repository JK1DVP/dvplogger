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

#ifndef FILE_UI_H
#define FILE_UI_H

void select_radio(int idx) ;
void change_focused_radio(int new_focus);

int ui_cursor_next_entry_partial_check (struct radio *radio);
int ui_pick_partial_check(struct radio *radio);
int ui_toggle_partial_check_flag();
int ui_perform_exch_partial_check(struct radio *radio);
int ui_perform_partial_check(struct radio *radio);
int ui_response_call_and_move_to_exch(struct radio *radio);
int ui_send_cq(struct radio *radio);
int ui_send_mycall(struct radio *radio);
int ui_send_exch(struct radio *radio);
void send_single_char_radio(struct radio *radio,char c);
void on_key_down(MODIFIERKEYS modkey, uint8_t key, uint8_t c);
//void function_keys(uint8_t key, MODIFIERKEYS modkey, uint8_t c) ;
void function_keys(uint8_t key, uint8_t c) ;
int ui_perform_partial_check(struct radio *radio);
int ui_perform_exch_partial_check(struct radio *radio);

void process_enter(int option) ;
int check_edit_mode() ; // CW key input and Remarks  return 1 (insert) else return 0 (overwrite edit)
void logw_handler(char key, char c);
void 	check_call_show_dx_entity_info(struct radio *radio) ;
void switch_logw_entry(int option) ;
void sat_name_entered() ;
void print_help(int option) ;



#endif
