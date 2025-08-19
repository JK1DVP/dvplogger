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
// Copyright (c) 2021-2024 Eiichiro Araki
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Arduino.h"
#include "decl.h"
#include "variables.h"
#include "usb_host.h"

#include <cdcftdi.h>  // serial adapter
#include <hidboot.h>
#include <usbhub.h>
#include <BTHID.h>
#include "keyboard.h"
#include "ui.h"
#include "cat.h"
#include "cw_keying.h"
#include "so2r.h"
#include "mux_transport.h"
#include "pgmstrings_usbhost.h"

#ifdef notdef
#include "cdc_ch34x.h"
#endif

USB Usb;
USBHub Hub(&Usb);  // 使用するハブの数だけ定義しておく
USBHub          Hub2(&Usb);

void print_hex(int v, int num_places);
void printintfdescr( uint8_t* descr_ptr );
void printconfdescr( uint8_t* descr_ptr );
void printepdescr( uint8_t* descr_ptr );
void printunkdescr( uint8_t* descr_ptr );
uint8_t getconfdescr( uint8_t addr, uint8_t conf );
void printProgStr(const char* str);

void PrintAllAddresses(UsbDevice *pdev)
{
  UsbDeviceAddress adr;
  adr.devAddress = pdev->address.devAddress;
  Serial.print("\r\nAddr:");
  Serial.print(adr.devAddress, HEX);
  Serial.print("(");
  Serial.print(adr.bmHub, HEX);
  Serial.print(".");
  Serial.print(adr.bmParent, HEX);
  Serial.print(".");
  Serial.print(adr.bmAddress, HEX);
  Serial.println(")");
}

void PrintAddress(uint8_t addr)
{
  UsbDeviceAddress adr;
  adr.devAddress = addr;
  Serial.print("\r\nADDR:\t");
  Serial.println(adr.devAddress, HEX);
  Serial.print("DEV:\t");
  Serial.println(adr.bmAddress, HEX);
  Serial.print("PRNT:\t");
  Serial.println(adr.bmParent, HEX);
  Serial.print("HUB:\t");
  Serial.println(adr.bmHub, HEX);
}
uint8_t getdevdescr( uint8_t addr, uint8_t &num_conf );


void PrintDescriptors(uint8_t addr)
{
  uint8_t rcode = 0;
  uint8_t num_conf = 0;

  rcode = getdevdescr( (uint8_t)addr, num_conf );
  if ( rcode )
  {
    printProgStr(Gen_Error_str);
    print_hex( rcode, 8 );
  }
  Serial.print("\r\n");

  for (int i = 0; i < num_conf; i++)
  {
    rcode = getconfdescr( addr, i );                 // get configuration descriptor
    if ( rcode )
    {
      printProgStr(Gen_Error_str);
      print_hex(rcode, 8);
    }
    Serial.println("\r\n");
  }
}

void PrintAllDescriptors(UsbDevice *pdev)
{
  Serial.println("\r\n");
  print_hex(pdev->address.devAddress, 8);
  Serial.println("\r\n--");
  PrintDescriptors( pdev->address.devAddress );
}

uint8_t getdevdescr( uint8_t addr, uint8_t &num_conf )
{
  USB_DEVICE_DESCRIPTOR buf;
  uint8_t rcode;
  rcode = Usb.getDevDescr( addr, 0, 0x12, ( uint8_t *)&buf );
  if ( rcode ) {
    return ( rcode );
  }
  printProgStr(Dev_Header_str);
  printProgStr(Dev_Length_str);
  print_hex( buf.bLength, 8 );
  printProgStr(Dev_Type_str);
  print_hex( buf.bDescriptorType, 8 );
  printProgStr(Dev_Version_str);
  print_hex( buf.bcdUSB, 16 );
  printProgStr(Dev_Class_str);
  print_hex( buf.bDeviceClass, 8 );
  printProgStr(Dev_Subclass_str);
  print_hex( buf.bDeviceSubClass, 8 );
  printProgStr(Dev_Protocol_str);
  print_hex( buf.bDeviceProtocol, 8 );
  printProgStr(Dev_Pktsize_str);
  print_hex( buf.bMaxPacketSize0, 8 );
  printProgStr(Dev_Vendor_str);
  print_hex( buf.idVendor, 16 );
  printProgStr(Dev_Product_str);
  print_hex( buf.idProduct, 16 );
  printProgStr(Dev_Revision_str);
  print_hex( buf.bcdDevice, 16 );
  printProgStr(Dev_Mfg_str);
  print_hex( buf.iManufacturer, 8 );
  printProgStr(Dev_Prod_str);
  print_hex( buf.iProduct, 8 );
  printProgStr(Dev_Serial_str);
  print_hex( buf.iSerialNumber, 8 );
  printProgStr(Dev_Nconf_str);
  print_hex( buf.bNumConfigurations, 8 );
  num_conf = buf.bNumConfigurations;
  return ( 0 );
}

