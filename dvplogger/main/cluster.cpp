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
#include "bandmap.h"
#include "cluster.h"
#include "display.h"
#include "log.h"
#include "dupechk.h"
#include "multi.h"
#include "multi_process.h"
#include "tcp_server.h"
//#include "misc.h"
#include "network.h"
#include "AsyncTCP.h"
#include "so2r.h"

#include <HTTPClient.h>
#include "timekeep.h"

char cluster_server[40] = "arc.jg1vgx.net";
int cluster_port = 7000;
char cluster_buf[NCHR_CLUSTER_RINGBUF];

struct cluster cluster;

//WiFiClient client;

void renew_timeout_cluster()
{
    cluster.timeout_alive = millis() + 300000;  // 10 minutes countdown timer      
}

int passed_timeout_cluster()
{
  if (cluster.timeout_alive < millis()) {
    return 1;
  } else {
    return 0;
  }
}

// cluster.cpp
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string>

static const char* TAG = "cluster";
static StreamBufferHandle_t s_cluster_rx_sb = nullptr;

// チューニング推奨：回線速度と処理レイテンシに応じて
//static constexpr size_t CLUSTER_RX_SB_SIZE = 8 * 1024;  // ストリームバッファ容量
//static constexpr size_t CLUSTER_RX_CHUNK   = 512;       // 受信側一時バッファ長
static constexpr size_t CLUSTER_RX_SB_SIZE = 4 * 1024;  // ストリームバッファ容量
static constexpr size_t CLUSTER_RX_CHUNK   = 256;       // 受信側一時バッファ長

// 受信コールバック（AsyncTCP）
void handleData_cluster(void *arg, AsyncClient *client, void *data, size_t len)
{
  //	console->printf("\n data received from %s \n", client->remoteIP().toString().c_str());
  if (verbose & 16) {
    plogw->ostream->print("C:");
    plogw->ostream->write((uint8_t *)data, len);
  }

  if (cluster.stat == 5) {
    //
    renew_timeout_cluster();

    if (!s_cluster_rx_sb) return;

    // 非ブロッキングで詰める（ここで時間を使わない）
    size_t sent = xStreamBufferSend(s_cluster_rx_sb, data, len, 0);
    if (sent < len) {
      ESP_LOGW(TAG, "RX overflow: dropped %u (free=%u)",
	       (unsigned)(len - sent),
	       (unsigned)xStreamBufferSpacesAvailable(s_cluster_rx_sb));  // ← ここ
    }
  }
}

void upd_bandmap_cluster1(const char *cmdbuf) {
  int len;
  len=strlen(cmdbuf);
  if (len > 39 + 3) {
    if (strncmp(cmdbuf + 39, "FT", 2) == 0) {
      return;
    }
  }
	
  if (verbose & 16) {
    plogw->ostream->println("C readline:");
    plogw->ostream->println(cmdbuf);
  }
  if (len<75) {
    // short line
    plogw->ostream->print(":");	  	  
    plogw->ostream->print(cmdbuf);	  
    plogw->ostream->print(":short cluster cmdbuf line len=");
    plogw->ostream->println(len);
    return;
  }
  // check content
  if (strncmp(cmdbuf, "DX de", 5) == 0) {
    // DX line
    if ((strncmp(cmdbuf + 39, "CW", 2) == 0) || (strstr(cmdbuf + 39, "WPM") != NULL)) {
      // CW
      if (f_show_cluster >= 1) {
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("CLUSTER INFO CW:");
	  plogw->ostream->println(cmdbuf);
	}
      }
      // get call freq time info from the DX line and store it to bandmap structure
      upd_bandmap_cluster(cmdbuf);

    } else {
      if (f_show_cluster >= 2) {
	if (!plogw->f_console_emu) {
	  plogw->ostream->print("CLUSTER INFO OTHER:");
	  plogw->ostream->println(cmdbuf);
	}
      }
      //upd_bandmap_cluster(cmdbuf);
    }
  } else {
    if (f_show_cluster > 2) {
      if (!plogw->f_console_emu) {
	plogw->ostream->print("CLUSTER:");
	plogw->ostream->println(cmdbuf);
      }
    }
  }

  //0         1         2         3         4         5         6         7         8
  //012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789
  //DX de JA4ZRK-#:  3510.50  JA1YSS/1     CW 20 dB 19 WPM CQ           ? 1237Z
} 


