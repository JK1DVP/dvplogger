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
// SPDX-FileCopyrightText: 2025 2021-2025 Eiichiro Araki
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "decl.h"
#include "SD.h"
#include "variables.h"
#include "qso.h"
//#include <SPIFFS.h>
#include <math.h>
#include <maidenhead.h>
#include "settings.h"
#include "misc.h"
#include "cat.h"
#include <map>  // std::mapを使用するためにインクルード
#include "so2r.h"
#include "log.h"
#include "contest.h"
#include "user_contest_md.h"
#include "timekeep.h"
#include "esp_heap_caps.h"
#include "bandmap.h"
#include "dupechk.h"
#include "multi_process.h"
#include <algorithm>
#include <memory>

#include <stdarg.h>
#include <stdio.h>
#include <errno.h>

namespace {
constexpr size_t WEB_LOG_LINE_SIZE = 192;
constexpr uint8_t WEB_LOG_QUEUE_LEN = 8;

struct WebLogLine {
  char text[WEB_LOG_LINE_SIZE];
};

static WebLogLine web_log_queue[WEB_LOG_QUEUE_LEN];
static volatile uint8_t web_log_head = 0;
static volatile uint8_t web_log_tail = 0;
static volatile uint32_t web_log_dropped = 0;
static portMUX_TYPE web_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void enqueue_web_log_line(const char *text) {
  if (!text) return;
  portENTER_CRITICAL(&web_log_mux);
  uint8_t next = (uint8_t)((web_log_head + 1) % WEB_LOG_QUEUE_LEN);
  if (next == web_log_tail) {
    ++web_log_dropped;
    portEXIT_CRITICAL(&web_log_mux);
    return;
  }
  strlcpy(web_log_queue[web_log_head].text, text, WEB_LOG_LINE_SIZE);
  web_log_head = next;
  portEXIT_CRITICAL(&web_log_mux);
}

class DeferredWebLogPrint : public Print {
public:
  size_t write(uint8_t c) override {
    portENTER_CRITICAL(&mux_);
    if (c == '\r') {
      portEXIT_CRITICAL(&mux_);
      return 1;
    }
    if (c == '\n' || len_ >= sizeof(line_) - 1) {
      line_[len_] = '\0';
      if (len_ > 0) enqueue_web_log_line(line_);
      len_ = 0;
      if (c != '\n') line_[len_++] = (char)c;
    } else {
      line_[len_++] = (char)c;
    }
    portEXIT_CRITICAL(&mux_);
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    if (!buffer) return 0;
    for (size_t i = 0; i < size; ++i) write(buffer[i]);
    return size;
  }

private:
  char line_[WEB_LOG_LINE_SIZE];
  size_t len_ = 0;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

static DeferredWebLogPrint webLog;
}

void process_web_terminal_log_queue() {
  for (;;) {
    WebLogLine line;
    bool have_line = false;
    uint32_t dropped = 0;

    portENTER_CRITICAL(&web_log_mux);
    if (web_log_tail != web_log_head) {
      line = web_log_queue[web_log_tail];
      web_log_tail = (uint8_t)((web_log_tail + 1) % WEB_LOG_QUEUE_LEN);
      have_line = true;
    } else if (web_log_dropped != 0) {
      dropped = web_log_dropped;
      web_log_dropped = 0;
    }
    portEXIT_CRITICAL(&web_log_mux);

    if (have_line) {
      console->print("[WEB] ");
      console->println(line.text);
      continue;
    }
    if (dropped) {
      console->printf("[WEB] %lu log message(s) dropped\n", (unsigned long)dropped);
    }
    break;
  }
}


AsyncWebServer web_server(80);

const char* PARAM_MESSAGE = "message";

void notFound(AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
}


void rebootESP(String message) {
  webLog.print("Rebooting ESP32: "); webLog.println(message);
  ESP.restart();
}

String humanReadableSize(const size_t bytes);

// list all of the files, if ishtml=true, return html rather than simple text
String listFiles(bool ishtml) {
  String returnText = "";
  webLog.println("Listing files stored on SPIFFS");
  File root = SD.open("/");
  File foundfile = root.openNextFile();
  if (ishtml) {
    returnText += "<table><tr><th align='left'>Name</th><th align='left'>Size</th></tr>";
  }
  while (foundfile) {
    if (ishtml) {
      returnText += "<tr align='left'><td>" + String(foundfile.name()) + "</td><td>" + humanReadableSize(foundfile.size()) + "</td></tr>";
    } else {
      returnText += "File: " + String(foundfile.name()) + "\n";
    }
    foundfile = root.openNextFile();
  }
  if (ishtml) {
    returnText += "</table>";
  }
  root.close();
  foundfile.close();
  return returnText;
}

// Make size of files human readable
// source: https://github.com/CelliesProjects/minimalUploadAuthESP32
String humanReadableSize(const size_t bytes) {
  if (bytes < 1024) return String(bytes) + " B";
  else if (bytes < (1024 * 1024)) return String(bytes / 1024.0) + " KB";
  else if (bytes < (1024 * 1024 * 1024)) return String(bytes / 1024.0 / 1024.0) + " MB";
  else return String(bytes / 1024.0 / 1024.0 / 1024.0) + " GB";
}
// handles uploads
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();
  webLog.println(logmessage);

  if (!index) {
    logmessage = "Upload Start: " + String(filename);
    // open the file on first call and store the file handle in the request object
    request->_tempFile = SD.open("/" + filename, "w");
    webLog.println(logmessage);
  }

  if (len) {
    // stream the incoming chunk to the opened file
    request->_tempFile.write(data, len);
    logmessage = "Writing file: " + String(filename) + " index=" + String(index) + " len=" + String(len);
    webLog.println(logmessage);
  }

  if (final) {
    logmessage = "Upload Complete: " + String(filename) + ",size: " + String(index + len);
    // close the file handle as the upload is now done
    request->_tempFile.close();
    webLog.println(logmessage);
    request->redirect("/");
  }
}

String processor(const String& var) {
  if (var == "FILELIST") {
    return listFiles(true);
  }
  if (var == "FREESPIFFS") {
    return humanReadableSize((SD.totalBytes() - SD.usedBytes()));
  }

  if (var == "USEDSPIFFS") {
    return humanReadableSize(SD.usedBytes());
  }

  if (var == "TOTALSPIFFS") {
    return humanReadableSize(SD.totalBytes());
  }

  if (var == "GRID_LOCATOR") {
    if (plogw!=NULL) {
      return String(plogw->grid_locator_set);
    } else {
      return String("");
    }
  }

  return String();
}


// Maidenhead Grid Locator → 緯度・経度 (中心点)
void gridToLatLon(const char* grid, double& lat, double& lon) {
  if (!grid || strlen(grid) < 4) {
    lat = 0;
    lon = 0;
    return;
  }

  char U = 'A';  // Base for letters
  lon = (grid[0] - 'A') * 20 - 180;
  lat = (grid[1] - 'A') * 10 - 90;

  lon += (grid[2] - '0') * 2;
  lat += (grid[3] - '0') * 1;

  if (strlen(grid) >= 6) {
    lon += (grid[4] - 'A') * (2.0 / 24.0);
    lat += (grid[5] - 'A') * (1.0 / 24.0);

    // 中心に合わせる
    lon += (2.0 / 24.0) / 2.0;
    lat += (1.0 / 24.0) / 2.0;
  } else {
    // 4文字グリッドなら中央に補正
    lon += 1.0;
    lat += 0.5;
  }
}


// ハバーサイン距離（km）
double haversine(double lat1, double lon1, double lat2, double lon2) {
  double R = 6371.0;
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  lat1 = radians(lat1);
  lat2 = radians(lat2);
  double a = sin(dLat/2) * sin(dLat/2) +
             sin(dLon/2) * sin(dLon/2) * cos(lat1) * cos(lat2);
  double c = 2 * atan2(sqrt(a), sqrt(1-a));
  return R * c;
}

#define DEG_TO_RAD (M_PI / 180.0)
#define RAD_TO_DEG (180.0 / M_PI)
// 2地点間の初期方位（北=0°, 東=90°…）を度で返す
double calculateBearing(double lat1, double lon1, double lat2, double lon2) {
  double dLon = (lon2 - lon1) * DEG_TO_RAD;
  lat1 *= DEG_TO_RAD;
  lat2 *= DEG_TO_RAD;

  double y = sin(dLon) * cos(lat2);
  double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon);
  double bearing = atan2(y, x) * RAD_TO_DEG;

  // 0～360度に正規化
  return fmod((bearing + 360.0), 360.0);
}


struct ParkInfo {
  double dist;   // km
  String code;   // JA-0001 …
  String name;
  double bearing;
  /* デフォルト (必須) */
  ParkInfo() : dist(1e9), code(""), name(""),bearing(0.0) {}

  /* 値付きコンストラクタ */
  ParkInfo(double d, const String& c, const String& n, double b)
    : dist(d), code(c), name(n),bearing(b) {}
};



// `/nearest?grid=PM95ru`
void setupNearestHandler(AsyncWebServer &server) {
  server.on("/nearest", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("grid")) {
      request->send(400, "text/plain", "Missing grid param");
      return;
    }

    String grid = request->getParam("grid")->value();
    grid.toUpperCase();  // 必須
    float myLat = 0, myLon = 0;
    //    gridToLatLon(grid.c_str(), myLat, myLon);

    char gridstr[10];
    strcpy(gridstr,grid.c_str());
    webLog.print("grid=");webLog.print(gridstr);
    webLog.print("<-");webLog.println(grid.c_str());
	      
    myLat=mh2lat(gridstr);
    myLon=mh2lon(gridstr);

    webLog.print("lat,lon=");webLog.print(myLat);webLog.print(" ");webLog.println(myLon);

    File f = SD.open("/pota-jp.csv", "r");
    if (!f) {
      request->send(500, "text/plain", "File open error");
      return;
    }

    ParkInfo top3[3];
    int found = 0;

    while (f.available()) {
      String line = f.readStringUntil('\n');

      if (line.startsWith("reference")) continue;

      int c1 = line.indexOf(',');
      int c2 = line.indexOf(',', c1 + 1);
      int c3 = line.indexOf(',', c2 + 1);
      int c4 = line.indexOf(',', c3 + 1);
      int c5 = line.indexOf(',', c4 + 1);
      int c6 = line.indexOf(',', c5 + 1);

      String code = line.substring(0, c1);
      String name = line.substring(c1 + 1, c2);
      double lat = line.substring(c2 + 1, c3).toFloat();
      double lon = line.substring(c3 + 1, c4).toFloat();
      /*      webLog.print("code:");webLog.print(code);
      webLog.print("name:");webLog.print(name);      
      webLog.print(" lat:");webLog.print(lat);
      webLog.print(" lon:");webLog.println(lon);
      */
      

      if (lat == 0 || lon == 0) continue;

      double dist = haversine(myLat, myLon, lat, lon);
      double bearing = calculateBearing(myLat, myLon, lat, lon);

      yield();
      // 上位3件に追加または更新
      if (found < 3) {
        top3[found++] = {dist, code, name, bearing };
      } else {
        // 一番遠いのを探して置き換え
        int maxIndex = 0;
        for (int i = 1; i < 3; ++i) {
          if (top3[i].dist > top3[maxIndex].dist) maxIndex = i;
        }
        if (dist < top3[maxIndex].dist) {
          top3[maxIndex] = {dist, code, name, bearing};
        }
      }
    }
    f.close();

    // ソート（昇順）
    for (int i = 0; i < found - 1; ++i) {
      for (int j = i + 1; j < found; ++j) {
        if (top3[i].dist > top3[j].dist) {
          ParkInfo temp = top3[i];
          top3[i] = top3[j];
          top3[j] = temp;
        }
      }
    }

    // JSON出力
    String result = "[";
    for (int i = 0; i < found; ++i) {
      if (i > 0) result += ",";
      result += "{\"code\":\"" + top3[i].code + "\",";
      result += "\"name\":\"" + top3[i].name + "\",";
      result += "\"distance_km\":" + String(top3[i].dist, 2) +",";
      result += "\"bearing_deg\":" + String(top3[i].bearing, 1) + "}";
    }
    result += "]";
    request->send(200, "application/json", result);
  });
}


struct SummitInfo {
  double dist;
  String code;   // JA/KN-001 …
  String name;
  int    alt;    // 標高 m
  double bearing;

  SummitInfo() : dist(1e9), code(""), name(""),  alt(0), bearing(0) {}
  SummitInfo(double d, const String& c, const String& n, int a,double b)
    : dist(d), code(c), name(n), alt(a),bearing(b) {}
};

void setupNearestSummit(AsyncWebServer &server) {
  server.on("/nearest_summit", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!req->hasParam("grid")) { req->send(400,"text/plain","grid?"); return; }
    String grid = req->getParam("grid")->value();
    grid.toUpperCase();  // 必須
    float myLat=0,myLon=0;
    //gridToLatLon(grid.c_str(), myLat, myLon);
    char gridstr[10];
    strcpy(gridstr,grid.c_str());
    webLog.print("grid=");webLog.print(gridstr);
    webLog.print("<-");webLog.println(grid.c_str());
	      
    myLat=mh2lat(gridstr);
    myLon=mh2lon(gridstr);
    webLog.print("lat,lon=");webLog.print(myLat);webLog.print(" ");webLog.println(myLon);

    File f = SD.open("/ja_sota.csv","r");
    if (!f){ req->send(500,"text/plain","SOTA CSV open err"); return; }
    webLog.println("open ja_sota.csv");

    SummitInfo best[3]; int filled=0;
    while(f.available()){
      String line=f.readStringUntil('\n');
      if(line.startsWith("summitCode")) continue;
      //      webLog.println(line);
      int c1=line.indexOf(',');
      int c2=line.indexOf(',',c1+1);
      int c3=line.indexOf(',',c2+1);
      int c4=line.indexOf(',',c3+1);
      int c5=line.indexOf(',',c4+1);
      int c6=line.indexOf(',',c5+1);
      //JA/YN-082,Oomuroyama,35.44090,138.65359,1468
      String code=line.substring(0,c1);
      String name=line.substring(c1+1,c2);
      double lat= line.substring(c2+1,c3).toFloat();
      double lon= line.substring(c3+1,c4).toFloat();
      int alt   = line.substring(c4+1,c5).toInt();      
      if(lat==0||lon==0) continue;
      //      webLog.print("lat:");webLog.print(lat);
      //      webLog.print("lon:");webLog.println(lon);      
      yield();
      double d = haversine(myLat,myLon,lat,lon);
      double bearing = calculateBearing(myLat, myLon, lat, lon);

      if(filled<3){ best[filled++]={d,code,name,alt,bearing }; }
      else{
        int far=0;
	for(int i=1;i<3;i++) {
	  if(best[i].dist>best[far].dist) far=i;
	}
        if(d<best[far].dist) {
	  best[far]={d,code,name,alt,bearing };
	}
      }
    }
    f.close();
    // 距離で昇順ソート
    for(int i=0;i<filled-1;i++) for(int j=i+1;j<filled;j++)
      if(best[i].dist>best[j].dist){ SummitInfo t=best[i]; best[i]=best[j]; best[j]=t; }

    String json="[";
    for(int i=0;i<filled;i++){
      if(i) json+=",";
      json+="{\"code\":\""+best[i].code+"\",\"name\":\""+best[i].name+
            "\",\"alt\":"+String(best[i].alt)+
            ",\"distance_km\":"+String(best[i].dist,2)+
            ",\"bearing_deg\":"+String(best[i].bearing,1)+	
	"}";
    }
    json+="]";
    webLog.println(json);
    req->send(200,"application/json",json);
  });
}

//#include <AsyncTCP.h>
//#include <ESPAsyncWebServer.h>

// 必要な外部参照
//extern const char *pwin_index(int i);         // 編集する文字列
//extern const char *pwin_name_index(int i);    // 項目名

enum InputRestrict { Allowall, Callsign, Nospace };

InputRestrict pwin_type_index(int i) {
  switch (i) {
  case 0:return Callsign; // My Call
  case 1:return Nospace; // SentExch
  case 7:return Allowall; // Cluster Cmd
  case 5:return Nospace; // "Wifi_SSID";
  case 6:return Nospace; // "Wifi_Passwd";
  default : return Allowall;
  }
}  

const int N_EDITWIN=25;
const char *pwin_name_index(int i) {
  switch (i) {
  case 0:return "My Call";
  case 1:return "Sent Exch";
  case 2:return "Contest Name";
  case 3:return "Power Code";    
  case 4:return "JCC/JCG POTA/ SOTA/";
  case 5:return "Wifi_SSID";
  case 6:return "Wifi_Passwd";
  case 7:return "Cluster Name";        
  case 8:return "Cluster Cmd";    
  case 9:return "Sat Name";
  case 10:return "Grid Locator";    
  case 11:return "My Name";
  case 12:return "zServer Name";        
  case 13:return "CW Message 0 F1";
  case 14:return "CW Message 1 F2";
  case 15:return "CW Message 2 F3";
  case 16:return "CW Message 3 F4";
  case 17:return "CW Message 4 F5";
  case 18:return "CW Message 5 F6";
  case 19:return "CW Message 6 F7";        
  default : return "--";
  }
  
}

char *pwin_index(int i) {
  switch (i) {
  case 0:return plogw->my_callsign;
  case 1:return plogw->sent_exch;
  case 2:return plogw->contest_name;   // "Contest Name";
  case 3:return plogw->power_code;// "Power Code";    
  case 4:return plogw->jcc;  // "JCC/JCG POTA/ SOTA/";
  case 5:return plogw->wifi_ssid; //"Wifi_SSID";
  case 6:return plogw->wifi_passwd; //"Wifi_Passwd";
  case 7:return plogw->cluster_name; //"Cluster Name";
  case 8:return plogw->cluster_cmd; //"Cluster Cmd";
  case 9:return plogw->sat_name;//"Sat Name";
  case 10:return plogw->grid_locator; //"Grid Locator";    
  case 11:return plogw->my_name; //"My Name";
  case 12:return plogw->zserver_name; //"zServer Name";        
  case 13:return plogw->cw_msg[0]; // cw_msg[0]
  case 14:return plogw->cw_msg[1]; // cw_msg[1]
  case 15:return plogw->cw_msg[2]; // cw_msg[2]
  case 16:return plogw->cw_msg[3]; // cw_msg[3]
  case 17:return plogw->cw_msg[4]; // cw_msg[4]
  case 18:return plogw->cw_msg[5]; // cw_msg[5]                    
  case 19:return plogw->cw_msg[6]; // cw_msg[6]                    
  default:return NULL;
  }
}



