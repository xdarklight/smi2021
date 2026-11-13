#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline tests of bridge.py and probe.py against an emulated device.

FakeTransport behaves like the original firmware as docs/protocol.md and
docs/hardware.md describe it, with made-up answers where those documents
say that nothing is known: an ATmega103 Timer0 at 30 MHz, a 32 byte i2c
buffer, and endpoint register blocks that follow the reading favoured in
hardware.md. So these tests show that the experiments do what they should
and come to the right conclusion for that device; they don't show that the
device is like that.

    python3 -m unittest -v test_probe

Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""

import pathlib
import struct
import tempfile
import time
import unittest

import bridge as br
import probe

FCPU = 30e6
ATMEGA103_DIV = {1: 1, 2: 8, 3: 32, 4: 64, 5: 128, 6: 256, 7: 1024}
I2C_BUFFER = 32


class FakeTransport:
    """The original firmware and the hardware it drives, as documented."""

    def __init__(self, pid=0x003c, latency=0.0):
        self.pid = pid
        self.latency = latency
        self.mem = bytearray(0x10000)
        self.mem[0x60:0x70] = bytes(range(16))
        self.mode = 3
        self.alt = 0
        self.reply = bytes(37)
        self.saved = bytes(4)
        self.timer_start = time.monotonic()
        self.timer_base = 0
        # 7 bit address -> registers, per bus
        self.devices = [{0x25: bytearray(range(256))}, {}]
        self.pointer = [0, 0]
        self.uploaded = bytearray()
        self.started = False
        self.unsafe_reads = []
        self.i2c_starts = []
        self._apply_mode(3)

    # -- transport interface ------------------------------------------------

    def ids(self):
        return br.VENDOR_ID, self.pid

    def serial(self):
        return 'FAKE'

    def ctrl_out(self, value, data):
        if self.latency:
            time.sleep(self.latency)
        data = bytes(data)
        if self.pid == br.BOOTLOADER_ID:
            if data[0] == 0x05:
                assert data[1] == 0xff and len(data) == 64
                self.uploaded += data[2:]
            elif data[0] == 0x07:
                self.pid = 0x003c
            return
        if data[0] == 0x01:
            self.mode = data[1]
            self._apply_mode(self.mode)
            return
        self._register_access(data)

    def ctrl_in(self, value, length):
        if self.latency:
            time.sleep(self.latency)
        if self.pid == br.BOOTLOADER_ID:
            return bytes([0x01, 0x07])
        if value == 0x01:
            return bytes([0x01, self.mode])
        reply_len = br.REPLY_LENGTH[value]
        n = reply_len - 5
        return bytes([value]) + self.saved + self.reply[:n]

    def set_alt(self, alt):
        self.alt = alt
        if alt:
            self.started = True
            self._start_engines()

    def iso_capture(self, alt, seconds, transfers=8, packets=32):
        self.set_alt(alt)
        out = []
        count = max(8, int(seconds * 200))
        a, b = self._block(0x1520), self._block(0x1530)
        ep2 = [blk for blk in (('A', a), ('B', b)) if blk[1][0] >> 4 == 2]
        for i in range(count):
            data = b''
            if len(ep2) == 1 and ep2[0][1][1] == 0b00:
                name = ep2[0][0]
                if name == 'B' and self.mode == 5 and alt == 2:
                    data = (b'\xaa\xaa\x00\x00' + bytes(1020)) * 2 + \
                        b'\xaa\xaa\x00\x01' + bytes(1020)
                elif name == 'A' and self.mode == 2:
                    data = bytes((i * 48 + j) & 0xff for j in range(48))
            out.append((0, data))
        self.set_alt(0)
        return out

    # -- firmware -----------------------------------------------------------

    def _block(self, base):
        return self.mem[base + 1], (self.mem[base] >> 1) & 3

    def _set_block(self, base, cfg, state):
        self.mem[base + 1] = cfg
        self.mem[base] = (self.mem[base] & ~0x06) | (state << 1)

    def _apply_mode(self, mode):
        if mode in (3, 5):
            self._set_block(0x1530, 0x25, 0b00)
            self._set_block(0x1520, 0x15, 0b01)
        elif mode in (2, 4):
            self._set_block(0x1520, 0x25, 0b00)
            self._set_block(0x1530, 0x15, 0b01)
        self.mem[0x1538] = self.mem[0x153c] = 0

    def _start_engines(self):
        if self.mode == 5:
            self.mem[0x1800] |= 0x0d
            self.mem[0x1740] |= 0x05
        elif self.mode == 2:
            self.mem[0x1740] |= 0x05
        self.mem[0x1600] |= 0x80

    def _tcnt0(self):
        cs = self.mem[0x53] & 7
        if not cs:
            return self.timer_base
        ticks = (time.monotonic() - self.timer_start) * FCPU / \
            ATMEGA103_DIV[cs]
        return (self.timer_base + int(ticks)) & 0xff

    def _read_mem(self, addr):
        if addr == 0x52:
            return self._tcnt0()
        if addr == 0x1503:
            self.unsafe_reads.append(addr)
        if 0x1785 + I2C_BUFFER <= addr < 0x17c0 or \
                0x3185 + I2C_BUFFER <= addr < 0x31c0:
            return 0
        return self.mem[addr]

    def _write_mem(self, addr, value):
        if addr == 0x52:
            self.timer_base = value
            self.timer_start = time.monotonic()
            return
        if addr == 0x53:
            self.timer_base = self._tcnt0()
            self.timer_start = time.monotonic()
        if 0x1785 + I2C_BUFFER <= addr < 0x17c0 or \
                0x3185 + I2C_BUFFER <= addr < 0x31c0:
            return
        if addr in (0x1780, 0x3180):
            self.i2c_starts.append(addr)
        # Pretend that ADMUX and EEDR don't exist, to check that the
        # experiment tells.
        if addr in (probe.REG['ADMUX'], probe.REG['EEDR']):
            return
        self.mem[addr] = value

    def _register_access(self, req):
        head, i2c_addr, cmd, flags, size = req[:5]
        naddr = flags & 0x0f
        payload = req[5:]
        if flags & br.FLAG_INFO:
            self.reply = bytes(self.mem[0x60:0x70]) + bytes(21)
            self.saved = bytes([0, 0, flags, size])
            return
        if flags & br.FLAG_BRIDGE:
            assert naddr == 2, 'the firmware stalls this'
            addr = payload[0] << 8 | payload[1]
            if cmd & br.CMD_READ:
                data = bytes(self._read_mem(addr + i) for i in range(size))
                self.reply = bytes(payload[:2]) + data + bytes(32)
                self.saved = bytes([i2c_addr, cmd, flags, size])
            else:
                for i, v in enumerate(payload[2:2 + size]):
                    self._write_mem(addr + i, v)
            return
        if cmd & 0xc0 == 0x80:
            assert naddr == 0, 'the firmware stalls this'
        if cmd & 0xc0 == 0xc0:
            assert naddr == 1, 'the firmware stalls this'
        bus = 1 if flags & br.FLAG_BUS1 else 0
        dev = self.devices[bus].get(i2c_addr >> 1)
        sub = payload[:naddr]
        if cmd & br.CMD_READ:
            if dev is None:
                data, failed = bytes(size), True
            else:
                if cmd & 0x40 and sub:
                    self.pointer[bus] = sub[0]
                p = self.pointer[bus]
                data = bytes(dev[(p + i) & 0xff] for i in range(size))
                failed = False
            self.reply = bytes(sub) + data + bytes(32)
            self.saved = bytes([i2c_addr, cmd | (1 if failed else 0),
                                flags, size])
        elif dev is not None:
            p = sub[0] if sub else self.pointer[bus]
            for i, v in enumerate(payload[naddr:naddr + size]):
                dev[(p + i) & 0xff] = v


