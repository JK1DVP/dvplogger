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
// iambic_keyer.cpp Eiichiro Araki 2024/9/27
// modified for jk1dvplog

// Copyright (c) 2009 Steven T. Elliott, K1EL
// Copyright (c) 2023 Michael Babineau, VE3WMB (mbabineau.ve3wmb@gmail.com)
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <Arduino.h>
#include "iambic_keyer.h"
#include "cw_keying.h"
#include "Ticker.h"

struct paddle_queue paddle_queue;
QueueHandle_t xQueuePaddle;

Ticker iambic_keyer;

/////////////////////////////////////////////////////////////////////////////////////////
//
//  g_keyerControl bit definitions
//
////////////////////////////////////////////////////////////////////////////////////////
#define DIT_L 0x01     // Dit latch (Binary 0000 0001)
#define DAH_L 0x02     // Dah latch (Binary 0000 0010)
#define DIT_PROC 0x04  // Dit is being processed (Binary 0000 0100)
#define IAMBIC_B 0x10  // 0x00 for Iambic A, 0x10 for Iambic B (Binary 0001 0000)
#define IAMBIC_A 0

////////////////////////////////////////////////////////////////////////////////////////
//
//  Morse Iambic Keyer State Machine type
//
///////////////////////////////////////////////////////////////////////////////////////
// Keyer State machine type - states for sending of Morse elements based on paddle input
enum KSTYPE {
  IDLE,
  CHK_DIT,
  CHK_DAH,
  KEYED_PREP,
  KEYED,
  INTER_ELEMENT,
  PRE_IDLE,
};


// Keyer Command State type - states for processing of Commands entered via the paddles after entering Command Mode
enum KEYER_CMD_STATE_TYPE {
  CMD_IDLE,
  CMD_ENTER,
  CMD_TUNE,
  CMD_SPEED_WAIT_D1,
  CMD_SPEED_WAIT_D2,

};

// Data type for storing Keyer settings in EEPROM
struct KeyerConfigType {
  uint32_t ms_per_dit;         // Speed
  uint8_t dit_paddle_pin;      // Dit paddle pin number
  uint8_t dah_paddle_pin;      // Dah Paddle pin number
  uint8_t iambic_keying_mode;  // 0 - Iambic-A, 1 Iambic-B
  uint8_t sidetone_is_muted;   //
  uint32_t num_writes;         // Number of writes to EEPROM, incremented on each 'W' command
  uint32_t data_checksum;      // Checksum containing a 32-bit sum of all of the other fields
};


////////////////////////////////////////////////////////////////////////////////////////
//
//  Global Variables
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// I do recognized that this is quite a lot of global variables for such a small program
// and many would frown upon this. In general it is not great design practice but it was
// easiest to start out this way when prototyping the code and at the moment I can't be bothered
// to restructure the code to implement proper data-hiding since it is working quite well and I don't want
// to break it. For now I have prefixed the globals with g_ as a reminder to be careful of what code is changing these.
// Perhaps in a future iteration of code I will come back to address this.  - VE3WMB
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int paddle_setting=0; // bit 0 reverse  bit 1 iambic A(1)/B(0)

int paddle_voltage=0 ;  // paddle input voltage copy
uint32_t g_ditTime;           // Number of milliseconds per dit
uint8_t g_keyerControl;       // 0x1 Dit latch, 0x2 Dah latch, 0x04 Dit being processed, 0x10 for Iambic A or 1 for B
KSTYPE g_keyerState = IDLE;   // Keyer state global variable
bool g_sidetone_muted;  // If 'true' the Piezo speaker output is muted except for when in command mode


// We declare these as variables so that DIT and DAH paddles can be swapped for both left and right hand users. Convention is DIT on thumb.
uint8_t g_dit_paddle;  // Current PIN number for Dit paddle
uint8_t g_dah_paddle;  // Current PIN number for Dah paddle

// We use this variable to encode Morse elements (DIT = 0, DAH =1) to capture the current character sent via the paddles which is needed for user commands.
// The encoding practice is to left pad the character with 1's. A zero start bit is to the left of the first encoded element.
// We start with B11111110 and shift left with a 0 or 1 from the least significant bit, according the last Morse element received (DIT = 0, DAH =1).
uint8_t g_current_morse_character = B11111110;

// Keyer Command Mode Variables
uint32_t g_cmd_mode_input_timeout = 0;                  // Maximum wait time after hitting the command button with no paddle input before Command Mode auto-exit
KEYER_CMD_STATE_TYPE g_keyer_command_state = CMD_IDLE;  // This is the state variable for the Command Mode state machine

