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
#include "dupechk.h"
#include "callhist.h"
#include "qso.h"
#include "display.h"
#include "multi_process.h"
#include "multi.h"
#include "log.h"
#include "cw_keying.h"
#include "tcp_server.h"
#include "timekeep.h"

#include "FS.h"
#include "SD.h"
//#include "SdFat.h"
#include "sd_files.h"
#include "SPI.h"
//#include "DS3231.h"
#include "zserver.h"
#include "misc.h"
#include "esp_task_wdt.h"
#include "so2r.h"


File qsologf;            // qso logf

void init_qsofiles() {
  strcpy(qsologfn, "/qso.txt");
  strcpy(callhistfn, "/callhist.txt");
}

void init_qso() {
  init_qsofiles();
  init_score();
  init_dupechk();
  init_callhist();
}


void makedupe_qso_entry() {
  // update dupe list from current qso
  // char *call, byte bandid, byte mask)

  // check
  //plogw->ostream->println("makedupe_qso_entry()");

  if (qso.entry.type[0] != 'Q') return;
  // set bandid and modetype according to what is written in the log (needed in dupe checking)
  int bandid, modetype;
  int bandmode;
  bandid = freq2bandid((unsigned long)(atoll(qso.entry.freq)/FREQ_UNIT));
  modetype = 0;
  for (int i = 0; i < 4; i++) {
    if (strncmp(qso.entry.mode, modetype_str[i], 2) == 0) {
      modetype = i;
      break;
    }
  }
  bandmode = bandmode_param(bandid,modetype);

  // update seqnr for the band
  plogw->seqnr_band[bandid-1]++;
  
  //  bandmode = bandid * 4 + modetype;
  //plogw->ostream->print("makedupe3");

  // const char *modetype_str[4] = {"*", "CW", "PH", "DG"};
  // plogw->ostream->println("makedupe_qso_entry()");
  if (!dupe_check_nocallhist(qso.entry.hiscall, bandmode, plogw->mask)) {
    if (dupechk->ncallsign < NMAXQSO) {
      //
      int i;
      i = dupechk->ncallsign;  // add to the last
      strcpy(dupechk->callsign[i], qso.entry.hiscall);
      strcpy(dupechk->exch[i], qso.entry.rcvexch);
      dupechk->bandmode[i] = bandmode;
      dupechk->ncallsign++;

      // update statistics in score
      score.worked[modetype == LOG_MODETYPE_CW ? 0 : 1][bandid - 1]++;

      // seqnr update
      if (isdigit(*qso.entry.seqnr)) {
        int seqnr;
        seqnr = atoi(qso.entry.seqnr);
        if (seqnr <= 3000) {
          if (plogw->seqnr < seqnr) {
            plogw->seqnr = seqnr + 1;
          }
        }
      }

      if (verbose & 1) {
        plogw->ostream->print(qso.entry.hiscall);
        plogw->ostream->print(" b ");
        plogw->ostream->print(bandid);
        plogw->ostream->print(" m ");
        plogw->ostream->print(modetype);
        plogw->ostream->print(" exch ");
        plogw->ostream->print(qso.entry.rcvexch);
        plogw->ostream->print(" added  #");
        plogw->ostream->println(dupechk->ncallsign);
      }

    } else {
      plogw->ostream->println(" dupechk overflow ");
    }

    //	entry_dupechk();
  } else {
    //
    if (verbose & 1) {
      plogw->ostream->print(qso.entry.hiscall);
      plogw->ostream->print(" b ");
      plogw->ostream->print(bandid);
      plogw->ostream->print(" m ");
      plogw->ostream->print(modetype);
      plogw->ostream->println(" already in dupechk");
    }
  }


  //	plogw->ostream->println("makedupe1");
  // multi check and entry multi
  int found;
  found = 0;
  int multi;

  if (*multi_list.multi != NULL) {
    //	plogw->ostream->println("makedupe");
    int len;
    char rcvexch[10];
    len = strlen(qso.entry.rcvexch);
    if ((plogw->multi_type == 1) || (plogw->multi_type == 3) || (plogw->multi_type == 4) ) {
      // jarl contest, ACAG , JA8
      len--;
    }
    if (len >= 1) {
      *rcvexch = '\0';

      strncat(rcvexch, qso.entry.rcvexch, len);
      if (verbose & 1) {
        plogw->ostream->print("len=");
        plogw->ostream->print(len);
        plogw->ostream->print(" rcvexch:");
        plogw->ostream->println(rcvexch);
      }
      for (multi = 0; multi < multi_list.n_multi[bandid-1]; multi++) {
        /*	plogw->ostream->print("multi=");
        	plogw->ostream->println(multi);
        	plogw->ostream->print(multi_list.multi->mul[multi]);
        	plogw->ostream->print("<-->");
        	plogw->ostream->print(qso.entry.rcvexch);
        	plogw->ostream->println(":");
        */
        if (strcmp(multi_list.multi[bandid-1]->mul[multi], rcvexch) == 0) {
          // hit
          found = 1;
          break;
        }
      }
    }
    if (found) {
      /*	plogw->ostream->print("bandid=");
      	plogw->ostream->print(bandid);
      	plogw->ostream->print(" multi= ");plogw->ostream->println(multi);
      */
      // valid multiplier, so entry into multi check list for the band

      if (multi_list.multi_worked[bandid - 1][multi] == 0) {
        // new multi found
        score.nmulti[bandid - 1]++;
        if (verbose & 1) {
          plogw->ostream->print("new multi:");
          plogw->ostream->print(qso.entry.rcvexch);
          plogw->ostream->println(" nmulti=");
          plogw->ostream->println(score.nmulti[bandid - 1]);
        }
      }
      multi_list.multi_worked[bandid - 1][multi] = 1;
    }
  }
  //upd_display_info_contest_settings();
}

