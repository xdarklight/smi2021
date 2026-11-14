#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Send requests that OpenSMI21 must stall, each followed by valid ones.

usage: stall-test.py [ROUNDS]

Each kind of request that the firmware must refuse is sent ROUNDS times
(default 20); every one must be stalled, and after each the device
descriptor and the mode must still read fine. Then the halt feature of
endpoint 0x82 is set and cleared, with GET_STATUS before and after. The
device must not re-enumerate meanwhile. The Linux driver must not be bound
(rmmod smi2021), as this claims the interface; needs python-libusb1.

Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""
import sys
import usb1

VIN = usb1.TYPE_VENDOR | usb1.RECIPIENT_DEVICE | usb1.ENDPOINT_IN
VOUT = usb1.TYPE_VENDOR | usb1.RECIPIENT_DEVICE | usb1.ENDPOINT_OUT
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 20


def reg_msg(cntl, flags, size, data):
    m = bytes([0x0b, 0, cntl, flags, size]) + bytes(data)
    return m + bytes(max(0, 13 - len(m)))


def bridge_read(h, addr):
    h.controlWrite(VOUT, 1, 0x0b, 0, reg_msg(0x20, 0x82, 1, [addr >> 8, addr & 0xff]))
    return h.controlRead(VIN, 1, 0x0b, 0, 13)[7]


def valid(h):
    d = h.controlRead(0x80, 6, 0x0100, 0, 18)
    assert len(d) == 18 and d[1] == 1, d
    m = h.controlRead(VIN, 1, 0x0001, 0, 2)
    assert m[0] == 1, m


CASES = [
    ('IN vendor, unknown bRequest 0x02', lambda h: h.controlRead(VIN, 0x02, 0, 0, 2)),
    ('IN vendor, unknown wValue 0x42', lambda h: h.controlRead(VIN, 0x01, 0x42, 0, 2)),
    ('IN vendor, former test request 0xf0', lambda h: h.controlRead(VIN, 0xf0, 0, 0, 29)),
    ('GET_DESCRIPTOR string 7', lambda h: h.controlRead(0x80, 6, 0x0307, 0x0409, 255)),
    ('GET_DESCRIPTOR type 0x22', lambda h: h.controlRead(0x80, 6, 0x2200, 0, 64)),
    ('GET_DESCRIPTOR configuration 1', lambda h: h.controlRead(0x80, 6, 0x0201, 0, 255)),
    ('OUT vendor, 3 bytes', lambda h: h.controlWrite(VOUT, 1, 0x0b, 0, bytes([0x0b, 0, 0]))),
    ('OUT vendor, bridge access with 1 address byte',
     lambda h: h.controlWrite(VOUT, 1, 0x0b, 0, reg_msg(0x20, 0x81, 1, [0x31]))),
    ('OUT vendor, bridge read of 40 bytes',
     lambda h: h.controlWrite(VOUT, 1, 0x0b, 0, reg_msg(0x20, 0x82, 40, [0x31, 0xa5]))),
    ('OUT vendor, i2c write of 33 bytes',
     lambda h: h.controlWrite(VOUT, 1, 0x0b, 0,
                              bytes([0x0b, 0x4a, 0xc0, 0x01, 33, 0x00]) + bytes(33))),
    ('OUT vendor, mode 7', lambda h: h.controlWrite(VOUT, 1, 0x01, 0, bytes([0x01, 7]))),
    ('OUT vendor, 300 bytes', lambda h: h.controlWrite(VOUT, 1, 0x0b, 0, bytes(300))),
    ('SET_FEATURE halt on endpoint 0', lambda h: h.controlWrite(0x02, 3, 0, 0x00, b'')),
]

with usb1.USBContext() as ctx:
    h = ctx.openByVendorIDAndProductID(0x1c88, 0x003c)
    dev = h.getDevice()
    print(f'device {dev.getBusNumber()}-{dev.getDeviceAddress()}, 0x31a5 = 0x{bridge_read(h, 0x31a5):02x}')
    bad = 0
    for name, req in CASES:
        stalled = other = 0
        for _ in range(ROUNDS):
            try:
                req(h)
            except usb1.USBErrorPipe:
                stalled += 1
            except usb1.USBError as e:
                other += 1
                last = e
            valid(h)
        ok = stalled == ROUNDS
        bad += not ok
        extra = f', {other} other errors (last: {last})' if other else ''
        print(f'{"ok  " if ok else "FAIL"} {name}: {stalled}/{ROUNDS} stalled{extra}')

    h.claimInterface(0)
    st = lambda: h.controlRead(0x82, 0, 0, 0x82, 2)[0]
    before = st()
    h.controlWrite(0x02, 3, 0, 0x82, b'')
    halted = st()
    h.clearHalt(0x82)
    after = st()
    ok = (before, halted, after) == (0, 1, 0)
    bad += not ok
    print(f'{"ok  " if ok else "FAIL"} halt of 0x82: GET_STATUS {before}, after SET_FEATURE {halted}, after CLEAR_FEATURE {after}')
    h.releaseInterface(0)
    valid(h)
    print(f'device {dev.getBusNumber()}-{dev.getDeviceAddress()}, 0x31a5 = 0x{bridge_read(h, 0x31a5):02x}')
    print('FAIL' if bad else 'PASS')
    sys.exit(1 if bad else 0)