class Args:
    def __init__(self, **kw):
        self.__dict__.update(kw)


class BridgeTest(unittest.TestCase):
    def setUp(self):
        self.t = FakeTransport()
        self.b = br.Bridge(self.t)

    def test_read_write(self):
        self.b.write(0x2000, bytes(range(100)))
        self.assertEqual(self.b.read(0x2000, 100), bytes(range(100)))
        self.assertEqual(self.b.read8(0x2005), 5)

    def test_guards(self):
        with self.assertRaises(br.UnsafeAccess):
            self.b.read(0x1500, 8)
        with self.assertRaises(br.UnsafeAccess):
            self.b.write8(0x1780, 0xe0)
        with self.assertRaises(br.UnsafeAccess):
            self.b.write8(0x1521, 0x25)
        self.b.write8(0x1521, 0x25, allow_usb=True)
        self.assertEqual(self.t.unsafe_reads, [])
        self.assertEqual(self.t.i2c_starts, [])

    def test_mode_and_info(self):
        self.b.set_mode(5)
        self.assertEqual(self.b.get_mode(), 5)
        self.assertEqual(self.b.info_query(), bytes(range(16)))

    def test_i2c(self):
        data, failed = self.b.i2c(0x25, 0xe0, sub=0x10, read_len=3)
        self.assertEqual((data, failed), (bytes([0x10, 0x11, 0x12]), False))
        self.b.i2c(0x25, 0xc0, sub=0x10, data=b'\x99')
        self.assertEqual(self.b.i2c(0x25, 0xe0, sub=0x10, read_len=1),
                         (b'\x99', False))
        self.assertTrue(self.b.i2c(0x30, 0xe0, sub=0, read_len=1)[1])
        self.assertTrue(self.b.i2c(0x25, 0xa0, read_len=1, bus=1)[1])
        with self.assertRaises(ValueError):
            self.b.i2c(0x25, 0xa0, sub=0, read_len=1)
        with self.assertRaises(ValueError):
            self.b.i2c(0x25, 0xe0, read_len=1)

    def test_upload(self):
        t = FakeTransport(pid=br.BOOTLOADER_ID)
        image = (probe.FIRMWARE_DIR / 'smi2021_3c.bin').read_bytes()
        br.Bridge(t).upload_firmware(image)
        self.assertEqual(len(t.uploaded), 121 * 62)
        self.assertEqual(bytes(t.uploaded[:len(image)]), image)
        self.assertEqual(t.pid, 0x003c)

    def test_packet_size(self):
        self.assertEqual(br.max_packet_bytes(0x1400), 3072)
        self.assertEqual(br.max_packet_bytes(0x03ff), 1023)


