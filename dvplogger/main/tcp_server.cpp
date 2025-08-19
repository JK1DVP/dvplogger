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
#include <AsyncTCP.h>
#include <Print.h>

#include "decl.h"
#include "variables.h"
#include "cmd_interp.h"
#include "ui.h"
#include "usb_host.h"
#include "tcp_server.h"
#include "console.h"


#include <AsyncTCP.h>
#include <Print.h>

#include <AsyncTCP.h>
#include <Stream.h>
#include "esp_task_wdt.h"


// AsyncTCPBufferedStream.h
//#pragma once

#include <AsyncTCP.h>
#include <Arduino.h>
#include <Stream.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

class AsyncTCPBufferedStream : public Stream {
public:
    AsyncTCPBufferedStream(AsyncClient* client, size_t maxQueueSize = 10,
                           TickType_t ackTimeoutTicks = pdMS_TO_TICKS(1000),
                           size_t flushThreshold = 512,
                           TickType_t flushInterval = pdMS_TO_TICKS(100))
        : client(client), maxQueueSize(maxQueueSize), ackTimeout(ackTimeoutTicks),
          flushSizeThreshold(flushThreshold), flushTimeThreshold(flushInterval) {
        sendQueue = xQueueCreate(maxQueueSize, sizeof(SendBuffer));
        _writing = false;
        bufferLen = 0;
        lastFlushTime = xTaskGetTickCount();
        client->onAck([](void* arg, AsyncClient*, size_t len, uint32_t time) {
            static_cast<AsyncTCPBufferedStream*>(arg)->_writing = false;
	    //Serial.println("Ack");
        }, this);
        xTaskCreatePinnedToCore(senderTaskWrapper, "SenderTask", 4096, this, 1, &senderHandle, 1);
    }

    ~AsyncTCPBufferedStream() {
        if (senderHandle) vTaskDelete(senderHandle);
        if (sendQueue) vQueueDelete(sendQueue);
    }

    void setFlushThreshold(size_t bytes, TickType_t intervalTicks) {
        flushSizeThreshold = bytes;
        flushTimeThreshold = intervalTicks;
    }

    size_t write(uint8_t b) override {
        return write(&b, 1);
    }

    size_t write(const uint8_t* buffer, size_t size) override {
        if (!client || !client->connected()) return 0;
        size_t sent = 0;
        for (size_t i = 0; i < size; ++i) {
	  TickType_t now = xTaskGetTickCount();
	  if (bufferLen >= flushSizeThreshold ||
	                   (now - lastFlushTime) > flushTimeThreshold) {
	    flush();
	    lastFlushTime = now;
	    bufferLen=0;
	  }
	  bufferBuf[bufferLen++] = buffer[i];
	  sent++;	    
        }
        return sent;
    }

    size_t write(const char* str) {
        return write((const uint8_t*)str, strlen(str));
    }

    void flush() override {
        if (bufferLen == 0 || !client || !client->connected()) return;
        uint8_t* copy = (uint8_t*)malloc(bufferLen);
        if (!copy) return;
        memcpy(copy, bufferBuf, bufferLen);
        SendBuffer buf = {copy, bufferLen};
        if (xQueueSend(sendQueue, &buf, 0) != pdTRUE) {
            free(copy);
        }
        bufferLen = 0;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }

private:
    struct SendBuffer {
        uint8_t* data;
        size_t length;
    };

    AsyncClient* client;
    QueueHandle_t sendQueue;
    TaskHandle_t senderHandle = nullptr;
    size_t maxQueueSize;
    volatile bool _writing;
    TickType_t ackTimeout;

    uint8_t bufferBuf[1024];
    size_t bufferLen = 0;
    TickType_t lastFlushTime;
    size_t flushSizeThreshold;
    TickType_t flushTimeThreshold;

    void senderTaskImpl() {
        SendBuffer buf;
        while (xQueueReceive(sendQueue, &buf, portMAX_DELAY)) {
            while (client->space() < buf.length) {
                vTaskDelay(1);
            }
            _writing = true;
            client->write((const char*)buf.data, buf.length);

            // ACK待ち with timeout
            TickType_t startTick = xTaskGetTickCount();
	    //            while (_writing) {
	    //                if ((xTaskGetTickCount() - startTick) > ackTimeout) {
                    _writing = false;
		    //                    break;
		    //                }
		    //                vTaskDelay(1);
		    //            }

            free(buf.data);
        }
    }

    static void senderTaskWrapper(void* pvParameters) {
        AsyncTCPBufferedStream* self = static_cast<AsyncTCPBufferedStream*>(pvParameters);
        self->senderTaskImpl();
    }
};


  

