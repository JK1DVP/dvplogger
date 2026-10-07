/*
  espmwap.h - WiFi Multi AP Library for ESP8266/ESP32
 
  Copyright (c) 2023 Sasapea's Lab. All right reserved.
  DVPlogger-specific modifications: Copyright (c) 2026 Eiichiro Araki
 
  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
 
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General
  Public License along with this library; if not, write to the
  Free Software Foundation, Inc., 59 Temple Place, Suite 330,
  Boston, MA  02111-1307  USA
*/
#ifndef __ESPWMAP_H
#define __ESPWMAP_H
 
#include <vector>
#if defined(ESP32)
  #include "WiFi.h"
#elif defined(ESP8266)
  #include "ESP8266WiFi.h"
  #define WiFiScanClass ESP8266WiFiScanClass
#else
  #error "not supported enviroment."
#endif
#if defined(ESP32)
  #include "esp_heap_caps.h"
#endif
 
#define ESPWMAP_DEBUG 0
#ifndef ESPWMAP_MEMDIAG
#define ESPWMAP_MEMDIAG 1
#endif
#define ESPWMAP_CONNECTION_TIMEOUT (3 * 60 * 1000) // ms

struct ESPWMAPDiagEvent
{
  const char *phase;
  uint32_t free8;
  uint32_t largest8;
  uint32_t min8;
};
 
class ESPWMAPClass : private WiFiScanClass
{
  private:

#if defined(ESP32) && ESPWMAP_MEMDIAG
    static constexpr uint8_t DIAG_RING_LEN = 16;
    ESPWMAPDiagEvent _diag[DIAG_RING_LEN];
    uint8_t _diag_head = 0;
    uint8_t _diag_tail = 0;
    uint32_t _diag_dropped = 0;

    void memdiag(const char *tag)
    {
      const uint8_t next = (uint8_t)((_diag_head + 1U) % DIAG_RING_LEN);
      if (next == _diag_tail) {
        ++_diag_dropped;
        return;
      }
      ESPWMAPDiagEvent &e = _diag[_diag_head];
      e.phase = tag ? tag : "?";
      e.free8 = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
      e.largest8 = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      e.min8 = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
      _diag_head = next;
    }
#else
    void memdiag(const char *) {}
#endif
 
    typedef struct
    {
      String ssid;
      String pswd;
      int32_t rssi;
    } ap_info_t;
 
    std::vector<ap_info_t>  _aplist;
    std::vector<ap_info_t>  _apscan;
    std::vector<ap_info_t*> _apnext;
    unsigned long _disconnect;
    bool _connecting;
 
    void complete(int scanCount)
    {
      memdiag("complete entry");
      _apscan.clear();
      for (size_t i = 0; i < (size_t)scanCount; ++i)
      {
        auto scan = _apscan.begin();
        for (; scan != _apscan.end(); ++scan)
        {
          if (scan->ssid.equalsIgnoreCase(SSID(i)))
          {
            if (scan->rssi < RSSI(i))
              scan->rssi = RSSI(i);
            break;
          }
        }
        if (scan == _apscan.end())
        {
          ap_info_t ap = {SSID(i).c_str(), "", RSSI(i)};
          _apscan.push_back(ap);
        }
      }
      memdiag("complete vector");
      std::sort(_apscan.begin(), _apscan.end(),
        [](const ap_info_t& a, const ap_info_t& b)
        {
          return a.rssi > b.rssi;
        }
      );
      memdiag("complete sorted");
      scanDelete();
      memdiag("complete scanDelete");
      _apnext.clear();
      for (auto scan = _apscan.begin(); scan != _apscan.end(); ++scan)
      {
        for (auto list = _aplist.begin(); list != _aplist.end(); ++list)
        {
          if (list->ssid.equalsIgnoreCase(scan->ssid))
          {
            list->rssi = scan->rssi;
            _apnext.push_back(&*list);
            break;
          }
        }
      }
      memdiag("complete apnext");
    }
 
