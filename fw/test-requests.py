#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Use the test requests of OpenSMI21, see README.md "Test requests".

usage: test-requests.py identity | events | census | crash [LEN] | irq |
                        reset | insn N [RAMPZ] | pm ADDR LEN [RAMPZ [elpm]] |
                        timing N...

pm writes the bytes to stdout. Needs python-libusb1, and access to the
device, which runs OpenSMI21; the Linux driver must not be bound to it.

Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""
import struct
import sys
import time

import usb1

VID = 0x1c88
REQ_IN = usb1.TYPE_VENDOR | usb1.RECIPIENT_DEVICE | usb1.ENDPOINT_IN


def open_dev(ctx):
    for dev in ctx.getDeviceIterator(skip_on_error=True):
        if dev.getVendorID() == VID:
            return dev.open()
    sys.exit('no 1c88 device')


def req(h, breq, value=0, index=0, length=64, timeout=5000):
    return bytes(h.controlRead(REQ_IN, breq, value, index, length,
                               timeout=timeout))


def identity(h):
    r = req(h, 0xf0, length=28)
    if r[:9] != b'OpenSMI21' or r[9] != 2:
        sys.exit(f'not OpenSMI21 with test interface 2: {r[:10]!r}')
    magic, iface, vmaj, vmin, pid, size, opts, mode, starts, lastvec, \
        state, alt, speed, unknown, v11 = struct.unpack('<9sBBBHHBBHBBBBHH', r)
    print(f'{magic.decode()} interface {iface}, version {vmaj}.{vmin}, pid '
          f'0x{pid:04x}, image {size} bytes, options 0x{opts:02x}, mode '
          f'{mode}, starts {starts}, last stray vector '
          f'{"none" if lastvec == 0xff else lastvec}, state {state}, alt '
          f'{alt}, 0x1501 = 0x{speed:02x}, unknown events {unknown}, '
          f'vector 11 taken {v11} times')
    return starts


def main():
    args = sys.argv[1:]
    with usb1.USBContext() as ctx:
        h = open_dev(ctx)
        cmd = args[0]
        if cmd == 'identity':
            identity(h)
        elif cmd == 'events':
            r = req(h, 0xf5, length=34)
            print(f'next {r[0]}, count {r[1]}:',
                  ' '.join(f'{r[2 + 2 * i]:02x}/{r[3 + 2 * i]:02x}'
                           for i in range(16)))
        elif cmd == 'crash':
            r = req(h, 0xf6, length=int(args[1]) if len(args) > 1 else 70)
            r = r + bytes(70 - len(r))
            boots, sp = struct.unpack('<HH', r[:4])
            phase, pos = r[4], r[5]
            print(f'starts {boots}; at the last restart: SP {sp:#06x}, '
                  f'phase {phase}; its last setup packets, oldest first:')
            for i in range(8):
                e = r[6 + 8 * ((pos + i) % 8):][:8]
                if any(e):
                    print('   ', e.hex(' '))
        elif cmd == 'irq':
            r = req(h, 0xf7, length=38)
            v11, storm, n = struct.unpack('<IBB', r[:6])
            print(f'vector 11 taken {v11} times, storm guard '
                  f'{"tripped" if storm else "not tripped"}; first {n} '
                  'calls (0x1503 0x1502 0x151c 0x1518):',
                  ', '.join(r[6 + 4 * i:10 + 4 * i].hex(' ') for i in range(n)))
        elif cmd == 'reset':
            h.resetDevice()
            print('port reset')
        elif cmd == 'census':
            r = req(h, 0xf3, length=97)
            counts = struct.unpack(f'<{(len(r) - 1) // 2}H', r[:len(r) - 1])
            print(f'start counter {counts[0]}; vectors taken:',
                  ', '.join(f'{n}: {c}' for n, c in enumerate(counts)
                            if n and c) or 'none',
                  f'; last stray vector {"none" if r[-1] == 0xff else r[-1]}')
        elif cmd == 'insn':
            n = int(args[1], 0)
            rampz = int(args[2], 0) if len(args) > 2 else 0
            before = identity(h)
            try:
                r = req(h, 0xf1, n, rampz, 18)
            except usb1.USBError as e:
                print(f'test {n}: request failed: {e!r}')
                return
            print(f'test {r[0]}: RAMPZ {r[1]:#04x}; before '
                  f'{r[2:6].hex(" ")}, after {r[6:10].hex(" ")}; SREG '
                  f'{r[10]:#04x} -> {r[11]:#04x}; RAMPZ read back '
                  f'{r[12]:#04x}, after {r[13]:#04x}; pattern '
                  f'{r[14:16].hex(" ")} at {struct.unpack("<H", r[16:18])[0]:#06x}')
            after = identity(h)
            print('alive, no restart' if after == before else
                  f'RESTARTED: start counter {before} -> {after}')
        elif cmd == 'pm':
            addr, n = int(args[1], 0), int(args[2], 0)
            rampz = int(args[3], 0) if len(args) > 3 else 0
            elpm = len(args) > 4 and args[4] == 'elpm'
            out = bytearray()
            while n:
                k = min(n, 64)
                out += req(h, 0xf2, addr & 0xffff,
                           rampz | (0x100 if elpm else 0), k)
                addr += k
                n -= k
            sys.stdout.buffer.write(bytes(out))
        elif cmd == 'timing':
            for n in map(lambda a: int(a, 0), args[1:]):
                t0 = time.perf_counter()
                r = req(h, 0xf4, n, 0, 4, timeout=10000)
                dt = time.perf_counter() - t0
                print(f'N {n}: {dt * 1000:.3f} ms (reply {r.hex()})')
        else:
            sys.exit(__doc__)


if __name__ == '__main__':
    main()
