/*********************************************************************
 Adafruit invests time and resources providing this open source code,
 please support Adafruit and open-source hardware by purchasing
 products from Adafruit!

 MIT license, check LICENSE for more information
 Copyright (c) 2019 Ha Thach for Adafruit Industries
 All text above, and the splash screen below must be included in
 any redistribution
*********************************************************************/
#ifndef FILE_ADAFRUIT_USBHOST_H
#define FILE_ADAFRUIT_USBHOST_H
#include "Arduino.h"
#include "decl.h"
#include "usbh_helper.h"
extern Adafruit_USBH_CDC SerialHost, SerialHost1;
extern Adafruit_USBH_Host USBHost;
void forward_serial(void);
void usbhost_rtos_task(void *param) ;
void adafruit_usbhost_setup() ;
void adafruit_usbhost_loop() ;

#endif
