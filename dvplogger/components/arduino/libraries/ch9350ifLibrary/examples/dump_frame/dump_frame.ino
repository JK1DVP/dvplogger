/**
 * dump_frame.ino
 * CH9350L keyboard interface library example.
 * show frame from ch9350l.
 * Copyright (c) 2022 Takeshi Higasa
 * This software is released under the MIT License.
 */
#if defined(USE_TINYUSB)
#include "Adafruit_TinyUSB.h"
#endif
#include <ch9350if.h>
#include <ch9350if_hidkeys.h>

class _ch9350 : public ch9350if {
public:
  _ch9350(uint8_t rst) : ch9350if(rst) {}
  void newframe() { Serial.println(); }
  void rx_byte(uint8_t c) { dump_byte(c); }
};

_ch9350 ch9350(CH_RST_PORT);

void setup() {
  ch9350.ch9350_setup();
  Serial.begin(115200); // for Serial monitor.
  while(!Serial)
    delay(10);
}

void loop() {
  ch9350.ch9350_loop();
}
