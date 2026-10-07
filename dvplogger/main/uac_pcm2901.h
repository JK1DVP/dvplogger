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
/*
 * Minimal USB Audio Class 1 capture support for the TI PCM2901 used by IC-705.
 *
 * This is intentionally a receive-only proof of concept.  The radio exposes
 * the PCM2901 as a separate USB device behind its internal hub.  Audio is
 * received as 48 kHz / 16-bit / stereo isochronous PCM; the left channel is
 * stored as 48 kHz / 16-bit mono in PSRAM for later FT8/DSP processing.
 */
#ifndef FILE_UAC_PCM2901_H
#define FILE_UAC_PCM2901_H

#include <Arduino.h>
#include <Usb.h>

class PCMAudioCapture : public USBDeviceConfig {
 public:
  static const uint16_t kVid = 0x08bb;
  static const uint16_t kPid = 0x2901;
  static const uint32_t kSampleRate = 48000;
  static const uint32_t kCaptureSeconds = 15;
  static const size_t kCaptureSamples = kSampleRate * kCaptureSeconds;

  explicit PCMAudioCapture(USB *usb);

  uint8_t Init(uint8_t parent, uint8_t port, bool lowspeed) override;
  uint8_t Release() override;
  uint8_t Poll() override;
  uint8_t GetAddress() override { return bAddress; }
  bool VIDPIDOK(uint16_t vid, uint16_t pid) override {
    return vid == kVid && pid == kPid;
  }

  bool ready() const { return bAddress != 0 && bReady; }
  bool capturing() const { return fCapture; }
  bool start(Print *out);
  void stop(Print *out);
  void status(Print *out) const;
  void freeBuffer(Print *out);
  void setSofSync(bool enable, Print *out);
  void diagnose(Print *out);
  bool sofSync() const { return fSofSync; }

  const int16_t *buffer() const { return captureBuf; }
  size_t samples() const { return captureSamples; }

 private:
  USB *pUsb;
  uint8_t bAddress;
  bool bReady;
  bool fCapture;
  EpInfo epInfo[2];

  // PCM2901 UAC1 layout used by the IC-705.
  static const uint8_t kConfiguration = 1;
  static const uint8_t kCaptureInterface = 2;
  static const uint8_t kCaptureAltSetting = 1;
  static const uint8_t kCaptureEndpoint = 0x82;
  static const uint8_t kCaptureEndpointNumber = 2;
  static const uint16_t kCaptureMaxPacket = 196;

  int16_t *captureBuf;
  size_t captureSamples;
  uint32_t isoPackets;
  uint32_t isoErrors;
  uint32_t isoResult[16];  // MAX3421E HRSL result nibble counters (0x0..0xF)
  uint32_t noRcvDav;       // transfer completed without receive data
  uint32_t isoRawBytes;     // raw stereo bytes drained from MAX3421E RCVFIFO
  uint32_t isoFifoChunks;   // number of RCVFIFO chunks drained
  uint32_t isoHxfrDone;     // HXFRDNIRQ observations
  uint32_t isoWaitTimeouts; // direct ISO transfer wait timeouts
  bool fSofSync;            // wait for a fresh SOF edge before each ISO IN
  uint32_t sofWaits;
  uint32_t sofTimeouts;
  uint8_t jerrDiagPrinted;
  uint8_t lastIsoError;
  uint32_t malformedBytes;
  int16_t minSample;
  int16_t maxSample;
  uint32_t startedMs;
  uint32_t finishedMs;

  uint8_t setInterface(uint8_t iface, uint8_t alt);
  uint8_t getInterface(uint8_t iface, uint8_t *alt);
  uint8_t setSampleRate48k();
  uint8_t getSampleRate(uint32_t *hz);
  void dumpConfiguration(Print *out);
  uint8_t receiveIsoPacket(uint8_t *buf, uint8_t *len);
  void drainStaleReceiveFifo();
  bool waitFreshSof();
  void dumpJerrRegisters(uint8_t result);
  void consumeStereo16(const uint8_t *buf, uint8_t len);
  void resetStats();
};

#endif