#define LEFT_PADDLE 2
#define RIGHT_PADDLE 1


void set_paddle()
{
  if (paddle_setting&1) {
    reverse_paddle();
  } else {
    normal_paddle();
  }
  
  if (paddle_setting&2) {
    g_keyerControl = IAMBIC_A;
  } else {
    g_keyerControl = IAMBIC_B;   // Make Iambic B the default mode
  }
  
}

void paddle_iambic_a()
{
  g_keyerControl&=~IAMBIC_B;
}

void paddle_iambic_b()
{
  g_keyerControl|=IAMBIC_B;
}

void reverse_paddle()
{
  g_dit_paddle = RIGHT_PADDLE;   // Dits on right hand thumb
  g_dah_paddle = LEFT_PADDLE;  // Dahs on right hand index finger
}

void normal_paddle()
{
  g_dit_paddle = LEFT_PADDLE;   // Dits on right hand thumb
  g_dah_paddle = RIGHT_PADDLE;  // Dahs on right hand index finger
}

///////////////////////////////////////////////////////////////////////////////
//
//  System Initialization - this runs on power up
//
///////////////////////////////////////////////////////////////////////////////

void init_iambic_keyer() {
  // setting based on paddle_setting variable

  g_keyerState = IDLE;



  loadWPM(cw_spd); // cw_spd is important to determine paddle action
  
  // Setup for Righthanded paddle by default

  
  //  g_dit_paddle = LEFT_PADDLE;   // Dits on right hand thumb
  //  g_dah_paddle = RIGHT_PADDLE;  // Dahs on right hand index finger

  g_sidetone_muted = true;  // Default to no sidetone

  set_paddle();

  init_paddle_input();


  xQueuePaddle = xQueueCreate(QUEUE_PADDLE_LEN, sizeof(struct paddle_queue));

  
  // attach interrupt handler
  iambic_keyer.attach_ms(1, iambic_keyer_handler);
  
}







///////////////////////////////////////////////////////////////////////////////
//
//  Main Work Loop
//  called from Ticker service
//
///////////////////////////////////////////////////////////////////////////////

void iambic_keyer_handler() {

  static long ktimer;

  int paddle;


  while (1) {
  // This state machine translates paddle input into DITS and DAHs and keys the transmitter.
  paddle= get_Paddle();

  // report paddle status
  paddle_queue.paddle=paddle;
  paddle_queue.voltage=paddle_voltage;
  xQueueSend(xQueuePaddle, &paddle_queue, 0);
  
  switch (g_keyerState) {
    case IDLE:
      // Wait for direct or latched paddle press
      if ((paddle & g_dit_paddle) || (paddle & g_dah_paddle) || (g_keyerControl & 0x03)) {
        update_PaddleLatch(paddle);
        g_current_morse_character = B11111110;  // Sarting point for accumlating a character input via paddles
        g_keyerState = CHK_DIT;
      } else {
	return ;
      }
      break;
    case CHK_DIT:
      // See if the dit paddle was pressed
      if (g_keyerControl & DIT_L) {
        g_keyerControl |= DIT_PROC;
        ktimer = g_ditTime;
        g_current_morse_character = (g_current_morse_character << 1);  // Shift a DIT (0) into the bit #0 position.
        g_keyerState = KEYED_PREP;
      } else {
        g_keyerState = CHK_DAH;
      }
      break;

    case CHK_DAH:
      // See if dah paddle was pressed
      if (g_keyerControl & DAH_L) {
        ktimer = g_ditTime * 3;
        g_current_morse_character = ((g_current_morse_character << 1) | 1);  // Shift left one position and make bit #0 a DAH (1)
        g_keyerState = KEYED_PREP;
      } else {
        ktimer = millis() + (g_ditTime * 2);  // Character space, is g_ditTime x 2 because already have a trailing intercharacter space
        g_keyerState = PRE_IDLE;
      }
      break;

    case KEYED_PREP:
      // import wpm value here
      loadWPM(cw_spd);
      // Assert key down, start timing, state shared for dit or dah
      tx_key_down();
      ktimer += millis();                  // set ktimer to interval end time
      g_keyerControl &= ~(DIT_L + DAH_L);  // clear both paddle latch bits
      g_keyerState = KEYED;                // next state
      //      break;
      return;

    case KEYED:
      // Wait for timer to expire
      if (millis() > ktimer) {  // are we at end of key down ?
        tx_key_up();

        ktimer = millis() + g_ditTime;  // inter-element time
        g_keyerState = INTER_ELEMENT;   // next state
	
      } else if (g_keyerControl & IAMBIC_B) {
        update_PaddleLatch(paddle);  // early paddle latch check in Iambic B mode
	return;
      } else {
	return;
      }
      break;

    case INTER_ELEMENT:
      // Insert time between dits/dahs
      update_PaddleLatch(paddle);                       // latch paddle state
      if (millis() > ktimer) {                    // are we at end of inter-space ?
        if (g_keyerControl & DIT_PROC) {          // was it a dit or dah ?
          g_keyerControl &= ~(DIT_L + DIT_PROC);  // clear two bits
          g_keyerState = CHK_DAH;                 // dit done, check for dah
        } else {
          g_keyerControl &= ~(DAH_L);           // clear dah latch
          ktimer = millis() + (g_ditTime * 2);  // Character space, is g_ditTime x 2 because already have a trailing intercharacter space
          g_keyerState = PRE_IDLE;              // go idle
        }
	break;
      }
      //      break;
      return;

    case PRE_IDLE:  // Wait for an intercharacter space
      // Check for direct or latched paddle press
      if ((paddle&g_dit_paddle) || (paddle&g_dah_paddle)  || (g_keyerControl & 0x03)) {
        update_PaddleLatch(paddle);
        g_keyerState = CHK_DIT;
      } else {                    // Check for intercharacter space
        if (millis() > ktimer) {  // We have a character
          //Serial.println(g_current_morse_character);
	  // set current 
	  cw_send_current=morse_code_char(g_current_morse_character);
	  cw_send_update=1;
	  
          g_keyerState = IDLE;  // go idle
          if (g_keyer_command_state != CMD_IDLE) {
	    //            process_keyer_command(g_current_morse_character, g_ditTime);

          }
	  break;	    	  
        }
	return;

      }
      break;
  }
  }

} // end of Loop 