const char *settings_page_html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>DVPlogger Settings</title>
  <style>
    body { font-family: sans-serif; margin: 20px; }
    input[type="text"] { width: 60%; padding: 5px; margin: 5px 0; }
    label { display: block; margin-top: 10px; font-weight: bold; }
    .setting { margin-bottom: 15px; }
  </style>
</head>
<body>
  <h2>DVPlogger Settings</h2>
<button onclick="fetch('/save_settings').then(() => alert('Settings saved'));">Save</button>
<button onclick="fetch('/load_settings').then(() => {
  alert('Settings loaded');
  location.reload();  // ページを再読み込み
});">Load</button>

  <form id="settingsForm">
    %SETTINGS_INPUTS%
  </form>
  <p id="status"></p>
<script>
function updateSetting(index) {
  const input = document.getElementById('edit_' + index);
  const value = encodeURIComponent(input.value);
  fetch(`/set_edit?index=${index}&value=${value}`)
    .then(res => res.text())
    .then(msg => {
      document.getElementById("status").innerText = msg;
    });
}

document.addEventListener("DOMContentLoaded", () => {
  const inputs = document.querySelectorAll("input[type=text]");
  inputs.forEach(input => {
    input.addEventListener("keydown", function(event) {
      if (event.key === "Enter") {
        event.preventDefault();
        const index = this.dataset.index;
        updateSetting(index);
      }
    });
  });
});
</script>
</body>
</html>
)rawliteral";


static const char rigs_page_header[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>DVPlogger RIG Settings</title>
  <style>
    body { font-family: sans-serif; margin: 20px; }
    input[type="text"] { width: 60%; padding: 5px; margin: 5px 0; }
    label { display: block; margin-top: 10px; font-weight: bold; }
    .setting { margin-bottom: 15px; }
  </style>
</head>
<body>
  <h2>DVPlogger RIG Settings</h2>
<p>Separated args by ,(comma)</p>
<ul>
  <li><strong>CW:<em>cwport</em></strong> (1–3)</li>
  <li><strong>B:<em>baudrate</em></strong></li>
  <li><strong>P:<em>catport_number</em></strong> ((-2:Manual) ‑1:USB, 1:Bluetooth, 2:CI‑V, 3:CAT, 4:CAT2)</li>
  <li><strong>ADR:<em>CI‑V_address</em></strong></li>
  <li><strong>NAME:<em>rig_name</em></strong></li>
  <li><strong>XVTR:<em>transverter_frequencies</em></strong> Frequencies joined by `_` from index 0: IFlo_0 IFhi_0 RFlo_0 RFhi_0 IFlo_1 …</li>
  <li><strong>R:<em>CAT_reverse_polarity</em></strong> (0/1)</li>
  <li><strong>BM:<em>band_mask</em></strong> in_hex</li>
  <li><strong>TP:<em>cat_type</em>_<em>rig_type</em></strong> <br>(cat_type 0: ICOM CIV, 1:Yaesu(New), 2:Kenwood 3:Manual(NoCAT) 4:Yaesu(old))<br> (rig_type 0:IC-705 1:IC-9700 2:Yaesu 3:Kenwood 5:IC-7300 )</li> 
</ul>
<p>Press Enter in input box to reflect changes.</p>
<p><a href="/" >go back to Home</a></p>
<button onclick="fetch('/save_rigs').then(() => location.reload());">Save RIGs</button>
<button onclick="fetch('/load_rigs').then(() => location.reload());">Load RIGs</button>
  <form id="settingsForm">
)rawliteral";

static const char rigs_page_footer[] PROGMEM = R"rawliteral(
  </form>
  <p id="status"></p>
<script>
function updateSetting(index) {
  const input = document.getElementById('edit_' + index);
  const value = encodeURIComponent(input.value);
  fetch(`/rig_edit?index=${index}&value=${value}`)
    .then(res => res.text())
    .then(msg => {
      document.getElementById("status").innerText = msg;
      // 設定変更後にページリロード
      setTimeout(() => location.reload(), 500);
    });
}
document.addEventListener("DOMContentLoaded", () => {
  const inputs = document.querySelectorAll("input[type=text]");
  inputs.forEach(input => {
    input.addEventListener("keydown", function(event) {
      if (event.key === "Enter") {
        event.preventDefault();
        const index = this.dataset.index;
        updateSetting(index);
      }
    });
  });
});
</script>
</body>
</html>
)rawliteral";

const char *example_input_html = R"rawliteral(
<div class="setting">
  <label for="edit_%d">%s</label>
  <input type="text" id="edit_%d" data-index="%d" value="%s" maxlength="%d"
    %s>
</div>
)rawliteral";


const char *pattern_upper = "oninput=\"this.value = this.value.toUpperCase()\"";
const char *pattern_no_space = "oninput=\"this.value = this.value.replace(/[ \\t]/g,'')\"";



// また組み合わせパターン
const char *pattern_both = 
  "oninput=\"this.value = this.value.toUpperCase().replace(/[^A-Z0-9\\/]/g,'')\"";


// settings_page_htmlとexample_input_html は前述の定数文字列

