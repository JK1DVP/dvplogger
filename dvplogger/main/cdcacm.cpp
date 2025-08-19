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

// --- cdcacm.cpp (簡易版) ---

#include "cdcacm.h"

ACM::ACM(USBHost *usb) : pUsb(usb), bAddress(0), ready(false),
                         bControlIf(0xFF), bDataIf(0xFF),
                         epInterruptIn(0), epDataIn(0), epDataOut(0) {
    epInfo[0].epAddr = 0;
    epInfo[0].maxPktSize = 8;
    epInfo[0].epAttribs = 0;
    epInfo[0].bmNakPower = USB_NAK_NOWAIT;
}

uint8_t ACM::Init(uint8_t parent, uint8_t port, bool lowspeed) {
    USB_HOST_DEVICE *dev = nullptr;
    pUsb->getUsbDevicePtr(parent, &dev);
    if (!dev) return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;

    if (ready || bControlIf == 0xFF || bDataIf == 0xFF) {
        printf("[ACM] Init skipped: ready=%d bControlIf=0x%02X bDataIf=0x%02X\n", ready, bControlIf, bDataIf);
        return USB_DEV_CONFIG_ERROR_DEVICE_NOT_SUPPORTED;
    }

    bAddress = dev->address;
    EpInfo *oldEpInfo;
    pUsb->getEpInfoEntry(bAddress, 0, &oldEpInfo);
    pUsb->setEpInfoEntry(bAddress, 1, epInfo);

    ready = true;
    printf("[ACM] Init success: address=0x%02X ControlIf=%u DataIf=%u\n", bAddress, bControlIf, bDataIf);

    // 初期化時に DTR/RTS を ON に設定
    SetControlLineState(1, 1);

    // 初期化時に LineCoding (115200 8N1) を設定
    SetLineCoding(115200, 0, 0, 8);

    return 0;
}

uint8_t ACM::Release() {
    printf("[ACM] Release called. Resetting state.\n");
    bAddress = 0;
    ready = false;
    bControlIf = 0xFF;
    bDataIf = 0xFF;
    return 0;
}

uint8_t ACM::Poll() {
    return 0;
}

uint8_t ACM::RcvData(uint8_t *data, uint16_t *length) {
    if (!ready || !data || !length) return USB_ERROR_INVALID_ARGUMENT;
    return pUsb->inTransfer(bAddress, epDataIn, length, data);
}

uint8_t ACM::SendData(const uint8_t *data, uint16_t length) {
    if (!ready || !data) return USB_ERROR_INVALID_ARGUMENT;
    return pUsb->outTransfer(bAddress, epDataOut, length, const_cast<uint8_t*>(data));
}

uint8_t ACM::SetControlLineState(uint8_t dtr, uint8_t rts) {
    if (!ready || bAddress == 0 || bControlIf == 0xFF)
        return USB_ERROR_DEVICE_NOT_SUPPORTED;

    uint8_t lineState = 0;
    if (dtr) lineState |= 0x01;
    if (rts) lineState |= 0x02;

    uint8_t rcode = pUsb->ctrlReq(
        bAddress, 0,
        USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_CLASS | USB_SETUP_RECIPIENT_INTERFACE,
        0x22,
        lineState,
        bControlIf,
        0, nullptr, nullptr
    );

    printf("[ACM] SetControlLineState: DTR=%u RTS=%u => rcode=0x%02X\n", dtr, rts, rcode);
    return rcode;
}

uint8_t ACM::SetLineCoding(uint32_t baudrate, uint8_t stopBits, uint8_t parity, uint8_t dataBits) {
    if (!ready || bAddress == 0 || bControlIf == 0xFF)
        return USB_ERROR_DEVICE_NOT_SUPPORTED;

    uint8_t lineCoding[7];
    lineCoding[0] = baudrate & 0xFF;
    lineCoding[1] = (baudrate >> 8) & 0xFF;
    lineCoding[2] = (baudrate >> 16) & 0xFF;
    lineCoding[3] = (baudrate >> 24) & 0xFF;
    lineCoding[4] = stopBits;
    lineCoding[5] = parity;
    lineCoding[6] = dataBits;

    uint8_t rcode = pUsb->ctrlReq(
        bAddress, 0,
        USB_SETUP_HOST_TO_DEVICE | USB_SETUP_TYPE_CLASS | USB_SETUP_RECIPIENT_INTERFACE,
        0x20,
        0,
        bControlIf,
        sizeof(lineCoding), lineCoding, nullptr
    );

    printf("[ACM] SetLineCoding: baud=%lu stop=%u parity=%u bits=%u => rcode=0x%02X\n",
           baudrate, stopBits, parity, dataBits, rcode);

    return rcode;
}

void ACM::ParseInterfaceDescriptor(const uint8_t *buff) {
    const USB_INTERFACE_DESCRIPTOR *iface = (const USB_INTERFACE_DESCRIPTOR*)buff;

    if (iface->bInterfaceClass == 0x02 && iface->bInterfaceSubClass == 0x02) {
        if (bControlIf == 0xFF) {
            bControlIf = iface->bInterfaceNumber;
            printf("[ACM] Found Control Interface: %u\n", bControlIf);
        }
    } else if (iface->bInterfaceClass == 0x0A) {
        if (bDataIf == 0xFF) {
            bDataIf = iface->bInterfaceNumber;
            printf("[ACM] Found Data Interface: %u\n", bDataIf);
        }
    }
}

void ACM::EndpointXtract(uint8_t conf, uint8_t iface, uint8_t alt, uint8_t proto,
                         const USB_ENDPOINT_DESCRIPTOR *ep) {
    if (iface != bControlIf && iface != bDataIf) return;

    uint8_t index = 0;

    if (ep->bmAttributes == 0x03) {
        epInterruptIn = ep->bEndpointAddress & 0x0F;
        index = 1;
        printf("[ACM] Interrupt IN EP: 0x%02X\n", ep->bEndpointAddress);
    } else if (ep->bmAttributes == 0x02) {
        if ((ep->bEndpointAddress & 0x80) == 0x80) {
            epDataIn = ep->bEndpointAddress & 0x0F;
            index = 2;
            printf("[ACM] Data IN EP: 0x%02X\n", ep->bEndpointAddress);
        } else {
            epDataOut = ep->bEndpointAddress & 0x0F;
            index = 3;
            printf("[ACM] Data OUT EP: 0x%02X\n", ep->bEndpointAddress);
        }
    }

    epInfo[index].epAddr = ep->bEndpointAddress;
    epInfo[index].maxPktSize = ep->wMaxPacketSize;
    epInfo[index].epAttribs = 0;
    epInfo[index].bmNakPower = USB_NAK_NOWAIT;
}
