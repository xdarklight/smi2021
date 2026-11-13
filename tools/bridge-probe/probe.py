#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Experiments on an SMI2021 bridge that need only its original firmware.

Each "exp" subcommand is one of the experiments listed under "Experiments"
in docs/hardware.md, and writes its results to results/<time>-<name>/. The
other subcommands are building blocks for poking at the device by hand. See
README.md for the procedure and for what each result means.

Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""

import argparse
import collections
import datetime
import json
import math
import os
import pathlib
import sys
import time

import bridge as br

HERE = pathlib.Path(__file__).resolve().parent
FIRMWARE_DIR = HERE.parent.parent / 'firmware'

# ATmega103 I/O registers, as data space addresses (I/O address + 0x20), from
# avr-libc's iom103.h, plus DDRC and PINC, which the SMI2021 has.
AVR_REGS = {
    0x20: 'PINF', 0x21: 'PINE', 0x22: 'DDRE', 0x23: 'PORTE',
    0x24: 'ADCL', 0x25: 'ADCH', 0x26: 'ADCSR', 0x27: 'ADMUX', 0x28: 'ACSR',
    0x29: 'UBRR', 0x2a: 'UCR', 0x2b: 'USR', 0x2c: 'UDR',
    0x2d: 'SPCR', 0x2e: 'SPSR', 0x2f: 'SPDR',
    0x30: 'PIND', 0x31: 'DDRD', 0x32: 'PORTD',
    0x33: 'PINC', 0x34: 'DDRC', 0x35: 'PORTC',
    0x36: 'PINB', 0x37: 'DDRB', 0x38: 'PORTB',
    0x39: 'PINA', 0x3a: 'DDRA', 0x3b: 'PORTA',
    0x3c: 'EECR', 0x3d: 'EEDR', 0x3e: 'EEARL', 0x3f: 'EEARH',
    0x41: 'WDTCR', 0x43: 'OCR2', 0x44: 'TCNT2', 0x45: 'TCCR2',
    0x46: 'ICR1L', 0x47: 'ICR1H', 0x48: 'OCR1BL', 0x49: 'OCR1BH',
    0x4a: 'OCR1AL', 0x4b: 'OCR1AH', 0x4c: 'TCNT1L', 0x4d: 'TCNT1H',
    0x4e: 'TCCR1B', 0x4f: 'TCCR1A', 0x50: 'ASSR', 0x51: 'OCR0',
    0x52: 'TCNT0', 0x53: 'TCCR0', 0x54: 'MCUSR', 0x55: 'MCUCR',
    0x56: 'TIFR', 0x57: 'TIMSK', 0x58: 'EIFR', 0x59: 'EIMSK', 0x5a: 'EICR',
    0x5b: 'RAMPZ', 0x5c: 'XDIV', 0x5d: 'SPL', 0x5e: 'SPH', 0x5f: 'SREG',
}
REG = {name: addr for addr, name in AVR_REGS.items()}

# Registers that may be written to see whether they exist, with the value to
# write. None of these enables an interrupt (the vectors are empty), drives a
# pin, or touches the clock, sleep, the watchdog or external memory, and each
# is restored afterwards. Timer 1 and 2 are stopped by the firmware, so their
# counters keep a written value.
AVR_WRITE_TESTS = [
    ('OCR0', 0x5a), ('OCR2', 0xa5), ('TCNT2', 0x3c), ('OCR1AL', 0x5a),
    ('OCR1BL', 0xa5), ('TCNT1L', 0x3c), ('UBRR', 0x5a), ('ADMUX', 0x05),
    ('SPCR', 0x03), ('EEARL', 0x5a), ('EEDR', 0xa5), ('RAMPZ', 0x01),
]

GPIO_PORTS = {'A': (0x39, 0x3a, 0x3b), 'C': (0x33, 0x34, 0x35)}

TIMER_CS = (7, 6, 5, 4, 3, 2, 1)
# What Timer0's clock select values divide by. An external clock on a pin
# that sees no edges counts as 0.
PRESCALERS = {
    'ATmega103': {1: 1, 2: 8, 3: 32, 4: 64, 5: 128, 6: 256, 7: 1024},
    'ATmega16': {1: 1, 2: 8, 3: 64, 4: 256, 5: 1024, 6: 0, 7: 0},
}

ENDPOINT_BLOCK = {'A': 0x1520, 'B': 0x1530}