static const char contests_page_header[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVPlogger Contest Selection</title>
<style>
body{font-family:sans-serif;margin:18px;max-width:1250px}
table{border-collapse:collapse;width:100%;margin:12px 0 24px}
th,td{border:1px solid #aaa;padding:6px;text-align:left;vertical-align:middle}
tr.current{font-weight:bold;background:#e8f3ff}
input{box-sizing:border-box;padding:5px;font-size:.95em;width:100%;min-width:9em}
button{padding:5px 10px;white-space:nowrap}.dupe-ok{color:#075f16;font-weight:bold}.dupe-ng{color:#9b1c1c;font-weight:bold}
#status{min-height:1.4em;font-weight:bold}.note{font-size:.9em}.name{white-space:nowrap}
.contest-wrap{overflow-x:auto;width:100%}.contest-table{table-layout:fixed;min-width:1180px}
.contest-table .col-id{width:38px}.contest-table .col-name{width:150px}.contest-table .col-dupe{width:80px}
.contest-table .col-msg{width:175px}.contest-table .col-exch{width:150px}.contest-table .col-action{width:145px}
.user-table{table-layout:fixed;min-width:1120px}
.user-table .col-user-name{width:220px}.user-table .col-msg{width:175px}
.user-table .col-exch{width:150px}.user-table .col-action{width:145px}
.user-name-field{display:flex;align-items:center;gap:4px;white-space:nowrap}
.user-name-field input{min-width:0;flex:1}
.help{max-width:900px}.help th:first-child,.help td:first-child{white-space:nowrap}.examples code{white-space:nowrap}
</style></head><body><h2>Contest selection</h2>
<p>Current contest: <strong>%CURRENT_CONTEST%</strong></p>
<p class="note"><strong>%SD_STATUS%</strong><br>Last action: %LAST_STATUS%</p><p id="status"></p>
<p class="note">Select &amp; Save stores the F1, F2, F3, F5 and sent exchange preset on the SD card, then activates the contest.</p>
<div class="contest-wrap"><table class="contest-table"><colgroup>
<col class="col-id"><col class="col-name"><col class="col-dupe">
<col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-msg">
<col class="col-exch"><col class="col-action"></colgroup>
<thead><tr><th>ID</th><th>Contest</th><th>Dupe</th><th>CW F1 (CQ)</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>Sent EXCH</th><th>Action</th></tr></thead><tbody>
)rawliteral";

static const char contests_page_footer[] PROGMEM = R"rawliteral(
</tbody></table></div><h3>User contest (.MD)</h3>
<p>Enter the filename without <code>User</code> and <code>.MD</code>. Two User contest settings are retained independently.</p>
<div class="contest-wrap"><table class="user-table"><colgroup>
<col class="col-user-name"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-exch"><col class="col-action">
</colgroup><thead><tr><th>User MD filename</th><th>CW F1 (CQ)</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>Sent EXCH</th><th>Action</th></tr></thead><tbody>
<tr%USER1_CLASS%>
<td><form id="user_contest_form_1" method="GET" action="/select_user_contest"><input type="hidden" name="slot" value="0"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_1" name="filename" maxlength="8" value="%USER1_FILENAME%" placeholder="PRESET1" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"></div></td>
<td><input form="user_contest_form_1" name="f1" maxlength="30" value="%USER1_F1%"></td><td><input form="user_contest_form_1" name="f2" maxlength="30" value="%USER1_F2%"></td><td><input form="user_contest_form_1" name="f3" maxlength="30" value="%USER1_F3%"></td><td><input form="user_contest_form_1" name="f5" maxlength="30" value="%USER1_F5%"></td><td><input form="user_contest_form_1" name="exch" maxlength="17" value="%USER1_EXCH%"></td>
<td><button form="user_contest_form_1" type="submit">%USER1_ACTION%</button></td></tr>
<tr%USER2_CLASS%>
<td><form id="user_contest_form_2" method="GET" action="/select_user_contest"><input type="hidden" name="slot" value="1"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_2" name="filename" maxlength="8" value="%USER2_FILENAME%" placeholder="PRESET2" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"></div></td>
<td><input form="user_contest_form_2" name="f1" maxlength="30" value="%USER2_F1%"></td><td><input form="user_contest_form_2" name="f2" maxlength="30" value="%USER2_F2%"></td><td><input form="user_contest_form_2" name="f3" maxlength="30" value="%USER2_F3%"></td><td><input form="user_contest_form_2" name="f5" maxlength="30" value="%USER2_F5%"></td><td><input form="user_contest_form_2" name="exch" maxlength="17" value="%USER2_EXCH%"></td>
<td><button form="user_contest_form_2" type="submit">%USER2_ACTION%</button></td></tr>
</tbody></table></div>
<p class="note">The MD file must exist as <code>/FILENAME.MD</code>. Allowed filename characters: A-Z, 0-9, _ and -.</p>

<h3>CW message macros</h3>
<p class="note">Enter the sent exchange itself (for example <code>11</code> or <code>1115</code>) in the <strong>Sent EXCH</strong> box. Use <code>$W</code> in a CW message to transmit that value. Number abbreviation follows the DVPlogger CW-number abbreviation setting.</p>
<table class="help"><thead><tr><th>Macro</th><th>Expanded value</th></tr></thead><tbody>
<tr><td><code>$I</code></td><td>Your callsign</td></tr>
<tr><td><code>$C</code></td><td>Other station's callsign</td></tr>
<tr><td><code>$U</code></td><td><code>CQ</code> in CW/Digital mode</td></tr>
<tr><td><code>$T</code></td><td><code>TEST</code> in CW/Digital mode</td></tr>
<tr><td><code>$A</code></td><td><code>TU</code> in CW/Digital mode</td></tr>
<tr><td><code>$V</code></td><td>Sent RST; normally abbreviated as <code>5NN</code> in CW</td></tr>
<tr><td><code>$W</code></td><td>Sent EXCH entered in this table</td></tr>
<tr><td><code>$P</code></td><td>Band-dependent power code</td></tr>
<tr><td><code>$J</code></td><td>Your JCC/JCG number from the logger settings</td></tr>
<tr><td><code>$S</code></td><td>Current sequential QSO number</td></tr>
<tr><td><code>$Q</code></td><td>Next band-specific sequential number, formatted with three digits</td></tr>
<tr><td><code>$N</code></td><td>Your operator name</td></tr>
</tbody></table>

<h3>Examples</h3>
<table class="help examples"><thead><tr><th>Key</th><th>Example</th><th>Typical result</th></tr></thead><tbody>
<tr><td>F1</td><td><code>$U $T DE $I $I $T</code></td><td><code>CQ TEST DE JK1DVP JK1DVP TEST</code></td></tr>
<tr><td>F2</td><td><code>$C $V $W</code></td><td><code>JA1ABC 5NN 1115</code></td></tr>
<tr><td>F3</td><td><code>$A $I $T</code></td><td><code>TU JK1DVP TEST</code></td></tr>
<tr><td>F5</td><td><code>$C $V$W$P</code></td><td><code>JA1ABC 5NN1115M</code></td></tr>
</tbody></table>
<p class="note">Spaces written in the message are transmitted as word spaces. Macros may be joined without spaces, as in <code>$V$W$P</code>.</p>
<p><a href="/">Back to Home</a></p>
</body></html>
)rawliteral";

struct ContestWebPreset {
  bool used;
  char name[LEN_CONTEST_NAME + 1];
  char f1[LEN_CWMSG_WINDOW + 1];
  char f2[LEN_CWMSG_WINDOW + 1];
  char f3[LEN_CWMSG_WINDOW + 1];
  char f5[LEN_CWMSG_WINDOW + 1];
  char exch[LEN_SENT_EXCH_WINDOW + 1];
};

static constexpr int MAX_CONTEST_WEB_PRESETS = N_CONTEST + 8;
static ContestWebPreset contest_web_presets[MAX_CONTEST_WEB_PRESETS];
static bool contest_web_presets_loaded = false;
static constexpr int N_USER_CONTEST_SLOTS = 2;
static char contest_web_user_slot[N_USER_CONTEST_SLOTS][9];
static const char *CONTEST_PRESET_FILE = "/CONTEST.TXT";
static const char *CONTEST_PRESET_VFS_FILE = "/sd/CONTEST.TXT";
static String contest_web_last_status = "No contest action has been received since boot.";
static bool contest_web_file_loaded = false;
static size_t contest_web_file_size = 0;

static bool valid_web_user_md_basename(const String &filename);

static void set_contest_web_status(const String &message) {
  contest_web_last_status = message;
  if (console) console->printf("WEB CONTEST: %s\n", message.c_str());
  Serial.printf("WEB CONTEST: %s\n", message.c_str());
}

static String contest_web_sd_status() {
  String s = "microSD: ";
  if (SD.cardType() == CARD_NONE) return s + "not mounted / no card";
  s += "mounted, preset=";
  if (SD.exists(CONTEST_PRESET_FILE)) {
    File f = SD.open(CONTEST_PRESET_FILE, FILE_READ);
    if (f) {
      s += CONTEST_PRESET_FILE;
      s += " (";
      s += String(f.size());
      s += " bytes)";
      f.close();
    } else {
      s += "exists but cannot open";
    }
  } else {
    s += "not created yet";
  }
  return s;
}

static void copy_web_value(char *dst, size_t dst_size, const String &src) {
  if (!dst || dst_size == 0) return;
  size_t n = 0;
  while (n + 1 < dst_size && n < src.length()) {
    char c = src.charAt(n);
    dst[n] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    ++n;
  }
  dst[n] = '\0';
}

static String html_attr_escape(const char *src) {
  String out;
  if (!src) return out;
  while (*src) {
    switch (*src) {
      case '&': out += F("&amp;"); break;
      case '"': out += F("&quot;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      default: out += *src; break;
    }
    ++src;
  }
  return out;
}

static String json_string_escape(const char *src) {
  String out;
  if (!src) return out;
  while (*src) {
    switch (*src) {
      case '\\': out += F("\\\\"); break;
      case '"': out += F("\\\""); break;
      case '\r': out += F("\\r"); break;
      case '\n': out += F("\\n"); break;
      case '\t': out += F("\\t"); break;
      default: out += *src; break;
    }
    ++src;
  }
  return out;
}

static ContestWebPreset *find_contest_web_preset(const char *name, bool create) {
  if (!name || !*name) return NULL;
  ContestWebPreset *free_slot = NULL;
  for (int i = 0; i < MAX_CONTEST_WEB_PRESETS; ++i) {
    if (contest_web_presets[i].used) {
      if (strcasecmp(contest_web_presets[i].name, name) == 0) return &contest_web_presets[i];
    } else if (!free_slot) free_slot = &contest_web_presets[i];
  }
  if (!create || !free_slot) return NULL;
  memset(free_slot, 0, sizeof(*free_slot));
  free_slot->used = true;
  strlcpy(free_slot->name, name, sizeof(free_slot->name));
  return free_slot;
}

static void initialize_user_contest_slot_defaults() {
  static const char *default_filename[N_USER_CONTEST_SLOTS] = {
    "PRESET1", "PRESET2"
  };

  for (int i = 0; i < N_USER_CONTEST_SLOTS; ++i) {
    if (!contest_web_user_slot[i][0]) {
      strlcpy(contest_web_user_slot[i], default_filename[i],
              sizeof(contest_web_user_slot[i]));
    }

    String contest_name = String("User") + contest_web_user_slot[i];
    if (find_contest_web_preset(contest_name.c_str(), false)) continue;

    ContestWebPreset *p = find_contest_web_preset(contest_name.c_str(), true);
    if (!p) continue;
    copy_web_value(p->f1, sizeof(p->f1), String(plogw->cw_msg[0] + 2));
    copy_web_value(p->f2, sizeof(p->f2), String(plogw->cw_msg[1] + 2));
    copy_web_value(p->f3, sizeof(p->f3), String(plogw->cw_msg[2] + 2));
    copy_web_value(p->f5, sizeof(p->f5), String(plogw->cw_msg[4] + 2));
    copy_web_value(p->exch, sizeof(p->exch), String(plogw->sent_exch + 2));
  }
}

static void load_contest_web_presets() {
  if (contest_web_presets_loaded) return;
  contest_web_presets_loaded = true;
  memset(contest_web_presets, 0, sizeof(contest_web_presets));
  memset(contest_web_user_slot, 0, sizeof(contest_web_user_slot));
  File f = SD.open(CONTEST_PRESET_FILE, FILE_READ);
  if (!f) {
    contest_web_file_loaded = false;
    contest_web_file_size = 0;
    initialize_user_contest_slot_defaults();
    set_contest_web_status(String("preset file not found at ") + CONTEST_PRESET_FILE + "; using default User presets");
    return;
  }
  contest_web_file_loaded = true;
  contest_web_file_size = f.size();
  set_contest_web_status(String("loaded ") + CONTEST_PRESET_FILE + " (" + String(contest_web_file_size) + " bytes)");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    if (!line.length()) continue;

    bool slot_line = false;
    for (int i = 0; i < N_USER_CONTEST_SLOTS; ++i) {
      String prefix = String("#USER_SLOT") + String(i + 1) + "=";
      if (!line.startsWith(prefix)) continue;
      String filename = line.substring(prefix.length());
      filename.trim();
      filename.toUpperCase();
      if (valid_web_user_md_basename(filename)) {
        strlcpy(contest_web_user_slot[i], filename.c_str(), sizeof(contest_web_user_slot[i]));
      }
      slot_line = true;
      break;
    }
    if (slot_line || line.charAt(0) == '#') continue;

    int p1 = line.indexOf('\t');
    int p2 = p1 < 0 ? -1 : line.indexOf('\t', p1 + 1);
    int p3 = p2 < 0 ? -1 : line.indexOf('\t', p2 + 1);
    int p4 = p3 < 0 ? -1 : line.indexOf('\t', p3 + 1);
    int p5 = p4 < 0 ? -1 : line.indexOf('\t', p4 + 1);
    if (p1 < 1 || p2 < 0 || p3 < 0) continue;
    String name = line.substring(0, p1);
    ContestWebPreset *p = find_contest_web_preset(name.c_str(), true);
    if (!p) continue;
    copy_web_value(p->f1, sizeof(p->f1), line.substring(p1 + 1, p2));
    if (p4 >= 0 && p5 >= 0) {
      copy_web_value(p->f2, sizeof(p->f2), line.substring(p2 + 1, p3));
      copy_web_value(p->f3, sizeof(p->f3), line.substring(p3 + 1, p4));
      copy_web_value(p->f5, sizeof(p->f5), line.substring(p4 + 1, p5));
      copy_web_value(p->exch, sizeof(p->exch), line.substring(p5 + 1));
    } else {
      copy_web_value(p->f3, sizeof(p->f3), line.substring(p2 + 1, p3));
      copy_web_value(p->exch, sizeof(p->exch), line.substring(p3 + 1));
    }
  }
  f.close();
  initialize_user_contest_slot_defaults();
}

static bool save_contest_web_presets() {
  if (SD.cardType() == CARD_NONE) {
    set_contest_web_status("save failed: microSD is not mounted");
    return false;
  }
  if (SD.exists(CONTEST_PRESET_FILE) && !SD.remove(CONTEST_PRESET_FILE)) {
    set_contest_web_status(String("save failed: cannot remove old ") + CONTEST_PRESET_FILE);
    return false;
  }
  // FILE_WRITE in the old Arduino-ESP32 core used by this project may open
  // an existing file without O_CREAT.  Use the already-mounted VFS path
  // explicitly so a new 8.3 file can always be created/truncated.
  FILE *fp = fopen(CONTEST_PRESET_VFS_FILE, "w");
  if (!fp) {
    set_contest_web_status(String("save failed: fopen(") + CONTEST_PRESET_VFS_FILE + ",w) failed, errno=" + String(errno));
    return false;
  }
  int n = fprintf(fp, "#USER_SLOT1=%s\n#USER_SLOT2=%s\n",
                  contest_web_user_slot[0], contest_web_user_slot[1]);
  if (n < 0) {
    fclose(fp);
    set_contest_web_status(String("save failed while writing User slots to ") + CONTEST_PRESET_FILE + ", errno=" + String(errno));
    return false;
  }
  size_t written = (size_t)n;
  n = fprintf(fp, "# contest-name\tF1\tF2\tF3\tF5\tsent-exchange\n");
  if (n > 0) written += (size_t)n;
  for (int i = 0; i < MAX_CONTEST_WEB_PRESETS; ++i) {
    const ContestWebPreset &p = contest_web_presets[i];
    if (!p.used) continue;
    n = fprintf(fp, "%s\t%s\t%s\t%s\t%s\t%s\n",
                p.name, p.f1, p.f2, p.f3, p.f5, p.exch);
    if (n < 0) {
      fclose(fp);
      set_contest_web_status(String("save failed while writing ") + CONTEST_PRESET_FILE + ", errno=" + String(errno));
      return false;
    }
    written += (size_t)n;
  }
  if (fflush(fp) != 0) {
    fclose(fp);
    set_contest_web_status(String("save failed while flushing ") + CONTEST_PRESET_FILE + ", errno=" + String(errno));
    return false;
  }
  if (fclose(fp) != 0) {
    set_contest_web_status(String("save failed while closing ") + CONTEST_PRESET_FILE + ", errno=" + String(errno));
    return false;
  }
  if (!SD.exists(CONTEST_PRESET_FILE)) {
    set_contest_web_status(String("save failed: ") + CONTEST_PRESET_FILE + " is absent after close");
    return false;
  }
  File verify = SD.open(CONTEST_PRESET_FILE, FILE_READ);
  if (!verify) {
    set_contest_web_status(String("save failed: cannot reopen ") + CONTEST_PRESET_FILE);
    return false;
  }
  const size_t verified = verify.size();
  verify.close();
  contest_web_file_loaded = true;
  contest_web_file_size = verified;
  set_contest_web_status(String("saved ") + CONTEST_PRESET_FILE + " (" + String(verified) + " bytes, write reported " + String(written) + ")");
  return true;
}

static void set_current_contest_messages(const ContestWebPreset &p) {
  strlcpy(plogw->cw_msg[0] + 2, p.f1, LEN_CWMSG_WINDOW + 1);
  plogw->cw_msg[0][1] = strlen(plogw->cw_msg[0] + 2);
  strlcpy(plogw->cw_msg[1] + 2, p.f2, LEN_CWMSG_WINDOW + 1);
  plogw->cw_msg[1][1] = strlen(plogw->cw_msg[1] + 2);
  strlcpy(plogw->cw_msg[2] + 2, p.f3, LEN_CWMSG_WINDOW + 1);
  plogw->cw_msg[2][1] = strlen(plogw->cw_msg[2] + 2);
  strlcpy(plogw->cw_msg[4] + 2, p.f5, LEN_CWMSG_WINDOW + 1);
  plogw->cw_msg[4][1] = strlen(plogw->cw_msg[4] + 2);
  strlcpy(plogw->sent_exch + 2, p.exch, LEN_SENT_EXCH_WINDOW + 1);
  plogw->sent_exch[1] = strlen(plogw->sent_exch + 2);
}

static AsyncWebParameter *contest_request_param(AsyncWebServerRequest *request, const char *name) {
  if (request->hasParam(name, true)) return request->getParam(name, true);
  if (request->hasParam(name)) return request->getParam(name);
  return NULL;
}

static bool update_preset_from_request(AsyncWebServerRequest *request, const char *name, ContestWebPreset **result) {
  AsyncWebParameter *f1 = contest_request_param(request, "f1");
  AsyncWebParameter *f2 = contest_request_param(request, "f2");
  AsyncWebParameter *f3 = contest_request_param(request, "f3");
  AsyncWebParameter *f5 = contest_request_param(request, "f5");
  AsyncWebParameter *exch = contest_request_param(request, "exch");
  if (!f1 || !f2 || !f3 || !f5 || !exch) return false;
  ContestWebPreset *p = find_contest_web_preset(name, true);
  if (!p) return false;
  copy_web_value(p->f1, sizeof(p->f1), f1->value());
  copy_web_value(p->f2, sizeof(p->f2), f2->value());
  copy_web_value(p->f3, sizeof(p->f3), f3->value());
  copy_web_value(p->f5, sizeof(p->f5), f5->value());
  copy_web_value(p->exch, sizeof(p->exch), exch->value());
  if (result) *result = p;
  return true;
}

static bool valid_web_user_md_basename(const String &filename) {
  if (filename.length() < 1 || filename.length() > 8) return false;
  for (size_t i = 0; i < filename.length(); ++i) {
    const char c = filename.charAt(i);
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

static void setupContestPageHandler() {
  load_contest_web_presets();

  web_server.on("/contests", HTTP_GET, [](AsyncWebServerRequest *request) {
    struct State { enum Stage:uint8_t {Header,Entry,Footer,Done} stage=Header; size_t offset=0,length=0; int index=0; char text[6144]; };
    std::shared_ptr<State> state = std::make_shared<State>();
    if (!state) {
      request->send(503, "text/plain", "Not enough memory to build contest page");
      return;
    }
    AsyncWebServerResponse *response=request->beginChunkedResponse("text/html",
      [state](uint8_t *buffer,size_t maxLen,size_t chunkIndex) mutable -> size_t {
        (void)chunkIndex; size_t written=0;
        auto prepare=[&](const char *source,bool footer){
          String text=FPSTR(source);
          if (!footer) {
            text.replace("%CURRENT_CONTEST%",html_attr_escape(plogw->contest_name+2));
            text.replace("%SD_STATUS%", html_attr_escape(contest_web_sd_status().c_str()));
            text.replace("%LAST_STATUS%", html_attr_escape(contest_web_last_status.c_str()));
          }
          else {
            for (int i = 0; i < N_USER_CONTEST_SLOTS; ++i) {
              String filename = contest_web_user_slot[i];
              if (!filename.length() && i == 0 && !contest_web_user_slot[1][0] &&
                  is_user_md_contest_name(plogw->contest_name + 2)) {
                filename = plogw->contest_name + 6;
              }

              String contest_name;
              if (filename.length()) contest_name = String("User") + filename;
              bool current = contest_name.length() &&
                             strcasecmp(plogw->contest_name + 2, contest_name.c_str()) == 0;
              ContestWebPreset *p = contest_name.length()
                                      ? find_contest_web_preset(contest_name.c_str(), false)
                                      : NULL;
              const char *f1 = p ? p->f1 : (current ? plogw->cw_msg[0] + 2 : "");
              const char *f2 = p ? p->f2 : (current ? plogw->cw_msg[1] + 2 : "");
              const char *f3 = p ? p->f3 : (current ? plogw->cw_msg[2] + 2 : "");
              const char *f5 = p ? p->f5 : (current ? plogw->cw_msg[4] + 2 : "");
              const char *ex = p ? p->exch : (current ? plogw->sent_exch + 2 : "");
              String tag = String("%USER") + String(i + 1);

              text.replace(tag + "_CLASS%", current ? " class=\"current\"" : "");
              text.replace(tag + "_FILENAME%", html_attr_escape(filename.c_str()));
              text.replace(tag + "_F1%", html_attr_escape(f1));
              text.replace(tag + "_F2%", html_attr_escape(f2));
              text.replace(tag + "_F3%", html_attr_escape(f3));
              text.replace(tag + "_F5%", html_attr_escape(f5));
              text.replace(tag + "_EXCH%", html_attr_escape(ex));
              text.replace(tag + "_ACTION%", current ? "Save / Re-select" : "Select &amp; Save");
            }
          }
          text.toCharArray(state->text,sizeof(state->text)); state->length=strnlen(state->text,sizeof(state->text)); state->offset=0;
        };
        auto copy=[&]()->bool { size_t remain=state->length-state->offset,room=maxLen-written,n=remain<room?remain:room; if(n){memcpy(buffer+written,state->text+state->offset,n);written+=n;state->offset+=n;} if(state->offset==state->length){state->offset=0;state->length=0;return true;} return false; };
        while(written<maxLen && state->stage!=State::Done){
          switch(state->stage){
          case State::Header:
            if (!state->length) prepare(contests_page_header, false);
            if (copy()) state->stage = State::Entry;
            break;
          case State::Entry:
            if(state->index>=contest_definition_count()){state->stage=State::Footer;break;}
            if(!state->length){
              int id=contest_definition_id(state->index); const char *name=contest_definition_name(state->index);
              bool current=plogw->contest_id==id && strcasecmp(plogw->contest_name+2,name)==0;
              ContestWebPreset *p=find_contest_web_preset(name,false);
              const char *f1=p?p->f1:(current?plogw->cw_msg[0]+2:plogw->cw_msg[0]+2);
              const char *f2=p?p->f2:(current?plogw->cw_msg[1]+2:plogw->cw_msg[1]+2);
              const char *f3=p?p->f3:(current?plogw->cw_msg[2]+2:plogw->cw_msg[2]+2);
              const char *f5=p?p->f5:(current?plogw->cw_msg[4]+2:plogw->cw_msg[4]+2);
              const char *ex=p?p->exch:(current?plogw->sent_exch+2:plogw->sent_exch+2);
              bool dupe_ok=contest_definition_mask(state->index)==CW_PH_DUPE_OK;
              String form_id = String("contest_form_") + String(state->index);
              String row=String("<tr")+(current?" class=\"current\"":"")+"><td><form id=\""+form_id+"\" method=\"GET\" action=\"/select_contest\"><input type=\"hidden\" name=\"id\" value=\""+String(id)+"\"></form>"+String(id)+"</td><td class=\"name\">"+html_attr_escape(name)+"</td><td class=\""+(dupe_ok?"dupe-ok":"dupe-ng")+"\">"+(dupe_ok?"OK C/P":"NG C/P")+"</td>";
              row += "<td><input form=\""+form_id+"\" name=\"f1\" maxlength=\"30\" value=\""+html_attr_escape(f1)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"f2\" maxlength=\"30\" value=\""+html_attr_escape(f2)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"f3\" maxlength=\"30\" value=\""+html_attr_escape(f3)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"f5\" maxlength=\"30\" value=\""+html_attr_escape(f5)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"exch\" maxlength=\"17\" value=\""+html_attr_escape(ex)+"\"></td>";
              row += String("<td><button form=\"") + form_id + "\" type=\"submit\">"
                     + (current ? "Save / Re-select" : "Select & Save")
                     + "</button></td></tr>\n";
              row.toCharArray(state->text,sizeof(state->text)); state->length=strnlen(state->text,sizeof(state->text)); state->offset=0;
            }
            if (copy()) ++state->index;
            break;
          case State::Footer:
            if (!state->length) prepare(contests_page_footer, true);
            if (copy()) state->stage = State::Done;
            break;
          case State::Done: break;
          }
        }
        return written;
      });
    response->addHeader("Cache-Control","no-store"); request->send(response);
  });

  web_server.on("/contest_preset", HTTP_GET, [](AsyncWebServerRequest *request) {
    if(!request->hasParam("name")){request->send(400,"text/plain","Missing name");return;}
    ContestWebPreset *p=find_contest_web_preset(request->getParam("name")->value().c_str(),false);
    String json="{\"f1\":\""; json += json_string_escape(p ? p->f1 : ""); json += "\",\"f2\":\""; json += json_string_escape(p ? p->f2 : "");
    json += "\",\"f3\":\""; json += json_string_escape(p ? p->f3 : ""); json += "\",\"f5\":\""; json += json_string_escape(p ? p->f5 : "");
    json += "\",\"exch\":\""; json += json_string_escape(p ? p->exch : ""); json += "\"}";
    request->send(200,"application/json",json);
  });

  web_server.on("/select_contest", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.printf("WEB CONTEST: request /select_contest params=%u\n", (unsigned)request->params());
    AsyncWebParameter *id_param=contest_request_param(request,"id");
    if(!id_param){set_contest_web_status("request rejected: missing contest id");request->send(400,"text/plain",contest_web_last_status);return;}
    int id=id_param->value().toInt(); int index=-1;
    for(int i=0;i<contest_definition_count();++i) if(contest_definition_id(i)==id){index=i;break;}
    if(index<0){set_contest_web_status(String("request rejected: invalid contest id ")+String(id));request->send(400,"text/plain",contest_web_last_status);return;}
    Serial.printf("WEB CONTEST: built-in id=%d name=%s\n", id, contest_definition_name(index));
    ContestWebPreset *p=NULL;
    if(!update_preset_from_request(request,contest_definition_name(index),&p)){set_contest_web_status(String("request rejected: missing preset values for ")+contest_definition_name(index));request->send(400,"text/plain",contest_web_last_status);return;}
    Serial.println("WEB CONTEST: preset values received; saving");
    if(!save_contest_web_presets()){request->send(500,"text/plain",contest_web_last_status);return;}
    Serial.println("WEB CONTEST: preset saved; applying contest");
    plogw->contest_id=id; set_contest_id(); set_current_contest_messages(*p);
    upd_display_info_contest_settings(so2r.radio_selected());
    set_contest_web_status(String("selected ") + (plogw->contest_name+2) + ", F2=\"" + (plogw->cw_msg[1]+2) + "\", EXCH=\"" + (plogw->sent_exch+2) + "\"");
    request->redirect("/contests");
  });

  web_server.on("/select_user_contest", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.printf("WEB CONTEST: request /select_user_contest params=%u\n", (unsigned)request->params());
    AsyncWebParameter *filename_param=contest_request_param(request,"filename");
    AsyncWebParameter *slot_param=contest_request_param(request,"slot");
    if(!filename_param){set_contest_web_status("User request rejected: missing filename");request->send(400,"text/plain",contest_web_last_status);return;}
    if(!slot_param){set_contest_web_status("User request rejected: missing slot");request->send(400,"text/plain",contest_web_last_status);return;}
    int slot=slot_param->value().toInt();
    if(slot<0 || slot>=N_USER_CONTEST_SLOTS){set_contest_web_status(String("User request rejected: invalid slot ")+slot_param->value());request->send(400,"text/plain",contest_web_last_status);return;}
    String filename=filename_param->value(); filename.toUpperCase();
    if(!valid_web_user_md_basename(filename)){set_contest_web_status(String("User request rejected: invalid filename ")+filename);request->send(400,"text/plain",contest_web_last_status);return;}
    if(user_md_contest_loading()){request->send(409,"text/plain","Another User contest is still loading");return;}
    String contestName=String("User")+filename;
    ContestWebPreset *p=NULL;
    if(!update_preset_from_request(request,contestName.c_str(),&p)){request->send(400,"text/plain","Missing or invalid preset values");return;}
    strlcpy(contest_web_user_slot[slot],filename.c_str(),sizeof(contest_web_user_slot[slot]));
    if(!save_contest_web_presets()){request->send(500,"text/plain",contest_web_last_status);return;}
    strncpy(plogw->contest_name+2,contestName.c_str(),LEN_CONTEST_NAME); plogw->contest_name[2+LEN_CONTEST_NAME]='\0';
    set_current_contest_messages(*p);
    upd_display_info_contest_settings(so2r.radio_selected());
    if(!start_user_md_contest(plogw->contest_name+2)){set_contest_web_status(String("saved preset, but failed to start loading /")+filename+".MD");request->send(400,"text/plain",contest_web_last_status);return;}
    set_contest_web_status(String("saved preset and started loading /")+filename+".MD, F2=\""+(plogw->cw_msg[1]+2)+"\", EXCH=\""+(plogw->sent_exch+2)+"\"");
    request->redirect("/contests");
  });
}

//AsyncWebServer web_server(80);

void setupSettingsPageHandler() {
  web_server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = settings_page_html;  // テンプレートを複製
    String inputs;

    for (int i = 0; i < N_EDITWIN; ++i) {
      if (pwin_index(i)==NULL) break;
      char line[256];
      const char *attr="";
      switch (pwin_type_index(i)) {
      case Allowall:attr="";break;
      case Callsign:attr=pattern_both;break;
      case Nospace:attr=pattern_no_space;break;
      }
      snprintf(line, sizeof(line), example_input_html,
               i,
               pwin_name_index(i),
               i,
               i,
               pwin_index(i)+2,
               pwin_index(i)[0] - 1,attr);
      inputs += line;
    }

    html.replace("%SETTINGS_INPUTS%", inputs);
    request->send(200, "text/html", html);
  });


  web_server.on("/rigs", HTTP_GET, [](AsyncWebServerRequest *request) {
    struct RigsPageState {
      enum Stage : uint8_t { Header, RigEntry, Footer, Done } stage = Header;
      size_t offset = 0;
      size_t line_length = 0;
      int rig_index = 0;
      char line[640];
    };

    RigsPageState state;
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/html",
      [state](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {
        (void)index;
        size_t written = 0;

        // Copy as much as possible from a NUL-terminated source while
        // remembering the current offset between chunk callbacks.
        auto copy_text = [&](const char *src) -> bool {
          const size_t length = strlen(src);
          const size_t remain = length - state.offset;
          const size_t ncopy = (remain < (maxLen - written))
                               ? remain : (maxLen - written);
          if (ncopy > 0) {
            memcpy(buffer + written, src + state.offset, ncopy);
            written += ncopy;
            state.offset += ncopy;
          }
          if (state.offset == length) {
            state.offset = 0;
            return true;
          }
          return false;
        };

        while (written < maxLen && state.stage != RigsPageState::Done) {
          switch (state.stage) {
          case RigsPageState::Header:
            if (copy_text(rigs_page_header)) {
              state.stage = RigsPageState::RigEntry;
            }
            break;

          case RigsPageState::RigEntry:
            if (state.line_length == 0) {
              while (state.rig_index < N_RIG &&
                     *rig_spec[state.rig_index].name == '\0') {
                state.rig_index = N_RIG;
              }

              if (state.rig_index >= N_RIG) {
                state.stage = RigsPageState::Footer;
                break;
              }

              char spec_buf[300];
              spec_buf[0] = '\0';
              print_rig_spec_str(state.rig_index, spec_buf);
              spec_buf[sizeof(spec_buf) - 1] = '\0';

              const int len = snprintf(state.line, sizeof(state.line),
                                       example_input_html,
                                       state.rig_index,
                                       rig_spec[state.rig_index].name,
                                       state.rig_index,
                                       state.rig_index,
                                       spec_buf,
                                       300,
                                       "");
              if (len < 0) {
                state.line[0] = '\0';
                state.line_length = 0;
                ++state.rig_index;
                break;
              }

              state.line_length = strnlen(state.line, sizeof(state.line));
              state.offset = 0;
            }

            {
              const size_t remain = state.line_length - state.offset;
              const size_t ncopy = (remain < (maxLen - written))
                                   ? remain : (maxLen - written);
              if (ncopy > 0) {
                memcpy(buffer + written, state.line + state.offset, ncopy);
                written += ncopy;
                state.offset += ncopy;
              }
              if (state.offset == state.line_length) {
                state.offset = 0;
                state.line_length = 0;
                ++state.rig_index;
              }
            }
            break;

          case RigsPageState::Footer:
            if (copy_text(rigs_page_footer)) {
              state.stage = RigsPageState::Done;
            }
            break;

          case RigsPageState::Done:
            break;
          }
        }

        return written;
      });

    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  web_server.on("/save_settings", HTTP_GET, [](AsyncWebServerRequest *request){
    release_memory();
    save_settings(""); 
    request->send(200, "text/plain", "Settings saved");
  });
  
  web_server.on("/load_settings", HTTP_GET, [](AsyncWebServerRequest *request){
    load_settings(""); 
    request->send(200, "text/plain", "Settings loaded");
  });


  web_server.on("/save_rigs", HTTP_GET, [](AsyncWebServerRequest *request){
    save_rigs("RIGS"); 
    request->send(200, "text/plain", "RIG Settings saved");
  });
  
  web_server.on("/load_settings", HTTP_GET, [](AsyncWebServerRequest *request){
    load_rigs("RIGS"); 
    request->send(200, "text/plain", "RIG Settings loaded");
  });
  

  // 設定更新ハンドラ
  web_server.on("/set_edit", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("index") && request->hasParam("value")) {
      int index = request->getParam("index")->value().toInt();
      String value = request->getParam("value")->value();
      if (index >= 0 && index < N_EDITWIN) {
	if (pwin_index(index)!=NULL) {
	  strncpy(pwin_index(index)+2, value.c_str(), pwin_index(index)[0] - 1);
	  (pwin_index(index)+2)[pwin_index(index)[0] - 1] = '\0';
	}
	request->send(200, "text/plain", "Updated setting.");
	return;
      }
    }
    request->send(400, "text/plain", "Invalid parameters.");
  });



  // RIG設定更新ハンドラ
  web_server.on("/rig_edit", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("index") && request->hasParam("value")) {
      int index = request->getParam("index")->value().toInt();
      String value = request->getParam("value")->value();
      struct rig *spec;
      //      webLog.print("rig_edit value:");webLog.println(value.c_str());
      set_rig_spec_from_str_rig(&rig_spec[index],value.c_str());
      
      //      if (index >= 0 && index < N_EDITWIN) {
      //	if (pwin_index(index)!=NULL) {
      //	  strncpy(pwin_index(index)+2, value.c_str(), pwin_index(index)[0] - 1);
      //	  (pwin_index(index)+2)[pwin_index(index)[0] - 1] = '\0';
      //	}
      request->send(200, "text/plain", "Updated setting.");
      //	return;
      //      }
      return;
    }
    request->send(400, "text/plain", "Invalid parameters.");
  });
}

