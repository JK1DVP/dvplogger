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

static constexpr uint8_t N_CLUSTER_CONNECTIONS = 2;
static constexpr size_t CLUSTER_RX_EVENT_DATA = 128;
static constexpr size_t CLUSTER_RX_QUEUE_LEN = 16;

struct ClusterRuntime {
  struct cluster *state;
  AsyncClient *client;
  char server[40];
  int port;
  uint8_t id;
};

static struct cluster cluster2;
static char cluster2_buf[NCHR_CLUSTER_RINGBUF];
static ClusterRuntime cluster_rt[N_CLUSTER_CONNECTIONS];

static inline void renew_timeout_cluster(ClusterRuntime *rt) {
  rt->state->timeout_alive = millis() + 300000;
}

static inline bool passed_timeout_cluster(ClusterRuntime *rt) {
  return rt->state->timeout_alive < millis();
}

// cluster.cpp
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string>

static const char* TAG = "cluster";

struct ClusterRxEvent {
  uint8_t source;
  uint8_t len;
  uint8_t data[CLUSTER_RX_EVENT_DATA];
};

static QueueHandle_t s_cluster_rx_queue = nullptr;
// チューニング推奨：回線速度と処理レイテンシに応じて

// 受信コールバック（AsyncTCP）
void handleData_cluster(void *arg, AsyncClient *client, void *data, size_t len)
{
  ClusterRuntime *rt = static_cast<ClusterRuntime *>(arg);
  if (!rt || rt->state->stat != 5 || !s_cluster_rx_queue) return;

  renew_timeout_cluster(rt);
  const uint8_t *src = static_cast<const uint8_t *>(data);
  while (len > 0) {
    ClusterRxEvent ev{};
    ev.source = rt->id;
    ev.len = (uint8_t)min(len, CLUSTER_RX_EVENT_DATA);
    memcpy(ev.data, src, ev.len);
    if (xQueueSend(s_cluster_rx_queue, &ev, 0) != pdTRUE) {
      ESP_LOGW(TAG, "RX queue overflow: cluster=%u dropped=%u",
               (unsigned)(rt->id + 1), (unsigned)len);
      break;
    }
    src += ev.len;
    len -= ev.len;
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
    console->println("C readline:");
    console->println(cmdbuf);
  }
  if (len<75) {
    // short line
    console->print(":");	  	  
    console->print(cmdbuf);	  
    console->print(":short cluster cmdbuf line len=");
    console->println(len);
    return;
  }
  // check content
  if (strncmp(cmdbuf, "DX de", 5) == 0) {
    // DX line
    if ((strncmp(cmdbuf + 39, "CW", 2) == 0) || (strstr(cmdbuf + 39, "WPM") != NULL)) {
      // CW
      if (f_show_cluster >= 1) {
	if (!plogw->f_console_emu) {
	  console->print("CLUSTER INFO CW:");
	  console->println(cmdbuf);
	}
      }
      // get call freq time info from the DX line and store it to bandmap structure
      upd_bandmap_cluster(cmdbuf);

    } else {
      if (f_show_cluster >= 2) {
	if (!plogw->f_console_emu) {
	  console->print("CLUSTER INFO OTHER:");
	  console->println(cmdbuf);
	}
      }
      //upd_bandmap_cluster(cmdbuf);
    }
  } else {
    if (f_show_cluster > 2) {
      if (!plogw->f_console_emu) {
	console->print("CLUSTER:");
	console->println(cmdbuf);
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
    std::string acc[N_CLUSTER_CONNECTIONS];
    for (auto &a : acc) a.reserve(1024);
    ClusterRxEvent ev;

    for (;;) {
        if (xQueueReceive(s_cluster_rx_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.source >= N_CLUSTER_CONNECTIONS) continue;
        std::string &a = acc[ev.source];
        a.append(reinterpret_cast<const char*>(ev.data), ev.len);

        size_t start = 0;
        while (true) {
            size_t nl = a.find('\n', start);
            if (nl == std::string::npos) {
                if (start > 0) a.erase(0, start);
                if (a.size() > 2048) {
                    ESP_LOGW(TAG, "overlong line dropped: cluster=%u", (unsigned)(ev.source + 1));
                    a.clear();
                }
                break;
            }
            std::string line = a.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) upd_bandmap_cluster1(line.c_str());
            start = nl + 1;
        }
    }
}

void cluster_io_init() {
    if (!s_cluster_rx_queue) {
        s_cluster_rx_queue = xQueueCreate(CLUSTER_RX_QUEUE_LEN, sizeof(ClusterRxEvent));
        configASSERT(s_cluster_rx_queue != nullptr);
        xTaskCreatePinnedToCore(cluster_worker_task, "cluster_worker",
                               6144, nullptr, 4, nullptr, tskNO_AFFINITY);
    }
}

void onDisconnect_cluster(void *arg, AsyncClient *client)
{
  ClusterRuntime *rt = static_cast<ClusterRuntime *>(arg);
  if (!rt) return;
  rt->state->stat = 0;
  rt->state->timeout = millis() + 2000;
  if (!plogw->f_console_emu) {
    console->printf("disconnected from cluster %u\n", (unsigned)(rt->id + 1));
  }
}

void onConnect_cluster(void *arg, AsyncClient *client)
{
  ClusterRuntime *rt = static_cast<ClusterRuntime *>(arg);
  if (!rt) return;
  if (!plogw->f_console_emu) {
    console->printf("connected to cluster %u %s port:%d\n",
                    (unsigned)(rt->id + 1), rt->server, rt->port);
  }
  sprintf(dp->lcdbuf, "Cluster %u\nConnected\n%s\nPort %d\nMyIP:%s",
          (unsigned)(rt->id + 1), rt->server, rt->port,
          WiFi.localIP().toString().c_str());
  upd_display_info_flash(dp->lcdbuf);
  rt->state->stat = 1;
  rt->state->timeout = millis() + 2000;
  renew_timeout_cluster(rt);
}

AsyncClient *client_tcp = nullptr;

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
  // The :F cluster report is a machine/diagnostic report and is intentionally
  // kept on the hardware serial console even while Telnet owns normal logs.
  char buf[256];
  sprint_cluster_info(buf, entry, bandid, idx);
  Serial.print(buf);
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
      console->print("C:");
      console->println(s);
    }
  }


  // search the first encounter to :
  s1 = strtok(s, ":");
  if (s1 != NULL) {
    s1 = strtok(NULL, " ");  // s1 points to freq
  } else return;
  if (s1 == NULL) return;
  // console->print("FSTR:"); console->print((String)s1); console->print(":");
  frequency = check_frequency((String)(s1));
  // console->print(" freq double "); console->print(frequency);
  ifreq = frequency * (1000/FREQ_UNIT);  // frequency in FREQ_UNIT conversion

  //  console->print("ifreq:");
  //  console->println(ifreq);  
  // locate band
  bandid = freq2bandid(ifreq);
  if (bandid == 0) {
    if (verbose & 16) {
      console->print("invalid band for freq:");
      console->println(ifreq);
    }
    return;
  }

  // check bandmap mask
  if ((bandmap_mask & (1 << (bandid - 1))) != 0) {
    if (verbose & 16) {
      console->print("band is masked:");
      console->print(bandid);
      console->println(" no update");
    }
    return;
  }
  // check contest frequency if contest_id != 0 , cqww  3
  //  if (plogw->contest_id != 0 && plogw->contest_id != 3 && plogw->contest_id != 15) {
  if (!is_international_contest()) {
    // check contest frequency
    if (!in_contest_frequency(ifreq)) {
      if (verbose & 16) {
        console->print(ifreq);
        console->println(" outside contest freq.");
      }
      return;
    }
  }
  // get operation info

  s1 = strtok(NULL, " ");  // s1 points to callsign
  if (s1 == NULL) return;
  // stn = trim(s1);
  stn=s1;
  //console->print(" stn "); console->print(stn);

  // mode
  s1 = strtok(NULL, "");  // s1 points to mode and remarks
  // skimmer does not specify mode so scan s1 for possible modes to identify mode
  if (s1 == NULL) return;

  //md = trim(s1);
  //console->print(" mode "); console->print(md);
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
    console->print("search_bandmap:");
    console->println(idx);
  }
  if (idx != -1) {
    // found existing entry
    // replace the entry with current one
    entry = bandmap[bandid - 1].entry + idx;
    if (verbose & 16) {
      console->print("existing entry idx:");
      console->println(idx);
    }
  } else {
    // new entry
    f_newentry=1;
    idx = new_entry_bandmap(bandid,200);  // return entry which is not used
    entry = bandmap[bandid - 1].entry + idx;
    if (verbose & 16) {
      console->print("new entry idx:");
      console->println(idx);
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
      console->print("existing entry allband idx:");
      console->println(idx_allband);
    }
  } else {
    // new entry
    idx_allband = new_entry_bandmap(N_BAND+1,200);  // return entry which is not used
    if (idx_allband>=0) {
      entry_allband = bandmap[N_BAND].entry + idx_allband;
      
      if (verbose & 16) {
	console->print("new entry allband idx:");
	console->println(idx_allband);
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
      console->print("WORKED already");
      console->println(stn);
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
    console->print(buf);

    // check exch for multi
    int multi;
    multi=multi_check(exch_history,bandid);
    if (multi<0) {
      sprintf(buf,"not valid multi: %s\n",exch_history);
      console->print(buf);
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
      console->print(buf);
    }
  } else {
    //    sprintf(buf,"no exch history found for %s\n",stn);
    //    console->print(buf);
    
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
    console->print(buf);
  }


  // Keep the cluster :F report on the hardware serial console only.
  print_cluster_info(entry, bandid, idx);
  
  /*
  for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
    if (serverClients[i] && serverClients[i].connected()) {
      plogw->ostream = &serverClients[i];
      print_cluster_info(entry, bandid, idx );
    }
  }
  */


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
static const char *cluster_user_cmd(uint8_t id) {
  return id == 0 ? plogw->cluster_cmd + 2 : plogw->cluster2_cmd + 2;
}

static void cluster_process_one(ClusterRuntime *rt) {
  struct cluster *st = rt->state;
  if (!rt->client || rt->server[0] == '\0') {
    st->stat = 11;
    return;
  }

  switch (st->stat) {
    case 0:
      if (wifi_status == 0) {
        st->stat = 10;
        st->timeout = millis() + 1000;
      } else if (!rt->client->connected()) {
        console->printf("connecting to cluster %u %s port %d\n",
                        (unsigned)(rt->id + 1), rt->server, rt->port);
        rt->client->connect(rt->server, rt->port);
        st->stat = 10;
        st->timeout = millis() + 10000;
      }
      break;
    case 10:
      if (st->timeout < millis()) st->stat = 0;
      break;
    case 11:
      break;
    case 1:
      if (st->timeout < millis()) {
        println_tcpserver(rt->client, callsign);
        if (!plogw->f_console_emu)
          console->printf("%s... sent to cluster %u\n", callsign, (unsigned)(rt->id + 1));
        st->stat = 2;
        st->timeout = millis() + 500;
      }
      break;
    case 2:
    case 3:
    case 4:
      if (st->timeout < millis()) {
        println_tcpserver(rt->client, cluster_cmd[st->stat - 2]);
        if (!plogw->f_console_emu)
          console->printf("cluster %u: %s\n", (unsigned)(rt->id + 1), cluster_cmd[st->stat - 2]);
        st->stat++;
        st->timeout = millis() + 500;
      }
      break;
    case 6:
      if (rt->client->connected()) {
        const char *cmd = cluster_user_cmd(rt->id);
        if (*cmd) println_tcpserver(rt->client, cmd);
        if (!plogw->f_console_emu)
          console->printf("cluster %u command: %s\n", (unsigned)(rt->id + 1), cmd);
      }
      st->stat = 5;
      renew_timeout_cluster(rt);
      break;
    case 5:
      if (!rt->client->connected()) {
        rt->client->stop();
        st->stat = 0;
      } else if (passed_timeout_cluster(rt)) {
        console->printf("cluster %u inactive for 5 minutes; reconnecting\n", (unsigned)(rt->id + 1));
        rt->client->stop();
        st->stat = 0;
      }
      break;
  }
}

void cluster_process() {
  for (uint8_t i = 0; i < N_CLUSTER_CONNECTIONS; ++i) cluster_process_one(&cluster_rt[i]);
}

static void init_cluster_state(struct cluster *st, char *ring_storage) {
  st->ringbuf.buf = ring_storage;
  st->ringbuf.len = NCHR_CLUSTER_RINGBUF;
  st->ringbuf.wptr = 0;
  st->ringbuf.rptr = 0;
  st->timeout = 0;
  st->stat = 0;
  st->cmdbuf_ptr = 0;
  st->cmdbuf_len = NCHR_CLUSTER_CMD;
  memset(st->cmdbuf, '\0', NCHR_CLUSTER_CMD + 1);
}

void init_cluster_info() {
  cluster_io_init();
  init_cluster_state(&cluster, cluster_buf);
  init_cluster_state(&cluster2, cluster2_buf);

  memset(cluster_rt, 0, sizeof(cluster_rt));

  cluster_rt[0].state = &cluster;
  cluster_rt[0].client = new AsyncClient;
  cluster_rt[0].port = 7000;
  cluster_rt[0].id = 0;

  cluster_rt[1].state = &cluster2;
  cluster_rt[1].client = new AsyncClient;
  cluster_rt[1].port = 7000;
  cluster_rt[1].id = 1;

  client_tcp = cluster_rt[0].client;

  for (uint8_t i = 0; i < N_CLUSTER_CONNECTIONS; ++i) {
    cluster_rt[i].client->onData(handleData_cluster, &cluster_rt[i]);
    cluster_rt[i].client->onConnect(onConnect_cluster, &cluster_rt[i]);
    cluster_rt[i].client->onDisconnect(onDisconnect_cluster, &cluster_rt[i]);
  }
  set_cluster();
  set_cluster2();
}

static void disconnect_cluster_id(uint8_t id, bool hold) {
  ClusterRuntime *rt = &cluster_rt[id];
  if (rt->client) rt->client->stop();
  console->printf("disconnected from cluster %u\n", (unsigned)(id + 1));
  rt->state->stat = hold ? 11 : 0;
}

void disconnect_cluster() { disconnect_cluster_id(0, true); }
void disconnect_cluster_temp() { disconnect_cluster_id(0, false); }
void disconnect_cluster2() { disconnect_cluster_id(1, true); }
void disconnect_cluster2_temp() { disconnect_cluster_id(1, false); }

static int connect_cluster_id(uint8_t id) {
  ClusterRuntime *rt = &cluster_rt[id];
  if (wifi_status != 1 || !rt->client || !rt->server[0]) return 0;
  if (!rt->client->connected()) {
    rt->state->stat = 0;
    return 1;
  }
  disconnect_cluster_id(id, false);
  return 0;
}

int connect_cluster() { return connect_cluster_id(0); }
int connect_cluster2() { return connect_cluster_id(1); }

const char *callsign = "JK1DVP";
const char *cluster_cmd[3] = { "set dx ext skimmerquality",
                                "set dx fil not skimdupe and not skimbusted and not skimqsy and cty=ja and SpotterCty=ja",
                                "sh dx fil" };

void send_cluster_cmd() { if (cluster.stat == 5) cluster.stat = 6; }
void send_cluster2_cmd() { if (cluster2.stat == 5) cluster2.stat = 6; }

static void set_cluster_id(uint8_t id, const char *setting) {
  ClusterRuntime *rt = &cluster_rt[id];
  if (!rt->state) return;
  char tmp[LEN_HOST_NAME + 1];
  strlcpy(tmp, setting ? setting : "", sizeof(tmp));
  char *colon = strrchr(tmp, ':');
  rt->port = 7000;
  if (colon) {
    *colon++ = '\0';
    if (*colon) rt->port = atoi(colon);
  }
  strlcpy(rt->server, tmp, sizeof(rt->server));
  if (id == 0) {
    strlcpy(cluster_server, rt->server, sizeof(cluster_server));
    cluster_port = rt->port;
  }
  if (rt->client && rt->client->connected()) rt->client->stop();
  rt->state->stat = rt->server[0] ? 0 : 11;
  if (!plogw->f_console_emu)
    console->printf("cluster %u server:%s port:%d%s\n", (unsigned)(id + 1),
                    rt->server, rt->port, rt->server[0] ? "" : " (disabled)");
}

void set_cluster() { set_cluster_id(0, plogw->cluster_name + 2); }
void set_cluster2() { set_cluster_id(1, plogw->cluster2_name + 2); }


