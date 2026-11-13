#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a capture of the test pattern for dropped or reordered data.

Decodes the binary frame counter that make-pattern.sh draws, separately for
each of the two interlaced fields, and reports frames that went missing, that
arrived twice or that arrived out of order. With a raw audio capture it also
verifies the per channel tones and looks for gaps in them.

The counter is decoded per field on purpose. A frame handed up by the driver
is woven from two fields that the device sent one after the other, so the two
halves disagreeing says something a whole frame check can not: the fields were
put together wrongly, or in the wrong order. The second field showing a newer
source frame than the first is the exception, as the player can flip between
the two; see main().

Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""

import argparse
import pathlib
import sys

import numpy as np

# A set bit is white and a clear bit is black, so the two are more than 100
# luma apart even after a trip through composite video. Anything this far
# above the black of the strip the counter sits on is taken as a set bit.
BIT_THRESHOLD = 40
# Rows and columns at the edge of a block are skipped: composite video is
# soft and the edges bleed into the black around them.
BLOCK_INSET = 10
# A channel quieter than this, relative to the louder one, is taken as not
# connected rather than as a channel whose tone went missing.
SILENCE_FLOOR = 0.01
# How far a measured peak may sit from a tone that was sent and still count
# as that tone.
TONE_TOLERANCE = 20
WIDTH = 720
BYTES_PER_LINE = WIDTH * 2  # UYVY, SMI2021_BYTES_PER_LINE


def load_conf(path):
    """Parse the KEY=VALUE pairs of pattern.conf."""
    conf = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, _, value = line.partition('=')
        conf[key.strip()] = int(value.strip())
    return conf


def luma(frames):
    """Take the luma plane out of UYVY frames, shaped (frame, line, pixel).

    UYVY packs two pixels into four bytes as U Y0 V Y1, so the luma of every
    pixel is at an odd byte offset.
    """
    return frames[:, :, 1::2]


def decode_counters(y, conf, field):
    """Decode the frame counter from the lines of one field.

    field 0 is the field carried by the even lines of the woven frame, field 1
    the one carried by the odd lines.
    """
    bits, block = conf['BITS'], conf['BLOCK']
    gap, x0, y0 = conf['GAP'], conf['ORIGIN_X'], conf['ORIGIN_Y']

    rows = np.arange(y0 + BLOCK_INSET, y0 + block - BLOCK_INSET)
    rows = rows[rows % 2 == field]
    if rows.size == 0:
        raise SystemExit('counter block is too small to sample a single field')

    # The black strip extends MARGIN to the left of the first block; that is
    # the black reference, taken from the same lines as the blocks themselves.
    ref = y[:, rows, x0 - conf['MARGIN']:x0 - 2].mean(axis=(1, 2))

    counters = np.zeros(y.shape[0], dtype=np.int64)
    ambiguous = np.zeros(y.shape[0], dtype=bool)
    for bit in range(bits):
        x = x0 + bit * (block + gap)
        level = y[:, rows, x + BLOCK_INSET:x + block - BLOCK_INSET]
        level = level.mean(axis=(1, 2)) - ref
        counters |= (level > BIT_THRESHOLD).astype(np.int64) << bit
        # A block that lands near the threshold means the decode can not be
        # trusted, which is itself worth reporting rather than hiding.
        ambiguous |= np.abs(level - BIT_THRESHOLD) < BIT_THRESHOLD / 2
    return counters, ambiguous


def check_sequence(counters, modulus):
    """Find missing, repeated and out of order counter values."""
    missing, repeated, reordered = [], [], []
    for index in range(1, len(counters)):
        step = (int(counters[index]) - int(counters[index - 1])) % modulus
        if step == 1:
            continue
        if step == 0:
            repeated.append(index)
        elif step <= modulus // 2:
            missing.append((index, step - 1))
        else:
            reordered.append((index, step - modulus))
    return missing, repeated, reordered