struct QsoDumpState {
  File file;
  size_t pos = 0;
  bool isCurrent = false;
  String fname;
  bool finished = false;
  union qso_union_tag qso;
  int type = 0; // 0: dump 1:txt 2;adif 3:csv
  String pendingLine ="";
  char dumpbuf[1024];
  bool headerWritten = false;  // added
  String park;
  String summit;
  
};

size_t readQsoChunk( struct QsoDumpState& state, uint8_t* buffer, size_t maxLen) {

#define RECORD_SIZE sizeof(state.qso.all)
  size_t bytesWritten = 0;
  //  char raw[RECORD_SIZE + 1];  // 1レコード分
  //  raw[RECORD_SIZE] = '\0';

  //  char linebuf[2048];

  // 1. ヘッダー未出力なら先に出す
  if (!state.headerWritten) {
    String header;
    if (state.type == 4) {
      // show the leading part of html      
      header += "<body onload=\"document.getElementById('form').submit()\"><form id=\"form\" method=\"POST\" action=\"https://contest.jarl.org/cgi-bin/logsheetform.cgi\"> <input type=\"hidden\" name=\"command\" value=\"load\" />    <textarea name=\"logsheet_file\" cols=\"80\" rows=\"10\">\r\nDVPlogger text log follows; time:";
      header +=plogw->tm;
      header +="\r\n";
    } else if (state.type==2) {
      // adif header
      header+="<eoh>\n";
    } else {
      header = "";
    }

    if (header.length() <= maxLen) {
      memcpy(buffer, header.c_str(), header.length());
      bytesWritten += header.length();
      state.headerWritten = true;
      //      webLog.println("header written");
    } else {
      // ヘッダーすら入らない → ダミー送信
      memcpy(buffer, "\n", 1);
      return 1;
    }
  }

  // 2. pendingLine の処理（前回入らなかった行）
  if (!state.pendingLine.isEmpty()) {
    size_t len = state.pendingLine.length();
    if (bytesWritten + len <= maxLen) {
      memcpy(buffer + bytesWritten, state.pendingLine.c_str(), len);
      bytesWritten += len;
      state.pendingLine = "";
      webLog.println("pending line write");      
    } else {
      // まだ入らない → 1バイトだけ送る
      memcpy(buffer + bytesWritten, "\n", 1);
      webLog.println("dummy write +");
      return bytesWritten + 1;
    }
  }

  // 2. ファイル終端チェック
  if (!state.file || state.finished || state.pos >= state.file.size()) {
    state.finished = true;
    webLog.print("state.file:");    webLog.print(state.file);
    webLog.print(" state.finished:");    webLog.println(state.finished);    
    return 0;
  }

  // main qso read & dump loop
  webLog.print("maxLen:");      webLog.println(maxLen);
  while ((bytesWritten < maxLen) && (bytesWritten < 2048) ) {
    //  while ((bytesWritten < maxLen)) {
    if (state.pos >= state.file.size()) {
      state.finished = true;
      webLog.print("chk state.pos:");      webLog.print(state.pos);
      webLog.print(" state.file.size():");      webLog.println(state.file.size());      
      break;
    }

    state.file.seek(state.pos);
    size_t n = state.file.read((uint8_t*)state.qso.all, RECORD_SIZE);
    if (n < RECORD_SIZE) {
      state.finished = true;
      webLog.println("n<record_size");
      break;
    }
    state.pos += n;

    // 整形処理
    state.dumpbuf[0]='\0';
    if (state.type==0) {
      // dump
      strcat(state.dumpbuf,(char *)state.qso.all1);
    } else {
      reformat_qso_entry(&state.qso);

      // check park number
      char *p1;
      if (!state.park.isEmpty()) {
	if ((p1=strstr(state.qso.entry.remarks,"POTA_MY:"))!=NULL) { // my park information in POTA activation
	  char tmpbuf1[100];
	  strcpy(tmpbuf1,p1+8);
	  p1=strtok(tmpbuf1," ");
	  if (p1!=NULL) {
	    if (!state.park.equals(p1)) {
	      // match
	      continue;
	    }
	  } else {
	    continue;
	  }
	} else {
	  continue;
	}
      }

      if (!state.summit.isEmpty()) {
	if ((p1=strstr(state.qso.entry.remarks,"SOTA_MY:"))!=NULL) { // my park information in SOTA activation
	  char tmpbuf1[100];
	  strcpy(tmpbuf1,p1+8);
	  p1=strtok(tmpbuf1," ");
	  if (p1!=NULL) {
	    if (!state.park.equals(p1)) {
	      // match
	      continue;
	    }
	  } else {
	    continue;
	  }
	} else {
	  continue;
	}
      }
      
      if (state.type==1) {
	// txt
	sprint_qso_entry(state.dumpbuf,&state.qso);
      } else if (state.type==2) {
	// adif
	sprint_qso_entry_adif(state.dumpbuf,&state.qso);
      } else if (state.type==3) {
	// hamlogcsv
	sprint_qso_entry_hamlogcsv(state.dumpbuf,&state.qso);	
      } else if (state.type==4) {
	// jarllog
	sprint_qso_entry(state.dumpbuf,&state.qso);	
      } else {
	// error
	webLog.print("errortic state.type=");webLog.println(state.type);
	state.finished = true;
	break;
      }
    }
    String line;
    line = String(state.dumpbuf) ;

    /*    size_t lineLen = line.length();
    if (bytesWritten + lineLen > maxLen) {
      // 次回にまわす
      state.pos -= RECORD_SIZE;  // 読み戻す
      break;
    }
    */
    
    size_t lineLen = line.length();
    if (bytesWritten+lineLen <= maxLen) {
      memcpy(buffer + bytesWritten, line.c_str(), lineLen);
      bytesWritten += lineLen;
      webLog.print("bytesWritten:");webLog.print(bytesWritten);
      webLog.print(" pos:");webLog.println(state.pos);    
    } else {
      // 5. 今回は出力できない → キャッシュに入れて、最小限の出力
      state.pendingLine = line;
      memcpy(buffer+bytesWritten, "\n", 1);  // ダミーでも返す
      webLog.println("pending line  +cache + dummy write");      
      return bytesWritten+1;
    }
  }

  // 終了処理
  if (state.finished) {
    webLog.println("state.finished reached");
    String footer = "";
    if (state.type!=2 && state.type!=3) { // not adif
      footer += "---- end of file ";
      footer += state.isCurrent ? "(current)" : state.fname;
      footer += " -----\n";
    }
    if (state.type==4) { // jarllog
      footer+="</textarea> <br /><input type=\"submit\" value=\"send to logsheetform\" /></form></body>";
    }
    size_t footerLen = std::min(maxLen - bytesWritten, (size_t)footer.length());
    memcpy(buffer + bytesWritten, footer.c_str(), footerLen);
    bytesWritten += footerLen;
    
    if (!state.isCurrent) state.file.close();
  } else {
    if (bytesWritten==0) {
      // dummy write here
      webLog.println("dummy write");
      memcpy(buffer, "\n", 1);
      return 1;
    }
  }
  webLog.print("returning bytesWritten:");webLog.println(bytesWritten);
  return bytesWritten;
}

void handleQsoLogDump(AsyncWebServerRequest* request, const String& numstr, int type) {
    struct QsoDumpState state;
    state.type = type;
    state.qso.all1[sizeof(state.qso.all)]='\0';
    state.park = request->hasParam("park") ? request->getParam("park")->value() : "";
    state.summit = request->hasParam("summit") ? request->getParam("summit")->value() : "";    
    
    if (numstr.isEmpty()) {
      // 現在ログ (共通ファイル)
      state.file = qsologf;
      state.isCurrent = true;
      state.fname = "";
    } else {
      state.fname = "/qsobak." + numstr;
      if (!SD.exists(state.fname)) {
	request->send(404, "text/plain", "Log file not found.");
	return;
      }
      state.file = SD.open(state.fname, "r");
    }

    if (!state.file) {
      request->send(500, "text/plain", "Failed to open log file.");
      return;
    }

    AsyncWebServerResponse* response = request->beginChunkedResponse(type == 4 ? "text/html" : "text/plain",
								     [state](uint8_t* buffer, size_t maxLen, size_t index) mutable -> size_t {
								       return readQsoChunk(state, buffer, maxLen);
								     });

    response->addHeader("Server", "ESP Async Web Server");
    request->send(response);
  }



const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html lang="en">
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="UTF-8">
</head>
<body>
  <p><h1>DVPlogger Usage</h1></p>
<p><a href="/jarllog">/jarllog</a> to send log to JARL Log Maker.</p>
<p><a href="/readqso">/readqso</a> to read QSO data in text.</p>
<p><a href="/csv">/csv</a> to read QSO data in HAMLOG csv.</p>
<p><a href="/adif">/adif</a> to read QSO data in ADIF format.</p>
<p><a href="/dumpqso">/dumpqso</a> to dump backup qso data\n(*) </p>
<p>jarllog,readqso,dumpqso,csv,adif?num=QSOFILENUM(001,...) to process backup QSO files.</p>
<p>?park=PARK# でPARK#からQRVしたログ(Remarks にPOTA_MY:PARK#)のみ出力します。</p>
<p>?summit=SUMMIT# でSOTA SUMMIT#からQRVしたログ(Remarks にSOTA_MY:SUMMIT#)のみ出力します。</p>
<p><a href="/potahelp">/potahelp</a> POTA 最寄り検索/ログ簡単アップロード</p>
<p><a href="/sotahelp">/sotahelp</a> SOTA 最寄り検索/ログ簡単アップロード</p>
<p><a href="/settings">/settings</a> View/Edit Logger Settings</p>
<p><a href="/contests">/contests</a> Select Contest</p>
<p><a href="/rigs">/rigs</a> View/Edit RIG Settings</p>
<p><a href="/bandmap">/bandmap</a> Multi-band Bandmap</p>
<p><a href="https://github.com/JK1DVP/dvplogger/blob/main/DVPlogger_manual_260718.pdf">Manual DVPlogger_manual_2560718.pdf</a></p>
<p><a href="/op">/op</a> Web Opeartion Window</p>

  <p><h1>File Upload</h1></p>
  <p>Free Storage: %FREESPIFFS% | Used Storage: %USEDSPIFFS% | Total Storage: %TOTALSPIFFS%</p>
  <form method="POST" action="/upload" enctype="multipart/form-data"><input type="file" name="data"/><input type="submit" name="upload" value="Upload" title="Upload File"></form>
<p>パーシャルチェックのファイルはname.pck (8.3形式)でアップロードしてください。</p>
<p>CALLHISTnameとコマンドを入力すると、name.pckを読み込みます。</p>
  <p>ファイルアップロードの開始終了は表示されませんので、ファイルリスト更新までお待ちください。</p>
<p>After clicking upload it will take some time for the file to firstly upload and then be written to SPIFFS, there is no indicator that the upload began.  Please be patient.</p>
  <p>Once uploaded the page will refresh and the newly uploaded file will appear in the file list.</p>
  <p>If a file does not appear, it will be because the file was too big, or had unusual characters in the file name (like spaces).</p>
  <p>You can see the progress of the upload by watching the serial output.</p>
  <p>%FILELIST%</p>
</body>
</html>
)rawliteral";



static String selectedParkCode  = "";
static String selectedParkName  = "";
static String selectedGrid      = ""; // pota variables

static String selSotaCode="", selSotaName="", selGrid=""; // sota variables