void printhubdescr(uint8_t *descrptr, uint8_t addr)
{
  HubDescriptor  *pHub = (HubDescriptor*) descrptr;
  uint8_t        len = *((uint8_t*)descrptr);

  printProgStr(PSTR("\r\n\r\nHub Descriptor:\r\n"));
  printProgStr(PSTR("bDescLength:\t\t"));
  Serial.println(pHub->bDescLength, HEX);

  printProgStr(PSTR("bDescriptorType:\t"));
  Serial.println(pHub->bDescriptorType, HEX);

  printProgStr(PSTR("bNbrPorts:\t\t"));
  Serial.println(pHub->bNbrPorts, HEX);

  printProgStr(PSTR("LogPwrSwitchMode:\t"));
  Serial.println(pHub->LogPwrSwitchMode, BIN);

  printProgStr(PSTR("CompoundDevice:\t\t"));
  Serial.println(pHub->CompoundDevice, BIN);

  printProgStr(PSTR("OverCurrentProtectMode:\t"));
  Serial.println(pHub->OverCurrentProtectMode, BIN);

  printProgStr(PSTR("TTThinkTime:\t\t"));
  Serial.println(pHub->TTThinkTime, BIN);

  printProgStr(PSTR("PortIndicatorsSupported:"));
  Serial.println(pHub->PortIndicatorsSupported, BIN);

  printProgStr(PSTR("Reserved:\t\t"));
  Serial.println(pHub->Reserved, HEX);

  printProgStr(PSTR("bPwrOn2PwrGood:\t\t"));
  Serial.println(pHub->bPwrOn2PwrGood, HEX);

  printProgStr(PSTR("bHubContrCurrent:\t"));
  Serial.println(pHub->bHubContrCurrent, HEX);

  for (uint8_t i = 7; i < len; i++)
    print_hex(descrptr[i], 8);

  //for (uint8_t i=1; i<=pHub->bNbrPorts; i++)
  //    PrintHubPortStatus(&Usb, addr, i, 1);
}

uint8_t getconfdescr( uint8_t addr, uint8_t conf )
{
  uint8_t buf[ BUFSIZE ];
  uint8_t* buf_ptr = buf;
  uint8_t rcode;
  uint8_t descr_length;
  uint8_t descr_type;
  uint16_t total_length;
  rcode = Usb.getConfDescr( addr, 0, 4, conf, buf );  //get total length
  LOBYTE( total_length ) = buf[ 2 ];
  HIBYTE( total_length ) = buf[ 3 ];
  if ( total_length > 256 ) {   //check if total length is larger than buffer
    printProgStr(Conf_Trunc_str);
    total_length = 256;
  }
  rcode = Usb.getConfDescr( addr, 0, total_length, conf, buf ); //get the whole descriptor
  while ( buf_ptr < buf + total_length ) { //parsing descriptors
    descr_length = *( buf_ptr );
    descr_type = *( buf_ptr + 1 );
    switch ( descr_type ) {
      case ( USB_DESCRIPTOR_CONFIGURATION ):
        printconfdescr( buf_ptr );
        break;
      case ( USB_DESCRIPTOR_INTERFACE ):
        printintfdescr( buf_ptr );
        break;
      case ( USB_DESCRIPTOR_ENDPOINT ):
        printepdescr( buf_ptr );
        break;
      case 0x29:
        printhubdescr( buf_ptr, addr );
        break;
      default:
        printunkdescr( buf_ptr );
        break;
    }//switch( descr_type
    buf_ptr = ( buf_ptr + descr_length );    //advance buffer pointer
  }//while( buf_ptr <=...
  return ( rcode );
}
// copyright, Peter H Anderson, Baltimore, MD, Nov, '07
// source: http://www.phanderson.com/arduino/arduino_display.html
void print_hex(int v, int num_places)
{
  int mask = 0, n, num_nibbles, digit;

  for (n = 1; n <= num_places; n++) {
    mask = (mask << 1) | 0x0001;
  }
  v = v & mask; // truncate v to specified number of places

  num_nibbles = num_places / 4;
  if ((num_places % 4) != 0) {
    ++num_nibbles;
  }
  do {
    digit = ((v >> (num_nibbles - 1) * 4)) & 0x0f;
    Serial.print(digit, HEX);
  }
  while (--num_nibbles);
}
void printconfdescr( uint8_t* descr_ptr )
{
  USB_CONFIGURATION_DESCRIPTOR* conf_ptr = ( USB_CONFIGURATION_DESCRIPTOR* )descr_ptr;
  printProgStr(Conf_Header_str);
  printProgStr(Conf_Totlen_str);
  print_hex( conf_ptr->wTotalLength, 16 );
  printProgStr(Conf_Nint_str);
  print_hex( conf_ptr->bNumInterfaces, 8 );
  printProgStr(Conf_Value_str);
  print_hex( conf_ptr->bConfigurationValue, 8 );
  printProgStr(Conf_String_str);
  print_hex( conf_ptr->iConfiguration, 8 );
  printProgStr(Conf_Attr_str);
  print_hex( conf_ptr->bmAttributes, 8 );
  printProgStr(Conf_Pwr_str);
  print_hex( conf_ptr->bMaxPower, 8 );
  return;
}
void printintfdescr( uint8_t* descr_ptr )
{
  USB_INTERFACE_DESCRIPTOR* intf_ptr = ( USB_INTERFACE_DESCRIPTOR* )descr_ptr;
  printProgStr(Int_Header_str);
  printProgStr(Int_Number_str);
  print_hex( intf_ptr->bInterfaceNumber, 8 );
  printProgStr(Int_Alt_str);
  print_hex( intf_ptr->bAlternateSetting, 8 );
  printProgStr(Int_Endpoints_str);
  print_hex( intf_ptr->bNumEndpoints, 8 );
  printProgStr(Int_Class_str);
  print_hex( intf_ptr->bInterfaceClass, 8 );
  printProgStr(Int_Subclass_str);
  print_hex( intf_ptr->bInterfaceSubClass, 8 );
  printProgStr(Int_Protocol_str);
  print_hex( intf_ptr->bInterfaceProtocol, 8 );
  printProgStr(Int_String_str);
  print_hex( intf_ptr->iInterface, 8 );
  return;
}
void printepdescr( uint8_t* descr_ptr )
{
  USB_ENDPOINT_DESCRIPTOR* ep_ptr = ( USB_ENDPOINT_DESCRIPTOR* )descr_ptr;
  printProgStr(End_Header_str);
  printProgStr(End_Address_str);
  print_hex( ep_ptr->bEndpointAddress, 8 );
  printProgStr(End_Attr_str);
  print_hex( ep_ptr->bmAttributes, 8 );
  printProgStr(End_Pktsize_str);
  print_hex( ep_ptr->wMaxPacketSize, 16 );
  printProgStr(End_Interval_str);
  print_hex( ep_ptr->bInterval, 8 );

  return;
}
void printunkdescr( uint8_t* descr_ptr )
{
  uint8_t length = *descr_ptr;
  uint8_t i;
  printProgStr(Unk_Header_str);
  printProgStr(Unk_Length_str);
  print_hex( *descr_ptr, 8 );
  printProgStr(Unk_Type_str);
  print_hex( *(descr_ptr + 1 ), 8 );
  printProgStr(Unk_Contents_str);
  descr_ptr += 2;
  for ( i = 0; i < length; i++ ) {
    print_hex( *descr_ptr, 8 );
    descr_ptr++;
  }
}


