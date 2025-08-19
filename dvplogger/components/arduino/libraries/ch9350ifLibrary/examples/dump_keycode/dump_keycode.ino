/**
 * dump_keycode.ino
 * CH9350L keyboard interface library example.
 * show data frame and keyname.
 * Copyright (c) 2022 Takeshi Higasa
 * This software is released under the MIT License.
 */
#if defined(USE_TINYUSB)
#include "Adafruit_TinyUSB.h"
#endif
#include <ch9350if.h>
#include <ch9350if_hidkeys.h>

class _ch9350 : public ch9350if {
  uint8_t led_state = 0;
public:
  _ch9350(uint8_t rst) : ch9350if(rst) {}
  void key_event(uint8_t hid_code, bool on) {
    Serial.print(" : ");
    dump_byte(hid_code);
    Serial.print(on ? " ON  " : " OFF ");
    Serial.println(get_hid_keyname(hid_code));
    if (on) {
      uint8_t led = get_led_state();
      switch(hid_code) {
      case HID_CAPS: // caps
        led ^= HID_LED_CAPSLOCK;
        break;
      case HID_KEYPAD_NUMLOCK: // numlock
        led ^= HID_LED_NUMLOCK;
        break;
      case HID_SCRLOCK: // numlock
        led ^= HID_LED_SCRLOCK;
        break;
      }
      set_led_state(led);
    }    
  }
  void dataframe(uint8_t* data, uint8_t data_length) {
    for(uint8_t i = 0; i < data_length; i++)
      dump_byte(data[i]);    
  }
};

_ch9350 ch9350(CH_RST_PORT);

void setup() {
  ch9350.ch9350_setup();
 // ch9350.set_composite_report_id(1);
  Serial.begin(115200); // for Serial monitor.
  while(!Serial)
    delay(10);
}

void loop() {
  ch9350.ch9350_loop();
}