// どこかの初期化フェーズで1回だけ呼ぶ
// cluster_io_init();
// client->onData(handleData_cluster, this_or_null);

// 前方宣言：1行を処理する既存関数。内部で.c_str()の寿命に注意！
// 下流でポインタ保持せず、必要ならコピーする設計にしてください。
//extern void upd_bandmap_cluster(const char* line);

static void cluster_worker_task(void* /*pv*/) {
    std::string acc;           // 行の断片を貯めるアキュムレータ
    acc.reserve(1024);

    uint8_t buf[CLUSTER_RX_CHUNK];

    for (;;) {
        // 受信待ち（ずっと待つ）
        size_t n = xStreamBufferReceive(s_cluster_rx_sb, buf, sizeof(buf), portMAX_DELAY);
        if (n == 0) continue;

        // 断片を追記
        acc.append(reinterpret_cast<const char*>(buf), n);

        // 改行で行に切る（\r\n / \n の両対応）
        size_t start = 0;
        while (true) {
            size_t nl = acc.find('\n', start);
            if (nl == std::string::npos) {
                // 末尾に未完な断片が残っていれば保持（先頭以外は捨てる）
                if (start > 0) acc.erase(0, start);
                break;
            }
            std::string line = acc.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // ここで未知行や欠損行を足切り
            if (!line.empty()) {
                // 例: 型判定（必要に応じて）
                // char t = line[0];
                // if (t=='D' || t=='X' || t=='S' /* ... */) { ... }
                upd_bandmap_cluster1(line.c_str());  // lineの寿命はこのスコープ内
            }

            start = nl + 1;

	    /*
    char c;
    int ret;
    for (int i=0;i<len;i++) {
      c=(char)((uint8_t *)data)[i];
      
      write_ringbuf(&cluster.ringbuf, c);

      ret = readfrom_ringbuf(&cluster.ringbuf, cluster.cmdbuf + cluster.cmdbuf_ptr, (char)0x0d, (char)0x0a, cluster.cmdbuf_len - cluster.cmdbuf_ptr);
      if (ret < 0) {
	// one line read

	*/
	}
    }
}


void cluster_io_init() {
    if (!s_cluster_rx_sb) {
        s_cluster_rx_sb = xStreamBufferCreate(CLUSTER_RX_SB_SIZE, 1 /*trigger level*/);
        configASSERT(s_cluster_rx_sb != nullptr);
        // パーサタスク（ネットワークより少し低い〜同等の優先度でOK）
        xTaskCreatePinnedToCore(cluster_worker_task, "cluster_worker",
                                6144, nullptr, 9, nullptr, tskNO_AFFINITY);
    }
}

void onDisconnect_cluster(void *arg, AsyncClient *client)
{
  cluster.stat = 0;
  cluster.timeout = millis() + 2000;
  if (!plogw->f_console_emu) {
    plogw->ostream->print("disconnected from cluster ");
  }
}

void onConnect_cluster(void *arg, AsyncClient *client)
{
  if (!plogw->f_console_emu) {
    plogw->ostream->print("connected to cluster ");
    plogw->ostream->print(cluster_server);
    plogw->ostream->print(" port:");
    plogw->ostream->println(cluster_port);
  }
  sprintf(dp->lcdbuf, "Cluster\nConnected\n%s\nPort %d\nMyIP:%s", cluster_server, cluster_port,WiFi.localIP().toString().c_str());
  upd_display_info_flash(dp->lcdbuf);
  plogw->ostream->println("connected to cluster.");
  cluster.stat = 1;
  cluster.timeout = millis() + 2000;
  //  cluster.timeout_alive = millis() + 120000;
  renew_timeout_cluster();
  
  //  console->printf("\n client has been connected to  on port \n") ;
  //  replyToServer(client);
}

AsyncClient *client_tcp = new AsyncClient;

void sprint_cluster_info(char *buf,struct bandmap_entry *entry, int bandid, int idx )
{
  sprintf(buf,":F %8d t %d mode %d Remarks: %s type %d bandid=%d station %s nentry %d idx %d \n",
	  entry->freq,
	  entry->time,
	  entry->mode,
	  entry->remarks,
	  entry->type,
	  bandid,
	  entry->station,
	  bandmap[bandid - 1].nentry,
	  idx);
}

