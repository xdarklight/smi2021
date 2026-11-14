#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Use the test requests of OpenSMI21, see README.md "Test requests".

usage: test-requests.py identity | events | census | crash [LEN] | irq | ret |
                        reset | insn N [RAMPZ] |
                        pm ADDR LEN [RAMPZ [elpm]] | timing N... |
                        soak N [pm|dev|cfg|id|evl] |
                        win MODE [PASSES [TIMES]]

soak sends N rounds of a program memory read, the device and the
configuration descriptor and the identity, or N of one of them (evl: the
event log), and tells whether the firmware restarted meanwhile.

win runs the interrupt window test of an ISR_BARE=9 build for PASSES passes
of the main loop, TIMES times; MODE is wValue, its low byte the mode and
its high byte the second mode byte of win.h. It prints how often an
interrupt returned to each address of the window, and for the first 64
interrupts the return address and the stack pointer found there, relative
to the window's.

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


# Interrupt window test record, see win.h
WIN_LOG = 24
WIN_HIST = WIN_LOG + 3 * 64
WIN_REPLY = WIN_HIST + 64 + 2


def open_dev(ctx):
    for dev in ctx.getDeviceIterator(skip_on_error=True):
        if dev.getVendorID() == VID:
            return dev.open()
    sys.exit('no 1c88 device')


def req(h, breq, value=0, index=0, length=64, timeout=5000):
    return bytes(h.controlRead(REQ_IN, breq, value, index, length,
                               timeout=timeout))


def identity(h):
    r = req(h, 0xf0, length=29)
    if r[:9] != b'OpenSMI21' or r[9] not in (2, 3):
        sys.exit(f'not OpenSMI21 with test interface 2 or 3: {r[:10]!r}')
    magic, iface, vmaj, vmin, pid, size, opts, mode, starts, lastvec, \
        state, alt, speed, unknown, v11 = struct.unpack('<9sBBBHHBBHBBBBHH',
                                                        r[:28])
    opts2 = ''
    if len(r) > 28:
        opts2 = (f' (push guard {"on" if r[28] & 1 else "off"}, '
                 f'ISR_BARE {r[28] >> 4})')
    print(f'{magic.decode()} interface {iface}, version {vmaj}.{vmin}, pid '
          f'0x{pid:04x}, image {size} bytes, options 0x{opts:02x}{opts2}, mode '
          f'{mode}, starts {starts}, last stray vector '
          f'{"none" if lastvec == 0xff else lastvec}, state {state}, alt '
          f'{alt}, 0x1501 = 0x{speed:02x}, unknown events {unknown}, '
          f'vector 11 taken {v11} times')
    return starts


SOAK_KINDS = ['pm', 'dev', 'cfg', 'id']


