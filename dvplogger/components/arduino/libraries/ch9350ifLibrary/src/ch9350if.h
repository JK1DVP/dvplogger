/**
 * ch9350if.h
 * Definitions of CH9350L keyboard interface library for Arduino.
 * Copyright (c) 2022 Takeshi Higasa, okiraku-camera.tokyo
 *
 * This file is part of ch9350ifLibrary.
 *
 * ch9350ifLibrary is free software: you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, either version 3 of the License, or 
 * (at your option) any later version.
 *
 * ch9350ifLibrary is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ch9350ifLibrary.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "Arduino.h"
#include "ch9350if_settings.h"

#ifndef __CH9350L_H__
#define __CH9350L_H__

static const uint8_t keys_count = 8;     // key_array size of input report.
static const uint8_t report_size = keys_count + 2; // key_array and modifiers and reserved.
static const uint8_t data_size = report_size + 3; // (dev_kind, report, serial, sum)
static const uint8_t version_frame[] =   { 0x57,0xAB,0x12,0x00,0x00,0x00,0x00,0x00,0xFF,0xA0,0x07 };
static const uint8_t status_frame[] =    { 0x57,0xAB,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0xAC,0x20 };
// was static const uint8_t status_frame[] =    { 0x57,0xAB,0x12,0x00,0x00,0x00,0x00,0x00,0x81,0xAC,0x20 }; // with 0x81, no change in LED status by modifying 
static const uint8_t stopstatus_frame[] ={ 0x57,0xAB,0x12,0x00,0x00,0x00,0x00,0xFF,0x80,0x00,0x20 };
static const uint8_t state1_frame[] ={ 0x57,0xAB,0x12,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x20 };


class ch9350if {
  //  static const uint8_t led_value_index = 7;
  static const uint8_t led_value_index = 7;  

  // ch9350_loop state and variables
  enum { has_none = 0, has_57, has_57ab, has_op, wait_data };
  uint8_t ch_rx_state;
  uint8_t ch_opcode;
  uint8_t ch_data_length;
  uint8_t ch_rx_length;
	
  uint8_t rst_port;
  uint8_t led_state;
  uint8_t response_frame[sizeof(status_frame)];
  int8_t composite_keyboard;
  uint8_t composite_report_id;



  bool sum_check(uint8_t datalen);
  void _init() {
    ch_rx_state = has_none;
    ch_opcode = 0;
    ch_data_length = 0;
    ch_rx_length = 0;
    composite_keyboard = -1;
    memset(prev_data, 0, data_size);	
  }
  void reinit();

  unsigned long timeout=0;  
public:
  ch9350if(uint8_t _rst_port = 0xff) {
    _init();
    composite_report_id = 1;
    led_state = 0;
    rst_port = _rst_port;
    memcpy(response_frame, status_frame, sizeof(status_frame));
  };
	
  void ch9350_setup();
  void ch9350_loop();
  void set_led_state(uint8_t led) {
    led_state = led;
    //    if (response_frame[led_value_index] != led_state) {
    //      response_frame[led_value_index] = led_state;
    //      CH_SERIAL.write(response_frame, sizeof(response_frame));
    //    }
    
  }
  uint8_t get_led_state() { return led_state; }

  void reset();
  void suspend();
  void resume();

  virtual void key_event(uint8_t hid_code, bool on) {};
  virtual void dataframe(uint8_t* data, uint8_t data_length)  {};
  virtual void newframe() {};
  virtual void rx_byte(uint8_t c) {};
  virtual bool modifiers_changed(uint8_t prev, uint8_t current) { return false; };

  void set_composite_report_id(uint8_t id = 0) { composite_report_id = id; }
protected:
  void parse_report(uint8_t datalen);
  uint8_t data_frame[data_size];
  uint8_t prev_data[data_size];
};

#endif
