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
#include <AsyncTCP.h>
#include <Print.h>

#include "decl.h"
#include "variables.h"
#include "cmd_interp.h"
#include "ui.h"
#include "main.h"
#include "usb_host.h"
#include "tcp_server.h"
#include "console.h"
#include "misc.h"
#include "network_arena.h"


#include <AsyncTCP.h>
#include <Print.h>

#include <AsyncTCP.h>
#include <Stream.h>
#include "esp_task_wdt.h"
#include <new>
#include <limits.h>
#include <stdarg.h>

static void telnet_diag(const char *fmt, ...) {
    if (!lowmem_trace || !local_console || !fmt) return;
    char line[192];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    local_console->print("[TELNETDBG] "); local_console->print(line);
}

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
        telnet_diag("stream ctor sizeof=%u buffer=%u senditem=%u qlen=%u free=%u\n", (unsigned)sizeof(*this), (unsigned)sizeof(bufferBuf), (unsigned)sizeof(SendBuffer), (unsigned)maxQueueSize, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        sendQueue = xQueueCreate(maxQueueSize, sizeof(SendBuffer));
        telnet_diag("send queue created handle=%p free=%u\n", sendQueue, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        _writing = false;
        bufferLen = 0;
        lastFlushTime = xTaskGetTickCount();
        client->onAck([](void* arg, AsyncClient*, size_t len, uint32_t time) {
            static_cast<AsyncTCPBufferedStream*>(arg)->_writing = false;
	    //console->println("Ack");
        }, this);
        BaseType_t task_rc = xTaskCreatePinnedToCore(senderTaskWrapper, "SenderTask", 2560, this, 1, &senderHandle, 1);
        telnet_diag("sender task create rc=%d handle=%p free=%u\n", (int)task_rc, senderHandle, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    }

    ~AsyncTCPBufferedStream() {
        stopping = true;
        if (senderHandle) {
            vTaskDelete(senderHandle);
            senderHandle = nullptr;
        }
        // If the sender task was deleted while holding a block outside the
        // queue, return it as well or the shared arena would remain in
        // TELNET mode indefinitely.
        if (senderActiveBlock) {
            network_arena_telnet_release((uint8_t *)senderActiveBlock);
            senderActiveBlock = nullptr;
        }
        if (sendQueue) {
            SendBuffer pending;
            while (xQueueReceive(sendQueue, &pending, 0) == pdTRUE) {
                network_arena_telnet_release(pending.data);
            }
            vQueueDelete(sendQueue);
            sendQueue = nullptr;
        }
    }

    void setFlushThreshold(size_t bytes, TickType_t intervalTicks) {
        flushSizeThreshold = bytes;
        flushTimeThreshold = intervalTicks;
    }

    size_t write(uint8_t b) override {
        return write(&b, 1);
    }

    size_t write(const uint8_t* buffer, size_t size) override {
        if (!client || !client->connected()) { telnet_diag("TX write rejected size=%u client=%p connected=%d\n", (unsigned)size, client, client ? client->connected() : 0); return 0; }
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
        // Reuse the common 6144-byte network arena instead of allocating a
        // fresh heap chunk for every Telnet flush.  Web owns the arena
        // exclusively while formatting a heavy response, so wait briefly for
        // that lease (or for a queued Telnet block) to clear.
        uint8_t* copy = nullptr;
        const TickType_t arenaStart = xTaskGetTickCount();
        do {
            copy = network_arena_telnet_acquire(bufferLen);
            if (copy) break;
            vTaskDelay(1);
        } while ((xTaskGetTickCount() - arenaStart) < pdMS_TO_TICKS(100));
        if (!copy) {
            NetworkArenaStatus st = {};
            network_arena_get_status(&st);
            telnet_diag("TX arena busy/drop len=%u mode=%u blocks=%u free=%u largest=%u\n",
                        (unsigned)bufferLen, (unsigned)st.mode,
                        (unsigned)st.telnet_blocks,
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            bufferLen = 0;
            ++txDroppedChunks;
            return;
        }
        memcpy(copy, bufferBuf, bufferLen);
        SendBuffer buf = {copy, bufferLen};
        // Apply bounded backpressure instead of immediately spinning when TCP is
        // congested.  Never block indefinitely: after 100 ms drop this chunk.
        BaseType_t qrc = xQueueSend(sendQueue, &buf, pdMS_TO_TICKS(100));
        UBaseType_t qwait = uxQueueMessagesWaiting(sendQueue);
        if (qwait > txQueueHighWater) {
            txQueueHighWater = qwait;
            if (qwait == 5 || qwait == 8 || qwait >= maxQueueSize) {
                telnet_diag("TX queue high-water=%u/%u drops=%u free=%u\n",
                            (unsigned)qwait, (unsigned)maxQueueSize,
                            (unsigned)txDroppedChunks,
                            (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            }
        }
        if (qrc != pdTRUE) {
            ++txDroppedChunks;
            telnet_diag("TX queue timeout/drop len=%u qwait=%u/%u drops=%u free=%u largest=%u\n",
                        (unsigned)buf.length, (unsigned)qwait, (unsigned)maxQueueSize,
                        (unsigned)txDroppedChunks,
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            network_arena_telnet_release(copy);
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
    volatile bool stopping = false;
    TickType_t ackTimeout;

    uint8_t bufferBuf[512];
    size_t bufferLen = 0;
    TickType_t lastFlushTime;
    size_t flushSizeThreshold;
    TickType_t flushTimeThreshold;
    UBaseType_t txQueueHighWater = 0;
    uint32_t txDroppedChunks = 0;
    UBaseType_t senderMinWatermark = UINT_MAX;
    volatile uint8_t *senderActiveBlock = nullptr;

    void senderTaskImpl() {
        SendBuffer buf;
        senderMinWatermark = uxTaskGetStackHighWaterMark(nullptr);
        telnet_diag("sender task start watermark=%u\n", (unsigned)senderMinWatermark);
        while (!stopping && xQueueReceive(sendQueue, &buf, portMAX_DELAY) == pdTRUE) {
            senderActiveBlock = buf.data;
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
            if (watermark < senderMinWatermark) {
                senderMinWatermark = watermark;
                telnet_diag("sender new low watermark=%u used~=%u qwait=%u/%u\n",
                            (unsigned)watermark, (unsigned)(2560 - watermark),
                            (unsigned)uxQueueMessagesWaiting(sendQueue),
                            (unsigned)maxQueueSize);
            }
            while (!stopping && client && client->connected() &&
                   client->space() < buf.length) {
                vTaskDelay(1);
            }
            if (stopping || !client || !client->connected()) {
                network_arena_telnet_release(buf.data);
                senderActiveBlock = nullptr;
                continue;
            }
            _writing = true;
            size_t nw = client->write((const char*)buf.data, buf.length);
            if (nw != buf.length) {
                telnet_diag("sender short write requested=%u returned=%u space_after=%u\n",
                            (unsigned)buf.length, (unsigned)nw,
                            (unsigned)client->space());
            }

            // ACK待ち with timeout
            TickType_t startTick = xTaskGetTickCount();
	    //            while (_writing) {
	    //                if ((xTaskGetTickCount() - startTick) > ackTimeout) {
                    _writing = false;
		    //                    break;
		    //                }
		    //                vTaskDelay(1);
		    //            }

            network_arena_telnet_release(buf.data);
            senderActiveBlock = nullptr;
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

#define TCP_SERVER_PORT 23
#define N_TCPCLIENTS 2
#define TELNET_EVENT_DATA_SIZE 64
#define TELNET_EVENT_QUEUE_LEN 16
#define TELNET_COMMAND_SIZE 128

enum TelnetEventType : uint8_t {
  TELNET_EVENT_DATA,
  TELNET_EVENT_DISCONNECT,
  TELNET_EVENT_ERROR,
  TELNET_EVENT_TIMEOUT,
  TELNET_EVENT_CONNECT
};

struct TelnetEvent {
  TelnetEventType type;
  uint8_t slot;
  uint8_t len;
  int16_t value;
  AsyncClient *client;
  char data[TELNET_EVENT_DATA_SIZE];
};

struct TelnetClientState {
  char command[TELNET_COMMAND_SIZE + 1];
  uint16_t command_len;
  uint8_t protocol_state;
  uint8_t modifier;
};

static AsyncClient *clientPool[N_TCPCLIENTS];
static AsyncTCPBufferedStream *streamWrapper[N_TCPCLIENTS];
static TelnetClientState telnetState[N_TCPCLIENTS];
static QueueHandle_t telnetEventQueue = nullptr;
static AsyncServer *telnetServer = nullptr;
static portMUX_TYPE telnetPoolMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t telnetDroppedEvents = 0;
static volatile bool telnetDisconnectPending[N_TCPCLIENTS] = {false, false};
static int permits = N_TCPCLIENTS;
// Only one Telnet terminal is logically active.  The second slot exists only
// long enough to accept a replacement connection and disconnect the old one.
static int activeTelnetSlot = -1;

static int findClientSlot(AsyncClient *client) {
  int slot = -1;
  portENTER_CRITICAL(&telnetPoolMux);
  for (int i = 0; i < N_TCPCLIENTS; ++i) {
    if (clientPool[i] == client) {
      slot = i;
      break;
    }
  }
  portEXIT_CRITICAL(&telnetPoolMux);
  return slot;
}

static bool enqueueTelnetEvent(const TelnetEvent& ev) {
  BaseType_t rc = telnetEventQueue ? xQueueSend(telnetEventQueue, &ev, 0) : pdFALSE;
  telnet_diag("event enqueue type=%u slot=%u len=%u rc=%d qwait=%u\n", (unsigned)ev.type, (unsigned)ev.slot, (unsigned)ev.len, (int)rc, telnetEventQueue ? (unsigned)uxQueueMessagesWaiting(telnetEventQueue) : 0);
  if (!telnetEventQueue || rc != pdTRUE) {
    ++telnetDroppedEvents;
    return false;
  }
  return true;
}

static void handleData(void *, AsyncClient *client, void *data, size_t len) {
  int slot = findClientSlot(client);
  if (slot < 0 || !data) return;

  telnet_diag("RX callback slot=%d len=%u free=%u\n", slot, (unsigned)len, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  const char *src = static_cast<const char *>(data);
  while (len > 0) {
    TelnetEvent ev{};
    ev.type = TELNET_EVENT_DATA;
    ev.slot = (uint8_t)slot;
    ev.client = client;
    ev.len = (uint8_t)min(len, (size_t)TELNET_EVENT_DATA_SIZE);
    memcpy(ev.data, src, ev.len);
    if (!enqueueTelnetEvent(ev)) break;
    src += ev.len;
    len -= ev.len;
  }
}

static void handleError(void *, AsyncClient *client, int8_t error) {
  int slot = findClientSlot(client);
  if (slot < 0) return;
  TelnetEvent ev{};
  ev.type = TELNET_EVENT_ERROR;
  ev.slot = (uint8_t)slot;
  ev.client = client;
  ev.value = error;
  enqueueTelnetEvent(ev);
}

static void handleDisconnect(void *, AsyncClient *client) {
  int slot = findClientSlot(client);
  if (slot < 0) return;
  // A disconnect must not be lost even when the event queue is full.
  telnetDisconnectPending[slot] = true;
}

static void handleTimeOut(void *, AsyncClient *client, uint32_t time) {
  int slot = findClientSlot(client);
  if (slot < 0) return;
  TelnetEvent ev{};
  ev.type = TELNET_EVENT_TIMEOUT;
  ev.slot = (uint8_t)slot;
  ev.client = client;
  ev.value = (int16_t)min(time, (uint32_t)INT16_MAX);
  enqueueTelnetEvent(ev);
}

static void handleNewClient(void *, AsyncClient *client) {
  if (!client) return;

  memtrace_event("telnet connect callback");

  int slot = -1;
  portENTER_CRITICAL(&telnetPoolMux);
  for (int i = 0; i < N_TCPCLIENTS; ++i) {
    if (clientPool[i] == nullptr) {
      clientPool[i] = client;
      slot = i;
      --permits;
      break;
    }
  }
  portEXIT_CRITICAL(&telnetPoolMux);

  if (slot < 0) {
    client->close(true);
    delete client;
    return;
  }

  memtrace_event("before telnet stream");
  streamWrapper[slot] = new (std::nothrow) AsyncTCPBufferedStream(client);
  memtrace_event("after telnet stream");
  if (!streamWrapper[slot]) {
    portENTER_CRITICAL(&telnetPoolMux);
    clientPool[slot] = nullptr;
    ++permits;
    portEXIT_CRITICAL(&telnetPoolMux);
    client->close(true);
    delete client;
    return;
  }

  memset(&telnetState[slot], 0, sizeof(telnetState[slot]));
  client->onData(&handleData, nullptr);
  client->onError(&handleError, nullptr);
  client->onDisconnect(&handleDisconnect, nullptr);
  client->onTimeout(&handleTimeOut, nullptr);

  TelnetEvent ev{};
  ev.type = TELNET_EVENT_CONNECT;
  ev.slot = (uint8_t)slot;
  ev.client = client;
  enqueueTelnetEvent(ev);
  memtrace_event("after telnet connect setup");
}

static void closeTelnetClient(int slot, AsyncClient *expected) {
  if (slot < 0 || slot >= N_TCPCLIENTS) return;

  AsyncClient *client = nullptr;
  AsyncTCPBufferedStream *stream = nullptr;
  portENTER_CRITICAL(&telnetPoolMux);
  if (clientPool[slot] == expected) {
    client = clientPool[slot];
    stream = streamWrapper[slot];
    clientPool[slot] = nullptr;
    streamWrapper[slot] = nullptr;
    ++permits;
  }
  portEXIT_CRITICAL(&telnetPoolMux);

  if (!client) return;

  if (activeTelnetSlot == slot) activeTelnetSlot = -1;

  // Return output to the serial instance currently allocated as the local
  // console. UART0 may meanwhile belong to CAT.
  Stream *fallback = local_console ? local_console : &Serial;
  if (console == stream) console = fallback;
  if (plogw && plogw->ostream == stream) plogw->ostream = fallback;
  rebind_memstat_output(stream, fallback);

  delete stream;
  client->onData(nullptr, nullptr);
  client->onError(nullptr, nullptr);
  client->onDisconnect(nullptr, nullptr);
  client->onTimeout(nullptr, nullptr);
  if (client->connected()) client->close(true);
  delete client;
  memset(&telnetState[slot], 0, sizeof(telnetState[slot]));
}

static void executeTelnetCommand(int slot) {
  TelnetClientState& state = telnetState[slot];
  state.command[state.command_len] = '\0';
  telnet_diag("command slot=%d len=%u text=<%s>\n", slot, (unsigned)state.command_len, state.command);
  AsyncTCPBufferedStream *stream = streamWrapper[slot];
  AsyncClient *client = clientPool[slot];
  if (!stream || !client) {
    state.command_len = 0;
    return;
  }

  if (strcmp(state.command, "exit") == 0) {
    stream->println("exit from the terminal");
    stream->flush();
    client->close(true);
    state.command_len = 0;
    return;
  }

  telnet_diag("command before cmd_interp slot=%d\n", slot);
  cmd_interp(state.command, stream);
  telnet_diag("command after cmd_interp slot=%d; flushing\n", slot);
  stream->flush();
  state.command_len = 0;
}

static void processTelnetByte(int slot, uint8_t c) {
  TelnetClientState& state = telnetState[slot];

  // Existing private keyboard framing: 239/mod/key and 238/mod/key.
  if (state.protocol_state == 2) {
    state.modifier = c;
    state.protocol_state = 4;
    return;
  }
  if (state.protocol_state == 3) {
    state.modifier = c;
    state.protocol_state = 5;
    return;
  }
  if (state.protocol_state == 4) {
    MODIFIERKEYS modkey;
    *((uint8_t *)&modkey) = state.modifier;
    uint8_t ascii = kbd_oemtoascii2(state.modifier, c);
    if ((verbose & 16) && (c == 0x36 || c == 0x37)) {
      console->printf("KBDLOW t=%lu src=TCP hid=0x%02X mod=0x%02X on=1\n",
                    (unsigned long)millis(), (unsigned int)c,
                    (unsigned int)state.modifier);
    }
    on_key_down(modkey, c, ascii);
    state.protocol_state = 0;
    return;
  }
  if (state.protocol_state == 5) {
    state.protocol_state = 0;
    return;
  }
  if (c == 239) {
    state.protocol_state = 2;
    return;
  }
  if (c == 238) {
    state.protocol_state = 3;
    return;
  }

  // Minimal telnet IAC suppression. The following two option bytes are ignored.
  if (state.protocol_state == 10) {
    state.protocol_state = 11;
    return;
  }
  if (state.protocol_state == 11) {
    state.protocol_state = 0;
    return;
  }
  if (c == 255) {
    state.protocol_state = 10;
    return;
  }

  if (c == '\n') return;
  if (c == '\r') {
    executeTelnetCommand(slot);
    return;
  }
  if (c == 8 || c == 127) {
    if (state.command_len > 0) --state.command_len;
    return;
  }
  if (!isprint(c)) return;

  if (state.command_len < TELNET_COMMAND_SIZE) {
    state.command[state.command_len++] = (char)c;
  } else {
    state.command_len = 0;
    if (streamWrapper[slot]) {
      streamWrapper[slot]->println("command too long; discarded");
      streamWrapper[slot]->flush();
    }
  }
}

void process_tcpserver() {
  if (!telnetEventQueue) return;

  for (int i = 0; i < N_TCPCLIENTS; ++i) {
    if (telnetDisconnectPending[i]) {
      telnetDisconnectPending[i] = false;
      AsyncClient *client = clientPool[i];
      if (client) {
        console->printf("telnet: disconnected slot=%d\n", i);
        closeTelnetClient(i, client);
      }
    }
  }

  TelnetEvent ev;
  int budget = 16;
  while (budget-- > 0 && xQueueReceive(telnetEventQueue, &ev, 0) == pdTRUE) {
    telnet_diag("event dequeue type=%u slot=%u len=%u qremain=%u\n", (unsigned)ev.type, (unsigned)ev.slot, (unsigned)ev.len, (unsigned)uxQueueMessagesWaiting(telnetEventQueue));
    if (ev.slot >= N_TCPCLIENTS || clientPool[ev.slot] != ev.client) { telnet_diag("event discarded stale/invalid slot=%u\n", (unsigned)ev.slot); continue; }

    switch (ev.type) {
      case TELNET_EVENT_DATA:
        for (uint8_t i = 0; i < ev.len; ++i) processTelnetByte(ev.slot, (uint8_t)ev.data[i]);
        break;
      case TELNET_EVENT_CONNECT: {
        // A newly connected terminal always takes ownership.  Keep two
        // internal slots only so that the replacement can connect before the
        // previous terminal is forcibly closed.
        int oldSlot = activeTelnetSlot;
        if (oldSlot >= 0 && oldSlot != ev.slot &&
            clientPool[oldSlot] && streamWrapper[oldSlot]) {
          streamWrapper[oldSlot]->println("another terminal connected; exiting");
          streamWrapper[oldSlot]->flush();
          AsyncClient *oldClient = clientPool[oldSlot];
          closeTelnetClient(oldSlot, oldClient);
        }

        console->printf("telnet: connected %s slot=%u permits=%d\n",
                      ev.client->remoteIP().toString().c_str(), ev.slot, permits);
        if (streamWrapper[ev.slot] && clientPool[ev.slot] == ev.client) {
          activeTelnetSlot = ev.slot;
          console = streamWrapper[ev.slot];
          if (plogw) plogw->ostream = streamWrapper[ev.slot];
          streamWrapper[ev.slot]->println("connected; this terminal is now active");
          streamWrapper[ev.slot]->flush();
        }
        break;
      }
      case TELNET_EVENT_ERROR:
        console->printf("telnet: error slot=%u code=%d\n", ev.slot, ev.value);
        break;
      case TELNET_EVENT_TIMEOUT:
        console->printf("telnet: ACK timeout slot=%u\n", ev.slot);
        break;
      case TELNET_EVENT_DISCONNECT:
        // Disconnects are handled through telnetDisconnectPending.
        break;
    }
  }

  if (telnetDroppedEvents) {
    uint32_t dropped = telnetDroppedEvents;
    telnetDroppedEvents = 0;
    console->printf("telnet: %lu event(s) dropped\n", (unsigned long)dropped);
  }
}

void write_allTCPclients(char *buf, int len) {
  if (!buf || len <= 0) return;
  for (int i = 0; i < N_TCPCLIENTS; ++i) {
    if (streamWrapper[i] && clientPool[i] && clientPool[i]->connected()) {
      // BufferedPrintStream already flushes by size or timeout.  Forcing a
      // network flush here makes QSO confirmation wait on every Telnet client.
      streamWrapper[i]->write((const uint8_t *)buf, (size_t)len);
    }
  }
}

void print_allTCPclients(char *buf) {
  if (buf) write_allTCPclients(buf, strlen(buf));
}

void init_tcpserver() {
  telnet_diag("static sizes event=%u eventQ=%u*%u=%u state=%u*%u=%u stream=%u\n", (unsigned)sizeof(TelnetEvent), (unsigned)TELNET_EVENT_QUEUE_LEN, (unsigned)sizeof(TelnetEvent), (unsigned)(TELNET_EVENT_QUEUE_LEN * sizeof(TelnetEvent)), (unsigned)sizeof(TelnetClientState), (unsigned)N_TCPCLIENTS, (unsigned)(N_TCPCLIENTS * sizeof(TelnetClientState)), (unsigned)sizeof(AsyncTCPBufferedStream));
  memtrace_event("telnet init entry");
  telnetEventQueue = xQueueCreate(TELNET_EVENT_QUEUE_LEN, sizeof(TelnetEvent));
  memtrace_event("after telnet event queue");
  if (!telnetEventQueue) {
    console->println("telnet: cannot allocate event queue");
    return;
  }

  for (int i = 0; i < N_TCPCLIENTS; ++i) {
    clientPool[i] = nullptr;
    streamWrapper[i] = nullptr;
    memset(&telnetState[i], 0, sizeof(telnetState[i]));
    telnetDisconnectPending[i] = false;
  }

  memtrace_event("before new AsyncServer");
  telnetServer = new (std::nothrow) AsyncServer(TCP_SERVER_PORT);
  memtrace_event("after new AsyncServer");
  if (!telnetServer) {
    console->println("telnet: cannot allocate AsyncServer");
    vQueueDelete(telnetEventQueue);
    telnetEventQueue = nullptr;
    return;
  }
  telnetServer->onClient(&handleNewClient, telnetServer);
  memtrace_event("after telnet onClient");
  telnetServer->begin();
  memtrace_event("after telnet begin");
}