// replace target char * for the given *param_name with *param_value , delimiter *delim (allow single delimiter)
void replace_string(char *target,const char *param_name, const char *param_value,const char *delim)
{
  char *p;char tmpbuf[200];int replaced=0;
  *tmpbuf='\0';
  p=strtok(target,delim);  
  while (p!=NULL) {
    // check if start with param_name
    if (strncmp(p,param_name,strlen(param_name))==0) {
      // this arg start with param_name
      if (!replaced) {
	strcat(tmpbuf,param_name);
	strcat(tmpbuf,param_value);
	strcat(tmpbuf,delim);
	replaced=1;
      }
    } else {
      // other param
      strcat(tmpbuf,p);
      strcat(tmpbuf,delim);
    }
    // next arg
    p=strtok(NULL,delim);
  }
  if (!replaced) {
    // add param and value
    strcat(tmpbuf,param_name);
    strcat(tmpbuf,param_value);
  }
  // copy back to the target
  strcpy(target,tmpbuf);
}



// HIDキーコードに対応するマッピング（JavaScriptのキーコードをUSB HIDキーコードに変換）
const std::map<int, int> keycodeToHid = {
  {8, 0x2A},   // Backspace
  {9, 0x2B},   // Tab
  {13, 0x28},  // Enter
  {27, 0x29},  // Escape
  {32, 0x2C},  // Space
  {65, 0x04},  // 'A'
  {66, 0x05},  // 'B'
  {67, 0x06},  // 'C'
  {68, 0x07},  // 'D'
  {69, 0x08},  // 'E'
  {70, 0x09},  // 'F'
  {71, 0x0A},  // 'G'
  {72, 0x0B},  // 'H'
  {73, 0x0C},  // 'I'
  {74, 0x0D},  // 'J'
  {75, 0x0E},  // 'K'
  {76, 0x0F},  // 'L'
  {77, 0x10},  // 'M'
  {78, 0x11},  // 'N'
  {79, 0x12},  // 'O'
  {80, 0x13},  // 'P'
  {81, 0x14},  // 'Q'
  {82, 0x15},  // 'R'
  {83, 0x16},  // 'S'
  {84, 0x17},  // 'T'
  {85, 0x18},  // 'U'
  {86, 0x19},  // 'V'
  {87, 0x1A},  // 'W'
  {88, 0x1B},  // 'X'
  {89, 0x1C},  // 'Y'
  {90, 0x1D},  // 'Z'
  {48, 0x27},  // '0'
  {49, 0x1E},  // '1'
  {50, 0x1F},  // '2'
  {51, 0x20},  // '3'
  {52, 0x21},  // '4'
  {53, 0x22},  // '5'
  {54, 0x23},  // '6'
  {55, 0x24},  // '7'
  {56, 0x25},  // '8'
  {57, 0x26},  // '9'
  {189, 0x2D}, // '-' (minus)
  {187, 0x3D}, // '=' (equals)
  {192, 0x35}, // '`' (backtick)
  {219, 0x2F}, // '[' (left bracket)
  {221, 0x30}, // ']' (right bracket)
  {220, 0x31}, // '\' (backslash)
  {188, 0x36}, // ',' (comma)
  {190, 0x37}, // '.' (period)
  {191, 0x38}, // '/' (slash)
  {38, 0x52},  // Arrow Up
  {40, 0x51},  // Arrow Down
  {37, 0x50},  // Arrow Left
  {39, 0x4F},  // Arrow Right
};


const char oppage_html[] PROGMEM =R"rawliteral(
<!DOCTYPE html>
<!-- saved from url=(0021)http://192.168.1.2/op -->
<html lang="en"><head><meta http-equiv="Content-Type" content="text/html; charset=UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  
 <style>
  div {
      margin-bottom: 3px; /* div要素の下に10pxの隙間を追加 */
      padding: 5px;
      background-color: #ffffff;
      border: 1px solid #ccc;
    }
.button-container {
  width: 100vw;              /* 画面の横幅にフィット */
  display: flex;             /* 横並びにする */
  flex-wrap: wrap;           /* 折り返しを許可 */
  gap: 8px;                  /* ボタン間のすき間（任意） */
  padding: 8px;              /* 画面端との余白（任意） */
  box-sizing: border-box;    /* paddingも含めて100vwとする */
}

.button-container button {
  flex: 0 1 auto;            /* 自然な大きさ、必要に応じて縮む */
  min-width: 80px;           /* 必要に応じて指定（任意） */
  font-size: 16px;           /* スマホでも押しやすく */
  background-color: #a0a0a0;
  color: black;
}

    button {
      padding: 10px 20px;
      font-size: 16px;
      background-color: #a0a0a0;
      color: black;
      border: none;
      border-radius: 4px;
      cursor: pointer;
    }

 .form-container {
  width: 100vw;              /* 画面の横幅にフィット */
  display: flex;             /* 横並びにする */
  flex-wrap: wrap;           /* 折り返しを許可 */
  gap: 8px;                  /* ボタン間のすき間（任意） */
  padding: 8px;              /* 画面端との余白（任意） */
  box-sizing: border-box;    /* paddingも含めて100vwとする */
    }

    label {
      font-size: 14px;
      font-weight: bold;
    }

    input#edit_2 {
      padding: 10px;
      font-size: 14px;
      border: 1px solid #ccc;
      border-radius: 4px;
      width: 50px;
    }


    input#edit_0 {
      padding: 10px;
      font-size: 16px;
      border: 1px solid #ccc;
      border-radius: 4px;
      width: 200px;
    }


    input#edit_1 {
      padding: 10px;
      font-size: 16px;
      border: 1px solid #ccc;
      border-radius: 4px;
      width: 200px;
    }

    button {
      padding: 10px 20px;
      font-size: 16px;
      background-color: #a0a0a0;
      color: black;
      border: none;
      border-radius: 4px;
      cursor: pointer;
    }

    button:hover {
      background-color: #a0a0a0;
    }
  </style>
</head>

<body>
  <h3>DVPlogger Operation</h3>

  <form id="settingsForm" onsubmit="return false;">
    
  <div class="button-container">
 <label "="">Radio:</label>
    <button id="b_radio_0" type="button" onclick="selectRadio(0)" style="background-color: green;">0</button>
   <input type="text" id="edit_10" data-index="10" size="6"> <!-- radio_name 0 -->
    <button id="b_radio_1" type="button" onclick="selectRadio(1)" style="background-color: gray;">1</button>
   <input type="text" id="edit_11" data-index="11" size="6"> <!-- radio_name 1 -->
    <button id="b_radio_2" type="button" onclick="selectRadio(2)" style="background-color: gray;">2</button>
   <input type="text" id="edit_12" data-index="12" size="6"> <!-- radio_name 2 -->

</div>
  <div class="button-container">
 <label "="">Mode:</label>
    <button id="b_mode_CW" type="button" onclick="selectMode(&#39;CW&#39;)" style="background-color: gray;">CW</button>
    <button id="b_mode_USB" type="button" onclick="selectMode(&#39;USB&#39;)" style="background-color: green;">USB</button>
    <button id="b_mode_LSB" type="button" onclick="selectMode(&#39;LSB&#39;)" style="background-color: gray;">LSB</button>
    <button id="b_mode_FM" type="button" onclick="selectMode(&#39;FM&#39;)" style="background-color: gray;">FM</button>
</div>
  <div class="button-container">
 <label "="">Band:</label>
    <button id="b_band_1" type="button" onclick="selectBand(1)" style="background-color: green;">1.9</button>
    <button id="b_band_2" type="button" onclick="selectBand(2)" style="background-color: gray;">3.5</button>
    <button id="b_band_3" type="button" onclick="selectBand(3)" style="background-color: gray;">7</button>
    <button id="b_band_4" type="button" onclick="selectBand(4)" style="background-color: gray;">14</button>
    <button id="b_band_5" type="button" onclick="selectBand(5)" style="background-color: gray;">21</button>
    <button id="b_band_6" type="button" onclick="selectBand(6)" style="background-color: gray;">28</button>
    <button id="b_band_7" type="button" onclick="selectBand(7)" style="background-color: gray;">50</button>
    <button id="b_band_8" type="button" onclick="selectBand(8)" style="background-color: gray;">144</button>
    <button id="b_band_9" type="button" onclick="selectBand(9)" style="background-color: gray;">430</button>
    <button id="b_band_10" type="button" onclick="selectBand(10)" style="background-color: gray;">1200</button>
</div>

<div id="radioDisplay">10:08:07       Radio:0 S&amp;P Freq:   1.801.00 Hz Mode: USB</div> <!-- Radio 状態を表示する部分 -->

<div class="button-container">
    <button type="button" onclick="sendKeyCode(27)">ESC</button>
    <button type="button" onclick="sendKeyCode(112)">F1 CQ</button> <!-- F1キー -->
    <button type="button" onclick="sendKeyCode(113)">F2 Exch</button> <!-- F2キー -->
    <button type="button" onclick="sendKeyCode(114)">F3 TU </button> <!-- F3キー -->
    <button type="button" onclick="sendKeyCode(115)">F4 MyCALL</button> <!-- F4キー -->
    <button type="button" onclick="sendKeyCode(116)">F5 Call&amp;Exch</button> <!-- F5キー -->
</div>

<div class="form-container">
   <label for="edit_0">Call:</label>
   <input type="text" id="edit_0" data-index="0" size="11">
   <button type="button" onclick="sendEnter(0)"> ⏎ </button>
</div>
<div class="form-container">
   <label for="edit_1">Recv:</label>
   <input type="text" id="edit_2" data-index="2" size="3"> <!-- received rst -->
   <input type="text" id="edit_1" data-index="1" size="11"> <!-- received exch -->
   <button type="button" onclick="sendEnter(1)"> ⏎ </button>
</div>
<div class="form-container">
   <label for="edit_5">MyCall:</label>
   <input type="text" id="edit_5" data-index="5" size="11"> <!-- sent rst -->
   <label for="edit_4">Sent:</label>
   <input type="text" id="edit_3" data-index="3" size="3"> <!-- sent rst -->
   <input type="text" id="edit_4" data-index="4" size="11"> <!-- sent number -->
</div>
<div class="form-container">
        <label>Contest:</label>
   <input type="text" id="edit_13" data-index="13" size="20"> <!-- contest_name 0 -->
    </div>
 <div class="form-container" id="cwkeyingDisplay"></div> <!-- CW keying ticker display -->
    <p>Band map test</p>
    <p><a href="/">go back to Home</a></p>

  </form>

<script>
function selectRadio(name) {
  fetch(`/control?type=Radio&value=${name}`)
}
function selectMode(name) {
  fetch(`/control?type=Mode&value=${name}`)
}
function selectBand(name) {
  fetch(`/control?type=Band&value=${name}`)
}

// `fetchMultipleIndexes` 関数での処理を非同期で実行
async function fetchMultipleIndexes() {
  const indices = [0, 1, 2, 3, 4, 5, 10,11,12,13 ];  // 任意のindexの配列
  const overwrite = [10,11,12]; // indices to overwrite regardless of the present value
  for (let i = 0; i < indices.length; i++) {
    const index = indices[i];
    try {
      const response = await fetch(`/radio_status?index=${index}`);
      const data = await response.text();
      console.log(`Response for index ${index}:`, data);

      // 取得したデータをinputに設定
      const inputElement = document.getElementById(`edit_${index}`);

      // inputのvalueが空の場合のみ更新
      //if (inputElement && ((inputElement.value === '')||(overwrite.includes(${index}))))) {
      if (inputElement && inputElement.value === '') {
        inputElement.value = data;
      }
    } catch (error) {
      console.error(`Error fetching for index ${index}:`, error);
    }
  }
}

// 1秒ごとに繰り返し処理を実行する
async function repeatTask() {
  let taskNumber = 1;

  while (true) {
    try {
      await fetchMultipleIndexes();
      await fetchStatus(99, 'radioDisplay');
      await fetchStatus(98, 'cwkeyingDisplay');
      await fetchStatus_button(7, 'b_radio');
      await fetchStatus_button(8, 'b_mode');
      await fetchStatus_button(9, 'b_band');
      
      // 処理完了後に1秒待機
      await new Promise(resolve => setTimeout(resolve, 1000));
      taskNumber++;
    } catch (error) {
      console.error('Error in task execution:', error);
      break;
    }
  }
}

// /radio_statusから状態を取得してHTMLに反映
async function fetchStatus(index, displayId) {
  try {
    const response = await fetch(`/radio_status?index=${index}`);
    const data = await response.text();
    document.getElementById(displayId).innerText = data;
  } catch (error) {
    console.error(`Error fetching server status for index ${index}:`, error);
  }
}

// /radio_statusから状態を取得してbuttonに反映
async function fetchStatus_button(index, buttonId) {
  try {
    const response = await fetch(`/radio_status?index=${index}`);
    const data = await response.text();
    const buttonId_sel = `${buttonId}_${data}`
    const button = document.getElementById(buttonId_sel);
    //const elements = document.querySelectorAll('[id^=${buttonId}]');
    const elements = document.querySelectorAll(`[id^="${buttonId}"]`);

    if (button) {
      elements.forEach(el => {
        if (el.id === button.id) {
          el.style.backgroundColor = "green";
        } else {
          el.style.backgroundColor = "gray";
        }
      });
    }


  } catch (error) {
    console.error(`Error fetching server status for index ${index}:`, error);
  }
}



// 初期化
repeatTask();

// F1〜F5ボタンを押したときにキーコードを送信
function sendKeyCode(keyCode) {
  console.log(`Sending key code: ${keyCode}`);  // デバッグ用ログ
  fetch(`/rig_key?keycode=${keyCode}`)
    .then(response => response.text())
    .then(data => console.log("Sent key code:", keyCode, "Response:", data))
    .catch(error => console.error("Error sending keycode:", error));
}

// フォーカス移動と入力内容送信
function sendEnter(inputIndex) {

  // inputIndex = 0  Call 1 Exch 6 Radio0 name  7 Radio1 name 8 Radio2 name
  console.log('sendEnter called',inputIndex);
  if (inputIndex == 0 || inputIndex == 1 ) {
    // 入力内容を送信
    const input1 = document.getElementById('edit_0').value;
    const input2 = document.getElementById('edit_1').value;

    fetch(`/rig_key?keycode=13&input0=${encodeURIComponent(input1)}&input1=${encodeURIComponent(input2)}&index=${inputIndex}`)
      .then(res => res.text())
      .then(msg => console.log('→ rig_key response:', msg))
      .catch(err => console.error('fetch error:', err));
  } else {
    // set remote variable in general
    const value = document.getElementById(`edit_${inputIndex}`).value;
    fetch(`/rig_key?command=set&index=${inputIndex}&value=${encodeURIComponent(value)}`)
      .then(res => res.text())
      .then(msg => console.log('→ rig_key response to radio setting:', msg))
      .catch(err => console.error('fetch error:', err));
    // to let remote server fill the setting value into edit window clear edit_inputIndex
    document.getElementById(`edit_${inputIndex}`).value = '';
  }

  // フォーカスを次のinputに移動（ループ）
  if (inputIndex === 0) {  // callsign
    document.getElementById('edit_1').focus();
  } else if (inputIndex === 1) {  // recv exch
    document.getElementById('edit_0').focus();
    clearInputs();  // 入力内容をクリア
  }
}

function clearInputs() {
  document.getElementById('edit_0').value = '';  // callsignをクリア
  document.getElementById('edit_1').value = '';  // recv exchをクリア
  document.getElementById('edit_2').value = '';  // recv rstをクリア
}

// keydownイベントの監視
document.addEventListener("DOMContentLoaded", () => {
  document.addEventListener("keydown", event => {
    const key = event.key;
    const code = event.keyCode || event.which;
    const focused = document.activeElement;
    const idx = focused && focused.dataset ? focused.dataset.index || "" : "";

    console.log('keydown idx=', idx, 'key=', key);

    // Shiftキーが押されたとき
    if (key === "Shift") {
      handleShiftKey(event);
    }

    // TabキーまたはSpaceキーが押された場合にデフォルト動作を防ぐ
    if (key === "Tab" || key === " ") {
      event.preventDefault();  // フォーカス移動を防止
      if (idx === "0") {
        document.getElementById('edit_1').focus();
      } else if (idx === "1") {
        document.getElementById('edit_0').focus();
      }
    }

    // Enterキーが押された場合、両方のinput欄の内容を送信
    if (key === "Enter") {
      event.preventDefault();  // フォーム送信を防ぐ

      sendEnter(idx);

      // フォーカス移動
      if (idx === "1") {
        document.getElementById('edit_0').focus();
      } else if (idx === "0") {
        document.getElementById('edit_1').focus();
      }
    }

    // fetchでキー送信（Space、Tabも含む）
    fetch(`/rig_key?keycode=${code}${idx !== "" ? "&index=" + idx : ""}`)
      .then(res => res.text())
      .then(msg => console.log('→ rig_key response:', msg))
      .catch(err => console.error('fetch error:', err));
  });
});

function handleShiftKey(event) {
  if (event.location === 0 && !shiftLeftPressed) {  // 左Shift
    shiftLeftPressed = true;
    fetch(`/rig_key?shiftState=pressed&shiftKey=left`);
  } else if (event.location === 1 && !shiftRightPressed) {  // 右Shift
    shiftRightPressed = true;
    fetch(`/rig_key?shiftState=pressed&shiftKey=right`);
  }
}

</script>
</body></html>
)rawliteral";

