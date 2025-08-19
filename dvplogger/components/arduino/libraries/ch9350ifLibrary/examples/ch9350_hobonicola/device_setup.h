/**
  device_setup.h  device(board) defs and funcs of "Hobo-nicola usb/ble adapter".
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

#include "arduino.h"
#ifndef __hobo_device_setup_h__
#define __hobo_device_setup_h__

#if !defined(USE_SERIAL1)
	#define USE_SERIAL1 0
#endif

#if !defined (USE_SPI)
	#define USE_SPI 1
#endif

#if defined(BLE_LED)
#undef BLE_LED
#endif

#if defined(ARDUINO_AVR_PROMICRO) || defined(ARDUINO_AVR_LEONARDO)
#include <avr/wdt.h>
	#ifndef PRTIM4
		#define PRTIM4 4
	#endif
	#define LED1 LED_BUILTIN_RX
	#define LED2 LED_BUILTIN_TX
	#define LED_OFF	1
#endif

// QTPy-m0 にはふつうのLEDが載ってないので、自分で用意する。
#if defined(ADAFRUIT_QTPY_M0)
	#define PIN_NEOPIXEL_VCC 12
	#define LED1 0
	#define LED2 1
	#define LED_OFF	1
#endif  

#if defined(ARDUINO_Seeed_XIAO_nRF52840)
//  #define PIN_NEOPIXEL_VCC 33
  #define LED1 LED_GREEN
  #define LED2 LED_RED
  #define LED_OFF 1
  #define BLE_LED LED_BLUE 
#endif  

// XIAO-m0 には3つのLEDがある。
// オンボードのLEDでもいいし、自分で用意したものでもよい。
#if defined(ARDUINO_SEEED_XIAO_M0)
	#define LED1 LED_BUILTIN
	#define LED2 PIN_LED2
//	#define LED1 0
//	#define LED2 1
	#define LED_OFF	1
#endif

// ISP1807-MB 用のLEDは自分で用意する。
#if defined(ARDUINO_SSCI_ISP1807_MICRO_BOARD)
//	#define LED1 19
  #define LED1 PIN_LED1 // とりあえずオンボードのLEDにしておく。
	#define LED2 20
  #define LED_OFF 1
  #define BLE_LED 18 
#endif  
#define LED_ON	(LED_OFF ^ 1)

#if defined(BLE_LED)
static const unsigned long bleled_interval = 50;
unsigned long bleled_timing = 0;
static const int16_t bleled_delta = 8;
static const int16_t bleled_max = 256; 
static const int16_t bleled_min = 128;

int16_t dir = -1;
int16_t bleled_value = bleled_min;

void ble_led() {
  if (!is_ble_connected()) {
    if (bleled_value != bleled_max) {
      bleled_value = bleled_max;
      analogWrite(BLE_LED, bleled_max);
      // digitalWrite(BLE_LED, HIGH);  // 一度analogWrite()するとdigitalWrite()で消えないような。
    }
    return;
  }
  unsigned long now = millis();
  if (bleled_timing == 0)
    bleled_timing = now;
  else if (now - bleled_timing > bleled_interval) {
    analogWrite(BLE_LED, bleled_value);
    bleled_timing = now;
    bleled_value += (bleled_delta * dir);
    if (bleled_value >= bleled_max)
      dir = -1;
    else if (bleled_value <= bleled_min)
      dir = 1;
  }
}
#else
#define ble_led()
#endif

void hobo_device_setup() {
  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);

#if defined(ADAFRUIT_QTPY_M0)	
  pinMode(PIN_NEOPIXEL_VCC, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_VCC, 0);	// NeoPixelは常時オフ。
#endif

//
// QTPy-m0   Serial1 : SERCOM0, SPI : SERCOM2
// XIAO-m0   Serial1 : SERCOM4, SPI : SERCOM0

// samd21
#if defined (ADAFRUIT_QTPY_M0) || defined(ARDUINO_SEEED_XIAO_M0)
  uint32_t pm = PM_APBCMASK_ADC | PM_APBCMASK_AC | PM_APBCMASK_DAC | PM_APBCMASK_I2S | PM_APBCMASK_PTC |
  		PM_APBCMASK_TCC0 | PM_APBCMASK_TCC1 | PM_APBCMASK_TCC2 | 
			PM_APBCMASK_TC3 | PM_APBCMASK_TC4 | PM_APBCMASK_TC5 | PM_APBCMASK_TC6 | PM_APBCMASK_TC7;

	#if defined(ADAFRUIT_QTPY_M0)
    pm |= PM_APBCMASK_SERCOM1 | PM_APBCMASK_SERCOM3 | PM_APBCMASK_SERCOM4  | PM_APBCMASK_SERCOM5;
		#if !USE_SERIAL1
			pm |= PM_APBCMASK_SERCOM0;	// Serial1を使わない
		#endif
		#if !USE_SPI
			pm |= PM_APBCMASK_SERCOM2;	// SPIを使わない。
		#endif
	#else // XIAO-m0
		pm |= PM_APBCMASK_SERCOM1 | PM_APBCMASK_SERCOM2 | PM_APBCMASK_SERCOM3 | PM_APBCMASK_SERCOM5;
		#if !USE_SERIAL1
			pm |= PM_APBCMASK_SERCOM4;	// Serial1を使わない
		#endif
		#if !USE_SPI
			pm |= PM_APBCMASK_SERCOM0;	// SPIを使わない。
		#endif
	#endif
	PM->APBCMASK.reg &= ~pm;

#elif defined(NRF52_SERIES)
  pinMode(BLE_LED, OUTPUT);
  digitalWrite(BLE_LED, LED_OFF); 
#elif defined(ARDUINO_AVR_PROMICRO) || defined(ARDUINO_AVR_LEONARDO)
	cli();
	wdt_disable();
  ADCSRA = ADCSRA & 0x7f;
  ACSR = ACSR | 0x80;
  #if USE_SPI
    PRR0 = _BV(PRTWI) | _BV(PRTIM1) | _BV(PRADC);	
  #else
    PRR0 = _BV(PRTWI) | _BV(PRTIM1) | _BV(PRADC) | _BV(PRSPI);  
  #endif
  #if USE_SERIAL1
  	PRR1 = _BV(PRTIM4) | _BV(PRTIM3);
  #else
    PRR1 = _BV(PRTIM4) | _BV(PRTIM3) | _BV(PRUSART1); 
  #endif
	sei();
#else
#error "Unsupported board."
#endif
#endif // __hobo_device_setup_h__
}