void print_cluster_info(struct bandmap_entry *entry, int bandid, int idx )
{

  if (!plogw->f_console_emu) {  
    plogw->ostream->print(":F ");
    plogw->ostream->print(entry->freq);
    plogw->ostream->print(" t ");
    plogw->ostream->print(entry->time);
    plogw->ostream->print(" mode ");
    plogw->ostream->print(entry->mode);
    plogw->ostream->print(" Remarks:");
    plogw->ostream->print(entry->remarks);
    plogw->ostream->print(": type ");
    plogw->ostream->print(entry->type);
    plogw->ostream->print(" bandid=");
    plogw->ostream->print(bandid);
    plogw->ostream->print(" station ");
    plogw->ostream->print((String)entry->station);
    plogw->ostream->print(" nentry ");
    plogw->ostream->print(bandmap[bandid - 1].nentry);
    plogw->ostream->print(" idx ");
    plogw->ostream->print(idx);
    plogw->ostream->println("");
  }

}



void get_info_cluster(const char *ssrc) {
  // obtain callsign , frequency , mode, time,  and remarks
  //DX de JI1HFJ-#:  7005.85  JK1ILA       CW 5 dB 16 WPM CQ            ? 0228Z
  //          1         2         3         4         5         6         7
  //01234567890123456789012345678901234567890123456789012345678901234567890123456789

  double frequency;
  unsigned int ifreq;
  int bandid;
  char *stn;
  char *s1;
  char *md;
  char *remarks;
  int modeid;
  int modetype;
  struct bandmap_entry *entry;
  /*  struct bandmap_entry *entry_allband;*/

  char s[256];
  strcpy(s,ssrc);

  if (verbose & 16) {
    if (!plogw->f_console_emu) {
      plogw->ostream->print("C:");
      plogw->ostream->println(s);
    }
  }


  // search the first encounter to :
  s1 = strtok(s, ":");
  if (s1 != NULL) {
    s1 = strtok(NULL, " ");  // s1 points to freq
  } else return;
  if (s1 == NULL) return;
  // plogw->ostream->print("FSTR:"); plogw->ostream->print((String)s1); plogw->ostream->print(":");
  frequency = check_frequency((String)(s1));
  // plogw->ostream->print(" freq double "); plogw->ostream->print(frequency);
  ifreq = frequency * (1000/FREQ_UNIT);  // frequency in FREQ_UNIT conversion

  //  console->print("ifreq:");
  //  console->println(ifreq);  
  // locate band
  bandid = freq2bandid(ifreq);
  if (bandid == 0) {
    if (verbose & 16) {
      plogw->ostream->print("invalid band for freq:");
      plogw->ostream->println(ifreq);
    }
    return;
  }

  // check bandmap mask
  if ((bandmap_mask & (1 << (bandid - 1))) != 0) {
    if (verbose & 16) {
      plogw->ostream->print("band is masked:");
      plogw->ostream->print(bandid);
      plogw->ostream->println(" no update");
    }
    return;
  }
  // check contest frequency if contest_id != 0 , cqww  3
  if (plogw->contest_id != 0 && plogw->contest_id != 3 && plogw->contest_id != 15) {
    // check contest frequency
    if (!in_contest_frequency(ifreq)) {
      if (verbose & 16) {
        plogw->ostream->print(ifreq);
        plogw->ostream->println(" outside contest freq.");
      }
      return;
    }
  }
  // get operation info

  s1 = strtok(NULL, " ");  // s1 points to callsign
  if (s1 == NULL) return;
  // stn = trim(s1);
  stn=s1;
  //plogw->ostream->print(" stn "); plogw->ostream->print(stn);

  // mode
  s1 = strtok(NULL, "");  // s1 points to mode and remarks
  // skimmer does not specify mode so scan s1 for possible modes to identify mode
  if (s1 == NULL) return;

  //md = trim(s1);
  //plogw->ostream->print(" mode "); plogw->ostream->print(md);
  md = "";
  if (strstr(s1, "CW") != NULL) {
    md = "CW";
  } else if (strstr(s1, "FT8") != NULL) {
    md = "FT8";
  } else if (strstr(s1, "FT4") != NULL) {
    md = "FT4";
  } else {
    md = "CW";
  }

  modeid = modeid_string(md);  //
  modetype = modetype_string(md);

  // remarks
  //  s1 = strtok(NULL, ""); // s1 points to remarks
  //  if (s1 == NULL) return;
  //  remarks = trim(s1);
  remarks=s1;

  adjust_callsign(stn);


  int idx;
  //  int idx_allband;
  bool f_newentry; // new entry in the bandmap 
  f_newentry=0;
  // find new entry point  for the station
  idx = search_bandmap(bandid, stn, modeid);

  if (verbose & 16) {
    plogw->ostream->print("search_bandmap:");
    plogw->ostream->println(idx);
  }
  if (idx != -1) {
    // found existing entry
    // replace the entry with current one
    entry = bandmap[bandid - 1].entry + idx;
    if (verbose & 16) {
      plogw->ostream->print("existing entry idx:");
      plogw->ostream->println(idx);
    }
  } else {
    // new entry
    f_newentry=1;
    idx = new_entry_bandmap(bandid,200);  // return entry which is not used
    entry = bandmap[bandid - 1].entry + idx;
    if (verbose & 16) {
      plogw->ostream->print("new entry idx:");
      plogw->ostream->println(idx);
    }
  }

  /*  
  idx_allband = search_bandmap_allband(bandid, stn, modeid);
  console->print("search_bandmap_allband() idx_allband=");console->println(idx_allband);
  entry_allband=NULL;
  if (idx_allband != -1) {
    // found existing entry
    // replace the entry with current one
    
    entry_allband = bandmap[N_BAND].entry + idx_allband;
    if (verbose & 16) {
      plogw->ostream->print("existing entry allband idx:");
      plogw->ostream->println(idx_allband);
    }
  } else {
    // new entry
    idx_allband = new_entry_bandmap(N_BAND+1,200);  // return entry which is not used
    if (idx_allband>=0) {
      entry_allband = bandmap[N_BAND].entry + idx_allband;
      
      if (verbose & 16) {
	plogw->ostream->print("new entry allband idx:");
	plogw->ostream->println(idx_allband);
      }
    } else {
      entry_allband=NULL;
      console->println("entry_allband=NULL");
    }
  }
  
  */
  int bandmode;
  bandmode = bandmode_param(bandid,modetype);
  char *exch_history;
  int dupe;
  entry->flag &= ~BANDMAP_ENTRY_FLAG_WORKED;
  /*
  if (entry_allband!=NULL) {
    entry_allband->flag &= ~BANDMAP_ENTRY_FLAG_WORKED;
  }
  */
  dupe=dupe_callhist_check(stn, bandmode, plogw->mask,1,&exch_history); // 1 means search including callhist_list (and that is needed to find out not worked station multi search)
  if (dupe) {
    // dupe
    entry->flag |= BANDMAP_ENTRY_FLAG_WORKED;
    /*
    if (entry_allband!=NULL) {
      entry_allband->flag |= BANDMAP_ENTRY_FLAG_WORKED;
    }
    */
    if (verbose & 16) {
      plogw->ostream->print("WORKED already");
      plogw->ostream->println(stn);
    }
  }
  // reset new multi flag
  entry->flag &= ~BANDMAP_ENTRY_FLAG_NEWMULTI;
  /*
    if (entry_allband!=NULL) {
    entry_allband->flag &= ~BANDMAP_ENTRY_FLAG_NEWMULTI;
  }
  */
  if (exch_history!=NULL) {
    sprintf(buf,"exch history found for %s: %s\n",stn,exch_history);
    plogw->ostream->print(buf);

    // check exch for multi
    int multi;
    multi=multi_check(exch_history,bandid);
    if (multi<0) {
      sprintf(buf,"not valid multi: %s\n",exch_history);
      plogw->ostream->print(buf);
    } else {
      // check if this is new multi
      if (multi_list.multi_worked[bandid-1][multi]==0) {
	sprintf(buf,"this entry %s is a new multi (multi=%d)\n",exch_history,multi);
	entry->flag |= BANDMAP_ENTRY_FLAG_NEWMULTI;
	/*
	  if (entry_allband!=NULL) {
	  entry_allband->flag |= BANDMAP_ENTRY_FLAG_NEWMULTI;
	}
	*/
      } else {
	sprintf(buf,"this entry %s is NOT NEW (multi=%d) \n",exch_history,multi);
      }
      plogw->ostream->print(buf);
    }
  } else {
    //    sprintf(buf,"no exch history found for %s\n",stn);
    //    plogw->ostream->print(buf);
    
  }


  // if possible obtain exchange info (from worked and callhist) (may not be appropriate)
  if (f_newentry) {
    
  }

  strcpy(entry->station, stn);       // station
  entry->freq = ifreq;               // frequency
  //  entry->time = rtctime.unixtime();  // current time for removing the entry in clean_bandmap();
  entry->time = my_rtc.unixtime();  // current time for removing the entry in clean_bandmap();  
  entry->mode = modeid;
  entry->remarks[0] = '\0';
  strncat(entry->remarks, remarks, 16);  // copy remarks
  trim(entry->remarks);
  entry->type = 2;


  /*
  if (entry_allband!=NULL) {
    strcpy(entry_allband->station, stn);       // station
    console->print("entry_allband->station=");
    console->println(entry_allband->station);
    
    entry_allband->freq = ifreq;               // frequency
    //  entry->time = rtctime.unixtime();  // current time for removing the entry in clean_bandmap();
    entry_allband->time = my_rtc.unixtime();  // current time for removing the entry in clean_bandmap();  
    entry_allband->mode = modeid;
    entry_allband->remarks[0] = '\0';
    strncat(entry_allband->remarks, remarks, 16);  // copy remarks
    trim(entry_allband->remarks);
    entry_allband->type = 2;
  } else {
    console->println("entry_allband =NULL");
  }
  */
  // print entry content
  if (plogw->f_console_emu) {
    char buf[20];
    sprintf(buf, "\033[%d;%dH", 17, 1);
    plogw->ostream->print(buf);
  }


  // print info to console
  char buf1[100];
  sprint_cluster_info(buf1,entry, bandid, idx );
  // print info to tcp clients
  print_allTCPclients(buf1);
  
  /*
  for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
    if (serverClients[i] && serverClients[i].connected()) {
      plogw->ostream = &serverClients[i];
      print_cluster_info(entry, bandid, idx );
    }
  }
  */
  plogw->ostream = console;


  // notify necessity for the display update
  //upd_display_bandmap ();
  
  // only update bandmap display currently shown on the band (bandid)
  struct radio *radio;
  radio = so2r.radio_selected() ;
  
  if (radio->bandid_bandmap == bandid) {
    bandmap_disp.f_update = 1;
  }


  // memo; bandmap shows latest entries on the top where scrollable (with ^ and v symbols showing more entry to the directions
  // or show the entry at the current operating frequency on the top (to indicate what station is in the frequency
}

