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

#ifndef FILE_CAT_H
#define FILE_CAT_H
#include "Arduino.h"
#include "decl.h"
#include "mux_transport.h"


#define RIG_TYPE_ICOM_IC705 0 // IC705
#define RIG_TYPE_ICOM_IC9700 1  // IC9700
#define RIG_TYPE_ICOM_IC7300 5  // IC7300
#define RIG_TYPE_YAESU 2 // FTDX10 and FT991A
#define RIG_TYPE_KENWOOD 3 // QCX_MINI and TS-2000 etc 
#define RIG_TYPE_MANUAL 4 // manual rig
//#define RIG_TYPE_YAESU_FTDX3000 5
//#define RIG_TYPE_IC7300 6  // IC7300  ( may be OK with IC705 by receiving ID)

#define CAT_TYPE_CIV 0
#define CAT_TYPE_YAESU_NEW 1
#define CAT_TYPE_KENWOOD 2
#define CAT_TYPE_NOCAT 3
#define CAT_TYPE_YAESU_OLD 4  // FT3000


struct catmsg_t {  // data structure to exchange cat/ci-v data by freertos queue
  uint8_t type; // not sure how to use this 
  int size; // number of data in the buffer buf
  uint8_t buf[64]; // data container  fixed size
} ;

extern QueueHandle_t xQueueCATUSBRx,xQueueCATUSBTx,xQueueCATUSBCtrlLine; // message queue for controling 
#define QUEUE_CATUSB_RX_LEN 5
#define QUEUE_CATUSB_TX_LEN 5

extern byte civ_buf[65];
extern int civ_buf_idx ;
byte dec2bcd(byte data) ;
byte bcd2dec(byte data) ;
void init_mux_serial();
void deinit_mux_serial();
int rig_modenum(const char *opmode) ;
char *opmode_string(int modenum) ;
int is_manual_rig(struct radio *radio) ;
void set_frequency_rig_radio(unsigned int freq, struct radio *radio) ;
void set_frequency_rig(unsigned int freq);
void print_cat_cmdbuf(struct radio *radio);
void attach_interrupt_civ();
void detach_interrupt_civ();
void send_head_civ(struct radio *radio);
void send_tail_civ(struct radio *radio);
void set_ptt_rig(struct radio *radio, int on) ;
void send_voice_memory(struct radio *radio, int num);
void send_rtty_memory(struct radio *radio, int num);
void add_civ_buf(byte c) ;
void clear_civ_buf() ;
void send_civ_buf_radio(struct radio *radio) ;
void send_civ_buf(Stream *civport);
void send_cat_cmd(struct radio *radio, char *cmd);
void receive_cat_data(struct radio *radio) ;
int freq_width_mode(char *opmode);
void set_scope_mode(struct radio *radio,int mode);
void set_scope() ;
void send_rit_setting(struct radio *radio, int rit, int xit) ;
void send_rit_freq_civ(struct radio *radio, int freq) ;
void send_freq_set_civ(struct radio *radio, unsigned int freq) ;
void send_mode_set_civ(const char *opmode, int filnr) ;
void send_gps_query_civ(struct radio *radio) ;
void send_freq_query_civ(struct radio *radio) ;
void send_mode_query_civ(struct radio *radio) ;
void send_rotator_head_civ(int from, int to) ;
void send_rotator_tail_civ() ;
void send_rotator_az_query_civ() ;
void send_rotator_az_set_civ(int az) ;
void send_rotator_command_civ(byte *cmds, int n) ;
void send_ptt_query_civ(struct radio *radio) ;
void send_preamp_query_civ(struct radio *radio) ;
void send_identification_query_civ(struct radio *radio) ;
void send_att_query_civ(struct radio *radio) ;
void send_smeter_query_civ(struct radio *radio) ;
void set_frequency(int freq, struct radio *radio) ;
void set_mode_nonfil(const char *opmode, struct radio *radio) ;
void set_mode(const char *opmode, byte filt, struct radio *radio) ;

void get_cat_kenwood(struct radio *radio) ;
void get_cat(struct radio *radio) ;
void conv_smeter(struct radio *radio) ;
void smeter_postprocess(struct radio *radio);
struct radio *search_civ_address(int civaddr);
void get_civ(struct radio *radio) ;
void print_civ(struct radio *radio) ;
void print_cat(struct radio *radio) ;

extern int receive_civport_count; // debugging receive_civport
void receive_pkt_handler_btserial(struct mux_packet *packet);
void receive_pkt_handler_cat2(struct mux_packet *packet);
void receive_pkt_handler_cat3(struct mux_packet *packet);
void receive_pkt_handler_portnum(struct mux_packet *packet,int port_num);

void receive_civport(struct radio *radio) ;
void receive_civport_1() ;
void clear_civ(struct radio *radio) ;
int antenna_switch_commands(char *cmd);
int signal_command(char *s);
int rotator_commands(char *s);
void RELAY_control(int relay, int on);
void switch_radios(int idx_radio, int radio_cmd);
void alternate_antenna_relay();
void select_rig(struct radio *radio) ;
void init_cat_usb() ;
void init_cat_kenwood() ;
void init_cat_serialport();
void init_rig() ;
void init_all_radio() ;
void print_radio_info(int idx_radio);
void print_serial_instance();
void print_rig_spec_str(int rig_idx,char *buf); // reverse set rig_spec_string from rig_spec

int check_rig_conflict(int rig_idx,struct rig *rig_spec);
void load_rigs(char *fn);
void save_rigs(char *fn);
void init_radio(struct radio *radio, const char *rig_name);
void set_rig_spec_from_str(struct radio *radio, char *s);
void set_rig_spec_str_from_spec(struct radio *radio); // reverse set rig_spec_string from rig_spec
void set_rig_spec_from_str_rig(struct rig *rig_spec,const char *s);
void save_rigs(char *fn);
void set_rig_from_name(struct radio *radio) ;
void switch_transverter() ;
void signal_process();
void civ_process() ;
int receive_civ(struct radio *radio) ;
int unique_num_radio(int i) ;
void Control_TX_process() ;
void rotator_sweep_process();
void set_sat_opmode(struct radio *radio, char *opmode) ;

void adjust_frequency(int dfreq);
int antenna_alternate_command(char *s);

void save_freq_mode_filt(struct radio *radio) ;
void recall_freq_mode_filt(struct radio *radio) ;
int bandid2freq(int bandid, struct radio *radio) ;
char *default_opmode(int bandid, int modetype) ;
int default_filt(const char *opmode) ;


#endif
