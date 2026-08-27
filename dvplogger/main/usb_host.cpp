/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2026 Eiichiro Araki
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
#include "usb_cat_transport.h"

#include <cdcftdi.h>  // serial adapter
#include <cp2105.h>
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
USBHub Hub2(&Usb);

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

static constexpr uint16_t QMX_USB_VID = 0x0483;
static constexpr uint16_t QMX_USB_PID = 0xA34C;

uint8_t ACMAsyncOper::OnInit(ACM *pacm) {
  uint8_t rcode;
  const bool is_qmx = pacm->IsDevice(QMX_USB_VID, QMX_USB_PID);
  const bool is_ats_mini = pacm->IsDevice(0x303A, 0x1001);
  const usb_cat_profile_t &profile = usb_cat_profile(
    is_qmx ? USB_CAT_BACKEND_ACM_QMX : USB_CAT_BACKEND_ACM_GENERIC);

  /*
   * QMX exposes a standard CDC data interface, but its CAT port does not
   * require UART line coding.  Some reconnects fail on SET_LINE_CODING,
   * and DTR/RTS may be assigned to PTT/CW keying.  Skip both requests for
   * QMX and start with a clean CAT queue so stale ICOM CI-V frames cannot
   * be sent to it after enumeration.
   */
  if (is_qmx) {
    const UBaseType_t cleared = usb_cat_reset_tx_queue();
    usb_cat_set_backend(profile.backend);
    console->println("USB ACM connected: QMX CAT");
    if (verbose & VERBOSE_USB) {
      console->printf(
        "USB ACM detail: VID=%04X PID=%04X CDC control skipped queue cleared=%u\n",
        pacm->GetVid(), pacm->GetPid(), (unsigned int)cleared);
    }
    return 0;
  }

  if (is_ats_mini) {
    // ATS Mini Ad hoc uses TinyUSB CDC data endpoints directly.  Avoid
    // SET_CONTROL_LINE_STATE / SET_LINE_CODING: they are unnecessary for the
    // protocol and can disturb/restart some ESP32-S3 TinyUSB builds.
    const UBaseType_t cleared = usb_cat_reset_tx_queue();
    usb_cat_set_backend(USB_CAT_BACKEND_ACM_GENERIC);
    console->printf("USB ACM connected: ATS-MINI VID=%04X PID=%04X\n",
                    pacm->GetVid(), pacm->GetPid());
    if (verbose & VERBOSE_USB)
      console->printf("ATS-MINI CDC control skipped queue cleared=%u\n",
                      (unsigned int)cleared);
    return 0;
  }

  // Preserve the existing ACM initialization for ICOM and other rigs.
  rcode = pacm->SetControlLineState(2); // RTS only
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
  if (rcode) {
    ErrorMessage<uint8_t>(PSTR("SetLineCoding"), rcode);
    return rcode;
  }

  usb_cat_set_backend(profile.backend);
  console->printf("USB ACM connected: VID=%04X PID=%04X\n",
                  pacm->GetVid(), pacm->GetPid());
  return 0;
}

ACMAsyncOper AsyncOper;
ACM Acm(&Usb, &AsyncOper);
CP2105 Cp2105(&Usb);
static uint8_t cp2105_cat_port = 0;

bool usb_qmx_cat_ready() {
  return usb_cat_ready_for_rig_type(CAT_TYPE_QMX);
}

bool usb_cat_ready_for_rig_type(uint8_t cat_type) {
  switch (cat_type) {
  case CAT_TYPE_QMX:
    return Acm.isReady() && Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
  case CAT_TYPE_ATS_MINI:
    // ATS Mini is standard ESP32-S3 CDC ACM. The selected rig profile
    // identifies the protocol, so do not depend on a fixed VID/PID.
    return Acm.isReady() && !Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
  case CAT_TYPE_YAESU_NEW:
  case CAT_TYPE_YAESU_OLD:
  case CAT_TYPE_YAESU_FT817:
    // Yaesu USB CAT will use the selected CP2105 port.  The profile is
    // already common; only descriptor-based port selection remains.
    return Cp2105.isReady() && Cp2105.portReady(cp2105_cat_port);
  default:
    return Acm.isReady() ||
           (Cp2105.isReady() && Cp2105.portReady(cp2105_cat_port));
  }
}

