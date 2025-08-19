/**
  ch9350_hobonicola.ino Main sketch of "Hobo-nicola usb/ble adapter using CH9350L".
  Copyright (c) 2022 Takeshi Higasa

  This file is part of "Hobo-nicola keyboard and adapter".

  "Hobo-nicola keyboard and adapter" is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by the Free Software Foundation, 
  either version 3 of the License, or (at your option) any later version.

  "Hobo-nicola keyboard and adapter" is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS 
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with "Hobo-nicola keyboard and adapter".  If not, see <http://www.gnu.org/licenses/>.

  included in version 1.6.2  May. 15, 2022.
   support Pro Micro(+3.3V/8MHz),QTPy-m0 and XIAO-m0, SSCI ISP1807 Microboard.
*/
#include "hobo_nicola.h"
#include "hobo_sleep.h"
#define USE_SERIAL1 1
#define USE_SPI 0
#include "device_setup.h"
#include <ch9350if.h>

#if defined(NRF52_SERIES)
static const uint8_t FN_BLE_SWITH  = FN_EXTRA_START;
#endif
// Function keys with Fn-key pressed.
static const uint16_t fn_keys[] PROGMEM = {
  HID_U_ARROW, FN_MEDIA_VOL_UP,
  HID_D_ARROW, FN_MEDIA_VOL_DOWN,
  HID_R_ARROW, FN_MEDIA_SCAN_NEXT,
  HID_L_ARROW, FN_MEDIA_SCAN_PREV,
  HID_ENTER,   FN_MEDIA_PLAY_PAUSE,
  HID_DELETE,  FN_MEDIA_STOP,
  HID_END,     FN_MEDIA_MUTE,
  HID_ESCAPE | WITH_R_CTRL,  FN_SYSTEM_SLEEP,   // Ctrl + App + Esc 
  HID_ESCAPE | WITH_R_SHIFT | WITH_R_CTRL,   FN_SYSTEM_POWER_DOWN,
  HID_S     | WITH_R_CTRL , FN_SETUP_MODE,
#if defined(NRF52_SERIES)
  HID_B | WITH_L_CTRL | WITH_L_ALT,  FN_BLE_SWITH,
#endif
  0, 0
};

class ch9350_hobo_nicola : public HoboNicola {
public:
  ch9350_hobo_nicola() {}
  virtual const uint16_t* get_fn_keys() { return fn_keys; }
  virtual void extra_function(uint8_t fk, bool pressed)  ;
  virtual void nicola_led(bool on) { digitalWrite(LED1, on ? 0 : 1); }
  virtual void toggle_nicola_led() { digitalWrite(LED1, digitalRead(LED1) ^ 1); }
  virtual void error_led(bool on) { digitalWrite(LED2, on ? 0 : 1); }
};

void ch9350_hobo_nicola::extra_function(uint8_t k, bool pressed) {
  if (!pressed)
    return;
  switch (k) {
#if defined(NRF52_SERIES)
    case FN_BLE_SWITH:
      releaseAll();
      delay(10);
      if (is_ble_connected())
        stop_ble();
      else
        start_ble();
      break;
#endif
    default:
      break;
  }
}

ch9350_hobo_nicola hobo_nicola;

class _ch9350if : public ch9350if {
public:  
  _ch9350if(uint8_t rst) : ch9350if(rst) { }
  void key_event(uint8_t hid_code, bool on) {
    if (is_usb_suspended()) {
      usb_wakeup();
      delay(400);
      return;
    }
    hobo_nicola.key_event(hid_code, on);
  }
};

_ch9350if ch9350(CH_RST_PORT);  // use D2 for reset CH9350L
void setup() {
  hobo_device_setup();
  HoboNicola::init_hobo_nicola(&hobo_nicola);
  delay(300);
  ch9350.ch9350_setup();
}

void loop() {
  ch9350.set_led_state(hobo_nicola.get_hid_led_state());
  ch9350.ch9350_loop();
  hobo_nicola.idle();
  if (is_usb_suspended()) {
    ch9350.suspend();
    hobo_nicola.error_led(false);
    hobo_nicola.nicola_led(false);
    enter_sleep(2000); // 4000にするとうまくいかない。
    ch9350.resume();
    unsigned long n = millis() + 300;
    while(n > millis()) ch9350.ch9350_loop();  // chance to resume by myself.
  } else
    enter_sleep();
}
