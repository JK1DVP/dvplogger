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


#include "Arduino.h"

#include <Wire.h>
#include "Ticker.h"
#include "keyboard.h"
#include "mux_transport.h"

//////
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_spi_flash.h"

#include <SoftwareSerial.h>
SoftwareSerial Serial3;


//#include "dac_playback.h"
#include "AudioPlayer.h"

// issues
// 25/4/21
// Software serial data receive corruption (loop too slow?)
// occurs very often when BTserial has a traffic

// Bluetooth modem for esp32-jk1dvplog E. Araki 24/8/24
// to link IC-705 and esp32-jk1dvplog by Bluetooth
//
// made from the following demo code SerialToSerialBT.ino
// 
// This example code is in the Public Domain (or CC0 licensed, at your option.)
// By Evandro Copercini - 2018
//
// This example creates a bridge between Serial and Classical Bluetooth (SPP)
// and also demonstrate that SerialBT have the same functionalities of a normal Serial
// Note: Pairing is authenticated automatically by this device

// to suppress Bluetooth debug information menuconfig and go to Bluetooth options to disable log information for Bluetooth 

#include "BluetoothSerial.h"
//#include "dac_playback.h"
#include "main.h"


Ticker serial_reader;


String device_name = "ESP32-BT";

// Check if Bluetooth is available
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

// Check Serial Port Profile
#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Port Profile for Bluetooth is not available or not enabled. It is only available for the ESP32 chip.
#endif

BluetoothSerial SerialBT;
uint8_t buf[2048];// serialBT receive buffer
uint8_t buf1[2048]; // Serial receive buffer
uint8_t buf2[2048]; // CAT2 Serial receive buffer
uint8_t buf3[2048]; // CAT3 Serial receive buffer
int count=0,count1=0,count2=0,count3=0;
// cat2 serial setting  (defaults fits FTDX10)
int cat2_baud=38400;
int cat2_reversed=1;

int cat3_baud=38400;
int cat3_reversed=1;

void read_serial_ports() // read serialports and buffer
{
  // cat2
  char c;
  int counta=0;
  while (Serial2.available() && counta<20) {
    c=Serial2.read();
    buf2[count2++]=c;
    counta++;
    // Serial.print("c:");
    //Serial.println(count);
    
    if ((c==0x0a)||(c==0xfd)||(c==';')) {
      if (f_mux_transport) {
	// f_mux_transport to send data from SerialBT to main board 
	mux_transport.send_pkt(MUX_PORT_CAT2_EXT,MUX_PORT_CAT2_MAIN,buf2,count2);
      } else {
	Serial.write(buf2,count2);	
      }
      count2=0;      
    }
    if (count2>100) { // originally 2000 shortened buffer length
      if (f_mux_transport) {
	mux_transport.send_pkt(MUX_PORT_CAT2_EXT,MUX_PORT_CAT2_MAIN,buf2,count2);
      } else {
	Serial2.write(buf2,count2);
      }
      count2=0;
    }
  }

  counta=0;
  while (SerialBT.available()&&(counta<20) ) {
    c=SerialBT.read();
    buf[count++]=c;
    counta++;
    // Serial.print("c:");
    //Serial.println(count);
    if ((c==0x0a)||(c==0xfd)) {
      if (f_mux_transport) {
	// f_mux_transport to send data from SerialBT to main board 
	mux_transport.send_pkt(MUX_PORT_BT_SERIAL_EXT,MUX_PORT_BT_SERIAL_MAIN,buf,count);
      } else {
	Serial.write(buf,count);	
      }
      count=0;      
    }
    if (count>100) { // originally 2000 shortened buffer length
      if (f_mux_transport) {
	mux_transport.send_pkt(MUX_PORT_BT_SERIAL_EXT,MUX_PORT_BT_SERIAL_MAIN,buf,count);
      } else {
	Serial.write(buf,count);
      }
      count=0;
    }
  }
  
}


