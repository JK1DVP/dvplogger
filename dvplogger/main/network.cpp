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
#include "main.h"
#include "network.h"
#include "timekeep.h"
#include "cluster.h"
#include "display.h"
#include "tcp_server.h"
#include "zserver.h"
#include "AsyncTCP.h"
#include "SD.h"
#include "settings.h"
#include "misc.h"

#include "ESP32FtpServer.h"
//FtpServer ftpSrv;   //set #define FTP_DEBUG in ESP32FtpServer.h to see ftp verbose on serial


#include <WiFi.h>  // for WiFi shield

#include <espwmap.h>
#include <ESPmDNS.h>

#include <WiFiUdp.h>
#include <esp_sntp.h>


//#include <ESP_Mail_Client.h>

// network 関連の管理について
// setup() で1度、その後loop() で1秒ごとにwifi_enable == True であれば、wifi 接続状態を確認して接続できるようであれば接続する。-> wifi_status = True とする。
// wifi_count が累積5回程度NGなら、wifi_enable = False として、その後は、チェックしない。WIFIコマンドで接続しなおしをすることができる。

// network 利用のプロセスは、それぞれで、wifi をチェックするのでなく、wifi_status を確認してTrue の時だけ実行するようにする。

void init_multiwifi() {
  // read from sd for ssid and password if file is available 

  f=SD.open("/wifiset.txt","r");
  char *ssid,*pass;
  
  if (f) {
    // read from wifiset.txt to add all listed ssid
    while (readline(&f, buf, 0x0d0a, 128) != 0) {
      ssid = strtok(buf," ");
      if (ssid!= NULL) {
	pass= strtok(NULL," ");
	if (pass!= NULL) {
	  // read ssid and pass
	  plogw->ostream->print("setting wifi:");	  	  
	  plogw->ostream->print(ssid);
	  //	  plogw->ostream->print(" ");
	  //	  plogw->ostream->println(pass);	  
	  ESPWMAP.add(ssid,pass);
	}
      }
    }
    f.close();
  } else {
    plogw->ostream->println("fail opening wifi setting file /wifiset.txt");
  }
  
  ESPWMAP.begin();
}

void multiwifi_addap(char *ssid,char *passwd)
{
  // check
  if ((*ssid !='\0') && (*passwd != '\0')) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);    
    ESPWMAP.add(ssid,passwd);
    sprintf(dp->lcdbuf, "addAp:%s",ssid);

    // update wifiset.txt for the entries
    File f1;
    f  = SD.open("/wifiset.new","w");
    
    f1 = SD.open("/wifiset.txt","r");
    if (f1) {
      // read f1 (existing entries) and updat
      while (readline(&f1, buf, 0x0d0a, 128) != 0) {
	//	plogw->ostream->print("f1 read:");
	//	plogw->ostream->println(buf);
	
	if (strncmp(ssid,buf,strlen(ssid))==0) {
	  // this entry hits the current add ap entry
	  plogw->ostream->print(ssid);
	  plogw->ostream->println(" already exists");
	  // --> skip
	} else {
	  // copy the entry to f
	  f.println(buf);
	}
      }
      f1.close();
    }
    // append current entry
    f.print(ssid);
    f.print(" ");
    f.println(passwd);
    f.flush();
    f.close();
    
    // rename after removing old file
    if (SD.remove("/wifiset.txt")) {
      plogw->ostream->println("removed wifiset.txt");
    }
    
    if (SD.rename("/wifiset.new","/wifiset.txt") == 0) {
      if (!plogw->f_console_emu) plogw->ostream->println("renaming /wifiset.new /wifiset.txt failed");
    } else {
      plogw->ostream->println("success renaming wifiset.new to wifiset.txt");
    }
      
    plogw->ostream->println("wifiset updated");
  } else {
    sprintf(dp->lcdbuf, "Please set\nSSID and Passwd");
  }
  upd_display_info_flash(dp->lcdbuf);  
}