# Variants of experiment 5, applied after the mode request of mode 5: the
# values for +1 and the state bits 2-1 of +0 of each block, or None to leave
# them as the mode request set them (A: 0x15/01, B: 0x25/00).
ENDPOINT_VARIANTS = {
    'baseline': {},
    'swap-both': {'A': (0x25, 0b00), 'B': (0x15, 0b01)},
    'swap-states': {'A': (None, 0b00), 'B': (None, 0b01)},
    'swap-numbers': {'A': (0x25, None), 'B': (0x15, None)},
    'both-active': {'A': (None, 0b00), 'B': (None, 0b00)},
}

MODE_RUNS = [(2, 1), (2, 2), (5, 2), (3, 1), (3, 2), (4, 1), (4, 2)]

I2C_READ_CMDS = (0xa0, 0xe0, 0x20, 0x60, 0xa4, 0xe4, 0xb0, 0xf0)


# -- results ----------------------------------------------------------------

class Results:
    """A directory for one run, with a summary and a synced breadcrumb."""

    def __init__(self, name, base):
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        self.base = pathlib.Path(base)
        self.dir = self.base / f'{stamp}-{name}'
        self.dir.mkdir(parents=True, exist_ok=True)
        self.summary = open(self.dir / 'summary.txt', 'w', encoding='utf-8')
        self.data = {}

    def crumb(self, text):
        # On the disk before the step runs, so that a wedged device or board
        # still tells which step it was.
        path = self.base / 'breadcrumb'
        with open(path, 'w', encoding='utf-8') as f:
            f.write(f'{datetime.datetime.now().isoformat()} {self.dir.name}: '
                    f'{text}\n')
            f.flush()
            os.fsync(f.fileno())

    def say(self, text=''):
        print(text)
        self.summary.write(text + '\n')
        self.summary.flush()

    def path(self, name):
        return self.dir / name

    def close(self):
        with open(self.dir / 'data.json', 'w', encoding='utf-8') as f:
            json.dump(self.data, f, indent=1, sort_keys=True)
        self.summary.close()
        crumb = self.base / 'breadcrumb'
        if crumb.exists():
            crumb.unlink()


# -- capture analysis -------------------------------------------------------

