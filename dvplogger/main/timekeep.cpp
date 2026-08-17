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
#include "display.h"
#include "timekeep.h"
#include "i2c_guard.h"
#include "dupechk.h"
#include "mux_transport.h"
#include "satellite.h"
#include "misc.h"
#include "Ticker.h"
#include <sys/time.h>
#include "network.h"


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

// You can specify the time server pool and the offset (in seconds, can be
// changed later with setTimeOffset() ). Additionaly you can specify the
// update interval (in milliseconds, can be changed using setUpdateInterval() ).
//NTPClient timeClient(ntpUDP, "ntp.nict.jp", 32400, 60000);

//DS3231 rtcclock;
RTC_DS1307 rtcclock;
DateTime myRTC;
//RTClib myRTC;

// NTP -> DS1307 propagation remains deliberately conservative: only after a
// persistent >=2 s error.  Schedule the write for the next system-clock second
// boundary without blocking the main loop.
static bool rtc_ntp_adjust_pending = false;
static uint32_t rtc_ntp_adjust_due_ms = 0;

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
  console->printf("The current date/time in Japan is: %s\n", strftime_buf);

  if (i2c_bus_lock("rtc_begin", pdMS_TO_TICKS(50))) {
    uint32_t t0 = micros();
    rtcclock.begin();
    uint32_t dt = micros() - t0;
    i2c_bus_unlock("rtc_begin");
    i2c_diag_io("rtc_begin", dt);
  }

  bool rtc_ok = false;
  if (i2c_bus_lock("rtc_init", pdMS_TO_TICKS(20))) {
    uint32_t t0 = micros();
    clock = rtcclock.now();
    uint32_t dt = micros() - t0;
    i2c_bus_unlock("rtc_init");
    i2c_diag_io("rtc_init", dt);
    rtc_ok = true;
  }
  if (rtc_ok) {
    my_rtc = myDateTime(clock.year(), clock.month(), clock.day(),
                        clock.hour(), clock.minute(), clock.second());
  }
  console->print("my_rtc ");
  console->println(my_rtc.msec);
  my_rtc_ticker.attach_ms(100, interrupt_my_rtc);
}




void print_rtcclock() {
  DateTime clock;
  if (!i2c_bus_lock("rtc_print", pdMS_TO_TICKS(20))) return;
  uint32_t t0 = micros();
  clock = rtcclock.now();
  uint32_t dt = micros() - t0;
  i2c_bus_unlock("rtc_print");
  i2c_diag_io("rtc_print", dt);

  char s[80];
  sprintf(s, "rtc read : %02d/%02d/%02d-%02d:%02d:%02d\n",
          clock.year() % 100, clock.month(), clock.day(),
          clock.hour(), clock.minute(), clock.second());
  plogw->ostream->print(s);
}



void print_ntpstatus(Stream *out) {
  if (!out) out = console;

  timeval systime;
  struct tm local_tm;
  char datestr[100];
  gettimeofday(&systime, NULL);
  localtime_r(&systime.tv_sec, &local_tm);
  snprintf(datestr, sizeof(datestr),
           "NTP:%04d/%02d/%02d %02d:%02d:%02d.%03ld "
           "myRTC:%02d:%02d:%02d.%03d",
           local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
           local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
           systime.tv_usec / 1000,
           my_rtc.hour(), my_rtc.minute(), my_rtc.second(), my_rtc.msec);
  out->print(datestr);
  out->print(" status=");
  out->print(network_ntp_synced() ? "SYNCED" : "NOT_SYNCED");
  out->printf(" Free heap: %u\n", ESP.getFreeHeap());
}

void set_rtcclock(char *timestr) { // yymmddhhmmss to set 
  DateTime clock;
  char s[80];  
  clock=DateTime(timestr);
  sprintf(s,"setting to : %02d/%02d/%02d-%02d:%02d:%02d\n", clock.year() % 100, clock.month(), clock.day(),
	  clock.hour(), clock.minute(), clock.second());
  plogw->ostream->print(s);  
  if (i2c_bus_lock("rtc_set", pdMS_TO_TICKS(20))) {
    uint32_t t0 = micros();
    rtcclock.adjust(clock);
    i2c_bus_unlock("rtc_set");
    i2c_diag_io("rtc_set", micros() - t0);
  }
  my_rtc=myDateTime(clock.year(),clock.month(),clock.day(),clock.hour(),clock.minute(),clock.second()); // my_rtc also set to the same time
  
  print_rtcclock();
}