void print_wifiinfo() {
  plogw->ostream->print(WiFi.localIP().toString());
  plogw->ostream->print(" ");
  plogw->ostream->print(WiFi.SSID());
  plogw->ostream->print(" ");
  plogw->ostream->println(WiFi.RSSI());
}

void localip_to_string(char *buf)
{
  WiFi.localIP().toString().toCharArray(buf, 16);
}


static volatile bool ntp_sync_event_pending = false;

void time_sync_notification_cb(struct timeval *tv)
{
  (void)tv;
  // Keep the lwIP/SNTP callback minimal.  LCD/log work is done later from
  // service_network_background() in the normal application context.
  ntp_sync_event_pending = true;
}


void init_network() {
  plogw->ostream->println("init_network()");
  sprintf(dp->lcdbuf, "init_network()\nPlease Wait");
  upd_display_info_flash(dp->lcdbuf);
  
  memtrace_event("network before multiwifi");
  init_multiwifi() ;
  memtrace_event("network after multiwifi");
  //  WiFi.begin(ssid, password);
  memtrace_event("network before wifi check");
  check_wifi();
  memtrace_event("network after wifi check");
  console->println("MDNS()");
  memtrace_event("network before mdns");
  MDNS.begin(plogw->hostname+2); // ホスト名
  memtrace_event("network after mdns");

  // older 
  //  timeClient.begin();
  //  plogw->ostream->println("timeclient started");
  // now uses system sntp

  // NTP is started by service_network_background() only after the local
  // Wi-Fi link has remained stable for 30 seconds.  The production path uses
  // the ESP-IDF/lwIP SNTP implementation directly.
  memtrace_event("network ntp deferred");
  
  
  memtrace_event("before init_cluster_info");
  init_cluster_info();
  memtrace_event("after init_cluster_info");
  plogw->ostream->println("inited cluster info");
  
  memtrace_event("before init_zserver_info");
  init_zserver_info();
  memtrace_event("after init_zserver_info");
  plogw->ostream->println("inited zserver info");
  
  //  for (int i = 0; i < MAX_SRV_CLIENTS; i++) serverClients_status[i] = 0;
  
  memtrace_event("before init_tcpserver");
  init_tcpserver();
  memtrace_event("after init_tcpserver");
  sprintf(dp->lcdbuf, "init_network()\nFinished");
  upd_display_info_flash(dp->lcdbuf);
  
  print_wifiinfo();
  plogw->ostream->println("tcp server start ");
  plogw->ostream->println("init_network() end");


  //  ftpSrv.begin("esp32","esp32");    //username, password for ftp.  set ports in ESP32FtpServer.h  (default 21, 50009 for PASV)
  //  plogw->ostream->println("ftp server start");
  
  
}

void ftp_service_loop()
{
  //  ftpSrv.handleFTP();        //make sure in loop you call handleFTP()!!   
  
}


