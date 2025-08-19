# Check ESP-IDF version and error out if it is not in the supported range.
#
# Note for arduino-esp32 developers: to bypass the version check locally,
# set ARDUINO_SKIP_IDF_VERSION_CHECK environment variable to 1. For example:
#   export ARDUINO_SKIP_IDF_VERSION_CHECK=1
#   idf.py build

set(min_supported_idf_version "4.4.0")
set(max_supported_idf_version "4.4.99")
set(idf_version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")

if ("${idf_version}" AND NOT "$ENV{ARDUINO_SKIP_IDF_VERSION_CHECK}")
  if (idf_version VERSION_LESS min_supported_idf_version)
    message(FATAL_ERROR "Arduino-esp32 can be used with ESP-IDF versions "
                        "between ${min_supported_idf_version} and ${max_supported_idf_version}, "
                        "but an older version is detected: ${idf_version}.")
  endif()
  if (idf_version VERSION_GREATER max_supported_idf_version)
    message(FATAL_ERROR "Arduino-esp32 can be used with ESP-IDF versions "
                        "between ${min_supported_idf_version} and ${max_supported_idf_version}, "
                        "but a newer version is detected: ${idf_version}.")
  endif()
endif()

set(CORE_SRCS
  cores/esp32/base64.cpp
  cores/esp32/cbuf.cpp
  cores/esp32/esp32-hal-adc.c
  cores/esp32/esp32-hal-bt.c
  cores/esp32/esp32-hal-cpu.c
  cores/esp32/esp32-hal-dac.c
  cores/esp32/esp32-hal-gpio.c
  cores/esp32/esp32-hal-i2c.c
  cores/esp32/esp32-hal-i2c-slave.c
  cores/esp32/esp32-hal-ledc.c
  cores/esp32/esp32-hal-matrix.c
  cores/esp32/esp32-hal-misc.c
  cores/esp32/esp32-hal-psram.c
  cores/esp32/esp32-hal-rgb-led.c
  cores/esp32/esp32-hal-sigmadelta.c
  cores/esp32/esp32-hal-spi.c
  cores/esp32/esp32-hal-time.c
  cores/esp32/esp32-hal-timer.c
  cores/esp32/esp32-hal-tinyusb.c
  cores/esp32/esp32-hal-touch.c
  cores/esp32/esp32-hal-uart.c
  cores/esp32/esp32-hal-rmt.c
  cores/esp32/Esp.cpp
  cores/esp32/FunctionalInterrupt.cpp
  cores/esp32/HardwareSerial.cpp
  cores/esp32/IPAddress.cpp
  cores/esp32/IPv6Address.cpp
  cores/esp32/libb64/cdecode.c
  cores/esp32/libb64/cencode.c
  cores/esp32/main.cpp
  cores/esp32/MD5Builder.cpp
  cores/esp32/Print.cpp
  cores/esp32/stdlib_noniso.c
  cores/esp32/Stream.cpp
  cores/esp32/StreamString.cpp
  cores/esp32/Tone.cpp
  cores/esp32/HWCDC.cpp
  cores/esp32/USB.cpp
  cores/esp32/USBCDC.cpp
  cores/esp32/USBMSC.cpp
  cores/esp32/FirmwareMSC.cpp
  cores/esp32/firmware_msc_fat.c
  cores/esp32/wiring_pulse.c
  cores/esp32/wiring_shift.c
  cores/esp32/WMath.cpp
  cores/esp32/WString.cpp
  )

set(LIBRARY_SRCS
  libraries/ArduinoOTA/src/ArduinoOTA.cpp
  libraries/AsyncUDP/src/AsyncUDP.cpp
  libraries/BluetoothSerial/src/BluetoothSerial.cpp
  libraries/BluetoothSerial/src/BTAddress.cpp
  libraries/BluetoothSerial/src/BTAdvertisedDeviceSet.cpp
  libraries/BluetoothSerial/src/BTScanResultsSet.cpp
  libraries/DNSServer/src/DNSServer.cpp
  libraries/EEPROM/src/EEPROM.cpp
  libraries/ESPmDNS/src/ESPmDNS.cpp
  libraries/Ethernet/src/ETH.cpp
  libraries/FFat/src/FFat.cpp
  libraries/FS/src/FS.cpp
  libraries/FS/src/vfs_api.cpp
  libraries/HTTPClient/src/HTTPClient.cpp
  libraries/HTTPUpdate/src/HTTPUpdate.cpp
  libraries/LittleFS/src/LittleFS.cpp
  libraries/Insights/src/Insights.cpp
  libraries/I2S/src/I2S.cpp
  libraries/NetBIOS/src/NetBIOS.cpp
  libraries/Preferences/src/Preferences.cpp
  libraries/RainMaker/src/RMaker.cpp
  libraries/RainMaker/src/RMakerNode.cpp
  libraries/RainMaker/src/RMakerParam.cpp
  libraries/RainMaker/src/RMakerDevice.cpp
  libraries/RainMaker/src/RMakerType.cpp
  libraries/RainMaker/src/RMakerQR.cpp
  libraries/RainMaker/src/RMakerUtils.cpp
  libraries/RainMaker/src/AppInsights.cpp
  libraries/SD_MMC/src/SD_MMC.cpp
  libraries/SD/src/SD.cpp
  libraries/SD/src/sd_diskio.cpp
  libraries/SD/src/sd_diskio_crc.c
  libraries/SimpleBLE/src/SimpleBLE.cpp
  libraries/SPIFFS/src/SPIFFS.cpp
  libraries/SPI/src/SPI.cpp
  libraries/Ticker/src/Ticker.cpp
  libraries/Update/src/Updater.cpp
  libraries/Update/src/HttpsOTAUpdate.cpp
  libraries/USB/src/USBHID.cpp
  libraries/USB/src/USBHIDMouse.cpp
  libraries/USB/src/USBHIDKeyboard.cpp
  libraries/USB/src/USBHIDGamepad.cpp
  libraries/USB/src/USBHIDConsumerControl.cpp
  libraries/USB/src/USBHIDSystemControl.cpp
  libraries/USB/src/USBHIDVendor.cpp
  libraries/USB/src/USBVendor.cpp
  libraries/WebServer/src/WebServer.cpp
  libraries/WebServer/src/Parsing.cpp
  libraries/WebServer/src/detail/mimetable.cpp
  libraries/WiFiClientSecure/src/ssl_client.cpp
  libraries/WiFiClientSecure/src/esp_crt_bundle.c
  libraries/WiFiClientSecure/src/WiFiClientSecure.cpp
  libraries/WiFi/src/WiFiAP.cpp
  libraries/WiFi/src/WiFiClient.cpp
  libraries/WiFi/src/WiFi.cpp
  libraries/WiFi/src/WiFiGeneric.cpp
  libraries/WiFi/src/WiFiMulti.cpp
  libraries/WiFi/src/WiFiScan.cpp
  libraries/WiFi/src/WiFiServer.cpp
  libraries/WiFi/src/WiFiSTA.cpp
  libraries/WiFi/src/WiFiUdp.cpp
  libraries/WiFiProv/src/WiFiProv.cpp
  libraries/Wire/src/Wire.cpp
libraries/u8g2/csrc/u8x8_setup.c
libraries/u8g2/csrc/u8x8_d_ssd1306_128x64_noname.c
libraries/u8g2/csrc/u8x8_d_ssd1306_96x40.c
libraries/u8g2/csrc/u8x8_8x8.c
libraries/u8g2/csrc/mui_u8g2.c
libraries/u8g2/csrc/u8x8_d_s1d15721.c
libraries/u8g2/csrc/u8x8_d_ld7032_60x32.c
libraries/u8g2/csrc/u8x8_d_gp1247ai.c
libraries/u8g2/csrc/u8x8_capture.c
libraries/u8g2/csrc/u8x8_d_st75320.c
libraries/u8g2/csrc/u8x8_d_stdio.c
libraries/u8g2/csrc/u8g2_bitmap.c
libraries/u8g2/csrc/u8g2_arc.c
libraries/u8g2/csrc/u8x8_debounce.c
libraries/u8g2/csrc/u8x8_d_st75256.c
libraries/u8g2/csrc/u8x8_d_sh1106_72x40.c
libraries/u8g2/csrc/u8x8_d_ssd1306_2040x16.c
libraries/u8g2/csrc/u8g2_selection_list.c
libraries/u8g2/csrc/u8g2_kerning.c
libraries/u8g2/csrc/u8x8_d_t6963.c
libraries/u8g2/csrc/u8x8_d_ssd1327.c
libraries/u8g2/csrc/u8x8_d_st7567.c
libraries/u8g2/csrc/u8x8_d_ssd1329.c
libraries/u8g2/csrc/u8x8_d_ssd1309.c
libraries/u8g2/csrc/u8x8_d_uc1701_dogs102.c
libraries/u8g2/csrc/u8x8_d_uc1628.c
libraries/u8g2/csrc/u8x8_u8toa.c
libraries/u8g2/csrc/u8x8_d_uc1604.c
libraries/u8g2/csrc/u8x8_d_ssd1306_64x48.c
libraries/u8g2/csrc/u8x8_d_uc1617.c
libraries/u8g2/csrc/u8x8_d_st7588.c
libraries/u8g2/csrc/u8x8_d_pcf8814_hx1230.c
libraries/u8g2/csrc/u8g2_intersection.c
libraries/u8g2/csrc/u8x8_d_ist3088.c
libraries/u8g2/csrc/u8x8_d_uc1601.c
libraries/u8g2/csrc/u8x8_d_ssd1317.c
libraries/u8g2/csrc/u8x8_d_st7571.c
libraries/u8g2/csrc/u8g2_message.c
libraries/u8g2/csrc/u8x8_d_st7511.c
libraries/u8g2/csrc/u8x8_u16toa.c
libraries/u8g2/csrc/u8x8_d_ssd1316.c
libraries/u8g2/csrc/u8x8_d_pcf8812.c
libraries/u8g2/csrc/u8g2_circle.c
libraries/u8g2/csrc/u8x8_d_ssd1325.c
libraries/u8g2/csrc/u8x8_gpio.c
libraries/u8g2/csrc/u8x8_d_ist7920.c
libraries/u8g2/csrc/u8x8_d_st7586s_s028hn118a.c
libraries/u8g2/csrc/u8x8_d_ist3020.c
libraries/u8g2/csrc/u8log.c
libraries/u8g2/csrc/u8x8_d_st7528.c
libraries/u8g2/csrc/u8g2_cleardisplay.c
libraries/u8g2/csrc/u8x8_d_sh1107.c
libraries/u8g2/csrc/u8x8_d_uc1638.c
libraries/u8g2/csrc/u8x8_d_ssd1306_64x32.c
libraries/u8g2/csrc/u8x8_d_ssd1322.c
libraries/u8g2/csrc/u8x8_d_ssd1305.c
libraries/u8g2/csrc/u8x8_d_ssd1606_172x72.c
libraries/u8g2/csrc/u8x8_input_value.c
libraries/u8g2/csrc/u8x8_d_sh1122.c
libraries/u8g2/csrc/u8x8_d_uc1609.c
libraries/u8g2/csrc/u8x8_d_lc7981.c
libraries/u8g2/csrc/u8x8_selection_list.c
libraries/u8g2/csrc/u8x8_d_st7920.c
libraries/u8g2/csrc/u8x8_d_sed1330.c
libraries/u8g2/csrc/u8x8_d_st7586s_jlx320160.c
libraries/u8g2/csrc/u8x8_d_ssd1326.c
libraries/u8g2/csrc/u8x8_d_ssd1306_48x64.c
libraries/u8g2/csrc/u8x8_d_ls013b7dh03.c
libraries/u8g2/csrc/u8x8_d_ssd1306_72x40.c
libraries/u8g2/csrc/u8x8_d_ks0108.c
libraries/u8g2/csrc/u8x8_d_ssd1607_200x200.c
libraries/u8g2/csrc/mui.c
libraries/u8g2/csrc/u8x8_d_a2printer.c
libraries/u8g2/csrc/u8x8_d_uc1611.c
libraries/u8g2/csrc/u8x8_d_uc1610.c
libraries/u8g2/csrc/u8g2_line.c
libraries/u8g2/csrc/u8g2_box.c
libraries/u8g2/csrc/u8x8_d_ssd1362.c
libraries/u8g2/csrc/u8g2_button.c
libraries/u8g2/csrc/u8g2_setup.c
libraries/u8g2/csrc/u8x8_d_ssd1312.c
libraries/u8g2/csrc/u8x8_fonts.c
libraries/u8g2/csrc/u8x8_d_st75160.c
libraries/u8g2/csrc/u8x8_d_gu800.c
libraries/u8g2/csrc/u8x8_message.c
libraries/u8g2/csrc/u8x8_d_s1d15e06.c
libraries/u8g2/csrc/u8x8_d_ssd1306_128x32.c
libraries/u8g2/csrc/u8x8_d_ssd1320.c
libraries/u8g2/csrc/u8x8_d_gp1287ai.c
libraries/u8g2/csrc/u8g2_polygon.c
libraries/u8g2/csrc/u8x8_d_st7565.c
libraries/u8g2/csrc/u8x8_d_ssd1306_96x16.c
libraries/u8g2/csrc/u8g2_buffer.c
libraries/u8g2/csrc/u8x8_d_max7219.c
libraries/u8g2/csrc/u8g2_input_value.c
libraries/u8g2/csrc/u8x8_d_st7539.c
libraries/u8g2/csrc/u8g2_ll_hvline.c
libraries/u8g2/csrc/u8g2_fonts.c
libraries/u8g2/csrc/u8x8_byte.c
libraries/u8g2/csrc/u8x8_d_sbn1661.c
libraries/u8g2/csrc/u8log_u8g2.c
libraries/u8g2/csrc/u8x8_d_hd44102.c
libraries/u8g2/csrc/u8x8_d_st7586s_ymc240160.c
libraries/u8g2/csrc/u8g2_d_memory.c
libraries/u8g2/csrc/u8x8_d_st7586s_jlx384160.c
libraries/u8g2/csrc/u8x8_cad.c
libraries/u8g2/csrc/u8log_u8x8.c
libraries/u8g2/csrc/u8g2_font.c
libraries/u8g2/csrc/u8x8_d_gp1294ai.c
libraries/u8g2/csrc/u8x8_d_st7586s_erc240160.c
libraries/u8g2/csrc/u8x8_d_s1d15300.c
libraries/u8g2/csrc/u8x8_d_il3820_296x128.c
libraries/u8g2/csrc/u8x8_string.c
libraries/u8g2/csrc/u8x8_d_sh1106_64x32.c
libraries/u8g2/csrc/u8g2_d_setup.c
libraries/u8g2/csrc/u8x8_d_sh1108.c
libraries/u8g2/csrc/u8g2_hvline.c
libraries/u8g2/csrc/u8x8_display.c
libraries/u8g2/cppsrc/U8x8lib.cpp
libraries/u8g2/cppsrc/U8g2lib.cpp
#libraries/DS3231/DS3231.cpp
libraries/RTClib-master/src/RTC_DS1307.cpp
libraries/RTClib-master/src/RTClib.cpp
libraries/RTClib-master/src/RTC_DS3231.cpp
#libraries/NTPClient/NTPClient.cpp
libraries/ESPNtpClient/src/ESPNtpClient.cpp
libraries/EspSoftwareSerial/src/SoftwareSerial.cpp
libraries/USB_Host_Shield_2.0-1.7.0/AMBX.cpp
libraries/USB_Host_Shield_2.0-1.7.0/XBOXUSB.cpp
libraries/USB_Host_Shield_2.0-1.7.0/BTD.cpp
libraries/USB_Host_Shield_2.0-1.7.0/adk.cpp
libraries/USB_Host_Shield_2.0-1.7.0/BTHID.cpp
libraries/USB_Host_Shield_2.0-1.7.0/cdc_XR21B1411.cpp
libraries/USB_Host_Shield_2.0-1.7.0/MiniDSP.cpp
libraries/USB_Host_Shield_2.0-1.7.0/cdcacm.cpp
libraries/USB_Host_Shield_2.0-1.7.0/PS3BT.cpp
libraries/USB_Host_Shield_2.0-1.7.0/cdcftdi.cpp
libraries/USB_Host_Shield_2.0-1.7.0/PS3USB.cpp
libraries/USB_Host_Shield_2.0-1.7.0/cdcprolific.cpp
libraries/USB_Host_Shield_2.0-1.7.0/PS4Parser.cpp
libraries/USB_Host_Shield_2.0-1.7.0/hidboot.cpp
libraries/USB_Host_Shield_2.0-1.7.0/PS5Parser.cpp
libraries/USB_Host_Shield_2.0-1.7.0/hidcomposite.cpp
libraries/USB_Host_Shield_2.0-1.7.0/PS5Trigger.cpp
libraries/USB_Host_Shield_2.0-1.7.0/hidescriptorparser.cpp
libraries/USB_Host_Shield_2.0-1.7.0/PSBuzz.cpp
libraries/USB_Host_Shield_2.0-1.7.0/hiduniversal.cpp
libraries/USB_Host_Shield_2.0-1.7.0/SPP.cpp
libraries/USB_Host_Shield_2.0-1.7.0/hidusagetitlearrays.cpp
libraries/USB_Host_Shield_2.0-1.7.0/SwitchProParser.cpp
libraries/USB_Host_Shield_2.0-1.7.0/masstorage.cpp
libraries/USB_Host_Shield_2.0-1.7.0/UHS2_gpio.cpp
libraries/USB_Host_Shield_2.0-1.7.0/max_LCD.cpp
libraries/USB_Host_Shield_2.0-1.7.0/Usb.cpp
libraries/USB_Host_Shield_2.0-1.7.0/message.cpp
libraries/USB_Host_Shield_2.0-1.7.0/Wii.cpp
libraries/USB_Host_Shield_2.0-1.7.0/parsetools.cpp
libraries/USB_Host_Shield_2.0-1.7.0/XBOXOLD.cpp
libraries/USB_Host_Shield_2.0-1.7.0/usbh_midi.cpp
libraries/USB_Host_Shield_2.0-1.7.0/XBOXONE.cpp
libraries/USB_Host_Shield_2.0-1.7.0/usbhid.cpp
libraries/USB_Host_Shield_2.0-1.7.0/XBOXONESParser.cpp
libraries/USB_Host_Shield_2.0-1.7.0/usbhub.cpp
libraries/USB_Host_Shield_2.0-1.7.0/XBOXRECV.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/AMBX.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/BTD.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/BTHID.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/MiniDSP.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/PS3BT.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/PS3USB.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/PS4Parser.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/PS5Parser.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/PS5Trigger.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/PSBuzz.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/SPP.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/SwitchProParser.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/UHS2_gpio.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/Usb.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/Wii.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/XBOXOLD.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/XBOXONE.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/XBOXONESParser.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/XBOXRECV.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/XBOXUSB.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/adk.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/cdc_XR21B1411.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/cdcacm.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/cdcftdi.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/cdcprolific.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/hidboot.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/hidcomposite.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/hidescriptorparser.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/hiduniversal.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/hidusagetitlearrays.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/masstorage.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/max_LCD.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/message.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/parsetools.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/usbh_midi.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/usbhid.cpp
#libraries/USB_Host_Shield_Library_2_0-1.6.2/usbhub.cpp
##libraries/USB_Host_Shield_Library_2_0-1.6.2/cdc_ch34x.cpp
libraries/Adafruit-MCP23017-Arduino-Library-master/src/Adafruit_MCP23X08.cpp
libraries/Adafruit-MCP23017-Arduino-Library-master/src/Adafruit_MCP23X17.cpp
libraries/Adafruit-MCP23017-Arduino-Library-master/src/Adafruit_MCP23XXX.cpp
libraries/Adafruit_BusIO/Adafruit_BusIO_Register.cpp
libraries/Adafruit_BusIO/Adafruit_SPIDevice.cpp
libraries/Adafruit_BusIO/Adafruit_I2CDevice.cpp
libraries/maidenhead-master/maidenhead.c
#libraries/AsyncTCP-master/src/AsyncTCP.cpp
libraries/AsyncTCP/src/AsyncTCP.cpp
libraries/esp-multiap/espwmap.cpp
libraries/ESP32_FTPServer_SD/ESP32FtpServer.cpp
libraries/ch9350ifLibrary/src/ch9350if.cpp
libraries/ESPAsyncWebServer/src/AsyncEventSource.cpp
libraries/ESPAsyncWebServer/src/WebHandlers.cpp
libraries/ESPAsyncWebServer/src/AsyncWebSocket.cpp
libraries/ESPAsyncWebServer/src/WebRequest.cpp
libraries/ESPAsyncWebServer/src/SPIFFSEditor.cpp
libraries/ESPAsyncWebServer/src/WebResponses.cpp
libraries/ESPAsyncWebServer/src/WebAuthentication.cpp
libraries/ESPAsyncWebServer/src/WebServer.cpp
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/webusb/Adafruit_USBD_WebUSB.cpp
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/ports/nrf/Adafruit_TinyUSB_nrf.cpp
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/ports/ch32/Adafruit_TinyUSB_ch32.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/ports/esp32/Adafruit_TinyUSB_esp32.cpp
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/ports/rp2040/Adafruit_TinyUSB_rp2040.cpp
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/ports/samd/Adafruit_TinyUSB_samd.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/hid/Adafruit_USBD_HID.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/video/Adafruit_USBD_Video.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/Adafruit_USBD_Device.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/Adafruit_USBD_CDC.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/Adafruit_TinyUSB_API.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/Adafruit_USBD_Interface.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/cdc/Adafruit_USBH_CDC.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/midi/Adafruit_USBD_MIDI.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/Adafruit_USBH_Host.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/msc/Adafruit_USBD_MSC.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/arduino/msc/Adafruit_USBH_MSC.cpp
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/device/usbd.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/device/usbd_control.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/tusb.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/audio/audio_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/net/ecm_rndis_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/net/ncm_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/hid/hid_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/hid/hid_host.c
#llibraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/usbtmc/usbtmc_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/dfu/dfu_rt_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/dfu/dfu_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/video/video_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/bth/bth_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/vendor/vendor_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/cdc/cdc_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/cdc/cdc_host.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/midi/midi_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/msc/msc_device.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/class/msc/msc_host.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/wch/dcd_ch32_usbhs.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/wch/dcd_ch32_usbfs.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/analog/max3421/hcd_max3421.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/synopsys/dwc2/dwc2_common.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/synopsys/dwc2/hcd_dwc2.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/synopsys/dwc2/dcd_dwc2.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/nordic/nrf5x/dcd_nrf5x.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/microchip/samd/dcd_samd.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/raspberrypi/rp2040/dcd_rp2040.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/raspberrypi/rp2040/rp2040_usb.c
#libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/portable/raspberrypi/rp2040/hcd_rp2040.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/common/tusb_fifo.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/host/usbh.c
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/host/hub.c
  )

set(BLE_SRCS
  libraries/BLE/src/BLE2902.cpp
  libraries/BLE/src/BLE2904.cpp
  libraries/BLE/src/BLEAddress.cpp
  libraries/BLE/src/BLEAdvertisedDevice.cpp
  libraries/BLE/src/BLEAdvertising.cpp
  libraries/BLE/src/BLEBeacon.cpp
  libraries/BLE/src/BLECharacteristic.cpp
  libraries/BLE/src/BLECharacteristicMap.cpp
  libraries/BLE/src/BLEClient.cpp
  libraries/BLE/src/BLEDescriptor.cpp
  libraries/BLE/src/BLEDescriptorMap.cpp
  libraries/BLE/src/BLEDevice.cpp
  libraries/BLE/src/BLEEddystoneTLM.cpp
  libraries/BLE/src/BLEEddystoneURL.cpp
  libraries/BLE/src/BLEExceptions.cpp
  libraries/BLE/src/BLEHIDDevice.cpp
  libraries/BLE/src/BLERemoteCharacteristic.cpp
  libraries/BLE/src/BLERemoteDescriptor.cpp
  libraries/BLE/src/BLERemoteService.cpp
  libraries/BLE/src/BLEScan.cpp
  libraries/BLE/src/BLESecurity.cpp
  libraries/BLE/src/BLEServer.cpp
  libraries/BLE/src/BLEService.cpp
  libraries/BLE/src/BLEServiceMap.cpp
  libraries/BLE/src/BLEUtils.cpp
  libraries/BLE/src/BLEUUID.cpp
  libraries/BLE/src/BLEValue.cpp
  libraries/BLE/src/FreeRTOS.cpp
  libraries/BLE/src/GeneralUtils.cpp
  )

set(includedirs
  variants/${CONFIG_ARDUINO_VARIANT}/
  cores/esp32/
  libraries/ArduinoOTA/src
  libraries/AsyncUDP/src
  libraries/BLE/src
  libraries/BluetoothSerial/src
  libraries/DNSServer/src
  libraries/EEPROM/src
  libraries/ESP32/src
  libraries/ESPmDNS/src
  libraries/Ethernet/src
  libraries/FFat/src
  libraries/FS/src
  libraries/HTTPClient/src
  libraries/HTTPUpdate/src
  libraries/LittleFS/src
  libraries/Insights/src
  libraries/I2S/src
  libraries/NetBIOS/src
  libraries/Preferences/src
  libraries/RainMaker/src
  libraries/SD_MMC/src
  libraries/SD/src
  libraries/SimpleBLE/src
  libraries/SPIFFS/src
  libraries/SPI/src
  libraries/Ticker/src
  libraries/Update/src
  libraries/USB/src
  libraries/WebServer/src
  libraries/WiFiClientSecure/src
  libraries/WiFi/src
  libraries/WiFiProv/src
  libraries/Wire/src
libraries/u8g2/csrc/
libraries/u8g2/cppsrc/
#libraries/DS3231/
#libraries/NTPClient/
libraries/ESPNtpClient/src
libraries/EspSoftwareSerial/src/
#libraries/USB_Host_Shield_Library_2_0-1.6.2/
libraries/USB_Host_Shield_2.0-1.7.0/
libraries/Adafruit-MCP23017-Arduino-Library-master/src/
libraries/Adafruit_BusIO/
libraries/maidenhead-master/
libraries/RTClib-master/src/
#libraries/AsyncTCP-master/src/
libraries/AsyncTCP/src/
libraries/esp-multiap/
libraries/ESP32_FTPServer_SD/
libraries/ch9350ifLibrary/src/
libraries/ESPAsyncWebServer/src/
#libraries/SdFat-2.2.3/src/
libraries/Adafruit_TinyUSB_Arduino-3.4.4/src/
  )

set(srcs ${CORE_SRCS} ${LIBRARY_SRCS} ${BLE_SRCS})
set(priv_includes cores/esp32/libb64)
set(requires spi_flash mbedtls mdns esp_adc_cal wifi_provisioning nghttp wpa_supplicant)
set(priv_requires fatfs nvs_flash app_update spiffs bootloader_support openssl bt esp_ipc esp_hid)

idf_component_register(INCLUDE_DIRS ${includedirs} PRIV_INCLUDE_DIRS ${priv_includes} SRCS ${srcs} REQUIRES ${requires} PRIV_REQUIRES ${priv_requires})

if(NOT CONFIG_FREERTOS_HZ EQUAL 1000 AND NOT "$ENV{ARDUINO_SKIP_TICK_CHECK}")
    # See delay() in cores/esp32/esp32-hal-misc.c.
    message(FATAL_ERROR "esp32-arduino requires CONFIG_FREERTOS_HZ=1000 "
                        "(currently ${CONFIG_FREERTOS_HZ})")
endif()

string(TOUPPER ${CONFIG_ARDUINO_VARIANT} idf_target_caps)
string(REPLACE "-" "_" idf_target_for_macro "${idf_target_caps}")
target_compile_options(${COMPONENT_TARGET} PUBLIC
    -DARDUINO=10812
    -DARDUINO_${idf_target_for_macro}_DEV
    -DARDUINO_ARCH_ESP32
    -DARDUINO_BOARD="${idf_target_caps}_DEV"
    -DARDUINO_VARIANT="${CONFIG_ARDUINO_VARIANT}"
    -DESP32)

if(CONFIG_AUTOSTART_ARDUINO)
    # in autostart mode, arduino-esp32 contains app_main() function and needs to
    # reference setup() and loop() in the main component. If we add main
    # component to priv_requires then we create a large circular dependency
    # (arduino-esp32 -> main -> arduino-esp32) and can get linker errors, so
    # instead we add setup() and loop() to the undefined symbols list so the
    # linker will always include them.
    #
    # (As they are C++ symbol, we need to add the C++ mangled names.)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u _Z5setupv -u _Z4loopv")
endif()

# This function adds a dependency on the given component if the component is included into the build.
function(maybe_add_component component_name)
    idf_build_get_property(components BUILD_COMPONENTS)
    if (${component_name} IN_LIST components)
        idf_component_get_property(lib_name ${component_name} COMPONENT_LIB)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${lib_name})
    endif()
endfunction()

maybe_add_component(esp-dsp)

if(CONFIG_ESP_INSIGHTS_ENABLED)
    maybe_add_component(esp_insights)
endif()
if(CONFIG_ESP_RMAKER_WORK_QUEUE_TASK_STACK)
    maybe_add_component(esp_rainmaker)
    maybe_add_component(qrcode)
endif()
if(IDF_TARGET MATCHES "esp32s2|esp32s3" AND CONFIG_TINYUSB_ENABLED)
    maybe_add_component(arduino_tinyusb)
endif()
if(NOT CONFIG_ARDUINO_SELECTIVE_COMPILATION OR CONFIG_ARDUINO_SELECTIVE_ArduinoOTA)
    maybe_add_component(esp_https_ota)
endif()
if(NOT CONFIG_ARDUINO_SELECTIVE_COMPILATION OR CONFIG_ARDUINO_SELECTIVE_LITTLEFS)
    maybe_add_component(esp_littlefs)
endif()