void upd_bandmap_cluster(const char *s) {
  //
  get_info_cluster(s);
}


int f_show_cluster = 0;
void cluster_process() {
  //plogw->ostream->print(cluster_stat);
  int ret;

  switch (cluster.stat) {
    case 0:  // not logged in
      //      plogw->ostream->print("cluster.stat=");plogw->ostream->println(cluster.stat);
      //      plogw->ostream->print("wifi_status=");plogw->ostream->println(wifi_status);      
      if (wifi_status == 0 ) {
        cluster.stat = 10;
        cluster.timeout = millis() + 1000;
        break;
      } else {
	if (!client_tcp->connected()) {
	  connect_cluster();
	  cluster.stat =10;
	  cluster.timeout = millis() + 10000;	  
        } 
      }
      break;
    case 10:
      if (cluster.timeout < millis()) {
        cluster.stat = 0;  // try again
      }
      break;
    case 11:  // after forced disconnection, not try to connect
      break;
    case 1:  // connected and wait for a while to send callsign
      if (cluster.timeout < millis()) {
	//        client_tcp->println(String(callsign));
	println_tcpserver(client_tcp,callsign);
        if (!plogw->f_console_emu) {
          plogw->ostream->print(String(callsign));
          plogw->ostream->println("... sent to cluster");
        }
        cluster.stat = 2;
        cluster.timeout = millis() + 500;
      }
      break;
    case 2:
    case 3:
    case 4:
      if (cluster.timeout < millis()) {
	//        client_tcp->println(cluster_cmd[cluster.stat - 2]);
	println_tcpserver(client_tcp,cluster_cmd[cluster.stat - 2]);
        if (!plogw->f_console_emu) {
          plogw->ostream->println(cluster_cmd[cluster.stat - 2]);
          plogw->ostream->print("cluster.stat=");
          plogw->ostream->println(cluster.stat);
        }
        cluster.stat++;
        cluster.timeout = millis() + 500;
      }
      break;
    case 6:  // send command written in plogw->cluster_cmd
      if (client_tcp->connected()) {
	//        client_tcp->println(plogw->cluster_cmd + 2);
	println_tcpserver(client_tcp,plogw->cluster_cmd + 2);
        if (!plogw->f_console_emu) {
          plogw->ostream->println(plogw->cluster_cmd + 2);
          plogw->ostream->print("cluster.stat=");
          plogw->ostream->println(cluster.stat);
        }
      }
      cluster.stat = 5;
      //      cluster.timeout_alive = millis() + 120000;
      renew_timeout_cluster();
      break;
    case 5:
      /// cluster is connected and receive information
      if (!client_tcp->connected()) {
        if (verbose & 16) plogw->ostream->println("disconnecting from cluster .2");
	disconnect_cluster_temp();
        cluster.stat = 0;
      } else {
        // cluster print
	if (passed_timeout_cluster()) {
          plogw->ostream->println("cluster inactive for 10 minutes. try to disconnect and reconnect.");
	  disconnect_cluster_temp();
          // re-connection atempted
          cluster.stat = 0;
          break;
        }
      }
  }
}