def measure_channel(samples, rate, tones):
    """Describe what one channel carries.

    Returns its level relative to full scale, the frequency it peaks at, and
    which of the configured tones that is, if any.
    """
    level = float(np.sqrt(np.mean(samples ** 2)) / 2 ** 31)
    spectrum = np.fft.rfft(samples * np.hanning(len(samples)))
    peak = float(np.fft.rfftfreq(len(samples), 1 / rate)[
        np.argmax(np.abs(spectrum))])
    matched = next((t for t in tones if abs(peak - t) < TONE_TOLERANCE), None)
    return level, peak, matched


def check_continuity(samples, rate, nominal, block=480):
    """Look for gaps in a tone.

    The phase of the tone is measured over consecutive blocks by projecting
    onto a complex exponential at its frequency. Both tones divide the sample
    rate exactly, so every block should come out at the same phase; a jump
    means samples went missing or were inserted.

    Where the jump happened is exact, but how big it was is only determined
    modulo one period of the tone, since the phase wraps: at 1000 Hz a gap of
    100 samples and one of 4 samples look the same. Use the jump to find the
    gap, not to size it.
    """
    count = len(samples) // block
    if count < 2:
        return None
    trimmed = samples[:count * block].reshape(count, block)

    phases = np.angle((trimmed * np.exp(-2j * np.pi * nominal *
                                        np.arange(block) / rate)).sum(axis=1))
    jumps = []
    step = 2 * np.pi * nominal / rate
    for index in range(1, count):
        delta = np.angle(np.exp(1j * (phases[index] - phases[index - 1])))
        if abs(delta) > 0.3:
            jumps.append((index * block, round(-delta / step)))
    return jumps, round(rate / nominal)


