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
#include "web_server.h"
#include "decl.h"
#include "cluster.h"
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
#include "antenna.h"
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

static String normalize_op_value(int index, const String &source) {
  String out;
  out.reserve(source.length());
  for (size_t i = 0; i < source.length(); ++i) {
    unsigned char uc = (unsigned char)source[i];
    char c = (char)uc;
    if (index == 0 || index == 1 || index == 4 || index == 5 ||
        index == 10 || index == 11 || index == 12) {
      if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    }
    bool accept = false;
    switch (index) {
      case 0: case 1:
        accept = ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.');
        break;
      case 4:
        // Sent Exch is a CW macro source.  Preserve characters such as '$'
        // exactly as entered; expansion is performed only when transmitting.
        accept = (uc >= 0x21 && uc <= 0x7e);
        break;
      case 2: case 3:
        accept = (c >= '0' && c <= '9');
        break;
      case 5: case 10: case 11: case 12:
        accept = (uc >= 0x21 && uc <= 0x7e);  // printable ASCII except space
        break;
      case 13:
        accept = (uc >= 0x21 && uc <= 0x7e);  // contest name: keep case, omit spaces
        break;
      default:
        accept = (uc >= 0x20 && uc <= 0x7e);
        break;
    }
    if (accept) out += c;
  }
  return out;
}

static void normalize_op_cstr(int index, char *dst, size_t dst_size, const String &source) {
  String value = normalize_op_value(index, source);
  strlcpy(dst, value.c_str(), dst_size);
}

enum WebUiCommandType : uint8_t { WEB_UI_KEY=1, WEB_UI_CONTROL, WEB_UI_ENTER, WEB_UI_SET };
struct WebUiCommand {
  uint8_t type;
  int16_t value;
  int8_t index;
  char name[12];
  // input0 is also used for Sent Exch, which is longer than recv_exch.
  char input0[LEN_SENT_EXCH_WINDOW + 1];
  char input1[LEN_EXCH_WINDOW + 1];
};
static QueueHandle_t s_web_ui_queue = nullptr;

static bool enqueue_web_ui(const WebUiCommand &cmd) {
  if (!s_web_ui_queue) s_web_ui_queue = xQueueCreate(12, sizeof(WebUiCommand));
  return s_web_ui_queue && xQueueSend(s_web_ui_queue, &cmd, 0) == pdTRUE;
}
}

