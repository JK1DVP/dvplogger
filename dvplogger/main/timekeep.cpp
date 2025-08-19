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
#include "display.h"
#include "timekeep.h"
#include <WiFiUdp.h>
//#include <NTPClient.h>
#include "misc.h"
#include "Ticker.h"
//#include <sys/time.h>
//#include <esp_sntp.h>
#include <ESPNtpClient.h>


myDateTime my_rtc;
Ticker my_rtc_ticker; // inc every MY_RTC_TICK_INTERVAL_MS
#define MY_RTC_TICK_INTERVAL_MS 100



short myDateTime::isleap(short yr)
{
  if ((yr%4)!=0) return 0;
  else if ((yr%100)!=0) return 1;
  else if ((yr%400)!=0) return 0;
  else return 1;
}



myDateTime::myDateTime()
{
  evt_second=0;
  adj=0;
  msec=0; 
}


myDateTime::myDateTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour,
                   uint8_t min, uint8_t sec) {
  if (year >= 2000U)
    year -= 2000U;
  yOff = year;
  m = month;
  d = day;
  hh = hour;
  mm = min;
  ss = sec;
  
  msec=0;
  adj=0;
  evt_second=0;
}


void myDateTime::inc_ms(int tick_interval_ms) {
  
  msec+=tick_interval_ms;
  if (adj!=0) { // clock adjustment
    msec+=adj;
    adj=0;
  }
  
  if (msec>=1000) {
    // crossed 1sec
    msec-=1000;
    ss++;

    if(ss>=60) {
      // minute
      ss=0;
      mm++;
      if (mm>=60) {
	mm=0;
	hh++;

	if (hh>=24) {
	  hh=0;
	  d++;
	  if (d>mday[isleap(yOff+2000)][m-1]) {
	    d=1;
	    m++;
	    if (m>12) {
	      m=1;
	      yOff++;
	    }
	  }
	}
      }
    }
    evt_second=1;
  }
}

void interrupt_my_rtc()
{
  my_rtc.inc_ms(100);
}

WiFiUDP ntpUDP;

// You can specify the time server pool and the offset (in seconds, can be
// changed later with setTimeOffset() ). Additionaly you can specify the
// update interval (in milliseconds, can be changed using setUpdateInterval() ).
//NTPClient timeClient(ntpUDP, "ntp.nict.jp", 32400, 60000);

//DS3231 rtcclock;
RTC_DS1307 rtcclock;
DateTime myRTC;
//RTClib myRTC;

//static const uint8_t LED = 2;

void init_timekeep()
{
  DateTime clock; // RTC clock

  // system clock initialization
  time_t now;
  char strftime_buf[64];
  struct tm timeinfo;

  time(&now);
  setenv("TZ", "JST-9", 1); // set local time to JST
  tzset();
  
  localtime_r(&now, &timeinfo);
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
  console->printf( "The current date/time in Japan is: %s\n", strftime_buf); // shown jst

  
  rtcclock.begin();
  // set my_rtc from RTC and start
  clock=rtcclock.now();
  my_rtc=myDateTime(clock.year(),clock.month(),clock.day(),clock.hour(),clock.minute(),clock.second());
  console->print("my_rtc ");
  console->println(my_rtc.msec);
  my_rtc_ticker.attach_ms(100,interrupt_my_rtc);
  
}




void print_rtcclock() {
  DateTime clock;
  clock=rtcclock.now();
  char s[80];
  sprintf(s,"rtc read : %02d/%02d/%02d-%02d:%02d:%02d\n", clock.year() % 100, clock.month(), clock.day(),
	  clock.hour(), clock.minute(), clock.second());
  plogw->ostream->print(s);
}


void print_ntpstatus() {

    timeval ntptime;
    struct tm local_tm;
    char datestr[100];
    gettimeofday(&ntptime,NULL);
    localtime_r(&ntptime.tv_sec,&local_tm);
    sprintf(datestr, "NTP:%04d/%02d/%02d %02d:%02d:%02d.%03ld myRTC:%02d:%02d:%02d.%03d",
	    local_tm.tm_year+1900,local_tm.tm_mon+1,local_tm.tm_mday,local_tm.tm_hour,local_tm.tm_min,local_tm.tm_sec,ntptime.tv_usec/1000,
	    my_rtc.hour(), my_rtc.minute(), my_rtc.second(),my_rtc.msec);
    plogw->ostream->print(datestr);
    console->print(" status=");
    console->print(NTP.syncStatus());
    console->printf (" Free heap: %u\n", ESP.getFreeHeap ());      
}

void set_rtcclock(char *timestr) { // yymmddhhmmss to set 
  DateTime clock;
  char s[80];  
  clock=DateTime(timestr);
  sprintf(s,"setting to : %02d/%02d/%02d-%02d:%02d:%02d\n", clock.year() % 100, clock.month(), clock.day(),
	  clock.hour(), clock.minute(), clock.second());
  plogw->ostream->print(s);  
  rtcclock.adjust(clock);
  my_rtc=myDateTime(clock.year(),clock.month(),clock.day(),clock.hour(),clock.minute(),clock.second()); // my_rtc also set to the same time
  
  print_rtcclock();
}

