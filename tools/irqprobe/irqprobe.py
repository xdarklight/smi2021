#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host side of the IRQPROBE build: see README.md.

usage: irqprobe.py ELF REQUESTS [STEP]

ELF is the probe build's build/opensmi21.elf, for the addresses of its
tables (needs avr-nm); REQUESTS the register reads per run, which make the
USB events; STEP the delay step, in units of 4 cycles. Needs
tools/bridge-probe on PYTHONPATH and the driver unloaded.
"""
import struct, subprocess, sys
import bridge as br

# name, probe_addr, probe_mask, probe_inv, probe_in, probe_noread
RUNS = [
    ('pending after ep0_tx() queued a packet, registers read before', 0x1502, 0, 0, 1, 0),
    ('pending after ep0_tx() queued a packet, no reads', 0x1502, 0, 0, 1, 1),
    ('main loop, setup packet, registers read before', 0x1502, 0x10, 0, 0, 0),
    ('main loop, setup packet, no reads', 0x1502, 0x10, 0, 0, 1),
    ('trace after ep0_tx() queued a packet', 0x1502, 0, 0, 3, 0),
    ('sweep: window against the moment the host took the packet', 0x1502, 0, 0, 2, 0),
]
elf, reqs = sys.argv[1], int(sys.argv[2])
step = int(sys.argv[3]) if len(sys.argv) > 3 else 2
sym = {}
out = subprocess.run(['avr-nm', elf], capture_output=True, text=True).stdout
for line in out.split('\n'):
    p = line.split()
    if len(p) == 3:
        sym[p[2]] = int(p[0], 16) & 0xffff
N, SNAP_N, TRACE_N, TRACE_E = 128, 24, 12, 8
TABS = ('probe_tries', 'probe_v11', 'probe_v0')
b = br.Bridge(br.Usb1Transport())
b.write8(sym['probe_step'], step)


def u16(name):
    return struct.unpack('<H', b.read(sym[name], 2))[0]


for name, addr, mask, inv, pin, noread in RUNS:
    b.write8(sym['probe_mask'], 0)
    b.write8(sym['probe_in'], 0)
    for t in TABS:
        b.write(sym[t], bytes(2 * N))
    b.write(sym['probe_snaps'], bytes(8 * SNAP_N))
    b.write(sym['probe_pre'], bytes(2))
    b.write(sym['probe_pre_v0'], bytes(2))
    b.write8(sym['probe_trace_n'], 0)
    b.write(sym['probe_d'], bytes(2))
    b.write(sym['probe_addr'], struct.pack('<H', addr))
    b.write8(sym['probe_inv'], inv)
    b.write8(sym['probe_noread'], noread)
    b.write8(sym['probe_mask'], mask)
    b.write8(sym['probe_in'], pin)
    for i in range(reqs if pin != 3 else 20):
        b.read8(sym['probe_step'])
    b.write8(sym['probe_mask'], 0)
    b.write8(sym['probe_in'], 0)
    print(f'== {name}')
    if pin == 3:
        n = b.read8(sym['probe_trace_n'])
        raw = b.read(sym['probe_traces'], TRACE_N * TRACE_E * 5)
        for k in range(n):
            ev = []
            for e in range(TRACE_E):
                poll, r2, rc, r3 = struct.unpack_from('<HBBB', raw, (k * TRACE_E + e) * 5)
                if e and poll == 0:
                    break
                ev.append(f'{poll:4d}: 1502={r2:02x} 151c={rc:02x} 1503={r3:02x}')
            print('   packet', k, ' | '.join(ev))
        continue
    tab = {t: struct.unpack(f'<{N}H', b.read(sym[t], 2 * N)) for t in TABS}
    tr, a, z = (sum(tab[t]) for t in TABS)
    if pin == 2:
        print(f'   {tr} tries: {a} via 11, {z} via 0, {tr - a - z} none')
        print('   window - moment taken, in polls of ~10 cycles   tries  via 11  via 0')
        for i in range(N):
            t, v, w = (tab[x][i] for x in TABS)
            if t:
                lo = (i - 64) * 2
                lab = f'<= {lo}' if i == 0 else f'>= {lo}' if i == N - 1 else f'{lo:4d} .. {lo + 1:4d}'
                print(f'   {lab:>14} {t:8d} {v:7d} {w:6d}')
        continue
    pre, pre0 = u16('probe_pre'), u16('probe_pre_v0')
    print(f'   {tr} timed tries: {a} via 11, {z} via 0, {tr - a - z} none; '
          f'{pre} found a request pending, {pre0} of them via 0')
    snaps = [struct.unpack('<4BHH', b.read(sym['probe_snaps'] + 8 * k, 8))
             for k in range(SNAP_N)]
    print('   0x1503 0x1502 0x151c 0x1518   via 11  via 0')
    for k, (r3, r2, rc, r8, v11, v0) in enumerate(snaps):
        if v11 or v0:
            lab = '  (rest, or not read)' if k == SNAP_N - 1 else ''
            print(f'     0x{r3:02x}   0x{r2:02x}   0x{rc:02x}   0x{r8:02x} {v11:8d} {v0:6d}{lab}')
