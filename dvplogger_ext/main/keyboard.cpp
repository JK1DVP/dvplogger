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

#include <Arduino.h>
#include <ch9350if.h>
#include <ch9350if_hidkeys.h>
#include "mux_transport.h"

class _ch9350 : public ch9350if {
  uint8_t led_state = 0;
public:
  _ch9350(uint8_t rst) : ch9350if(rst) {}
  void key_event(uint8_t hid_code, bool on) {
    if (f_mux_transport) {
      unsigned char tmp_buf[2];
      tmp_buf[0]=hid_code;
      tmp_buf[1]=on;
      mux_transport.send_pkt(MUX_PORT_USB_KEYBOARD1_EXT,MUX_PORT_USB_KEYBOARD1_MAIN,tmp_buf,2);
    } else {
      Serial.print(" : ");
      dump_byte(hid_code);
      Serial.print(on ? " ON  " : " OFF ");
      Serial.println(get_hid_keyname(hid_code));
      if (on) {
	uint8_t led = get_led_state();
	switch(hid_code) {
	case HID_CAPS: // caps
	  //        led ^= HID_LED_CAPSLOCK;
	  break;
	case HID_KEYPAD_NUMLOCK: // numlock
	  //        led ^= HID_LED_NUMLOCK;
	  break;
	case HID_SCRLOCK: // numlock
	  led ^= HID_LED_SCRLOCK;
	  break;
	}
	set_led_state(led);
      }
    }
  }
  void dataframe(uint8_t* data, uint8_t data_length) {
    if (!f_mux_transport) {    
      for(uint8_t i = 0; i < data_length; i++)
	dump_byte(data[i]);    
    }
  }
};

//_ch9350 ch9350(CH_RST_PORT);
_ch9350 ch9350(0xff); // without reset port connection

void init_keyboard()
{
  ch9350.ch9350_setup();
}

void loop_keyboard()
{
  ch9350.ch9350_loop();
    
  //  ch9350.set_led_state(HID_LED_CAPSLOCK);
  //    delay(10);
  //    ch9350.set_led_state(0);
  //    delay(10);    
}
// 57 ab 12 00 00 00 00 ff 80 00 20 stop sending
// 57 ab 82 a3
// 57 ab 83/88 len label key_values serial checksum