void printProgStr(const char* str)
{
  char c;
  if (!str) return;
  while ((c = pgm_read_byte(str++)))
    Serial.print(c);
}
// these are from USB_desc.ino

void USB_desc()
{
    if ( Usb.getUsbTaskState() == USB_STATE_RUNNING )  {
      Usb.ForEachUsbDevice(&PrintAllDescriptors);
      Usb.ForEachUsbDevice(&PrintAllAddresses);
    }

}


// qmx idVendor=0483, idProduct=a34c

#ifdef notdef
class FTDIAsync : public FTDIAsyncOper {
  public:
    uint8_t OnInit(FTDI *pftdi);
};

uint8_t FTDIAsync::OnInit(FTDI *pftdi) {
  uint8_t rcode = 0;

  rcode = pftdi->SetBaudRate(38400);  // Yaesu CAT baudrate
  //rcode = pftdi->SetBaudRate(115200);  // RN42 default  --> change to 38400

  if (rcode) {
    ErrorMessage<uint8_t>(PSTR("SetBaudRate"), rcode);
    return rcode;
  }
  rcode = pftdi->SetFlowControl(FTDI_SIO_DISABLE_FLOW_CTRL);

  if (rcode)
    ErrorMessage<uint8_t>(PSTR("SetFlowControl"), rcode);

  return rcode;
}
#endif

//using MyMax = 
//USB<MyMax> Usb;


/// CH340 arduino nano clone SO2R mini
// baudrate 9600
// capital character send
// tx switch send command twice
// switch pattern
// TX1 RX1 0x90 0x90    0000
// TX1 RX2 0x91         0001
// TX1 Stereo 0x92      0010
// TX2 RX1 0x94 0x94    1000
// TX2 RX2 0x95         0101
// TX2 Stereo 0x96 0x96 0110
//
//0x80 so2r close
//0x81 so2r open
//0x82 ptt off
//0x83 ptt on
//0x84 latch off
//0x85 latch on
// winkey command
// 0x02 wpm set wpm
// 0x04 lead tail 10ms set ptt lead/tail
// 0x0a clear buffer
// 0x0b 0/1  key immediate
// 0x0e set winkey mode
// bit 7 disable paddle watchdog
//     6  paddle echoback enable
//    5,4 key mode 00 imabic b 01 iambic a 10 ultimatic 11 bug
//     3 paddle swap
//     2 serial echoback enable
//     1 autospace enable
//     0 ct spacing
// 0x03 weight% weight set key weight 50
// 0x0f load default
// 0x10 ms set 1st extension  (first dit extension for slow TX/RX switching)
// 0x11 msec set key compensation for QSK
// 0x15 winkey status request
// 0x16 buffer pointer commands ???
//0x17 0x50 dit/dah ratio (1:3)
// 0x18 1/0  ptt control
// 1c wpm speed change in buffer
// 0x1f buffered NOP

#ifdef notdef
class CH34XAsyncOper : public CDCAsyncOper {
  public:
    uint8_t OnInit(CH34X *pch34x);
};

uint8_t CH34XAsyncOper::OnInit(CH34X *pch34x) {
  uint8_t rcode;

  LINE_CODING lc;
  lc.dwDTERate = 9600;
  lc.bCharFormat = 0;
  lc.bParityType = 0;
  lc.bDataBits = 8;
  lc.bFlowControl = 0;

  rcode = pch34x->SetLineCoding(&lc);

  if (rcode)
    ErrorMessage<uint8_t>(PSTR("SetLineCoding"), rcode);

  return rcode;
}
#endif