def describe_wiring(found, tones):
    """Say how the capture device's single audio input appears to be wired.

    The ALSA device is always stereo because the CS5340 is, but a board may
    have only one connector for it. What that does to the two channels is a
    property of the board, not of the driver, so it is reported rather than
    judged.
    """
    live = [name for name, (_, _, matched) in found.items() if matched]
    if not live:
        return 'no tone on either channel'
    if len(live) == 1:
        return f'only the {live[0]} channel carries a tone'
    matches = {found[name][2] for name in live}
    if len(matches) == 1:
        return 'the same tone on both channels, so one input is duplicated'
    if matches == set(tones):
        return 'a different tone per channel, so the two are independent'
    return 'both channels carry a tone'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--video', type=pathlib.Path,
                        help='raw UYVY capture, as v4l2-ctl writes it')
    parser.add_argument('--audio', type=pathlib.Path,
                        help='raw S32_LE stereo capture')
    parser.add_argument('--std', choices=('ntsc', 'pal'), default='ntsc',
                        help='standard the capture was taken in')
    parser.add_argument('--audio-expect', choices=('auto', 'stereo', 'mono'),
                        default='auto',
                        help='auto reports how the channels are wired without '
                             'judging it; stereo and mono require it')
    parser.add_argument('--conf', type=pathlib.Path,
                        default=pathlib.Path(__file__).with_name('pattern.conf'))
    args = parser.parse_args()

    if not args.video and not args.audio:
        parser.error('nothing to check: pass --video, --audio or both')

    conf = load_conf(args.conf)
    failures = 0

    if args.video:
        height = 480 if args.std == 'ntsc' else 576
        frame_bytes = BYTES_PER_LINE * height
        raw = np.fromfile(args.video, dtype=np.uint8)
        count, remainder = divmod(raw.size, frame_bytes)
        print(f'video: {count} frames of {WIDTH}x{height} UYVY')
        if remainder:
            print(f'  WARNING: {remainder} trailing bytes, not a whole frame')
            failures += 1
        if count == 0:
            return 1
        y = luma(raw[:count * frame_bytes].reshape(count, height,
                                                  BYTES_PER_LINE))

        modulus = 1 << conf['BITS']
        per_field = []
        for field in (0, 1):
            counters, ambiguous = decode_counters(y, conf, field)
            per_field.append(counters)
            if ambiguous.any():
                print(f'  field {field + 1}: {ambiguous.sum()} frames decoded '
                      'from blocks near the threshold, check the signal level')
                failures += 1

        # Field 1 is sent first, so it carries the sequence of frames.
        missing, repeated, reordered = check_sequence(per_field[0], modulus)
        total = sum(n for _, n in missing)
        print(f'  frames: {total} missing in {len(missing)} gaps, '
              f'{len(repeated)} repeated, {len(reordered)} out of order')
        for index, n in missing[:10]:
            print(f'    gap of {n} before capture frame {index} '
                  f'(counter {per_field[0][index]})')
        for index in repeated[:10]:
            print(f'    repeat at capture frame {index} '
                  f'(counter {per_field[0][index]})')
        for index, back in reordered[:10]:
            print(f'    went back {abs(back)} at capture frame {index}')
        failures += len(missing) + len(repeated) + len(reordered)

        # Field 2 is sent after field 1, so it can show the same source
        # frame or a newer one. Newer happens when the player's page flip
        # lands on the vblank between the two fields, which an interlaced
        # mode has once per field: a property of the test signal. An older
        # field 2, or one further away, can only come from fields woven
        # from the wrong pair or in the wrong order.
        skew = (per_field[1] - per_field[0]) % modulus
        flipped = np.where(skew == 1)[0]
        bad = np.where((skew != 0) & (skew != 1))[0]
        print(f'  field pairing: {len(bad)} frames woven from the wrong '
              'fields')
        for index in bad[:10]:
            print(f'    frame {index}: field 1 counter {per_field[0][index]}, '
                  f'field 2 counter {per_field[1][index]}')
        failures += len(bad)
        if len(flipped):
            print(f'    note: in {len(flipped)} frames the source moved on '
                  'between the two fields; that is the player flipping on a '
                  'field vblank, not the capture')

    if args.audio:
        raw = np.fromfile(args.audio, dtype='<i4')
        frames = raw.size // 2
        print(f'audio: {frames} frames '
              f'({frames / conf["SAMPLE_RATE"]:.2f} s) of S32_LE stereo')
        if frames == 0:
            return 1
        stereo = raw[:frames * 2].reshape(frames, 2).astype(np.float64)
        tones = (conf['TONE_LEFT'], conf['TONE_RIGHT'])
        rate = conf['SAMPLE_RATE']

        # Measure first and judge after. A board with one audio connector
        # cannot put a different tone on each channel, so which channel
        # carries what is something to find out, not to assert.
        found = {}
        for channel, name in enumerate(('left', 'right')):
            found[name] = measure_channel(stereo[:, channel], rate, tones)

        loudest = max(level for level, _, _ in found.values())
        for name, (level, peak, matched) in found.items():
            if loudest and level < loudest * SILENCE_FLOOR:
                print(f'  {name}: silent')
                continue
            if matched:
                print(f'  {name}: {matched} Hz tone')
            else:
                print(f'  {name}: {peak:.0f} Hz, not one of the tones sent '
                      f'{tones}  <-- UNEXPECTED')
                failures += 1

        print(f'  wiring: {describe_wiring(found, tones)}')
        if args.audio_expect == 'stereo':
            if found['left'][2] != tones[0] or found['right'][2] != tones[1]:
                print('    FAIL: --audio-expect stereo wanted '
                      f'{tones[0]} Hz left and {tones[1]} Hz right')
                failures += 1
        elif args.audio_expect == 'mono':
            matches = {m for _, _, m in found.values() if m}
            if len(matches) != 1:
                print('    FAIL: --audio-expect mono wanted one tone, found '
                      f'{sorted(matches)}')
                failures += 1

        # Continuity is only meaningful where there is a tone to follow, and
        # is checked against the frequency actually found, not the one sent.
        for name, (level, _, matched) in found.items():
            if not matched or (loudest and level < loudest * SILENCE_FLOOR):
                continue
            channel = ('left', 'right').index(name)
            result = check_continuity(stereo[:, channel], rate, matched)
            if result is None:
                print(f'  {name}: too short to check for gaps')
                failures += 1
                continue
            jumps, period = result
            print(f'  {name}: {len(jumps)} discontinuities')
            for offset, lost in jumps[:10]:
                print(f'      at sample {offset}: phase jump of {lost:+d} '
                      f'samples, modulo {period}')
            failures += len(jumps)

    print()
    print('FAIL' if failures else 'PASS')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