void reformat_qso_entry(union qso_union_tag *qso) {
  // reformatting the entry
  // time
  string_trim_right(qso->entry.tm, ' ');
  //    plogw->ostream->print(" T:");
  qso->entry.tm[8] = ' ';  // put a space between date and time


  string_trim_right(qso->entry.seqnr, ' ');
  //plogw->ostream->print("seqnr:");plogw->ostream->print(qso.entry.seqnr);plogw->ostream->println(":");

  string_trim_right(qso->entry.hiscall, ' ');
  string_trim_right(qso->entry.mycall, ' ');
  //plogw->ostream->print(" Callsign:");

  string_trim_right(qso->entry.freq, ' ');
  string_trim_right(qso->entry.opmode, ' ');
  string_trim_right(qso->entry.sentrst, ' ');
  // sent rst
  if (strncmp(qso->entry.mode, "PH", 2) == 0) {
    if (strcmp(qso->entry.sentrst, "599") == 0) {
      // phone but RST
      qso->entry.sentrst[2] = '\0';
    }
  }
  string_trim_right(qso->entry.sentexch, ' ');

  string_trim_right(qso->entry.rcvrst, ' ');
  if (strncmp(qso->entry.mode, "PH", 2) == 0) {
    if (strcmp(qso->entry.rcvrst, "599") == 0) {
      // phone but RST --> remove T
      qso->entry.rcvrst[2] = '\0';
    }
  }

  string_trim_right(qso->entry.rcvexch, ' ');
  string_trim_right(qso->entry.remarks, '\n');
}


// read the previous qso and print
void read_qso_log(int option) {
  // seek to the first byte and dump

  int pos, memo_pos;
  int len;
  int ret;
  len = sizeof(qso.all);

  pos = qsologf.position();
  memo_pos = pos;

  // pos = pos - len;  // start from the end record
  pos = 0;  // start from the beginning


  int count;
  count = 0;

  while (1) {
    if (!qsologf.seek(pos)) {
      if (!plogw->f_console_emu) plogw->ostream->println("file seek failed");
      goto end;
    }
    //    Serial.print("-");    
    ret = qsologf.read(qso.all, len);
    delay(1);
    //    Serial.print("*");
    if (ret != len) {
      //
      if (!plogw->f_console_emu) {
        plogw->ostream->print("qso not read bytes=");
        plogw->ostream->println(ret);
      }
      goto end;
    } else {
      //      plogw->ostream->print("read ");
      //      plogw->ostream->print(ret);
      //      plogw->ostream->println("bytes");
    } 
    if (option & READQSO_PRINT) {
      sprintf(dp->lcdbuf, "Reading QSO\nRead %d bytes", pos);
      upd_display_info_flash(dp->lcdbuf);
    }
    //    Serial.print("a");
    // check type
    if (qso.entry.type[0] != 'Q') {
      // not vaild qso
      if (!plogw->f_console_emu) {
	plogw->ostream->print("type:");		
	plogw->ostream->print(qso.entry.type[0]);
	plogw->ostream->print(" all:");			
	plogw->ostream->print((char *)(qso.all));	
	plogw->ostream->println(":not valid qso encountered");
      }
      //      goto end; // before finish
      //      skip
      pos = pos + len;
      continue;
    }
    // print content
    //    plogw->ostream->print("Pos:");plogw->ostream->println(pos);
    //    plogw->ostream->println("");

    reformat_qso_entry(&qso);
    //    Serial.print("b");

    // operations

    if (option & READQSO_PRINT) print_qso_entry(&qso);
    if (option & READQSO_MAKEDUPE) makedupe_qso_entry();
    esp_task_wdt_reset();  // WDTをリセット

    //    Serial.print("c");
    pos = pos + len;
  }

end:
  sprintf(dp->lcdbuf, "Reading QSO\nFinished\nPos=%d\n",pos);

  upd_display_info_flash(dp->lcdbuf);
  if (!plogw->f_console_emu) plogw->ostream->println("end of read_qso_log");
  plogw->ostream->print("Pos:");plogw->ostream->println(pos);    
  if (!qsologf.seek(memo_pos)) {
    if (!plogw->f_console_emu) plogw->ostream->println("file seek to end failed");
  }
  delay(300);
  upd_display_info_contest_settings(so2r.radio_selected());
  return;
}

int read_qso_log_to_file() {
  // seek to the first byte and dump

  int pos, memo_pos;
  int len;
  int ret;
  len = sizeof(qso.all);

  pos = qsologf.position();
  memo_pos = pos;


  f = SD.open("/qsomail.txt", FILE_WRITE);

  if (!f) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file qsomail.txt for writing");
    return 0;
  }

  // pos = pos - len;  // start from the end record
  pos = 0;  // start from the beginning

  int count;
  count = 0;
  plogw->ostream->println("read qso log to file started");
  while (1) {
    if (!plogw->f_console_emu) plogw->ostream->println(pos);
    if (!qsologf.seek(pos)) {
      if (!plogw->f_console_emu) plogw->ostream->println("file seek failed");
      goto end;
    }
    ret = qsologf.read(qso.all, len);
    if (ret != len) {
      //
      if (!plogw->f_console_emu) plogw->ostream->print("qso not read bytes=");
      plogw->ostream->println(ret);
      goto end;
    }

    sprintf(dp->lcdbuf, "Mail QSO Log\nRead %d bytes", pos);
    upd_display_info_flash(dp->lcdbuf);

    // check type
    if (qso.entry.type[0] != 'Q') {
      // not vaild qso
      if (!plogw->f_console_emu) plogw->ostream->println("not valid qso encountered");

      //goto end;
    } else {
      // print content
      //plogw->ostream->print("Pos:");plogw->ostream->print(pos);
      //plogw->ostream->print(" ");

      reformat_qso_entry(&qso);


      // operations

      print_qso_entry_file(&f);
    }
    pos = pos + len;
  }

end:
  f.close();
  if (!plogw->f_console_emu) plogw->ostream->println("end of read_qso_log_to_file");
  if (!qsologf.seek(memo_pos)) {
    if (!plogw->f_console_emu) plogw->ostream->println("file seek to end failed");
    return 0;
  }
  return 1;
}

