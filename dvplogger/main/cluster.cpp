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
#include "misc.h"
#include "network.h"
#include "AsyncTCP.h"
#include "so2r.h"

#include <HTTPClient.h>
#include "timekeep.h"

char cluster_server[40] = "arc.jg1vgx.net";
int cluster_port = 7000;
char cluster_buf[NCHR_CLUSTER_RINGBUF];
struct cluster cluster;

char cluster2_startup_cmd[N_CLUSTER2_STARTUP_CMDS][LEN_CLUSTER_CMD + 3];

static constexpr uint8_t N_CLUSTER_CONNECTIONS = 2;
static constexpr size_t CLUSTER_RX_LINE_MAX = 192;
static constexpr size_t CLUSTER_RX_QUEUE_LEN = 32;

struct ClusterRuntime {
  struct cluster *state;
  AsyncClient *client;
  char server[40];
  int port;
  uint8_t id;
  uint8_t startup_index;
  char rx_line[CLUSTER_RX_LINE_MAX];
  uint16_t rx_line_len;
  bool rx_discard_until_eol;
  uint32_t rx_dropped_lines;
  uint32_t rx_dropped_bytes;
  uint32_t rx_overlong_lines;
  uint32_t rx_last_report_ms;
  bool disconnect_handled;
  bool disconnect_notice_pending;
  bool hold_requested;
  bool connected_state;
};

static struct cluster cluster2;
static char cluster2_buf[NCHR_CLUSTER_RINGBUF];
static ClusterRuntime cluster_rt[N_CLUSTER_CONNECTIONS];

int cluster1_auto_enable = 1;
int cluster2_auto_enable = 1;

static inline bool cluster_auto_enabled(uint8_t id) {
  return id == 0 ? cluster1_auto_enable != 0 : cluster2_auto_enable != 0;
}

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

struct ClusterRxLine {
  uint8_t source;
  uint16_t len;
  char data[CLUSTER_RX_LINE_MAX];
};

static QueueHandle_t s_cluster_rx_queue = nullptr;

static void note_cluster_rx_overload(ClusterRuntime *rt, size_t dropped_bytes,
                                     bool overlong) {
  if (!rt) return;
  rt->rx_dropped_lines++;
  rt->rx_dropped_bytes += dropped_bytes;
  if (overlong) rt->rx_overlong_lines++;

  const uint32_t now = millis();
  if (rt->rx_last_report_ms == 0 ||
      (uint32_t)(now - rt->rx_last_report_ms) >= 1000U) {
    const UBaseType_t queued = s_cluster_rx_queue
        ? uxQueueMessagesWaiting(s_cluster_rx_queue) : 0;
    ESP_LOGW(TAG,
             "RX overload: cluster=%u dropped_lines=%lu dropped_bytes=%lu "
             "overlong=%lu queued=%u/%u",
             (unsigned)(rt->id + 1),
             (unsigned long)rt->rx_dropped_lines,
             (unsigned long)rt->rx_dropped_bytes,
             (unsigned long)rt->rx_overlong_lines,
             (unsigned)queued, (unsigned)CLUSTER_RX_QUEUE_LEN);
    rt->rx_last_report_ms = now;
    rt->rx_dropped_lines = 0;
    rt->rx_dropped_bytes = 0;
    rt->rx_overlong_lines = 0;
  }
}

static void enqueue_complete_cluster_line(ClusterRuntime *rt) {
  if (!rt || !s_cluster_rx_queue || rt->rx_line_len == 0) return;

  ClusterRxLine line{};
  line.source = rt->id;
  line.len = rt->rx_line_len;
  memcpy(line.data, rt->rx_line, line.len);
  line.data[line.len] = '\0';

  if (xQueueSend(s_cluster_rx_queue, &line, 0) != pdTRUE) {
    // Preserve line boundaries: discard one complete old line, then keep the
    // newest complete line.  Never discard only part of a line.
    ClusterRxLine old_line;
    if (xQueueReceive(s_cluster_rx_queue, &old_line, 0) == pdTRUE &&
        xQueueSend(s_cluster_rx_queue, &line, 0) == pdTRUE) {
      ClusterRuntime *old_rt = old_line.source < N_CLUSTER_CONNECTIONS
          ? &cluster_rt[old_line.source] : rt;
      note_cluster_rx_overload(old_rt, old_line.len, false);
    } else {
      note_cluster_rx_overload(rt, line.len, false);
    }
  }
}