static void web_heap_point(const char *tag)
{
  webLog.printf("[MEM] %-22s internal=%u largest=%u min=%u psram=%u\n",
                  tag,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}



namespace {
constexpr int WEB_BANDMAP_BANDS = N_BAND - 1;
constexpr int WEB_BANDMAP_MAX_ENTRIES = 200;
constexpr int WEB_BANDMAP_COLUMNS = 4;
constexpr uint32_t WEB_BANDMAP_REFRESH_MS = 1000;
constexpr uint8_t WEB_BANDMAP_COMMAND_QUEUE_LEN = 8;

struct WebBandmapEntry {
  uint32_t freq;
  int32_t time;
  char station[LEN_CALLSIGN + 1];
  uint8_t mode;
  uint8_t flag;
};

struct WebBandmapSnapshot {
  uint16_t count[WEB_BANDMAP_BANDS];
  WebBandmapEntry entry[WEB_BANDMAP_BANDS][WEB_BANDMAP_MAX_ENTRIES];
  uint32_t band_generation[WEB_BANDMAP_BANDS];
  uint32_t generation;
  uint8_t sort_type;
};

enum WebBandmapCommandType : uint8_t {
  WEB_BANDMAP_COMMAND_SELECT = 0,
  WEB_BANDMAP_COMMAND_DELETE,
  WEB_BANDMAP_COMMAND_CORRECT
};

struct WebBandmapCommand {
  WebBandmapCommandType type;
  uint8_t bandid;
  uint8_t mode;
  uint32_t freq;
  int32_t time;
  char station[LEN_CALLSIGN + 1];
  char new_station[LEN_CALLSIGN + 1];
};

static WebBandmapSnapshot *web_bandmap_snapshots[2] = {nullptr, nullptr};
static volatile uint8_t web_bandmap_active_snapshot = 0;
static volatile uint32_t web_bandmap_published_generation = 0;
static volatile bool web_bandmap_snapshot_ready = false;
static SemaphoreHandle_t web_bandmap_snapshot_mutex = nullptr;
static uint32_t web_bandmap_next_refresh_ms = 0;

static WebBandmapCommand web_bandmap_command_queue[WEB_BANDMAP_COMMAND_QUEUE_LEN];
static volatile uint8_t web_bandmap_command_head = 0;
static volatile uint8_t web_bandmap_command_tail = 0;
static portMUX_TYPE web_bandmap_command_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t web_bandmap_hash_mix(uint32_t hash, uint32_t value) {
  hash ^= value;
  hash *= 16777619UL;
  return hash;
}

static bool web_bandmap_entry_less(const WebBandmapEntry &a,
                                   const WebBandmapEntry &b,
                                   uint8_t sort_type) {
  if (sort_type == 1) {
    if (a.freq != b.freq) return a.freq < b.freq;
    return a.time > b.time;
  }

  const int32_t dt = b.time - a.time;
  if (abs(dt) < 60) {
    const bool a_multi = (a.flag & BANDMAP_ENTRY_FLAG_NEWMULTI) != 0;
    const bool b_multi = (b.flag & BANDMAP_ENTRY_FLAG_NEWMULTI) != 0;
    if (a_multi != b_multi) return a_multi;
  }
  if (a.time != b.time) return a.time > b.time;
  return a.freq < b.freq;
}

static bool ensure_web_bandmap_snapshots() {
  if (!web_bandmap_snapshot_mutex) {
    web_bandmap_snapshot_mutex = xSemaphoreCreateMutex();
    if (!web_bandmap_snapshot_mutex) return false;
  }
  if (web_bandmap_snapshots[0] && web_bandmap_snapshots[1]) return true;

  for (int i = 0; i < 2; ++i) {
    if (!web_bandmap_snapshots[i]) {
      web_bandmap_snapshots[i] = static_cast<WebBandmapSnapshot *>(
        heap_caps_calloc(1, sizeof(WebBandmapSnapshot),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
  }

  if (!web_bandmap_snapshots[0] || !web_bandmap_snapshots[1]) {
    webLog.println("bandmap: cannot allocate PSRAM snapshots");
    return false;
  }
  return true;
}

static void rebuild_web_bandmap_snapshot() {
  if (!ensure_web_bandmap_snapshots()) return;

  // Build the inactive snapshot without holding the mutex.  Web handlers only
  // read the active snapshot, so the lock is needed only for the final swap.
  const uint8_t active = web_bandmap_active_snapshot;
  const uint8_t next = active ^ 1U;
  WebBandmapSnapshot *snapshot = web_bandmap_snapshots[next];
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->sort_type = bandmap_disp.sort_type;

  uint32_t hash = 2166136261UL;
  hash = web_bandmap_hash_mix(hash, snapshot->sort_type);

  for (int band_index = 0; band_index < WEB_BANDMAP_BANDS; ++band_index) {
    const int bandid = band_index + 1;
    uint16_t count = 0;

    for (int i = 0;
         i < bandmap[band_index].nentry && count < WEB_BANDMAP_MAX_ENTRIES;
         ++i) {
      struct bandmap_entry *source = bandmap[band_index].entry + i;
      if (source->station[0] == '\0') continue;
      if (source->mode >= NMODEID) continue;

      const int mode_type = modetype[source->mode];
      if (dupe_check_nocallhist(source->station,
                               bandmode_param(bandid, mode_type),
                               plogw->mask)) {
        source->flag |= BANDMAP_ENTRY_FLAG_WORKED;
        continue;
      }
      source->flag &= ~BANDMAP_ENTRY_FLAG_WORKED;

      WebBandmapEntry &dest = snapshot->entry[band_index][count++];
      dest.freq = source->freq;
      dest.time = source->time;
      strlcpy(dest.station, source->station, sizeof(dest.station));
      dest.mode = source->mode;
      dest.flag = source->flag;
    }

    snapshot->count[band_index] = count;
    std::sort(snapshot->entry[band_index],
              snapshot->entry[band_index] + count,
              [snapshot](const WebBandmapEntry &a, const WebBandmapEntry &b) {
                return web_bandmap_entry_less(a, b, snapshot->sort_type);
              });

    uint32_t band_hash = 2166136261UL;
    band_hash = web_bandmap_hash_mix(band_hash, snapshot->sort_type);
    band_hash = web_bandmap_hash_mix(band_hash, bandid);
    band_hash = web_bandmap_hash_mix(band_hash, count);
    for (uint16_t i = 0; i < count; ++i) {
      const WebBandmapEntry &entry = snapshot->entry[band_index][i];
      band_hash = web_bandmap_hash_mix(band_hash, entry.freq);
      band_hash = web_bandmap_hash_mix(band_hash, static_cast<uint32_t>(entry.time));
      band_hash = web_bandmap_hash_mix(band_hash, entry.mode);
      band_hash = web_bandmap_hash_mix(band_hash, entry.flag);
      for (const char *p = entry.station; *p; ++p) {
        band_hash = web_bandmap_hash_mix(
          band_hash, static_cast<uint8_t>(*p));
      }
    }
    snapshot->band_generation[band_index] = band_hash;
    hash = web_bandmap_hash_mix(hash, band_hash);
  }

  snapshot->generation = hash;

  // Publish the completed snapshot atomically.  This critical section is very
  // short, so /version and /data no longer contend with worked checks/sorting.
  if (xSemaphoreTake(web_bandmap_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
    web_bandmap_active_snapshot = next;
    web_bandmap_published_generation = hash;
    web_bandmap_snapshot_ready = true;
    xSemaphoreGive(web_bandmap_snapshot_mutex);
  }
}

static bool enqueue_web_bandmap_command(const WebBandmapCommand &command) {
  bool queued = false;
  portENTER_CRITICAL(&web_bandmap_command_mux);
  const uint8_t next = static_cast<uint8_t>(
    (web_bandmap_command_head + 1) % WEB_BANDMAP_COMMAND_QUEUE_LEN);
  if (next != web_bandmap_command_tail) {
    web_bandmap_command_queue[web_bandmap_command_head] = command;
    web_bandmap_command_head = next;
    queued = true;
  }
  portEXIT_CRITICAL(&web_bandmap_command_mux);
  return queued;
}

static bool dequeue_web_bandmap_command(WebBandmapCommand *command) {
  bool available = false;
  portENTER_CRITICAL(&web_bandmap_command_mux);
  if (web_bandmap_command_tail != web_bandmap_command_head) {
    *command = web_bandmap_command_queue[web_bandmap_command_tail];
    web_bandmap_command_tail = static_cast<uint8_t>(
      (web_bandmap_command_tail + 1) % WEB_BANDMAP_COMMAND_QUEUE_LEN);
    available = true;
  }
  portEXIT_CRITICAL(&web_bandmap_command_mux);
  return available;
}

static bool valid_web_bandmap_callsign(const char *station) {
  if (!station || !station[0]) return false;
  const size_t length = strlen(station);
  if (length > LEN_CALLSIGN) return false;
  bool has_alnum = false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(station[i]);
    if (isalnum(c)) has_alnum = true;
    else if (c != '/') return false;
  }
  return has_alnum;
}

static void update_web_bandmap_entry_flags(struct bandmap_entry *entry,
                                           uint8_t bandid) {
  if (!entry || entry->mode >= NMODEID) return;
  entry->flag &= ~(BANDMAP_ENTRY_FLAG_WORKED | BANDMAP_ENTRY_FLAG_NEWMULTI);

  char *exch_history = nullptr;
  const int bandmode = bandmode_param(bandid, modetype[entry->mode]);
  if (dupe_callhist_check(entry->station, bandmode, plogw->mask, 1,
                          &exch_history)) {
    entry->flag |= BANDMAP_ENTRY_FLAG_WORKED;
  }
  if (exch_history) {
    const int multi = multi_check(exch_history, bandid);
    if (multi >= 0 && multi_list.multi_worked[bandid - 1][multi] == 0) {
      entry->flag |= BANDMAP_ENTRY_FLAG_NEWMULTI;
    }
  }
}

static struct bandmap_entry *find_web_bandmap_entry(
    const WebBandmapCommand &command) {
  if (command.bandid < 1 || command.bandid >= N_BAND) return nullptr;
  const int band_index = command.bandid - 1;
  for (int i = 0; i < bandmap[band_index].nentry; ++i) {
    struct bandmap_entry *entry = bandmap[band_index].entry + i;
    if (entry->station[0] == '\0') continue;
    if (entry->freq == command.freq &&
        entry->mode == command.mode &&
        entry->time == command.time &&
        strcasecmp(entry->station, command.station) == 0) {
      return entry;
    }
  }
  return nullptr;
}

static void process_web_bandmap_command_queue() {
  WebBandmapCommand command;
  while (dequeue_web_bandmap_command(&command)) {
    struct bandmap_entry *entry = find_web_bandmap_entry(command);
    if (!entry) {
      webLog.printf("bandmap: requested spot no longer exists: %s\n",
                    command.station);
      continue;
    }

    if (command.type == WEB_BANDMAP_COMMAND_DELETE) {
      char old_station[LEN_CALLSIGN + 1];
      strlcpy(old_station, entry->station, sizeof(old_station));
      entry->station[0] = '\0';
      webLog.printf("bandmap: deleted %s %lu\n", old_station,
                    static_cast<unsigned long>(entry->freq));
      web_bandmap_next_refresh_ms = 0;
      continue;
    }

    if (command.type == WEB_BANDMAP_COMMAND_CORRECT) {
      if (!valid_web_bandmap_callsign(command.new_station)) {
        webLog.println("bandmap: invalid corrected callsign");
        continue;
      }
      char old_station[LEN_CALLSIGN + 1];
      strlcpy(old_station, entry->station, sizeof(old_station));
      strlcpy(entry->station, command.new_station, sizeof(entry->station));
      update_web_bandmap_entry_flags(entry, command.bandid);
      webLog.printf("bandmap: corrected %s -> %s\n", old_station,
                    entry->station);
      web_bandmap_next_refresh_ms = 0;
      continue;
    }

    update_web_bandmap_entry_flags(entry, command.bandid);
    if (entry->flag & BANDMAP_ENTRY_FLAG_WORKED) {
      webLog.printf("bandmap: %s is already worked\n", entry->station);
      web_bandmap_next_refresh_ms = 0;
      continue;
    }
    if (!select_appropriate_radio(command.bandid)) {
      webLog.printf("bandmap: no appropriate radio for band %u\n",
                    command.bandid);
      continue;
    }

    struct radio *radio = so2r.radio_selected();
    set_station_entry(radio, entry->station, entry->freq,
                      mode_str[entry->mode]);
    webLog.printf("bandmap: selected %s %lu\n", entry->station,
                  static_cast<unsigned long>(entry->freq));
  }
}

static const char web_bandmap_page[] PROGMEM = R"rawliteral(
<!doctype html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVPlogger Bandmap</title>
<style>
body{font-family:sans-serif;margin:10px;background:#f4f4f4;color:#111}
h1{font-size:1.35rem;margin:.3rem 0 .8rem}.toolbar{display:flex;gap:.5rem;flex-wrap:wrap;margin-bottom:.7rem}
select,button{font-size:1rem;padding:.35rem}.maps{display:flex;gap:10px;overflow-x:auto;align-items:flex-start;padding-bottom:8px}
.band{flex:0 0 285px;background:#fff;border:1px solid #bbb;border-radius:5px;max-height:78vh;overflow-y:auto}
.band h2{position:sticky;top:0;background:#e8e8e8;margin:0;padding:.45rem;font-size:1.05rem;border-bottom:1px solid #bbb;z-index:1}
.spot{display:grid;grid-template-columns:78px 1fr 42px 34px 30px;gap:4px;padding:.28rem .4rem;border-bottom:1px solid #eee;cursor:pointer}
.spot:hover{background:#fff4c4}.spot.newmulti{background:#fff3a8;border-left:5px solid #d07800;padding-left:calc(.4rem - 5px)}
.spot.newmulti:hover{background:#ffe57a}.freq{font-family:monospace}.call{font-weight:bold}.multi{color:#a00018;font-weight:bold;text-align:center}.age{text-align:right;color:#555}.empty{padding:.8rem;color:#777}
#status{font-size:.85rem;color:#555;margin-left:.3rem}.more{border:0;background:transparent;padding:0;font-size:1.25rem;line-height:1;cursor:pointer}.menu{position:fixed;display:none;z-index:20;background:#fff;border:1px solid #999;border-radius:5px;box-shadow:0 3px 14px #5558;min-width:170px}.menu button{display:block;width:100%;border:0;background:#fff;text-align:left;padding:.65rem}.menu button:hover{background:#eee}.modal{display:none;position:fixed;inset:0;background:#0007;z-index:30;align-items:center;justify-content:center}.dialog{background:#fff;border-radius:7px;padding:1rem;min-width:min(310px,88vw);box-shadow:0 5px 20px #0008}.dialog h3{margin:.1rem 0 .8rem}.dialog input{box-sizing:border-box;width:100%;font-size:1.1rem;padding:.45rem;text-transform:uppercase}.actions{display:flex;justify-content:flex-end;gap:.6rem;margin-top:1rem}.danger{color:#a00018;font-weight:bold}
</style></head><body><h1>DVPlogger Bandmap</h1>
<div class="toolbar"><select id="b0"></select><select id="b1"></select><select id="b2"></select><select id="b3"></select>
<button id="reload">更新</button><span id="status"></span></div><div id="maps" class="maps"></div><div id="spotMenu" class="menu"><button id="menuDelete" class="danger">Delete Spot</button><button id="menuCorrect">Callsign correction</button></div><div id="spotModal" class="modal"><div class="dialog"><h3 id="modalTitle"></h3><div id="modalText"></div><input id="callInput" maxlength="12" autocomplete="off" autocapitalize="characters"><div class="actions"><button id="modalCancel">Cancel</button><button id="modalApply">Apply</button></div></div></div>
<script>
const defaults=[3,4,5,6];let bandGeneration={};let loadingBands=new Set();let menuSpot=null;let modalAction=null;
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function selected(){return [0,1,2,3].map(i=>Number(document.getElementById('b'+i).value));}
function save(){localStorage.setItem('dvploggerBandmapBands',JSON.stringify(selected()));}
function sleep(ms){return new Promise(resolve=>setTimeout(resolve,ms));}
async function fetchJson(url,retry=true){try{const r=await fetch(url,{cache:'no-store'});const text=await r.text();
if(!r.ok)throw new Error('HTTP '+r.status+': '+text.slice(0,80));
try{return JSON.parse(text);}catch(e){console.error('bandmap JSON parse error',e,'length',text.length,'tail',text.slice(-160));throw new Error('JSON不正 len='+text.length);}}
catch(e){if(retry){await sleep(300);return fetchJson(url,false);}throw e;}}
function columnsForBand(bandid){const cols=[];for(let i=0;i<4;i++)if(Number(document.getElementById('b'+i).value)===Number(bandid))cols.push(i);return cols;}
function ensureColumns(){const maps=document.getElementById('maps');while(maps.children.length<4){const col=document.createElement('section');col.className='band';col.innerHTML='<h2>Loading...</h2>';maps.appendChild(col);}}
function renderBand(b,columns){ensureColumns();for(const column of columns){const col=document.getElementById('maps').children[column];const oldScroll=col.scrollTop;
let h='<h2>'+esc(b.label)+' ('+b.spots.length+')</h2>';if(!b.spots.length)h+='<div class="empty">未交信スポットなし</div>';
for(const s of b.spots){const cls=s.multi?'spot newmulti':'spot';h+='<div class="'+cls+'" data-band="'+b.id+'" data-freq="'+s.freq+'" data-mode="'+s.mode+'" data-time="'+s.time+'" data-call="'+esc(s.call)+'">'+
'<span class="freq">'+esc(s.freqText)+'</span><span class="call">'+esc(s.call)+'</span><span class="multi">'+(s.multi?'M':'')+'</span><span class="age">'+s.age+'m</span><button class="more" aria-label="Spot menu">⋮</button></div>';}
col.innerHTML=h;col.scrollTop=oldScroll;col.querySelectorAll('.spot').forEach(e=>{e.onclick=()=>selectSpot(e);const m=e.querySelector('.more');m.onclick=ev=>{ev.stopPropagation();openSpotMenu(e,m);};});}}
async function loadBand(bandid,force=false){bandid=Number(bandid);if(loadingBands.has(bandid))return;loadingBands.add(bandid);
try{const j=await fetchJson('/api/bandmap/data?band='+bandid);if(!j.bands||!j.bands.length)throw new Error('band data missing');
bandGeneration[bandid]=Number(j.bandGeneration);renderBand(j.bands[0],columnsForBand(bandid));}
catch(e){console.error('bandmap band load failed',bandid,e);document.getElementById('status').textContent='取得失敗: '+e.message;}finally{loadingBands.delete(bandid);}}
async function loadSelected(force=false){const unique=[...new Set(selected())];document.getElementById('status').textContent='更新中…';await Promise.all(unique.map(b=>loadBand(b,force)));document.getElementById('status').textContent='更新 '+new Date().toLocaleTimeString();}
async function loadBands(){const j=await fetchJson('/api/bandmap/bands');let saved;try{saved=JSON.parse(localStorage.getItem('dvploggerBandmapBands'));}catch(e){}
const valid=new Set(j.bands.map(b=>Number(b.id)));if(!Array.isArray(saved)||saved.length!==4)saved=defaults.slice();
saved=saved.map((v,n)=>{v=Number(v);return valid.has(v)?v:(valid.has(defaults[n])?defaults[n]:Number(j.bands[0].id));});
for(let n=0;n<4;n++){const s=document.getElementById('b'+n);s.innerHTML='';for(const b of j.bands){const o=document.createElement('option');o.value=String(b.id);o.textContent=b.label;s.appendChild(o);}s.value=String(saved[n]);s.onchange=()=>{save();loadBand(Number(s.value),true);};}save();ensureColumns();}
async function selectSpot(e){const p=new URLSearchParams({band:e.dataset.band,freq:e.dataset.freq,mode:e.dataset.mode,time:e.dataset.time,call:e.dataset.call});
const r=await fetch('/api/bandmap/select',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});document.getElementById('status').textContent=await r.text();}
function spotParams(e){return {band:e.dataset.band,freq:e.dataset.freq,mode:e.dataset.mode,time:e.dataset.time,call:e.dataset.call};}
function closeSpotMenu(){document.getElementById('spotMenu').style.display='none';menuSpot=null;}
function openSpotMenu(spot,button){menuSpot=spot;const menu=document.getElementById('spotMenu');const r=button.getBoundingClientRect();menu.style.left=Math.max(6,Math.min(window.innerWidth-180,r.right-170))+'px';menu.style.top=Math.min(window.innerHeight-110,r.bottom+3)+'px';menu.style.display='block';}
function closeModal(){document.getElementById('spotModal').style.display='none';modalAction=null;}
function showDeleteDialog(){if(!menuSpot)return;const p=spotParams(menuSpot);closeSpotMenu();menuSpot={dataset:p};modalAction='delete';document.getElementById('modalTitle').textContent='Delete Spot';document.getElementById('modalText').textContent=p.call+'  '+p.freq;document.getElementById('callInput').style.display='none';document.getElementById('modalApply').textContent='Delete';document.getElementById('modalApply').className='danger';document.getElementById('spotModal').style.display='flex';}
function showCorrectDialog(){if(!menuSpot)return;const p=spotParams(menuSpot);closeSpotMenu();menuSpot={dataset:p};modalAction='correct';document.getElementById('modalTitle').textContent='Callsign correction';document.getElementById('modalText').textContent='Current: '+p.call;const input=document.getElementById('callInput');input.style.display='block';input.value=p.call;document.getElementById('modalApply').textContent='Apply';document.getElementById('modalApply').className='';document.getElementById('spotModal').style.display='flex';setTimeout(()=>{input.focus();input.select();},0);}
async function postSpotAction(path,extra={}){if(!menuSpot)return;const p=new URLSearchParams({...spotParams(menuSpot),...extra});const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});const text=await r.text();if(!r.ok)throw new Error('HTTP '+r.status+': '+text);document.getElementById('status').textContent=text;}
async function applyModal(){try{if(modalAction==='delete')await postSpotAction('/api/bandmap/delete');else if(modalAction==='correct'){const v=document.getElementById('callInput').value.trim().toUpperCase();if(!/^[A-Z0-9/]+$/.test(v))throw new Error('Invalid callsign');await postSpotAction('/api/bandmap/correct',{newcall:v});}closeModal();}catch(e){document.getElementById('status').textContent='操作失敗: '+e.message;}}
document.getElementById('menuDelete').onclick=showDeleteDialog;document.getElementById('menuCorrect').onclick=showCorrectDialog;document.getElementById('modalCancel').onclick=closeModal;document.getElementById('modalApply').onclick=applyModal;document.getElementById('spotModal').onclick=e=>{if(e.target.id==='spotModal')closeModal();};document.addEventListener('click',e=>{if(!e.target.closest('#spotMenu')&&!e.target.closest('.more'))closeSpotMenu();});
async function checkVersion(){try{const j=await fetchJson('/api/bandmap/version',false);if(!j.ready)return;for(const b of [...new Set(selected())]){const next=Number(j.bands[String(b)]);if(Number.isFinite(next)&&bandGeneration[b]!==undefined&&next!==bandGeneration[b])loadBand(b);}}catch(e){console.debug('bandmap version check failed',e);}}
document.getElementById('reload').onclick=()=>loadSelected(true);(async()=>{await loadBands();await loadSelected(true);setInterval(checkVersion,1000);})();
</script></body></html>)rawliteral";

static constexpr uint16_t WEB_BANDMAP_MAX_DISPLAY_ENTRIES = 20;

struct WebBandmapApiState {
  struct BandData {
    uint8_t bandid;
    uint16_t count;
    WebBandmapEntry *entries;
  } band[WEB_BANDMAP_COLUMNS]{};
  uint8_t band_count = 0;
  uint8_t stage = 0;
  uint8_t band_index = 0;
  uint16_t entry_index = 0;
  size_t offset = 0;
  uint32_t generation = 0;
  char line[256];

  ~WebBandmapApiState() {
    for (int i = 0; i < WEB_BANDMAP_COLUMNS; ++i) free(band[i].entries);
  }
};

static uint8_t parse_web_bandmap_band(AsyncWebServerRequest *request) {
  int bandid = 3;
  if (request->hasParam("band")) {
    bandid = request->getParam("band")->value().toInt();
  }
  if (bandid < 1 || bandid >= N_BAND) bandid = 3;
  return static_cast<uint8_t>(bandid);
}

static WebBandmapApiState *make_web_bandmap_api_state(uint8_t bandid) {
  if (!ensure_web_bandmap_snapshots()) return nullptr;
  WebBandmapApiState *state = new (std::nothrow) WebBandmapApiState;
  if (!state) return nullptr;

  if (!web_bandmap_snapshot_ready ||
      xSemaphoreTake(web_bandmap_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
    delete state;
    return nullptr;
  }
  const uint8_t active = web_bandmap_active_snapshot;
  WebBandmapSnapshot *snapshot = web_bandmap_snapshots[active];
  state->generation = snapshot->band_generation[bandid - 1];
  const uint16_t count = std::min<uint16_t>(
    snapshot->count[bandid - 1], WEB_BANDMAP_MAX_DISPLAY_ENTRIES);
  state->band[0].bandid = bandid;
  state->band[0].count = count;
  if (count) {
    state->band[0].entries = static_cast<WebBandmapEntry *>(
      heap_caps_malloc(sizeof(WebBandmapEntry) * count,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!state->band[0].entries) {
      xSemaphoreGive(web_bandmap_snapshot_mutex);
      delete state;
      return nullptr;
    }
    memcpy(state->band[0].entries,
           snapshot->entry[bandid - 1],
           sizeof(WebBandmapEntry) * count);
  }
  xSemaphoreGive(web_bandmap_snapshot_mutex);
  state->band_count = 1;
  return state;
}

static size_t web_bandmap_copy_line(WebBandmapApiState *state,
                                    uint8_t *buffer, size_t maxLen) {
  const size_t length = strlen(state->line);
  const size_t remain = length - state->offset;
  const size_t ncopy = std::min(remain, maxLen);
  if (ncopy) memcpy(buffer, state->line + state->offset, ncopy);
  state->offset += ncopy;
  if (state->offset == length) {
    state->offset = 0;
    state->line[0] = '\0';
  }
  return ncopy;
}

static void web_bandmap_json_safe_copy(char *dest, size_t dest_size,
                                       const char *source) {
  if (!dest || dest_size == 0) return;
  size_t out = 0;
  if (!source) source = "";
  while (*source && out + 1 < dest_size) {
    const unsigned char c = static_cast<unsigned char>(*source++);
    // Callsigns and band labels should be printable ASCII.  Replace JSON
    // metacharacters/control bytes rather than allowing malformed JSON.
    if (c < 0x20 || c == '"' || c == '\\') dest[out++] = '?';
    else dest[out++] = static_cast<char>(c);
  }
  dest[out] = '\0';
}

static void setup_web_bandmap_handlers() {
  web_server.on("/bandmap", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(
      200, "text/html; charset=utf-8", web_bandmap_page);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  web_server.on("/api/bandmap/bands", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"bands\":[";
    for (int bandid = 1; bandid < N_BAND; ++bandid) {
      if (bandid > 1) json += ',';
      json += "{\"id\":" + String(bandid) + ",\"label\":\"";
      String label = bandid_str[bandid - 1];
      label.trim();
      json += label;
      json += "\"}";
    }
    json += "]}";
    request->send(200, "application/json", json);
  });

  web_server.on("/api/bandmap/version", HTTP_GET, [](AsyncWebServerRequest *request) {
    const bool ready = web_bandmap_snapshot_ready;
    char json[768];
    size_t used = snprintf(json, sizeof(json),
                           "{\"ready\":%s,\"bands\":{",
                           ready ? "true" : "false");
    if (ready &&
        xSemaphoreTake(web_bandmap_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
      const WebBandmapSnapshot *snapshot =
        web_bandmap_snapshots[web_bandmap_active_snapshot];
      for (int band_index = 0; band_index < WEB_BANDMAP_BANDS; ++band_index) {
        used += snprintf(json + used, sizeof(json) - used,
                         "%s\"%d\":%lu",
                         band_index ? "," : "", band_index + 1,
                         static_cast<unsigned long>(
                           snapshot->band_generation[band_index]));
      }
      xSemaphoreGive(web_bandmap_snapshot_mutex);
    }
    snprintf(json + used, sizeof(json) - used, "}}");
    request->send(200, "application/json", json);
  });

  web_server.on("/api/bandmap/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    const uint8_t bandid = parse_web_bandmap_band(request);
    WebBandmapApiState *state = make_web_bandmap_api_state(bandid);
    if (!state) {
      request->send(503, "text/plain", "bandmap snapshot unavailable");
      return;
    }

    // A single band is limited to WEB_BANDMAP_MAX_DISPLAY_ENTRIES (20), so
    // build a complete JSON document and send it with Content-Length.  This
    // avoids truncated HTTP-200 JSON caused by the chunk generator state
    // machine while keeping peak memory bounded to a few kilobytes.
    String json;
    size_t reserve_size = 160U +
      static_cast<size_t>(state->band[0].count) * 144U;
    if (reserve_size < 1024U) reserve_size = 1024U;
    if (!json.reserve(reserve_size)) {
      delete state;
      request->send(503, "text/plain", "bandmap JSON allocation failed");
      return;
    }

    json += F("{\"bandGeneration\":");
    json += static_cast<unsigned long>(state->generation);
    json += F(",\"bands\":[{");
    json += F("\"id\":");
    json += state->band[0].bandid;
    json += F(",\"label\":\"");

    String label = bandid_str[state->band[0].bandid - 1];
    label.trim();
    char safe_label[32];
    web_bandmap_json_safe_copy(safe_label, sizeof(safe_label), label.c_str());
    json += safe_label;
    json += F("\",\"spots\":[");

    const int now = my_rtc.unixtime();
    for (uint16_t i = 0; i < state->band[0].count; ++i) {
      const WebBandmapEntry &entry = state->band[0].entries[i];
      const int age = max(0, (now - entry.time) / 60);
      const unsigned long hz =
        static_cast<unsigned long>(entry.freq) * FREQ_UNIT;
      char freq_text[24];
      snprintf(freq_text, sizeof(freq_text), "%lu.%01lu",
               hz / 1000UL, (hz % 1000UL) / 100UL);
      char safe_station[sizeof(entry.station)];
      web_bandmap_json_safe_copy(safe_station, sizeof(safe_station),
                                 entry.station);

      if (i) json += ',';
      json += F("{\"freq\":");
      json += static_cast<unsigned long>(entry.freq);
      json += F(",\"freqText\":\"");
      json += freq_text;
      json += F("\",\"call\":\"");
      json += safe_station;
      json += F("\",\"mode\":");
      json += entry.mode;
      json += F(",\"time\":");
      json += static_cast<long>(entry.time);
      json += F(",\"age\":");
      json += age;
      json += F(",\"multi\":");
      json += (entry.flag & BANDMAP_ENTRY_FLAG_NEWMULTI) ? F("true") : F("false");
      json += '}';
    }
    json += F("]}]}" );

    delete state;
    AsyncWebServerResponse *response =
      request->beginResponse(200, "application/json", json);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  web_server.on("/api/bandmap/select", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      const char *required[] = {"band", "freq", "mode", "time", "call"};
      for (const char *name : required) {
        if (!request->hasParam(name, true)) {
          request->send(400, "text/plain", "missing parameter");
          return;
        }
      }

      WebBandmapCommand command{};
      command.type = WEB_BANDMAP_COMMAND_SELECT;
      command.bandid = request->getParam("band", true)->value().toInt();
      command.freq = request->getParam("freq", true)->value().toInt();
      command.mode = request->getParam("mode", true)->value().toInt();
      command.time = request->getParam("time", true)->value().toInt();
      strlcpy(command.station,
              request->getParam("call", true)->value().c_str(),
              sizeof(command.station));

      if (command.bandid < 1 || command.bandid >= N_BAND ||
          command.station[0] == '\0'  || command.mode >= NMODEID) {
        request->send(400, "text/plain", "invalid spot");
        return;
      }
      if (!enqueue_web_bandmap_command(command)) {
        request->send(503, "text/plain", "bandmap command queue full");
        return;
      }
      request->send(202, "text/plain", "spot selection queued");
    });

  auto enqueue_edit_command = [](AsyncWebServerRequest *request,
                                 WebBandmapCommandType type) {
    const char *required[] = {"band", "freq", "mode", "time", "call"};
    for (const char *name : required) {
      if (!request->hasParam(name, true)) {
        request->send(400, "text/plain", "missing parameter");
        return;
      }
    }

    WebBandmapCommand command{};
    command.type = type;
    command.bandid = request->getParam("band", true)->value().toInt();
    command.freq = request->getParam("freq", true)->value().toInt();
    command.mode = request->getParam("mode", true)->value().toInt();
    command.time = request->getParam("time", true)->value().toInt();
    String old_call = request->getParam("call", true)->value();
    old_call.trim(); old_call.toUpperCase();
    strlcpy(command.station, old_call.c_str(), sizeof(command.station));

    if (type == WEB_BANDMAP_COMMAND_CORRECT) {
      if (!request->hasParam("newcall", true)) {
        request->send(400, "text/plain", "missing newcall");
        return;
      }
      String new_call = request->getParam("newcall", true)->value();
      new_call.trim(); new_call.toUpperCase();
      strlcpy(command.new_station, new_call.c_str(),
              sizeof(command.new_station));
      if (!valid_web_bandmap_callsign(command.new_station)) {
        request->send(400, "text/plain", "invalid callsign");
        return;
      }
    }

    if (command.bandid < 1 || command.bandid >= N_BAND ||
        command.station[0] == '\0' || command.mode >= NMODEID) {
      request->send(400, "text/plain", "invalid spot");
      return;
    }
    if (!enqueue_web_bandmap_command(command)) {
      request->send(503, "text/plain", "bandmap command queue full");
      return;
    }
    request->send(202, "text/plain",
                  type == WEB_BANDMAP_COMMAND_DELETE
                    ? "spot deletion queued"
                    : "callsign correction queued");
  };

  web_server.on("/api/bandmap/delete", HTTP_POST,
    [enqueue_edit_command](AsyncWebServerRequest *request) {
      enqueue_edit_command(request, WEB_BANDMAP_COMMAND_DELETE);
    });
  web_server.on("/api/bandmap/correct", HTTP_POST,
    [enqueue_edit_command](AsyncWebServerRequest *request) {
      enqueue_edit_command(request, WEB_BANDMAP_COMMAND_CORRECT);
    });
}
}

void process_web_bandmap() {
  process_web_bandmap_command_queue();
  const uint32_t now = millis();
  if ((int32_t)(now - web_bandmap_next_refresh_ms) >= 0) {
    web_bandmap_next_refresh_ms = now + WEB_BANDMAP_REFRESH_MS;
    rebuild_web_bandmap_snapshot();
  }
}

void init_webserver() {
  web_heap_point("before web handlers");

  
  web_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String logmessage = "Client:" + request->client()->remoteIP().toString() + + " " + request->url();
    webLog.println(logmessage);
    request->send_P(200, "text/html", index_html, processor);
    logmessage="";
  });

  // /potahelp
  //  const char pota_page[] PROGMEM = R"rawliteral(
  const char *pota_page = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>POTA ADIF Downloader</title></head>
<body>
<h3>Enter QRV POTA Park Number</h3>
<input type="text" id="park" %PARK_ID% placeholder="e.g. JP-1001" oninput="updateDownloadLink()">
<button onclick="openPOTA()">Open POTA uploader</button>
<br><br>
<a id="dl" href="/adif" download="pota_log.adi">↓ Download ADIF</a>
<p id="status"></p>

<input id="grid" value="%GRID_LOCATOR%" placeholder="Grid (e.g. PM95ru)">
<button onclick="findNearest()">Find</button>
<ul id="results"></ul>
<a href="/" >go back to Home</a>

<script>
function findNearest() {
  const grid = document.getElementById("grid").value.trim();
  fetch(`/nearest?grid=${grid}`)
    .then(r => r.json())
    .then(showResults);
}

function showResults(list) {
  const ul = document.getElementById("results");
  ul.innerHTML = "";
  list.forEach(p => {
    const li  = document.createElement("li");
    const a   = document.createElement("a");
    a.href    = "#";
    a.textContent = `${p.code}: ${p.name} (${p.distance_km} km ${p.bearing_deg} deg.)`;
    a.onclick = () => selectPark(p.code, p.name, document.getElementById("grid").value);
    li.appendChild(a);
    ul.appendChild(li);
  });
}

function selectPark(code, name, grid) {
  // サーバーに選択を通知
  fetch(`/select?code=${encodeURIComponent(code)}&name=${encodeURIComponent(name)}&grid=${encodeURIComponent(grid)}`)
    .then(() => {
      // park入力欄を更新
      document.getElementById("park").value = code;
      // ダウンロードリンクを更新
      updateDownloadLink();
    });

  // pota park ページを開く
  window.open(`https://pota.app/#/park/${code}`, "_blank");
}

function updateDownloadLink() {
  const park = document.getElementById("park").value.trim();
  const link = document.getElementById("dl");

  if (park.length === 0) {
    link.href = "/adif";
    link.download = "pota_log.adi";
    document.getElementById("status").innerText = "Please enter a QRV park number.";
  } else {
    link.href = `/adif?park=${encodeURIComponent(park)}`;
    link.download = `pota_log_${park}.adi`;
    document.getElementById("status").innerText = `Ready to download log for park ${park}`;
  }
}

function openPOTA(){
  window.open('https://pota.app/#/user/logs','_blank');
  alert('① 新タブで POTA にログインし、\n② 先にダウンロードした pota_l_PARK#.adi をドラッグ＆ドロップしてください。');
}

</script>
</body>
</html>
)rawliteral";  

  
  web_server.on("/potahelp", HTTP_GET, [pota_page](AsyncWebServerRequest* request){
    //    String html = FPSTR(pota_page);
    String html(pota_page);
    String gl = String(plogw->grid_locator_set);
    html.replace("%GRID_LOCATOR%", gl);
    // replace park
    char *p1;
    if ((p1=strstr(plogw->jcc+2,"POTA/"))!=NULL) {
	char tmpbuf1[100];
	strcpy(tmpbuf1,p1+5);
	p1=strtok(tmpbuf1," ");
	if (p1!=NULL) {
	  String park = String("value=\"")+String(p1)+String("\"");
	  html.replace("%PARK_ID%",park);
	  park="";
	}
    } else {
      html.replace("%PARK_ID%",String(""));
    }

    //    request->send_P(200, "text/html", pota_page, processor);
    request->send(200, "text/html", html);
  });



  /* ───── /select?code=JA-0001&name=Yoyogi&grid=PM95ru ───── */
  web_server.on("/select", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("code") && req->hasParam("name") && req->hasParam("grid")) {
      selectedParkCode = req->getParam("code")->value();
      selectedParkName = req->getParam("name")->value();
      selectedGrid     = req->getParam("grid")->value();
      webLog.printf("SELECTED  %s  %s  %s\n",
		    selectedParkCode.c_str(),
		    selectedParkName.c_str(),
		    selectedGrid.c_str());

      // replace park(POTA/) in plogw->jcc+2
      replace_string(plogw->jcc+2,"POTA/",selectedParkCode.c_str()," ");
      webLog.print("jcc modified:");webLog.println(plogw->jcc+2);
      req->send(200, "text/plain", "OK");
    } else {
      req->send(400, "text/plain", "Missing params");
  }
  });


  const char *sota_page = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>SOTA helper</title></head>
<body>
<h3>Enter QRV SOTA Summit ID</h3>
<input type="text" id="summit" %SUMMIT_ID% placeholder="e.g. JA/KN-006" oninput="updateDownloadLinkSOTA()">
<button onclick="openSOTA()">Open SOTA uploader</button>
<br><br>
<a id="dl" href="/adif" download="sota_log.adi">↓ Download ADIF</a>
<p id="status"></p>

  <input id="grid" value="%GRID_LOCATOR%" placeholder="Grid (e.g. PM95ru)">
<button onclick="findSota()">Find SOTA summit</button>
<ul id="sotaResults"></ul>
<a href="/" >go back to Home</a>

<script>
function findSota(){
  const g=document.getElementById("grid").value.trim();
  fetch(`/nearest_summit?grid=${g}`).then(r=>r.json()).then(showSota);
}
function showSota(list){
  const ul=document.getElementById("sotaResults"); ul.innerHTML="";
  list.forEach(s=>{
    const li=document.createElement("li");
    const a=document.createElement("a");
    a.href="#"; a.textContent=`${s.code}: ${s.name} (${s.distance_km} km, ${s.alt}m, ${s.bearing_deg} deg.)`;
    a.onclick=()=>selectSota(s.code,s.name,document.getElementById("grid").value);
    li.appendChild(a); ul.appendChild(li);
  });
}
function selectSota(code, name, grid) {
  // サーバーに選択を通知
  fetch(`/select_summit?code=${code}&name=${encodeURIComponent(name)}&grid=${grid}`)
    .then(() => {
      // summit入力欄を更新
      document.getElementById("summit").value = code;
      // ダウンロードリンクを更新
      updateDownloadLinkSOTA();
    });

  // summitページを開く
  window.open(`https://sotl.as/summits/${code}`, "_blank");
}

function updateDownloadLinkSOTA() {
  const summit = document.getElementById("summit").value.trim();
  const link = document.getElementById("dl");

  if (summit.length === 0) {
    link.href = "/adif";
    link.download = "sota_log.adi";
    document.getElementById("status").innerText = "Please enter a QRV summit number.";
  } else {
    link.href = `/adif?summit=${encodeURIComponent(summit)}`;
    link.download = `sota_log_${summit}.adi`;
    document.getElementById("status").innerText = `Ready to download log for summit ${summit}`;
  }
}

function openSOTA(){
  window.open('https://www.sotadata.org.uk/ja/upload','_blank');
}

</script>
)rawliteral";  
  
  web_server.on("/sotahelp", HTTP_GET, [sota_page](AsyncWebServerRequest* request){
    //    String html = FPSTR(pota_page);
    String html(sota_page);
    String gl = String(plogw->grid_locator_set);
    html.replace("%GRID_LOCATOR%", gl);
    gl="";
    // replace summit
    char *p1;
    if ((p1=strstr(plogw->jcc+2,"SOTA/"))!=NULL) {
      char tmpbuf1[100];
      strcpy(tmpbuf1,p1+5);
      p1=strtok(tmpbuf1," ");
      if (p1!=NULL) {
	String summit = String("value=\"")+String(p1)+String("\"");
	html.replace("%SUMMIT_ID%",summit);
	summit="";
      }
    } else {
      html.replace("%SUMMIT_ID%",String(""));
    }

    //    request->send_P(200, "text/html", pota_page, processor);
    request->send(200, "text/html", html);
    html="";
  });



  web_server.on("/select_summit", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("code")&&r->hasParam("name")&&r->hasParam("grid")){
      selSotaCode=r->getParam("code")->value();
      selSotaName=r->getParam("name")->value();
      selGrid    =r->getParam("grid")->value();

      replace_string(plogw->jcc+2,"SOTA/",selSotaCode.c_str()," ");
      webLog.print("jcc modified:");webLog.println(plogw->jcc+2);
      
      webLog.print("select sota code:"); webLog.print(selSotaCode);
      webLog.print(" grid:"); webLog.println(selGrid);
      r->send(200,"text/plain","OK");
    } else r->send(400,"text/plain","missing");
  });
  
  // Send a GET request to <IP>/get?message=<message>  
  // run handleUpload function when any file is uploaded
  web_server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200);
  }, handleUpload);

  
  // /log
  web_server.on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    String type= request->hasParam("type") ? request->getParam("type")->value() : "0";
    handleQsoLogDump(request, numstr,type.toInt());  // option 0: plain, 1: html  type 0:dump 1:txt 2:adif 3:csv
  });

  // /jarlog
  web_server.on("/jarllog", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    handleQsoLogDump(request, numstr,4);  //   type 0:dump 1:txt 2:adif 3:csv 4:jarllog
  });

  // /readqso
  web_server.on("/readqso", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    handleQsoLogDump(request, numstr,1);  // type 0:dump 1:txt 2:adif 3:csv 4:jarlog
  });

  // /dumpqso
  web_server.on("/dumpqso", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    handleQsoLogDump(request, numstr,0);  // type 0:dump 1:txt 2:adif 3:csv 4:jarlog
  });
  
  // /adif
  web_server.on("/adif", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";

    handleQsoLogDump(request, numstr,2);  // type 0:dump 1:txt 2:adif 3:csv 4:jarlog
  });
  
  // /csv hamlogcsv
  web_server.on("/csv", HTTP_GET, [](AsyncWebServerRequest* request) {
    String numstr = request->hasParam("num") ? request->getParam("num")->value() : "";
    handleQsoLogDump(request, numstr,3);  // type 0:dump 1:txt 2:adif 3:csv 4:jarlog
  });