//WiFiServer server(23);
//WiFiClient serverClients[MAX_SRV_CLIENTS];
//int serverClients_status[MAX_SRV_CLIENTS] ;
//int timeout_tcpserver = 0;

#define TCP_SERVER_PORT 23 // telnet port

#define N_TCPCLIENTS 2
int permits=N_TCPCLIENTS;

AsyncClient *clientPool[N_TCPCLIENTS];
AsyncTCPBufferedStream *streamWrapper[N_TCPCLIENTS];
int serverClients_status[N_TCPCLIENTS];

// changed to use AsyncTCP to serve
void process_clientData(int i, char *buf,int len); // i index of the client referred in serverClients_status[]
static void handleData(void *arg, AsyncClient *client, void *data, size_t len)
{
  //  Serial.printf("\n data received from client %s \n", client->remoteIP().toString().c_str());
  //  Serial.write((uint8_t *)data, len);

  //our big json string test
  //  String jsonString = "{\"data_from_module_type\":5,\"hub_unique_id\":\"hub-bfd\",\"slave_unique_id\":\"\",\"water_sensor\":{\"unique_id\":\"water-sensor-ba9\",\"firmware\":\"0.0.1\",\"hub_unique_id\":\"hub-bfd\",\"ip_address\":\"192.168.4.2\",\"mdns\":\"water-sensor-ba9.local\",\"pair_status\":127,\"ec\":{\"value\":0,\"calib_launch\":0,\"sensor_k_origin\":1,\"sensor_k_calibration\":1,\"calibration_solution\":1,\"regulation_state\":1,\"max_pumps_durations\":5000,\"set_point\":200},\"ph\":{\"value\":0,\"calib_launch\":0,\"regulation_state\":1,\"max_pumps_durations\":5000,\"set_point\":700},\"water\":{\"temperature\":0,\"pump_enable\":false}}}";
	// reply to client
  //  if (client->space() > strlen(jsonString.c_str()) && client->canSend())
  //    {
  //      client->add(jsonString.c_str(), strlen(jsonString.c_str()));
  //      client->send();
  //    }
  for (int i=0;i<N_TCPCLIENTS;i++) {
    if (clientPool[i]==client) {
      process_clientData(i, (char *)data,len); // i index of the client referred in serverClients_status[]
      break;
    }
  }
}


static void handleError(void *arg, AsyncClient *client, int8_t error)
{
  Serial.printf("\n connection error %s from client %s \n", client->errorToString(error), client->remoteIP().toString().c_str());
}

static void handleDisconnect(void *arg, AsyncClient *client)
{
  Serial.printf("\n client %s disconnected \n", client->remoteIP().toString().c_str());

  client->close(true);
  int flag=0;
  for (int i=0;i<N_TCPCLIENTS;i++) {  
    if (clientPool[i]==client) {
      clientPool[i]=NULL;
      delete streamWrapper[i];
      flag=1;
      break;
    }
  }
  if (!flag) {
    Serial.println("corresponding client not found in clientPool (strange)");
  }
  delete client;
  permits++;
}

static void handleTimeOut(void *arg, AsyncClient *client, uint32_t time)
{
  Serial.printf("\n client ACK timeout ip: %s \n", client->remoteIP().toString().c_str());
}

static void handleNewClient(void *arg, AsyncClient *client)
{
  if (!permits) {
    client->close(true);
    delete client;
    return;
  }

  int flag=0;
  for (int i=0;i<N_TCPCLIENTS;i++) {
    if (clientPool[i]==NULL) {
      clientPool[i]=client;
      streamWrapper[i] = new AsyncTCPBufferedStream(client);

      permits--;
      flag=1;
      break;
    }
  }
  if (!flag) {
    Serial.println("no free clientPool found (strange)");
  }
  
  Serial.printf("\n new client has been connected to server, ip: %s permits %d", client->remoteIP().toString().c_str(),permits);

  char buf[30];
  sprintf(buf,"permits=%d\n",permits);
  client->write(buf,strlen(buf));
  
  // register events
  client->onData(&handleData, NULL);
  client->onError(&handleError, NULL);
  client->onDisconnect(&handleDisconnect, NULL);
  client->onTimeout(&handleTimeOut, NULL);
}