// AsyncTCP callback: assemble complete lines before queueing them.  If a line
// is too long, discard through the next newline so bytes from adjacent lines
// can never be joined into a false callsign.
void handleData_cluster(void *arg, AsyncClient *client, void *data, size_t len)
{
  ClusterRuntime *rt = static_cast<ClusterRuntime *>(arg);
  if (!rt || rt->state->stat != 5 || !s_cluster_rx_queue) return;

  renew_timeout_cluster(rt);
  const uint8_t *src = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(src[i]);

    if (rt->rx_discard_until_eol) {
      rt->rx_dropped_bytes++;
      if (c == '\n') {
        rt->rx_discard_until_eol = false;
        rt->rx_line_len = 0;
        note_cluster_rx_overload(rt, 0, true);
      }
      continue;
    }

    if (c == '\r') continue;
    if (c == '\n') {
      enqueue_complete_cluster_line(rt);
      rt->rx_line_len = 0;
      continue;
    }

    if (rt->rx_line_len + 1 >= CLUSTER_RX_LINE_MAX) {
      rt->rx_discard_until_eol = true;
      rt->rx_dropped_bytes += rt->rx_line_len + 1;
      rt->rx_line_len = 0;
      continue;
    }
    rt->rx_line[rt->rx_line_len++] = c;
  }
}