void format_display_clock(char *buf, size_t buflen, bool include_zone_name) {
  if (buf == nullptr || buflen == 0) return;

  if (clock_display_mode == 1) {
    // The internal clock is maintained as UTC+9. Convert to UTC for display only.
    time_t utc_epoch = static_cast<time_t>(my_rtc.unixtime()) - 9 * 60 * 60;
    struct tm utc_tm;
    gmtime_r(&utc_epoch, &utc_tm);
    if (include_zone_name) {
      snprintf(buf, buflen, "%02d:%02d:%02dU (UTC)",
               utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
    } else {
      snprintf(buf, buflen, "%02d:%02d:%02dU",
               utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
    }
  } else {
    if (include_zone_name) {
      snprintf(buf, buflen, "%02d:%02d:%02d (JST)",
               my_rtc.hour(), my_rtc.minute(), my_rtc.second());
    } else {
      snprintf(buf, buflen, "%02d:%02d:%02d",
               my_rtc.hour(), my_rtc.minute(), my_rtc.second());
    }
  }
}

static int64_t my_rtc_local_ms() {
  return (int64_t)my_rtc.unixtime() * 1000LL + (int64_t)my_rtc.msec;
}

static int64_t system_local_ms(const timeval &systime) {
  // my_rtc stores JST calendar fields as a DateTime-like local epoch.
  return ((int64_t)systime.tv_sec + 9LL * 60LL * 60LL) * 1000LL +
         (int64_t)(systime.tv_usec / 1000);
}

static void service_pending_rtc_ntp_adjust() {
  if (!rtc_ntp_adjust_pending) return;
  if ((int32_t)(millis() - rtc_ntp_adjust_due_ms) < 0) return;

  timeval systime;
  gettimeofday(&systime, NULL);
  const time_t local_epoch =
      systime.tv_sec + (time_t)(9 * 60 * 60);

  bool adjusted = false;
  if (i2c_bus_lock("rtc_ntp_set", pdMS_TO_TICKS(20))) {
    uint32_t t0 = micros();
    rtcclock.adjust((uint32_t)local_epoch);
    i2c_bus_unlock("rtc_ntp_set");
    i2c_diag_io("rtc_ntp_set", micros() - t0);
    adjusted = true;
  }

  if (adjusted) {
    console->printf("RTC reset by NTP at system phase %ld ms\n",
                    (long)(systime.tv_usec / 1000));
    rtc_ntp_adjust_pending = false;
    rtcadj_count = 0;
  } else {
    // I2C was temporarily busy.  Retry shortly without blocking anything.
    rtc_ntp_adjust_due_ms = millis() + 50;
  }
}

void timekeep() {
  //  DateTime rtctime_bak;

  service_pending_rtc_ntp_adjust();

  if (my_rtc.evt_second) {
    my_rtc.evt_second=0;

    // Satellite LCD also contains the clock. Refresh it from the same
    // my_rtc second-boundary event as the normal clock display instead of
    // from an independent millis()+1000 timer.
    if (plogw->sat) {
      upd_display_sat();
    }
    
    char datestr[40];

    if (i2c_bus_lock("rtc_now", pdMS_TO_TICKS(10))) {
      uint32_t t0 = micros();
      rtctime = rtcclock.now();
      i2c_bus_unlock("rtc_now");
      i2c_diag_io("rtc_now", micros() - t0);
    } else {
      rtctime = DateTime(my_rtc.year(), my_rtc.month(), my_rtc.day(),
                         my_rtc.hour(), my_rtc.minute(), my_rtc.second());
    }

    // my_rtc is the DVPlogger master clock.  DS1307 is the always-available
    // startup/holdover reference; NTP-synchronized system time is the more
    // accurate reference whenever it is available.
    TimeSpan dt1;
    int32_t dt;
    dt1 = my_rtc - rtctime;
    dt = dt1.totalseconds();
    if (verbose & VERBOSE_PERF) {
      snprintf(buf, 128,
               "my_rtc %04d/%02d/%02d %02d:%02d:%02d.%03d "
               "dt(ds1307)=%d",
               my_rtc.year(), my_rtc.month(), my_rtc.day(),
               my_rtc.hour(), my_rtc.minute(), my_rtc.second(),
               my_rtc.msec, dt);
      console->println(buf);
    }

    const bool ntp_synced = network_ntp_synced();

    // DS1307 -> my_rtc slew is holdover behavior only.  Once NTP is valid,
    // do not let the coarse one-second RTC reference compete with NTP.
    if (!ntp_synced && dt != 0) {
      if (dt >= 1 && dt <= 10) {
        // my_rtc is ahead of DS1307: pause one 100 ms tick.
        console->print("my_rtc adj -100 dt=");
        console->println(dt);
        my_rtc.adj = -100;
      } else if (dt <= -1 && dt >= -10) {
        // my_rtc is behind DS1307: advance one extra 100 ms tick.
        console->print("my_rtc adj 100 dt=");
        console->println(dt);
        my_rtc.adj = 100;
      } else {
        console->println("my_rtc large difference with DS1307");
        snprintf(buf, 128,
                 "my_rtc %02d:%02d:%02d.%03d dt(ds1307)=%d "
                 "DS1307 %02d:%02d:%02d",
                 my_rtc.hour(), my_rtc.minute(), my_rtc.second(),
                 my_rtc.msec, dt,
                 rtctime.hour(), rtctime.minute(), rtctime.second());
        console->println(buf);
      }
    }

    timeval systime;
    struct tm local_tm;
    time_t system_local_sec = 0;
    if (ntp_synced) {
      gettimeofday(&systime, NULL);
      system_local_sec = systime.tv_sec + (time_t)(9 * 60 * 60);
      localtime_r(&systime.tv_sec, &local_tm);

      // NTP/system -> my_rtc: slew by at most 100 ms per my_rtc second.
      // A +/-99 ms dead band matches my_rtc's 100 ms resolution and avoids
      // hunting on scheduler/readout phase jitter.
      const int64_t error_ms =
          system_local_ms(systime) - my_rtc_local_ms();
      if (error_ms >= 100) {
        my_rtc.adj = 100;
      } else if (error_ms <= -100) {
        my_rtc.adj = -100;
      }

      if (verbose & VERBOSE_PERF) {
        console->printf(
            "NTP/system %04d/%02d/%02d %02d:%02d:%02d.%03ld "
            "my_rtc_error=%lld ms slew=%d\n",
            local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
            local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
            systime.tv_usec / 1000, (long long)error_ms, my_rtc.adj);
      }

      // NTP/system -> DS1307: retain the existing conservative policy.
      // Compare DS1307 directly with the accurate system reference, ignore
      // sub-2-second differences, and require 10 consecutive observations
      // before rewriting the RTC.
      const int64_t rtc_error_sec =
          (int64_t)rtctime.unixtime() - (int64_t)system_local_sec;
      const uint64_t rtc_abs_error_sec =
          rtc_error_sec < 0 ? (uint64_t)(-rtc_error_sec)
                            : (uint64_t)rtc_error_sec;

      if (!rtc_ntp_adjust_pending && rtc_abs_error_sec >= 2) {
        if (!plogw->f_console_emu) {
          plogw->ostream->printf("dt(DS1307-NTP)=%lld\n",
                                  (long long)rtc_error_sec);
        }

        rtcadj_count++;
        if (!plogw->f_console_emu) {
          plogw->ostream->print("rtcadj_count=");
          plogw->ostream->println(rtcadj_count);
        }

        if (rtcadj_count >= 10) {
          // Preserve the old "write at the next integer second" idea, but do
          // not delay() the main loop.  timekeep() services this deadline on
          // subsequent loop iterations.
          uint32_t wait_ms =
              1000U - (uint32_t)(systime.tv_usec / 1000);
          if (wait_ms == 0) wait_ms = 1000U;
          rtc_ntp_adjust_due_ms = millis() + wait_ms;
          rtc_ntp_adjust_pending = true;
          rtcadj_count = 0;
          if (!plogw->f_console_emu)
            plogw->ostream->println("RTC NTP reset scheduled");
        }
      } else if (!rtc_ntp_adjust_pending) {
        rtcadj_count = 0;
      }
    } else {
      rtcadj_count = 0;
    }


    time_measure_start_name(PROF_TIMEKEEP_DISPLAY, "time_disp");
    /*    sprintf(plogw->tm, "%02d/%02d/%02d-%02d:%02d:%02d", rtctime.year() % 100, rtctime.month(), rtctime.day(),
	  rtctime.hour(), rtctime.minute(), rtctime.second());*/
    // now refers to my_rtc
    sprintf(plogw->tm, "%02d/%02d/%02d-%02d:%02d:%02d", my_rtc.year() % 100, my_rtc.month(), my_rtc.day(),
	    my_rtc.hour(), my_rtc.minute(), my_rtc.second());
    if (verbose&VERBOSE_PERF) console->println(plogw->tm);
    // Update the RAM buffer first.
    //
    // Give a latency-sensitive remote DUPE reply a short chance to arrive
    // before the relatively slow OLED transfer, but never suppress the
    // once-per-second clock refresh indefinitely.
    if (f_mux_transport) mux_transport.recv_pkt();
    upd_display_tm();

    if (dupechk_remote_query_pending()) {
      const uint32_t wait_started_us = micros();
      static const uint32_t CLOCK_DUPE_ACK_BUDGET_US = 8000U;

      while (dupechk_remote_query_pending() &&
             (uint32_t)(micros() - wait_started_us) <
                 CLOCK_DUPE_ACK_BUDGET_US) {
        if (f_mux_transport) mux_transport.recv_pkt();
        task_dupechk();
        delay(1);
      }
    }

    if (f_mux_transport) mux_transport.recv_pkt();
    right_display_sendBuffer();
    if (f_mux_transport) mux_transport.recv_pkt();
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

      if (network_ntp_synced()) {
        tm display_tm;
        if (!plogw->f_console_emu) {
          timeval display_time;
          gettimeofday(&display_time, NULL);
          localtime_r(&display_time.tv_sec, &display_tm);

          plogw->ostream->print(" ");
          snprintf(datestr, sizeof(datestr),
                   "%04d/%02d/%02d %02d:%02d:%02d",
                   display_tm.tm_year + 1900, display_tm.tm_mon + 1,
                   display_tm.tm_mday, display_tm.tm_hour,
                   display_tm.tm_min, display_tm.tm_sec);
          plogw->ostream->print("NTP:");
          plogw->ostream->println(datestr);
        }
      } else {
        if (!plogw->f_console_emu) plogw->ostream->println("");
      }

    }
    time_measure_stop(PROF_TIMEKEEP_DISPLAY);
  }
}