static bool cp2105_debug = false;

static void dump_cp2105_data(const char *direction,
                             uint8_t port,
                             const uint8_t *data,
                             uint16_t len)
{
    console->printf("CP2105 %s port%u len=%u HEX:",
                    direction, port, len);

    for (uint16_t i = 0; i < len; i++) {
        console->printf(" %02X", data[i]);
    }

    console->print(" ASCII:\"");

    for (uint16_t i = 0; i < len; i++) {
        char c = static_cast<char>(data[i]);
        if (c >= 0x20 && c <= 0x7e)
            console->print(c);
        else
            console->print('.');
    }

    console->println("\"");
}

bool CP2105selectPort(uint8_t port) {
  if (port >= CP2105::PORTS) return false;
  cp2105_cat_port = port;
  console->printf("CP2105 CAT port=%u\n", cp2105_cat_port);
  return true;
}

bool CP2105setBaud(uint8_t port, uint32_t baudrate) {
  if (!Cp2105.isReady() || port >= CP2105::PORTS || baudrate == 0) return false;
  uint8_t rcode = Cp2105.ConfigurePort(port, baudrate);
  console->printf("CP2105 port %u baud %lu rcode=0x%02x\n",
                  port, (unsigned long)baudrate, rcode);
  return rcode == 0;
}

void CP2105status(Stream *out) {
  if (!out) out = console;
  out->printf("CP2105 ready=%d addr=0x%02x CATport=%u\n",
                  Cp2105.isReady() ? 1 : 0,
                  Cp2105.GetAddress(), cp2105_cat_port);
  for (uint8_t port = 0; port < CP2105::PORTS; port++) {
    out->printf(" port%u ready=%d if=%u IN=%u/%u OUT=%u/%u baud=%lu%s\n",
                    port, Cp2105.portReady(port) ? 1 : 0,
                    Cp2105.interfaceNumber(port),
                    Cp2105.inEndpoint(port), Cp2105.inMaxPacket(port),
                    Cp2105.outEndpoint(port), Cp2105.outMaxPacket(port),
                    (unsigned long)Cp2105.baudRate(port),
                    port == cp2105_cat_port ? " CAT" : "");
  }
}

void CP2105process() {
  if (!Cp2105.isReady() || !Cp2105.portReady(cp2105_cat_port)) return;
  usb_cat_set_backend(USB_CAT_BACKEND_CP2105);

  struct catmsg_t catmsg;
  while (usb_cat_dequeue(&catmsg)) {

      if (cp2105_debug) {
	dump_cp2105_data("TX",
			 cp2105_cat_port,
			 reinterpret_cast<uint8_t *>(catmsg.buf),
			 catmsg.size);
      }

      uint8_t rcode =
	Cp2105.SndData(cp2105_cat_port,
		       catmsg.size,
		       reinterpret_cast<uint8_t *>(catmsg.buf));

      if (rcode && rcode != hrNAK) {
	console->printf("CP2105 TX error port%u rcode=0x%02X\n",
			cp2105_cat_port, rcode);
	// break; //?
      }
 
      /*      uint8_t rcode = Cp2105.SndData(cp2105_cat_port, catmsg.size,
                                    reinterpret_cast<uint8_t *>(catmsg.buf));
      if (rcode && rcode != hrNAK) {
        ErrorMessage<uint8_t>(PSTR("CP2105 SndData"), rcode);
        break;
      }
      */
  }

  uint8_t buf[64];
  uint16_t rcvd = Cp2105.inMaxPacket(cp2105_cat_port);
  if (rcvd == 0 || rcvd > sizeof(buf)) rcvd = sizeof(buf);
  
  uint8_t rcode = Cp2105.RcvData(cp2105_cat_port, &rcvd, buf);

  if (rcode && rcode != hrNAK) {
    console->printf("CP2105 RX error port%u rcode=0x%02X\n",
                    cp2105_cat_port, rcode);
    return;
  }

  if (rcvd) {
    if (cp2105_debug) {
      dump_cp2105_data("RX",
		       cp2105_cat_port,
		       buf,
		       rcvd);
    }

    usb_cat_deliver_rx(buf, rcvd);
  }
 
  /*  if (rcode && rcode != hrNAK) {
    ErrorMessage<uint8_t>(PSTR("CP2105 RcvData"), rcode);
    return;
  }
  if (rcvd) {
    catmsg.size = min((uint16_t)sizeof(catmsg.buf), rcvd);
    memcpy(catmsg.buf, buf, catmsg.size);
    xQueueSend(xQueueCATUSBRx, &catmsg, 0);
    if (verbose & 1) {
      console->printf("CP2105 port%u received %u bytes\n",
                      cp2105_cat_port, catmsg.size);
    }
  }
  */
}