def analyze_packets(packets):
    """Statistics that tell the kinds of stream apart."""
    lengths = collections.Counter(len(d) for _, d in packets)
    statuses = collections.Counter(s for s, _ in packets)
    data = b''.join(d for _, d in packets)
    chunks = collections.Counter()
    for _, d in packets:
        if d and len(d) % 1024 == 0:
            for off in range(0, len(d), 1024):
                magic = d[off:off + 4]
                if magic == b'\xaa\xaa\x00\x00':
                    chunks['video'] += 1
                elif magic == b'\xaa\xaa\x00\x01':
                    chunks['audio'] += 1
                else:
                    chunks['other'] += 1
    hist = collections.Counter(data)
    entropy = 0.0
    for count in hist.values():
        p = count / len(data)
        entropy -= p * math.log2(p)
    ts_sync = 0
    if len(data) >= 188 * 10:
        # A transport stream has 0x47 every 188 bytes from some offset on.
        best = 0
        for start in range(188):
            hits = sum(1 for i in range(start, min(len(data), 188 * 200),
                                        188) if data[i] == 0x47)
            best = max(best, hits)
        ts_sync = best / min(200, len(data) // 188)
    return {
        'packets': len(packets),
        'packets_with_data': sum(1 for _, d in packets if d),
        'bytes': len(data),
        'statuses': {str(k): v for k, v in sorted(statuses.items())},
        'lengths': {str(k): v for k, v in lengths.most_common(8)},
        'chunks': dict(chunks),
        'distinct_bytes': len(hist),
        'entropy_bits': round(entropy, 3),
        'zero_fraction': round(hist.get(0, 0) / len(data), 3) if data else 0,
        'ff_fraction': round(hist.get(0xff, 0) / len(data), 3) if data else 0,
        'ts_sync_fraction': round(ts_sync, 3),
        'head': data[:64].hex(),
    }


def describe_analysis(a):
    lines = [f'{a["packets"]} packets, {a["packets_with_data"]} with data, '
             f'{a["bytes"]} bytes',
             f'packet lengths (most common): {a["lengths"]}',
             f'packet status: {a["statuses"]}']
    if a['bytes']:
        lines.append(f'chunks: {a["chunks"] or "none"}; distinct bytes '
                     f'{a["distinct_bytes"]}, entropy {a["entropy_bits"]} '
                     f'bits; 0x00 {a["zero_fraction"]}, 0xff '
                     f'{a["ff_fraction"]}; 0x47 every 188 bytes '
                     f'{a["ts_sync_fraction"]}')
        lines.append(f'first bytes: {a["head"]}')
    return lines


def capture(bridge, mode, alt, seconds, path, res=None):
    t = bridge.t
    if res:
        res.crumb(f'capture mode {mode} alt {alt}')
    t.set_alt(0)
    bridge.set_mode(mode)
    packets = t.iso_capture(alt, seconds)
    meta = {'mode': mode, 'alt': alt, 'seconds': seconds,
            'time': datetime.datetime.now().isoformat()}
    br.write_capture(path, meta, packets)
    return analyze_packets(packets)


def stop_engines(bridge):
    for addr in (0x1740, 0x1800, 0x3100):
        bridge.write8(addr, 0)
    bridge.set_mode(3)


# -- experiments ------------------------------------------------------------

def timer_rate(samples):
    """Count rate of an 8 bit counter from (time, value) samples.

    A pair of samples can only be unwrapped if the counter moved by less
    than a wrap in between. A rough rate from the pairs that moved by less
    than half a wrap gives the wrap period, and then only pairs well inside
    it are summed, which leaves out the pairs that a stall of the host
    stretched over a wrap.

    A counter that is too fast for the samples looks like noise, which
    yields some rate too. Then the steps don't follow the intervals, so
    such a result is counted as using no pairs. Returns (rate, pairs used,
    mean interval)."""
    pairs = [(t2 - t1, (v2 - v1) & 0xff)
             for (t1, v1), (t2, v2) in zip(samples, samples[1:]) if t2 > t1]
    if not pairs:
        return 0.0, 0, 0.0
    interval = sum(dt for dt, _ in pairs) / len(pairs)
    rough = sorted(step / dt for dt, step in pairs if step < 128)
    if not rough:
        return 0.0, 0, interval
    rate = rough[len(rough) // 2]
    if rate == 0:
        moving = sum(1 for _, step in pairs if step)
        return 0.0, len(pairs) - moving, interval
    limit = 0.4 * 256 / rate
    good = [(dt, step) for dt, step in pairs if dt < limit]
    time_sum = sum(dt for dt, _ in good)
    if not good or time_sum == 0:
        return rate, 0, interval
    rate = sum(step for _, step in good) / time_sum
    # The host can only time a sample to within a few microframes, so allow
    # for that besides a share of the step.
    residuals = sorted(abs(step - rate * dt) for dt, step in good)
    median = residuals[len(residuals) // 2]
    expected = rate * interval
    if expected >= 128 or median > max(8.0, 0.3 * expected):
        return rate, 0, interval
    return rate, len(good), interval


def exp_timer(bridge, res, args):
    """1: Timer0 prescaler table and the CPU clock."""
    tcnt, tccr, assr = REG['TCNT0'], REG['TCCR0'], REG['ASSR']
    saved = bridge.read8(tccr), bridge.read8(tcnt)
    res.say(f'ASSR = 0x{bridge.read8(assr):02x} (bit 3 set: Timer0 runs from '
            f'the watch crystal input)')
    rates = {}
    for cs in TIMER_CS:
        res.crumb(f'timer clock select {cs}')
        bridge.write8(tccr, 0)
        bridge.write8(tcnt, 0)
        bridge.write8(tccr, cs)
        samples = []
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            value = bridge.read8(tcnt)
            samples.append((time.monotonic(), value))
        bridge.write8(tccr, 0)
        rate, used, interval = timer_rate(samples)
        trusted = used >= 0.5 * (len(samples) - 1) and used >= 50
        rates[cs] = {'rate_hz': rate, 'trusted': trusted, 'pairs_used': used,
                     'sample_interval_us': interval * 1e6,
                     'samples': len(samples)}
        res.say(f'clock select {cs}: {rate:12.1f} Hz '
                f'({"ok" if trusted else "too fast to unwrap"}, '
                f'{used} of {len(samples) - 1} sample pairs usable, a '
                f'sample every {interval * 1e6:.0f} us)')
    bridge.write8(tccr, 0)
    bridge.write8(tcnt, saved[1])
    bridge.write8(tccr, saved[0])
    res.data['rates'] = rates
    res.say()
    for name, table in PRESCALERS.items():
        # The slowest trusted divider that isn't an external clock gives the
        # CPU clock under this table; the other trusted rates have to agree
        # with it, and an external clock, with no edges on its pin, has to
        # stand still.
        ref = next((cs for cs in TIMER_CS
                    if table[cs] and rates[cs]['trusted']), None)
        fcpu = rates[ref]['rate_hz'] * table[ref] if ref else None
        worst, compared = 0.0, 0
        for cs in TIMER_CS:
            if not rates[cs]['trusted']:
                continue
            got = rates[cs]['rate_hz']
            if not table[cs]:
                err = 0.0 if got < 1.0 else 1.0
            elif fcpu is None:
                continue
            else:
                expect = fcpu / table[cs]
                err = abs(got - expect) / max(expect, got, 1.0)
            compared += 1
            worst = max(worst, err)
        res.data.setdefault('tables', {})[name] = {
            'fcpu': fcpu, 'worst_error': worst, 'compared': compared}
        clock = f'CPU clock {fcpu / 1e6:.3f} MHz' if fcpu else \
            'no CPU clock'
        res.say(f'{name} prescaler: {clock}, worst deviation of '
                f'{compared} trusted rates {worst * 100:.1f} %')


def _page_class(p1, p2, mask):
    if not all(mask):
        return 'not read'
    if p1 != p2:
        return 'changes'
    if len(set(p1)) == 1:
        return f'all 0x{p1[0]:02x}'
    return 'stable'


def exp_survey(bridge, res, args):
    """2: what responds in the data space."""
    exclude = [range(0x1500, 0x1580)] + [_parse_range(r)
                                         for r in args.exclude]
    start, stop = _parse_range(args.range).start, _parse_range(args.range).stop
    passes = [bytearray(0x10000), bytearray(0x10000)]
    mask = bytearray(0x10000)
    step = 30
    for p in (0, 1):
        addr = start
        while addr < stop:
            n = min(step, stop - addr)
            skip = next((r for r in exclude
                         if addr < r.stop and addr + n > r.start), None)
            if skip:
                if addr >= skip.start:
                    addr = skip.stop
                    continue
                n = skip.start - addr
            res.crumb(f'survey pass {p + 1} read 0x{addr:04x}+{n}')
            passes[p][addr:addr + n] = bridge.read(addr, n)
            mask[addr:addr + n] = b'\x01' * n
            addr += n
    for p in (0, 1):
        res.path(f'pass{p + 1}.bin').write_bytes(passes[p])
    res.path('mask.bin').write_bytes(mask)

    pages = {}
    seen = {}
    for page in range(start & ~0xff, stop, 0x100):
        lo, hi = max(page, start), min(page + 0x100, stop)
        if not any(mask[lo:hi]):
            continue
        p1, p2 = bytes(passes[0][lo:hi]), bytes(passes[1][lo:hi])
        cls = _page_class(p1, p2, mask[lo:hi])
        if cls == 'stable' and p1 in seen:
            cls = f'same as 0x{seen[p1]:04x}'
        elif cls == 'stable':
            seen[p1] = lo
        pages[f'0x{lo:04x}'] = cls
    res.data['pages'] = pages
    runs = []
    for addr, cls in pages.items():
        if runs and runs[-1][2] == cls and not cls.startswith(('stable',)):
            runs[-1][1] = addr
        else:
            runs.append([addr, addr, cls])
    res.say('256 byte pages:')
    for first, last, cls in runs:
        span = first if first == last else f'{first}-{last}'
        res.say(f'  {span:>13}: {cls}')

    # Program memory or the bootloader may show up in the data space.
    needles = {'USB device descriptor': bytes([0x12, 0x01, 0x00, 0x02]),
               'vendor id 1c88': bytes([0x88, 0x1c])}
    for fw in sorted(FIRMWARE_DIR.glob('*.bin')):
        image = fw.read_bytes()
        needles[f'{fw.name} vectors'] = image[:16]
        needles[f'{fw.name} code at 0x200'] = image[0x200:0x220]
    hits = {}
    blob = bytes(passes[0])
    for name, needle in needles.items():
        found = []
        pos = blob.find(needle)
        while pos >= 0 and len(found) < 16:
            if all(mask[pos:pos + len(needle)]):
                found.append(f'0x{pos:04x}')
            pos = blob.find(needle, pos + 1)
        hits[name] = found
        res.say(f'{name}: {", ".join(found) if found else "not found"}')
    res.data['hits'] = hits

    for r in args.write_test:
        rng = _parse_range(r)
        res.say(f'write test 0x{rng.start:04x}-0x{rng.stop - 1:04x}:')
        sticky = []
        for addr in rng:
            res.crumb(f'write test 0x{addr:04x}')
            old = bridge.read8(addr)
            bridge.write8(addr, old ^ 0xff)
            new = bridge.read8(addr)
            bridge.write8(addr, old)
            if new == old ^ 0xff:
                sticky.append(addr)
        res.say(f'  {len(sticky)} of {len(rng)} bytes keep a written value')
        res.data.setdefault('write_tests', {})[r] = [f'0x{a:04x}'
                                                     for a in sticky]


def exp_avr(bridge, res, args):
    """3: which ATmega103 peripherals exist."""
    regs = bridge.read(0x20, 0x40)
    res.data['io'] = {f'0x{0x20 + i:02x}': v for i, v in enumerate(regs)}
    res.say('I/O registers (data space address, ATmega103 name, value):')
    for i in range(0, 0x40, 8):
        cells = []
        for j in range(i, i + 8):
            name = AVR_REGS.get(0x20 + j, '-')
            cells.append(f'{name:>6}={regs[j]:02x}')
        res.say(f'  0x{0x20 + i:02x}: ' + ' '.join(cells))
    res.say()
    res.say('write tests (each restored afterwards):')
    results = {}
    for name, value in AVR_WRITE_TESTS:
        addr = REG[name]
        res.crumb(f'write test {name}')
        old = bridge.read8(addr)
        bridge.write8(addr, value)
        new = bridge.read8(addr)
        bridge.write8(addr, old)
        back = bridge.read8(addr)
        kept = new == value
        results[name] = {'old': old, 'wrote': value, 'read': new,
                         'restored': back == old, 'kept': kept}
        res.say(f'  {name:>6} (0x{addr:02x}): was 0x{old:02x}, wrote '
                f'0x{value:02x}, read 0x{new:02x} -> '
                f'{"exists" if kept else "no such register"}'
                f'{"" if back == old else ", NOT RESTORED"}')
    res.data['write_tests'] = results


def exp_i2c(bridge, res, args):
    """4: size of the i2c data buffers, and who is on each bus."""
    for base in br.I2C_ENGINES:
        first, last = base + 5, base + args.buffer_end
        res.crumb(f'i2c buffer at 0x{base:04x}')
        old = bridge.read(first, last - first + 1)
        pattern = bytes(((i * 37) ^ 0xa5) & 0xff
                        for i in range(last - first + 1))
        bridge.write(first, pattern)
        back = bridge.read(first, last - first + 1)
        bridge.write(first, old)
        kept = [i for i in range(len(pattern)) if back[i] == pattern[i]]
        size = 0
        while size < len(pattern) and back[size] == pattern[size]:
            size += 1
        res.say(f'engine 0x{base:04x}: the first {size} bytes from '
                f'+5 keep a written value; {len(kept)} of {len(pattern)} '
                f'tested do')
        res.data.setdefault('buffers', {})[f'0x{base:04x}'] = {
            'contiguous': size, 'kept': kept, 'read_back': back.hex(),
            'before': old.hex()}
    for bus in (0, 1):
        found = []
        for addr in range(0x08, 0x78):
            res.crumb(f'i2c scan bus {bus} address 0x{addr:02x}')
            _, failed = bridge.i2c(addr, 0xa0, read_len=1, bus=bus)
            if not failed:
                found.append(addr)
        res.say(f'bus {bus}: devices at '
                f'{", ".join(f"0x{a:02x}" for a in found) or "none"} '
                f'(7 bit addresses)')
        res.data.setdefault('scan', {})[bus] = found
    if args.commands:
        addr, reg = args.decoder, 0x00
        ref = [bridge.i2c(addr, 0xe0, sub=reg, read_len=1)
               for _ in range(5)]
        res.say(f'decoder 0x{addr:02x} register 0x{reg:02x} with 0xe0: '
                f'{[(d.hex(), f) for d, f in ref]}')
        for cmd in I2C_READ_CMDS:
            res.crumb(f'i2c command 0x{cmd:02x}')
            sub = reg if cmd & 0xc0 != 0x80 else None
            try:
                got = [bridge.i2c(addr, cmd, sub=sub, read_len=2)
                       for _ in range(5)]
            except ValueError as e:
                res.say(f'  0x{cmd:02x}: skipped, {e}')
                continue
            res.say(f'  0x{cmd:02x}: {[(d.hex(), f) for d, f in got]}')
            res.data.setdefault('commands', {})[f'0x{cmd:02x}'] = [
                (d.hex(), f) for d, f in got]


def _usb_regs(bridge):
    out = {}
    for lo, hi in ((0x1500, 0x1503), (0x1504, 0x1580)):
        for addr, value in zip(range(lo, hi), bridge.read(lo, hi - lo)):
            out[addr] = value
    return out


def exp_endpoints(bridge, res, args):
    """5: what the endpoint register blocks do."""
    t = bridge.t
    res.say('USB controller registers after each mode request (0x1503 is '
            'not read):')
    t.set_alt(0)
    dumps = {}
    for mode in (2, 3, 4, 5):
        res.crumb(f'endpoint registers in mode {mode}')
        bridge.set_mode(mode)
        regs = _usb_regs(bridge)
        dumps[mode] = regs
        for lo in range(0x1500, 0x1580, 0x10):
            row = ' '.join('..' if a == 0x1503 else f'{regs[a]:02x}'
                           for a in range(lo, lo + 0x10))
            res.say(f'  mode {mode} 0x{lo:04x}: {row}')
    res.data['registers'] = {m: {f'0x{a:04x}': v for a, v in r.items()}
                             for m, r in dumps.items()}
    stop_engines(bridge)
    res.say()
    res.say(f'mode 5 with the blocks changed after the mode request, '
            f'captured at alternate setting 2 for {args.seconds} s:')
    for name in args.variants:
        res.crumb(f'endpoint variant {name}')
        t.set_alt(0)
        bridge.set_mode(5)
        for block, (cfg, st) in ENDPOINT_VARIANTS[name].items():
            base = ENDPOINT_BLOCK[block]
            if cfg is not None:
                bridge.write8(base + 1, cfg, allow_usb=True)
            if st is not None:
                bridge.update8(base, 0x06, st << 1, allow_usb=True)
        before = {b: (bridge.read8(base + 1), (bridge.read8(base) >> 1) & 3)
                  for b, base in ENDPOINT_BLOCK.items()}
        packets = t.iso_capture(2, args.seconds)
        br.write_capture(res.path(f'{name}.cap'),
                         {'mode': 5, 'alt': 2, 'variant': name}, packets)
        a = analyze_packets(packets)
        a['blocks'] = before
        res.data.setdefault('variants', {})[name] = a
        res.say(f'{name}: A +1=0x{before["A"][0]:02x} state '
                f'{before["A"][1]:02b}, B +1=0x{before["B"][0]:02x} state '
                f'{before["B"][1]:02b}')
        for line in describe_analysis(a):
            res.say('    ' + line)
        stop_engines(bridge)


def exp_modes(bridge, res, args):
    """6: what modes 3 and 4 stream, next to modes 2 and 5 for reference."""
    for mode, alt in MODE_RUNS:
        a = capture(bridge, mode, alt, args.seconds,
                    res.path(f'mode{mode}-alt{alt}.cap'), res)
        res.data[f'mode{mode}-alt{alt}'] = a
        res.say(f'mode {mode}, alternate setting {alt}:')
        for line in describe_analysis(a):
            res.say('    ' + line)
        stop_engines(bridge)


EXPERIMENTS = {
    'timer': exp_timer, 'survey': exp_survey, 'avr': exp_avr,
    'i2c': exp_i2c, 'endpoints': exp_endpoints, 'modes': exp_modes,
}


# -- commands -----------------------------------------------------------------

def _parse_range(text):
    lo, _, hi = text.partition('-')
    lo = int(lo, 0)
    hi = int(hi, 0) if hi else lo
    if hi < lo:
        raise ValueError(f'empty range {text}')
    return range(lo, hi + 1)


def _parse_pin(text):
    text = text.upper()
    if len(text) != 3 or text[0] != 'P' or text[1] not in GPIO_PORTS or \
            not text[2].isdigit() or int(text[2]) > 7:
        raise ValueError(f'unknown pin {text}; use PA0-PA7 or PC0-PC7')
    return GPIO_PORTS[text[1]], int(text[2])


def cmd_info(bridge, args):
    vid, pid = bridge.t.ids()
    print(f'USB id {vid:04x}:{pid:04x}, serial {bridge.t.serial()}')
    if pid == br.BOOTLOADER_ID:
        print('bootloader; upload a firmware first')
        return
    print(f'mode {bridge.get_mode()}')
    for addr, name in ((0x1580, 'mode 3 engine'), (0x1600, 'stream/IR'),
                       (0x1740, 'audio'), (0x1800, 'video'),
                       (0x1880, 'stream source'), (0x3080, 'system'),
                       (0x3100, 'mode 4 engine'),
                       (0x31a5, 'board id')):
        print(f'0x{addr:04x} {name:>13}: 0x{bridge.read8(addr):02x}')
    for port, (pin, ddr, prt) in GPIO_PORTS.items():
        print(f'port {port}: DDR 0x{bridge.read8(ddr):02x} PORT '
              f'0x{bridge.read8(prt):02x} PIN 0x{bridge.read8(pin):02x}')
    print(f'info query: {bridge.info_query().hex()}')


def cmd_read(bridge, args):
    data = bridge.read(args.addr, args.count)
    for i in range(0, len(data), 16):
        print(f'{args.addr + i:04x}: {data[i:i + 16].hex(" ")}')


def cmd_write(bridge, args):
    bridge.write(args.addr, args.values, allow_usb=args.usb)


def cmd_mode(bridge, args):
    if args.mode is not None:
        bridge.set_mode(args.mode)
    print(f'mode {bridge.get_mode()}')


def cmd_i2c(bridge, args):
    if args.write is not None:
        bridge.i2c(args.addr, args.cmd if args.cmd is not None else 0xc0,
                   sub=args.reg, data=args.write, bus=args.i2c_bus)
        return
    data, failed = bridge.i2c(args.addr,
                              args.cmd if args.cmd is not None else 0xe0,
                              sub=args.reg, read_len=args.count,
                              bus=args.i2c_bus)
    print(f'{data.hex(" ")}{"  (failed)" if failed else ""}')


def cmd_gpio(bridge, args):
    (pin_reg, ddr, port), bit = _parse_pin(args.pin)
    mask = 1 << bit
    if args.set is not None or args.toggle:
        bridge.update8(port, mask, mask if args.set else 0)
        bridge.update8(ddr, 0, mask)
    level = args.set or 0
    for _ in range(args.count if args.toggle else 0):
        level ^= 1
        bridge.update8(port, mask, mask if level else 0)
        print(f'{args.pin.upper()} = {level}', flush=True)
        time.sleep(args.toggle)
    if args.input:
        bridge.update8(ddr, mask, 0)
    d, p, i = (bridge.read8(r) for r in (ddr, port, pin_reg))
    print(f'{args.pin.upper()}: {"output" if d & mask else "input"}, PORT '
          f'{int(bool(p & mask))}, PIN {int(bool(i & mask))}')


def cmd_upload(bridge, args):
    image = pathlib.Path(args.firmware).read_bytes()
    bridge.upload_firmware(image)
    print(f'uploaded {len(image)} bytes; the device now enumerates again')


def cmd_capture(bridge, args):
    a = capture(bridge, args.mode, args.alt, args.seconds, args.out)
    stop_engines(bridge)
    for line in describe_analysis(a):
        print(line)


def cmd_analyze(_, args):
    meta, packets = br.read_capture(args.capture)
    print(json.dumps(meta))
    for line in describe_analysis(analyze_packets(packets)):
        print(line)


def build_parser():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--bus', type=int, help='USB bus of the device')
    p.add_argument('--address', type=int, help='USB address of the device')
    p.add_argument('--results', default=str(HERE / 'results'),
                   help='where the experiments put their results')
    p.add_argument('--unsafe', action='store_true',
                   help='allow accesses with known side effects')
    sub = p.add_subparsers(dest='command', required=True)
    num = lambda s: int(s, 0)  # noqa: E731

    sub.add_parser('info', help='device state and key registers')
    s = sub.add_parser('read', help='read bridge registers')
    s.add_argument('addr', type=num)
    s.add_argument('count', type=num, nargs='?', default=1)
    s = sub.add_parser('write', help='write bridge registers')
    s.add_argument('addr', type=num)
    s.add_argument('values', type=num, nargs='+')
    s.add_argument('--usb', action='store_true',
                   help='allow writes to the USB controller')
    s = sub.add_parser('mode', help='read or set the mode')
    s.add_argument('mode', type=num, nargs='?')
    s = sub.add_parser('i2c', help='one i2c transfer')
    s.add_argument('addr', type=num, help='7 bit address')
    s.add_argument('reg', type=num, nargs='?', help='subaddress')
    s.add_argument('--i2c-bus', type=int, default=0, choices=(0, 1),
                   help='i2c engine')
    s.add_argument('--cmd', type=num, help='engine command')
    s.add_argument('--count', type=num, default=1, help='bytes to read')
    s.add_argument('--write', type=num, nargs='+', help='bytes to write')
    s = sub.add_parser('gpio', help='inspect or drive PA0-7 or PC0-7')
    s.add_argument('pin')
    s.add_argument('--set', type=int, choices=(0, 1),
                   help='make it an output at this level')
    s.add_argument('--toggle', type=float, metavar='SECONDS',
                   help='toggle as an output, this long per level')
    s.add_argument('--count', type=int, default=10, help='toggles')
    s.add_argument('--input', action='store_true',
                   help='make it an input afterwards')
    s = sub.add_parser('upload', help='upload a firmware to the bootloader')
    s.add_argument('firmware')
    s = sub.add_parser('capture', help='capture the stream of a mode')
    s.add_argument('mode', type=num)
    s.add_argument('--alt', type=int, default=2, choices=(1, 2))
    s.add_argument('--seconds', type=float, default=2.0)
    s.add_argument('--out', required=True)
    s = sub.add_parser('analyze', help='statistics of a capture file')
    s.add_argument('capture')

    e = sub.add_parser('exp', help='run an experiment')
    esub = e.add_subparsers(dest='experiment', required=True)
    s = esub.add_parser('timer', help='1: Timer0 prescaler and CPU clock')
    s.add_argument('--seconds', type=float, default=3.0,
                   help='per clock select value')
    s = esub.add_parser('survey', help='2: map the data space')
    s.add_argument('--range', default='0x1000-0xffff')
    s.add_argument('--exclude', action='append', default=[],
                   help='range to leave out, besides 0x1500-0x157f')
    s.add_argument('--write-test', action='append', default=[],
                   metavar='RANGE',
                   help='also flip and restore each byte of this range')
    esub.add_parser('avr', help='3: other ATmega103 peripherals')
    s = esub.add_parser('i2c', help='4: i2c buffers and buses')
    s.add_argument('--buffer-end', type=num, default=0x24,
                   help='last offset of the buffer test (default 0x24, '
                   'which tests 32 bytes)')
    s.add_argument('--commands', action='store_true',
                   help='also try read commands on the decoder')
    s.add_argument('--decoder', type=num, default=0x25,
                   help='7 bit address of the decoder (default 0x25)')
    s = esub.add_parser('endpoints', help='5: endpoint register blocks')
    s.add_argument('--seconds', type=float, default=1.0)
    s.add_argument('--variants', nargs='+', default=list(ENDPOINT_VARIANTS),
                   choices=list(ENDPOINT_VARIANTS))
    s = esub.add_parser('modes', help='6: streams of modes 2 to 5')
    s.add_argument('--seconds', type=float, default=2.0)
    return p


COMMANDS = {'info': cmd_info, 'read': cmd_read, 'write': cmd_write,
            'mode': cmd_mode, 'i2c': cmd_i2c, 'gpio': cmd_gpio,
            'upload': cmd_upload, 'capture': cmd_capture,
            'analyze': cmd_analyze}


def run(args, transport):
    bridge = br.Bridge(transport, allow_unsafe=args.unsafe)
    if args.command != 'exp':
        COMMANDS[args.command](bridge, args)
        return None
    res = Results(args.experiment, args.results)
    res.say(f'experiment {args.experiment}, '
            f'{datetime.datetime.now().isoformat()}')
    try:
        EXPERIMENTS[args.experiment](bridge, res, args)
    finally:
        res.close()
    return res.dir


def main():
    args = build_parser().parse_args()
    if args.command == 'analyze':
        cmd_analyze(None, args)
        return 0
    transport = br.Usb1Transport(bus=args.bus, address=args.address)
    try:
        if args.command not in ('upload', 'info') and \
                transport.ids()[1] == br.BOOTLOADER_ID:
            print('the device runs the bootloader; upload a firmware first',
                  file=sys.stderr)
            return 1
        out = run(args, transport)
        if out:
            print(f'results in {out}')
    finally:
        transport.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