namespace {
// Local-link age and external-service staging.  Internet reachability is not
// used to decide whether Wi-Fi itself is connected.
uint32_t wifi_link_connected_since_ms = 0;

// ESP-IDF/lwIP SNTP is run as a short one-shot attempt.  On success DVPlogger
// stops SNTP and schedules the next refresh for 30 minutes later.  On failure
// it stops SNTP and backs off for five minutes, avoiding background DNS/UDP
// retry traffic when Internet access is unavailable.
constexpr uint32_t NTP_START_DELAY_MS = 30000;
constexpr const char *NTP_SERVER_HOST = "pool.ntp.org";
constexpr uint32_t NTP_SYNC_TIMEOUT_MS = 20000;
constexpr uint32_t NTP_NORMAL_INTERVAL_MS = 1800UL * 1000UL;
constexpr uint32_t NTP_RETRY_BACKOFF_MS = 300UL * 1000UL;
constexpr uint32_t NTP_STATUS_INTERVAL_MS = 1000;

bool ntp_client_started = false;
bool ntp_synced_state = false;
uint32_t ntp_client_started_ms = 0;
uint32_t ntp_next_attempt_ms = 0;
uint32_t ntp_last_status_ms = 0;

// Wi-Fi timing diagnostics use the common performance/timing verbose bit.
constexpr uint32_t ESPWMAP_SLOW_US = 10000;
constexpr uint32_t ESPWMAP_SUMMARY_MS = 30000;

wl_status_t timed_espwmap_handle(const char *phase) {
  static uint32_t max_us = 0;
  static uint32_t calls = 0;
  static uint32_t slow_calls = 0;
  static uint32_t last_summary_ms = 0;

  const wl_status_t before = WiFi.status();
  const uint32_t started_us = micros();
  const wl_status_t handled = ESPWMAP.handle();
  const uint32_t elapsed_us = micros() - started_us;
  const wl_status_t after = WiFi.status();

  calls++;
  if (elapsed_us > max_us) max_us = elapsed_us;
  if (elapsed_us >= ESPWMAP_SLOW_US) slow_calls++;

  // A slow call is always reported because it can directly explain an
  // input/display stall.  Verbose mode additionally reports state changes
  // and a periodic summary, while avoiding one log line per normal call.
  const bool state_changed = before != after || handled != after;
  if (elapsed_us >= ESPWMAP_SLOW_US ||
      ((verbose & VERBOSE_PERF) && state_changed)) {
    const int rssi = (after == WL_CONNECTED) ? WiFi.RSSI() : 0;
    snprintf(buf, 128,
             "ESPWMAP timing phase=%s dt=%luus max=%luus calls=%lu slow=%lu "
             "before=%d handled=%d after=%d wifi_status=%d count=%d rssi=%d",
             phase,
             (unsigned long)elapsed_us,
             (unsigned long)max_us,
             (unsigned long)calls,
             (unsigned long)slow_calls,
             (int)before, (int)handled, (int)after,
             wifi_status, wifi_count, rssi);
    console->println(buf);
  }

  const uint32_t now_ms = millis();
  if ((verbose & VERBOSE_PERF) &&
      now_ms - last_summary_ms >= ESPWMAP_SUMMARY_MS) {
    last_summary_ms = now_ms;
    snprintf(buf, 128,
             "ESPWMAP summary calls=%lu slow=%lu max=%luus status=%d "
             "wifi_status=%d count=%d rssi=%d",
             (unsigned long)calls,
             (unsigned long)slow_calls,
             (unsigned long)max_us,
             (int)after, wifi_status, wifi_count,
             after == WL_CONNECTED ? WiFi.RSSI() : 0);
    console->println(buf);
  }

  return handled;
}

void stop_ntp_service() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  ntp_client_started = false;
  ntp_client_started_ms = 0;
  ntp_last_status_ms = 0;
  ntp_sync_event_pending = false;
}

void start_ntp_service() {
  stop_ntp_service();

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
  esp_sntp_setservername(0, NTP_SERVER_HOST);

  ntp_client_started = true;
  ntp_client_started_ms = millis();
  ntp_last_status_ms = 0;
  ntp_next_attempt_ms = 0;

  console->printf("NTP: starting ESP-IDF SNTP server=%s timeout=%lu ms\n",
                  NTP_SERVER_HOST, (unsigned long)NTP_SYNC_TIMEOUT_MS);
  esp_sntp_init();
}