// qso entry -> radio qso edit buffers fill
void set_qsodata_from_qso_entry() {
  struct radio *radio;
  radio = so2r.radio_selected();
  if (!plogw->f_console_emu) plogw->ostream->println("set_qsodata_from_qso_entry");
  //  strcpy(radio->tm_loaded,"20");
  strcpy(radio->tm_loaded, qso.entry.tm);
  radio->tm_loaded[8] = '-';  // restore '-' between date and time
  long long tmp;
  sscanf(qso.entry.freq, "%lld", &tmp);
  radio->freq_loaded=tmp/FREQ_UNIT;
  sscanf(qso.entry.seqnr, "%d", &radio->seqnr_loaded);
  strcpy(radio->opmode_loaded, qso.entry.opmode);
  //  strcpy(plogw->my_callsign + 2,qso.entry.mycall); // do not load my_callsign
  strcpy(radio->sent_rst + 2, qso.entry.sentrst);
  radio->sent_rst[1] = strlen(radio->sent_rst + 2);
  // strcpy(plogw->sent_exch + 2,qso.entry.sentexch); // do not load sent_exch
  strcpy(radio->callsign + 2, qso.entry.hiscall);
  radio->callsign[1] = strlen(radio->callsign + 2);
  strcpy(radio->recv_rst + 2, qso.entry.rcvrst);
  radio->recv_rst[1] = strlen(radio->recv_rst + 2);

  strcpy(radio->recv_exch + 2, qso.entry.rcvexch);
  radio->recv_exch[1] = strlen(radio->recv_exch + 2);
  strcpy(radio->remarks + 2, qso.entry.remarks);
  radio->remarks[1] = 0;
}

void print_qso_entry_file(File *f) {
  // print entry
  f->print("20");
  f->write((uint8_t *)qso.entry.tm, strlen(qso.entry.tm));
  f->print(" ");

  // callsign
  f->write((uint8_t *)qso.entry.hiscall, strlen(qso.entry.hiscall));
  f->print(" ");

  // freq
  unsigned long freq;
  freq = atoll(qso.entry.freq)/FREQ_UNIT; // freq in FREQ_UNIT Hz
  char buf[10];
  dtostrf((float)freq / (1000000/FREQ_UNIT), 5, 1, buf);
  f->print(buf);
  f->print(" ");

  // opmode
  f->write((uint8_t *)qso.entry.opmode, strlen(qso.entry.opmode));
  f->print(" ");

  // check rst in phone

  f->write((uint8_t *)qso.entry.sentrst, strlen(qso.entry.sentrst));
  //plogw->ostream->print(" ");

  // sentexch
  f->write((uint8_t *)qso.entry.sentexch, strlen(qso.entry.sentexch));
  f->print(" ");

  // rcv rst
  f->write((uint8_t *)qso.entry.rcvrst, strlen(qso.entry.rcvrst));
  //plogw->ostream->print(" ");

  // rcvexch
  f->write((uint8_t *)qso.entry.rcvexch, strlen(qso.entry.rcvexch));
  f->print(" ");

  // remarks
  f->write((uint8_t *)qso.entry.remarks, strlen(qso.entry.remarks));
  f->println("");
}


void open_qsolog() {
  // open qsolog
  // appendFile(fs::FS &fs, const char * path, const char * message){
  if (!plogw->f_console_emu) plogw->ostream->printf("Appending to file: %s\n", qsologfn);

  qsologf = SD.open(qsologfn, "a+");

  if (!qsologf) {
    if (!plogw->f_console_emu) plogw->ostream->println("Failed to open file for appending");
    return;
  }
  //    if(qsologf.print("test item")){
  //        plogw->ostream->println("Message appended");
  //    } else {
  //        plogw->ostream->println("Append failed");
  //    }


  //dump_qso_log();

  //    file.close();
}


void close_qsolog() {
  if (qsologf) {
    qsologf.close();
    if (verbose&4) 	    console->println("closed qsolog.");
  }
}

void create_new_qso_log() {
  if (!plogw->f_console_emu) plogw->ostream->println("Creating new QSO logfile");

  // create backup of current QSO log
  qsologf.close();

  char fname[30];
  int n;
  n = 0;
  while (1) {
    sprintf(fname, "/qsobak.%03d", n);
    if (SD.exists(fname)) {
      n++;
      continue;
    }
    if (SD.rename(qsologfn, fname) == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("Rename QSO logfile failed");
    }
    if (!plogw->f_console_emu) {
      plogw->ostream->print(qsologfn);
      plogw->ostream->print(" -> ");
      plogw->ostream->println(fname);
    }
    break;
  }

  open_qsolog();

  // reset  dupe check
  init_dupechk();
  init_score();
  //

  plogw->seqnr = 0;
  new_log_entry();
}



void sprint_qso_entry(char *buf,union qso_union_tag *qso) {
  // print entry
  if (plogw->f_console_emu) return;

  //  Serial.print("d");
  *buf='\0';
  
  strcat(buf,"20");
  strcat(buf,qso->entry.tm);
  strcat(buf," ");

  // callsign
  strcat(buf,qso->entry.hiscall);
  strcat(buf," ");

  // freq
  unsigned long freq;
  freq = atoll(qso->entry.freq)/FREQ_UNIT;
  char bufa[10];
  dtostrf((double)freq / (1000000/FREQ_UNIT), 5, 1, bufa);
  strcat(buf,bufa);
  strcat(buf," ");

  // opmode
  strcat(buf,qso->entry.opmode);
  strcat(buf," ");

  // check rst in phone

  strcat(buf,qso->entry.sentrst);
  //plogw->ostream->print(" ");

  // sentexch
  strcat(buf,qso->entry.sentexch);
  strcat(buf," ");

  // rcv rst
  strcat(buf,qso->entry.rcvrst);
  //plogw->ostream->print(" ");

  // rcvexch
  strcat(buf,qso->entry.rcvexch);
  strcat(buf," ");

  // remarks
  strcat(buf,qso->entry.remarks);
  strcat(buf,"\r\n");
  //  Serial.print("e");
  //  plogw->ostream->println(buf);
  
}
void print_qso_entry(union qso_union_tag *qso) {
  sprint_qso_entry(buf,qso);
  //  Serial.print("f");    
  plogw->ostream->write(buf,strlen(buf));
  //  Serial.print("g");  
}


// qso->entry.tm の形式をstruct tm に変換
//"%02d/%02d/%02d-%02d:%02d:%02d",
struct tm parse_datetime(const char *datetime_str) {
    struct tm tm_result = {0};
    int year, month, day, hour, min, sec;
    /*    console->print("datetime_str:");console->print(datetime_str);*/
    if (sscanf(datetime_str, "%2d/%2d/%2d %2d:%2d:%2d",
               &year, &month, &day, &hour, &min, &sec) == 6) {
        // 年は2000年以降と仮定（25 -> 2025）
        tm_result.tm_year = year + 100;
        tm_result.tm_mon  = month - 1;   // 0〜11
        tm_result.tm_mday = day;
        tm_result.tm_hour = hour;
        tm_result.tm_min  = min;
        tm_result.tm_sec  = sec;

	/*	console->print(" yr:");console->print(year);
	console->print(" mn:");console->print(month);
	console->print(" day:");console->print(day);
	console->print(" hr:");console->print(hour);
	console->print(" min:");console->print(min);
	console->print(" sec:");console->println(sec);*/
    } else {
      //      console->print("parse_datetime: 文字列の解析に失敗しました\n");
    }