def soak_one(h, kind, ref, i):
    if kind == 'pm':
        if req(h, 0xf2, 0, 0, 64) != ref:
            sys.exit(f'round {i}: program memory read differs')
    elif kind == 'dev':
        d = bytes(h.controlRead(0x80, 6, 0x0100, 0, 18))
        if len(d) != 18 or d[0] != 18 or d[1] != 1:
            sys.exit(f'round {i}: bad device descriptor {d.hex()}')
    elif kind == 'cfg':
        d = bytes(h.controlRead(0x80, 6, 0x0200, 0, 255))
        if len(d) < 9 or d[1] != 2:
            sys.exit(f'round {i}: bad configuration descriptor')
    elif kind == 'id':
        r = req(h, 0xf0, length=29)
        if r[:9] != b'OpenSMI21':
            sys.exit(f'round {i}: bad identity {r.hex()}')
    elif kind == 'evl':
        req(h, 0xf5, 0, 0, 64)
    else:
        sys.exit(f'unknown soak kind {kind}')


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
            r = req(h, 0xf7, length=71)
            r = r + bytes(71 - len(r))
            v11, storm, n = struct.unpack('<IBB', r[:6])
            print(f'vector 11 taken {v11} times, storm guard '
                  f'{"tripped" if storm else "not tripped"}; first {n} '
                  'calls (0x1503 0x1502 0x151c 0x1518):',
                  ', '.join(r[6 + 4 * i:10 + 4 * i].hex(' ') for i in range(n)))
            nd = r[38]
            print(f'first {nd} setup packets in the main loop (0x1503 twice, '
                  '0x1502 twice):',
                  ', '.join(r[39 + 4 * i:43 + 4 * i].hex(' ') for i in range(nd)))
        elif cmd == 'ret':
            r = req(h, 0xf8, length=206)
            r = r + bytes(200 - min(len(r), 200))
            for name, rec in (('at the last restart', r[:100]),
                              ('now', r[100:200])):
                pos = rec[0]
                n = min(pos, 32)
                entries = [rec[4 + 3 * (i % 32):7 + 3 * (i % 32)]
                           for i in range(pos - n, pos)]
                print(f'{name}: {pos} entries written (wraps at 256); last '
                      f'{n}, oldest first, as return address/SREG:')
                print('   ', ' '.join(f'{2 * ((e[0] << 8) | e[1]):#06x}/'
                                      f'{e[2]:02x}' for e in entries))
            if len(r) >= 206:
                v = r[200:206]
                print(f'interrupts that entered the reset vector: {v[0]}; '
                      f'the last with SREG 0x{v[1]:02x}, SP '
                      f'0x{(v[3] << 8 | v[2]) + 3:04x}, returning to '
                      f'{2 * ((v[4] << 8) | v[5]):#06x}' if v[0] else
                      'interrupts that entered the reset vector: 0')
        elif cmd == 'win':
            mode = int(args[1], 0)
            passes = int(args[2], 0) if len(args) > 2 else 1
            reps = int(args[3], 0) if len(args) > 3 else 1
            for rep in range(reps):
                # arm; the next request's interrupts fall into the window
                req(h, 0xf9, mode, passes, 2)
                time.sleep(0.05)
                r = req(h, 0xf9, 0, 0, WIN_REPLY)
                for _ in range(passes):
                    if not r[0] & 0x80:
                        break
                    r = req(h, 0xf9, 0, 0, WIN_REPLY)
                n = r[1]
                done, total, bad = struct.unpack('<HHH', r[2:8])
                first = struct.unpack('<H', r[WIN_HIST + 64:WIN_REPLY])[0]
                print(f'mode {mode:#04x} -> {r[0]:#04x}: {done} windows, '
                      f'{total} interrupts, most in a round {r[16]}; rounds '
                      f'that ended with SPL off: {bad}, last by '
                      f'{r[8] - 256 if r[8] > 127 else r[8]}; taken with the '
                      f'window closed: {r[9]}, last returning to '
                      f'{2 * ((r[10] << 8) | r[11]):#06x}; given up: {r[12]}; '
                      f'window at {first:#06x}')
                lost_at = (f'{first + 2 * r[19]:#x}' if r[19] != 0xff
                           else 'none in that round')
                print(f'  I clear without the handler: {r[23]};'
                      f' return address not written: {r[18]}', end='')
                print(f'; the last one found {(r[20] << 8) | r[21]:#06x}'
                      f' (word) there, the interrupt before it returned to '
                      f'{lost_at}' if r[18] else '')
                print('  returns per address:',
                      ' '.join(f'{first + 2 * i:#x}:{c}'
                               for i, c in enumerate(r[WIN_HIST:WIN_HIST + 64])
                               if c))
                log = []
                for k in range(n):
                    e = r[WIN_LOG + 3 * k:][:3]
                    log.append(f'{2 * ((e[0] << 8) | e[1]):#x}/'
                               f'{e[2] - 256 if e[2] > 127 else e[2]:+d}')
                print(f'  first {n} (return address/SPL):', ' '.join(log))
        elif cmd == 'soak':
            # control transfers of all kinds, or of one kind, to make
            # interrupts
            n = int(args[1], 0)
            kinds = [args[2]] if len(args) > 2 else SOAK_KINDS
            before = identity(h)
            ref = req(h, 0xf2, 0, 0, 64)
            t0 = time.perf_counter()
            for i in range(n):
                for kind in kinds:
                    soak_one(h, kind, ref, i)
            dt = time.perf_counter() - t0
            after = identity(h)
            print(f'{n} rounds of {len(kinds)} control transfers in '
                  f'{dt:.1f} s, '
                  + ('no restart' if after == before else
                     f'RESTARTED: start counter {before} -> {after}'))
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