//#ifdef notdef
// IC-705 USB Acm port
#include <cdcacm.h>

class ACMAsyncOper : public CDCAsyncOper {
  public:
    uint8_t OnInit(ACM *pacm);
};

uint8_t ACMAsyncOper::OnInit(ACM *pacm) {
  uint8_t rcode;
  // bit1 RTS=1 bit0 DTR = 1
  rcode = pacm->SetControlLineState(2);
  
  if (rcode) {
    ErrorMessage<uint8_t>(PSTR("SetControlLineState"), rcode);
    return rcode;
  }

  LINE_CODING lc;
  lc.dwDTERate = 115200;
  lc.bCharFormat = 0;
  lc.bParityType = 0;
  lc.bDataBits = 8;

  rcode = pacm->SetLineCoding(&lc);

  if (rcode)
    ErrorMessage<uint8_t>(PSTR("SetLineCoding"), rcode);

  return rcode;
}

ACMAsyncOper AsyncOper;
ACM Acm(&Usb, &AsyncOper);



void ACMprocess() {
  // IC-705,IC-905 operation
  uint8_t rcode;
  int ret;
  struct catmsg_t catmsg;
  
  if (Acm.isReady()) {
    // check queue and forward to USB
    while (uxQueueMessagesWaiting(xQueueCATUSBTx)){
      ret = xQueueReceive(xQueueCATUSBTx, &catmsg, 0);
      if (ret==pdTRUE) {
	if (verbose &1)
	  console->printf("xQueueReceive() : CATUSBTx ret = %d size =%d\n", ret,catmsg.size);

	rcode = Acm.SndData(catmsg.size, (uint8_t *)&catmsg.buf);
	if (rcode) {
	  ErrorMessage<uint8_t>(PSTR("SndData CATUSBTx"), rcode);
	  plogw->ostream->println("SndData CATUSBTx error");
	}
      }
    }
	
	
    if (1==0) {
      // Acm.SndData は送った後にポインタの中身をクリアするので
      // 別途バッファを用意する。
      char usbSndBuff[40];
      strcpy(usbSndBuff, "");

      //rcode = Acm.SndData(39, usbSndBuff);
      // 一度にまとめて送っても反映されないので1文字ずつ送る。
      // サンプルプログラムと同じ。
      //        plogw->ostream->print("wxSend: ");
      for (uint8_t i = 0; i < strlen(usbSndBuff); i++) {
	//             plogw->ostream->print(wxStr[i], HEX);
	//             plogw->ostream->print(' ');
	//             delay(10);
	rcode = Acm.SndData(1, (uint8_t *)&usbSndBuff[i]);
	if (rcode) {
	  ErrorMessage<uint8_t>(PSTR("SndData"), rcode);
	  plogw->ostream->println("SndData error");
	}
      }  //for
    }
    //        plogw->ostream->println();

    /* reading from usb device */
    /* buffer size must be greater or equal to max.packet size */
    /* it it set to 64 (largest possible max.packet size) here, can be tuned down
       for particular endpoint */
    // 受け取り側はサンプルプログラムのまま。

    uint8_t buf[64];
    uint16_t rcvd = 64;
    int ret;
    rcode = Acm.RcvData(&rcvd, buf);
    if (rcode && rcode != hrNAK)   ErrorMessage<uint8_t>(PSTR("Ret"), rcode);

    struct catmsg_t catmsg;
    if (rcvd) {  //more than zero bytes received
      if (verbose &1) {
	plogw->ostream->print("ACMrcvd:"); // IC-705, 905 CI-V is from here
	for (uint16_t i = 0; i < rcvd; i++) {
	  plogw->ostream->printf("%02x ",(char)buf[i]);  //printing on the screen
	}
	plogw->ostream->print("\r\n");
      }
      // send catmsg
      catmsg.size=rcvd;
      memcpy(catmsg.buf,buf,rcvd);
      ret = xQueueSend(xQueueCATUSBRx, &catmsg, 0);
      if (verbose &1) plogw->ostream->printf("CATUSBRx queuesend ret=%d size=%d\r\n",ret,catmsg.size);
    }

    rcvd=64;
    rcode = Acm.RcvData1(&rcvd, buf);
    if (rcode && rcode != hrNAK)   ErrorMessage<uint8_t>(PSTR("Ret1"), rcode);

    if (rcvd) {  //more than zero bytes received
      plogw->ostream->print("ACMrcvd1:");
      for (uint16_t i = 0; i < rcvd; i++) {
        plogw->ostream->print((char)buf[i]);  //printing on the screen
      }
      plogw->ostream->print("\r\n");      
    }
  }
}

//#endif

#ifdef notdef
CH34XAsyncOper CH34XAsyncOper;
CH34X Ch34x(&Usb, &CH34XAsyncOper);
#endif
BTD Btd(&Usb);  // You have to create the Bluetooth Dongle instance like so
//BTHID bthid(&Btd);
BTHID bthid(&Btd, PAIR, "0000");
#ifdef notdef
FTDIAsync FtdiAsync;
FTDI Ftdi(&Usb, &FtdiAsync);
#endif

