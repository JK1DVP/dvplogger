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

#ifndef FILE_MUX_TRANSPORT_H
#define FILE_MUX_TRANSPORT_H
// this program implements multi-port transport between BT serial, and CH9350 keyboard/mouse, and the main board serial using a single serial connection to the main board
// packeting rule as follows;


// BOP bytes from to data(bytes-3) CKSUM EOP
// bytes max 128
// from port#
// BOP 0xFC
// EOP 0xFB
// port # 0 main board control 
//        1 extension control 
//        2 BT serial source
//        3 BT serial receiver
//        4 USB keyboard1 source
//        5 USB keyboard1 receiver
//        6 CAT2 source
//        7 CAT2 receiver

// extension control receive command to switch modes BT I/F only and normal multiplexed
// "BTONLY"
// "MUX"

#define MUX_PORT_MAIN_BRD_CTRL 0
#define MUX_PORT_EXT_BRD_CTRL 1
#define MUX_PORT_BT_SERIAL_MAIN 2
#define MUX_PORT_BT_SERIAL_EXT 3 // REC is extension board side 
#define MUX_PORT_USB_KEYBOARD1_MAIN 4
#define MUX_PORT_USB_KEYBOARD1_EXT 5 // REC is extension board side
#define MUX_PORT_CAT2_MAIN 6 // SRC is main board side 
#define MUX_PORT_CAT2_EXT 7 // REC is extension board side
#define MUX_PORT_CAT3_MAIN 8 // SRC is main board side 
#define MUX_PORT_CAT3_EXT 9 // REC is extension board side


#include <Arduino.h>
//#define N_PORT 8
#define N_PORT 10
#define N_PACKET_POOL 10
extern int f_mux_transport;
extern int f_mux_transport_cmd; // 1: transition to f_mux_transport=1 2: transition to f_mux_transport=0
struct mux_packet {
  uint8_t status; // 0 free 1 being filled with data 2 ready to be handed to ports
  char *buf;
  uint8_t cksum;
  uint8_t from,to;
  int idx;
  int size;
};  

// handler for each data packet defined in


class Mux_transport {
  uint8_t pkt_status=0;
  unsigned char rbuf[256]; // serial packet receive buffer 
  int rbuf_idx=0;
  
  uint8_t cksum;
  uint8_t  data_bytes;  
  uint8_t from,to;
  unsigned char sbuf[256]; // serial packet send buffer

  struct mux_packet *packet_reading;
  struct mux_packet packet_pool[N_PACKET_POOL];


  typedef void (*callback_func)(struct mux_packet *packet);
  callback_func port_handler[N_PORT]; // handler when packet is received for the port
  
  char bop=0xfc;
  char eop=0xfb;

  enum { pkt_wait_bop,pkt_bytes,pkt_from,pkt_to, pkt_data,pkt_cksum, pkt_eop };
  
public:
  void sync_transport_modes_master() ;  
  void set_port_handler(int to_idx,callback_func handler);
  
  void debug_print(char *s) {
    if (debug_stream!=NULL) {
      if (debug) debug_stream->print(s);
    }
  };
  void debug_print(int i) {
    if (debug_stream!=NULL) {
      if (debug) debug_stream->print(i);
    }
  };
  void debug_print(uint8_t i) {
    if (debug_stream!=NULL) {
      if (debug) debug_stream->print(i,HEX);
    }
  };
  
  uint8_t debug=0;
  Stream *mux_stream=NULL; // packet i/o stream
  Stream *debug_stream=NULL; // debug monitor output to here
  Mux_transport();
  struct mux_packet *get_free_packet() ;
  void recv_pkt();

  int send_pkt(int frm, int to, unsigned char *data,int ndata);
};
extern Mux_transport mux_transport;
#endif
