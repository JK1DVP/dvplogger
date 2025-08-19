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

#ifndef FILE_USB_HOST_H
#define FILE_USB_HOST_H

// USB host interface MAX3421E defineed by  SS and INT pins by MAX3421E_SS and MAX3421E_INT 
#define MAX3421E_SS P5
#if JK1DVPLOG_HWVER >=2 
#define MAX3421E_INT P25 // enabling PSRAM  moved INT pin moved to GPIO25
#else
#define MAX3421E_INT P17 // original hardware INT pin connected to GPIO17
#endif
#include <hidboot.h>
#include <BTHID.h>
#include "mux_transport.h"



//USB Usb;
//BTD Btd(USB);  // You have to create the Bluetooth Dongle instance like so
//BTHID bthid(BTD);
//HIDBoot<USB_HID_PROTOCOL_KEYBOARD> HidKeyboard(USB);

void receive_pkt_handler_keyboard1_main(struct mux_packet *packet);

#define KEYMSG_TYPE_ONCONTROLKEYSCHANGED 1
#define KEYMSG_TYPE_HANDLELOCKINGKEYS 2
#define KEYMSG_TYPE_ONKEYDOWN 3
#define KEYMSG_TYPE_ONKEYUP 4
struct keymsg_t {
  uint8_t type; // keymessage type 1: OnControlKeysChanged(uint8_t , uint8_t)
  // 2: HandleLockingKeys()
  // 3: OnKeyDown()
  // 4: OnKeyUp()
  uint8_t arg1; // 1st arg
  uint8_t arg2; // 2nd arg
  USBHID *hid;
};

#define QUEUE_KEYRPT_LEN 30
class KbdRptParser : public KeyboardReportParser {
  static  uint8_t symKeysUp_us[12];
  static uint8_t symKeysLo_us[12] ;
  static  uint8_t symKeysUp_jp[12] ;
  static   uint8_t symKeysLo_jp[12] ;    
  int kbd_type=0; // 0 us 1 jp106
  void PrintKey(uint8_t mod, uint8_t key);
  uint8_t buf_ext[8]; // external keyboard control keys state (only 0th is used)
  //  protected:
  
  struct keymsg_t msg;

  QueueHandle_t xQueueKeyRpt;

  virtual const uint8_t *getSymKeysUp() {
    if (kbd_type==1) {    
      return symKeysUp_jp;
    } else {
      return symKeysUp_us;
    }
  };

  virtual const uint8_t *getSymKeysLo() {
    if (kbd_type==1) {        
      return symKeysLo_jp;
    } else {
      return symKeysLo_us;      
    }
  };


  void init_keyrpt_queue() ;
  void send_keyrpt_queue() ;
  
  public:
  void process_keyrpt_queue() ;  
  KbdRptParser() {
    kbdLockingKeys.bLeds = 0;
    init_keyrpt_queue();
    us();
  };
  // here keyboard USB polling is done in separate task and reports to a queue, process_keyrpt_queue() will peek the reports in the queue.
  void Parse(USBHID *hid, bool is_rpt_id, uint8_t len, uint8_t *buf);
  uint8_t HandleLockingKeys(USBHID* hid, uint8_t key);
  void OnControlKeysChanged(uint8_t before, uint8_t after);
  void OnKeyDown(uint8_t mod, uint8_t key);
  void OnKeyUp(uint8_t mod, uint8_t key);
  void OnKeyPressed(uint8_t key);
  void Parse_extKbd(uint8_t hid_code,bool on)   ;
  void init_extKbd();
  uint8_t OemToAscii(uint8_t mod, uint8_t key); // added to use  
  uint8_t OemToAscii2(uint8_t mod, uint8_t key); // added to use
  
  void jp() {
    kbd_type = 1;
  }
  void us() {
    kbd_type = 0;
  }

};

//KbdRptParser Prs;
extern KbdRptParser Prs,Prs1;

void USB_desc(); // print usb descriptors
void ACMprocess() ;
void init_usb();
void loop_usb();
void usb_send_civ_buf() ;
void usb_send_cat_buf(char *cmd) ;
void usb_receive_cat_data(struct radio *radio);
uint8_t kbd_oemtoascii2(uint8_t mod,char c);



#endif