void receive_pkt_handler_keyboard1_main(struct mux_packet *packet)
{
  //  Serial.print("kbd1:idx=");
  //  Serial.println(packet->idx);
  if (packet->idx>=2) {
    Prs1.Parse_extKbd(packet->buf[0],packet->buf[1]);
  }
}

void KbdRptParser::init_extKbd()
{
    for (uint8_t i = 1; i < 8; i++) {
      prevState.bInfo[i]=1;
    }
    prevState.bInfo[0]=0;
}

  
void KbdRptParser::Parse_extKbd(uint8_t hid_code,bool on) 
{
  // receive hid_code and on to process OnControlKeysChanged(prevState, curState)
  // OnKeyDown(), OnKeyUp() updating prevState.bInfo[i] ...

  // check control keys to modify control keys state
  buf_ext[0]=prevState.bInfo[0x00];
  uint8_t bmask;
  bmask=0;
  switch (hid_code) {
  case 0xe0: // L_Ctrl
    bmask=0x1;    break;
  case 0xe1: // L_Shift
    bmask=0x2;    break;    
  case 0xe2: // L_Alt
    bmask=0x4;    break;    
  case 0xe3: // L_Gui
    bmask=0x8;    break;    
  case 0xe4: // R_Ctrl
    bmask=0x10;    break;        
  case 0xe5: // R_Shift
    bmask=0x20;    break;        
  case 0xe6: // R_Alt
    bmask=0x40;    break;        
  case 0xe7: // R_Gui
    bmask=0x80;    break;
  }
  if (bmask!=0) {
    if (on) {
      buf_ext[0]|=bmask;
    } else {
      buf_ext[0]&=~bmask;
    }
  }

  if (prevState.bInfo[0x00] != buf_ext[0x00]) {
    //                OnControlKeysChanged(prevState.bInfo[0x00], buf[0x00]);
    // send to keymsg
    msg.arg1=prevState.bInfo[0x00];
    msg.arg2=buf_ext[0x00];
    msg.type=KEYMSG_TYPE_ONCONTROLKEYSCHANGED;
    send_keyrpt_queue();
  }

  bool found=false;  
  for (uint8_t i = 2; i < 8; i++) {
    //      Serial.print("prevState0:");
    //      Serial.print(prevState.bInfo[i]);
    //      Serial.println(":");
    
    // search hid_code in prevState.binfo[]
    if (prevState.bInfo[i] == hid_code) {
      // found in previous key scan codes
      found=true;
      if (on) {
	// keep pressed state
      } else {
	prevState.bInfo[i]=1; // delete entry	
	
	msg.arg1=buf_ext[0];
	msg.arg2=hid_code;
	msg.type=KEYMSG_TYPE_ONKEYUP;
	send_keyrpt_queue();
      }
      break;
    }
  }
  if (!found) {
    // add to key list
    found=false;
    for (uint8_t i = 2; i < 8; i++) {
      //      Serial.print("prevState:");
      //      Serial.print(prevState.bInfo[i]);
      //      Serial.println(":");
      if (prevState.bInfo[i]==1) {
	found=true;
	// empty entry found
	if (on) {
	  // update directly prevState 
	  prevState.bInfo[i]=hid_code;
	  //	  Serial.print("prevState2:");
	  //	  Serial.print(i);
	  //	  Serial.print(":");
	  //	  Serial.println(prevState.bInfo[i]);
	  
	  // handle locking keys 
	  msg.hid=(USBHID *)buf_ext[0];
	  msg.arg2=hid_code;
	  msg.type=KEYMSG_TYPE_HANDLELOCKINGKEYS;
	  send_keyrpt_queue();

	  // onkeydown
	  msg.arg1=buf_ext[0];
	  msg.arg2=hid_code;
	  msg.type=KEYMSG_TYPE_ONKEYDOWN;
	  send_keyrpt_queue();

	}
	break;
      }
    }
  }
  
  prevState.bInfo[0]=buf_ext[0];

// other keys  ... 
//  case 0x53: // NumLock
//  case 0x39: // CapsLock
//  case 0x47: // ScrLock
  
}