class AnalysisTest(unittest.TestCase):
    def test_capture_file(self):
        packets = [(0, b'\x01\x02'), (0, b''), (3, bytes(3072))]
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / 'x.cap'
            br.write_capture(path, {'mode': 5}, packets)
            meta, back = br.read_capture(path)
        self.assertEqual(meta, {'mode': 5})
        self.assertEqual(back, packets)

    def test_chunks_and_ts(self):
        chunk = b'\xaa\xaa\x00\x00' + bytes(1020)
        a = probe.analyze_packets([(0, chunk * 3)] * 10)
        self.assertEqual(a['chunks'], {'video': 30})
        ts = bytes([0x47] + [0x11] * 187) * 50
        a = probe.analyze_packets([(0, ts[i:i + 940])
                                   for i in range(0, len(ts), 940)])
        self.assertEqual(a['ts_sync_fraction'], 1.0)

    def test_timer_rate_drops_stalls(self):
        rate = 29296.875
        samples, t = [], 0.0
        for i in range(2000):
            t += 0.02 if i % 100 == 99 else 0.0005  # stalls over a wrap
            samples.append((t, int(t * rate) & 0xff))
        got, used, _ = probe.timer_rate(samples)
        self.assertAlmostEqual(got, rate, delta=rate * 0.01)
        self.assertGreater(used, 1900)


class ExperimentTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.t = FakeTransport(latency=0.0002)
        self.b = br.Bridge(self.t)

    def tearDown(self):
        self.tmp.cleanup()

    def run_exp(self, name, **kw):
        res = probe.Results(name, self.tmp.name)
        probe.EXPERIMENTS[name](self.b, res, Args(**kw))
        res.close()
        self.assertFalse((pathlib.Path(self.tmp.name) / 'breadcrumb')
                         .exists())
        self.assertEqual(self.t.unsafe_reads, [])
        self.assertEqual(self.t.i2c_starts, [])
        return res

    def test_timer(self):
        res = self.run_exp('timer', seconds=0.3)
        tables = res.data['tables']
        self.assertAlmostEqual(tables['ATmega103']['fcpu'], FCPU,
                               delta=FCPU * 0.03)
        self.assertLess(tables['ATmega103']['worst_error'], 0.05)
        self.assertGreater(tables['ATmega16']['worst_error'], 0.5)
        self.assertEqual(self.t.mem[0x53], 0)

    def test_survey(self):
        image = (probe.FIRMWARE_DIR / 'smi2021_3c.bin').read_bytes()
        self.t.mem[0x8000:0x8000 + len(image)] = image
        res = self.run_exp('survey', range='0x1400-0x8fff',
                           exclude=['0x3000-0x30ff'],
                           write_test=['0x1785-0x17aa'])
        hits = res.data['hits']
        self.assertEqual(hits['smi2021_3c.bin vectors'], ['0x8000'])
        self.assertEqual(hits['smi2021_3c.bin code at 0x200'], ['0x8200'])
        self.assertEqual(len(res.data['write_tests']['0x1785-0x17aa']), 32)
        self.assertEqual(res.data['pages']['0x1500'], 'not read')

    def test_avr(self):
        res = self.run_exp('avr')
        tests = res.data['write_tests']
        self.assertTrue(tests['OCR0']['kept'])
        self.assertFalse(tests['ADMUX']['kept'])
        self.assertTrue(all(t['restored'] for t in tests.values()))

    def test_i2c(self):
        self.t.devices[1][0x50] = bytearray(256)
        res = self.run_exp('i2c', buffer_end=0x3f, commands=True,
                           decoder=0x25)
        self.assertEqual(res.data['buffers']['0x1780']['contiguous'],
                         I2C_BUFFER)
        self.assertEqual(res.data['scan'], {0: [0x25], 1: [0x50]})
        self.assertIn('0xe0', res.data['commands'])

    def test_endpoints(self):
        res = self.run_exp('endpoints', seconds=0.1,
                           variants=list(probe.ENDPOINT_VARIANTS))
        v = res.data['variants']
        self.assertGreater(v['baseline']['chunks'].get('video', 0), 0)
        self.assertGreater(v['both-active']['chunks'].get('video', 0), 0)
        for name in ('swap-both', 'swap-states', 'swap-numbers'):
            self.assertEqual(v[name]['bytes'], 0, name)
        self.assertEqual(res.data['registers'][5][f'0x{0x1531:04x}'], 0x25)

    def test_modes(self):
        res = self.run_exp('modes', seconds=0.1)
        self.assertGreater(res.data['mode5-alt2']['chunks']['video'], 0)
        self.assertEqual(res.data['mode2-alt2']['lengths'], {'48': 20})
        self.assertEqual(res.data['mode3-alt2']['bytes'], 0)


class CliTest(unittest.TestCase):
    def test_parser(self):
        p = probe.build_parser()
        a = p.parse_args(['--bus', '3', 'i2c', '0x25', '0x10',
                          '--i2c-bus', '1'])
        self.assertEqual((a.bus, a.i2c_bus, a.addr, a.reg), (3, 1, 0x25, 16))
        a = p.parse_args(['exp', 'survey', '--write-test', '0x1785-0x17a4'])
        self.assertEqual(a.write_test, ['0x1785-0x17a4'])
        self.assertEqual(probe._parse_range('0x10-0x1f'), range(16, 32))
        self.assertEqual(probe._parse_pin('pc4')[1], 4)

    def test_gpio_and_read(self):
        t = FakeTransport()
        b = br.Bridge(t)
        p = probe.build_parser()
        probe.cmd_gpio(b, p.parse_args(['gpio', 'PA7', '--set', '1']))
        self.assertEqual(t.mem[0x3a] & 0x80, 0x80)
        self.assertEqual(t.mem[0x3b] & 0x80, 0x80)
        probe.cmd_gpio(b, p.parse_args(['gpio', 'PA7', '--input']))
        self.assertEqual(t.mem[0x3a] & 0x80, 0)


if __name__ == '__main__':
    unittest.main()
