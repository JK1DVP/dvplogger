/**
 * ch9350if_hidkeys.h
 * miscs functions of CH9350L keyboard interface library for Arduino.
 * Copyright (c) 2022 Takeshi Higasa, okiraku-camera.tokyo
 *
 * This file is part of ch9350ifLibrary.
 *
 * ch9350ifLibrary is free software: you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, either version 3 of the License, or 
 * (at your option) any later version.
 *
 * ch9350ifLibrary is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ch9350ifLibrary.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef __CH9350_HID_KEYS_H__
#define __CH9350_HID_KEYS_H__

// some keys used by samples.
static const uint8_t HID_CAPS = 0x39;
static const uint8_t HID_KEYPAD_NUMLOCK = 0x53;
static const uint8_t HID_SCRLOCK = 0x47;

// Output report LED positions.
static const uint8_t HID_LED_NUMLOCK  = 1;
static const uint8_t HID_LED_CAPSLOCK = 2;
static const uint8_t HID_LED_SCRLOCK  = 4;

#if !defined(ARDUINO_ARCH_AVR)
#define PROGMEM
#define PGM_READ_ADDR(a)	(uint8_t*)a
#define PGM_READ_BYTE(a)	*a
#define PGM_READ_WORD(a)	*(uint16_t*)a
#else
#define PGM_READ_ADDR(a)	(const uint8_t*)pgm_read_word(&a)
#define PGM_READ_BYTE(a)	(uint8_t)pgm_read_byte(a)
#define PGM_READ_WORD(a)	(uint16_t)pgm_read_word(a)
#endif

static const uint8_t id_00[] PROGMEM = "?";

static const uint8_t id_04[] PROGMEM = "A";
static const uint8_t id_05[] PROGMEM = "B";
static const uint8_t id_06[] PROGMEM = "C";
static const uint8_t id_07[] PROGMEM = "D";
static const uint8_t id_08[] PROGMEM = "E";
static const uint8_t id_09[] PROGMEM = "F";
static const uint8_t id_0A[] PROGMEM = "G";
static const uint8_t id_0B[] PROGMEM = "H";
static const uint8_t id_0C[] PROGMEM = "I";
static const uint8_t id_0D[] PROGMEM = "J";
static const uint8_t id_0E[] PROGMEM = "K";
static const uint8_t id_0F[] PROGMEM = "L";
static const uint8_t id_10[] PROGMEM = "M";

static const uint8_t id_11[] PROGMEM = "N";
static const uint8_t id_12[] PROGMEM = "O";
static const uint8_t id_13[] PROGMEM = "P";
static const uint8_t id_14[] PROGMEM = "Q";
static const uint8_t id_15[] PROGMEM = "R";
static const uint8_t id_16[] PROGMEM = "S";
static const uint8_t id_17[] PROGMEM = "T";
static const uint8_t id_18[] PROGMEM = "U";
static const uint8_t id_19[] PROGMEM = "V";
static const uint8_t id_1A[] PROGMEM = "W";
static const uint8_t id_1B[] PROGMEM = "X";
static const uint8_t id_1C[] PROGMEM = "Y";
static const uint8_t id_1D[] PROGMEM = "Z";
static const uint8_t id_1E[] PROGMEM = "1";
static const uint8_t id_1F[] PROGMEM = "2";

static const uint8_t id_20[] PROGMEM = "3";
static const uint8_t id_21[] PROGMEM = "4";
static const uint8_t id_22[] PROGMEM = "5";
static const uint8_t id_23[] PROGMEM = "6";
static const uint8_t id_24[] PROGMEM = "7";
static const uint8_t id_25[] PROGMEM = "8";
static const uint8_t id_26[] PROGMEM = "9";
static const uint8_t id_27[] PROGMEM = "0";
static const uint8_t id_28[] PROGMEM = "Enter";
static const uint8_t id_29[] PROGMEM = "Escape";
static const uint8_t id_2A[] PROGMEM = "BackSP";
static const uint8_t id_2B[] PROGMEM = "Tab";
static const uint8_t id_2C[] PROGMEM = "SPC";
static const uint8_t id_2D[] PROGMEM = "-";
static const uint8_t id_2E[] PROGMEM = "=";
static const uint8_t id_2F[] PROGMEM = "[";

static const uint8_t id_30[] PROGMEM = "]";
static const uint8_t id_31[] PROGMEM = "\\";
static const uint8_t id_32[] PROGMEM = " ";
static const uint8_t id_33[] PROGMEM = ";";
static const uint8_t id_34[] PROGMEM = "'";
static const uint8_t id_35[] PROGMEM = "~";
static const uint8_t id_36[] PROGMEM = ",";
static const uint8_t id_37[] PROGMEM = ".";
static const uint8_t id_38[] PROGMEM = "/";
static const uint8_t id_39[] PROGMEM = "CapsLock";
static const uint8_t id_3A[] PROGMEM = "F1";
static const uint8_t id_3B[] PROGMEM = "F2";
static const uint8_t id_3C[] PROGMEM = "F3";
static const uint8_t id_3D[] PROGMEM = "F4";
static const uint8_t id_3E[] PROGMEM = "F5";
static const uint8_t id_3F[] PROGMEM = "F6";

static const uint8_t id_40[] PROGMEM = "F7";
static const uint8_t id_41[] PROGMEM = "F8";
static const uint8_t id_42[] PROGMEM = "F9";
static const uint8_t id_43[] PROGMEM = "F10";
static const uint8_t id_44[] PROGMEM = "F11";
static const uint8_t id_45[] PROGMEM = "F12";
static const uint8_t id_46[] PROGMEM = "PrtSc";
static const uint8_t id_47[] PROGMEM = "ScrLock";
static const uint8_t id_48[] PROGMEM = "Pause";
static const uint8_t id_49[] PROGMEM = "Insert";
static const uint8_t id_4A[] PROGMEM = "Home";
static const uint8_t id_4B[] PROGMEM = "PgUp";
static const uint8_t id_4C[] PROGMEM = "Delete";
static const uint8_t id_4D[] PROGMEM = "End";
static const uint8_t id_4E[] PROGMEM = "PgDn";
static const uint8_t id_4F[] PROGMEM = "R_Arrow";

static const uint8_t id_50[] PROGMEM = "L_Arrow";
static const uint8_t id_51[] PROGMEM = "D_Arrow";
static const uint8_t id_52[] PROGMEM = "U_Arrow";
static const uint8_t id_53[] PROGMEM = "NumLock";
static const uint8_t id_54[] PROGMEM = "/ (kp)";
static const uint8_t id_55[] PROGMEM = "* (kp)";
static const uint8_t id_56[] PROGMEM = "- (kp)";
static const uint8_t id_57[] PROGMEM = "+ (kp)";
static const uint8_t id_58[] PROGMEM = "Enter (kp)";
static const uint8_t id_59[] PROGMEM = "1 (kp)";
static const uint8_t id_5A[] PROGMEM = "2 (kp)";
static const uint8_t id_5B[] PROGMEM = "3 (kp)";
static const uint8_t id_5C[] PROGMEM = "4 (kp)";
static const uint8_t id_5D[] PROGMEM = "5 (kp)";
static const uint8_t id_5E[] PROGMEM = "6 (kp)";
static const uint8_t id_5F[] PROGMEM = "7 (kp)";

static const uint8_t id_60[] PROGMEM = "8 (kp)";
static const uint8_t id_61[] PROGMEM = "9 (kp)";
static const uint8_t id_62[] PROGMEM = "0 (kp)";
static const uint8_t id_63[] PROGMEM = ". (kp)";
static const uint8_t id_64[] PROGMEM = "\\";
static const uint8_t id_65[] PROGMEM = "App";
static const uint8_t id_66[] PROGMEM = "Power";
static const uint8_t id_67[] PROGMEM = "= (kp)";
static const uint8_t id_68[] PROGMEM = "F13";
static const uint8_t id_69[] PROGMEM = "F14";
static const uint8_t id_6A[] PROGMEM = "F15";
static const uint8_t id_6B[] PROGMEM = "F16";
static const uint8_t id_6C[] PROGMEM = "F17";
static const uint8_t id_6D[] PROGMEM = "F18";
static const uint8_t id_6E[] PROGMEM = "F19";
static const uint8_t id_6F[] PROGMEM = "F20";

static const uint8_t id_70[] PROGMEM = "F21";
static const uint8_t id_71[] PROGMEM = "F22";
static const uint8_t id_72[] PROGMEM = "F23";
static const uint8_t id_73[] PROGMEM = "F24";
static const uint8_t id_74[] PROGMEM = " ";
static const uint8_t id_75[] PROGMEM = " ";
static const uint8_t id_76[] PROGMEM = " ";
static const uint8_t id_77[] PROGMEM = " ";
static const uint8_t id_78[] PROGMEM = " ";
static const uint8_t id_79[] PROGMEM = " ";
static const uint8_t id_7A[] PROGMEM = " ";
static const uint8_t id_7B[] PROGMEM = " ";
static const uint8_t id_7C[] PROGMEM = " ";
static const uint8_t id_7D[] PROGMEM = " ";
static const uint8_t id_7E[] PROGMEM = " ";
static const uint8_t id_7F[] PROGMEM = " ";

static const uint8_t id_80[] PROGMEM = " ";
static const uint8_t id_81[] PROGMEM = " ";
static const uint8_t id_82[] PROGMEM = " ";
static const uint8_t id_83[] PROGMEM = " ";
static const uint8_t id_84[] PROGMEM = " ";
static const uint8_t id_85[] PROGMEM = " ";
static const uint8_t id_86[] PROGMEM = " ";
static const uint8_t id_87[] PROGMEM = "Int1";
static const uint8_t id_88[] PROGMEM = "Int2";
static const uint8_t id_89[] PROGMEM = "Int3";
static const uint8_t id_8A[] PROGMEM = "Int4";
static const uint8_t id_8B[] PROGMEM = "Int5";
static const uint8_t id_8C[] PROGMEM = "Int6";
static const uint8_t id_8D[] PROGMEM = "Int7";
static const uint8_t id_8E[] PROGMEM = "Int8";
static const uint8_t id_8F[] PROGMEM = "Int9";

static const uint8_t id_90[] PROGMEM = "Lang1";
static const uint8_t id_91[] PROGMEM = "Lang2";
static const uint8_t id_92[] PROGMEM = "Lang3";
static const uint8_t id_93[] PROGMEM = "Lang4";
static const uint8_t id_94[] PROGMEM = "Lang5";
static const uint8_t id_95[] PROGMEM = "Lang6";
static const uint8_t id_96[] PROGMEM = "Lang7";
static const uint8_t id_97[] PROGMEM = "Lang8";
static const uint8_t id_98[] PROGMEM = "Lang9";
static const uint8_t id_99[] PROGMEM = " ";
static const uint8_t id_9A[] PROGMEM = " ";
static const uint8_t id_9B[] PROGMEM = " ";
static const uint8_t id_9C[] PROGMEM = " ";
static const uint8_t id_9D[] PROGMEM = " ";
static const uint8_t id_9E[] PROGMEM = " ";
static const uint8_t id_9F[] PROGMEM = " ";

static const uint8_t id_E0[] PROGMEM = "L_Ctrl";
static const uint8_t id_E1[] PROGMEM = "L_Shift";
static const uint8_t id_E2[] PROGMEM = "L_Alt";
static const uint8_t id_E3[] PROGMEM = "L_Gui";
static const uint8_t id_E4[] PROGMEM = "R_Ctrl";
static const uint8_t id_E5[] PROGMEM = "R_Shift";
static const uint8_t id_E6[] PROGMEM = "R_Alt ";
static const uint8_t id_E7[] PROGMEM = "R_Gui";

// id = 0x00～0x9f
static const uint8_t* const str[] PROGMEM = {	
	id_00,id_00,id_00,id_00,id_04,id_05,id_06,id_07,id_08,id_09,id_0A,id_0B,id_0C,id_0D,id_0E,id_0F,
	id_10,id_11,id_12,id_13,id_14,id_15,id_16,id_17,id_18,id_19,id_1A,id_1B,id_1C,id_1D,id_1E,id_1F,
	id_20,id_21,id_22,id_23,id_24,id_25,id_26,id_27,id_28,id_29,id_2A,id_2B,id_2C,id_2D,id_2E,id_2F,
	id_30,id_31,id_32,id_33,id_34,id_35,id_36,id_37,id_38,id_39,id_3A,id_3B,id_3C,id_3D,id_3E,id_3F,
	id_40,id_41,id_42,id_43,id_44,id_45,id_46,id_47,id_48,id_49,id_4A,id_4B,id_4C,id_4D,id_4E,id_4F,
	id_50,id_51,id_52,id_53,id_54,id_55,id_56,id_57,id_58,id_59,id_5A,id_5B,id_5C,id_5D,id_5E,id_5F,
	id_60,id_61,id_62,id_63,id_64,id_65,id_66,id_67,id_68,id_69,id_6A,id_6B,id_6C,id_6D,id_6E,id_6F,
	id_70,id_71,id_72,id_73,id_74,id_75,id_76,id_77,id_78,id_79,id_7A,id_7B,id_7C,id_7D,id_7E,id_7F,
	id_80,id_81,id_82,id_83,id_84,id_85,id_86,id_87,id_88,id_89,id_8A,id_8B,id_8C,id_8D,id_8E,id_8F,
	id_90,id_91,id_92,id_93,id_94,id_95,id_96,id_97,id_98,id_99,id_9A,id_9B,id_9C,id_9D,id_9E,id_9F
};

static const uint8_t* const str_mod[] PROGMEM = { id_E0,id_E1,id_E2,id_E3,id_E4,id_E5,id_E6,id_E7 };

const uint8_t* get_hid_keyname_P(uint8_t id) {
	const uint8_t* address = PGM_READ_ADDR(id_00);
	if (id < 0xa0)
		address = PGM_READ_ADDR(str[id]);
	else if (id >= 0xe0 && id < 0xe8)
		address = PGM_READ_ADDR(str_mod[id & 0x0f]);
	return address;

}

const char* get_hid_keyname(uint8_t id) { 
	static char buf[20];
	strncpy_P(buf, (const char*)get_hid_keyname_P(id), sizeof(buf));
	return (const char*)buf;
}

void dump_byte(uint8_t c) {
  if (Serial) {
    static const char hex[] = "0123456789ABCDEF";
    char tmp[4];
    tmp[0] = hex[(c >> 4) & 0x0f];
    tmp[1] = hex[c & 0x0f];
    tmp[2] = ' ';  
    tmp[3] = 0;  
    Serial.print((const char*)tmp);
  }
}


#endif //__CH9350_HID_KEYS_H__