    return tm_result;
}

int strcpy_to_chr(char *dest, char *src,char c) {
    const char *colon = strchr(src, c);
    size_t len = 0;

    if (colon != NULL) {
        len = colon - src;  // ':'までの長さ
    } else {
        len = strlen(src);  // ':'がなければ全文字列
    }

    strncpy(dest, src, len);
    dest[len] = '\0';  // null終端
    return len;
}

char *parse_strings(const char *remarks,char *parse_str) {
  char *p;
  char tmpbuf1[100];
  if ((p=strstr(remarks,parse_str))!=NULL) { // my park information in POTA activation
    strcpy(tmpbuf1,p+strlen(parse_str));
    p=strtok(tmpbuf1," ");
    if (p!=NULL) {
      return p;
    }
  }
  return NULL;
}


void sprint_qso_entry_hamlogcsv(char *buf,union qso_union_tag *qso) {
  char tmpbuf[200];
  int len;
  //1.  No                : 1
  //  sprintf(tmpbuf,"%s,",qso->entry.seqnr);
  //  strcat(buf,tmpbuf);

  //4.  相手局コールサイン     : JA1ZLO
  sprintf(tmpbuf,"%s,",qso->entry.hiscall);
  strcat(buf,tmpbuf);
  
  //2.  交信日             : 2025/06/15
  //3.  交信時刻           : 1330
  //<QSO_DATE:8>20230611

  struct tm jst_tm;  
  jst_tm=parse_datetime(qso->entry.tm);
  // convert to gmt
  time_t jst_time = mktime(&jst_tm);
  //  time_t utc_time = jst_time - 9 * 3600;
  //  console->println(jst_time);
  //  console->println(utc_time);
  //  struct tm utc_tm = *localtime(&utc_time);  // UTCに変換
  struct tm utc_tm = *localtime(&jst_time);  // UTCに変換  
  sprintf(tmpbuf,"%04d/%02d/%02d,",
	  utc_tm.tm_year+1900, utc_tm.tm_mon + 1, utc_tm.tm_mday);
  strcat(buf,tmpbuf);
  //<TIME_ON:4>0016  
  sprintf(tmpbuf,"%02d:%02dJ,",utc_tm.tm_hour, utc_tm.tm_min);
  strcat(buf,tmpbuf);

  //5.  送信RST           : 599
  //<RST_SENT:2>59
  sprintf(tmpbuf,"%s,",qso->entry.sentrst);
  strcat(buf,tmpbuf);
  
  //6.  受信RST           : 599
  //<RST_RCVD:2>59
  sprintf(tmpbuf,"%s,",qso->entry.rcvrst);
  strcat(buf,tmpbuf);
  //7.  周波数（MHz）      : 7
  //<BAND:2>2m
  float tmp;
  int ret;
  ret=sscanf(qso->entry.freq,"%f",&tmp);
  sprintf(tmpbuf,"%.5f,",tmp/1000000.0);
  strcat(buf,tmpbuf);
  //8.  モード             : CW
  //<MODE:2>FM
  sprintf(tmpbuf,"%s,",qso->entry.opmode);
  strcat(buf,tmpbuf);
  // code  blank
  // gl    blank
  //11. QSL送受           : J   （J:発行済 / N:未発行 / W:希望）
  char *p;

  if ((p=strstr(qso->entry.remarks,"JARL"))!=NULL) {
    strcat(buf,",,J,");
  } else if ((p=strstr(qso->entry.remarks,"hQSL"))!=NULL) {
    strcat(buf,",,H,");
  } else {
    strcat(buf,",,,");
  }
  // Hisname    blank
  // QTH        blank
  strcat(buf,",,");
  
  // Remarks1  --> received_exchange % Op Location J: POTA_MY: SOTA_MY: args from Remarks mycall %
  strcat(buf,"\"");  
  strcat(buf,qso->entry.rcvexch);
  strcat(buf," %");
  if ((p=parse_strings(qso->entry.remarks,"J:"))!=NULL) {
    strcat(buf,"JCC/JCG:");
    strcat(buf,p);
    strcat(buf," ");    
  }
  
  if ((p=parse_strings(qso->entry.remarks,"POTA_MY:"))!=NULL) {
    strcat(buf,"POTA_MY:");
    strcat(buf,p);
    strcat(buf," ");    
  }
  
  if ((p=parse_strings(qso->entry.remarks,"SOTA_MY:"))!=NULL) {
    strcat(buf,"SOTA_MY:");
    strcat(buf,p);
    strcat(buf," ");    
  }
  strcat(buf,qso->entry.mycall);
  strcat(buf,"%\",");
      
  // Remarks2  --> % contest_name % Remarks Sent_exchange
  // 0 ???
  // hamloguser or not
  strcat(buf,"\"");
  if (strlen(plogw->contest_name+2)>0) {
    strcat(buf,"%");
    strcat(buf,plogw->contest_name+2);
    strcat(buf,"% ");    
  }
  strcat(buf,qso->entry.sentexch);
  strcat(buf," ");
  strcat(buf,qso->entry.remarks);
  strcat(buf,"\",");
  // JM1LDV/1,25/08/16,22:23J,599,599,7.01502,CW,CODE,GL,J,HisName,QTH,Remarks1,Remarks2,0,
  strcat(buf,"\n");
}