// static変数としてShiftキーの状態を保持
static bool shiftLeftPressed = false;  // 左Shiftキーの状態
static bool shiftRightPressed = false;  // 右Shiftキーの状態

// /op ページ配信
web_server.on("/op", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", oppage_html);
});

web_server.on("/rig_key", HTTP_GET, [](AsyncWebServerRequest *req) {
  char response_string[100];
  strcpy(response_string,"");
  struct radio *radio;  
  if (req->hasParam("keycode")) {
    int keycode = req->getParam("keycode")->value().toInt();  // 送信されたキーコードを整数に変換
    String keyName = "Unknown Key";

    // HIDキーコードに基づいて処理
    if (keycodeToHid.find(keycode) != keycodeToHid.end()) {
      keyName = String(keycodeToHid.at(keycode));  // HIDキーコードから名前を取得
      webLog.printf("Received HID Key: %s keycode %d\n", keyName.c_str(),keycode);
    } else {
      webLog.printf("Unknown keycode: %d\n", keycode);
    }
    keyName="";

    // function keys 
    if (keycode>=112 && keycode <=116) {
      webLog.println("function keys");
      so2r.cancel_msg_tx();	
      so2r.set_msg_tx_to_focused(); // start sending in the currently focued radio
      so2r.set_rx_in_sending_msg();	
      function_keys(keycode-54, 0);
    } else if (keycode==27) {
      // esc
      webLog.println("esc from web");
      so2r.cancel_msg_tx();
      switch(so2r.radio_mode) {
      case SO2R::RADIO_MODE_SO2R:
	so2r.sequence_mode(SO2R::SO2R_CQSandP);
	break;
      case SO2R::RADIO_MODE_SO1R:
      case SO2R::RADIO_MODE_SAT:	
	so2r.sequence_mode(SO2R::Manual);
	break;
      }
      so2r.sequence_stat(SO2R::Default);
    }


    radio=so2r.radio_selected();
    
    // インデックスが送信されている場合
    int idx = -1;
    if (req->hasParam("index")) {
      idx = req->getParam("index")->value().toInt();
    }

    // 入力内容（input0, input1）の受け取り
    String input1 = "";
    String input2 = "";
    int flag=0;
    if (req->hasParam("input0")) {
      flag|=1;
      input1 = req->getParam("input0")->value();
      // update radio->callsign
      strncpy(radio->callsign+2,input1.c_str(),LEN_CALL_WINDOW-1);
    }
    if (req->hasParam("input1")) {
      flag|=2;
      input2 = req->getParam("input1")->value();
      // update radio->recv_exch
      strncpy(radio->recv_exch+2,input2.c_str(),LEN_EXCH_WINDOW-1);
    }
    if (flag) {
      switch (idx) {
      case 0:// callsign 
	radio->ptr_curr=0; // callsign window
	// process_enter
	process_enter(0);
	break;
      case 1: // exch
	radio->ptr_curr=1; // callsign window
	process_enter(0);
	break;
      }
    }


    // ログに出力
    webLog.printf("Received input0: %s, input1: %s, index: %d\n", input1.c_str(), input2.c_str(), idx);
    input1="";
    input2="";
    strcpy(response_string,"Keycode received and processed.");
    req->send(200, "text/plain",response_string);
    
  } else if (req->hasParam("command")) {

    String command;
    // rig name change
    command = req->getParam("command")->value();
    webLog.print("command:");webLog.println(command);
    if (command == "set") {
      if (req->hasParam("index") && req->hasParam("value")) {
	// get index and value
	int idx = req->getParam("index")->value().toInt();
	String value= req->getParam("value")->value();
	switch (idx) {
	case 10: // radio 0 name
	case 11: // radio 1 name
	case 12: // radio 2 name
	  int radio_idx;	  
	  radio_idx=idx-10;
	  if (radio_idx>=0 || radio_idx <= 2) {
	    struct radio *radio ;
	    radio = &radio_list[radio_idx];
	    strncpy(radio->rig_name+2,value.c_str(),LEN_RIG_NAME-1);
	    set_rig_from_name(radio);
	    webLog.print("rig change radio=");webLog.print(radio_idx);
	    webLog.print(" name=");webLog.println(radio->rig_name+2);
	    strcpy(response_string,"OK");
	  }
	  break;
	case 13: // contest name
	  strncpy(plogw->contest_name+2,value.c_str(),LEN_CONTEST_NAME);
          plogw->contest_name[2 + LEN_CONTEST_NAME] = '\0';
          if (is_user_md_contest_name(plogw->contest_name + 2)) {
            start_user_md_contest(plogw->contest_name + 2);
          } else {
	    search_contest_id_from_name();
          }
	  strcpy(response_string,"OK");
	  break;
	}
	value="";
      }
    }
    command="";
    req->send(200, "text/plain",response_string);
  } else {
    req->send(400, "text/plain", "Missing parameter");
  }

});