void service_ntp_attempt() {
  if (!ntp_client_started) return;

  const uint32_t now = millis();
  const uint32_t age_ms = now - ntp_client_started_ms;
  const sntp_sync_status_t status = esp_sntp_get_sync_status();
  const uint8_t reach = sntp_getreachability(0);

  if ((verbose & VERBOSE_PERF) &&
      (ntp_last_status_ms == 0 ||
       (uint32_t)(now - ntp_last_status_ms) >= NTP_STATUS_INTERVAL_MS)) {
    ntp_last_status_ms = now;
    console->printf(
        "NTP: waiting age=%lu ms status=%d reach=0x%02x enabled=%d\n",
        (unsigned long)age_ms, (int)status, (unsigned)reach,
        esp_sntp_enabled() ? 1 : 0);
  }

  // Prefer the callback flag so a short-lived COMPLETED status cannot be
  // missed.  Keep the status test as a harmless fallback.
  if (ntp_sync_event_pending || status == SNTP_SYNC_STATUS_COMPLETED) {
    const bool was_synced = ntp_synced_state;
    ntp_sync_event_pending = false;
    console->printf("NTP: synchronized after %lu ms reach=0x%02x\n",
                    (unsigned long)age_ms, (unsigned)reach);
    stop_ntp_service();
    ntp_synced_state = true;
    ntp_next_attempt_ms = millis() + NTP_NORMAL_INTERVAL_MS;

    if (!was_synced) {
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf), "NTP\nSynchronized");
      upd_display_info_flash(dp->lcdbuf);
    }
    return;
  }

  if (age_ms >= NTP_SYNC_TIMEOUT_MS) {
    const bool was_synced = ntp_synced_state;
    console->printf(
        "NTP: sync timeout after %lu ms status=%d reach=0x%02x; "
        "retry in 300 s\n",
        (unsigned long)age_ms, (int)status, (unsigned)reach);
    stop_ntp_service();
    ntp_synced_state = false;
    ntp_next_attempt_ms = millis() + NTP_RETRY_BACKOFF_MS;

    // Initial inability to reach NTP is quiet.  Only a real
    // SYNCHRONIZED -> UNSYNCHRONIZED transition interrupts the LCD.
    if (was_synced)
      network_display_error("NETWORK ERROR\nNTP lost\nRetry in 5 min");
  }
}
}  // namespace

void network_display_error(const char *message) {
  if (message == nullptr || *message == '\0') return;

  // Async network callbacks may report the same transition more than once.
  // Suppress only identical messages in a short interval; different failures
  // are still shown immediately.  upd_display_info_flash() safely queues the
  // request when called outside the main loop.
  static char last_message[96] = {0};
  static uint32_t last_message_ms = 0;
  const uint32_t now = millis();
  if (strncmp(last_message, message, sizeof(last_message) - 1) == 0 &&
      (uint32_t)(now - last_message_ms) < 3000) {
    return;
  }

  strncpy(last_message, message, sizeof(last_message) - 1);
  last_message[sizeof(last_message) - 1] = '\0';
  last_message_ms = now;
  upd_display_info_flash(last_message);
}