void sprint_qso_entry_adif(char *buf,union qso_union_tag *qso) {
  *buf='\0';
  char tmpbuf[300],tmpbuf1[100];
  size_t len;

  // sprint qso entry in adif format
  //<EOH>
  //  strcat(buf,"<EOH>");
  //<STATION_CALLSIGN:6>JP7VAI

  //<CALL:6>JA7YAB    
  len=strlen(qso->entry.hiscall);
  sprintf(tmpbuf,"<CALL:%d>%s",len,qso->entry.hiscall);
  strcat(buf,tmpbuf);
  //
  sprintf(tmpbuf,"<STATION_CALLSIGN:%d>%s",strlen(qso->entry.mycall),qso->entry.mycall);
  strcat(buf,tmpbuf);
  //<QSO_DATE:8>20230611
  struct tm jst_tm;  
  jst_tm=parse_datetime(qso->entry.tm);
  // convert to gmt
  time_t jst_time = mktime(&jst_tm);
  time_t utc_time = jst_time - 9 * 3600;
  //  console->println(jst_time);
  //  console->println(utc_time);
  struct tm utc_tm = *localtime(&utc_time);  // UTCに変換
  sprintf(tmpbuf,"<QSO_DATE:8>%04d%02d%02d",
	  utc_tm.tm_year+1900, utc_tm.tm_mon + 1, utc_tm.tm_mday);
  strcat(buf,tmpbuf);
  //<TIME_ON:4>0016  
  sprintf(tmpbuf,"<TIME_ON:4>%02d%02d",utc_tm.tm_hour, utc_tm.tm_min);
  strcat(buf,tmpbuf);
  
  //<MODE:2>FM
  len=strlen(qso->entry.opmode);
  if ((strcmp(qso->entry.opmode,"USB")==0)||(strcmp(qso->entry.opmode,"LSB")==0)) {
    strcat(buf,"<MODE:3>SSB");
    sprintf(tmpbuf,"<SUBMODE:%d>%s",strlen(qso->entry.opmode),qso->entry.opmode);
    strcat(buf,tmpbuf);    
  } else if ((strcmp(qso->entry.opmode,"CW")==0)||(strcmp(qso->entry.opmode,"CW-R")==0)) {
    strcat(buf,"<MODE:2>CW");
  } else {
    sprintf(tmpbuf,"<MODE:%d>%s",strlen(qso->entry.opmode),qso->entry.opmode);
    strcat(buf,tmpbuf);        
  }
  
  //<BAND:2>2m
  int tmp,ret;
  tmp=1;
  ret=sscanf(qso->entry.band,"%d",&tmp);
  sprintf(tmpbuf,"<BAND:%d>%s",strlen(band_str_adif[tmp-1]),band_str_adif[tmp-1]);
  strcat(buf,tmpbuf);

  //<RST_RCVD:2>59
  sprintf(tmpbuf,"<RST_RCVD:%d>%s",strlen(qso->entry.rcvrst),qso->entry.rcvrst);
  strcat(buf,tmpbuf);

  // SRX_STRING
  if (strlen(qso->entry.rcvexch)>0) {
    sprintf(tmpbuf,"<SRX_STRING:%d>%s",strlen(qso->entry.rcvexch),qso->entry.rcvexch);
    strcat(buf,tmpbuf);    
  }
  
  //<RST_SENT:2>59
  sprintf(tmpbuf,"<RST_SENT:%d>%s",strlen(qso->entry.sentrst),qso->entry.sentrst);
  strcat(buf,tmpbuf);
  // STX_STRING
  if (strlen(qso->entry.sentexch)>0) {
    sprintf(tmpbuf,"<STX_STRING:%d>%s",strlen(qso->entry.sentexch),qso->entry.sentexch);
    strcat(buf,tmpbuf);    
  }
  
  //<MY_SIG:4>POTA
  //<MY_SIG_INFO:7>JA-0110
  //<SIG_INFO>

  char *p;
  if ((p=parse_strings(qso->entry.remarks,"POTA_MY:"))!=NULL) {
    strcat(buf,"<MY_SIG:4>POTA");
    sprintf(tmpbuf,"<MY_SIG_INFO:%d>%s",strlen(p),p);
    strcat(buf,tmpbuf);
  }
  if ((p=parse_strings(qso->entry.remarks,"POTA:"))!=NULL) {
    strcat(buf,"<SIG:4>POTA");
    sprintf(tmpbuf,"<SIG_INFO:%d>%s",strlen(p),p);
    strcat(buf,tmpbuf);
  }
  if ((p=parse_strings(qso->entry.remarks,"SOTA_MY:"))!=NULL) {
    strcat(buf,"<MY_SIG:4>SOTA");
    sprintf(tmpbuf,"<MY_SIG_INFO:%d>%s",strlen(p),p);
    strcat(buf,tmpbuf);
  }
  if ((p=parse_strings(qso->entry.remarks,"SOTA:"))!=NULL) {
    strcat(buf,"<SIG:4>SOTA");
    sprintf(tmpbuf,"<SIG_INFO:%d>%s",strlen(p),p);
    strcat(buf,tmpbuf);
  }
  /*
  char *p;
  if ((p=strstr(qso->entry.remarks,"POTA_MY:"))!=NULL) { // my park information in POTA activation
    // pota information in remarks
    strcpy(tmpbuf1,p+8);
    p=strtok(tmpbuf1," ");
    if (p!=NULL) {
      // got POTA token
      strcat(buf,"<MY_SIG:4>POTA");
      sprintf(tmpbuf,"<MY_SIG_INFO:%d>%s",strlen(p),p);
      strcat(buf,tmpbuf);
    }
    // SOTA in similar way
  }
  if ((p=strstr(qso->entry.remarks,"POTA:"))!=NULL) { // hunting park information 
    // pota information in remarks
    strcpy(tmpbuf1,p+5);
    p=strtok(tmpbuf1," ");
    if (p!=NULL) {
      // got POTA token
      strcat(buf,"<SIG:4>POTA");
      sprintf(tmpbuf,"<SIG_INFO:%d>%s",strlen(p),p);
      strcat(buf,tmpbuf);
    }
  }
  */
  //<OPERATOR:6>JP7VAI
  len=strcpy_to_chr(tmpbuf1,qso->entry.mycall,'/');
  sprintf(tmpbuf,"<OPERATOR:%d>%s",len,tmpbuf1);
  strcat(buf,tmpbuf);
  //<COMMENT>
  if (strlen(qso->entry.remarks)>0) {
    sprintf(tmpbuf,"<COMMENT:%d>%s",strlen(qso->entry.remarks),qso->entry.remarks);
    strcat(buf,tmpbuf);
  }
  //<EOR>
  strcat(buf,"<EOR>\n");
  
}
  

