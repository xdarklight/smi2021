#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the OUT data stage of OpenSMI21 with register writes of all sizes.

usage: out-test.py

Writes 1 to 255 random bytes with bridge register writes into SRAM at
0x0800, which OpenSMI21 leaves unused, in requests of one to five packets,
some padded beyond their data, reads them back and compares. Only for
OpenSMI21: another firmware may use that SRAM. Needs python-libusb1; the
Linux driver may stay bound.

Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""
import os
import sys
import usb1
VIN = usb1.TYPE_VENDOR | usb1.RECIPIENT_DEVICE | usb1.ENDPOINT_IN
VOUT = usb1.TYPE_VENDOR | usb1.RECIPIENT_DEVICE | usb1.ENDPOINT_OUT
BASE = 0x0800


def write(h, addr, data, pad=0):
    msg = bytes([0x0b, 0, 0x00, 0x82, len(data), addr >> 8, addr & 0xff]) + data
    h.controlWrite(VOUT, 1, 0x0b, 0, msg + bytes(pad), timeout=2000)


def read(h, addr, n):
    out = b''
    while n:
        k = min(n, 30)
        msg = bytes([0x0b, 0, 0x20, 0x82, k, addr >> 8, addr & 0xff]) + bytes(6)
        h.controlWrite(VOUT, 1, 0x0b, 0, msg, timeout=2000)
        out += bytes(h.controlRead(VIN, 1, 0x0d, 0, 37, timeout=2000)[7:7 + k])
        addr += k
        n -= k
    return out


bad = 0
with usb1.USBContext() as ctx:
    h = ctx.openByVendorIDAndProductID(0x1c88, 0x003c)
    for n in (1, 20, 56, 57, 58, 100, 121, 122, 186, 200, 250, 255):
        for pad in (0, 13):
            data = os.urandom(n)
            write(h, BASE, data, pad)
            back = read(h, BASE, n)
            ok = back == data
            bad += not ok
            print(f'{"ok  " if ok else "FAIL"} write of {n:3d} bytes, request of {7 + n + pad:3d} bytes in {(7 + n + pad + 63) // 64} packets')
print('FAIL' if bad else 'PASS')
sys.exit(1 if bad else 0)