void KbdRptParser::Parse(USBHID *hid, bool is_rpt_id __attribute__((unused)), uint8_t len __attribute__((unused)), uint8_t *buf) {
        // On error - return
        if (buf[2] == 1)
                return;

        //KBDINFO       *pki = (KBDINFO*)buf;

        // provide event for changed control key state
        if (prevState.bInfo[0x00] != buf[0x00]) {
	  //                OnControlKeysChanged(prevState.bInfo[0x00], buf[0x00]);
	  // send to keymsg
	  msg.arg1=prevState.bInfo[0x00];
	  msg.arg2=buf[0x00];
	  msg.type=KEYMSG_TYPE_ONCONTROLKEYSCHANGED;
	  send_keyrpt_queue();
        }

	//	Serial.print("HID:");	
	//        for (uint8_t i = 2; i < 8; i++) {
	//	  Serial.print(buf[i],HEX);
	//	}
	//	Serial.println("");	
        for (uint8_t i = 2; i < 8; i++) {
                bool down = false;
                bool up = false;
		
                for (uint8_t j = 2; j < 8; j++) {
                        if (buf[i] == prevState.bInfo[j] && buf[i] != 1)
                                down = true;
                        if (buf[j] == prevState.bInfo[i] && prevState.bInfo[i] != 1)
                                up = true;
                }
                if (!down) {
		  //                        HandleLockingKeys(hid, buf[i]);
		  msg.hid=hid;      // hid is USBHID pointer, why?
		  msg.arg2=buf[i];
		  msg.type=KEYMSG_TYPE_HANDLELOCKINGKEYS;
		  send_keyrpt_queue();
			
		  //		Serial.print("down i=");Serial.print(i);Serial.print("buf=");
		  //			Serial.println(buf[i],HEX);
		  //                        OnKeyDown(*buf, buf[i]);
		  msg.arg1=*buf; // OnKeyDown(mod, key) 
		  msg.arg2=buf[i];
		  msg.type=KEYMSG_TYPE_ONKEYDOWN;
		  send_keyrpt_queue();
		  
                }
                if (!up) {
		  //                        OnKeyUp(prevState.bInfo[0], prevState.bInfo[i]);
		  msg.arg1=prevState.bInfo[0];
		  msg.arg2=prevState.bInfo[i];
		  msg.type=KEYMSG_TYPE_ONKEYUP;
		  send_keyrpt_queue();
		}
		
        }
        for (uint8_t i = 0; i < 8; i++)
                prevState.bInfo[i] = buf[i];
};



//const uint8_t KbdRptParser::numKeys[10] PROGMEM = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
uint8_t KbdRptParser::symKeysUp_us[12] PROGMEM = {'_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?'};
uint8_t KbdRptParser::symKeysLo_us[12] PROGMEM = {'-', '=', '[', ']', '\\', ' ', ';', '\'', '`', ',', '.', '/'};

// jp106 use the following
uint8_t KbdRptParser::symKeysUp_jp[12] PROGMEM = {'=', '~', '`', '{', '|', '}', '+', '*', ' ', '<', '>', '?'};
uint8_t KbdRptParser::symKeysLo_jp[12] PROGMEM = {'-', '^', '@', '[', '\\', ']', ';', ':', ' ', ',', '.', '/'};





//const uint8_t KbdRptParser::padKeys[5] PROGMEM = {'/', '*', '-', '+', '\r'};


void KbdRptParser::init_keyrpt_queue() {
    xQueueKeyRpt = xQueueCreate(QUEUE_KEYRPT_LEN, sizeof(struct keymsg_t));
}

void KbdRptParser::send_keyrpt_queue(){
  BaseType_t ret;
  ret = xQueueSend(xQueueKeyRpt, &msg, 0);
  //  Serial.printf("xQueueSend(%d) : ret = %d\n", data, ret);
}

void KbdRptParser::process_keyrpt_queue() {
  BaseType_t ret;  
  //  Serial.println("\n受信");
  //  data2 = -1;
  //  MODIFIERKEYS modkey;
      
  struct keymsg_t msg;
  //  Serial.println("\nキューの数確認");
  //  Serial.printf("uxQueueMessagesWaiting = %d\n", uxQueueMessagesWaiting(xQueue));
  while (uxQueueMessagesWaiting(xQueueKeyRpt)){
    ret = xQueueReceive(xQueueKeyRpt, &msg, 0);
    if (ret==pdTRUE) {
      //  Serial.printf("%d = xQueueReceive() : ret = %d\n", data2, ret);
      switch (msg.type) {
      case KEYMSG_TYPE_ONCONTROLKEYSCHANGED:  // OnControlKeysChanged
	OnControlKeysChanged(msg.arg1,msg.arg2);
	break;
      case KEYMSG_TYPE_HANDLELOCKINGKEYS: // HandleLockingKeys()
	HandleLockingKeys(msg.hid,msg.arg2);
	break;
      case KEYMSG_TYPE_ONKEYDOWN: //OnKeyDown()
	OnKeyDown(msg.arg1,msg.arg2);            
	break;
      case KEYMSG_TYPE_ONKEYUP: // OnKeyUp()
	OnKeyUp(msg.arg1,msg.arg2);                  
	//      *((uint8_t *)&modkey) = msg.mod;
	break;
      }
    }
  }
}