void string_trim_right(char *s, char c) {
  // replace first encountered c with '\0'

  while (*s) {
    if (*s == c) {
      *s = '\0';
      return;
    }
    s++;
  }
}
void print_qso_logfile() {
  // check qsologf is open
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile ()");
  }

  if (!qsologf) {
    if (!plogw->f_console_emu) plogw->ostream->println("qso logfile is not open.");
    return;
  }
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile():1");
  }

  make_qsolog_entry();
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile():2");
  }
  int len = sizeof(qso.all);
  int ret;
  ret = qsologf.write(qso.all, len);
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile():3");
  }

  if (verbose & 1) {
    plogw->ostream->print("written QSO ,");
    plogw->ostream->print(ret);
    plogw->ostream->println("bytes");
  }
  qsologf.flush();
  if (verbose&4) 	{
    if (!plogw->f_console_emu) plogw->ostream->println("print_qso_logfile():4");
  }

  if (!plogw->f_console_emu) plogw->ostream->write(qso.all, len);
  if (!plogw->f_console_emu) {  
    write_allTCPclients((char *)qso.all,len);
    print_allTCPclients("\n");
  }
			
  /*  
  if (!plogw->f_console_emu) {
    for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
      if (serverClients[i] && serverClients[i].connected()) {
        plogw->ostream = &serverClients[i];
        plogw->ostream->write(qso.all, len);
        plogw->ostream->println("");
      }
    }
    plogw->ostream = console;
  }
  */

}

void print_qso_log() {
  // print content of logw
  if (!plogw->f_console_emu) plogw->ostream->println("print_qso_log()");
  struct radio *radio;
  //  radio = so2r.radio_selected();
  radio=so2r.radio_qso_process();

  if (verbose & 1) {
    plogw->ostream->print("QSO:");
    plogw->ostream->print(plogw->tm);
    plogw->ostream->print(",Freq,");
    plogw->ostream->print(radio->freq);
    plogw->ostream->print(",Mode,");
    plogw->ostream->print(radio->opmode);
    plogw->ostream->print(",call,");
    plogw->ostream->print(radio->callsign + 2);
    plogw->ostream->print(",rcvd rst,");
    plogw->ostream->print(radio->recv_rst + 2);
    plogw->ostream->print(",rcvd exch,");
    plogw->ostream->print(radio->recv_exch + 2);
    plogw->ostream->print(",my call,");
    plogw->ostream->print(plogw->my_callsign + 2);
    plogw->ostream->print(",sent rst,");
    plogw->ostream->print(radio->sent_rst + 2);
    plogw->ostream->print(",sent exch,");
    plogw->ostream->print(plogw->sent_exch + 2);
    plogw->ostream->println("");
  }
  print_qso_logfile();
  make_zlogqsodata(buf);
}

void expand_sent_exch(char *out, size_t out_size)
{
    struct radio *radio = so2r.radio_qso_process();
    char tmpbuf[40];

    if ((radio->bandid >= 11) && (radio->bandid <= 13)) {
        copy_token(tmpbuf, plogw->sent_exch + 2, 1, "/,;");
    } else {
        copy_token(tmpbuf, plogw->sent_exch + 2, 0, "/,;");
    }
    expand_macro_string(out,out_size,tmpbuf);
}

// create a single QSO log file entry (fixed length string)
void make_qsolog_entry() {
  struct radio *radio;
  char stmp[20];
  char tmp_buf[100];
  char *p1;
  //  radio = so2r.radio_selected();
  radio= so2r.radio_qso_process();
  // clear all
  memset(qso.all, ' ', sizeof(qso.all));
  strcpy(qso.entry.type, "Q");
  if (radio->qsodata_loaded) {
    // freq_loaded and tm_loaded are wrongly encoded 22/7/12
    // because frequency is not loaded correctly (shifted) at   set_qsodata_from_qso_entry()

    //    sprintf(qso.entry.freq, "%-10lld", radio->freq_loaded*((long long)FREQ_UNIT));
    sprintf(stmp, "%-11lld", radio->freq_loaded*((long long)FREQ_UNIT));
    strncpy(qso.entry.freq,stmp,11);
    strcpy(qso.entry.tm, radio->tm_loaded);
    sprintf(qso.entry.seqnr, "%-d", radio->seqnr_loaded);
    sprintf(qso.entry.band, "%-d", freq2bandid(radio->freq_loaded));
    strcpy(qso.entry.opmode, radio->opmode_loaded);
    strcpy(qso.entry.mode, modetype_str[modetype_string(radio->opmode_loaded)]);
  } else {
    //    sprintf(qso.entry.freq, "%-10lld", radio->freq*((long long)FREQ_UNIT));
    sprintf(stmp, "%-11lld", radio->freq*((long long)FREQ_UNIT));
    strncpy(qso.entry.freq,stmp,11);
    strcpy(qso.entry.tm, plogw->tm);
    sprintf(qso.entry.seqnr, "%-d", plogw->seqnr);
    sprintf(qso.entry.band, "%-d", freq2bandid(radio->freq));
    strcpy(qso.entry.opmode, radio->opmode);
    strcpy(qso.entry.mode, modetype_str[modetype_string(radio->opmode)]);
  }
  strcpy(qso.entry.mycall, plogw->my_callsign + 2);
  strcpy(qso.entry.sentrst, radio->sent_rst + 2);
  
  char sentexch_buf[100];
  expand_sent_exch(sentexch_buf, sizeof(sentexch_buf));
  strcpy(qso.entry.sentexch, sentexch_buf);
//  strcpy(qso.entry.sentexch, expand_sent_exch());
  strcpy(qso.entry.hiscall, radio->callsign + 2);
  strcpy(qso.entry.rcvrst, radio->recv_rst + 2);
  strcpy(qso.entry.rcvexch, radio->recv_exch + 2);
  qso.entry.remarks[0] = '\0';

  if (!radio->qsodata_loaded) {
    // add CQ/SP and TXnumber(7)/Random digits(2bytes) for determining QSOID for zLog (zserver)
    char tmpbuf[10];
    sprintf(tmpbuf,"%s %1d%02d ",
	    (radio->cq[radio->modetype]==LOG_CQ) ? "CQ":"SP",
	    (plogw->qsoid/100000000)%10,
	    (plogw->qsoid/100)%100);
    strcat(qso.entry.remarks,tmpbuf);

    if ((radio->modetype==LOG_MODETYPE_PH) && (radio->f_tone_keying)) {
      // F2A
      strcat(qso.entry.remarks,"F2A ");
    }
    if (plogw->sat) {
      // satellite qso add satellite name and grid locator before remarks
      strcat(qso.entry.remarks, plogw->sat_name_set);
      strcat(qso.entry.remarks, " ");
      strcat(qso.entry.remarks, plogw->grid_locator_set);
      strcat(qso.entry.remarks, " ");
      char buf[30];
      sprintf(buf, "O:%d ", sat_info[plogw->sat_idx_selected].offset_freq);
      strcat(qso.entry.remarks, buf);
    }
    if (strlen(plogw->jcc + 2) > 0) {
      // check POTA and SOTA number designators P: S: (after JCC/JCG numbers )
      if ((p1=strstr(plogw->jcc+2,"POTA/"))!=NULL) { // my park information in POTA activation
	char tmpbuf1[100];
	strcpy(tmpbuf1,p1+5);
	p1=strtok(tmpbuf1," ");
	if (p1!=NULL) {
	  strcat(qso.entry.remarks, "POTA_MY:");
	  strcat(qso.entry.remarks,p1);
	  strcat(qso.entry.remarks," ");
	}
      }
      if ((p1=strstr(plogw->jcc+2,"SOTA/"))!=NULL) { // my park information in POTA activation
	char tmpbuf1[100];
	strcpy(tmpbuf1,p1+5);
	p1=strtok(tmpbuf1," ");
	if (p1!=NULL) {
	  strcat(qso.entry.remarks, "SOTA_MY:");
	  strcat(qso.entry.remarks,p1);
	  strcat(qso.entry.remarks," ");
	}
      }
      if ((plogw->jcc[2]!='P') && (plogw->jcc[2]!='S')) { // jcc/jcg at the beginning
	char tmpbuf1[100];
	strcpy(tmpbuf1,plogw->jcc+2);
	p1=strtok(tmpbuf1," ");
	if (p1!=NULL) {
	  strcat(qso.entry.remarks, "J:");
	  strcat(qso.entry.remarks,p1);
	  strcat(qso.entry.remarks, " ");
	}
      }
    }
    if (radio->smeter_stat >= 1) {
      // record peak s-meter value
      if (radio->smeter_peak > SMETER_MINIMUM_DBM) {
        strcat(qso.entry.remarks, "S:");
        char buf[10];
		 dtostrf(radio->smeter_peak/(SMETER_UNIT_DBM*1.0),-1,1,buf);
        //sprintf(buf, "%d", radio->smeter_peak);
        strcat(qso.entry.remarks, buf);
        if (plogw->relay[0] != 0) {
          sprintf(buf, " A:%1d", plogw->relay[0] + plogw->relay[1] * 2);
          strcat(qso.entry.remarks, buf);
        }
        strcat(qso.entry.remarks, " ");
      }
      radio->smeter_stat = 0;  // end reading s-meter
    }
    switch(radio->f_qsl) {
    case 1: // JARL
      strcat(qso.entry.remarks,"JARL ");
      break;
    case 2: // hQSL
      strcat(qso.entry.remarks,"hQSL ");
      break;
    }
  } else {
    strcpy(qso.entry.remarks, "*E ");
  }
  strcat(qso.entry.remarks, radio->remarks + 2);
  strcat(qso.entry.remarks, "\n");

  // replace 0x00 with 0x20 (spc)
  uint8_t *p;
  int i;
  for (i = 0, p = qso.all; i < sizeof(qso.all); i++) {
    if (*p == 0x00) {
      *p = ' ';
    }
    p++;
  }
  *(p - 1) = 0x0d;  // last character is CR
}