void process_web_ui_queue() {
  if (!s_web_ui_queue) return;
  WebUiCommand cmd;
  int budget=8;
  while (budget-- > 0 && xQueueReceive(s_web_ui_queue, &cmd, 0) == pdTRUE) {
    struct radio *radio = so2r.radio_selected();
    if (cmd.type == WEB_UI_KEY) {
      if (cmd.value >= 112 && cmd.value <= 116) {
        so2r.cancel_msg_tx();
        so2r.set_msg_tx_to_focused();
        so2r.set_rx_in_sending_msg();
        function_keys(cmd.value - 54, 0);
      } else if (cmd.value == 27) {
        so2r.cancel_msg_tx();
        switch (so2r.radio_mode) {
          case SO2R::RADIO_MODE_SO2R: so2r.sequence_mode(SO2R::SO2R_CQSandP); break;
          case SO2R::RADIO_MODE_SO1R:
          case SO2R::RADIO_MODE_SAT: so2r.sequence_mode(SO2R::Manual); break;
        }
        so2r.sequence_stat(SO2R::Default);
      }
    } else if (cmd.type == WEB_UI_CONTROL) {
      if (!strcmp(cmd.name,"Radio")) {
        so2r.change_focused_radio(cmd.value);
      } else if (!strcmp(cmd.name,"Mode")) {
        radio=so2r.radio_selected();
        int mt=modetype_string(cmd.input0);
        int filt=radio->filtbank[radio->bandid][radio->cq[mt]][mt];
        if (!filt) filt=default_filt(cmd.input0);
        set_mode(cmd.input0,filt,radio);
        send_mode_set_civ(cmd.input0,filt);
        upd_display();
      } else if (!strcmp(cmd.name,"Band")) {
        radio=so2r.radio_selected();
        if (cmd.value>=1 && cmd.value<N_BAND && (((1<<(cmd.value-1)) & radio->band_mask)==0))
          band_change(cmd.value,radio);
      }
    } else if (cmd.type == WEB_UI_ENTER) {
      radio=so2r.radio_selected();
      strlcpy(radio->callsign+2,cmd.input0,LEN_CALL_WINDOW + 1);
      strlcpy(radio->recv_exch+2,cmd.input1,LEN_EXCH_WINDOW + 1);
      if (cmd.index == 0) {
        // Use the normal Call-window response path so the F5 transmission,
        // sent-callsign record, and QSO state are updated together.
        so2r.send_call_exch();
        upd_display();
      } else {
        radio->ptr_curr = 1;
        process_enter(0);
      }
    } else if (cmd.type == WEB_UI_SET) {
      switch (cmd.index) {
        case 0: strlcpy(radio->callsign+2, cmd.input0, LEN_CALL_WINDOW + 1); break;
        case 1: strlcpy(radio->recv_exch+2, cmd.input0, LEN_EXCH_WINDOW + 1); break;
        case 2: strlcpy(radio->recv_rst+2, cmd.input0, LEN_RST_WINDOW + 1); break;
        case 3: strlcpy(radio->sent_rst+2, cmd.input0, LEN_RST_WINDOW + 1); break;
        case 4:
          // Keep the unexpanded macro text (for example, "11$P") in the
          // runtime setting.  expand_sent_exch() expands it only for output.
          strlcpy(plogw->sent_exch + 2, cmd.input0, LEN_SENT_EXCH_WINDOW + 1);
          plogw->sent_exch[1] = strlen(plogw->sent_exch + 2);
          // Keep the currently selected contest preset in sync with /op.
          save_contest_runtime_preset(plogw->contest_name + 2);
          break;
        case 5: strlcpy(plogw->my_callsign+2, cmd.input0, LEN_CALL_WINDOW + 1); break;
      }
      upd_display();
    }
  }
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

// Format a byte count without allocating Arduino String objects.
static void humanReadableSizeToBuffer(size_t bytes, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  if (bytes < 1024) {
    snprintf(out, out_size, "%u B", (unsigned)bytes);
  } else if (bytes < (1024UL * 1024UL)) {
    snprintf(out, out_size, "%.1f KB", (double)bytes / 1024.0);
  } else if (bytes < (1024UL * 1024UL * 1024UL)) {
    snprintf(out, out_size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
  } else {
    snprintf(out, out_size, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
  }
}

// Stream the SD-card directory as HTML.  Only one table row is retained in
// RAM, rather than constructing the complete file list in a String.
static void setupSdFileListHandler() {
  web_server.on("/filelist", HTTP_GET, [](AsyncWebServerRequest *request) {
    struct FileListState {
      enum Stage : uint8_t { Header, OpenEntry, Row, Footer, Done } stage = Header;
      File root;
      File entry;
      size_t offset = 0;
      size_t length = 0;
      char text[320];
    };

    std::shared_ptr<FileListState> state = std::make_shared<FileListState>();
    if (!state) {
      request->send(503, "text/plain", "Not enough memory for SD file list");
      return;
    }

    state->root = SD.open("/");
    if (!state->root) {
      request->send(500, "text/plain", "Failed to open SD card root");
      return;
    }

    webLog.println("Listing files stored on SD card (streamed)");

    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/html; charset=utf-8",
      [state](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {
        (void)index;
        size_t written = 0;

        auto copy_pending = [&]() -> bool {
          if (state->offset >= state->length) return true;
          const size_t remain = state->length - state->offset;
          const size_t available = maxLen - written;
          const size_t ncopy = remain < available ? remain : available;
          if (ncopy) {
            memcpy(buffer + written, state->text + state->offset, ncopy);
            state->offset += ncopy;
            written += ncopy;
          }
          return state->offset >= state->length;
        };

        auto set_text = [&](const char *text) {
          strlcpy(state->text, text, sizeof(state->text));
          state->length = strlen(state->text);
          state->offset = 0;
        };

        while (written < maxLen && state->stage != FileListState::Done) {
          switch (state->stage) {
          case FileListState::Header:
            if (state->length == 0) {
              set_text("<table><tr><th align='left'>Name</th>"
                       "<th align='left'>Size</th></tr>");
            }
            if (copy_pending()) {
              state->length = 0;
              state->stage = FileListState::OpenEntry;
            }
            break;

          case FileListState::OpenEntry:
            state->entry = state->root.openNextFile();
            if (!state->entry) {
              state->root.close();
              state->stage = FileListState::Footer;
              break;
            }
            {
              char size_text[32];
              humanReadableSizeToBuffer(state->entry.size(), size_text, sizeof(size_text));
              snprintf(state->text, sizeof(state->text),
                       "<tr align='left'><td>%s</td><td>%s</td></tr>",
                       state->entry.name(), size_text);
              state->entry.close();
              state->length = strlen(state->text);
              state->offset = 0;
              state->stage = FileListState::Row;
            }
            break;

          case FileListState::Row:
            if (copy_pending()) {
              state->length = 0;
              state->stage = FileListState::OpenEntry;
            }
            break;

          case FileListState::Footer:
            if (state->length == 0) set_text("</table>");
            if (copy_pending()) {
              state->length = 0;
              state->stage = FileListState::Done;
            }
            break;

          case FileListState::Done:
            break;
          }
        }
        return written;
      });

    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
}

// Make size of files human readable.  Used only for the three small storage
// values inserted into the top page; the directory itself is streamed above.
String humanReadableSize(const size_t bytes) {
  char text[32];
  humanReadableSizeToBuffer(bytes, text, sizeof(text));
  return String(text);
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
  case 7:return Nospace; // Cluster Name
  case 8:return Allowall; // Cluster Cmd
  case 20:return Nospace; // Cluster2 Name
  case 21:return Allowall; // Cluster2 Cmd
  case 22:
  case 23:
  case 24:
  case 25:
  case 26:return Allowall; // Cluster2 startup commands
  case 5:return Nospace; // "Wifi_SSID";
  case 6:return Nospace; // "Wifi_Passwd";
  default : return Allowall;
  }
}  

const int N_EDITWIN=27;
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
  case 20:return "Cluster2 Name";
  case 21:return "Cluster2 Cmd";
  case 22:return "Cluster2 Startup Command 1";
  case 23:return "Cluster2 Startup Command 2";
  case 24:return "Cluster2 Startup Command 3";
  case 25:return "Cluster2 Startup Command 4";
  case 26:return "Cluster2 Startup Command 5";
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
  case 20:return plogw->cluster2_name;
  case 21:return plogw->cluster2_cmd;
  case 22:return cluster2_startup_cmd[0];
  case 23:return cluster2_startup_cmd[1];
  case 24:return cluster2_startup_cmd[2];
  case 25:return cluster2_startup_cmd[3];
  case 26:return cluster2_startup_cmd[4];
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
  <li><strong>TP:<em>cat_type</em>_<em>rig_type</em></strong><br>
    cat_type: 0 ICOM CI-V, 1 Yaesu(New), 2 Kenwood, 3 Manual(NoCAT), 4 Yaesu(Old), 5 Elecraft KX, 6 Yaesu FT-817<br>
    rig_type: 0 IC-705, 1 IC-9700, 2 Yaesu, 3 Kenwood, 4 Manual, 5 IC-7300, 6 Elecraft KX, 7 Xiegu X6100
  </li>
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
<title>DVPlogger コンテスト設定</title>
<style>
body{font-family:sans-serif;margin:18px;max-width:1250px}
table{border-collapse:collapse;width:100%;margin:12px 0 24px}
th,td{border:1px solid #aaa;padding:6px;text-align:left;vertical-align:middle}
tr.current{font-weight:bold;background:#e8f3ff}
input{box-sizing:border-box;padding:5px;font-size:.95em;width:100%;min-width:9em}
button{padding:5px 10px;white-space:nowrap}.dupe-ok{color:#075f16;font-weight:bold}.dupe-ng{color:#9b1c1c;font-weight:bold}
#status{min-height:1.4em;font-weight:bold}.note{font-size:.9em}.name{white-space:nowrap}
.contest-wrap{overflow-x:auto;width:100%}.contest-table{table-layout:fixed;min-width:1180px}
.contest-table .col-id{width:38px}.contest-table .col-name{width:220px}.contest-table .col-dupe{width:80px}
.contest-table .col-f1{width:175px}.contest-table .col-f2,.contest-table .col-f3{width:135px}
.contest-table .col-f5{width:160px}.contest-table .col-exch{width:145px}
.contest-name-field{display:flex;align-items:center;gap:8px;white-space:nowrap}.contest-name-field .name-text{flex:1;min-width:0}
.nav-links{display:flex;gap:1rem;align-items:center;margin:.2rem 0 1rem}.nav-links a{white-space:nowrap}
.user-table{table-layout:fixed;min-width:1200px}
.user-table .col-user-name{width:220px}.user-table .col-msg{width:175px}
.user-table .col-exch{width:150px}.user-table .col-dupe{width:110px}
.user-name-field{display:flex;align-items:center;gap:4px;white-space:nowrap}
.user-name-field input{min-width:0;flex:1}
.guide{max-width:1000px;border:1px solid #bbb;border-radius:8px;padding:12px 16px;margin:12px 0;background:#fafafa}.guide h3{margin:.4rem 0}.guide ul{margin:.5rem 0;padding-left:1.4rem}.warn{background:#fff4d6;border-left:5px solid #d29a00;padding:8px 12px;margin:10px 0}.help{max-width:900px}.help th:first-child,.help td:first-child{white-space:nowrap}.examples code{white-space:nowrap}
</style></head><body><h2>コンテスト設定</h2>
<nav class="nav-links"><a href="/">ホーム</a><a href="/op">運用画面</a><a href="/bandmap">バンドマップ</a><a href="/contests?lang=en">English</a></nav>
<p>現在選択中: <strong>%CURRENT_CONTEST%</strong></p>
<p class="note"><strong>%SD_STATUS%</strong><br>直前の処理: %LAST_STATUS%</p><p id="status"></p>
<div class="guide"><h3>このページの使い方</h3>
<ul><li>参加するコンテストの行で、必要に応じてCWメッセージと送出ナンバーを編集します。</li>
<li>コンテスト名の右にある「選択して保存」を押すと、その行の設定をSDカードの <code>/CONTEST.TXT</code> に保存し、直ちにそのコンテストへ切り替えます。</li>
<li>コンテストを切り替えると、選択したコンテスト名に対応する過去QSOだけを使ってデュープ・マルチと連番を再構築します。複数コンテストを往復して運用できます。</li>
<li><strong>Ctrl-2</strong>：登録されているコンテストを順番に切り替えます。</li>
<li><strong>Ctrl-Shift-2</strong>：現在のコンテストと直前に使用していたコンテストを交互に切り替えます。2つのコンテストを並行して運用するときに便利です。</li></ul>
<p class="note"><strong>Web画面とキーボードの設定は共通です。</strong>どちらから切り替えても、そのコンテスト用のCWメッセージ、送出ナンバー、連番が復元され、デュープ・マルチ情報が再構築されます。</p>
<div class="warn"><strong>コンテスト内／外の区別：</strong>通常QSOをコンテスト集計から除外したいときは、端末のコール欄に <code>OFFCONTEST</code> と入力します。再び現在のコンテストへ戻すときは <code>ONCONTEST</code> と入力します。OFFCONTEST中のQSOはRemarksに区別情報が記録され、コンテスト別のデュープ集計から除外されます。</div>
<p class="note"><strong>選び方の目安：</strong><code>NOMULTI</code> はマルチを使用しない一般QSO向けです。交換ナンバー形式が近い既定コンテストを選ぶか、一覧にない場合は下の「ユーザー定義コンテスト」を使用してください。</p></div>
<div class="contest-wrap"><table class="contest-table"><colgroup>
<col class="col-id"><col class="col-name"><col class="col-dupe">
<col class="col-f1"><col class="col-f2"><col class="col-f3"><col class="col-f5">
<col class="col-exch"></colgroup>
<thead><tr><th>ID</th><th>コンテスト／選択</th><th>デュープ規則</th><th>CW F1（CQ）</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>送出ナンバー</th></tr></thead><tbody>
)rawliteral";

static const char contests_page_footer[] PROGMEM = R"rawliteral(
</tbody></table></div><h3>ユーザー定義コンテスト（.MD）</h3>
<div class="guide"><p>CTESTWIN等のMD定義ファイルを本体トップページのFile Uploadから、8.3形式の <code>FILENAME.MD</code> としてSDカードへアップロードして使用します。</p>
<ul><li>ファイル名欄には <code>User</code> と <code>.MD</code> を除いた部分だけを入力します。例：<code>TOKYO.MD</code> なら <code>TOKYO</code>。</li>
<li>2行は独立した切替枠です。別々のMDファイルとCWメッセージを保存し、ボタン一つで切り替えられます。</li>
<li>MDファイルが見つからない場合でも、マルチなしのコンテストとして開始できます。デュープ規則は左のチェック欄で指定します。</li></ul></div>
<div class="contest-wrap"><table class="user-table"><colgroup>
<col class="col-user-name"><col class="col-dupe"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-exch">
</colgroup><thead><tr><th>MDファイル名／選択</th><th>CW／Phoneデュープ</th><th>CW F1（CQ）</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>送出ナンバー</th></tr></thead><tbody>
<tr%USER1_CLASS%>
<td><form id="user_contest_form_1" method="GET" action="/select_user_contest"><input type="hidden" name="lang" value="%LANG%"><input type="hidden" name="slot" value="0"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_1" name="filename" maxlength="8" value="%USER1_FILENAME%" placeholder="PRESET1" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"><button form="user_contest_form_1" type="submit">%USER1_ACTION%</button></div></td>
<td><label><input form="user_contest_form_1" type="checkbox" name="dupe_separate" value="1" %USER1_DUPE_CHECKED% style="width:auto;min-width:0"> CWとPhoneを別交信として許可</label></td>
<td><input form="user_contest_form_1" name="f1" maxlength="30" value="%USER1_F1%"></td><td><input form="user_contest_form_1" name="f2" maxlength="30" value="%USER1_F2%"></td><td><input form="user_contest_form_1" name="f3" maxlength="30" value="%USER1_F3%"></td><td><input form="user_contest_form_1" name="f5" maxlength="30" value="%USER1_F5%"></td><td><input form="user_contest_form_1" name="exch" maxlength="17" value="%USER1_EXCH%"></td></tr>
<tr%USER2_CLASS%>
<td><form id="user_contest_form_2" method="GET" action="/select_user_contest"><input type="hidden" name="lang" value="%LANG%"><input type="hidden" name="slot" value="1"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_2" name="filename" maxlength="8" value="%USER2_FILENAME%" placeholder="PRESET2" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"><button form="user_contest_form_2" type="submit">%USER2_ACTION%</button></div></td>
<td><label><input form="user_contest_form_2" type="checkbox" name="dupe_separate" value="1" %USER2_DUPE_CHECKED% style="width:auto;min-width:0"> CWとPhoneを別交信として許可</label></td>
<td><input form="user_contest_form_2" name="f1" maxlength="30" value="%USER2_F1%"></td><td><input form="user_contest_form_2" name="f2" maxlength="30" value="%USER2_F2%"></td><td><input form="user_contest_form_2" name="f3" maxlength="30" value="%USER2_F3%"></td><td><input form="user_contest_form_2" name="f5" maxlength="30" value="%USER2_F5%"></td><td><input form="user_contest_form_2" name="exch" maxlength="17" value="%USER2_EXCH%"></td></tr>
</tbody></table></div>
<p class="note"><code>/FILENAME.MD</code> がない場合はマルチチェックなしで開始しますが、デュープチェックは有効です。ファイル名に使用できる文字は A-Z、0-9、_、- です。</p>

<h3>CWメッセージのマクロ</h3>
<p class="note"><strong>送出ナンバー</strong>欄には、実際に送る値（例：<code>11</code>、<code>1115</code>）を入力します。CWメッセージ中の <code>$W</code> がこの値へ展開されます。数字の短縮送信はDVPlogger本体のCW数字短縮設定に従います。</p>
<table class="help"><thead><tr><th>マクロ</th><th>展開内容</th></tr></thead><tbody>
<tr><td><code>$I</code></td><td>自局コールサイン</td></tr>
<tr><td><code>$C</code></td><td>相手局コールサイン</td></tr>
<tr><td><code>$U</code></td><td>CW／Digital時の <code>CQ</code></td></tr>
<tr><td><code>$T</code></td><td>CW／Digital時の <code>TEST</code></td></tr>
<tr><td><code>$A</code></td><td>CW／Digital時の <code>TU</code></td></tr>
<tr><td><code>$V</code></td><td>送信RST。CWでは通常 <code>5NN</code> に短縮</td></tr>
<tr><td><code>$W</code></td><td>この表の「送出ナンバー」欄</td></tr>
<tr><td><code>$P</code></td><td>バンドごとの電力コード</td></tr>
<tr><td><code>$J</code></td><td>本体設定の自局JCC／JCG番号</td></tr>
<tr><td><code>$S</code></td><td>現在の通しQSO番号</td></tr>
<tr><td><code>$Q</code></td><td>バンド別の次の連番（3桁）</td></tr>
<tr><td><code>$N</code></td><td>オペレータ名</td></tr>
</tbody></table>

<h3>設定例</h3>
<table class="help examples"><thead><tr><th>キー</th><th>入力例</th><th>送信例</th></tr></thead><tbody>
<tr><td>F1</td><td><code>$U $T DE $I $I $T</code></td><td><code>CQ TEST DE JK1DVP JK1DVP TEST</code></td></tr>
<tr><td>F2</td><td><code>$C $V $W</code></td><td><code>JA1ABC 5NN 1115</code></td></tr>
<tr><td>F3</td><td><code>$A $I $T</code></td><td><code>TU JK1DVP TEST</code></td></tr>
<tr><td>F5</td><td><code>$C $V$W$P</code></td><td><code>JA1ABC 5NN1115M</code></td></tr>
</tbody></table>
<p class="note">メッセージ中の空白は語間として送信されます。<code>$V$W$P</code> のように、マクロを空白なしで連結することもできます。</p>
</body></html>
)rawliteral";


static const char contests_page_header_en[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVPlogger Contest Settings</title>
<style>
body{font-family:sans-serif;margin:18px;max-width:1250px}table{border-collapse:collapse;width:100%;margin:12px 0 24px}th,td{border:1px solid #aaa;padding:6px;text-align:left;vertical-align:middle}tr.current{font-weight:bold;background:#e8f3ff}input{box-sizing:border-box;padding:5px;font-size:.95em;width:100%;min-width:9em}button{padding:5px 10px;white-space:nowrap}.dupe-ok{color:#075f16;font-weight:bold}.dupe-ng{color:#9b1c1c;font-weight:bold}#status{min-height:1.4em;font-weight:bold}.note{font-size:.9em}.name{white-space:nowrap}.contest-wrap{overflow-x:auto;width:100%}.contest-table{table-layout:fixed;min-width:1180px}.contest-table .col-id{width:38px}.contest-table .col-name{width:220px}.contest-table .col-dupe{width:80px}.contest-table .col-f1{width:175px}.contest-table .col-f2,.contest-table .col-f3{width:135px}.contest-table .col-f5{width:160px}.contest-table .col-exch{width:145px}.contest-name-field{display:flex;align-items:center;gap:8px;white-space:nowrap}.contest-name-field .name-text{flex:1;min-width:0}.nav-links{display:flex;gap:1rem;align-items:center;margin:.2rem 0 1rem}.nav-links a{white-space:nowrap}.user-table{table-layout:fixed;min-width:1200px}.user-table .col-user-name{width:220px}.user-table .col-msg{width:175px}.user-table .col-exch{width:150px}.user-table .col-dupe{width:110px}.user-name-field{display:flex;align-items:center;gap:4px;white-space:nowrap}.user-name-field input{min-width:0;flex:1}.guide{max-width:1000px;border:1px solid #bbb;border-radius:8px;padding:12px 16px;margin:12px 0;background:#fafafa}.guide h3{margin:.4rem 0}.guide ul{margin:.5rem 0;padding-left:1.4rem}.warn{background:#fff4d6;border-left:5px solid #d29a00;padding:8px 12px;margin:10px 0}.help{max-width:900px}.help th:first-child,.help td:first-child{white-space:nowrap}.examples code{white-space:nowrap}
</style></head><body><h2>Contest Settings</h2>
<nav class="nav-links"><a href="/">Home</a><a href="/op">Operation</a><a href="/bandmap">Band map</a><a href="/contests?lang=ja">日本語</a></nav>
<p>Current contest: <strong>%CURRENT_CONTEST%</strong></p>
<p class="note"><strong>%SD_STATUS%</strong><br>Last action: %LAST_STATUS%</p><p id="status"></p>
<div class="guide"><h3>How to use this page</h3><ul>
<li>Edit the CW messages and sent exchange in the desired contest row when necessary.</li>
<li>Press “Select &amp; Save” to save the row to <code>/CONTEST.TXT</code> on the SD card and immediately switch contests.</li>
<li>When switching, DVPlogger rebuilds dupe, multiplier and serial-number information from QSOs tagged with that contest name. You can move back and forth between several contests.</li>
<li><strong>Ctrl-2</strong>: cycle through registered contests.</li>
<li><strong>Ctrl-Shift-2</strong>: toggle between the current and previously used contest; useful for operating two contests in parallel.</li></ul>
<p class="note"><strong>Web and keyboard settings are shared.</strong> Either method restores the contest-specific CW messages, sent exchange and serial number, then rebuilds dupe and multiplier information.</p>
<div class="warn"><strong>Contest / non-contest QSOs:</strong> Enter <code>OFFCONTEST</code> in the callsign field to exclude ordinary QSOs from contest scoring. Enter <code>ONCONTEST</code> to return to the active contest. OFFCONTEST QSOs are marked in Remarks and excluded from contest-specific dupe counting.</div>
<p class="note"><strong>Selection hint:</strong> <code>NOMULTI</code> is suitable for ordinary QSOs without multipliers. Select a built-in contest with a similar exchange, or use a User-defined contest below.</p></div>
<div class="contest-wrap"><table class="contest-table"><colgroup><col class="col-id"><col class="col-name"><col class="col-dupe"><col class="col-f1"><col class="col-f2"><col class="col-f3"><col class="col-f5"><col class="col-exch"></colgroup>
<thead><tr><th>ID</th><th>Contest / Select</th><th>Dupe rule</th><th>CW F1 (CQ)</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>Sent exchange</th></tr></thead><tbody>
)rawliteral";

static const char contests_page_footer_en[] PROGMEM = R"rawliteral(
</tbody></table></div><h3>User-defined contests (.MD)</h3>
<div class="guide"><p>Upload a CTESTWIN-compatible MD definition file from File Upload on the home page. Store it on the SD card as an 8.3 filename such as <code>FILENAME.MD</code>.</p><ul>
<li>Enter only the part between <code>User</code> and <code>.MD</code>. Example: enter <code>TOKYO</code> for <code>TOKYO.MD</code>.</li>
<li>The two rows are independent presets. Each can store a different MD file and CW messages for one-button switching.</li>
<li>If the MD file is missing, DVPlogger can still start the contest without multiplier checking. Set the dupe rule using the checkbox.</li></ul></div>
<div class="contest-wrap"><table class="user-table"><colgroup><col class="col-user-name"><col class="col-dupe"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-msg"><col class="col-exch"></colgroup>
<thead><tr><th>MD filename / Select</th><th>CW/Phone dupe</th><th>CW F1 (CQ)</th><th>CW F2</th><th>CW F3</th><th>CW F5</th><th>Sent exchange</th></tr></thead><tbody>
<tr%USER1_CLASS%><td><form id="user_contest_form_1" method="GET" action="/select_user_contest"><input type="hidden" name="lang" value="%LANG%"><input type="hidden" name="slot" value="0"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_1" name="filename" maxlength="8" value="%USER1_FILENAME%" placeholder="PRESET1" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"><button form="user_contest_form_1" type="submit">%USER1_ACTION%</button></div></td><td><label><input form="user_contest_form_1" type="checkbox" name="dupe_separate" value="1" %USER1_DUPE_CHECKED% style="width:auto;min-width:0"> Allow separate CW and Phone QSOs</label></td><td><input form="user_contest_form_1" name="f1" maxlength="30" value="%USER1_F1%"></td><td><input form="user_contest_form_1" name="f2" maxlength="30" value="%USER1_F2%"></td><td><input form="user_contest_form_1" name="f3" maxlength="30" value="%USER1_F3%"></td><td><input form="user_contest_form_1" name="f5" maxlength="30" value="%USER1_F5%"></td><td><input form="user_contest_form_1" name="exch" maxlength="17" value="%USER1_EXCH%"></td></tr>
<tr%USER2_CLASS%><td><form id="user_contest_form_2" method="GET" action="/select_user_contest"><input type="hidden" name="lang" value="%LANG%"><input type="hidden" name="slot" value="1"></form><div class="user-name-field"><span>User</span><input form="user_contest_form_2" name="filename" maxlength="8" value="%USER2_FILENAME%" placeholder="PRESET2" oninput="this.value=this.value.toUpperCase().replace(/[^A-Z0-9_-]/g,'')"><button form="user_contest_form_2" type="submit">%USER2_ACTION%</button></div></td><td><label><input form="user_contest_form_2" type="checkbox" name="dupe_separate" value="1" %USER2_DUPE_CHECKED% style="width:auto;min-width:0"> Allow separate CW and Phone QSOs</label></td><td><input form="user_contest_form_2" name="f1" maxlength="30" value="%USER2_F1%"></td><td><input form="user_contest_form_2" name="f2" maxlength="30" value="%USER2_F2%"></td><td><input form="user_contest_form_2" name="f3" maxlength="30" value="%USER2_F3%"></td><td><input form="user_contest_form_2" name="f5" maxlength="30" value="%USER2_F5%"></td><td><input form="user_contest_form_2" name="exch" maxlength="17" value="%USER2_EXCH%"></td></tr></tbody></table></div>
<p class="note">If <code>/FILENAME.MD</code> is not found, the contest starts without multiplier checking, while dupe checking remains enabled. Valid filename characters are A-Z, 0-9, _ and -.</p>
<h3>CW message macros</h3><p class="note">Enter the value actually sent (for example <code>11</code> or <code>1115</code>) in “Sent exchange”. <code>$W</code> expands to this value. Abbreviated CW numerals follow the DVPlogger CW-number setting.</p>
<table class="help"><thead><tr><th>Macro</th><th>Expansion</th></tr></thead><tbody>
<tr><td><code>$I</code></td><td>Your callsign</td></tr><tr><td><code>$C</code></td><td>Other station's callsign</td></tr><tr><td><code>$U</code></td><td><code>CQ</code> in CW/Digital modes</td></tr><tr><td><code>$T</code></td><td><code>TEST</code> in CW/Digital modes</td></tr><tr><td><code>$A</code></td><td><code>TU</code> in CW/Digital modes</td></tr><tr><td><code>$V</code></td><td>Sent RST; normally shortened to <code>5NN</code> in CW</td></tr><tr><td><code>$W</code></td><td>Sent exchange in this table</td></tr><tr><td><code>$P</code></td><td>Band-specific power code</td></tr><tr><td><code>$J</code></td><td>Your JCC/JCG number from settings</td></tr><tr><td><code>$S</code></td><td>Current overall QSO serial number</td></tr><tr><td><code>$Q</code></td><td>Next band-specific serial number (3 digits)</td></tr><tr><td><code>$N</code></td><td>Operator name</td></tr></tbody></table>
<h3>Examples</h3><table class="help examples"><thead><tr><th>Key</th><th>Input</th><th>Sent text</th></tr></thead><tbody><tr><td>F1</td><td><code>$U $T DE $I $I $T</code></td><td><code>CQ TEST DE JK1DVP JK1DVP TEST</code></td></tr><tr><td>F2</td><td><code>$C $V $W</code></td><td><code>JA1ABC 5NN 1115</code></td></tr><tr><td>F3</td><td><code>$A $I $T</code></td><td><code>TU JK1DVP TEST</code></td></tr><tr><td>F5</td><td><code>$C $V$W$P</code></td><td><code>JA1ABC 5NN1115M</code></td></tr></tbody></table>
<p class="note">Spaces in a message are sent as word spaces. Macros may also be concatenated without spaces, as in <code>$V$W$P</code>.</p></body></html>
)rawliteral";

struct ContestWebPreset {
  bool used;
  char name[LEN_CONTEST_NAME + 1];
  char f1[LEN_CWMSG_WINDOW + 1];
  char f2[LEN_CWMSG_WINDOW + 1];
  char f3[LEN_CWMSG_WINDOW + 1];
  char f5[LEN_CWMSG_WINDOW + 1];
  char exch[LEN_SENT_EXCH_WINDOW + 1];
  bool dupe_separate;
};

static ContestWebPreset contest_web_scratch;
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

static bool parse_contest_preset_line(const String &line, const char *wanted_name,
                                      ContestWebPreset *out) {
  int p1 = line.indexOf('\t');
  int p2 = p1 < 0 ? -1 : line.indexOf('\t', p1 + 1);
  int p3 = p2 < 0 ? -1 : line.indexOf('\t', p2 + 1);
  int p4 = p3 < 0 ? -1 : line.indexOf('\t', p3 + 1);
  int p5 = p4 < 0 ? -1 : line.indexOf('\t', p4 + 1);
  int p6 = p5 < 0 ? -1 : line.indexOf('\t', p5 + 1);
  if (p1 < 1 || p2 < 0 || p3 < 0) return false;
  String name = line.substring(0, p1);
  if (!name.equalsIgnoreCase(wanted_name)) return false;

  memset(out, 0, sizeof(*out));
  out->used = true;
  strlcpy(out->name, name.c_str(), sizeof(out->name));
  copy_web_value(out->f1, sizeof(out->f1), line.substring(p1 + 1, p2));
  if (p4 >= 0 && p5 >= 0) {
    copy_web_value(out->f2, sizeof(out->f2), line.substring(p2 + 1, p3));
    copy_web_value(out->f3, sizeof(out->f3), line.substring(p3 + 1, p4));
    copy_web_value(out->f5, sizeof(out->f5), line.substring(p4 + 1, p5));
    if (p6 >= 0) {
      copy_web_value(out->exch, sizeof(out->exch), line.substring(p5 + 1, p6));
      out->dupe_separate = line.substring(p6 + 1).toInt() != 0;
    } else {
      copy_web_value(out->exch, sizeof(out->exch), line.substring(p5 + 1));
    }
  } else {
    copy_web_value(out->f3, sizeof(out->f3), line.substring(p2 + 1, p3));
    copy_web_value(out->exch, sizeof(out->exch), line.substring(p3 + 1));
  }
  return true;
}

static ContestWebPreset *find_contest_web_preset(const char *name, bool create) {
  if (!name || !*name) return NULL;
  bool found = false;
  File f = SD.open(CONTEST_PRESET_FILE, FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      if (line.endsWith("\r")) line.remove(line.length() - 1);
      // Keep scanning: the last occurrence is the newest appended value.
      if (parse_contest_preset_line(line, name, &contest_web_scratch)) found = true;
    }
    f.close();
  }
  if (found) return &contest_web_scratch;
  if (!create) return NULL;
  memset(&contest_web_scratch, 0, sizeof(contest_web_scratch));
  contest_web_scratch.used = true;
  strlcpy(contest_web_scratch.name, name, sizeof(contest_web_scratch.name));
  return &contest_web_scratch;
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
  }
}

static void load_contest_web_presets() {
  if (contest_web_presets_loaded) return;
  contest_web_presets_loaded = true;
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
    for (int i = 0; i < N_USER_CONTEST_SLOTS; ++i) {
      String prefix = String("#USER_SLOT") + String(i + 1) + "=";
      if (!line.startsWith(prefix)) continue;
      String filename = line.substring(prefix.length());
      filename.trim(); filename.toUpperCase();
      if (valid_web_user_md_basename(filename))
        strlcpy(contest_web_user_slot[i], filename.c_str(), sizeof(contest_web_user_slot[i]));
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
  FILE *fp = fopen(CONTEST_PRESET_VFS_FILE, "a");
  if (!fp) {
    set_contest_web_status(String("save failed: fopen(") + CONTEST_PRESET_VFS_FILE + ",a) failed, errno=" + String(errno));
    return false;
  }
  int n = fprintf(fp, "#USER_SLOT1=%s\n#USER_SLOT2=%s\n",
                  contest_web_user_slot[0], contest_web_user_slot[1]);
  if (n >= 0 && contest_web_scratch.used) {
    n = fprintf(fp, "%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
                contest_web_scratch.name, contest_web_scratch.f1,
                contest_web_scratch.f2, contest_web_scratch.f3,
                contest_web_scratch.f5, contest_web_scratch.exch,
                contest_web_scratch.dupe_separate ? 1 : 0);
  }
  const bool write_failed = n < 0;
  const bool flush_failed = fflush(fp) != 0;
  const bool close_failed = fclose(fp) != 0;
  if (write_failed || flush_failed || close_failed) {
    set_contest_web_status(String("save failed while appending ") + CONTEST_PRESET_FILE + ", errno=" + String(errno));
    return false;
  }
  File verify = SD.open(CONTEST_PRESET_FILE, FILE_READ);
  if (!verify) {
    set_contest_web_status(String("save failed: cannot reopen ") + CONTEST_PRESET_FILE);
    return false;
  }
  contest_web_file_size = verify.size();
  verify.close();
  contest_web_file_loaded = true;
  set_contest_web_status(String("saved ") + CONTEST_PRESET_FILE + " (" + String(contest_web_file_size) + " bytes)");
  return true;
}

bool save_contest_runtime_preset(const char *contest_name) {
  if (!contest_name || !*contest_name || !plogw) return false;
  load_contest_web_presets();
  ContestWebPreset *p = find_contest_web_preset(contest_name, true);
  if (!p) return false;

  copy_web_value(p->f1, sizeof(p->f1), String(plogw->cw_msg[0] + 2));
  copy_web_value(p->f2, sizeof(p->f2), String(plogw->cw_msg[1] + 2));
  copy_web_value(p->f3, sizeof(p->f3), String(plogw->cw_msg[2] + 2));
  copy_web_value(p->f5, sizeof(p->f5), String(plogw->cw_msg[4] + 2));
  copy_web_value(p->exch, sizeof(p->exch), String(plogw->sent_exch + 2));
  p->dupe_separate = plogw->mask == CW_PH_DUPE_OK;
  return save_contest_web_presets();
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

bool apply_contest_runtime_preset(const char *contest_name) {
  if (!contest_name || !*contest_name || !plogw) return false;
  load_contest_web_presets();
  ContestWebPreset *p = find_contest_web_preset(contest_name, false);
  if (!p) return false;
  set_current_contest_messages(*p);
  return true;
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
  AsyncWebParameter *dupe_separate = contest_request_param(request, "dupe_separate");
  if (!f1 || !f2 || !f3 || !f5 || !exch) return false;
  ContestWebPreset *p = find_contest_web_preset(name, true);
  if (!p) return false;
  copy_web_value(p->f1, sizeof(p->f1), f1->value());
  copy_web_value(p->f2, sizeof(p->f2), f2->value());
  copy_web_value(p->f3, sizeof(p->f3), f3->value());
  copy_web_value(p->f5, sizeof(p->f5), f5->value());
  copy_web_value(p->exch, sizeof(p->exch), exch->value());
  p->dupe_separate = dupe_separate && dupe_separate->value() == "1";
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
    const bool japanese = request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja");
    struct State { enum Stage:uint8_t {Header,Entry,Footer,Done} stage=Header; size_t offset=0,length=0; int index=0; bool japanese=false; char text[6144]; };
    std::shared_ptr<State> state = std::make_shared<State>();
    if (state) state->japanese = japanese;
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
            text.replace("%LANG%", state->japanese ? "ja" : "en");
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
              text.replace(tag + "_DUPE_CHECKED%", (p && p->dupe_separate) ? "checked" : "");
              text.replace(tag + "_ACTION%", state->japanese ? (current ? "保存して再選択" : "選択して保存") : (current ? "Save & re-select" : "Select & save"));
            }
          }
          text.toCharArray(state->text,sizeof(state->text)); state->length=strnlen(state->text,sizeof(state->text)); state->offset=0;
        };
        auto copy=[&]()->bool { size_t remain=state->length-state->offset,room=maxLen-written,n=remain<room?remain:room; if(n){memcpy(buffer+written,state->text+state->offset,n);written+=n;state->offset+=n;} if(state->offset==state->length){state->offset=0;state->length=0;return true;} return false; };
        while(written<maxLen && state->stage!=State::Done){
          switch(state->stage){
          case State::Header:
            if (!state->length) prepare(state->japanese ? contests_page_header : contests_page_header_en, false);
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
              String action_label = state->japanese ? (current ? "保存して再選択" : "選択して保存") : (current ? "Save & re-select" : "Select & save");
              String row=String("<tr")+(current?" class=\"current\"":"")+"><td><form id=\""+form_id+"\" method=\"GET\" action=\"/select_contest\"><input type=\"hidden\" name=\"lang\" value=\""+(state->japanese?"ja":"en")+"\"><input type=\"hidden\" name=\"id\" value=\""+String(id)+"\"></form>"+String(id)+"</td><td class=\"name\"><div class=\"contest-name-field\"><span class=\"name-text\">"+html_attr_escape(name)+"</span><button form=\""+form_id+"\" type=\"submit\">"+action_label+"</button></div></td><td class=\""+(dupe_ok?"dupe-ok":"dupe-ng")+"\">"+(state->japanese ? (dupe_ok?"CW/Phone別":"モード共通") : (dupe_ok?"CW/Phone separate":"All modes"))+"</td>";
              row += "<td><input form=\""+form_id+"\" name=\"f1\" maxlength=\"30\" value=\""+html_attr_escape(f1)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"f2\" maxlength=\"30\" value=\""+html_attr_escape(f2)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"f3\" maxlength=\"30\" value=\""+html_attr_escape(f3)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"f5\" maxlength=\"30\" value=\""+html_attr_escape(f5)+"\"></td>";
              row += "<td><input form=\""+form_id+"\" name=\"exch\" maxlength=\"17\" value=\""+html_attr_escape(ex)+"\"></td>";
              row += "</tr>\n";
              row.toCharArray(state->text,sizeof(state->text)); state->length=strnlen(state->text,sizeof(state->text)); state->offset=0;
            }
            if (copy()) ++state->index;
            break;
          case State::Footer:
            if (!state->length) prepare(state->japanese ? contests_page_footer : contests_page_footer_en, true);
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
    request->redirect(request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja") ? "/contests?lang=ja" : "/contests?lang=en");
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
    set_user_md_fallback_dupe_mask(p->dupe_separate ? CW_PH_DUPE_OK : CW_PH_DUPE_NG);
    if(!start_user_md_contest(plogw->contest_name+2)){set_contest_web_status(String("saved preset, but failed to start loading /")+filename+".MD");request->send(400,"text/plain",contest_web_last_status);return;}
    set_contest_web_status(String("saved preset and started loading /")+filename+".MD, F2=\""+(plogw->cw_msg[1]+2)+"\", EXCH=\""+(plogw->sent_exch+2)+"\"");
    request->redirect(request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja") ? "/contests?lang=ja" : "/contests?lang=en");
  });
}

//AsyncWebServer web_server(80);

void setupSettingsPageHandler() {
  web_server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = settings_page_html;  // テンプレートを複製
    String inputs;

    // Keep the stored setting indexes unchanged, but present Cluster2
    // immediately after the primary Cluster settings on the Web page.
    static const uint8_t display_order[] = {
      0, 1, 2, 3, 4, 5, 6,
      7, 8, 20, 21, 22, 23, 24, 25, 26,
      9, 10, 11, 12,
      13, 14, 15, 16, 17, 18, 19
    };
    for (size_t pos = 0; pos < sizeof(display_order); ++pos) {
      const int i = display_order[pos];
      if (i >= N_EDITWIN || pwin_index(i) == NULL) continue;
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
<p><a href="/potahelp?lang=ja">POTA helper (jp)</a> <a href="/potahelp?lang=en">(en)</a> Nearest-park search / ADIF export</p>
<p><a href="/sotahelp?lang=ja">SOTA helper (jp)</a> <a href="/sotahelp?lang=en">(en)</a> Nearest-summit search / ADIF export</p>
<p><a href="/settings">/settings</a> View/Edit Logger Settings</p>
<p><a href="/status">/status</a> DVPlogger Status</p>
<p><a href="/contests?lang=ja">Contest settings (jp)</a> <a href="/contests?lang=en">(en)</a></p>
<p><a href="/rigs">/rigs</a> View/Edit RIG Settings</p>
<p><a href="/bandmap">/bandmap</a> Multi-band Bandmap</p>
<p><a href="https://github.com/JK1DVP/dvplogger/blob/main/DVPlogger_manual_260718.pdf">Manual DVPlogger_manual_260718.pdf</a></p>
<p><a href="/op">/op</a> Web Opeartion Window</p>

  <p><h1>File Upload</h1></p>
  <p>SD Free: %FREESPIFFS% | SD Used: %USEDSPIFFS% | SD Total: %TOTALSPIFFS%</p>
  <form method="POST" action="/upload" enctype="multipart/form-data"><input type="file" name="data"/><input type="submit" name="upload" value="Upload" title="Upload File"></form>
<p>パーシャルチェックのファイルはname.pck (8.3形式)でアップロードしてください。</p>
<p>CALLHISTnameとコマンドを入力すると、name.pckを読み込みます。</p>
  <p>ファイルアップロードの開始終了は表示されませんので、ファイルリスト更新までお待ちください。</p>
<p>After clicking upload it will take some time for the file to firstly upload and then be written to the SD card, there is no indicator that the upload began.  Please be patient.</p>
  <p>Once uploaded the page will refresh and the newly uploaded file will appear in the file list.</p>
  <p>If a file does not appear, it will be because the file was too big, or had unusual characters in the file name (like spaces).</p>
  <p>You can see the progress of the upload by watching the serial output.</p>
  <div id="filelist">Loading SD file list...</div>
<script>
fetch('/filelist', {cache:'no-store'})
  .then(function(r){ if(!r.ok) throw new Error('HTTP '+r.status); return r.text(); })
  .then(function(html){ document.getElementById('filelist').innerHTML=html; })
  .catch(function(e){ document.getElementById('filelist').textContent='SD file list error: '+e; });
</script>
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


const char antenna_page_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVPlogger Antenna Settings</title>
<style>body{font-family:sans-serif;margin:16px}table{border-collapse:collapse}th,td{border:1px solid #aaa;padding:5px}input{font-size:15px} .wide{width:360px;max-width:80vw}</style>
</head><body><h2>Antenna Settings</h2>
<p>DVPlogger allocates antennas in radio-number order. Each preference string follows the DVPlogger band order: 1.8, 3.5, 7, 14, 21, 28, 50, 144, 430, 1200, 2400, 5600, 10G, 10, 18, 24 MHz. 0 means no antenna.</p>
<form id="f">
<table><tr><th>Enable</th><td><input id="enable" type="checkbox"></td></tr>
<tr><th>OTRSP host</th><td><input id="host" class="wide"></td></tr><tr><th>Port</th><td><input id="port" type="number"></td></tr>
<tr><th>1st preference</th><td><input id="pref1" class="wide" maxlength="16"></td></tr>
<tr><th>2nd preference</th><td><input id="pref2" class="wide" maxlength="16"></td></tr>
<tr><th>3rd preference</th><td><input id="pref3" class="wide" maxlength="16"></td></tr></table>
<h3>Antenna names</h3><table><thead><tr><th>ID</th><th>Name</th></tr></thead><tbody id="names"></tbody></table>
<p><button type="button" onclick="saveConfig()">Save</button> <span id="msg"></span></p></form>
<p><a href="/op">Back to Operation</a></p>
<script>
async function loadConfig(){const r=await fetch('/antenna_config');const d=await r.json();enable.checked=d.enable;host.value=d.host;port.value=d.port;pref1.value=d.pref[0];pref2.value=d.pref[1];pref3.value=d.pref[2];names.innerHTML=d.names.map((n,i)=>`<tr><td>${i+1}</td><td><input id="name${i+1}" class="wide" maxlength="23" value="${n.replace(/&/g,'&amp;').replace(/"/g,'&quot;')}"></td></tr>`).join('');}
async function saveConfig(){const q=new URLSearchParams();q.set('enable',enable.checked?'1':'0');q.set('host',host.value);q.set('port',port.value);q.set('pref1',pref1.value);q.set('pref2',pref2.value);q.set('pref3',pref3.value);for(let i=1;i<=9;i++)q.set('name'+i,document.getElementById('name'+i).value);const r=await fetch('/antenna_config?'+q.toString(),{method:'POST'});msg.textContent=await r.text();}
loadConfig();
</script></body></html>
)rawliteral";

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
  
  .ant-section { padding: 8px; }
  .ant-section table { border-collapse: collapse; width: 100%; max-width: 900px; }
  .ant-section th, .ant-section td { border: 1px solid #aaa; padding: 5px 7px; text-align: left; }
  .ant-ready { background: #d9f2d9; }
  .ant-waiting { background: #fff2b3; }
  .ant-tx, .ant-disconnected { background: #f7cccc; }
  .ant-disabled { background: #eeeeee; }
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
<div class="ant-section">
  <h4>Antenna</h4>
  <table>
    <tbody>
      <tr><th>Controller</th><td id="antController">-</td><th>Connection</th><td id="antConnection">-</td></tr>
      <tr><th>Host</th><td id="antHost">-</td><th>State</th><td id="antState">-</td></tr>
      <tr><th>Reason</th><td id="antReason" colspan="3">-</td></tr>
    </tbody>
  </table>
  <table style="margin-top:6px">
    <thead><tr><th>Radio</th><th>Band</th><th>Current</th><th>Requested</th><th>Pref</th><th>Status</th></tr></thead>
    <tbody id="antRadioRows"><tr><td colspan="6">Loading...</td></tr></tbody>
  </table>
  <p><a href="/antenna">Antenna settings</a></p>
</div>
    <p><a href="/bandmap">Open Bandmap</a> | <a href="/contests">Contest settings</a> | <a href="/">go back to Home</a></p>

  </form>

<script>
function normalizeOpValue(index, value) {
  let v = String(value || '');
  if ([0,1,4,5,10,11,12].includes(index)) v = v.toUpperCase();
  if ([0,1].includes(index)) return v.replace(/[^A-Z0-9\/.]/g, '');
  if ([2,3].includes(index)) return v.replace(/[^0-9]/g, '');
  // Sent Exch (index 4) accepts the same visible ASCII macro characters
  // as CW message fields, including '$', '#', '%', '&', '*', '+', etc.
  if ([4,5,10,11,12,13].includes(index)) return v.replace(/[^\x21-\x7E]/g, '');
  return v.replace(/[^\x20-\x7E]/g, '');
}
function normalizeOpInput(input) {
  const index = Number(input.dataset.index);
  const normalized = normalizeOpValue(index, input.value);
  if (input.value !== normalized) input.value = normalized;
  return normalized;
}

document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('input[data-index]').forEach(input => {
    input.addEventListener('input', () => normalizeOpInput(input));
    input.addEventListener('change', () => normalizeOpInput(input));
  });
});

function setSelectedButton(prefix, value) {
  const selectedId = `${prefix}_${value}`;
  document.querySelectorAll(`[id^="${prefix}_"]`).forEach(el => {
    el.style.backgroundColor = (el.id === selectedId) ? "green" : "gray";
  });
}

async function sendControl(type, value, buttonPrefix) {
  requestFastOpPolling();
  // Reflect the user's operation immediately.  The periodic status poll later
  // corrects the display if the command is rejected or the rig reports a
  // different state.
  if (buttonPrefix) setSelectedButton(buttonPrefix, value);
  try {
    const response = await fetch(`/control?type=${encodeURIComponent(type)}&value=${encodeURIComponent(value)}`,
                                 { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    // Do not wait for the normal long polling cycle.  Give the main loop a
    // short opportunity to consume the queue, then reconcile this button.
    setTimeout(() => {
      const index = type === 'Radio' ? 7 : (type === 'Mode' ? 8 : 9);
      fetchStatus_button(index, buttonPrefix);
      fetchStatus(99, 'radioDisplay');
    }, 120);
  } catch (error) {
    console.error(`Control ${type} failed:`, error);
    // Restore the authoritative state promptly after an enqueue/network error.
    const index = type === 'Radio' ? 7 : (type === 'Mode' ? 8 : 9);
    fetchStatus_button(index, buttonPrefix);
  }
}

function selectRadio(name) {
  sendControl('Radio', name, 'b_radio');
}
function selectMode(name) {
  sendControl('Mode', name, 'b_mode');
}
function selectBand(name) {
  sendControl('Band', name, 'b_band');
}

// /opの主要状態を1回のHTTP要求で取得する。
let forceOpInputSyncUntil = 0;
let qsoFieldSyncing = [false, false];
let suppressQsoBlurOnce = [false, false];
let lastSyncedQsoValue = ['', ''];
let opStatusFetching = false;
let opFastPollUntil = 0;
let opPollTimer = null;
const OP_DEBUG = false;

function opDebug(...args) {
  if (OP_DEBUG) console.log(...args);
}

function setTextIfChanged(id, value) {
  const el = document.getElementById(id);
  if (el && el.textContent !== value) el.textContent = value;
}

function requestFastOpPolling(durationMs = 1500) {
  opFastPollUntil = Math.max(opFastPollUntil, Date.now() + durationMs);
}

function selectStatusButton(prefix, value) {
  const selectedId = `${prefix}_${value}`;
  document.querySelectorAll(`[id^="${prefix}_"]`).forEach(el => {
    const color = el.id === selectedId ? "green" : "gray";
    if (el.style.backgroundColor !== color) el.style.backgroundColor = color;
  });
}

async function fetchOpStatus() {
  if (opStatusFetching) return;
  opStatusFetching = true;
  try {
    const response = await fetch('/op_status', {cache: 'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const fields = (await response.text()).split('\t');
    if (fields.length < 16) return;
    const force = Date.now() < forceOpInputSyncUntil;
    const active = document.activeElement;
    const inputIndexes = [0,1,2,3,4,5,10,11,12,13];
    inputIndexes.forEach((idx, pos) => {
      const el = document.getElementById(`edit_${idx}`);
      const syncing = (idx === 0 || idx === 1) && qsoFieldSyncing[idx];
      if (idx === 0 || idx === 1) lastSyncedQsoValue[idx] = fields[pos];
      if (el && !syncing && (force || active !== el) && el.value !== fields[pos]) {
        el.value = fields[pos];
      }
    });
    setTextIfChanged('radioDisplay', fields[10]);
    setTextIfChanged('cwkeyingDisplay', fields[11]);
    selectStatusButton('b_radio', fields[12]);
    selectStatusButton('b_mode', fields[13]);
    selectStatusButton('b_band', fields[14]);
  } catch (error) {
    console.error('op status fetch failed:', error);
  } finally {
    opStatusFetching = false;
  }
}

function scheduleNextOpStatusPoll(delayMs) {
  if (opPollTimer !== null) clearTimeout(opPollTimer);
  opPollTimer = setTimeout(pollOpStatus, delayMs);
}

async function pollOpStatus() {
  await fetchOpStatus();
  const interval = document.hidden ? 5000 : (Date.now() < opFastPollUntil ? 500 : 1000);
  scheduleNextOpStatusPoll(interval);
}

async function fetchAntennaStatus() {
  try {
    const response = await fetch('/antenna_status');
    const data = await response.json();
    document.getElementById('antController').textContent = data.controller;
    document.getElementById('antConnection').textContent = data.connection;
    document.getElementById('antHost').textContent = data.host;
    document.getElementById('antState').textContent = data.state + (data.pending ? ' (Pending)' : '');
    document.getElementById('antReason').textContent = data.reason;
    const rows = data.radios.map(r => {
      let pref = r.pref > 0 ? (r.pref === 1 ? '1st' : (r.pref === 2 ? '2nd' : '3rd')) : '-';
      if (r.blockedBy >= 0 && r.pref > 1) pref += ` (R${r.blockedBy} using earlier choice)`;
      const cls = 'ant-' + r.status.toLowerCase();
      const current = r.current > 0 ? `${r.current} / ${r.currentName}` : (r.current < 0 ? 'Unknown' : 'None');
      const requested = r.requested > 0 ? `${r.requested} / ${r.requestedName}` : 'None';
      return `<tr class="${cls}"><td>R${r.radio}</td><td>${r.band}</td><td>${current}</td><td>${requested}</td><td>${pref}</td><td>${r.status}</td></tr>`;
    }).join('');
    document.getElementById('antRadioRows').innerHTML = rows;
  } catch (error) {
    document.getElementById('antConnection').textContent = 'Status unavailable';
  }
}

pollOpStatus();
fetchAntennaStatus();
setInterval(fetchAntennaStatus, 3000);
document.addEventListener('visibilitychange', () => {
  scheduleNextOpStatusPoll(document.hidden ? 5000 : 0);
});

// F1〜F5ボタンを押したときにキーコードを送信
function sendKeyCode(keyCode) {
  requestFastOpPolling();
  opDebug(`Sending key code: ${keyCode}`);
  fetch(`/rig_key?keycode=${keyCode}`)
    .then(response => response.text())
    .then(data => opDebug("Sent key code:", keyCode, "Response:", data))
    .catch(error => console.error("Error sending keycode:", error));
}

// フォーカス移動と入力内容送信
function sendEnter(inputIndex) {

  // inputIndex = 0  Call 1 Exch 6 Radio0 name  7 Radio1 name 8 Radio2 name
  requestFastOpPolling();
  opDebug('sendEnter called',inputIndex);
  if (inputIndex == 0 || inputIndex == 1 ) {
    // 入力内容を送信
    const input1 = normalizeOpInput(document.getElementById('edit_0'));
    const input2 = normalizeOpInput(document.getElementById('edit_1'));

    fetch(`/rig_key?keycode=13&input0=${encodeURIComponent(input1)}&input1=${encodeURIComponent(input2)}&index=${inputIndex}`)
      .then(res => res.text())
      .then(msg => opDebug('→ rig_key response:', msg))
      .catch(err => console.error('fetch error:', err));
  } else {
    // set remote variable in general
    const value = normalizeOpInput(document.getElementById(`edit_${inputIndex}`));
    fetch(`/rig_key?command=set&index=${inputIndex}&value=${encodeURIComponent(value)}`)
      .then(res => res.text())
      .then(msg => opDebug('→ rig_key response to radio setting:', msg))
      .catch(err => console.error('fetch error:', err));
  }

  // フォーカスを次のinputに移動（ループ）
  if (inputIndex === 0) {  // callsign
    document.getElementById('edit_1').focus();
  } else if (inputIndex === 1) {  // recv exch
    document.getElementById('edit_0').focus();
  }
  if (inputIndex === 0 || inputIndex === 1) {
    forceOpInputSyncUntil = Date.now() + 1200;
    requestFastOpPolling();
    scheduleNextOpStatusPoll(100);
  }
}

function clearInputs() {
  document.getElementById('edit_0').value = '';  // callsignをクリア
  document.getElementById('edit_1').value = '';  // recv exchをクリア
  document.getElementById('edit_2').value = '';  // recv rstをクリア
}

async function syncQsoFieldWithoutEnter(index) {
  index = Number(index);
  if (index !== 0 && index !== 1) return;

  if (suppressQsoBlurOnce[index]) {
    suppressQsoBlurOnce[index] = false;
    return;
  }

  const input = document.getElementById(`edit_${index}`);
  if (!input) return;

  const value = normalizeOpInput(input);
  if (value === lastSyncedQsoValue[index]) return;

  qsoFieldSyncing[index] = true;
  requestFastOpPolling();
  try {
    const response = await fetch(
      `/rig_key?command=set&index=${index}&value=${encodeURIComponent(value)}`,
      { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    lastSyncedQsoValue[index] = value;
  } catch (error) {
    console.error(`QSO field ${index} blur sync failed:`, error);
  } finally {
    // The response means queued, not yet applied by the main loop.  Prevent
    // /op_status from restoring the old value during that short interval.
    setTimeout(() => {
      qsoFieldSyncing[index] = false;
      fetchOpStatus();
    }, 300);
  }
}

// keydownイベントの監視
document.addEventListener("DOMContentLoaded", () => {
  const callInput = document.getElementById('edit_0');
  const exchInput = document.getElementById('edit_1');
  if (callInput) callInput.addEventListener('blur', () => syncQsoFieldWithoutEnter(0));
  if (exchInput) exchInput.addEventListener('blur', () => syncQsoFieldWithoutEnter(1));

  document.addEventListener("keydown", event => {
    const key = event.key;
    const code = event.keyCode || event.which;
    const focused = document.activeElement;
    const idx = focused && focused.dataset ? focused.dataset.index || "" : "";

    opDebug('keydown idx=', idx, 'key=', key);

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

      // sendEnter() already commits both QSO fields and runs the normal
      // Call/EXCH Enter operation.  Suppress the blur-only update caused by
      // the focus move performed inside sendEnter().
      if (idx === "0" || idx === "1") suppressQsoBlurOnce[Number(idx)] = true;
      sendEnter(Number(idx));
      return;  // Enter is already queued by sendEnter(); do not send it twice.
    }

    // fetchでキー送信（Space、Tabも含む）
    fetch(`/rig_key?keycode=${code}${idx !== "" ? "&index=" + idx : ""}`)
      .then(res => res.text())
      .then(msg => opDebug('→ rig_key response:', msg))
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
  if (!lowmem_trace) return;
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
constexpr int WEB_BANDMAP_NO_PSRAM_MAX_ENTRIES = 32;
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
  WebBandmapEntry *entry;
  uint16_t capacity_per_band;
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
static uint8_t web_bandmap_snapshot_count = 0;
static bool web_bandmap_has_psram = false;
static bool web_bandmap_memory_mode_logged = false;
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

static WebBandmapEntry *web_bandmap_entry_at(WebBandmapSnapshot *snapshot,
                                                int band_index,
                                                int entry_index) {
  return snapshot->entry +
    static_cast<size_t>(band_index) * snapshot->capacity_per_band + entry_index;
}

static const WebBandmapEntry *web_bandmap_entry_at(
    const WebBandmapSnapshot *snapshot, int band_index, int entry_index) {
  return snapshot->entry +
    static_cast<size_t>(band_index) * snapshot->capacity_per_band + entry_index;
}

static void *web_bandmap_alloc(size_t size, bool prefer_psram) {
  if (prefer_psram) {
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  return heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
}

static void web_bandmap_heap_trace(const char *tag) {
  webLog.printf("[BANDMAPTRACE] web %-22s free=%u largest=%u min=%u snapshots=%u\n",
                tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)web_bandmap_snapshot_count);
}

static bool ensure_web_bandmap_snapshots() {
  web_bandmap_heap_trace("ensure enter");
  if (!web_bandmap_snapshot_mutex) {
    web_bandmap_snapshot_mutex = xSemaphoreCreateMutex();
    web_bandmap_heap_trace("after mutex create");
    if (!web_bandmap_snapshot_mutex) return false;
  }
  if (web_bandmap_snapshot_count != 0) {
    web_bandmap_heap_trace("ensure already ready");
    return true;
  }

  web_bandmap_has_psram = ESP.getPsramSize() > 0;
  const uint8_t requested_count = web_bandmap_has_psram ? 2 : 1;
  const uint16_t capacity = web_bandmap_has_psram
    ? WEB_BANDMAP_MAX_ENTRIES : WEB_BANDMAP_NO_PSRAM_MAX_ENTRIES;
  webLog.printf("[BANDMAPTRACE] layout snapshot=%u entry=%u bands=%u capacity=%u requested=%u entries_bytes=%u\n",
                (unsigned)sizeof(WebBandmapSnapshot),
                (unsigned)sizeof(WebBandmapEntry),
                (unsigned)WEB_BANDMAP_BANDS,
                (unsigned)capacity,
                (unsigned)requested_count,
                (unsigned)(sizeof(WebBandmapEntry) *
                  static_cast<size_t>(WEB_BANDMAP_BANDS) * capacity));

  for (uint8_t i = 0; i < requested_count; ++i) {
    WebBandmapSnapshot *snapshot = static_cast<WebBandmapSnapshot *>(
      web_bandmap_alloc(sizeof(WebBandmapSnapshot), web_bandmap_has_psram));
    web_bandmap_heap_trace("after snapshot alloc");
    if (!snapshot) break;

    const size_t entries_size = sizeof(WebBandmapEntry) *
      static_cast<size_t>(WEB_BANDMAP_BANDS) * capacity;
    snapshot->entry = static_cast<WebBandmapEntry *>(
      web_bandmap_alloc(entries_size, web_bandmap_has_psram));
    web_bandmap_heap_trace("after entries alloc");
    if (!snapshot->entry) {
      free(snapshot);
      break;
    }
    snapshot->capacity_per_band = capacity;
    web_bandmap_snapshots[i] = snapshot;
    ++web_bandmap_snapshot_count;
  }

  if (web_bandmap_snapshot_count != requested_count) {
    for (uint8_t i = 0; i < web_bandmap_snapshot_count; ++i) {
      free(web_bandmap_snapshots[i]->entry);
      free(web_bandmap_snapshots[i]);
      web_bandmap_snapshots[i] = nullptr;
    }
    web_bandmap_snapshot_count = 0;
    static uint32_t last_error_ms = 0;
    const uint32_t now = millis();
    if (now - last_error_ms >= 5000U) {
      last_error_ms = now;
      webLog.printf("bandmap: snapshot allocation failed psram=%u free_internal=%u largest_internal=%u\n",
                    web_bandmap_has_psram ? 1U : 0U,
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    }
    return false;
  }

  if (!web_bandmap_memory_mode_logged) {
    web_bandmap_memory_mode_logged = true;
    webLog.printf("bandmap: snapshots=%u entries_per_band=%u memory=%s\n",
                  web_bandmap_snapshot_count, capacity,
                  web_bandmap_has_psram ? "PSRAM" : "internal RAM");
  }
  web_bandmap_heap_trace("ensure success");
  return true;
}

static void rebuild_web_bandmap_snapshot() {
  const size_t rebuild_free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t rebuild_largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  web_bandmap_heap_trace("rebuild enter");
  if (!ensure_web_bandmap_snapshots()) {
    web_bandmap_heap_trace("rebuild ensure failed");
    return;
  }

  const bool single_snapshot = web_bandmap_snapshot_count == 1;
  if (single_snapshot &&
      xSemaphoreTake(web_bandmap_snapshot_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  // With PSRAM, build the inactive snapshot and atomically swap it.  Without
  // PSRAM, update one compact snapshot while holding the mutex.
  const uint8_t active = web_bandmap_active_snapshot;
  const uint8_t next = single_snapshot ? 0 : (active ^ 1U);
  WebBandmapSnapshot *snapshot = web_bandmap_snapshots[next];
  memset(snapshot->count, 0, sizeof(snapshot->count));
  memset(snapshot->band_generation, 0, sizeof(snapshot->band_generation));
  snapshot->generation = 0;
  snapshot->sort_type = bandmap_disp.sort_type;

  uint32_t hash = 2166136261UL;
  hash = web_bandmap_hash_mix(hash, snapshot->sort_type);

  for (int band_index = 0; band_index < WEB_BANDMAP_BANDS; ++band_index) {
    const int bandid = band_index + 1;
    uint16_t count = 0;

    for (int i = 0;
         i < bandmap[band_index].nentry && count < snapshot->capacity_per_band;
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

      WebBandmapEntry &dest = *web_bandmap_entry_at(snapshot, band_index, count++);
      dest.freq = source->freq;
      dest.time = source->time;
      strlcpy(dest.station, source->station, sizeof(dest.station));
      dest.mode = source->mode;
      dest.flag = source->flag;
    }

    snapshot->count[band_index] = count;
    WebBandmapEntry *band_entries = web_bandmap_entry_at(snapshot, band_index, 0);
    std::sort(band_entries,
              band_entries + count,
              [snapshot](const WebBandmapEntry &a, const WebBandmapEntry &b) {
                return web_bandmap_entry_less(a, b, snapshot->sort_type);
              });

    uint32_t band_hash = 2166136261UL;
    band_hash = web_bandmap_hash_mix(band_hash, snapshot->sort_type);
    band_hash = web_bandmap_hash_mix(band_hash, bandid);
    band_hash = web_bandmap_hash_mix(band_hash, count);
    for (uint16_t i = 0; i < count; ++i) {
      const WebBandmapEntry &entry = *web_bandmap_entry_at(snapshot, band_index, i);
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

  if (single_snapshot) {
    web_bandmap_active_snapshot = 0;
    web_bandmap_published_generation = hash;
    web_bandmap_snapshot_ready = true;
    xSemaphoreGive(web_bandmap_snapshot_mutex);
  } else if (xSemaphoreTake(web_bandmap_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
    web_bandmap_active_snapshot = next;
    web_bandmap_published_generation = hash;
    web_bandmap_snapshot_ready = true;
    xSemaphoreGive(web_bandmap_snapshot_mutex);
  }

  const size_t rebuild_free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t rebuild_largest_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  web_bandmap_heap_trace("rebuild leave");
  webLog.printf("[BANDMAPTRACE] rebuild delta free=%d largest=%d\n",
                (int)rebuild_free_after - (int)rebuild_free_before,
                (int)rebuild_largest_after - (int)rebuild_largest_before);
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
  // WORKED is monotonic during a contest.  Preserve it across refreshes;
  // a transient DUPE-query timeout must not expose the spot again.
  entry->flag &= ~BANDMAP_ENTRY_FLAG_NEWMULTI;

  char *exch_history = nullptr;
  const int bandmode = bandmode_param(bandid, modetype[entry->mode]);
  if (dupe_callhist_check(entry->station, bandmode, plogw->mask, 1,
                          &exch_history)) {
    entry->flag |= BANDMAP_ENTRY_FLAG_WORKED;
  }
  if (exch_history) {
    const int multi = multi_check(exch_history, bandid);
    if (multi >= 0 && !multi_worked_get(&multi_list, bandid - 1, multi)) {
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
h1{font-size:1.35rem;margin:.3rem 0 .35rem}.nav-links{display:flex;gap:1rem;align-items:center;margin:0 0 .8rem}.nav-links a{white-space:nowrap}.toolbar{display:flex;gap:.5rem;flex-wrap:wrap;margin-bottom:.7rem}
select,button{font-size:1rem;padding:.35rem}.maps{display:flex;gap:10px;overflow-x:auto;align-items:flex-start;padding-bottom:8px}
.band{flex:0 0 285px;background:#fff;border:1px solid #bbb;border-radius:5px;max-height:78vh;overflow-y:auto}
.band h2{position:sticky;top:0;background:#e8e8e8;margin:0;padding:.45rem;font-size:1.05rem;border-bottom:1px solid #bbb;z-index:1}
.spot{display:grid;grid-template-columns:78px 1fr 42px 34px 30px;gap:4px;padding:.28rem .4rem;border-bottom:1px solid #eee;cursor:pointer}
.spot:hover{background:#fff4c4}.spot.newmulti{background:#fff3a8;border-left:5px solid #d07800;padding-left:calc(.4rem - 5px)}
.spot.newmulti:hover{background:#ffe57a}.freq{font-family:monospace}.call{font-weight:bold}.multi{color:#a00018;font-weight:bold;text-align:center}.age{text-align:right;color:#555}.empty{padding:.8rem;color:#777}
#status{font-size:.85rem;color:#555;margin-left:.3rem}.more{border:0;background:transparent;padding:0;font-size:1.25rem;line-height:1;cursor:pointer}.menu{position:fixed;display:none;z-index:20;background:#fff;border:1px solid #999;border-radius:5px;box-shadow:0 3px 14px #5558;min-width:170px}.menu button{display:block;width:100%;border:0;background:#fff;text-align:left;padding:.65rem}.menu button:hover{background:#eee}.modal{display:none;position:fixed;inset:0;background:#0007;z-index:30;align-items:center;justify-content:center}.dialog{background:#fff;border-radius:7px;padding:1rem;min-width:min(310px,88vw);box-shadow:0 5px 20px #0008}.dialog h3{margin:.1rem 0 .8rem}.dialog input{box-sizing:border-box;width:100%;font-size:1.1rem;padding:.45rem;text-transform:uppercase}.actions{display:flex;justify-content:flex-end;gap:.6rem;margin-top:1rem}.danger{color:#a00018;font-weight:bold}
</style></head><body><h1>DVPlogger Bandmap</h1>
<nav class="nav-links"><a href="/">Home</a><a href="/contests">Contest</a></nav>
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
                       web_bandmap_has_psram
                         ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                         : MALLOC_CAP_8BIT));
    if (!state->band[0].entries) {
      xSemaphoreGive(web_bandmap_snapshot_mutex);
      delete state;
      return nullptr;
    }
    memcpy(state->band[0].entries,
           web_bandmap_entry_at(snapshot, bandid - 1, 0),
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
  if (f_low_memory_mode) return;

  process_web_bandmap_command_queue();
  const uint32_t now = millis();
  if ((int32_t)(now - web_bandmap_next_refresh_ms) >= 0) {
    web_bandmap_next_refresh_ms = now + WEB_BANDMAP_REFRESH_MS;
    rebuild_web_bandmap_snapshot();
  }
}

void init_webserver() {
  web_heap_point("before web handlers");

  setupSdFileListHandler();

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
<html lang="ja">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>POTA運用・ログ作成ヘルパー</title>
<style>
body{font-family:sans-serif;line-height:1.6;margin:18px;max-width:920px;color:#222}h1{font-size:1.55rem}h2{font-size:1.2rem;margin-top:1.8rem;border-left:5px solid #555;padding-left:.6rem}.step{border:1px solid #bbb;border-radius:8px;padding:12px 14px;margin:12px 0}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}input{font-size:1rem;padding:7px;min-width:14em}button,.button{font-size:1rem;padding:7px 12px;cursor:pointer}.primary{font-weight:bold}.note{background:#f3f3f3;padding:9px 12px;border-radius:6px}.warn{background:#fff4d6;padding:9px 12px;border-radius:6px}.status{font-weight:bold;min-height:1.5em}.small{font-size:.92em;color:#444}code{background:#eee;padding:1px 4px}ul{padding-left:1.4em}a{color:#0645ad}
</style></head>
<body onload="updateDownloadLink()">
<p><a href="/potahelp?lang=en">English</a> | <a href="/">ホーム</a></p>
<h1>POTA運用・ログ作成ヘルパー</h1>
<p>このページでは、現在運用しているPOTA公園をDVPloggerへ設定し、その公園で記録したQSOだけをADIFで取り出してPOTAへアップロードできます。</p>
<div class="warn"><strong>重要：</strong>公園番号を入力しただけではログへ反映されません。必ず「この公園をDVPloggerへ設定」を押してください。設定後のQSOにはRemarksへ <code>POTA_MY:公園番号</code> が自動記録されます。</div>

<h2>1. 運用する公園を設定</h2>
<div class="step">
<label for="park"><strong>POTA公園番号</strong></label>
<div class="row"><input type="text" id="park" %PARK_ID% placeholder="例: JP-1001" oninput="updateDownloadLink()">
<button class="primary" onclick="setCurrentPark()">この公園をDVPloggerへ設定</button>
<button onclick="openParkPage()">公園情報をPOTAで確認</button></div>
<p id="setStatus" class="status"></p>
<p class="small">設定するとDVPloggerのJCC/JCG欄へ <code>POTA/JP-xxxx</code> が入ります。以後に登録したQSOが、この公園からのアクティベーションQSOとして識別されます。</p>
</div>

<h2>公園番号が分からない場合</h2>
<div class="step">
<p>現在地のグリッドロケーターから、近い公園を検索できます。検索結果をクリックすると、その公園を本体へ設定し、公園情報ページも開きます。</p>
<div class="row"><input id="grid" value="%GRID_LOCATOR%" placeholder="Grid例: PM95ru"><button onclick="findNearest()">近いPOTA公園を検索</button></div>
<p id="searchStatus" class="status"></p><ul id="results"></ul>
</div>

<h2>2. DVPloggerで通常どおり交信を記録</h2>
<div class="step"><p>CALLSIGN、RST、交換内容などを通常どおり入力してQSOを登録します。公園設定後に登録した各QSOへ、現在の公園番号が自動的に付加されます。</p>
<p class="note">途中で別の公園へ移動した場合は、移動後に新しい公園番号を設定してからQSOを登録してください。公園ごとにログを分けて出力できます。</p></div>

<h2>3. この公園のADIFログを作成</h2>
<div class="step"><p>下のボタンは、Remarksに現在の公園番号が記録されたQSOだけを抽出します。</p>
<a id="dl" class="button primary" href="/adif" download="pota_log.adi">この公園のADIFをダウンロード</a>
<p id="status" class="status"></p>
<p class="small">ファイル名は <code>pota_log_JP-xxxx.adi</code> です。ダウンロード前に公園番号が正しいことを確認してください。</p></div>

<h2>4. POTAへアップロード</h2>
<div class="step"><ol><li>先に上のボタンでADIFを保存します。</li><li>「POTAログ管理を開く」を押してPOTAへログインします。</li><li>POTAのログアップロード画面で、保存したADIFファイルを選択またはドラッグ＆ドロップします。</li><li>公園番号、日時、コールサインを確認して登録します。</li></ol>
<button onclick="openPOTA()">POTAログ管理を開く</button></div>

<h2>ボタンの意味</h2>
<ul><li><strong>この公園をDVPloggerへ設定：</strong>これから記録するQSOへ公園番号を付けます。</li><li><strong>公園情報をPOTAで確認：</strong>POTA公式の公園ページを別タブで開きます。本体設定は変更しません。</li><li><strong>近いPOTA公園を検索：</strong>SDカード上の公園一覧から距離順に候補を表示します。</li><li><strong>この公園のADIFをダウンロード：</strong>選択した公園からのQSOだけをADIFへ出力します。</li><li><strong>POTAログ管理を開く：</strong>POTA公式のログ画面を開きます。自動アップロードは行いません。</li></ul>
<p><a href="/">ホームへ戻る</a>　<a href="/sotahelp?lang=ja">SOTAヘルパーへ</a></p>
<script>
function normPark(){return document.getElementById('park').value.trim().toUpperCase();}
function findNearest(){
 const grid=document.getElementById('grid').value.trim(); const st=document.getElementById('searchStatus');
 if(!grid){st.textContent='グリッドロケーターを入力してください。';return;} st.textContent='検索中…';
 fetch(`/nearest?grid=${encodeURIComponent(grid)}`).then(r=>{if(!r.ok)throw new Error('検索に失敗しました');return r.json();}).then(showResults).catch(e=>st.textContent=e.message);
}
function showResults(list){
 const ul=document.getElementById('results');ul.innerHTML='';document.getElementById('searchStatus').textContent=list.length?`${list.length}件の候補を表示しました。公園名をクリックすると本体へ設定します。`:'候補が見つかりませんでした。';
 list.forEach(p=>{const li=document.createElement('li'),a=document.createElement('a');a.href='#';a.textContent=`${p.code}: ${p.name}（${p.distance_km} km、方位 ${p.bearing_deg}°）`;a.onclick=(ev)=>{ev.preventDefault();selectPark(p.code,p.name,document.getElementById('grid').value);};li.appendChild(a);ul.appendChild(li);});
}
function notifyPark(code,name,grid){return fetch(`/select?code=${encodeURIComponent(code)}&name=${encodeURIComponent(name||'')}&grid=${encodeURIComponent(grid||'')}`).then(r=>{if(!r.ok)throw new Error('DVPloggerへの設定に失敗しました');return r.text();});}
function setCurrentPark(){
 const code=normPark(),st=document.getElementById('setStatus');if(!code){st.textContent='公園番号を入力してください。';return;}
 notifyPark(code,'',document.getElementById('grid').value).then(()=>{document.getElementById('park').value=code;updateDownloadLink();st.textContent=`${code} を現在の運用公園として設定しました。これ以後のQSOへ記録されます。`;}).catch(e=>st.textContent=e.message);
}
function selectPark(code,name,grid){notifyPark(code,name,grid).then(()=>{document.getElementById('park').value=code;updateDownloadLink();document.getElementById('setStatus').textContent=`${code} ${name} を現在の運用公園として設定しました。`;window.open(`https://pota.app/#/park/${encodeURIComponent(code)}`,'_blank');}).catch(e=>document.getElementById('setStatus').textContent=e.message);}
function openParkPage(){const code=normPark();if(!code){document.getElementById('setStatus').textContent='公園番号を入力してください。';return;}window.open(`https://pota.app/#/park/${encodeURIComponent(code)}`,'_blank');}
function updateDownloadLink(){const park=normPark(),link=document.getElementById('dl'),st=document.getElementById('status');if(!park){link.href='/adif';link.download='pota_log.adi';st.textContent='公園番号を入力し、本体へ設定してください。';}else{link.href=`/adif?park=${encodeURIComponent(park)}`;link.download=`pota_log_${park}.adi`;st.textContent=`${park} のQSOだけを抽出する準備ができています。`;}}
function openPOTA(){window.open('https://pota.app/#/user/logs','_blank');}
</script></body></html>
)rawliteral";  

  
  const char *pota_page_en = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>POTA Operation and Log Helper</title><style>body{font-family:sans-serif;line-height:1.6;margin:18px;max-width:920px;color:#222}h1{font-size:1.55rem}h2{font-size:1.2rem;margin-top:1.8rem;border-left:5px solid #555;padding-left:.6rem}.step{border:1px solid #bbb;border-radius:8px;padding:12px 14px;margin:12px 0}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}input{font-size:1rem;padding:7px;min-width:14em}button,.button{font-size:1rem;padding:7px 12px;cursor:pointer}.primary{font-weight:bold}.note{background:#f3f3f3;padding:9px 12px;border-radius:6px}.warn{background:#fff4d6;padding:9px 12px;border-radius:6px}.status{font-weight:bold;min-height:1.5em}.small{font-size:.92em;color:#444}code{background:#eee;padding:1px 4px}ul{padding-left:1.4em}a{color:#0645ad}</style></head><body onload="updateDownloadLink()"><p><a href="/potahelp?lang=ja">日本語</a> | <a href="/">Home</a></p><h1>POTA Operation and Log Helper</h1><p>Set the POTA park currently being activated, export only QSOs made from that park as ADIF, and upload the file to POTA.</p><div class="warn"><strong>Important:</strong> Typing a park reference alone does not change the logger. Press “Set this park in DVPlogger”. Subsequent QSOs receive <code>POTA_MY:park-reference</code> in Remarks.</div>
<h2>1. Set the park being activated</h2><div class="step"><label for="park"><strong>POTA park reference</strong></label><div class="row"><input type="text" id="park" %PARK_ID% placeholder="Example: JP-1001" oninput="updateDownloadLink()"><button class="primary" onclick="setCurrentPark()">Set this park in DVPlogger</button><button onclick="openParkPage()">Open park information</button></div><p id="setStatus" class="status"></p><p class="small">DVPlogger stores <code>POTA/JP-xxxx</code> in the JCC/JCG field. QSOs logged afterward are identified as activation QSOs from this park.</p></div>
<h2>Find a nearby park</h2><div class="step"><p>Search for nearby parks using the current grid locator. Clicking a result sets the park in DVPlogger and opens its POTA page.</p><div class="row"><input id="grid" value="%GRID_LOCATOR%" placeholder="Grid example: PM95ru"><button onclick="findNearest()">Find nearby POTA parks</button></div><p id="searchStatus" class="status"></p><ul id="results"></ul></div>
<h2>2. Log QSOs normally</h2><div class="step"><p>Enter callsign, RST and exchange normally. Each QSO logged after setting the park is tagged with the current park reference.</p><p class="note">After moving to another park, set the new park before logging more QSOs. Logs can be exported separately for each park.</p></div>
<h2>3. Export this park's ADIF log</h2><div class="step"><p>The button below extracts only QSOs whose Remarks contain the current park reference.</p><a id="dl" class="button primary" href="/adif" download="pota_log.adi">Download ADIF for this park</a><p id="status" class="status"></p><p class="small">The filename is <code>pota_log_JP-xxxx.adi</code>. Verify the park reference before downloading.</p></div>
<h2>4. Upload to POTA</h2><div class="step"><ol><li>Save the ADIF file above.</li><li>Open POTA Log Manager and sign in.</li><li>Select or drag the saved ADIF file into the upload page.</li><li>Confirm the park, date/time and callsign before submitting.</li></ol><button onclick="openPOTA()">Open POTA Log Manager</button></div>
<h2>Button reference</h2><ul><li><strong>Set this park in DVPlogger:</strong> tags subsequently logged QSOs with the park reference.</li><li><strong>Open park information:</strong> opens the official POTA park page without changing DVPlogger.</li><li><strong>Find nearby POTA parks:</strong> lists candidates from the park file on the SD card.</li><li><strong>Download ADIF for this park:</strong> exports only QSOs made from the selected park.</li><li><strong>Open POTA Log Manager:</strong> opens the official log page; upload is not automatic.</li></ul><p><a href="/sotahelp?lang=en">SOTA helper</a></p>
<script>function normPark(){return document.getElementById('park').value.trim().toUpperCase();}function findNearest(){const grid=document.getElementById('grid').value.trim(),st=document.getElementById('searchStatus');if(!grid){st.textContent='Enter a grid locator.';return;}st.textContent='Searching...';fetch(`/nearest?grid=${encodeURIComponent(grid)}`).then(r=>{if(!r.ok)throw new Error('Search failed');return r.json();}).then(showResults).catch(e=>st.textContent=e.message);}function showResults(list){const ul=document.getElementById('results');ul.innerHTML='';document.getElementById('searchStatus').textContent=list.length?`${list.length} candidate(s). Click a park name to set it.`:'No candidates found.';list.forEach(p=>{const li=document.createElement('li'),a=document.createElement('a');a.href='#';a.textContent=`${p.code}: ${p.name} (${p.distance_km} km, bearing ${p.bearing_deg}°)`;a.onclick=(ev)=>{ev.preventDefault();selectPark(p.code,p.name,document.getElementById('grid').value);};li.appendChild(a);ul.appendChild(li);});}function notifyPark(code,name,grid){return fetch(`/select?code=${encodeURIComponent(code)}&name=${encodeURIComponent(name||'')}&grid=${encodeURIComponent(grid||'')}`).then(r=>{if(!r.ok)throw new Error('Failed to set DVPlogger');return r.text();});}function setCurrentPark(){const code=normPark(),st=document.getElementById('setStatus');if(!code){st.textContent='Enter a park reference.';return;}notifyPark(code,'',document.getElementById('grid').value).then(()=>{document.getElementById('park').value=code;updateDownloadLink();st.textContent=`${code} is now the active park. It will be recorded in subsequent QSOs.`;}).catch(e=>st.textContent=e.message);}function selectPark(code,name,grid){notifyPark(code,name,grid).then(()=>{document.getElementById('park').value=code;updateDownloadLink();document.getElementById('setStatus').textContent=`${code} ${name} is now the active park.`;window.open(`https://pota.app/#/park/${encodeURIComponent(code)}`,'_blank');}).catch(e=>document.getElementById('setStatus').textContent=e.message);}function openParkPage(){const code=normPark();if(!code){document.getElementById('setStatus').textContent='Enter a park reference.';return;}window.open(`https://pota.app/#/park/${encodeURIComponent(code)}`,'_blank');}function updateDownloadLink(){const park=normPark(),link=document.getElementById('dl'),st=document.getElementById('status');if(!park){link.href='/adif';link.download='pota_log.adi';st.textContent='Enter a park reference and set it in DVPlogger.';}else{link.href=`/adif?park=${encodeURIComponent(park)}`;link.download=`pota_log_${park}.adi`;st.textContent=`Ready to extract QSOs for ${park}.`;}}function openPOTA(){window.open('https://pota.app/#/user/logs','_blank');}</script></body></html>
)rawliteral";

  web_server.on("/potahelp", HTTP_GET, [pota_page,pota_page_en](AsyncWebServerRequest* request){
    const bool japanese = request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja");
    String html(japanese ? pota_page : pota_page_en);
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
<html lang="ja">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SOTA運用・ログ作成ヘルパー</title>
<style>
body{font-family:sans-serif;line-height:1.6;margin:18px;max-width:920px;color:#222}h1{font-size:1.55rem}h2{font-size:1.2rem;margin-top:1.8rem;border-left:5px solid #555;padding-left:.6rem}.step{border:1px solid #bbb;border-radius:8px;padding:12px 14px;margin:12px 0}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}input{font-size:1rem;padding:7px;min-width:14em}button,.button{font-size:1rem;padding:7px 12px;cursor:pointer}.primary{font-weight:bold}.note{background:#f3f3f3;padding:9px 12px;border-radius:6px}.warn{background:#fff4d6;padding:9px 12px;border-radius:6px}.status{font-weight:bold;min-height:1.5em}.small{font-size:.92em;color:#444}code{background:#eee;padding:1px 4px}ul{padding-left:1.4em}a{color:#0645ad}
</style></head>
<body onload="updateDownloadLinkSOTA()">
<p><a href="/sotahelp?lang=en">English</a> | <a href="/">ホーム</a></p>
<h1>SOTA運用・ログ作成ヘルパー</h1>
<p>このページでは、現在運用しているSOTA山頂をDVPloggerへ設定し、その山頂で記録したQSOだけをADIFで取り出してSOTA Databaseへアップロードできます。</p>
<div class="warn"><strong>重要：</strong>山頂IDを入力しただけではログへ反映されません。必ず「この山頂をDVPloggerへ設定」を押してください。設定後のQSOにはRemarksへ <code>SOTA_MY:山頂ID</code> が自動記録されます。</div>

<h2>1. 運用する山頂を設定</h2>
<div class="step"><label for="summit"><strong>SOTA山頂ID</strong></label>
<div class="row"><input type="text" id="summit" %SUMMIT_ID% placeholder="例: JA/KN-006" oninput="updateDownloadLinkSOTA()">
<button class="primary" onclick="setCurrentSummit()">この山頂をDVPloggerへ設定</button>
<button onclick="openSummitPage()">山頂情報を確認</button></div>
<p id="setStatus" class="status"></p><p class="small">設定するとDVPloggerのJCC/JCG欄へ <code>SOTA/JA/xx-xxx</code> が入ります。以後に登録したQSOが、この山頂からのアクティベーションQSOとして識別されます。</p></div>

<h2>山頂IDが分からない場合</h2>
<div class="step"><p>現在地のグリッドロケーターから近い山頂を検索できます。検索結果をクリックすると、その山頂を本体へ設定し、山頂情報ページも開きます。</p>
<div class="row"><input id="grid" value="%GRID_LOCATOR%" placeholder="Grid例: PM95ru"><button onclick="findSota()">近いSOTA山頂を検索</button></div>
<p id="searchStatus" class="status"></p><ul id="sotaResults"></ul></div>

<h2>2. DVPloggerで通常どおり交信を記録</h2>
<div class="step"><p>CALLSIGN、RST、交換内容などを通常どおり入力してQSOを登録します。山頂設定後に登録した各QSOへ、現在の山頂IDが自動的に付加されます。</p>
<p class="note">別の山頂へ移動した場合は、新しい山頂IDを設定してからQSOを登録してください。山頂ごとにログを分けて出力できます。</p></div>

<h2>3. この山頂のADIFログを作成</h2>
<div class="step"><p>下のボタンは、Remarksに現在の山頂IDが記録されたQSOだけを抽出します。</p>
<a id="dl" class="button primary" href="/adif" download="sota_log.adi">この山頂のADIFをダウンロード</a><p id="status" class="status"></p>
<p class="small">ファイル名は <code>sota_log_JA_xx-xxx.adi</code> です。ブラウザーによっては山頂ID中の「/」が「_」などへ置換されます。</p></div>

<h2>4. SOTA Databaseへアップロード</h2>
<div class="step"><ol><li>先に上のボタンでADIFを保存します。</li><li>「SOTAログアップロードを開く」を押してログインします。</li><li>Activator logのアップロードを選び、保存したADIFを指定します。</li><li>山頂ID、日時、コールサインを確認して登録します。</li></ol><button onclick="openSOTA()">SOTAログアップロードを開く</button></div>

<h2>ボタンの意味</h2><ul><li><strong>この山頂をDVPloggerへ設定：</strong>これから記録するQSOへ山頂IDを付けます。</li><li><strong>山頂情報を確認：</strong>SOTLASの山頂ページを別タブで開きます。本体設定は変更しません。</li><li><strong>近いSOTA山頂を検索：</strong>SDカード上の山頂一覧から距離順に候補を表示します。</li><li><strong>この山頂のADIFをダウンロード：</strong>選択した山頂からのQSOだけをADIFへ出力します。</li><li><strong>SOTAログアップロードを開く：</strong>SOTA Databaseのアップロード画面を開きます。自動アップロードは行いません。</li></ul>
<p><a href="/">ホームへ戻る</a>　<a href="/potahelp?lang=ja">POTAヘルパーへ</a></p>
<script>
function normSummit(){return document.getElementById('summit').value.trim().toUpperCase();}
function findSota(){const g=document.getElementById('grid').value.trim(),st=document.getElementById('searchStatus');if(!g){st.textContent='グリッドロケーターを入力してください。';return;}st.textContent='検索中…';fetch(`/nearest_summit?grid=${encodeURIComponent(g)}`).then(r=>{if(!r.ok)throw new Error('検索に失敗しました');return r.json();}).then(showSota).catch(e=>st.textContent=e.message);}
function showSota(list){const ul=document.getElementById('sotaResults');ul.innerHTML='';document.getElementById('searchStatus').textContent=list.length?`${list.length}件の候補を表示しました。山頂名をクリックすると本体へ設定します。`:'候補が見つかりませんでした。';list.forEach(s=>{const li=document.createElement('li'),a=document.createElement('a');a.href='#';a.textContent=`${s.code}: ${s.name}（${s.distance_km} km、標高 ${s.alt} m、方位 ${s.bearing_deg}°）`;a.onclick=(ev)=>{ev.preventDefault();selectSota(s.code,s.name,document.getElementById('grid').value);};li.appendChild(a);ul.appendChild(li);});}
function notifySummit(code,name,grid){return fetch(`/select_summit?code=${encodeURIComponent(code)}&name=${encodeURIComponent(name||'')}&grid=${encodeURIComponent(grid||'')}`).then(r=>{if(!r.ok)throw new Error('DVPloggerへの設定に失敗しました');return r.text();});}
function setCurrentSummit(){const code=normSummit(),st=document.getElementById('setStatus');if(!code){st.textContent='山頂IDを入力してください。';return;}notifySummit(code,'',document.getElementById('grid').value).then(()=>{document.getElementById('summit').value=code;updateDownloadLinkSOTA();st.textContent=`${code} を現在の運用山頂として設定しました。これ以後のQSOへ記録されます。`;}).catch(e=>st.textContent=e.message);}
function selectSota(code,name,grid){notifySummit(code,name,grid).then(()=>{document.getElementById('summit').value=code;updateDownloadLinkSOTA();document.getElementById('setStatus').textContent=`${code} ${name} を現在の運用山頂として設定しました。`;window.open(`https://sotl.as/summits/${encodeURIComponent(code)}`,'_blank');}).catch(e=>document.getElementById('setStatus').textContent=e.message);}
function openSummitPage(){const code=normSummit();if(!code){document.getElementById('setStatus').textContent='山頂IDを入力してください。';return;}window.open(`https://sotl.as/summits/${encodeURIComponent(code)}`,'_blank');}
function updateDownloadLinkSOTA(){const summit=normSummit(),link=document.getElementById('dl'),st=document.getElementById('status');if(!summit){link.href='/adif';link.download='sota_log.adi';st.textContent='山頂IDを入力し、本体へ設定してください。';}else{link.href=`/adif?summit=${encodeURIComponent(summit)}`;link.download=`sota_log_${summit.replaceAll('/','_')}.adi`;st.textContent=`${summit} のQSOだけを抽出する準備ができています。`;}}
function openSOTA(){window.open('https://www.sotadata.org.uk/ja/upload','_blank');}
</script></body></html>
)rawliteral";  
  
  const char *sota_page_en = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SOTA Operation and Log Helper</title><style>body{font-family:sans-serif;line-height:1.6;margin:18px;max-width:920px;color:#222}h1{font-size:1.55rem}h2{font-size:1.2rem;margin-top:1.8rem;border-left:5px solid #555;padding-left:.6rem}.step{border:1px solid #bbb;border-radius:8px;padding:12px 14px;margin:12px 0}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}input{font-size:1rem;padding:7px;min-width:14em}button,.button{font-size:1rem;padding:7px 12px;cursor:pointer}.primary{font-weight:bold}.note{background:#f3f3f3;padding:9px 12px;border-radius:6px}.warn{background:#fff4d6;padding:9px 12px;border-radius:6px}.status{font-weight:bold;min-height:1.5em}.small{font-size:.92em;color:#444}code{background:#eee;padding:1px 4px}ul{padding-left:1.4em}a{color:#0645ad}</style></head><body onload="updateDownloadLinkSOTA()"><p><a href="/sotahelp?lang=ja">日本語</a> | <a href="/">Home</a></p><h1>SOTA Operation and Log Helper</h1><p>Set the SOTA summit currently being activated, export only QSOs made from that summit as ADIF, and upload the file to SOTA Database.</p><div class="warn"><strong>Important:</strong> Typing a summit reference alone does not change the logger. Press “Set this summit in DVPlogger”. Subsequent QSOs receive <code>SOTA_MY:summit-reference</code> in Remarks.</div>
<h2>1. Set the summit being activated</h2><div class="step"><label for="summit"><strong>SOTA summit reference</strong></label><div class="row"><input type="text" id="summit" %SUMMIT_ID% placeholder="Example: JA/KN-006" oninput="updateDownloadLinkSOTA()"><button class="primary" onclick="setCurrentSummit()">Set this summit in DVPlogger</button><button onclick="openSummitPage()">Open summit information</button></div><p id="setStatus" class="status"></p><p class="small">DVPlogger stores <code>SOTA/JA/xx-xxx</code> in the JCC/JCG field. QSOs logged afterward are identified as activation QSOs from this summit.</p></div>
<h2>Find a nearby summit</h2><div class="step"><p>Search for nearby summits using the current grid locator. Clicking a result sets the summit in DVPlogger and opens its information page.</p><div class="row"><input id="grid" value="%GRID_LOCATOR%" placeholder="Grid example: PM95ru"><button onclick="findSota()">Find nearby SOTA summits</button></div><p id="searchStatus" class="status"></p><ul id="sotaResults"></ul></div>
<h2>2. Log QSOs normally</h2><div class="step"><p>Enter callsign, RST and exchange normally. Each QSO logged after setting the summit is tagged with the current summit reference.</p><p class="note">After moving to another summit, set the new reference before logging more QSOs. Logs can be exported separately for each summit.</p></div>
<h2>3. Export this summit's ADIF log</h2><div class="step"><p>The button below extracts only QSOs whose Remarks contain the current summit reference.</p><a id="dl" class="button primary" href="/adif" download="sota_log.adi">Download ADIF for this summit</a><p id="status" class="status"></p><p class="small">The filename is <code>sota_log_JA_xx-xxx.adi</code>. A browser may replace “/” in the summit reference with “_”.</p></div>
<h2>4. Upload to SOTA Database</h2><div class="step"><ol><li>Save the ADIF file above.</li><li>Open SOTA log upload and sign in.</li><li>Select Activator log upload and choose the saved ADIF file.</li><li>Confirm the summit, date/time and callsign before submitting.</li></ol><button onclick="openSOTA()">Open SOTA log upload</button></div>
<h2>Button reference</h2><ul><li><strong>Set this summit in DVPlogger:</strong> tags subsequently logged QSOs with the summit reference.</li><li><strong>Open summit information:</strong> opens the SOTLAS summit page without changing DVPlogger.</li><li><strong>Find nearby SOTA summits:</strong> lists candidates from the summit file on the SD card.</li><li><strong>Download ADIF for this summit:</strong> exports only QSOs made from the selected summit.</li><li><strong>Open SOTA log upload:</strong> opens the SOTA Database upload page; upload is not automatic.</li></ul><p><a href="/potahelp?lang=en">POTA helper</a></p>
<script>function normSummit(){return document.getElementById('summit').value.trim().toUpperCase();}function findSota(){const g=document.getElementById('grid').value.trim(),st=document.getElementById('searchStatus');if(!g){st.textContent='Enter a grid locator.';return;}st.textContent='Searching...';fetch(`/nearest_summit?grid=${encodeURIComponent(g)}`).then(r=>{if(!r.ok)throw new Error('Search failed');return r.json();}).then(showSota).catch(e=>st.textContent=e.message);}function showSota(list){const ul=document.getElementById('sotaResults');ul.innerHTML='';document.getElementById('searchStatus').textContent=list.length?`${list.length} candidate(s). Click a summit name to set it.`:'No candidates found.';list.forEach(x=>{const li=document.createElement('li'),a=document.createElement('a');a.href='#';a.textContent=`${x.code}: ${x.name} (${x.distance_km} km, altitude ${x.alt} m, bearing ${x.bearing_deg}°)`;a.onclick=(ev)=>{ev.preventDefault();selectSota(x.code,x.name,document.getElementById('grid').value);};li.appendChild(a);ul.appendChild(li);});}function notifySummit(code,name,grid){return fetch(`/select_summit?code=${encodeURIComponent(code)}&name=${encodeURIComponent(name||'')}&grid=${encodeURIComponent(grid||'')}`).then(r=>{if(!r.ok)throw new Error('Failed to set DVPlogger');return r.text();});}function setCurrentSummit(){const code=normSummit(),st=document.getElementById('setStatus');if(!code){st.textContent='Enter a summit reference.';return;}notifySummit(code,'',document.getElementById('grid').value).then(()=>{document.getElementById('summit').value=code;updateDownloadLinkSOTA();st.textContent=`${code} is now the active summit. It will be recorded in subsequent QSOs.`;}).catch(e=>st.textContent=e.message);}function selectSota(code,name,grid){notifySummit(code,name,grid).then(()=>{document.getElementById('summit').value=code;updateDownloadLinkSOTA();document.getElementById('setStatus').textContent=`${code} ${name} is now the active summit.`;window.open(`https://sotl.as/summits/${encodeURIComponent(code)}`,'_blank');}).catch(e=>document.getElementById('setStatus').textContent=e.message);}function openSummitPage(){const code=normSummit();if(!code){document.getElementById('setStatus').textContent='Enter a summit reference.';return;}window.open(`https://sotl.as/summits/${encodeURIComponent(code)}`,'_blank');}function updateDownloadLinkSOTA(){const summit=normSummit(),link=document.getElementById('dl'),st=document.getElementById('status');if(!summit){link.href='/adif';link.download='sota_log.adi';st.textContent='Enter a summit reference and set it in DVPlogger.';}else{link.href=`/adif?summit=${encodeURIComponent(summit)}`;link.download=`sota_log_${summit.replaceAll('/','_')}.adi`;st.textContent=`Ready to extract QSOs for ${summit}.`;}}function openSOTA(){window.open('https://www.sotadata.org.uk/en/upload','_blank');}</script></body></html>
)rawliteral";

  web_server.on("/sotahelp", HTTP_GET, [sota_page,sota_page_en](AsyncWebServerRequest* request){
    const bool japanese = request->hasParam("lang") && request->getParam("lang")->value().equalsIgnoreCase("ja");
    String html(japanese ? sota_page : sota_page_en);
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

// DVPlogger status page.  Use ?lang=en for English; Japanese is default.
web_server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
  bool english = request->hasParam("lang") &&
                 request->getParam("lang")->value().equalsIgnoreCase("en");
  const bool connected = (WiFi.status() == WL_CONNECTED);
  const uint32_t seconds = millis() / 1000UL;
  const uint32_t days = seconds / 86400UL;
  const uint32_t hours = (seconds / 3600UL) % 24UL;
  const uint32_t minutes = (seconds / 60UL) % 60UL;
  const uint32_t secs = seconds % 60UL;

  auto esc = [](const String &src) {
    String dst;
    dst.reserve(src.length() + 12);
    for (size_t i = 0; i < src.length(); ++i) {
      switch (src[i]) {
      case '&': dst += F("&amp;"); break;
      case '<': dst += F("&lt;"); break;
      case '>': dst += F("&gt;"); break;
      case '"': dst += F("&quot;"); break;
      default: dst += src[i]; break;
      }
    }
    return dst;
  };

  auto input_name = [english](int ptr) -> String {
    if (ptr >= 10 && ptr < 10 + N_CWMSG)
      return String(english ? "CW message F" : "CWメッセージ F") + String(ptr - 9);
    if (ptr >= 30 && ptr < 30 + N_CWMSG)
      return String(english ? "RTTY message F" : "RTTYメッセージ F") + String(ptr - 29);
    switch (ptr) {
    case 0: return english ? String("Callsign") : String("相手局コールサイン");
    case 1: return english ? String("Received exchange") : String("受信ナンバー");
    case 2: return english ? String("Sent RST") : String("送信RST");
    case 3: return english ? String("Received RST") : String("受信RST");
    case 4: return english ? String("My callsign") : String("自局コールサイン");
    case 5: return english ? String("Sent exchange") : String("送出ナンバー");
    case 6: return english ? String("Remarks") : String("Remarks");
    case 7: return english ? String("Satellite") : String("衛星名");
    case 8: return english ? String("Grid locator") : String("グリッドロケータ");
    case 9: return english ? String("JCC/JCG") : String("JCC/JCG");
    case 20: return english ? String("Rig name") : String("リグ名");
    case 21: return english ? String("Cluster name") : String("Cluster名");
    case 22: return english ? String("Email address") : String("メールアドレス");
    case 23: return english ? String("Cluster command") : String("Clusterコマンド");
    case 24: return english ? String("Power code") : String("電力コード");
    case 25: return english ? String("Wi-Fi SSID") : String("Wi-Fi SSID");
    case 26: return english ? String("Wi-Fi password") : String("Wi-Fiパスワード");
    case 27: return english ? String("Rig specification") : String("リグ仕様");
    case 28: return english ? String("Z-server") : String("Z-server");
    case 29: return english ? String("Operator name") : String("オペレータ名");
    case 40: return english ? String("Contest") : String("コンテスト");
    case 41: return english ? String("Cluster2 name") : String("Cluster2名");
    case 42: return english ? String("Cluster2 command") : String("Cluster2コマンド");
    default: return String("#") + String(ptr);
    }
  };

  auto input_value = [](struct radio *radio) -> String {
    const int ptr = radio->ptr_curr;
    if (ptr >= 10 && ptr < 10 + N_CWMSG) return String(plogw->cw_msg[ptr - 10] + 2);
    if (ptr >= 30 && ptr < 30 + N_CWMSG) return String(plogw->rtty_msg[ptr - 30] + 2);
    switch (ptr) {
    case 0: return String(radio->callsign + 2);
    case 1: return String(radio->recv_exch + 2);
    case 2: return String(radio->sent_rst + 2);
    case 3: return String(radio->recv_rst + 2);
    case 4: return String(plogw->my_callsign + 2);
    case 5: return String(plogw->sent_exch + 2);
    case 6: return String(radio->remarks + 2);
    case 7: return String(plogw->sat_name + 2);
    case 8: return String(plogw->grid_locator + 2);
    case 9: return String(plogw->jcc + 2);
    case 20: return String(radio->rig_name + 2);
    case 21: return String(plogw->cluster_name + 2);
    case 22: return String(plogw->email_addr + 2);
    case 23: return String(plogw->cluster_cmd + 2);
    case 24: return String(plogw->power_code + 2);
    case 25: return String(plogw->wifi_ssid + 2);
    case 26: return String("********");
    case 27: return String(radio->rig_spec_str + 2);
    case 28: return String(plogw->zserver_name + 2);
    case 29: return String(plogw->my_name + 2);
    case 40: return String(plogw->contest_name + 2);
    case 41: return String(plogw->cluster2_name + 2);
    case 42: return String(plogw->cluster2_cmd + 2);
    default: return String("-");
    }
  };

  String html;
  html.reserve(6000);
  html += F("<!doctype html><html lang=\"");
  html += english ? F("en") : F("ja");
  html += F("\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>DVPlogger Status</title><style>body{font-family:sans-serif;margin:20px;max-width:1050px}table{border-collapse:collapse;width:100%;margin-bottom:18px}th,td{border:1px solid #bbb;padding:7px;text-align:left}th{background:#eee}.summary th{width:32%}.radio th,.radio td{white-space:nowrap}.radio td:last-child{white-space:normal}.nav a{margin-right:14px}h2{margin-bottom:6px}</style></head><body>");
  html += F("<div class=\"nav\"><a href=\"/\">");
  html += english ? F("Home") : F("ホーム");
  html += F("</a><a href=\"/settings\">");
  html += english ? F("Settings") : F("設定");
  html += F("</a><a href=\"/status?lang=");
  html += english ? F("ja\">日本語") : F("en\">English");
  html += F("</a></div><h1>DVPlogger Status</h1>");

  auto row = [&](const __FlashStringHelper *ja, const __FlashStringHelper *en,
                 const String &value) {
    html += F("<tr><th>"); html += english ? en : ja;
    html += F("</th><td>"); html += esc(value); html += F("</td></tr>");
  };

  // Put the information needed most often during operation at the top.
  html += F("<h2>"); html += english ? F("Logger") : F("ロガー"); html += F("</h2><table class=\"summary\">");
  row(F("IPアドレス"), F("IP address"), connected ? WiFi.localIP().toString() : String("-"));
  row(F("自局コールサイン"), F("My callsign"), String(plogw->my_callsign + 2));
  row(F("現在のコンテスト"), F("Current contest"), String(plogw->contest_name + 2));
  row(F("コンテスト運用"), F("Contest logging"),
      plogw->f_off_contest ? (english ? String("OFF (OFFCONTEST)") : String("OFF（OFFCONTEST）"))
                          : (english ? String("ON (ONCONTEST)") : String("ON（ONCONTEST）")));
  row(F("フォーカスRadio"), F("Focused radio"), String(so2r.focused_radio() + 1));
  row(F("RX Radio"), F("RX radio"), String(so2r.rx() + 1));
  row(F("TX Radio"), F("TX radio"), String(so2r.tx() + 1));
  struct radio *selected = so2r.radio_selected();
  row(F("LCD入力欄"), F("LCD input field"), input_name(selected->ptr_curr));
  row(F("入力中の内容"), F("Current input"), input_value(selected));
  html += F("</table>");

  html += F("<h2>Radio</h2><table class=\"radio\"><tr><th>#</th><th>");
  html += english ? F("State") : F("状態");
  html += F("</th><th>Rig</th><th>"); html += english ? F("Frequency") : F("周波数");
  html += F("</th><th>Mode</th><th>S</th><th>CQ/S&amp;P</th><th>");
  html += english ? F("LCD input field") : F("LCD入力欄"); html += F("</th></tr>");
  for (int i = 0; i < N_RADIO; ++i) {
    struct radio *radio = &radio_list[i];
    String state;
    if (!radio->enabled) state = english ? String("Disabled") : String("無効");
    else state = english ? String("Enabled") : String("有効");
    if (so2r.focused_radio() == i) state += english ? String(" / Focus") : String(" / Focus");
    if (so2r.rx() == i) state += String(" / RX");
    if (so2r.tx() == i) state += String(" / TX");
    char fbuf[24];
    snprintf(fbuf, sizeof(fbuf), "%u.%05u MHz", radio->freq / 100000U,
             radio->freq % 100000U);
    const char *rig_name = (radio->rig_spec != NULL && radio->rig_spec->name != NULL)
                           ? radio->rig_spec->name : "-";
    html += F("<tr><td>"); html += String(i + 1); html += F("</td><td>"); html += esc(state);
    html += F("</td><td>"); html += esc(String(rig_name)); html += F("</td><td>"); html += fbuf;
    html += F("</td><td>"); html += esc(String(radio->opmode)); html += F("</td><td>");
    html += radio->enabled ? String(radio->smeter / SMETER_UNIT_DBM) : String("-");
    html += F("</td><td>");
    html += radio->cq[radio->modetype] ? String("CQ") : String("S&P");
    html += F("</td><td>"); html += esc(input_name(radio->ptr_curr)); html += F("</td></tr>");
  }
  html += F("</table>");

  html += F("<h2>Wi-Fi</h2><table class=\"summary\">");
  row(F("Wi-Fi状態"), F("Wi-Fi status"),
      connected ? (english ? String("Connected") : String("接続中"))
                : (english ? String("Disconnected") : String("未接続")));
  row(F("SSID"), F("SSID"), connected ? WiFi.SSID() : String("-"));
  row(F("受信強度"), F("Wi-Fi RSSI"), connected ? String(WiFi.RSSI()) + " dBm" : String("-"));
  row(F("サブネットマスク"), F("Subnet mask"), connected ? WiFi.subnetMask().toString() : String("-"));
  row(F("ゲートウェイ"), F("Gateway"), connected ? WiFi.gatewayIP().toString() : String("-"));
  row(F("MACアドレス"), F("MAC address"), WiFi.macAddress());
  html += F("</table>");

  html += F("<h2>"); html += english ? F("System") : F("システム"); html += F("</h2><table class=\"summary\">");
  row(F("稼働時間"), F("Uptime"),
      String(days) + "d " + String(hours) + "h " + String(minutes) + "m " + String(secs) + "s");
  row(F("空きヒープ"), F("Free heap"), String(ESP.getFreeHeap()) + " bytes");
  row(F("最小空きヒープ"), F("Minimum free heap"), String(ESP.getMinFreeHeap()) + " bytes");
  row(F("PSRAM容量"), F("PSRAM size"), String(ESP.getPsramSize()) + " bytes");
  row(F("メモリ動作モード"), F("Memory mode"),
      String(f_low_memory_mode ? "LOW (no PSRAM)" : "NORMAL (PSRAM)"));
  row(F("PSRAM空き"), F("Free PSRAM"), String(ESP.getFreePsram()) + " bytes");
  html += F("</table><p>");
  html += english ? F("Reload this page to refresh the values.")
                  : F("表示を更新するにはページを再読み込みしてください。");
  html += F("</p></body></html>");
  request->send(200, "text/html", html);
});

web_server.on("/antenna_status", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send(200, "application/json", antenna_status_json());
});

web_server.on("/antenna", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", antenna_page_html);
});

web_server.on("/antenna_config", HTTP_GET, [](AsyncWebServerRequest *request) {
  String out;
  out.reserve(700);
  out += F("{\"enable\":"); out += antenna_control_enable ? F("true") : F("false");
  out += F(",\"host\":\""); out += antenna_host;
  out += F("\",\"port\":"); out += String(antenna_port);
  out += F(",\"pref\":[");
  for (int i=0;i<ANTENNA_PREF_ROWS;i++){if(i)out+=',';out+='\"';out+=antenna_pref[i];out+='\"';}
  out += F("],\"names\":[");
  for (int i=0;i<ANTENNA_MAX_ID;i++){if(i)out+=',';out+='\"';out+=antenna_name[i];out+='\"';}
  out += F("]}");
  request->send(200, "application/json", out);
});

web_server.on("/antenna_config", HTTP_POST, [](AsyncWebServerRequest *request) {
  if (request->hasParam("enable")) antenna_control_enable = request->getParam("enable")->value().toInt() ? 1 : 0;
  if (request->hasParam("host")) strlcpy(antenna_host, request->getParam("host")->value().c_str(), sizeof(antenna_host));
  if (request->hasParam("port")) antenna_port = request->getParam("port")->value().toInt();
  for (int i=0;i<ANTENNA_PREF_ROWS;i++) {
    String key = String("pref") + String(i+1);
    if (request->hasParam(key)) strlcpy(antenna_pref[i], request->getParam(key)->value().c_str(), sizeof(antenna_pref[i]));
  }
  for (int i=0;i<ANTENNA_MAX_ID;i++) {
    String key = String("name") + String(i+1);
    if (request->hasParam(key)) strlcpy(antenna_name[i], request->getParam(key)->value().c_str(), sizeof(antenna_name[i]));
  }
  antenna_settings_changed();
  save_settings("");
  request->send(200, "text/plain", "Saved");
});

// /op ページ配信
web_server.on("/op", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", oppage_html);
});

web_server.on("/rig_key", HTTP_GET, [](AsyncWebServerRequest *req) {
  char response_string[100];
  strcpy(response_string,"");
  struct radio *radio;  
  if (req->hasParam("keycode")) {
    WebUiCommand cmd{};
    cmd.type=WEB_UI_KEY;
    cmd.value=req->getParam("keycode")->value().toInt();
    cmd.index=req->hasParam("index") ? req->getParam("index")->value().toInt() : -1;
    if (req->hasParam("input0")) normalize_op_cstr(0,cmd.input0,sizeof(cmd.input0),req->getParam("input0")->value());
    if (req->hasParam("input1")) normalize_op_cstr(1,cmd.input1,sizeof(cmd.input1),req->getParam("input1")->value());
    if ((cmd.index==0 || cmd.index==1) && req->hasParam("input0") && req->hasParam("input1")) cmd.type=WEB_UI_ENTER;
    if (!enqueue_web_ui(cmd)) { req->send(503,"text/plain","Web UI queue full"); return; }
    req->send(202,"text/plain","Queued");
  } else if (req->hasParam("command")) {

    String command;
    // rig name change
    command = req->getParam("command")->value();
    webLog.print("command:");webLog.println(command);
    if (command == "set") {
      if (req->hasParam("index") && req->hasParam("value")) {
	// get index and value
	int idx = req->getParam("index")->value().toInt();
	String value = normalize_op_value(idx, req->getParam("value")->value());
        if ((idx >= 0 && idx <= 5)) {
          WebUiCommand cmd{};
          cmd.type = WEB_UI_SET;
          cmd.index = idx;
          strlcpy(cmd.input0, value.c_str(), sizeof(cmd.input0));
          if (!enqueue_web_ui(cmd)) { req->send(503,"text/plain","Web UI queue full"); return; }
          req->send(202,"text/plain","Queued");
          return;
        }
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
  if (!request->hasParam("type") || !request->hasParam("value")) {
    request->send(400,"text/plain","Missing parameter"); return;
  }
  WebUiCommand cmd{};
  cmd.type=WEB_UI_CONTROL;
  strlcpy(cmd.name,request->getParam("type")->value().c_str(),sizeof(cmd.name));
  String value=request->getParam("value")->value();
  cmd.value=value.toInt();
  strlcpy(cmd.input0,value.c_str(),sizeof(cmd.input0));
  if (!enqueue_web_ui(cmd)) { request->send(503,"text/plain","Web UI queue full"); return; }
  request->send(202,"text/plain","Queued");
});

       
// /op main status: one compact response instead of many /radio_status requests.
web_server.on("/op_status", HTTP_GET, [](AsyncWebServerRequest *req) {
  struct radio *radio = so2r.radio_selected();
  char radio_text[120];
  unsigned int frac = (radio->freq % (1000/FREQ_UNIT))/(10/FREQ_UNIT);
  unsigned int khz = (radio->freq / (1000/FREQ_UNIT)) % 1000;
  snprintf(radio_text, sizeof(radio_text),
           "%-14s Radio:%d %3s Freq:%4d.%03d.%02d Hz Mode:%4s",
           plogw->tm+9, radio->rig_idx,
           radio->cq[radio->modetype] == LOG_CQ ? "CQ" : "S&P",
           radio->freq / (1000000/FREQ_UNIT), khz, frac, radio->opmode);
  char payload[512];
  snprintf(payload, sizeof(payload),
           "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%d\t1",
           radio->callsign+2, radio->recv_exch+2,
           radio->recv_rst+2, radio->sent_rst+2,
           plogw->sent_exch+2, plogw->my_callsign+2,
           radio_list[0].rig_name+2, radio_list[1].rig_name+2,
           radio_list[2].rig_name+2, plogw->contest_name+2,
           radio_text, plogw->cwbuf_display,
           radio->rig_idx, radio->opmode, radio->bandid);
  req->send(200, "text/plain", payload);
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
      strlcpy(string_buf, plogw->cwbuf_display, sizeof(string_buf));
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


  if (f_low_memory_mode) {
    webLog.println("[WEB] LOWMEM: bandmap snapshots/API disabled");
    web_server.on("/bandmap", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(200, "text/html; charset=utf-8",
        "<!doctype html><meta charset='utf-8'><title>DVPlogger low memory</title>"
        "<h2>Bandmap disabled</h2>"
        "<p>This unit has no PSRAM. The Web bandmap is disabled to preserve memory.</p>"
        "<p>LCD bandmap, logging, status and settings remain available.</p>"
        "<p><a href='/'>Home</a> | <a href='/status'>Status</a> | <a href='/settings'>Settings</a></p>");
    });
    web_server.on("/api/bandmap/version", HTTP_GET,
      [](AsyncWebServerRequest *request) {
        request->send(503, "application/json", "{\"ready\":false,\"low_memory\":true}");
      });
  } else {
    setup_web_bandmap_handlers();
  }

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  web_server.onNotFound(notFound);

  // Keep Nearest/Summit available in LOWMEM mode. Measure each handler
  // group separately so its permanent internal-RAM cost is visible.
  web_heap_point("before nearest");
  setupNearestHandler(web_server);
  web_heap_point("after nearest");
  setupNearestSummit(web_server);
  web_heap_point("after summit");
  setupContestPageHandler();
  web_heap_point("after contest handler");
  setupSettingsPageHandler();
  web_heap_point("after settings handler");
  web_heap_point("after web handlers");
  web_server.begin();
  web_heap_point("after web begin");
}