void cat2_pkt_handler(struct mux_packet *packet)
{
  Serial2.write(packet->buf,packet->idx);
}

void cat3_pkt_handler(struct mux_packet *packet)
{
  Serial3.write(packet->buf,packet->idx);
}

void serialbt_pkt_handler(struct mux_packet *packet)
{
  SerialBT.write((uint8_t *)packet->buf,packet->idx);
}

// cw keying related rountine
void cwbuf_control(char *cmd)
{
  // if KEY31 .. does not work ok implement interrupt based cw keying for key3
}

void key_control(char *cmd)
{
  switch(cmd[0]) {
  case '3': // key 3 port
    switch(cmd[1]) {
    case '1':// on
      digitalWrite(KEY3,1);
      break;
    case '0':// off
      digitalWrite(KEY3,0);
      break;
    }
    break;
  }
}

void control_pkt_handler(struct mux_packet *packet)
{
  int ret;
  char buf[100];
  // check content
  if (strncmp(packet->buf,"go_no_mux",9)==0) {
    f_mux_transport=0;
    return;
  }
  if (strncmp(packet->buf,"play",4)==0) {
    // playback command
    strncpy(buf,packet->buf+4,packet->idx-4);
    buf[packet->idx-4]='\0';
    //    play_sound(packet->buf+4);
    play_sound(buf);
    return;
  }
  if (strncmp(packet->buf,"key",3)==0) {
    // cw key on/off
    key_control(packet->buf+3);
    return;
  }
    
  if (strncmp(packet->buf,"cwbuf",5)==0) {
    // control cw/phone sending buffer
    cwbuf_control(packet->buf+5);
    return;
  }
  // setting cat2 parameter
  if (strncmp(packet->buf,"cat2_param",10)==0) {
    cat2_reversed=0;
    ret=sscanf(packet->buf+10,"%d %d",&cat2_baud,&cat2_reversed) ;
    // set serial
    Serial2.begin(cat2_baud,SERIAL_8N1,
		  27,
		  15,
		  cat2_reversed);
    return;
  }
  if (strncmp(packet->buf,"cat3_param",10)==0) {
    cat3_reversed=0;
    ret=sscanf(packet->buf+10,"%d %d",&cat3_baud,&cat3_reversed) ;
    // set serial
    //    Serial3.begin(cat3_baud,SERIAL_8N1,
    //		  27,
    //		  15,
    //		  cat3_reversed);
#define CATRX 35 
#define CATTX 33 
    Serial3.begin(cat3_baud,SWSERIAL_8N1,CATRX,CATTX,cat3_reversed,95,0);
    Serial3.enableIntTx(0); // disable interrupt diuring transmitting signal
    return;
  }


  
  
}

void setup() {
  Serial1.setRxBufferSize(512);
  Serial1.begin(115200, SERIAL_8N1, 16, 17); // for ch9350
  
  // Serial.begin(115200);
  //  Serial.begin(38400);
  //  SerialBT.setRxBufferSize(512);  
  SerialBT.begin(device_name);  //Bluetooth device name
  //SerialBT.deleteAllBondedDevices(); // Uncomment this to delete paired devices; Must be called after begin
  Serial.printf("The device with name \"%s\" is started.\nNow you can pair it with Bluetooth!\n", device_name.c_str());
  count=0;count1=0;


  Serial2.setRxBufferSize(512);
  // serial.begin(rx,tx)
  // serial3_tx IO15  seirla3_rx IO27
  Serial2.begin(cat2_baud, SERIAL_8N1, 27,15,cat2_reversed);
  // CAT2  // need to have ways to reconfigure the setting of CAT2 by packet command to controller
  //    Serial1.begin(baud,SERIAL_8N1,
  //		  rxPin,
  //		  txPin,
  //		  radio->rig_spec->civport_reversed);

  Serial3.begin(cat3_baud,SWSERIAL_8N1,CATRX,CATTX,cat3_reversed,95,0);
  Serial3.enableIntTx(0); // disable interrupt diuring transmitting signal
  
    
  mux_transport=Mux_transport();
  mux_transport.mux_stream=&Serial;
  mux_transport.set_port_handler(MUX_PORT_EXT_BRD_CTRL,control_pkt_handler);
  mux_transport.set_port_handler(MUX_PORT_CAT2_EXT,cat2_pkt_handler);
  mux_transport.set_port_handler(MUX_PORT_CAT3_EXT,cat3_pkt_handler);  
  mux_transport.set_port_handler(MUX_PORT_BT_SERIAL_EXT,serialbt_pkt_handler);
		
  init_keyboard()  ;

  serial_reader.attach_ms(1,read_serial_ports); // scan and read serial ports and store in ring buffers
}