    ap_info_t* next(void)
    {
      ap_info_t* rv = NULL;
      auto ap = _apnext.begin();
      if (ap != _apnext.end())
      {
        rv = *ap;
        _apnext.erase(ap);
      }
      return rv;
    }
 
  public:
 
    ESPWMAPClass(void)
    : _connecting(false)
    {
      _disconnect = millis();
    }
 
    virtual ~ESPWMAPClass(void)
    {
    }

    bool popDiag(ESPWMAPDiagEvent *out)
    {
#if defined(ESP32) && ESPWMAP_MEMDIAG
      if (!out || _diag_tail == _diag_head) return false;
      *out = _diag[_diag_tail];
      _diag_tail = (uint8_t)((_diag_tail + 1U) % DIAG_RING_LEN);
      return true;
#else
      (void)out;
      return false;
#endif
    }

    uint32_t diagDropped(void) const
    {
#if defined(ESP32) && ESPWMAP_MEMDIAG
      return _diag_dropped;
#else
      return 0;
#endif
    }
 
    std::vector<String>& ssid(std::vector<String>& names)
    {
      names.clear();
      for (auto ap = _apscan.begin(); ap != _apscan.end(); ++ap)
        names.push_back(ap->ssid);
      return names;
    }
 
    bool timeouted(void)
    {
      return millis() - _disconnect >= ESPWMAP_CONNECTION_TIMEOUT;
    }
 
    void clear(void)
    {
      _aplist.clear();
    }
 
    size_t size(void)
    {
      return _aplist.size();
    }
 
    void add(const String& ssid, const String& pswd)
    {
      for (size_t i = 0; i < _aplist.size(); ++i)
      {
        if (_aplist[i].ssid.equalsIgnoreCase(ssid))
        {
          _aplist[i].pswd = pswd;
          return;
        }
      }
      ap_info_t ap = {ssid, pswd, 0};
      _aplist.push_back(ap);
    }
 
    void begin(void)
    {
      memdiag("begin entry");
      WiFi.persistent(false);
      memdiag("after persistent");
      WiFi.mode(WIFI_STA);
      memdiag("after WIFI_STA");
      _connecting = false;
    }
 
    wl_status_t handle(void)
    {
      yield(); // Run System Task
      switch (WiFi.status())
      {
        case WL_CONNECTED:
          _apnext.clear();
          _disconnect = millis();
          _connecting = false;
          break;
        default:
          _connecting = false;
          /* Falls through. */
        case WL_IDLE_STATUS:
        case WL_DISCONNECTED:
          if (_connecting)
            break;
          int scan = scanComplete();
          if (scan >= 0)
          {
            memdiag("scan done before");
            complete(scan);
            memdiag("scan done after");
#if ESPWMAP_DEBUG
            Serial.printf("WiFi Scan End (%d/%d)\r\n", (int)_apnext.size(), (int)_apscan.size()); 
#endif
            if (size() == 0)
            {
              _disconnect = millis() - ESPWMAP_CONNECTION_TIMEOUT;
              break;
            }
          }
          ap_info_t* ap = next();
          if (ap)
          {
#if ESPWMAP_DEBUG
            Serial.printf("WiFi Begin (%s)\r\n", ap->ssid.c_str()); 
#endif
            _connecting = true;
            memdiag("connect before");
            WiFi.begin(ap->ssid.c_str(), ap->pswd.c_str());
            memdiag("connect after");
          }
          else if (scan != WIFI_SCAN_RUNNING)
          {
#if ESPWMAP_DEBUG
            Serial.println("\r\nWiFi Scan Start"); 
#endif
            memdiag("scan start before");
#if CONFIG_IDF_TARGET_ESP32C3
            scanNetworks(true, false, false, 500);
#else
            scanNetworks(true);
#endif
            memdiag("scan start after");
          }
          break;
      }
      return WiFi.status();
    }
};
 
extern ESPWMAPClass ESPWMAP;
 
#endif