void timekeep() {
  //  DateTime rtctime_bak;


  if (my_rtc.evt_second) {
    my_rtc.evt_second=0;
    
    char datestr[40];

    rtctime = rtcclock.now();

    // check my_rtc and rtctime (DS1307) to keep track with DS1307
    TimeSpan dt1;
    int32_t dt;    
    dt1=my_rtc-rtctime;
    dt=dt1.totalseconds();
    if (verbose & 1024) {
      sprintf(buf,"my_rtc %04d/%02d/%02d %02d:%02d:%02d.%03d dt(ds1307)=%d",my_rtc.year(), my_rtc.month(), my_rtc.day(),my_rtc.hour(),my_rtc.minute(),my_rtc.second(),my_rtc.msec,dt);
      console->println(buf);
    }

    if (dt!=0) {
      if (dt>=1 && dt<=10) {      
	// my_rtc is ahead of DS1307  my_rtc need to be slowed
	console->print("my_rtc adj -100 dt=");
	console->println(dt);
	my_rtc.adj=-100;
      } else {
	if (dt<=-1 && dt>=-10) {	
	  // my_rtc is behind DS1307  my_rtc need to be advanced
	  console->print("my_rtc adj 100 dt=");
	  console->println(dt);	  
	  my_rtc.adj=100;\
	} else {
	  // large difference
	  console->println("my_rtc large difference with DS1307");
	  sprintf(buf,"my_rtc %02d:%02d:%02d.%03d dt(ds1307)=%d DS1307 %02d:%02d:%02d ",my_rtc.hour(),my_rtc.minute(),my_rtc.second(),my_rtc.msec,dt,rtctime.hour(),rtctime.minute(),rtctime.second());
	  console->println(buf);
	}
      }
    }
    
    // ntp update
    if (verbose & 1024) {
      if (NTP.syncStatus() != 0) { // check only synched     
	time_t now;
	char strftime_buf[64];
	struct tm timeinfo;

	time(&now);
	localtime_r(&now, &timeinfo);
	strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
	console->printf("SystemTime(local): %s ", strftime_buf);

	console->print("ESPNtp:");
	console->print(NTP.getTimeDateStringUs ());
	console->print(" status=");
	console->print(NTP.syncStatus());
	console->printf (" Free heap: %u\n", ESP.getFreeHeap ());      
      }
    }

    timeval ntptime;
    struct tm local_tm;
    time_t ntptime_local;
    if (NTP.syncStatus() == 0) { // check only synched 
      gettimeofday(&ntptime,NULL);
      ntptime_local=ntptime.tv_sec+32400; // JST
      localtime_r(&ntptime.tv_sec,&local_tm);
      if (my_rtc.unixtime() > ntptime_local) { // my rtc.unixtime really is localtime
	dt = my_rtc.unixtime() - ntptime_local;
      } else {
	dt = ntptime_local - my_rtc.unixtime();
      }
	
      if (dt >= 2) {
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("dt=");
	  plogw->ostream->println(dt);
	}

	rtcadj_count++;
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("rtcadj_count=");
	  plogw->ostream->println(rtcadj_count);
	}
	if (rtcadj_count >= 10) {
	  // set rtc from ntptime
	  delay(1000-ntptime.tv_usec/1000); // delay until the next second
	  rtcclock.adjust(ntptime_local+1); // set DS1307 1sec ahead
	  my_rtc=myDateTime(local_tm.tm_year+1900,local_tm.tm_mon+1,local_tm.tm_mday,local_tm.tm_hour,local_tm.tm_min,local_tm.tm_sec);
	  console->printf("adjusting my_rtc set %04d/%02d/%02d %02d:%02d:%02d \n",local_tm.tm_year+1900,local_tm.tm_mon+1,local_tm.tm_mday,local_tm.tm_hour,local_tm.tm_min,local_tm.tm_sec);
	  if (!plogw->f_console_emu) plogw->ostream->println("RTC reset by NTP");
	  rtcadj_count = 0;
	}
      } else {
	rtcadj_count = 0;
      }
    }


    time_measure_start(14);
    /*    sprintf(plogw->tm, "%02d/%02d/%02d-%02d:%02d:%02d", rtctime.year() % 100, rtctime.month(), rtctime.day(),
	  rtctime.hour(), rtctime.minute(), rtctime.second());*/
    // now refers to my_rtc
    sprintf(plogw->tm, "%02d/%02d/%02d-%02d:%02d:%02d", my_rtc.year() % 100, my_rtc.month(), my_rtc.day(),
	    my_rtc.hour(), my_rtc.minute(), my_rtc.second());
    if (verbose&1024) console->println(plogw->tm);
    upd_display_tm();
    right_display_sendBuffer();
    if (f_show_clock == 2) {

      sprintf(datestr, "%04d/%02d/%02d-%02d:%02d:%02d",
	      /*              rtctime.year(), rtctime.month(), rtctime.day(),
			      rtctime.hour(), rtctime.minute(), rtctime.second());*/
	      my_rtc.year(), my_rtc.month(), my_rtc.day(),
	      my_rtc.hour(), my_rtc.minute(), my_rtc.second());

      if (!plogw->f_console_emu) {
        plogw->ostream->print("RTC:");
        plogw->ostream->print(datestr);
      }

      if (NTP.syncStatus() == 0) { // check only synched 
	tm local_tm;
        if (!plogw->f_console_emu) {
	  gettimeofday(&ntptime,NULL); // obtain Epoch time
	  localtime_r(&ntptime.tv_sec,&local_tm); // get local time from 
	  
          plogw->ostream->print(" ");
          sprintf(datestr, "%04d/%02d/%02d %02d:%02d:%02d",
		  local_tm.tm_year+1900,local_tm.tm_mon+1,local_tm.tm_mday,local_tm.tm_hour,local_tm.tm_min,local_tm.tm_sec);
          plogw->ostream->print("NTP:");
          plogw->ostream->println(datestr);
        }
      } else {
        if (!plogw->f_console_emu) plogw->ostream->println("");
      }

    }
    time_measure_stop(14);
  }
}