void CP2105toggleDebug()
{
    cp2105_debug = !cp2105_debug;
    console->printf("CP2105 debug=%d\n",
                    cp2105_debug ? 1 : 0);
}
bool CP2105sendRaw(uint8_t port, const char *text)
{
    if (!text ||
        !Cp2105.isReady() ||
        !Cp2105.portReady(port)) {
        return false;
    }

    uint16_t len = strlen(text);

    dump_cp2105_data(
        "RAW-TX",
        port,
        reinterpret_cast<const uint8_t *>(text),
        len);

    uint8_t rcode =
        Cp2105.SndData(
            port,
            len,
            reinterpret_cast<uint8_t *>(
                const_cast<char *>(text)));

    console->printf("CP2105 raw port%u rcode=0x%02X\n",
                    port, rcode);

    return rcode == 0;
}

static bool ats_mini_monitor_started = false;
static uint32_t ats_mini_monitor_retry_ms = 0;

static bool ats_mini_usb_rig_active()
{
  for (int i = 0; i < N_RADIO; ++i) {
    if (!radio_list[i].enabled || radio_list[i].rig_spec == NULL) continue;
    if (radio_list[i].rig_spec->civport_num == -1 &&
        radio_list[i].rig_spec->cat_type == CAT_TYPE_ATS_MINI)
      return true;
  }
  return false;
}

static void ats_mini_start_monitor_if_needed()
{
  if (!Acm.isReady() || !Acm.IsDevice(0x303A, 0x1001) ||
      !ats_mini_usb_rig_active() || ats_mini_monitor_started)
    return;

  const uint32_t now = millis();
  if ((int32_t)(now - ats_mini_monitor_retry_ms) < 0) return;

  // Send 't' directly and consider the monitor started ONLY after the USB OUT
  // transfer succeeds. hrNAK means "try again later", not failure.
  uint8_t cmd = 't';
  const uint8_t rcode = Acm.SndData(1, &cmd);
  if (rcode == 0) {
    ats_mini_monitor_started = true;
    usb_cat_set_backend(USB_CAT_BACKEND_ACM_GENERIC);
    console->println("ATS-MINI USB: status monitor enabled");
    return;
  }

  if (rcode == hrNAK) {
    ats_mini_monitor_retry_ms = now + 100;
    return;
  }

  ats_mini_monitor_retry_ms = now + 500;
  if (verbose & VERBOSE_USB)
    console->printf("ATS-MINI monitor start rcode=0x%02X; retrying\\n", rcode);
}

