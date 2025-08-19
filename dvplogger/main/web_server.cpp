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


AsyncWebServer web_server(80);

const char* PARAM_MESSAGE = "message";

void notFound(AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
}


void rebootESP(String message) {
  console->print("Rebooting ESP32: "); Serial.println(message);
  ESP.restart();
}

String humanReadableSize(const size_t bytes);

// list all of the files, if ishtml=true, return html rather than simple text
String listFiles(bool ishtml) {
  String returnText = "";
  console->println("Listing files stored on SPIFFS");
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
  console->println(logmessage);

  if (!index) {
    logmessage = "Upload Start: " + String(filename);
    // open the file on first call and store the file handle in the request object
    request->_tempFile = SD.open("/" + filename, "w");
    console->println(logmessage);
  }

  if (len) {
    // stream the incoming chunk to the opened file
    request->_tempFile.write(data, len);
    logmessage = "Writing file: " + String(filename) + " index=" + String(index) + " len=" + String(len);
    console->println(logmessage);
  }

  if (final) {
    logmessage = "Upload Complete: " + String(filename) + ",size: " + String(index + len);
    // close the file handle as the upload is now done
    request->_tempFile.close();
    console->println(logmessage);
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
    console->print("grid=");console->print(gridstr);
    console->print("<-");console->println(grid.c_str());
	      
    myLat=mh2lat(gridstr);
    myLon=mh2lon(gridstr);

    console->print("lat,lon=");console->print(myLat);console->print(" ");console->println(myLon);

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
      /*      console->print("code:");console->print(code);
      console->print("name:");console->print(name);      
      console->print(" lat:");console->print(lat);
      console->print(" lon:");console->println(lon);
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
    console->print("grid=");console->print(gridstr);
    console->print("<-");console->println(grid.c_str());
	      
    myLat=mh2lat(gridstr);
    myLon=mh2lon(gridstr);
    console->print("lat,lon=");console->print(myLat);console->print(" ");console->println(myLon);

    File f = SD.open("/ja_sota.csv","r");
    if (!f){ req->send(500,"text/plain","SOTA CSV open err"); return; }
    console->println("open ja_sota.csv");

    SummitInfo best[3]; int filled=0;
    while(f.available()){
      String line=f.readStringUntil('\n');
      if(line.startsWith("summitCode")) continue;
      //      console->println(line);
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
      //      console->print("lat:");console->print(lat);
      //      console->print("lon:");console->println(lon);      
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
    console->println(json);
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


const char *rigs_page_html = R"rawliteral(
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
    %RIGS_INPUTS%
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
    char spec_buf[300];
    String html = rigs_page_html;  // テンプレートを複製
    String inputs;

    for (int i = 0; i < N_RIG; ++i) {
      if (*rig_spec[i].name=='\0') {
	break;
      }
      char line[300];
      *spec_buf='\0';
      print_rig_spec_str(i,spec_buf);
      spec_buf[strlen(spec_buf)]='\0';
      //      console->print(i); console->print(";");
      //      console->print(rig_spec[i].name);      console->print(":");
      //      console->println(spec_buf);
      snprintf(line, sizeof(line), example_input_html,
	       i,
	       rig_spec[i].name,
	       i,
	       i,
	       spec_buf,
	       300,
	       ""
	       );
      //      console->print("line:");console->println(line);
      inputs += line;
    }

    html.replace("%RIGS_INPUTS%", inputs);
    request->send(200, "text/html", html);
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
      //      console->print("rig_edit value:");console->println(value.c_str());
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
      //      console->println("header written");
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
      console->println("pending line write");      
    } else {
      // まだ入らない → 1バイトだけ送る
      memcpy(buffer + bytesWritten, "\n", 1);
      console->println("dummy write +");
      return bytesWritten + 1;
    }
  }

  // 2. ファイル終端チェック
  if (!state.file || state.finished || state.pos >= state.file.size()) {
    state.finished = true;
    console->print("state.file:");    console->print(state.file);
    console->print(" state.finished:");    console->println(state.finished);    
    return 0;
  }

  // main qso read & dump loop
  console->print("maxLen:");      console->println(maxLen);
  while ((bytesWritten < maxLen) && (bytesWritten < 2048) ) {
    //  while ((bytesWritten < maxLen)) {
    if (state.pos >= state.file.size()) {
      state.finished = true;
      console->print("chk state.pos:");      console->print(state.pos);
      console->print(" state.file.size():");      console->println(state.file.size());      
      break;
    }

    state.file.seek(state.pos);
    size_t n = state.file.read((uint8_t*)state.qso.all, RECORD_SIZE);
    if (n < RECORD_SIZE) {
      state.finished = true;
      console->println("n<record_size");
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
	console->print("errortic state.type=");console->println(state.type);
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
      console->print("bytesWritten:");console->print(bytesWritten);
      console->print(" pos:");console->println(state.pos);    
    } else {
      // 5. 今回は出力できない → キャッシュに入れて、最小限の出力
      state.pendingLine = line;
      memcpy(buffer+bytesWritten, "\n", 1);  // ダミーでも返す
      console->println("pending line  +cache + dummy write");      
      return bytesWritten+1;
    }
  }

  // 終了処理
  if (state.finished) {
    console->println("state.finished reached");
    String footer = "";
    if (state.type!=2) { // not adif
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
      console->println("dummy write");
      memcpy(buffer, "\n", 1);
      return 1;
    }
  }
  console->print("returning bytesWritten:");console->println(bytesWritten);
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
<p><a href="/rigs">/rigs</a> View/Edit RIG Settings</p>
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
   <input type="text" id="edit_13" data-index="13" size="15"> <!-- contest_name 0 -->
    </div>
 <div class="form-container" id="cwkeyingDisplay"></div> <!-- CW keying ticker display -->
    <p>Band map test</p>
    <p><a href="http://192.168.1.2/">go back to Home</a></p>

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

void init_webserver() {

  
  web_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String logmessage = "Client:" + request->client()->remoteIP().toString() + + " " + request->url();
    console->println(logmessage);
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
      console->printf("SELECTED  %s  %s  %s\n",
		    selectedParkCode.c_str(),
		    selectedParkName.c_str(),
		    selectedGrid.c_str());

      // replace park(POTA/) in plogw->jcc+2
      replace_string(plogw->jcc+2,"POTA/",selectedParkCode.c_str()," ");
      console->print("jcc modified:");console->println(plogw->jcc+2);
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
      console->print("jcc modified:");console->println(plogw->jcc+2);
      
      console->print("select sota code:"); console->print(selSotaCode);
      console->print(" grid:"); console->println(selGrid);
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
      Serial.printf("Received HID Key: %s keycode %d\n", keyName.c_str(),keycode);
    } else {
      Serial.printf("Unknown keycode: %d\n", keycode);
    }
    keyName="";

    // function keys 
    if (keycode>=112 && keycode <=116) {
      console->println("function keys");
      so2r.cancel_msg_tx();	
      so2r.set_msg_tx_to_focused(); // start sending in the currently focued radio
      so2r.set_rx_in_sending_msg();	
      function_keys(keycode-54, 0);
    } else if (keycode==27) {
      // esc
      console->println("esc from web");
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
    Serial.printf("Received input0: %s, input1: %s, index: %d\n", input1.c_str(), input2.c_str(), idx);
    input1="";
    input2="";
    strcpy(response_string,"Keycode received and processed.");
    req->send(200, "text/plain",response_string);
    
  } else if (req->hasParam("command")) {

    String command;
    // rig name change
    command = req->getParam("command")->value();
    console->print("command:");console->println(command);
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
	    console->print("rig change radio=");console->print(radio_idx);
	    console->print(" name=");console->println(radio->rig_name+2);
	    strcpy(response_string,"OK");
	  }
	  break;
	case 13: // contest name
	  strncpy(plogw->contest_name+2,value.c_str(),10-1);
	  search_contest_id_from_name() ;
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
    console->print("/control type=");
    console->print(type);
    console->print(" value=");
    console->println(value);
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
	  console->print("band_change to band ");
	  console->println(ival);
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
      Serial.printf("[AJAX] Received: %s\n", call.c_str());
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


  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  web_server.onNotFound(notFound);

  setupNearestHandler(web_server);
  setupNearestSummit(web_server);
  setupSettingsPageHandler();  
  web_server.begin();
}



String listFiles(bool ishtml = false);