uint8_t KbdRptParser::OemToAscii(uint8_t mod, uint8_t key) {
        uint8_t shift = (mod & 0x22);

        // [a-z]
        if (VALUE_WITHIN(key, 0x04, 0x1d)) {
                // Upper case letters
                if ((kbdLockingKeys.kbdLeds.bmCapsLock == 0 && shift) ||
                        (kbdLockingKeys.kbdLeds.bmCapsLock == 1 && shift == 0))
                        return (key - 4 + 'A');

                        // Lower case letters
                else
                        return (key - 4 + 'a');
        }// Numbers
        else if (VALUE_WITHIN(key, 0x1e, 0x27)) {
                if (shift)
                        return ((uint8_t)pgm_read_byte(&getNumKeys()[key - 0x1e]));
                else
                        return ((key == UHS_HID_BOOT_KEY_ZERO) ? '0' : key - 0x1e + '1');
        }// Keypad Numbers
        else if(VALUE_WITHIN(key, 0x59, 0x61)) {
	  if(kbdLockingKeys.kbdLeds.bmNumLock == 1)    return (key - 0x59 + '1');
        } else if(VALUE_WITHIN(key, 0x2d, 0x38)) {

	  return ((shift) ? (uint8_t)pgm_read_byte(&getSymKeysUp()[key - 0x2d]) : (uint8_t)pgm_read_byte(&getSymKeysLo()[key - 0x2d]));
	    
	}
        else if(VALUE_WITHIN(key, 0x54, 0x58)) {
                return (uint8_t)pgm_read_byte(&getPadKeys()[key - 0x54]);
	}
        else {
                switch(key) {
                        case UHS_HID_BOOT_KEY_SPACE: return (0x20);
                        case UHS_HID_BOOT_KEY_ENTER: return ('\r'); // Carriage return (0x0D)
                        case UHS_HID_BOOT_KEY_ZERO2: return ((kbdLockingKeys.kbdLeds.bmNumLock == 1) ? '0': 0);
                        case UHS_HID_BOOT_KEY_PERIOD: return ((kbdLockingKeys.kbdLeds.bmNumLock == 1) ? '.': 0);
                }
        }
        return ( 0);
}



uint8_t KbdRptParser::HandleLockingKeys(USBHID* hid, uint8_t key) {
  uint8_t old_keys = kbdLockingKeys.bLeds;

  switch(key) {
  case UHS_HID_BOOT_KEY_NUM_LOCK:
    kbdLockingKeys.kbdLeds.bmNumLock = ~kbdLockingKeys.kbdLeds.bmNumLock;
    break;
  case UHS_HID_BOOT_KEY_CAPS_LOCK:
    // no caps lock function
    //                                kbdLockingKeys.kbdLeds.bmCapsLock = ~kbdLockingKeys.kbdLeds.bmCapsLock;
    break;
  case UHS_HID_BOOT_KEY_SCROLL_LOCK:
    kbdLockingKeys.kbdLeds.bmScrollLock = ~kbdLockingKeys.kbdLeds.bmScrollLock;
    break;
  }
  
  if(old_keys != kbdLockingKeys.bLeds && hid) {
    uint8_t lockLeds = kbdLockingKeys.bLeds;
    return (hid->SetReport(0, 0/*hid->GetIface()*/, 2, 0, 1, &lockLeds));
  }
  
  return 0;
};


void KbdRptParser::OnControlKeysChanged(uint8_t before, uint8_t after) {

  MODIFIERKEYS beforeMod;
  *((uint8_t *)&beforeMod) = before;
  MODIFIERKEYS afterMod;
  *((uint8_t *)&afterMod) = after;

  // check Right Shift if straight key mode
  struct radio *radio;

  if (verbose&4) {
    console->println("OnControlKeysChanged()");
  }
  radio = &radio_list[so2r.focused_radio()];
  if (plogw->f_straightkey) {
    if (beforeMod.bmRightShift == 0 && afterMod.bmRightShift == 1) {
      if (so2r.tx() != so2r.focused_radio()) {
	keying(0);
	so2r.set_tx(so2r.focused_radio());
      }
      switch (radio->modetype) {
        case LOG_MODETYPE_CW:
	  keying(1);
          if (verbose) plogw->ostream->print("keyon ");
          break;
      case LOG_MODETYPE_PH:  // not sure what this is trying to do on the phone
          radio->ptt = 1;
	  set_ptt_rig(radio, radio->ptt);
          break;
      }
    } else {
      if (beforeMod.bmRightShift == 1 && afterMod.bmRightShift == 0) {
        if (so2r.tx() != so2r.focused_radio()) {
	  keying(0);
	  // go back to tx target to focused ratio
	  so2r.set_tx(so2r.focused_radio());
        }
        switch (radio->modetype) {
          case LOG_MODETYPE_CW:
	    keying(0);
            if (verbose) plogw->ostream->print("keyoff ");
            break;
          case LOG_MODETYPE_PH: // not sure what this is trying to do on the phone
            radio->ptt = 0;
	    set_ptt_rig(radio, radio->ptt);
            break;
        }
      }
    }
  } else {
    if (plogw->f_toggle_ptt_mode) {
      // toggle ptt of the currently focused radio
      if (beforeMod.bmRightShift == 0 && afterMod.bmRightShift == 1) {
	// pressed rightshift
	console->println("rightshift pressed");	
	if (so2r.tx() != so2r.focused_radio()) {
	  keying(0);
	  so2r.set_tx(so2r.focused_radio());
	}
	switch (radio->modetype) {
        case LOG_MODETYPE_CW:
	  // do nothing
          break;
	case LOG_MODETYPE_PH:  
          radio->ptt = 1- radio->ptt; // toggle
	  set_ptt_rig(radio, radio->ptt);
	  console->println("-> set_ptt_rig()");	
          break;
	}
      }
    }
  }
}

void KbdRptParser::OnKeyUp(uint8_t mod, uint8_t key) {
  // check capslock for another modifier
  if (key == 0x39) {
    f_capslock = 0;
  }
}

void KbdRptParser::OnKeyPressed(uint8_t key) {
  if (verbose & 1) plogw->ostream->print((char)key);
};

void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key) {

  MODIFIERKEYS modkey;
  *((uint8_t *)&modkey) = mod;
  uint8_t c = KbdRptParser::OemToAscii(mod, key);

  // send the request to queue (plan)

  
  on_key_down(modkey, key, c);
}



