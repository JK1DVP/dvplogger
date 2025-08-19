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

// --- cdcacm.h (簡易版) ---

#ifndef __CDCACM_H__
#define __CDCACM_H__

#include <usbhub.h>
#include <stdio.h> // for printf if available

class ACM : public USBDeviceConfig, public UsbConfigXtracter {
public:
    ACM(USBHost *usb);

    uint8_t Init(uint8_t parent, uint8_t port, bool lowspeed) override;
    uint8_t Release() override;
    uint8_t Poll() override;
    uint8_t RcvData(uint8_t *data, uint16_t *length);
    uint8_t SendData(const uint8_t *data, uint16_t length);
    uint8_t SetControlLineState(uint8_t dtr, uint8_t rts);
    uint8_t SetLineCoding(uint32_t baudrate, uint8_t stopBits, uint8_t parity, uint8_t dataBits);

    bool isReady() const { return ready; }

    void EndpointXtract(uint8_t conf, uint8_t iface, uint8_t alt, uint8_t proto,
                        const USB_ENDPOINT_DESCRIPTOR *ep) override;

private:
    USBHost *pUsb;
    uint8_t bAddress;
    bool ready;

    EpInfo epInfo[4];
    uint8_t bControlIf;
    uint8_t bDataIf;

    uint8_t epInterruptIn;
    uint8_t epDataIn;
    uint8_t epDataOut;

    void ParseInterfaceDescriptor(const uint8_t *buff);
};

#endif