void upd_bandmap_cluster1(uint8_t source, const char *cmdbuf) {
  const uint8_t cluster_no = source + 1;
  const uint8_t show_level = source < 2 ? cluster_verbose_level[source] : 0;
  int len;
  len=strlen(cmdbuf);
  if (len > 39 + 3) {
    if (strncmp(cmdbuf + 39, "FT", 2) == 0) {
      return;
    }
  }
	
  if (verbose & 16) {
    console->printf("[CL%u RX raw] ", (unsigned)cluster_no);
    console->println(cmdbuf);
  }
  if (len<75) {
    // short line
    if (show_level >= 3) {
      console->printf("[CL%u RX short len=%d] %s\n",
                      (unsigned)cluster_no, len, cmdbuf);
    }
    return;
  }
  // check content
  if (strncmp(cmdbuf, "DX de", 5) == 0) {
    // DX line
    if ((strncmp(cmdbuf + 39, "CW", 2) == 0) || (strstr(cmdbuf + 39, "WPM") != NULL)) {
      // CW
      if (show_level >= 1) {
	if (!plogw->f_console_emu) {
	  console->printf("[CL%u RX CW] ", (unsigned)cluster_no);
	  console->println(cmdbuf);
	}
      }
      // get call freq time info from the DX line and store it to bandmap structure
      upd_bandmap_cluster(cmdbuf);

    } else {
      if (show_level >= 2) {
	if (!plogw->f_console_emu) {
	  console->printf("[CL%u RX DX] ", (unsigned)cluster_no);
	  console->println(cmdbuf);
	}
      }
      //upd_bandmap_cluster(cmdbuf);
    }
  } else {
    if (show_level >= 3) {
      if (!plogw->f_console_emu) {
	console->printf("[CL%u RX] ", (unsigned)cluster_no);
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
    ClusterRxLine line;
    for (;;) {
        if (xQueueReceive(s_cluster_rx_queue, &line, portMAX_DELAY) != pdTRUE)
          continue;
        if (line.source >= N_CLUSTER_CONNECTIONS || line.len == 0 ||
            line.len >= CLUSTER_RX_LINE_MAX)
          continue;
        line.data[line.len] = '\0';
        upd_bandmap_cluster1(line.source, line.data);
    }
}

void cluster_io_init() {
    if (!s_cluster_rx_queue) {
        s_cluster_rx_queue = xQueueCreate(CLUSTER_RX_QUEUE_LEN, sizeof(ClusterRxLine));
        configASSERT(s_cluster_rx_queue != nullptr);
        xTaskCreatePinnedToCore(cluster_worker_task, "cluster_worker",
                               6144, nullptr, 4, nullptr, tskNO_AFFINITY);
    }
}

void onDisconnect_cluster(void *arg, AsyncClient *client)
{
  memtrace_event("cluster disconnected");
  ClusterRuntime *rt = static_cast<ClusterRuntime *>(arg);
  if (!rt) return;

  // AsyncTCP may report the same disconnect more than once.  Also, stop()
  // used for an intentional hold must not be turned back into an automatic
  // reconnect by this callback.
  if (rt->disconnect_handled) return;
  rt->disconnect_handled = true;
  const bool was_connected = rt->connected_state;
  rt->connected_state = false;

  rt->startup_index = 0;
  rt->rx_line_len = 0;
  rt->rx_discard_until_eol = false;

  if (rt->hold_requested) {
    rt->state->stat = 11;
    rt->state->timeout = 0;
    return;
  }

  rt->state->stat = 10;
  rt->state->timeout = millis() + 60000;
  if (!plogw->f_console_emu) {
    console->printf("disconnected from cluster %u; retry in 60 sec\n",
                    (unsigned)(rt->id + 1));
  }
  // Only interrupt the LCD for a real CONNECTED -> DISCONNECTED transition.
  // Failed connection attempts remain quiet and simply retry in the background.
  if (was_connected && WiFi.status() == WL_CONNECTED && cluster_auto_enabled(rt->id)) {
    rt->disconnect_notice_pending = true;
  }
}

void onConnect_cluster(void *arg, AsyncClient *client)
{
  memtrace_event("cluster connected");
  ClusterRuntime *rt = static_cast<ClusterRuntime *>(arg);
  if (!rt) return;
  if (!cluster_auto_enabled(rt->id)) {
    rt->hold_requested = true;
    rt->connected_state = false;
    rt->state->stat = 11;
    client->stop();
    return;
  }
  rt->connected_state = true;
  if (!plogw->f_console_emu) {
    console->printf("connected to cluster %u %s port:%d\n",
                    (unsigned)(rt->id + 1), rt->server, rt->port);
  }
  sprintf(dp->lcdbuf, "Cluster %u\nConnected\n%s\nPort %d\nMyIP:%s",
          (unsigned)(rt->id + 1), rt->server, rt->port,
          WiFi.localIP().toString().c_str());
  upd_display_info_flash(dp->lcdbuf);
  rt->startup_index = 0;
  rt->rx_line_len = 0;
  rt->rx_discard_until_eol = false;
  rt->disconnect_handled = false;
  rt->disconnect_notice_pending = false;
  rt->hold_requested = false;
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
  /* High-rate :F reports can block the console and aggravate RX overload. */
  if ((verbose & 16) == 0) return;
  char buf[256];
  sprint_cluster_info(buf, entry, bandid, idx);
  Serial.print(buf);
}



static int find_latest_cluster_entry(int bandid, const char *stn,
                                     int modeid) {
  if (bandid <= 0 || bandid > N_BAND || !stn) return -1;
  struct bandmap *bm = &bandmap[bandid - 1];
  int keep = -1;
  for (int i = 0; i < bm->nentry; ++i) {
    struct bandmap_entry *e = &bm->entry[i];
    if (!e->station[0] || strcmp(e->station, stn) != 0 || e->mode != modeid)
      continue;
    if (keep < 0 || e->time > bm->entry[keep].time ||
        (e->time == bm->entry[keep].time &&
         e->receive_order > bm->entry[keep].receive_order)) {
      if (keep >= 0) bm->entry[keep].station[0] = '\0';
      keep = i;
    } else {
      e->station[0] = '\0';
    }
  }
  return keep;
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


  int bandmode = bandmode_param(bandid, modetype);
  char remote_exch[LEN_EXCH + 1] = "";
  bool dupe = false;
  /*
   * Do not synchronously query the SUBCPU for every incoming cluster line.
   * A busy skimmer feed can otherwise fill the RX queue while each request
   * waits for the remote DUPE result.  When the DUPE database is on SUBCPU,
   * accept the spot here and run the authoritative check when it is picked.
   */
  if (dupechk->dupechk_at != 1) {
    bool dupe_confirmed = dupe_check_with_exch_confirmed(
        stn, bandmode, plogw->mask, remote_exch, sizeof(remote_exch), &dupe);
    if (!dupe_confirmed) {
      if (verbose & 16) {
        console->printf("cluster spot discarded: DUPE result unavailable call=%s\n",
                        stn);
      }
      return;
    }
  }
  char *exch_history = remote_exch[0] ? remote_exch : NULL;

  int idx = find_latest_cluster_entry(bandid, stn, modeid);
  bool f_newentry = false;
  if (verbose & 16) {
    console->print("search_bandmap latest same call/mode:");
    console->println(idx);
  }
  if (idx >= 0) {
    // Same callsign on the same band and mode: refresh the existing record.
    // A QSY therefore replaces the old frequency with the latest spot.
    entry = bandmap[bandid - 1].entry + idx;
  } else {
    f_newentry = true;
    idx = new_entry_bandmap(bandid, 200);
    if (idx < 0) return;
    entry = bandmap[bandid - 1].entry + idx;
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
  // Do not clear an existing WORKED flag when the same spot is refreshed.
  // A DUPE query can temporarily fail or race with the just-completed QSO
  // being entered on the subcpu; clearing here would make a worked station
  // reappear as unworked.
  /*
  if (entry_allband!=NULL) {
    entry_allband->flag &= ~BANDMAP_ENTRY_FLAG_WORKED;
  }
  */
  if (dupe) {
    // Keep DUPE spots in the database.  The display layer may hide them in
    // the normal list, but an on-frequency lookup still needs this record.
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
  } else {
    entry->flag &= ~BANDMAP_ENTRY_FLAG_WORKED;
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
      if (!multi_worked_get(&multi_list, bandid-1, multi)) {
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
  stamp_bandmap_entry(entry);  // reception time and within-second order
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


uint8_t cluster_verbose_level[2] = {0, 0};

void print_cluster_verbose_status(Stream *out) {
  if (!out) out = console;
  out->printf("clusterverbose: cluster1=%u cluster2=%u\n",
              (unsigned)cluster_verbose_level[0],
              (unsigned)cluster_verbose_level[1]);
  out->println("levels: 0=off, 1=CW spots, 2=all DX spots, 3=all RX and TX/control");
}

bool set_cluster_verbose_level(int cluster_no, int level, Stream *out) {
  if (!out) out = console;
  if (cluster_no < 1 || cluster_no > 2 || level < 0 || level > 3) {
    out->println("usage: clusterverbose [1|2] [0-3]");
    print_cluster_verbose_status(out);
    return false;
  }
  cluster_verbose_level[cluster_no - 1] = (uint8_t)level;
  out->printf("cluster %d verbose level=%d\n", cluster_no, level);
  return true;
}
static const char *cluster_user_cmd(uint8_t id) {
  return id == 0 ? plogw->cluster_cmd + 2 : plogw->cluster2_cmd + 2;
}

void initialize_cluster2_startup_commands() {
  static bool initialized = false;
  if (initialized) return;
  initialized = true;

  static const char *defaults[N_CLUSTER2_STARTUP_CMDS] = {
    "<MYCALL>",
    "SKIMMER/SETT",
    "SH/DX",
    "",
    ""
  };

  for (uint8_t i = 0; i < N_CLUSTER2_STARTUP_CMDS; ++i) {
    cluster2_startup_cmd[i][0] = LEN_CLUSTER_CMD + 1;
    cluster2_startup_cmd[i][1] = 0;
    strlcpy(cluster2_startup_cmd[i] + 2, defaults[i],
            sizeof(cluster2_startup_cmd[i]) - 2);
  }
}

static String expand_cluster2_startup_command(const char *src) {
  String command = src ? src : "";
  const char *mycall = plogw->my_callsign + 2;

  if (!mycall || !*mycall) mycall = callsign;

  command.replace("<MYCALL>", mycall);
  command.replace("$MYCALL", mycall);
  return command;
}

static void cluster_process_one(ClusterRuntime *rt) {
  struct cluster *st = rt->state;

  if (!cluster_auto_enabled(rt->id)) {
    rt->disconnect_notice_pending = false;
    rt->hold_requested = true;
    st->stat = 11;
    st->timeout = 0;
    if (rt->client && rt->client->connected()) {
      rt->disconnect_handled = true;
      rt->connected_state = false;
      rt->client->stop();
    }
    return;
  }

  // Run display work in the normal loop, never in the AsyncTCP callback.
  if (rt->disconnect_notice_pending) {
    rt->disconnect_notice_pending = false;
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "NETWORK ERROR\nCluster %u lost\nRetry in 60 sec",
             (unsigned)(rt->id + 1));
    network_display_error(dp->lcdbuf);
  }

  if (!rt->client || rt->server[0] == '\0') {
    st->stat = 11;
    return;
  }

  switch (st->stat) {
    case 0:
      if (!network_external_service_ready(5000)) {
        st->stat = 10;
        st->timeout = millis() + 1000;
      } else if (!rt->client->connected()) {
        rt->disconnect_handled = false;
        rt->disconnect_notice_pending = false;
        if (cluster_verbose_level[rt->id] >= 3)
          console->printf("[CL%u CONNECT] %s port %d\n",
                          (unsigned)(rt->id + 1), rt->server, rt->port);
        memtrace_event(rt->id == 0 ? "before cluster1 connect" : "before cluster2 connect");
        rt->client->connect(rt->server, rt->port);
        memtrace_event(rt->id == 0 ? "after cluster1 connect" : "after cluster2 connect");
        st->stat = 10;
        st->timeout = millis() + 60000;
      }
      break;
    case 10:
      if (st->timeout < millis()) st->stat = 0;
      break;
    case 11:
      break;
    case 1:
      if (st->timeout < millis()) {
        if (rt->id == 1) {
          while (rt->startup_index < N_CLUSTER2_STARTUP_CMDS &&
                 cluster2_startup_cmd[rt->startup_index][2] == '\0') {
            ++rt->startup_index;
          }

          if (rt->startup_index < N_CLUSTER2_STARTUP_CMDS) {
            const uint8_t command_index = rt->startup_index;
            String command = expand_cluster2_startup_command(
              cluster2_startup_cmd[command_index] + 2);

            if (command.length() && rt->client->connected()) {
              println_tcpserver(rt->client, command.c_str());

              if (cluster_verbose_level[rt->id] >= 3 &&
                  !plogw->f_console_emu) {
                console->printf("[CL2 INIT %u] %s\n",
                                (unsigned)(command_index + 1),
                                command.c_str());
              }
            }

            ++rt->startup_index;
            st->timeout = millis() + 500;
          } else {
            st->stat = 5;
            renew_timeout_cluster(rt);
          }

          break;
        }

        println_tcpserver(rt->client, callsign);
        if (cluster_verbose_level[rt->id] >= 3 && !plogw->f_console_emu)
          console->printf("[CL%u TX] %s\n", (unsigned)(rt->id + 1), callsign);
        st->stat = 2;
        st->timeout = millis() + 500;
      }
      break;
    case 2:
    case 3:
    case 4:
      if (st->timeout < millis()) {
        println_tcpserver(rt->client, cluster_cmd[st->stat - 2]);
        if (cluster_verbose_level[rt->id] >= 3 && !plogw->f_console_emu)
          console->printf("[CL%u TX] %s\n", (unsigned)(rt->id + 1), cluster_cmd[st->stat - 2]);
        st->stat++;
        st->timeout = millis() + 500;
      }
      break;
    case 6:
      if (rt->client->connected()) {
        const char *cmd = cluster_user_cmd(rt->id);
        if (*cmd) println_tcpserver(rt->client, cmd);
        if (cluster_verbose_level[rt->id] >= 3 && !plogw->f_console_emu)
          console->printf("[CL%u TX user] %s\n", (unsigned)(rt->id + 1), cmd);
      }
      st->stat = 5;
      renew_timeout_cluster(rt);
      break;
    case 5:
      if (!rt->client->connected()) {
        // The AsyncTCP/lwIP close path may still be running after
        // connected() becomes false.  Calling stop() again here can close
        // the same PCB twice.  Let onDisconnect_cluster() finish the close
        // and keep this connection in the reconnect backoff state.
        if (!rt->disconnect_handled) {
          rt->disconnect_handled = true;
          rt->startup_index = 0;
          rt->rx_line_len = 0;
          rt->rx_discard_until_eol = false;
          st->stat = 10;
          st->timeout = millis() + 60000;
          if (!plogw->f_console_emu) {
            console->printf(
                "cluster %u found disconnected; retry in 60 sec\n",
                (unsigned)(rt->id + 1));
          }
        }
      } else if (passed_timeout_cluster(rt)) {
        console->printf(
            "cluster %u inactive for 5 minutes; reconnecting in 60 sec\n",
            (unsigned)(rt->id + 1));

        // Move to backoff before initiating the asynchronous close.  The
        // disconnect callback is deliberately suppressed for this
        // logger-initiated close, so the main loop and callback cannot both
        // close or reschedule the same AsyncClient.
        rt->disconnect_handled = true;
        rt->disconnect_notice_pending = false;
        rt->startup_index = 0;
        rt->rx_line_len = 0;
        rt->rx_discard_until_eol = false;
        st->stat = 10;
        st->timeout = millis() + 60000;
        rt->client->stop();
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
  memtrace_event("cluster before io init");
  cluster_io_init();
  memtrace_event("cluster after io init");
  init_cluster_state(&cluster, cluster_buf);
  init_cluster_state(&cluster2, cluster2_buf);

  memset(cluster_rt, 0, sizeof(cluster_rt));

  cluster_rt[0].state = &cluster;
  memtrace_event("before cluster clients");
  cluster_rt[0].client = new AsyncClient;
  cluster_rt[0].port = 7000;
  cluster_rt[0].id = 0;
  cluster_rt[0].startup_index = 0;

  cluster_rt[1].state = &cluster2;
  cluster_rt[1].client = new AsyncClient;
  cluster_rt[1].port = 7000;
  cluster_rt[1].id = 1;
  cluster_rt[1].startup_index = 0;

  client_tcp = cluster_rt[0].client;

  for (uint8_t i = 0; i < N_CLUSTER_CONNECTIONS; ++i) {
    cluster_rt[i].client->onData(handleData_cluster, &cluster_rt[i]);
    cluster_rt[i].client->onConnect(onConnect_cluster, &cluster_rt[i]);
    cluster_rt[i].client->onDisconnect(onDisconnect_cluster, &cluster_rt[i]);
  }
  set_cluster();
  set_cluster2();
  memtrace_event("after cluster clients");
}

static void disconnect_cluster_id(uint8_t id, bool hold) {
  ClusterRuntime *rt = &cluster_rt[id];
  if (!rt->client) return;

  // Make repeated manual disconnect requests idempotent.
  if (hold && rt->hold_requested && rt->state->stat == 11) return;

  rt->hold_requested = hold;
  rt->disconnect_notice_pending = false;
  rt->disconnect_handled = true;
  rt->state->stat = hold ? 11 : 0;
  rt->state->timeout = 0;

  if (rt->client->connected()) rt->client->stop();
  if (!plogw->f_console_emu) {
    console->printf("cluster %u disconnected%s\n",
                    (unsigned)(id + 1), hold ? " (held)" : "");
  }
}

void disconnect_cluster() { disconnect_cluster_id(0, true); }
void disconnect_cluster_temp() { disconnect_cluster_id(0, false); }
void disconnect_cluster2() { disconnect_cluster_id(1, true); }
void disconnect_cluster2_temp() { disconnect_cluster_id(1, false); }

static int connect_cluster_id(uint8_t id) {
  ClusterRuntime *rt = &cluster_rt[id];
  if (!cluster_auto_enabled(id) || wifi_status != 1 || !rt->client || !rt->server[0]) return 0;
  rt->hold_requested = false;
  rt->disconnect_handled = false;
  rt->disconnect_notice_pending = false;
  if (!rt->client->connected()) {
    rt->state->stat = 0;
    rt->state->timeout = 0;
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

bool send_cluster_terminal_cmd(uint8_t cluster_no, const char *cmd, Stream *out) {
  if (!out) out = console;

  if (cluster_no < 1 || cluster_no > N_CLUSTER_CONNECTIONS) {
    out->println("cluster number must be 1 or 2");
    return false;
  }

  if (!cmd) cmd = "";

  while (*cmd == ' ' || *cmd == '\t') {
    ++cmd;
  }

  if (*cmd == '\0') {
    out->printf("usage: c%ucmd command\n", (unsigned)cluster_no);
    return false;
  }

  ClusterRuntime *rt = &cluster_rt[cluster_no - 1];

  if (!rt->client || !rt->client->connected()) {
    out->printf("cluster %u is not connected\n",
                (unsigned)cluster_no);
    return false;
  }

  /*
   * Send directly without changing the persistent Cluster Cmd
   * settings stored in plogw.
   */
  println_tcpserver(rt->client, cmd);

  out->printf("sent to cluster %u: %s\n",
              (unsigned)cluster_no, cmd);

  return true;
}

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
  if (rt->client && rt->client->connected()) {
    rt->disconnect_handled = true;
    rt->connected_state = false;
    rt->client->stop();
  }
  rt->hold_requested = !cluster_auto_enabled(id);
  rt->disconnect_notice_pending = false;
  rt->state->stat = (rt->server[0] && cluster_auto_enabled(id)) ? 0 : 11;
  if (!plogw->f_console_emu)
    console->printf("cluster %u server:%s port:%d%s\n", (unsigned)(id + 1),
                    rt->server, rt->port, rt->server[0] ? "" : " (disabled)");
}

void set_cluster_auto(uint8_t cluster_no, int enabled) {
  if (cluster_no < 1 || cluster_no > N_CLUSTER_CONNECTIONS) return;
  const uint8_t id = cluster_no - 1;
  if (id == 0) cluster1_auto_enable = enabled ? 1 : 0;
  else cluster2_auto_enable = enabled ? 1 : 0;

  ClusterRuntime *rt = &cluster_rt[id];
  if (!rt->state || !rt->client) return;

  rt->disconnect_notice_pending = false;
  if (!cluster_auto_enabled(id)) {
    rt->hold_requested = true;
    rt->state->stat = 11;
    rt->state->timeout = 0;
    rt->connected_state = false;
    if (rt->client->connected()) {
      rt->disconnect_handled = true;
      rt->client->stop();
    }
  } else {
    rt->hold_requested = false;
    rt->disconnect_handled = false;
    if (!rt->client->connected()) {
      rt->connected_state = false;
      rt->state->stat = rt->server[0] ? 0 : 11;
      rt->state->timeout = 0;
    }
  }
}

int get_cluster_auto(uint8_t cluster_no) {
  if (cluster_no == 1) return cluster1_auto_enable ? 1 : 0;
  if (cluster_no == 2) return cluster2_auto_enable ? 1 : 0;
  return 0;
}

bool cluster_is_connected(uint8_t cluster_no) {
  if (cluster_no < 1 || cluster_no > N_CLUSTER_CONNECTIONS) return false;
  ClusterRuntime *rt = &cluster_rt[cluster_no - 1];
  return cluster_auto_enabled(cluster_no - 1) && rt->client &&
         rt->client->connected() && rt->connected_state;
}

const char *cluster_connection_state(uint8_t cluster_no) {
  if (cluster_no < 1 || cluster_no > N_CLUSTER_CONNECTIONS) return "INVALID";
  const uint8_t id = cluster_no - 1;
  ClusterRuntime *rt = &cluster_rt[id];
  if (!cluster_auto_enabled(id)) return "OFF";
  if (!rt->server[0]) return "NOT CONFIGURED";
  if (cluster_is_connected(cluster_no)) return "CONNECTED";
  if (rt->state && rt->state->stat == 10) return "RETRY WAIT";
  return "CONNECTING";
}

void set_cluster() { set_cluster_id(0, plogw->cluster_name + 2); }
void set_cluster2() { set_cluster_id(1, plogw->cluster2_name + 2); }