///////////////////////////////////////////////////////////////////////////////
//
//    Latch dit and/or dah press
//
///////////////////////////////////////////////////////////////////////////////



#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"

#define DEFAULT_VREF    3900        //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   1          //Multisampling

static esp_adc_cal_characteristics_t *adc_chars;
static const adc_channel_t channel = ADC_CHANNEL_3;    
static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
static const adc_atten_t atten = ADC_ATTEN_DB_12;
static const adc_unit_t unit = ADC_UNIT_1;


static void check_efuse(void)
{
    //Check if TP is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) {
        printf("eFuse Two Point: Supported\n");
    } else {
        printf("eFuse Two Point: NOT supported\n");
    }
    //Check Vref is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) {
        printf("eFuse Vref: Supported\n");
    } else {
        printf("eFuse Vref: NOT supported\n");
    }
}


static void print_char_val_type(esp_adc_cal_value_t val_type)
{
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        printf("Characterized using Two Point Value\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        printf("Characterized using eFuse Vref\n");
    } else {
        printf("Characterized using Default Vref\n");
    }
}

// paddle connection
// (pin1)          adc1-3 (VN pin4 DEVKITC) // 0.1uF   GND J3-1
//  3.3V ---- 10k --+-+--- 20k ---  tip (dit?) 
//                    +--- 10k --  ring (dah?)
//  gnd  -------------------------  sleeve
//  so status is  3300 mV  open
//                2200     dit   1  dah  0
//                1650     dit   0  dah  1
//                1320     dit   1  dah  1


void init_paddle_input(void)
{
    //Check if Two Point or Vref are burned into eFuse
    check_efuse();

    //Configure ADC
    adc1_config_width(width);
    adc1_config_channel_atten((adc1_channel_t) channel, atten);


    // Characterize ADC
    adc_chars = (esp_adc_cal_characteristics_t*) calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);
}

