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

#ifndef FILE_TIMEKEEP_H
#define FILE_TIMEKEEP_H
void timekeep() ;
void init_timekeep();
//#include <NTPClient.h> // ntp client
#include "RTClib.h"   // DS1307



class myDateTime : public DateTime
{

  int mday[2][12]={
    {31,28,31,30,31,30,31,31,30,31,30,31},
    {31,29,31,30,31,30,31,31,30,31,30,31}
  };

  
 public:
  short msec;
  short evt_second; // if nonzero crossed second, user need reset
  int adj;  // if nonzero inc_rtc will add adj to msec and set adj=0  
  myDateTime();
  myDateTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour,
	     uint8_t min, uint8_t sec);
  
  short isleap(short yr);
  void inc_ms(int tick_interval_ms);
  
};

extern myDateTime my_rtc;

extern RTC_DS1307 rtcclock;
void interrupt_my_rtc();

void print_rtcclock();
void set_rtcclock(char *timestr) ; // yymmddhhmmss to set
void print_ntpstatus() ;

//extern NTPClient timeClient;

#endif
