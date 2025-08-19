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
// Copyright (c) 2021-2025 Eiichiro Araki
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later




// nRF52 and ESP32 use freeRTOS, we may need to run USBhost.task() in its own rtos's thread.
// Since USBHost.task() will put loop() into dormant state and prevent followed code from running
// until there is USB host event.
#include <Arduino.h>
#if defined(ARDUINO_NRF52_ADAFRUIT) || defined(ARDUINO_ARCH_ESP32)
  #define USE_FREERTOS
#endif

// USBHost is defined in usbh_helper.h
#include "usbh_helper.h"

#if defined(ARDUINO_METRO_ESP32S2)
  Adafruit_USBH_Host USBHost(&SPI, 15, 14);
#elif defined(ARDUINO_ADAFRUIT_FEATHER_ESP32_V2)
  Adafruit_USBH_Host USBHost(&SPI, 33, 15);
#elif defined(ARDUINO_ADAFRUIT_FEATHER_ESP32C6)
  Adafruit_USBH_Host USBHost(&SPI, 8, 7);
#elif defined(ARDUINO_ESP32C3_DEV)
  Adafruit_USBH_Host USBHost(&SPI, 10, 7);
#else
  // Default CS and INT are pin 10, 9
  Adafruit_USBH_Host USBHost(&SPI, 5, 17);
#endif

// CDC Host object
Adafruit_USBH_CDC SerialHost, SerialHost1;

// forward Seral <-> SerialHost
void forward_serial(void) {
  uint8_t buf[64];

  // Serial -> SerialHost
  if (Serial.available()) {
    size_t count = Serial.read(buf, sizeof(buf));
    if (SerialHost && SerialHost.connected()) {
      SerialHost.write(buf, count);
      SerialHost.flush();
    }
  }

  // SerialHost -> Serial
  if (SerialHost.connected() && SerialHost.available()) {
    size_t count = SerialHost.read(buf, sizeof(buf));
    Serial.print("SerialHost:");
    Serial.write(buf, count);
    Serial.flush();
  }
  // SerialHost1 -> Serial
  if (SerialHost1.connected() && SerialHost1.available()) {
    size_t count = SerialHost1.read(buf, sizeof(buf));
    Serial.print("SerialHost1:");
    Serial.write(buf, count);
    Serial.flush();
  }
}

#if defined(CFG_TUH_MAX3421) && CFG_TUH_MAX3421
//--------------------------------------------------------------------+
// Using Host shield MAX3421E controller
//--------------------------------------------------------------------+

#ifdef USE_FREERTOS

//#ifdef ARDUINO_ARCH_ESP32
//  #define USBH_STACK_SZ 2048
  #define USBH_STACK_SZ 8192
//#else
//  #define USBH_STACK_SZ 200
//#endif

void usbhost_rtos_task(void *param) {
  (void) param;
  while (1) {
    USBHost.task();
    //    vTaskDelay(10/portTICK_PERIOD_MS); // check USB every 5 ms
    vTaskDelay(1); // check USB every 5 ms
  }
}
#endif

TaskHandle_t gxHandle_USBloop1;

void adafruit_usbhost_setup() {
  //  Serial.begin(115200);

  // init host stack on controller (rhport) 1
  USBHost.begin(1);

  // Initialize SerialHost
  SerialHost.begin(115200);
  SerialHost1.begin(115200);

  //#ifdef USE_FREERTOS
  // Create a task to run USBHost.task() in background
  //  xTaskCreate(usbhost_rtos_task, "usbh", USBH_STACK_SZ, NULL, 2, &gxHandle_USBloop1);
  //#endif

//  while ( !Serial ) delay(10);   // wait for native usb
  Serial.println("TinyUSB Host Serial Echo Example");
}

void adafruit_usbhost_loop() {
  //#ifndef USE_FREERTOS
  USBHost.task();
  //#endif

  forward_serial();
}

#elif defined(ARDUINO_ARCH_RP2040)
//--------------------------------------------------------------------+
// For RP2040 use both core0 for device stack, core1 for host stack
//--------------------------------------------------------------------+

//------------- Core0 -------------//
void adafruit_usbhost_setup() {
  Serial.begin(115200);
  // while ( !Serial ) delay(10);   // wait for native usb
  Serial.println("TinyUSB Host Serial Echo Example");
}

void loop() {
  forward_serial();
}

//------------- Core1 -------------//
void setup1() {
  // configure pio-usb: defined in usbh_helper.h
  rp2040_configure_pio_usb();

  // run host stack on controller (rhport) 1
  // Note: For rp2040 pico-pio-usb, calling USBHost.begin() on core1 will have most of the
  // host bit-banging processing works done in core1 to free up core0 for other works
  USBHost.begin(1);

  // Initialize SerialHost
  SerialHost.begin(115200);
}

void loop1() {
  USBHost.task();
}

#endif

//--------------------------------------------------------------------+
// TinyUSB Host callbacks
//--------------------------------------------------------------------+
extern "C" {

// Invoked when a device with CDC interface is mounted
// idx is index of cdc interface in the internal pool.
void tuh_cdc_mount_cb(uint8_t idx) {

  // bind SerialHost object to this interface index
  switch(idx) {
    case 0:
      SerialHost.mount(idx);
      break;
    case 1:
      SerialHost1.mount(idx);
      break;
  }
  
     Serial.print("SerialHost is connected to a new CDC device : ");
  Serial.println(idx);
}

// Invoked when a device with CDC interface is unmounted
void tuh_cdc_umount_cb(uint8_t idx) {
  switch (idx) {
  case 0:
  SerialHost.umount(idx);break;
  case 1:    
  SerialHost1.umount(idx);break;
  }
  Serial.print("SerialHost is disconnected :device =");
  Serial.println(idx);
}



// Invoked when device with hid interface is mounted
// Report descriptor is also available for use.
// tuh_hid_parse_report_descriptor() can be used to parse common/simple enough
// descriptor. Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE,
// it will be skipped therefore report_desc = NULL, desc_len = 0
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
  (void) desc_report;
  (void) desc_len;
  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  Serial.printf("HID device address = %d, instance = %d is mounted\r\n", dev_addr, instance);
  Serial.printf("VID = %04x, PID = %04x\r\n", vid, pid);
  if (!tuh_hid_receive_report(dev_addr, instance)) {
    Serial.printf("Error: cannot request to receive report\r\n");
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  Serial.printf("HID device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  Serial.printf("HIDreport : ");
  for (uint16_t i = 0; i < len; i++) {
    Serial.printf("0x%02X ", report[i]);
  }
  Serial.println();
  // continue to request to receive report
  if (!tuh_hid_receive_report(dev_addr, instance)) {
    Serial.printf("Error: cannot request to receive report\r\n");
  }
}

}