uint8_t KbdRptParser::OemToAscii2(uint8_t mod, uint8_t key) {

  MODIFIERKEYS modkey;
  *((uint8_t *)&modkey) = mod;
  uint8_t c = KbdRptParser::OemToAscii(mod, key);
  return c;
}

void KbdRptParser::PrintKey(uint8_t m, uint8_t key) {
  MODIFIERKEYS mod;
  *((uint8_t *)&mod) = m;
  print_key(mod, key);
}


HIDBoot<USB_HID_PROTOCOL_KEYBOARD> HidKeyboard(&Usb);
KbdRptParser Prs,Prs1;

void init_usb() {

  // external keyboard on the extension board handler 
  Prs1.init_extKbd();
  
  if (Usb.Init() == -1)
    plogw->ostream->println("OSC did not start.");
  delay(500);
  HidKeyboard.SetReportParser(0, &Prs);

  
  bthid.SetReportParser(KEYBOARD_PARSER_ID, &Prs);
  //  bthid.SetReportParser(MOUSE_PARSER_ID, &mousePrs);


  // If "Boot Protocol Mode" does not work, then try "Report Protocol Mode"
  // If that does not work either, then uncomment PRINTREPORT in BTHID.cpp to see the raw report
  bthid.setProtocolMode(USB_HID_BOOT_PROTOCOL);  // Boot Protocol Mode
  //  bthid.setProtocolMode(HID_RPT_PROTOCOL); // Report Protocol Mode

  plogw->ostream->print(F("\r\nHID Bluetooth Library Started"));

}

void loop_usb() {
    Usb.Task();
}

// just a wrapper 
uint8_t kbd_oemtoascii2(uint8_t mod,char c)
{
  return Prs.OemToAscii2(mod, c);
}

void usb_send_civ_buf() {
  return; // return doing nothing 
    if (Usb.getUsbTaskState() == USB_STATE_RUNNING) {
      uint8_t rcode;
      rcode=0;
      //      rcode = Ftdi.SndData(civ_buf_idx, (uint8_t *)civ_buf);
      rcode = Acm.SndData(civ_buf_idx, (uint8_t *)civ_buf);
      if (verbose & 1) {
        plogw->ostream->print("send civ cmd:");
        for (int i = 0; i < civ_buf_idx; i++) {
          plogw->ostream->print((civ_buf[i]), HEX);
          plogw->ostream->print(" ");
        }
        plogw->ostream->println("");
      }

      if (rcode) {
	//        ErrorMessage<uint8_t>(PSTR("SndData"), rcode);
      }
    }
}

void usb_send_cat_buf(char *cmd) {
  return;
    // send to USB host serial adapter
    if (Usb.getUsbTaskState() == USB_STATE_RUNNING) {
      uint8_t rcode;
      //char strbuf[] = "IF;";
      rcode=0;
      //      rcode = Ftdi.SndData(strlen(cmd), (uint8_t *)cmd);
      //      rcode = Acm.SndData(strlen(cmd), (uint8_t *)cmd);      
      if (verbose & 1) {
        plogw->ostream->print("send cat cmd:");
        plogw->ostream->println(cmd);
        //	plogw->ostream->print("r:");plogw->ostream->print(r_ptr);
        //	plogw->ostream->print("w:");plogw->ostream->println(w_ptr);
        //	plogw->ostream->print("cmdbuf:");plogw->ostream->print(cmdbuf);plogw->ostream->print(":");plogw->ostream->println(cmd_ptr);
      }

      if (rcode) {
	//        ErrorMessage<uint8_t>(PSTR("SndData"), rcode);
      }
    }
}


void usb_receive_cat_data(struct radio *radio) {
  return;
    while (Usb.getUsbTaskState() == USB_STATE_RUNNING) {
      uint8_t rcode;
      uint8_t buf[64];
      for (uint8_t i = 0; i < 64; i++)
        buf[i] = 0;

      uint16_t rcvd = 64;
      //      rcode = Ftdi.RcvData(&rcvd, buf);
      rcode = Acm.RcvData(&rcvd, buf);      

      if (rcode && rcode != hrNAK) {
	//        ErrorMessage<uint8_t>(PSTR("Ret"), rcode);
        return;
      }
      if (verbose & 1) {
	//plogw->ostream->print("Ftdi: rcode=");
	plogw->ostream->print("Acm: rcode=");	
        plogw->ostream->println(rcode);
      }

      // The device reserves the first two bytes of data
      //   to contain the current values of the modem and line status registers.

      if (rcvd > 2) {
        if (verbose & 1) {
          plogw->ostream->print("received from USB serial : ");
        }
        for (uint8_t i = 2; i < rcvd; i++) {
          if (verbose & 1) {
            plogw->ostream->print(buf[i], HEX);
            plogw->ostream->print(" ");
          }
          radio->bt_buf[radio->w_ptr] = buf[i];
          radio->w_ptr++;
          radio->w_ptr %= 256;
        }
        if (verbose & 1) {
          plogw->ostream->println("");
        }
        //            plogw->ostream->println((char*)(buf+2));
      }
    }
}
// key input from usb running in separate task 24/10/29 