int get_Paddle() {
  static int prev_voltage=0;
  static int prev_paddle=0;
  int tmp;
  uint32_t adc_reading = 0;
  //Multisampling
  for (int i = 0; i < NO_OF_SAMPLES; i++) {
    if (unit == ADC_UNIT_1) {
      adc_reading += adc1_get_raw((adc1_channel_t)channel);
    } else {
      int raw;
      adc2_get_raw((adc2_channel_t)channel, width, &raw);
      adc_reading += raw;
    }
  }
  adc_reading /= NO_OF_SAMPLES;
  //Convert adc_reading to voltage in mV
  uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);

  tmp=(int)voltage;
  if (abs(tmp-prev_voltage)>10) {
    prev_voltage=voltage;
    return prev_paddle;
  }
  prev_voltage=voltage;
  // check paddle state
  if (voltage > 2400) {  // not touch
    prev_paddle=0;
    return 0; // pull up

  } else if (voltage > 2100) {  // left 
    if (prev_paddle==LEFT_PADDLE) {
      prev_paddle=LEFT_PADDLE;
      return LEFT_PADDLE; // dit on dah 0
    } else {
      tmp= prev_paddle;
      prev_paddle=LEFT_PADDLE;
      return prev_paddle;
    }
    
  } else if (voltage > 1800) {
    return prev_paddle;// buffer
    
  } else if (voltage > 1600) {  // right
    if (prev_paddle==RIGHT_PADDLE) {
      prev_paddle=RIGHT_PADDLE;      
      return RIGHT_PADDLE; // dit 0 dah on
    } else {
      tmp=prev_paddle;
      prev_paddle=RIGHT_PADDLE;      
      return tmp;
    }

  } else if (voltage > 1450) { // buffer
    return  prev_paddle;

    
  } else if (voltage > 1000) {   // both 
    prev_paddle=LEFT_PADDLE|RIGHT_PADDLE;
    return LEFT_PADDLE|RIGHT_PADDLE; // dit on dah 1
    
  } else {
    return 0; // no paddle circuit installed 
  }
  return 0;
}


void update_PaddleLatch(int paddle) {
  if (paddle & g_dit_paddle) {
    g_keyerControl |= DIT_L;
  }
  if (paddle & g_dah_paddle) {
    g_keyerControl |= DAH_L;
  }
}

