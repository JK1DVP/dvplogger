/**
 * show_modifiers.ino
 * CH9350L keyboard interface library example.
 * Show Modifiers state as flags.
 * Copyright (c) 2022 Takeshi Higasa
 * This software is released under the MIT License.
 */
#if defined(USE_TINYUSB)
#include "Adafruit_TinyUSB.h"
#endif
#include <ch9350if.h>
#include <ch9350if_hidkeys.h>

uint8_t mods[] = "____+____";
class _ch9350 : public ch9350if {
public:
  _ch9350(uint8_t rst) : ch9350if(rst) {}
  bool modifiers_changed(uint8_t prev, uint8_t current) {
    uint8_t str[] = "CSAGcsag";
    uint8_t change = prev ^ current;
    if (change) {
      uint8_t mask = 1;
      for(uint8_t i = 0; i < 8; i++, mask <<= 1) {
        uint8_t pos = i;
        if (i > 3)
          pos++;
        if (current & mask)
          mods[pos] = str[i];
        else
          mods[pos] = '_';
      }
      Serial.println((const char*)mods);      
    } 
    return true;  
  }
  void key_event(uint8_t hid_code, bool on) {
    Serial.print((const char*)mods);      
    Serial.print(" : ");
    dump_byte(hid_code);
    Serial.print(get_hid_keyname(hid_code));
    Serial.print(" is ");
    Serial.println(on ? "ON  " : "OFF ");
    if (on) {
      uint8_t led = get_led();
      switch(hid_code) {
      case 0x39: // caps
        led ^= HID_LED_CAPSLOCK;
        break;
      case 0x53: // numlock
        led ^= HID_LED_NUMLOCK;
        break;
      case 0x47: // numlock
        led ^= HID_LED_SCRLOCK;
        break;
      }
      set_led(led);
    }    
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