int timeout_play=0;
void loop() {
  char c;

  loop_keyboard();
  /*    while (Serial1.available()) {
      c=Serial1.read();
      Serial.println(c,HEX);
    }
  */
  /*
  char buf[100];
  if (timeout_play < millis()) {
    if (!player.isPlaying()) {
      player.play_string("jk1edc ");
      ESP_LOGI("MAIN", "pushed FIFO jk1edc ");      
    } else {
      if (player.get_string_fifo(buf,100)) {
	ESP_LOGI("MAIN", "FIFO empty, playback finished %s",buf);
      } 
    }
    timeout_play=millis()+100;
  }
  */
  // cat3
  while (Serial3.available() ) {
    c=Serial3.read();
    buf3[count3++]=c;
    // Serial.print("c:");
    //Serial.println(count);

    if ((c==0x0a)||(c==0xfd)||(c==';')) {
      if (f_mux_transport) {
	// f_mux_transport to send data from SerialBT to main board 
	mux_transport.send_pkt(MUX_PORT_CAT3_EXT,MUX_PORT_CAT3_MAIN,buf3,count3);
      } else {
	Serial.write(buf3,count3);	
      }
      count3=0;      
    }
    if (count3>100) { // originally 2000 shortened buffer length
      if (f_mux_transport) {
	mux_transport.send_pkt(MUX_PORT_CAT3_EXT,MUX_PORT_CAT3_MAIN,buf3,count3);
      } else {
	Serial3.write(buf3,count3);
      }
      count3=0;
    }
  }
  

  if (f_mux_transport) {
    mux_transport.recv_pkt();
  } else {
    while (Serial.available()) {
      c=Serial.read();
      buf1[count1++]=c;
      //      Serial.print("c1:");
      //      Serial.println(c);
      //      Serial.print(c); // loop back test
      if ((c==0x0a)||(c==0xfd)) {
	// check command
	buf1[count1]='\0';
	//	Serial.print("received cmd:");
	//	Serial.println((char *)buf1);
	if (strcmp((char *)buf1,"go_mux\r\n")==0) {
	  // go to mux_transport
	  Serial.println("go_mux accepted");
	  f_mux_transport=1;
	} else {
	  SerialBT.write(buf1,count1);
	}
	count1=0;
      }
      if (count1>=2000) {
	SerialBT.write(buf1,count1);
	count1=0;
      }
    }
  }
  delay(1);
  //  Serial.print("*");
}

extern "C" void app_main_test1(void);
extern "C" void app_main_test(void) ;


extern "C" void app_main(void)
{
  esp_log_level_set("*", ESP_LOG_NONE);
  initArduino();
  Serial.setRxBufferSize(512);
  //  Serial.begin(115200);
  //  Serial.begin(750000);
  Serial.begin(3000000);    
  while (!Serial) ;

  setup();
  //  app_main_test1() ;
  //  app_main_test();
  init_AudioPlayer();

  //dac_playback_test();
  while (1) {
    loop();
  }
    
}