///////////////////////////////////////////////////////////////////////////////
//       Key the Transmitter
///////////////////////////////////////////////////////////////////////////////
void tx_key_down() {

  if (g_keyer_command_state != CMD_IDLE) {   // In Cmd mode only send the audio tone
    //    digitalWrite(LED_PIN, HIGH);             // turn the LED on
    //    tone(PIEZO_SPKR_PIN, SIDETONE_FREQ_HZ);  // We don't mute the sidetone in CMD mode
    //    keying(1);
  } else {                                   // Not in Command mode
    //    digitalWrite(LED_PIN, HIGH);             // turn the LED on
    //    digitalWrite(TX_SWITCH_PIN, HIGH);       // Turn the transmitter on
    keying(1);
    if (!g_sidetone_muted) {
      //      tone(PIEZO_SPKR_PIN, SIDETONE_FREQ_HZ); // Sidetone on if not muted
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
//    Un-key the transmitter
///////////////////////////////////////////////////////////////////////////////
void tx_key_up() {

  keying(0);

  if (g_keyer_command_state != CMD_IDLE) {  // In Cmd mode sowe only send the audio tone
    //    noTone(PIEZO_SPKR_PIN);
  } else {                             // Not in Command mode
    //    digitalWrite(TX_SWITCH_PIN, LOW);  // Turn the transmitter off
    keying(0);
    if (!g_sidetone_muted) {
      //      noTone(PIEZO_SPKR_PIN);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
//
//    Calculate new Dit duration in ms based on wpm value
//
///////////////////////////////////////////////////////////////////////////////

void loadWPM(int wpm) {
  g_ditTime = 1200 / wpm;
}

void exit_command_mode() {
}

/////////////////////////////////////////////////////////////////////////////
// Use the decoded send character to determine the current user-command
// and execute it. 
////////////////////////////////////////////////////////////////////////////
void process_keyer_command(uint8_t current_character, uint32_t ditTime_ms) {



}  // End process_keyer_command()

#define SPACE B11101111  // Special encoding for interword Space character

uint8_t morse_char_code(char c) {

  // This function returns the encoded Morse pattern for the ASCII character passed in.
  // Binary encoding is left-padded. Unused high-order bits are all ones.
  // The first zero is the start bit, which is discarded.
  // Processing from higher to lower order bits we skip over ones, then discard first 0 (start bit). The next bit is the first element.
  // We process each element sending a DIT or DAH, until we reach the end of the pattern.
  //
  // Pattern encoding is 0 = DIT, 1 = DAH.
  // So 'A' = B11111001, which is 1 1 1 1 1 (padding bits) 0 (start bit)  0 1 (dit, dah)
  // This excellent encoding scheme was developed by Hans, G0UPL as noted above.

  switch (c) {
    case 'A': return B11111001; break;  // A  .-
    case 'B': return B11101000; break;  // B  -...
    case 'C': return B11101010; break;  // C  -.-.
    case 'D': return B11110100; break;  // D  -..
    case 'E': return B11111100; break;  // E  .
    case 'F': return B11100010; break;  // F  ..-.
    case 'G': return B11110110; break;  // G  --.
    case 'H': return B11100000; break;  // H  ....
    case 'I': return B11111000; break;  // I  ..
    case 'J': return B11100111; break;  // J  .---
    case 'K': return B11110101; break;  // K  -.-
    case 'L': return B11100100; break;  // L  .-..
    case 'M': return B11111011; break;  // M  --
    case 'N': return B11111010; break;  // N  -.
    case 'O': return B11110111; break;  // O  ---
    case 'P': return B11100110; break;  // P  .--.
    case 'Q': return B11101101; break;  // Q  --.-
    case 'R': return B11110010; break;  // R  .-.
    case 'S': return B11110000; break;  // S  ...
    case 'T': return B11111101; break;  // T  -
    case 'U': return B11110001; break;  // U  ..-
    case 'V': return B11100001; break;  // V  ...-
    case 'W': return B11110011; break;  // W  .--
    case 'X': return B11101001; break;  // X  -..-
    case 'Y': return B11101011; break;  // Y  -.--
    case 'Z': return B11101100; break;  // Z  --..
    case '0': return B11011111; break;  // 0  -----
    case '1': return B11001111; break;  // 1  .----
    case '2': return B11000111; break;  // 2  ..---
    case '3': return B11000011; break;  // 3  ...--
    case '4': return B11000001; break;  // 4  ....-
    case '5': return B11000000; break;  // 5  .....
    case '6': return B11010000; break;  // 6  -....
    case '7': return B11011000; break;  // 7  --...
    case '8': return B11011100; break;  // 8  ---..
    case '9': return B11011110; break;  // 9  ----.
    case ' ': return SPACE; break;      // Space - equal to 4 dah lengths
    case '/': return B11010010; break;  // /  -..-.
    case '?': return B10001100; break;  // ? ..--..
    case '*': return B11001010; break;  // AR .-.-.  End of Transmission (we use * in ASCII to represent this character)
    case '#': return B00000000; break;  // ERROR .......  (we use # in ASCII to represent this)

    default: return SPACE;  // If we don't recognize the character then just return Space - equal to 4 dah lengths
  }                         // end switch
}  // end morse_char_code()


char morse_code_char(uint8_t d)
{
  switch(d) {
  case B11111001: return 'A'; break;  // A  .-
  case B11101000: return 'B'; break;  // B  -...
  case B11101010: return 'C'; break;  // C  -.-.
  case B11110100: return 'D'; break;  // D  -..
  case B11111100: return 'E'; break;  // E  .
  case B11100010: return 'F'; break;  // F  ..-.
  case B11110110: return 'G'; break;  // G  --.
  case B11100000: return 'H'; break;  // H  ....
  case B11111000: return 'I'; break;  // I  ..
  case B11100111: return 'J'; break;  // J  .---
  case B11110101: return 'K'; break;  // K  -.-
  case B11100100: return 'L'; break;  // L  .-..
  case B11111011: return 'M'; break;  // M  --
  case B11111010 : return 'N'; break;  // N  -.
  case B11110111 : return 'O'; break;  // O  ---
  case B11100110 : return 'P'; break;  // P  .--.
  case B11101101 : return 'Q'; break;  // Q  --.-
  case B11110010 : return 'R'; break;  // R  .-.
  case B11110000 : return 'S'; break;  // S  ...
  case B11111101 : return 'T'; break;  // T  -
  case B11110001 : return 'U'; break;  // U  ..-
  case B11100001 : return 'V'; break;  // V  ...-
  case B11110011 : return 'W'; break;  // W  .--
  case B11101001 : return 'X'; break;  // X  -..-
  case B11101011 : return 'Y'; break;  // Y  -.--
  case B11101100 : return 'Z'; break;  // Z  --..
  case B11011111 : return '0'; break;  // 0  -----
  case B11001111 : return '1'; break;  // 1  .----
  case B11000111 : return '2'; break;  // 2  ..---
  case B11000011 : return '3'; break;  // 3  ...--
  case B11000001 : return '4'; break;  // 4  ....-
  case B11000000 : return '5'; break;  // 5  .....
  case B11010000 : return '6'; break;  // 6  -....
  case B11011000 : return '7'; break;  // 7  --...
  case B11011100 : return '8'; break;  // 8  ---..
  case B11011110 : return '9'; break;  // 9  ----.
  case     SPACE : return ' '; break;      // Space - equal to 4 dah lengths
  case B11010010 : return '/'; break;  // /  -..-.
  case B10001100 : return '?'; break;  // ? ..--..
  case B11001010 : return '*'; break;  // AR .-.-.  End of Transmission (we use * in ASCII to represent this character)
  case B00000000 : return '#'; break;  // ERROR .......  (we use # in ASCII to represent this)
  default: break;   // If we don't recognize the character then just return Space - equal to 4 dah lengths
  }
  return ' ';  
}