int check_wifi() {
  // Keep a valid local Wi-Fi link even when the tethering phone has no
  // Internet route.  ESPWMAP.handle() may scan/reselect APs, so calling it
  // while already associated can tear down a perfectly usable local link.
  static uint32_t next_connect_attempt_ms = 0;
  static wl_status_t last_reported_status = WL_NO_SHIELD;
  constexpr uint32_t WIFI_CONNECT_RETRY_MS = 5000;

  if (!wifi_enable) {
    wifi_status = 0;
    return 0;
  }

  const wl_status_t current = WiFi.status();

  if (current == WL_CONNECTED) {
    const bool newly_connected = (wifi_status == 0);
    wifi_count = 0;
    wifi_status = 1;
    next_connect_attempt_ms = 0;

    if (newly_connected) {
      wifi_link_connected_since_ms = millis();
      ntp_next_attempt_ms = 0;
      snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
               "WiFi Connected\n%s\nLocal services OK",
               WiFi.localIP().toString().c_str());
      upd_display_info_flash(dp->lcdbuf);
      memtrace_event("wifi connected");
      console->printf("WIFI: link connected ip=%s rssi=%d; Internet not required\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    return 1;
  }

  // The station link is really down.  Do not treat DNS, NTP, cluster, or
  // zserver failures as Wi-Fi failures; only WiFi.status() controls this path.
  const bool link_was_up = (wifi_status == 1 || wifi_link_connected_since_ms != 0);
  wifi_status = 0;
  wifi_link_connected_since_ms = 0;
  if (link_was_up) {
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "NETWORK ERROR\nWiFi link lost\nReconnecting...");
    network_display_error(dp->lcdbuf);
  }
  if (ntp_client_started || esp_sntp_enabled()) {
    stop_ntp_service();
    ntp_next_attempt_ms = 0;
    ntp_synced_state = false;
    console->println("NTP: stopped because local Wi-Fi link is down");
  }

  const uint32_t now = millis();
  if ((int32_t)(now - next_connect_attempt_ms) < 0) return 0;
  next_connect_attempt_ms = now + WIFI_CONNECT_RETRY_MS;

  const wl_status_t status = timed_espwmap_handle("connect");
  if (status == WL_CONNECTED || WiFi.status() == WL_CONNECTED) {
    wifi_count = 0;
    wifi_status = 1;
    wifi_link_connected_since_ms = millis();
    next_connect_attempt_ms = 0;
    ntp_next_attempt_ms = 0;
    snprintf(dp->lcdbuf, sizeof(dp->lcdbuf),
             "WiFi Connected\n%s\nLocal services OK",
             WiFi.localIP().toString().c_str());
    upd_display_info_flash(dp->lcdbuf);
    memtrace_event("wifi connected");
    return 1;
  }

  // Report only on state change or every fifth retry.  Repeated OLED updates
  // and logs made an Internet outage look like a system-wide failure.
  if (status != last_reported_status || (wifi_count % 5) == 0) {
    console->printf("WIFI: link retry count=%d status=%d mode=%d; local AP not connected\n",
                    wifi_count, (int)status, (int)WiFi.getMode());
    last_reported_status = status;
  }
  wifi_count++;

  // Do not disable Wi-Fi after a fixed number of failures.  A tethering AP may
  // return later, and local Web access should recover automatically.
  return 0;
}



bool network_external_service_ready(uint32_t delay_ms) {
  if (!wifi_enable || WiFi.status() != WL_CONNECTED || wifi_status == 0)
    return false;
  if (wifi_link_connected_since_ms == 0) return false;
  return (uint32_t)(millis() - wifi_link_connected_since_ms) >= delay_ms;
}

void service_network_background() {
  if (!network_external_service_ready(NTP_START_DELAY_MS)) return;

  const uint32_t now = millis();

  if (ntp_client_started) {
    service_ntp_attempt();
    return;
  }

  if (ntp_next_attempt_ms != 0 &&
      (int32_t)(now - ntp_next_attempt_ms) < 0) {
    return;
  }

  start_ntp_service();
}

bool network_ntp_started() { return ntp_client_started; }
bool network_ntp_synced() { return ntp_synced_state; }

void set_wifi_enabled(int enabled) {
  wifi_count = 0;
  wifi_status = 0;
  cluster.stat = 0;

  if (!enabled) {
    wifi_enable = 0;
    wifi_link_connected_since_ms = 0;
    if (ntp_client_started || esp_sntp_enabled()) {
      stop_ntp_service();
    }
    ntp_next_attempt_ms = 0;
    ntp_synced_state = false;
    console->println("WIFI: disabling station and disconnecting");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  wifi_enable = 1;
  console->println("WIFI: enabling station and restarting ESPWMAP");

  // WIFI_OFF cannot be recovered by ESPWMAP.handle() alone.  Restore STA
  // mode and restart the multi-AP manager before attempting a connection.
  WiFi.mode(WIFI_STA);
  delay(100);
  ESPWMAP.begin();

  int result = check_wifi();
  snprintf(buf, 128, "WIFI: reconnect attempt result=%d status=%d mode=%d",
           result, (int)WiFi.status(), (int)WiFi.getMode());
  console->println(buf);
}

/// AsyncTCP comm related routines
int println_tcpserver(void *arg,const char *s)
{
  AsyncClient *client = reinterpret_cast<AsyncClient *>(arg);
  // send s 
  if (client->space() > strlen(s)+2 && client->canSend())    {
    client->add(s, strlen(s));
    client->add("\r\n", 2);    
    client->send();
    return 1;
  } else {
    return 0;
  }
}
