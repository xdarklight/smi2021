#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture video and audio for a long time and check them as they arrive.

check-capture.py needs the whole capture in memory, which rules out more
than a minute or so. This reads the video from v4l2-ctl and the audio from
arecord through pipes instead, decodes the frame counter of every frame and
follows the phase of the tone, and only keeps what went wrong, with the
time it happened. Once a minute it also notes the memory of the system, to
find leaks.

Not everything that looks wrong comes from the capture:

- mpv skips or repeats a frame and leaves a gap in the audio where it loops
  the test clip. Given how long the clip is, the first gap in the audio
  tells where the loops are, and anything close to one is the source's.
  Should that first gap not be a loop, the real loops turn up as failures.
- mpv's clock and the vertical sync of the output drift against each
  other. When they are close to a field apart, mpv shows a frame for one
  field and the next for three, and the counter skips and repeats.

The sequence numbers of the buffers tell the two apart from frames that
the capture lost: a frame the driver had to drop leaves a gap in them. So
skipped or repeated counters while the sequence numbers go on without a
gap are put down to the source.

Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""

import argparse
import importlib.util
import pathlib
import re
import subprocess
import sys
import threading
import time

import numpy as np

here = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('cc', here / 'check-capture.py')
cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cc)

# How far from a loop of the clip an anomaly is put down to the source
LOOP_MARGIN = 1.5
AUDIO_BLOCK = 480


class Log:
    def __init__(self, start):
        self.start = start
        self.lock = threading.Lock()
        self.events = []

    def add(self, when, what, fatal=False):
        """Note an anomaly; fatal ones fail the test wherever they are"""
        with self.lock:
            self.events.append((when, what, fatal))
            print(f'{when - self.start:9.2f} s  {what}', flush=True)


def read_exactly(stream, n):
    data = stream.read(n)
    while data and len(data) < n:
        more = stream.read(n - len(data))
        if not more:
            break
        data += more
    return data


def sequence(stream, log, stats):
    """Follow the sequence numbers that v4l2-ctl --verbose prints"""
    last = None
    for line in stream:
        m = re.match(rb'cap dqbuf: *\d+ seq: *(\d+)', line)
        if not m:
            continue
        seq = int(m.group(1))
        if last is not None and seq != last + 1:
            stats['dropped'] += seq - last - 1
            log.add(time.time(), f'capture: {seq - last - 1} frames dropped '
                                 f'before sequence {seq}', fatal=True)
        last = seq
        stats['buffers'] += 1


def video(stream, height, conf, log, stats):
    frame_bytes = cc.BYTES_PER_LINE * height
    modulus = 1 << conf['BITS']
    last = None
    while True:
        data = read_exactly(stream, frame_bytes)
        if len(data) < frame_bytes:
            break
        now = time.time()
        y = cc.luma(np.frombuffer(data, dtype=np.uint8).reshape(
            1, height, cc.BYTES_PER_LINE))
        f1, amb1 = cc.decode_counters(y, conf, 0)
        f2, amb2 = cc.decode_counters(y, conf, 1)
        f1, f2 = int(f1[0]), int(f2[0])
        stats['frames'] += 1
        if amb1[0] or amb2[0]:
            log.add(now, f'video: frame {stats["frames"]} decoded from '
                         'blocks near the threshold', fatal=True)
        if last is not None:
            step = (f1 - last) % modulus
            if step == 0:
                log.add(now, f'video: counter {f1} repeated')
            elif step > modulus // 2:
                log.add(now, f'video: counter went back {modulus - step}',
                        fatal=True)
            elif step > 1:
                log.add(now, f'video: {step - 1} frames missing before '
                             f'counter {f1}')
        skew = (f2 - f1) % modulus
        if skew not in (0, 1):
            log.add(now, f'video: woven from fields {f1} and {f2}',
                    fatal=True)
        last = f1


def audio(stream, conf, log, stats):
    rate = conf['SAMPLE_RATE']
    tones = (conf['TONE_LEFT'], conf['TONE_RIGHT'])
    block_bytes = AUDIO_BLOCK * 8
    # Find out from the first second which tone each channel carries
    head = read_exactly(stream, rate * 8)
    if len(head) < rate * 8:
        log.add(time.time(), 'audio: ended within the first second')
        return
    first = np.frombuffer(head, dtype='<i4').reshape(-1, 2).astype(float)
    stats['audio_blocks'] += rate // AUDIO_BLOCK
    found = [cc.measure_channel(first[:, c], rate, tones) for c in (0, 1)]
    loudest = max(level for level, _, _ in found)
    follow = [(c, m) for c, (level, _, m) in enumerate(found)
              if m and level >= loudest * cc.SILENCE_FLOOR]
    stats['tones'] = ', '.join(f'{("left", "right")[c]} {m} Hz'
                               for c, m in follow) or 'none'
    if not follow:
        log.add(time.time(), 'audio: no tone on either channel')
        return
    t = np.arange(AUDIO_BLOCK) / rate
    probes = {m: np.exp(-2j * np.pi * m * t) for _, m in follow}
    last = {}
    while True:
        data = read_exactly(stream, block_bytes)
        if len(data) < block_bytes:
            break
        now = time.time()
        a = np.frombuffer(data, dtype='<i4').reshape(-1, 2)
        stats['audio_blocks'] += 1
        stats['full_scale'] += int(np.sum(np.abs(a.astype(np.int64))
                                          >= 0x7fff0000))
        for c, m in follow:
            phase = np.angle((a[:, c] * probes[m]).sum())
            if c in last:
                delta = np.angle(np.exp(1j * (phase - last[c])))
                if abs(delta) > 0.3:
                    if stats['first_gap'] is None:
                        stats['first_gap'] = now
                    log.add(now, f'audio: {("left", "right")[c]} '
                                 f'discontinuity, phase jump of '
                                 f'{-delta / (2 * np.pi * m / rate):+.0f} '
                                 f'samples modulo {rate // m}', fatal=True)
            last[c] = phase