void process_clientData(int i, char *buf,int len) // i index of the client referred in serverClients_status[]
{
  int ret;
  char c;
  static uint8_t mod_c; // storage of received modifier character  
  for (int j=0;j<len;j++) {
    //            c = serverClients[i].read();
    c = buf[j];

    if (plogw->f_console_emu) {
      //      plogw->ostream = &serverClients[i];
      //      emulate_keyboard(c);
      continue;
    }
	  
    if (isprint(c)) {
      plogw->ostream->print(c);
    } else {
      plogw->ostream->print(" "); plogw->ostream->print(c, HEX); plogw->ostream->print(" ");
    }
    // check keycode over tcp
    if (serverClients_status[i] == 0) {
      if (c >= 238) {
	if (c == 239) {
	  // key pressed
	  serverClients_status[i] = 2;
	  break;
	}
	if (c == 238) {
	  // key released
	  serverClients_status[i] = 3;
	  break;
	}
	// check telnet commands
	if (c == 255) {
	  // IAC
	}
	serverClients_status[i] = 0;
	continue;
      }
    } else if (serverClients_status[i] == 1) {
      // telnet commands
      continue;
    } else if (serverClients_status[i] == 4) {
      // void KbdRptParser::OnKeyDown(uint8_t mod, uint8_t key) {
      // mod : or'ed 0x01 control 0x02 left_shift 0x04 alt 0x08 command 0x10 right_shift ? 0x40 alt 0x80 command
      // key : USB HID scan code for the key
      //KbdRptParser::OnKeyDown((unit8_t mod),(uint8_t c));
      uint8_t mod;
      mod = 0;
      MODIFIERKEYS modkey;
      *((uint8_t *)&modkey) = mod_c;
      uint8_t c_to_ascii = kbd_oemtoascii2(mod,c);
      //on_key_down(modkey, (uint8_t)key, (uint8_t)c);

      on_key_down(modkey, (uint8_t)c, (uint8_t) c_to_ascii);
      // key pressed event
      serverClients_status[i] = 0; // finished processing keydown sequence
      continue;
    } else if (serverClients_status[i] == 5) {
      // key released event
      continue;
    } else if (serverClients_status[i] == 2) {
      // after recieved
      mod_c = c;
      serverClients_status[i] = 4;
      continue;
    } else if (serverClients_status[i] == 3) {
      mod_c = c;
      serverClients_status[i] = 5;
      continue;
    }

    // not special character put into ring line buffer
    write_ringbuf(&(plogw->tcp_ringbuf), c);
    // and check line
    ret = readfrom_ringbuf(&plogw->tcp_ringbuf, plogw->tcp_cmdbuf + plogw->tcp_cmdbuf_ptr, (char)0x0d, (char)0x0a, plogw->tcp_cmdbuf_len - plogw->tcp_cmdbuf_ptr);
    if (ret < 0) {
      // one line read
      
      plogw->ostream->print("tcp readline:");
      plogw->ostream->println(plogw->tcp_cmdbuf);
      plogw->ostream =streamWrapper[i]; // redirect to TCP client
      // check 'exit' command 
      if (strcmp(plogw->tcp_cmdbuf,"exit")==0) {
	plogw->ostream->println("exit from the terminal");
	clientPool[i]->close(true);
	delete clientPool[i];	
	clientPool[i]=NULL;
	delete streamWrapper[i];
	serverClients_status[i]=0;
	plogw->ostream=console;	      
      } else {

	// WDT timeout longer for this part only
	// インタプリタ処理を呼び出す前にWDTタイムアウトを長く設定
	esp_task_wdt_init(10, true);  // タイムアウトを10秒に延長
	esp_task_wdt_reset();  // WDTをリセット
	
	cmd_interp(plogw->tcp_cmdbuf); // pass to command interpreter

	// インタプリタ処理を呼び出す前にWDTタイムアウトを長く設定
	esp_task_wdt_init(10, true);  // タイムアウトを10秒に延長
	esp_task_wdt_reset();  // WDTをリセット
	
	plogw->ostream = console; // change output stream back to serial port
      }
      *plogw->tcp_cmdbuf = '\0';
      plogw->tcp_cmdbuf_ptr = 0;
    } else {
      if (ret > 0 ) {
	plogw->tcp_cmdbuf_ptr += ret;
      }
      if (plogw->tcp_cmdbuf_ptr == plogw->tcp_cmdbuf_len) {
	plogw->tcp_cmdbuf_ptr = 0;
      }
    }
  }
}





void write_allTCPclients(char *buf,int len) // send buf to all client
{
  for (int i=0;i<N_TCPCLIENTS;i++) {
    if (clientPool[i]!=NULL) {
      clientPool[i]->write(buf,len);
    }
  }
}
  
void print_allTCPclients(char *buf)
{
  write_allTCPclients(buf,strlen(buf));
}

void init_tcpserver()
{
  AsyncServer *server = new AsyncServer(TCP_SERVER_PORT); // start listening on tcp port 7050
  for (int i=0;i<N_TCPCLIENTS;i++) {
    clientPool[i]=NULL;
    serverClients_status[i]=0;
    streamWrapper[i]=NULL;
  }
  
  server->onClient(&handleNewClient, server);
  server->begin();
}


////// older, will be integrated new handle data functions