void ACMprocess() {
  /*
   * One-second heartbeat for the USB CAT path.  Print while QMX is attached
   * or while the TX queue contains data, so a stalled consumer is visible
   * without flooding normal non-CAT operation.
   */
  static uint32_t last_cat_heartbeat = 0;
  const bool qmx_attached = Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
  const UBaseType_t tx_waiting = usb_cat_tx_waiting();
  const uint32_t now = millis();
  if (!Acm.isReady() && !Cp2105.isReady()) {
    usb_cat_set_backend(USB_CAT_BACKEND_NONE);
    ats_mini_monitor_started = false;
    ats_mini_monitor_retry_ms = 0;
  }
  if ((verbose & VERBOSE_USB) &&
      (qmx_attached || tx_waiting != 0) &&
      now - last_cat_heartbeat >= 1000) {
    last_cat_heartbeat = now;
    console->printf(
      "USB CAT heartbeat state=0x%02X vbus=0x%02X "
      "ACMready=%d QMX=%d CP2105ready=%d waiting=%u free=%u\n",
      Usb.getUsbTaskState(), Usb.getVbusState(),
      Acm.isReady() ? 1 : 0, qmx_attached ? 1 : 0,
      Cp2105.isReady() ? 1 : 0,
      (unsigned int)tx_waiting,
      (unsigned int)usb_cat_tx_free());
  }

  if (Cp2105.isReady()) {
    if ((verbose & VERBOSE_USB) && (qmx_attached || tx_waiting != 0)) {
      static uint32_t last_cp2105_redirect_report = 0;
      if (now - last_cp2105_redirect_report >= 1000) {
        last_cp2105_redirect_report = now;
        console->printf(
          "USB CAT consumer redirected to CP2105 addr=0x%02X waiting=%u\n",
          Cp2105.GetAddress(), (unsigned int)tx_waiting);
      }
    }
    CP2105process();
    return;
  }
  // IC-705,IC-905 operation
  uint8_t rcode;
  int ret;
  struct catmsg_t catmsg;

  if ((verbose & VERBOSE_USB) && !Acm.isReady() && tx_waiting != 0) {
    static uint32_t last_not_ready_report = 0;
    if (now - last_not_ready_report >= 1000) {
      last_not_ready_report = now;
      console->printf(
        "USB CAT TX blocked: ACM not ready state=0x%02X waiting=%u free=%u\n",
        Usb.getUsbTaskState(), (unsigned int)tx_waiting,
        (unsigned int)usb_cat_tx_free());
    }
  }
  
  static bool qmx_ready_prev = false;
  const bool qmx_ready_now = Acm.isReady() && qmx_attached;
  if (qmx_ready_now && !qmx_ready_prev) {
    if (verbose & VERBOSE_USB) console->printf(
      "QMX CDC endpoints: BulkOUT=%02X BulkIN=%02X BulkIN2=%02X second=%d\n",
      Acm.GetDataOutEp(), Acm.GetDataInEp(), Acm.GetSecondDataInEp(),
      Acm.HasSecondDataIn() ? 1 : 0);

    // Queries generated before USB enumeration are deliberately suppressed.
    // Seed the newly ready connection with a clean first status request.
    usb_cat_set_backend(USB_CAT_BACKEND_ACM_QMX);
    usb_cat_reset_tx_queue();
    const usb_cat_profile_t &profile = usb_cat_profile(USB_CAT_BACKEND_ACM_QMX);
    const bool queued = profile.startup_query &&
      usb_cat_enqueue(reinterpret_cast<const uint8_t *>(profile.startup_query),
                      strlen(profile.startup_query), false);
    if (verbose & VERBOSE_USB) console->printf(
      "QMX CAT startup enqueue ret=%d waiting=%u free=%u cmd=%s\n",
      queued ? 1 : 0,
      (unsigned int)usb_cat_tx_waiting(),
      (unsigned int)usb_cat_tx_free(),
      profile.startup_query ? profile.startup_query : "-");
  }
  qmx_ready_prev = qmx_ready_now;

  if (Acm.isReady()) {
    ats_mini_start_monitor_if_needed();

    // check queue and forward to USB
    while (usb_cat_dequeue(&catmsg)) {
      ret = pdTRUE;
	const bool is_qmx = Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
        usb_cat_set_backend(is_qmx ? USB_CAT_BACKEND_ACM_QMX
                                   : USB_CAT_BACKEND_ACM_GENERIC);
        usb_cat_dump("TX", catmsg.buf, catmsg.size);
	rcode = Acm.SndData(catmsg.size, (uint8_t *)catmsg.buf);
        const bool is_ats_mini = Acm.IsDevice(0x303A, 0x1001);
	if (verbose & VERBOSE_USB) {
	  console->printf("%sUSB CAT TX complete rcode=0x%02X len=%d\n",
	                  is_qmx ? "QMX " : (is_ats_mini ? "ATS-MINI " : ""),
                          rcode, catmsg.size);
	}
        if (is_ats_mini && rcode == hrNAK) {
          // TinyUSB may NAK briefly after enumeration or while its CDC task is
          // busy. Preserve command order and retry instead of silently losing
          // F<Hz> or other ATS commands.
          if (!usb_cat_requeue_front(&catmsg) && (verbose & VERBOSE_USB))
            console->println("ATS-MINI TX NAK: failed to requeue command");
          break;
        }
	if (rcode) {
	  ErrorMessage<uint8_t>(PSTR("SndData CATUSBTx"), rcode);
	  plogw->ostream->println("SndData CATUSBTx error");
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
      const bool is_qmx = Acm.IsDevice(QMX_USB_VID, QMX_USB_PID);
      usb_cat_set_backend(is_qmx ? USB_CAT_BACKEND_ACM_QMX
                                 : USB_CAT_BACKEND_ACM_GENERIC);
      usb_cat_dump("RX", buf, rcvd);
      ret = usb_cat_deliver_rx(buf, rcvd) ? pdTRUE : pdFALSE;
      if (verbose & VERBOSE_USB) plogw->ostream->printf(
        "CATUSBRx queuesend ret=%d size=%u\r\n", ret,
        (unsigned int)rcvd);
    }

    // QMX has only one CDC data interface.  Polling RcvData1() when no
    // second Bulk-IN endpoint exists can enter a long MAX3421E NAK loop and
    // starve IDLE0, triggering the task watchdog.
    if (Acm.HasSecondDataIn()) {
      rcvd=64;
      rcode = Acm.RcvData1(&rcvd, buf);
      if (rcode && rcode != hrNAK) ErrorMessage<uint8_t>(PSTR("Ret1"), rcode);

      if (rcvd) {
        plogw->ostream->print("ACMrcvd1:");
        for (uint16_t i = 0; i < rcvd; i++) {
          plogw->ostream->print((char)buf[i]);
        }
        plogw->ostream->print("\r\n");
      }
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
  // New extension firmware appends an 8-bit event sequence number.  Continue
  // accepting the legacy two-byte packet during mixed-version updates.
  static bool seq_valid = false;
  static uint8_t expected_seq = 0;

  if (packet->idx >= 3) {
    const uint8_t received_seq = (uint8_t)packet->buf[2];
    if (seq_valid && received_seq != expected_seq) {
      Serial.printf("KBD EXT sequence gap expected=%u received=%u; resync\n",
                    (unsigned int)expected_seq,
                    (unsigned int)received_seq);
      Prs1.resync_extKbd("sequence gap");
    }
    expected_seq = (uint8_t)(received_seq + 1);
    seq_valid = true;
  }

  if (packet->idx >= 2) {
    Prs1.Parse_extKbd((uint8_t)packet->buf[0], packet->buf[1] != 0);
  }
}

void KbdRptParser::init_extKbd()
{
    for (uint8_t i = 1; i < 8; i++) {
      prevState.bInfo[i]=1;
    }
    prevState.bInfo[0]=0;
}

  
void KbdRptParser::resync_extKbd(const char *reason)
{
  const uint8_t old_mod = prevState.bInfo[0];

  // Release modifier-driven functions (notably Right-Shift PTT/keying) before
  // clearing the parser state.  Normal keys do not have persistent actions.
  if (old_mod != 0) {
    OnControlKeysChanged(old_mod, 0);
  }

  for (uint8_t i = 1; i < 8; i++) {
    prevState.bInfo[i] = 1;
  }
  prevState.bInfo[0] = 0;
  buf_ext[0] = 0;
  f_capslock = 0;

  Serial.printf("KBD EXT resync reason=%s old_mod=0x%02X\n",
                reason ? reason : "unknown", (unsigned int)old_mod);
}

void KbdRptParser::Parse_extKbd(uint8_t hid_code,bool on) 
{
  /*
   * The extension-board keyboard reports each key transition separately.
   * Treating CapsLock as Ctrl later through f_capslock is racy for chords:
   * the CapsLock key event can be queued/released independently of Shift+key.
   * Convert CapsLock to the real Left-Ctrl HID modifier here, before modifier
   * state and key-down events are generated.  This makes Caps+Shift+2 behave
   * exactly like Ctrl+Shift+2 and also keeps the existing Ctrl shortcuts.
   */
  if (hid_code == UHS_HID_BOOT_KEY_CAPS_LOCK) {
    hid_code = 0xe0;  // Left Ctrl HID usage
  }

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

bool KbdRptParser::send_keyrpt_queue(){
  const BaseType_t ret = xQueueSend(xQueueKeyRpt, &msg, 0);
  if (ret != pdTRUE) {
    key_queue_drop_count++;
    const uint32_t now_ms = millis();
    if (key_queue_drop_last_report_ms == 0 ||
        (uint32_t)(now_ms - key_queue_drop_last_report_ms) >= 1000U) {
      key_queue_drop_last_report_ms = now_ms;
      Serial.printf("KBD queue full drops=%lu type=%u arg1=0x%02X arg2=0x%02X depth=%u\n",
                    (unsigned long)key_queue_drop_count,
                    (unsigned int)msg.type, (unsigned int)msg.arg1,
                    (unsigned int)msg.arg2,
                    (unsigned int)uxQueueMessagesWaiting(xQueueKeyRpt));
    }
    return false;
  }
  return true;
}

void KbdRptParser::process_keyrpt_queue(const char *profile_name) {
  struct key_profile_stats_t {
    uint32_t window_start_ms;
    uint32_t calls;
    uint32_t messages;
    uint32_t max_total_us;
    uint32_t max_waiting_us;
    uint32_t max_receive_us;
    uint32_t max_control_us;
    uint32_t max_locking_us;
    uint32_t max_keydown_us;
    uint32_t max_keyup_us;
    uint32_t slow_total;
    uint32_t slow_handler;
    UBaseType_t max_depth;
  };

  // process_keyrpt_queue() is called for the main and external keyboard.
  // Keep independent statistics without adding state to KbdRptParser.
  static key_profile_stats_t stats[2] = {};
  const int profile_index =
      (profile_name != NULL && profile_name[0] == 'e') ? 1 : 0;
  key_profile_stats_t &st = stats[profile_index];
  const char *name = (profile_name != NULL) ? profile_name : "unknown";
  const bool perf_verbose = (verbose & VERBOSE_PERF) != 0;
  const uint32_t slow_threshold_us = 5000;
  const uint32_t total_start_us = micros();

  st.calls++;

  uint32_t t0 = micros();
  UBaseType_t waiting = uxQueueMessagesWaiting(xQueueKeyRpt);
  uint32_t dt = (uint32_t)(micros() - t0);
  if (dt > st.max_waiting_us) st.max_waiting_us = dt;
  if (waiting > st.max_depth) st.max_depth = waiting;

  struct keymsg_t msg;
  BaseType_t ret;

  while (waiting > 0) {
    t0 = micros();
    ret = xQueueReceive(xQueueKeyRpt, &msg, 0);
    dt = (uint32_t)(micros() - t0);
    if (dt > st.max_receive_us) st.max_receive_us = dt;

    if (ret == pdTRUE) {
      st.messages++;
      const uint32_t handler_start_us = micros();
      const char *handler_name = "unknown";

      switch (msg.type) {
      case KEYMSG_TYPE_ONCONTROLKEYSCHANGED:
        handler_name = "control";
        OnControlKeysChanged(msg.arg1, msg.arg2);
        dt = (uint32_t)(micros() - handler_start_us);
        if (dt > st.max_control_us) st.max_control_us = dt;
        break;

      case KEYMSG_TYPE_HANDLELOCKINGKEYS:
        handler_name = "locking";
        HandleLockingKeys(msg.hid, msg.arg2);
        dt = (uint32_t)(micros() - handler_start_us);
        if (dt > st.max_locking_us) st.max_locking_us = dt;
        break;

      case KEYMSG_TYPE_ONKEYDOWN:
        handler_name = "keydown";
        OnKeyDown(msg.arg1, msg.arg2);
        dt = (uint32_t)(micros() - handler_start_us);
        if (dt > st.max_keydown_us) st.max_keydown_us = dt;
        break;

      case KEYMSG_TYPE_ONKEYUP:
        handler_name = "keyup";
        OnKeyUp(msg.arg1, msg.arg2);
        dt = (uint32_t)(micros() - handler_start_us);
        if (dt > st.max_keyup_us) st.max_keyup_us = dt;
        break;

      default:
        dt = (uint32_t)(micros() - handler_start_us);
        break;
      }

      if (dt >= slow_threshold_us) {
        st.slow_handler++;
        if (perf_verbose) {
          Serial.printf(
              "KEY PROFILE SLOW keyboard=%s stage=%s dt=%luus type=%u "
              "arg1=0x%02X arg2=0x%02X depth=%u core=%d\n",
              name, handler_name, (unsigned long)dt,
              (unsigned int)msg.type, (unsigned int)msg.arg1,
              (unsigned int)msg.arg2, (unsigned int)waiting,
              xPortGetCoreID());
        }
      }
    }

    t0 = micros();
    waiting = uxQueueMessagesWaiting(xQueueKeyRpt);
    dt = (uint32_t)(micros() - t0);
    if (dt > st.max_waiting_us) st.max_waiting_us = dt;
    if (waiting > st.max_depth) st.max_depth = waiting;
  }

  const uint32_t total_us = (uint32_t)(micros() - total_start_us);
  if (total_us > st.max_total_us) st.max_total_us = total_us;
  if (total_us >= slow_threshold_us) {
    st.slow_total++;
    if (perf_verbose) {
      Serial.printf(
          "KEY PROFILE SLOW keyboard=%s stage=total dt=%luus messages=%lu "
          "max_depth=%u core=%d\n",
          name, (unsigned long)total_us, (unsigned long)st.messages,
          (unsigned int)st.max_depth, xPortGetCoreID());
    }
  }

  const uint32_t now_ms = millis();
  if (st.window_start_ms == 0) st.window_start_ms = now_ms;
  if (perf_verbose && (uint32_t)(now_ms - st.window_start_ms) >= 1000U) {
    Serial.printf(
        "KEY PROFILE summary keyboard=%s calls=%lu messages=%lu depth=%u "
        "total=%lu waiting=%lu receive=%lu control=%lu locking=%lu "
        "keydown=%lu keyup=%lu slow_total=%lu slow_handler=%lu\n",
        name, (unsigned long)st.calls, (unsigned long)st.messages,
        (unsigned int)st.max_depth, (unsigned long)st.max_total_us,
        (unsigned long)st.max_waiting_us, (unsigned long)st.max_receive_us,
        (unsigned long)st.max_control_us, (unsigned long)st.max_locking_us,
        (unsigned long)st.max_keydown_us, (unsigned long)st.max_keyup_us,
        (unsigned long)st.slow_total, (unsigned long)st.slow_handler);

    key_profile_stats_t cleared = {};
    cleared.window_start_ms = now_ms;
    st = cleared;
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
void init_usb()
{
    Prs1.init_extKbd();

    /*
     * Power-cycle an already-connected USB device.
     * This emulates unplugging and reconnecting it.
     */
    Usb.vbusPower(vbus_off);
    delay(1500);

    Usb.vbusPower(vbus_on);
    delay(1000);

    int8_t ret = Usb.Init(1500);

    Serial.printf(
		  "Usb.Init(1500) returned %d, vbus=%02x\n",
		  ret,
		  Usb.getVbusState()
		  );

    if (ret == -1) {
      plogw->ostream->println("OSC did not start.");
    }
 
    //    int8_t ret = Usb.Init();
    //    Serial.printf("Usb.Init() returned %d\n", ret);
    //    if (ret == -1) {
    //        plogw->ostream->println("OSC did not start.");
    //    }

    HidKeyboard.SetReportParser(0, &Prs);

    bthid.SetReportParser(KEYBOARD_PARSER_ID, &Prs);
    bthid.setProtocolMode(USB_HID_BOOT_PROTOCOL);

    plogw->ostream->print(
        F("\r\nHID Bluetooth Library Started")
    );
}
void init_usb_bak() {

  // external keyboard on the extension board handler 
  Prs1.init_extKbd();

  // Wait before sampling the USB bus.


  int ret;
  if ((ret=Usb.Init()) == -1) 
    console->printf("Usb.Init() returned %d\n", ret);
  
  if (ret == -1)  
    plogw->ostream->println("OSC did not start.");
  
  HidKeyboard.SetReportParser(0, &Prs);
  
  bthid.SetReportParser(KEYBOARD_PARSER_ID, &Prs);
  //  bthid.SetReportParser(MOUSE_PARSER_ID, &mousePrs);


  // If "Boot Protocol Mode" does not work, then try "Report Protocol Mode"
  // If that does not work either, then uncomment PRINTREPORT in BTHID.cpp to see the raw report
  bthid.setProtocolMode(USB_HID_BOOT_PROTOCOL);  // Boot Protocol Mode
  //  bthid.setProtocolMode(HID_RPT_PROTOCOL); // Report Protocol Mode

  plogw->ostream->print(F("\r\nHID Bluetooth Library Started"));

}
void loop_usb()
{
    static uint8_t previous_state = 0xff;
    static uint8_t previous_vbus = 0xff;
    static uint32_t last_report = 0;

    Usb.Task();

    uint8_t state = Usb.getUsbTaskState();
    uint8_t vbus = Usb.getVbusState();

    if ((verbose & VERBOSE_USB) &&
        (state != previous_state || vbus != previous_vbus)) {
        Serial.printf(
            "USB: state 0x%02x -> 0x%02x, "
            "vbus 0x%02x -> 0x%02x, ACM=%d\n",
            previous_state,
            state,
            previous_vbus,
            vbus,
            Acm.isReady()
        );

    }

    previous_state = state;
    previous_vbus = vbus;

    /*
    if (millis() - last_report >= 1000) {
        last_report = millis();

        Serial.printf(
            "USB heartbeat: state=0x%02x vbus=0x%02x ACM=%d\n",
            state,
            vbus,
            Acm.isReady()
        );
    }
    */
}

void loop_usb_bak1()
{
    static uint8_t previous_state = 0xff;
    static uint32_t last_report = 0;
    static uint32_t task_count = 0;

    Usb.Task();
    task_count++;

    uint8_t state = Usb.getUsbTaskState();

    if (state != previous_state) {
        Serial.printf(
            "USB state changed: 0x%02x -> 0x%02x, ACM ready=%d\n",
            previous_state,
            state,
            Acm.isReady()
        );
        previous_state = state;
    }

    /*
     * Print periodically even when the state does not change.
     * This also confirms that usb_loop_task() is alive.
     */
    /*
    if (millis() - last_report >= 1000) {
        last_report = millis();

        Serial.printf(
            "USB heartbeat: count=%lu state=0x%02x ACM=%d\n",
            (unsigned long)task_count,
            state,
            Acm.isReady()
        );
    }
    */
}

void loop_usb_bak() {
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
  if (radio == NULL || radio->rig_spec == NULL) return;
  if (radio->rig_spec->civport_num != -1) return;
  if (xQueueCATUSBRx == NULL) return;

  struct catmsg_t catmsg;
  int copied = 0;
  int dropped = 0;

  // USB bulk packets can split a CAT response at any byte boundary.  Copy
  // every received chunk into the existing per-radio CAT ring buffer; the
  // normal CAT parser will join the chunks and recognize the ';' terminator.
  while (xQueueReceive(xQueueCATUSBRx, &catmsg, 0) == pdTRUE) {
    int size = catmsg.size;
    if (size < 0) size = 0;
    if (size > (int)sizeof(catmsg.buf)) size = sizeof(catmsg.buf);

    for (int i = 0; i < size; i++) {
      const int next = (radio->w_ptr + 1) % 256;
      if (next == radio->r_ptr) {
        // Keep the already buffered partial command intact.  Drop the rest of
        // this USB chunk and wait for the parser to make room.
        dropped += size - i;
        break;
      }
      radio->bt_buf[radio->w_ptr] = catmsg.buf[i];
      radio->w_ptr = next;
      copied++;
    }
  }

  if ((verbose & VERBOSE_USB) && (copied > 0 || dropped > 0)) {
    console->printf(
        "USB CAT RX bridge rig=%d copied=%d dropped=%d r=%d w=%d\n",
        radio->rig_idx, copied, dropped, radio->r_ptr, radio->w_ptr);
  }
}
// key input from usb running in separate task 24/10/29 


