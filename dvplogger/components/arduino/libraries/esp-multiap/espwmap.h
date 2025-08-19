/*
  espmwap.h - WiFi Multi AP Library for ESP8266/ESP32
 
  Copyright (c) 2023 Sasapea's Lab. All right reserved.
 
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
 
#define ESPWMAP_DEBUG 0
#define ESPWMAP_CONNECTION_TIMEOUT (3 * 60 * 1000) // ms
 
class ESPWMAPClass : private WiFiScanClass
{
  private:
 
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
      std::sort(_apscan.begin(), _apscan.end(),
        [](ap_info_t a, ap_info_t b)
        {
          return a.rssi > b.rssi;
        }
      );
      scanDelete();
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
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
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
            complete(scan);
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
            WiFi.begin(ap->ssid.c_str(), ap->pswd.c_str());
          }
          else if (scan != WIFI_SCAN_RUNNING)
          {
#if ESPWMAP_DEBUG
            Serial.println("\r\nWiFi Scan Start"); 
#endif
#if CONFIG_IDF_TARGET_ESP32C3
            scanNetworks(true, false, false, 500);
#else
            scanNetworks(true);
#endif
          }
          break;
      }
      return WiFi.status();
    }
};
 
extern ESPWMAPClass ESPWMAP;
 
#endif