web_server.on("/control", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasParam("type") && request->hasParam("value")) {
    String type =request->getParam("type")->value();
    String value =request->getParam("value")->value();    
    //      int index = request->getParam("index")->value().toInt();
    //      String value = request->getParam("value")->value();
    int ival;
    ival = value.toInt();
    const char *valuecstr;
    valuecstr=value.c_str();    
    struct radio *radio;

    radio=so2r.radio_selected();
    // control type Radio Mode Band
    webLog.print("/control type=");
    webLog.print(type);
    webLog.print(" value=");
    webLog.println(value);
    int modetype;
    if (type == "Radio") {
      so2r.change_focused_radio(ival);
    } else if (type == "Mode") {
      modetype = modetype_string(valuecstr);
      int filt;
      filt = radio->filtbank[radio->bandid][radio->cq[modetype]][modetype];
      if (filt==0) {
	filt=default_filt(valuecstr);
      }
      set_mode(valuecstr, filt, radio);
      send_mode_set_civ(valuecstr, filt);
    } else if (type == "Band") {
      if (ival>=1 && ival<N_BAND) {
	if (((1<<(ival -1)) & radio->band_mask) == 0) {
	  band_change(ival,radio);
	  webLog.print("band_change to band ");
	  webLog.println(ival);
	}
      }
    }

    type="";
    value="";
  }
  request->send(200, "text/plain","OK");
 });

       
// サーバー側でHTMLの一部（radio状態）を返すエンドポイント
web_server.on("/radio_status", HTTP_GET, [](AsyncWebServerRequest *req) {
  struct radio *radio;  
  radio=so2r.radio_selected();
  char string_buf[100];  
  if (req->hasParam("index")) {
    // indexパラメータに応じて、返す内容を変更
    int index = req->getParam("index")->value().toInt();
    unsigned int tmp1, tmp2;

    switch (index) {
    case 0: // call
      req->send(200, "text/plain", radio->callsign+2);  // 状態をテキストとして返す      
      break;
    case 1: // exch
      req->send(200, "text/plain", radio->recv_exch+2); 
      break;
    case 2: // received rst
      req->send(200, "text/plain", radio->recv_rst+2);  
      break;
    case 3: // sent rst
      req->send(200, "text/plain",radio->sent_rst+2);        
      break;
    case 4: // sent exch
      req->send(200, "text/plain", plogw->sent_exch+2); // this needs to be revised to reflect expanded exchange character
      break;
    case 5: // my_callsign
      req->send(200, "text/plain", plogw->my_callsign+2);              
      break;
    case 6: // rig name (focused radio)
      req->send(200, "text/plain", radio->rig_name+2);
      break;
    case 7: // rig_idx (radio #)
      sprintf(string_buf,"%d",radio->rig_idx);
      req->send(200, "text/plain", string_buf);
      break;
    case 8: // mode
      req->send(200, "text/plain", radio->opmode);
      break;
    case 9: // bandid
      sprintf(string_buf,"%d",radio->bandid);
      req->send(200, "text/plain", string_buf);
      break;
    case 10: // rig_name (radio 0)
      radio=&radio_list[0];
      req->send(200, "text/plain", radio->rig_name+2);
      break;
    case 11: // rig_name (radio 1)
      radio=&radio_list[1];
      req->send(200, "text/plain", radio->rig_name+2);
      break;
    case 12: // rig_name (radio 2)
      radio=&radio_list[2];
      req->send(200, "text/plain", radio->rig_name+2);
      break;
    case 13: // contest_name 
      req->send(200, "text/plain", plogw->contest_name+2);
      break;
    case 98: // cw
      strncpy(string_buf,plogw->cwbuf_display,50);
      req->send(200, "text/plain", string_buf);
      break;
    case 99: // radio
      tmp2= (radio->freq % (1000/FREQ_UNIT))/(10/FREQ_UNIT); // below kHz
      tmp1 = radio->freq / (1000/FREQ_UNIT); 
      tmp1 = tmp1 % 1000 ;  // kHz
      
      sprintf(string_buf,"%-14s Radio:%d %3s Freq:%4d.%03d.%02d Hz Mode:%4s",
	      plogw->tm+9,
	      radio->rig_idx,
	      radio->cq[radio->modetype] == LOG_CQ ? "CQ" : "S&P",
	      radio->freq/(1000000/FREQ_UNIT),tmp1,tmp2,
	      radio->opmode);
      req->send(200, "text/plain", string_buf);
      //      req->send(200, "text/plain", "");
      break;
    default:
       req->send(400, "text/plain", "Invalid index parameter");
       break;
    }
  } else {
    req->send(400, "text/plain", "Missing index parameter");
    return;
  }
});

  // 入力受信用API
  web_server.on("/input", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("call")) {
      String call = request->getParam("call")->value();
      webLog.printf("[AJAX] Received: %s\n", call.c_str());
      request->send(200, "text/plain", "Received: " + call);
    } else {
      request->send(400, "text/plain", "Missing call param");
    }
  });

  // Send a POST request to <IP>/post with a form field message set to <message>
  web_server.on("/post", HTTP_POST, [](AsyncWebServerRequest *request){
    String message;
    if (request->hasParam(PARAM_MESSAGE, true)) {
      message = request->getParam(PARAM_MESSAGE, true)->value();
    } else {
      message = "No message sent";
    }
    request->send(200, "text/plain", "Hello, POST: " + message);
  });


  setup_web_bandmap_handlers();

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  web_server.onNotFound(notFound);

  setupNearestHandler(web_server);
  setupNearestSummit(web_server);
  setupContestPageHandler();
  setupSettingsPageHandler();  
  web_heap_point("after web handlers");
  web_server.begin();
  web_heap_point("after web begin");
}



String listFiles(bool ishtml = false);

