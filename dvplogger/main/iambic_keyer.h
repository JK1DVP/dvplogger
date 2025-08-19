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

#ifndef FILE_IAMBIC_KEYER_H
#define FILE_IAMBIC_KEYER_H

#define QUEUE_PADDLE_LEN 50
struct paddle_queue {
  int paddle;
  int voltage;
};
extern struct paddle_queue paddle_queue;
extern QueueHandle_t xQueuePaddle;

extern int paddle_setting; // bit 0 reverse  bit 1 iambic A(1)/B(0)
extern int paddle_voltage ;  // paddle input voltage copy
extern uint8_t g_keyerControl;       // 0x1 Dit latch, 0x2 Dah latch, 0x04 Dit being processed, 0x10 for Iambic A or 1 for B
extern bool g_sidetone_muted;  // If 'true' the Piezo speaker output is muted except for when in command mode
extern uint8_t g_dit_paddle;  // Current PIN number for Dit paddle
extern uint8_t g_dah_paddle;  // Current PIN number for Dah paddle
void set_paddle();
void init_iambic_keyer() ;
void iambic_keyer_handler();
void init_paddle_input(void);
void update_PaddleLatch(int paddle) ;
void tx_key_down() ;
void tx_key_up() ;
void loadWPM(int wpm) ;
int get_Paddle() ;
char morse_code_char(uint8_t d);
void normal_paddle();
void reverse_paddle();
void paddle_iambic_a();
void paddle_iambic_b();
#endif