void make_zlogqsodata(char *buf)
{
  struct radio *radio;
  //  radio = so2r.radio_selected();
  radio = so2r.radio_qso_process();  
  
  char buf1[50];
  long st; double stfloat;
  //  DateTime reftime;
  //  reftime=DateTime(2024,4,2);
  
  //  st=rtctime.unixtime();
  //  console->print("secondstime:");
  //  console->println(st);
  //  console->print("reftime 2024/4/2 00:00:00");
  //  console->println(reftime.unixtime()); // 1712016000/86400 = 19815 2024/4/2 00:00:00 day 45384 in zLog
  // unixtime()/86400 + (45384-19815) = zLog day
  // unixtime()%86400/86400.0 = zLog time
  *buf='\0';
  strcat(buf,"#ZLOG# PUTQSO ZLOGQSODATA:~"); // 1 identification
  // DateTime
  //  st=rtctime.unixtime();
  st=my_rtc.unixtime();
  if (verbose&4) 	{  
    console->print("tm:");
    console->println(plogw->tm);
    console->print("sec of day:");
    console->println(st%86400);
  }
  stfloat=(st%86400)/86400.0L;
  stfloat+=((st/86400)+(45384-19815));
  
  sprintf(buf1,"%.8lf~",stfloat);
  strcat(buf,buf1); // 2 datetime
  strcat(buf,radio->callsign+2);strcat(buf,"~"); // 3 callsign
  //  strcat(buf,plogw->sent_exch+2);strcat(buf,"~"); // 4 sent exch
  
  char sentexch_buf[100];
  expand_sent_exch(sentexch_buf, sizeof(sentexch_buf));
  
  strcat(buf,sentexch_buf);strcat(buf,"~"); // 4 sent exch
  strcat(buf,radio->recv_exch+2);strcat(buf,"~"); // 5 recv exch 
  strcat(buf,radio->sent_rst+2);strcat(buf,"~");  // 6 sent rst
  strcat(buf,radio->recv_rst+2);strcat(buf,"~"); // 7 recv rst
  sprintf(buf1,"%d",plogw->seqnr);strcat(buf,buf1);strcat(buf,"~"); // 8 serial
  sprintf(buf1,"%d",opmode2zLogmode(radio->opmode));strcat(buf,buf1);strcat(buf,"~"); // 9 mode
  sprintf(buf1,"%d",zserver_bandid_freqcodes_map[radio->bandid]);strcat(buf,buf1);strcat(buf,"~"); // 10 band 
  char *s;int tmp,pwr,txnum;
  s=power_code(radio->bandid);
  if (strcmp(s,"P")==0) {
    tmp=2;pwr=5;
  } else if (strcmp(s,"L")==0) {
    tmp=3;pwr=10;
  } else if (strcmp(s,"M")==0) {
    tmp=6;pwr=50;
  } else if (strcmp(s,"H")==0) {
    tmp=10;pwr=1000;
  } else {
    tmp=6;pwr=50;
  }
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~");  // 11 power code
  strcat(buf,"~"); // 12 multi1 ?
  strcat(buf,"~"); // 13 multi2 ?
  sprintf(buf1,"%d",0);strcat(buf,buf1);strcat(buf,"~"); // 14 new multi flag1 ?  
  sprintf(buf1,"%d",0);strcat(buf,buf1);strcat(buf,"~"); // 15 new multi flag2 ?
  tmp=  (radio->modetype == LOG_MODETYPE_CW) ? plogw->cw_pts : 1;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 16 points
  strcat(buf,plogw->my_name+2); strcat(buf,"~"); // 17 op name
  strcat(buf,radio->remarks+2); strcat(buf,"~"); // 18 memo
  tmp= radio->cq[radio->modetype]==LOG_CQ ? 1:0;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 19 CQ flag
  tmp= radio->dupe;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 20 Dupe flag
  tmp=0;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1); strcat(buf,"~");  // 21 reserve  in zlog 10 in here
  //  txnum = 7; // txnum =7+ hostname last letter digit (<3) so that txnum <=9
  txnum = plogw->txnum; // txnum =7+ hostname last letter digit (<3) so that txnum <=9  
  sprintf(buf1,"%d",txnum);strcat(buf,buf1);strcat(buf,"~"); // 22 tx number
  sprintf(buf1,"%d",pwr);strcat(buf,buf1);strcat(buf,"~"); // 23 power code 2 ARRL DX
  sprintf(buf1,"%d",0);strcat(buf,buf1);strcat(buf,"~"); // 24 reserve 2
  // qsoid
  //  tmp= (txnum)* 100000000 + plogw->seqnr *10000 + random(100)*100;
  tmp=plogw->qsoid;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 25 qsoid
  if (verbose&4) 	{
    console->print("Freq:");console->println(radio->freq*FREQ_UNIT);
  }
  sprintf(buf1,"%lld",radio->freq*((long long)FREQ_UNIT));strcat(buf,buf1);strcat(buf,"~"); // 26 Freq
  tmp=0; 
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 27 QSY ihan 0 no ihan 1 ihan
  strcat(buf,plogw->hostname+2);strcat(buf,"~"); // 28 pc name
  tmp=0;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 29 Force flag
  sprintf(buf1,"%d",tmp);strcat(buf,buf1);strcat(buf,"~"); // 30 QSL 0 not set 1 PSEQSL 2 NOQSL
  tmp=0;
  sprintf(buf1,"%d",tmp);strcat(buf,buf1); // 31 validity 0 valid 1 invalid
  // example from zLog
  //#ZLOG# PUTQSO ZLOGQSODATA:~45472.5651934954~JA1ZLO~11H~10H~599~599~1~0~12~3~10~~1~0~1~   ~~0~0~10~0~500~0~30252000  ~         ~0~thinkpad~0~0~0
  //#ZLOG# PUTQSO ZLOGQSODATA:~45472.56640625  ~JA1ZLO~11M~10H~599~599~1~0~11~6~  ~~0~0~1~Ron~~1~0~ ~10~ 50~0~1000010089~430054600~0~jk1dvplog~0~0~0
  // from this prog  
  // print	  
  if (verbose&4) 	{
    console->print("QSOzlog:");
    console->println(buf);
  }
  zserver_send(buf);
}