def meminfo():
    info = {}
    for line in open('/proc/meminfo'):
        key, value = line.split(':')
        info[key] = int(value.split()[0])
    return info['MemAvailable'], info['Slab']


def near_loop(when, loop_start, period):
    if not period:
        return False
    k = round((when - loop_start) / period)
    return abs(when - (loop_start + k * period)) < LOOP_MARGIN


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--device', default='/dev/video1')
    parser.add_argument('--audio-device', default='hw:1,0')
    parser.add_argument('--std', choices=('ntsc', 'pal'), default='ntsc')
    parser.add_argument('--minutes', type=float, default=60)
    parser.add_argument('--loop-period', type=float, default=0,
                        help='length of the clip in seconds')
    parser.add_argument('--video-file', type=pathlib.Path,
                        help='check this raw capture instead, for testing')
    parser.add_argument('--audio-file', type=pathlib.Path)
    args = parser.parse_args()

    conf = cc.load_conf(here / 'pattern.conf')
    height = 480 if args.std == 'ntsc' else 576
    start = time.time()
    log = Log(start)
    stats = {'frames': 0, 'audio_blocks': 0, 'full_scale': 0, 'tones': '?',
             'buffers': 0, 'dropped': 0,
             'first_gap': None}
    procs = []

    threads = []

    def follow(target, *targs):
        th = threading.Thread(target=target, args=targs)
        th.start()
        threads.append(th)

    if args.video_file:
        follow(video, open(args.video_file, 'rb'), height, conf, log, stats)
        if args.audio_file:
            follow(audio, open(args.audio_file, 'rb'), conf, log, stats)
    else:
        seconds = int(args.minutes * 60)
        rate = 30000 / 1001 if args.std == 'ntsc' else 25
        v = subprocess.Popen(['v4l2-ctl', '-d', args.device, '--stream-mmap',
                              f'--stream-count={int(seconds * rate)}',
                              '--verbose', '--stream-to', '-'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             bufsize=0)
        # Read from the start: a full pipe would stall v4l2-ctl, and then
        # the driver drops frames for want of buffers
        follow(video, v.stdout, height, conf, log, stats)
        follow(sequence, v.stderr, log, stats)
        # Let the video stream run first, so that the audio joins it
        time.sleep(2)
        a = subprocess.Popen(['arecord', '-q', '-D', args.audio_device,
                              '-f', 'S32_LE', '-c', '2', '-r', '48000',
                              '-d', str(seconds - 4), '-t', 'raw'],
                             stdout=subprocess.PIPE, bufsize=0)
        follow(audio, a.stdout, conf, log, stats)
        procs = [v, a]

    mem = [(start, *meminfo())]
    while any(th.is_alive() for th in threads):
        for th in threads:
            th.join(timeout=60)
        if any(th.is_alive() for th in threads):
            mem.append((time.time(), *meminfo()))
            m = (time.time() - start) / 60
            print(f'{m:6.1f} min: {stats["frames"]} frames, '
                  f'{stats["audio_blocks"] * AUDIO_BLOCK / 48000:.0f} s audio, '
                  f'MemAvailable {mem[-1][1] // 1024} MiB, '
                  f'Slab {mem[-1][2] // 1024} MiB', flush=True)
    for p in procs:
        p.wait()
    mem.append((time.time(), *meminfo()))

    first_gap = stats['first_gap']
    # A frame lost by the capture is never the source's
    loops = [e for e in log.events if first_gap is not None and
             not e[1].startswith('capture:') and
             near_loop(e[0], first_gap, args.loop_period)]
    rest = [e for e in log.events if e not in loops]
    fatal = [e for e in rest if e[2]]
    # Skips and repeats of the counter, while no frame went missing in
    # the capture, are the cadence of the source
    if stats['buffers'] or not args.video_file:
        drops = [e[0] for e in fatal if e[1].startswith('capture:')]
        cadence = [e for e in rest if not e[2] and
                   all(abs(e[0] - d) > 1 for d in drops)]
        # The counter skips where the capture dropped frames
        fatal = sorted(set(rest) - set(cadence))
    else:
        cadence = []
        fatal = rest
    skipped = sum(int(re.match(r'video: (\d+)', e[1]).group(1))
                  for e in cadence if 'missing' in e[1])
    repeated = sum('repeated' in e[1] for e in cadence)
    duration = time.time() - start
    print()
    print(f'{duration / 60:.1f} min, {stats["frames"]} frames, '
          f'{stats["audio_blocks"] * AUDIO_BLOCK / 48000:.0f} s of audio '
          f'({stats["tones"]}), {stats["full_scale"]} samples at full scale')
    print(f'MemAvailable {mem[0][1] // 1024} -> {mem[-1][1] // 1024} MiB, '
          f'Slab {mem[0][2] // 1024} -> {mem[-1][2] // 1024} MiB')
    if stats['buffers']:
        print(f'{stats["buffers"]} buffers, {stats["dropped"]} frames '
              'dropped by the capture')
    print(f'{len(loops)} anomalies at loops of the clip, '
          f'{len(cadence)} cadence slips of the source '
          f'({skipped} counters skipped, {repeated} repeated), '
          f'{len(fatal)} anywhere else')
    for when, what, _ in fatal[:20]:
        print(f'  {when - start:9.2f} s  {what}')
    bad = fatal or stats['frames'] == 0
    print('FAIL' if bad else 'PASS')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