////////

void init_cluster_info() {
  cluster.ringbuf.buf = cluster_buf;
  cluster.ringbuf.len = NCHR_CLUSTER_RINGBUF;
  cluster.ringbuf.wptr = 0;
  cluster.ringbuf.rptr = 0;
  cluster.timeout = 0;
  cluster.stat = 0;
  cluster.cmdbuf_ptr = 0;
  cluster.cmdbuf_len = NCHR_CLUSTER_CMD;
  memset(cluster.cmdbuf, '\0', NCHR_CLUSTER_CMD + 1);

  cluster_io_init();

  // define handler
  client_tcp->onData(handleData_cluster, client_tcp);
  client_tcp->onConnect(onConnect_cluster, client_tcp);
  client_tcp->onDisconnect(onDisconnect_cluster, client_tcp);  

}


void disconnect_cluster() {
  client_tcp->stop();
  plogw->ostream->println("disconnected from cluster");
  cluster.stat = 11;  // force keep disconnection state
}

void disconnect_cluster_temp() {
  client_tcp->stop();
  plogw->ostream->println("disconnected from cluster");
}


int connect_cluster() {
  if (wifi_status == 1) {
    if (!client_tcp->connected() ) {
      plogw->ostream->print("connecting to cluster ");
      plogw->ostream->print(cluster_server);
      plogw->ostream->print(" port ");
      plogw->ostream->println(cluster_port);                  
      client_tcp->connect(cluster_server, cluster_port);
      return 1;
    } else {
      if (!plogw->f_console_emu) {
	plogw->ostream->println("already connected\n");
	disconnect_cluster_temp();
	plogw->ostream->println("disconnect from cluster temporalily ");
	cluster.stat = 0;
      }
      return 0;
    }
  } else {
    return 0;
  }
}



const char *callsign = "JK1DVP";
const char *cluster_cmd[3] = { "set dx ext skimmerquality",
                                "set dx fil not skimdupe and not skimbusted and not skimqsy and cty=ja and SpotterCty=ja",
                                "sh dx fil"
                              };


void send_cluster_cmd() {
  if (cluster.stat == 5) cluster.stat = 6;
}


void set_cluster() {
  char *p;
  strcpy(cluster_server, plogw->cluster_name + 2);
  p = strtok(cluster_server, ":");
  if (p != NULL) {
    p = strtok(NULL, ":");
    if (p != NULL) {
      cluster_port = atoi(p);
    } else {
      cluster_port = 7000;
    }
  } else {
    // port not specified
    cluster_port = 7000;  // default value
  }
  if (!plogw->f_console_emu) {
    plogw->ostream->print("cluster_server:");
    plogw->ostream->print(cluster_server);
    plogw->ostream->print(" port:");
    plogw->ostream->println(cluster_port);
  }
  if (client_tcp->connected()) {
    //    client.stop();
    disconnect_cluster_temp();
    if (!plogw->f_console_emu) plogw->ostream->println("disconnected cluster");
  }
  cluster.stat = 0;
}