void dump_qso_current() {
  // dump current qso
  int pos, memo_pos;
  int len;
  int ret;
  len = sizeof(qso.all);
  pos = qsologf.position();
  memo_pos = pos;
  // pos = pos - len;  // start from the end record
  //pos = 0; // start from the beginning
  pos = info_disp.pos;
  plogw->ostream->print("pos= ");
  plogw->ostream->print(info_disp.pos);
  plogw->ostream->print(" # ");
  plogw->ostream->println(info_disp.pos/sizeof(qso.all));    
  
  if (!qsologf.seek(pos)) {
    if (!plogw->f_console_emu) plogw->ostream->println("file seek failed");
    goto end;
  }
  ret = qsologf.read(qso.all, len);
  if (ret != len) {
    //
    if (!plogw->f_console_emu) {
      plogw->ostream->print("qso not read bytes=");
      plogw->ostream->println(ret);
    }
    goto end;
  }
  // check type
  if (qso.entry.type[0] != 'Q') {
    // not vaild qso
    if (!plogw->f_console_emu) plogw->ostream->println("not valid qso encountered");
    //     goto end;
  }
  // print content
  plogw->ostream->write(qso.all,len); // flush
end:
  if (!qsologf.seek(memo_pos)) {
    if (!plogw->f_console_emu) plogw->ostream->println("file seek to end failed");
  }
}

void dump_qso_log() {
  // seek to the first byte and dump
  if (!qsologf.seek(0)) {
    if (!plogw->f_console_emu) plogw->ostream->println("file seek failed");
  }
  if (!plogw->f_console_emu) {
    plogw->ostream->print("printing contents of ");
    plogw->ostream->println(qsologfn);
  }
  int count;
  count = 0;
  int count_buf;char *pbuf;
  count_buf=0;pbuf=buf;
  while (qsologf.available()) {
    char c;
    c = qsologf.read();
    if (count_buf>=512) {
      plogw->ostream->write(buf,count_buf); // flush
      plogw->ostream->flush();
      delay(1); // task switch
      count_buf=0;
      pbuf=buf;
    }
    *pbuf++=c; // store
    count_buf++;
    
  }
  // flush remaining
  plogw->ostream->write(buf,count_buf); // flush
  
  
  if (!plogw->f_console_emu) {
    plogw->ostream->println("\nend printing.");
    plogw->ostream->print("position in file:");
    plogw->ostream->println(qsologf.position());
  }
}

void dump_qso_bak(char *numstr) {
  char fname[30];
  sprintf(fname, "/qsobak.%s", numstr);
  if (SD.exists(fname)) {

    plogw->ostream->println(fname);
    plogw->ostream->println("-- begin --");
    File file = SD.open(fname, "r");

    int count_buf;char *pbuf;
    count_buf=0;pbuf=buf;
    
    while (file.available()) {
      char c;
      c = file.read();


      if (count_buf>=512) {
	plogw->ostream->write(buf,count_buf); // flush
	plogw->ostream->flush();
	count_buf=0;
	pbuf=buf;
      }
      *pbuf++=c; // store
      count_buf++;
      
      //      plogw->ostream->print(c);
    }
    // flush remaining
    plogw->ostream->write(buf,count_buf); // flush
    
    plogw->ostream->println("\n-- end --");
    file.close();
  } else {
    plogw->ostream->print(fname);
    plogw->ostream->println("not exist");
  }
}
