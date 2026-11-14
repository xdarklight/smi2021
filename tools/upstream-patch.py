#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""
Turn this repository into the single patch that adds the driver to a Linux
tree, ready to be sent with git send-email.

The patch is created in a temporary git worktree of the Linux tree, so the
tree itself is left untouched. The commit carries no Signed-off-by: tag; add
yours with "git am -s" or "git commit --amend -s" after reviewing the patch.

usage: tools/upstream-patch.py [--keep] LINUX_TREE [OUTPUT_DIR]
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
DRIVER_DIR = 'drivers/media/usb/smi2021'
DRIVER_FILES = ['Kconfig', 'Makefile', 'smi2021.h', 'smi2021-audio.c',
                'smi2021-bootloader.c', 'smi2021-main.c', 'smi2021-v4l2.c']

KCONFIG_LINE = 'source "drivers/media/usb/smi2021/Kconfig"\n'
MAKEFILE_LINE = 'obj-$(CONFIG_VIDEO_SMI2021) += smi2021/\n'
MAINTAINERS_ENTRY = '''\
SOMAGIC SMI2021 VIDEO CAPTURE DRIVER
M:	Martin Blumenstingl <martin.blumenstingl@googlemail.com>
L:	linux-media@vger.kernel.org
S:	Maintained
T:	git git://linuxtv.org/media.git
F:	drivers/media/usb/smi2021/
'''

COMMIT_MESSAGE = '''\
media: usb: add Somagic SMI2021 video capture driver

The Somagic SMI2021 is a USB 2.0 bridge for analog video and audio
capture, found in devices sold as EasyCAP DC60 and EasyCAP 002. It
consists of an AVR microcontroller running a firmware that the host
uploads, and of i2c, video and audio engines. A saa7113 compatible
video decoder, a GM7113C on most devices, is attached to the i2c engine,
and a CS5340 audio ADC to the audio engine.

The devices enumerate as 1c88:0007 until the firmware has been uploaded,
and then as 1c88:003c to 1c88:003f, depending on the firmware. Video and
audio data arrive interleaved on one high bandwidth isochronous
endpoint, in chunks of 1024 bytes that each start with a magic word. The
video data is the ITU-R BT.656 output of the decoder, from which the
driver assembles interlaced UYVY frames. The audio data is 24 bit stereo
PCM at 48 kHz, exposed as an ALSA capture device.

The decoder is handled by the saa7115 driver, which tells a saa7113 from
a gm7113c itself. The devices have either one composite and one S-Video
input, or four composite inputs. As the product id depends on the
uploaded firmware rather than on the board, the driver identifies the
board by its straps once the firmware runs, and offers all of these
inputs on a board it doesn't know.

This is based on the out of tree driver by Jon Arne Jørgensen, which was
submitted as an RFC in 2013, reworked for the current kernel APIs, with
the issues raised in the review of that submission addressed. The
register protocol was verified against the firmware that the driver
uploads.

Link: https://lore.kernel.org/all/1378064571-10537-1-git-send-email-jonarne@jonarne.no/
Assisted-by: Claude:claude-opus-5-5
'''


def run(*args, cwd=None, capture=False, input=None):
    return subprocess.run(args, cwd=cwd, check=True, text=True, input=input,
                          stdout=subprocess.PIPE if capture else None
                          ).stdout


def insert_kconfig(path):
    lines = path.read_text().splitlines(keepends=True)
    # Within the "Analog TV USB devices" block, keep the sources sorted
    start = lines.index('if MEDIA_ANALOG_TV_SUPPORT\n')
    end = lines.index('endif\n', start)
    for i in range(start, end):
        if lines[i].startswith('source ') and lines[i] > KCONFIG_LINE:
            break
    else:
        i = max(j for j in range(start, end)
                if lines[j].startswith('source ')) + 1
    lines.insert(i, KCONFIG_LINE)
    path.write_text(''.join(lines))


def insert_makefile(path):
    lines = path.read_text().splitlines(keepends=True)
    # This part of the Makefile is sorted by Kconfig symbol
    conf = [i for i, l in enumerate(lines) if l.startswith('obj-$(CONFIG_')]
    for i in conf:
        if lines[i] > MAKEFILE_LINE:
            break
    else:
        i = conf[-1] + 1
    lines.insert(i, MAKEFILE_LINE)
    path.write_text(''.join(lines))


def insert_maintainers(path):
    lines = path.read_text().splitlines(keepends=True)
    title = MAINTAINERS_ENTRY.splitlines()[0]
    tag = re.compile(r'^[A-Z]:\t')
    first = next(i for i, l in enumerate(lines) if l.startswith('3C59X '))
    headings = [i for i in range(first, len(lines) - 1)
                if lines[i].strip() and not tag.match(lines[i])
                and tag.match(lines[i + 1])]
    # A few entries are out of order, so go by the closest preceding one
    prev = max((i for i in headings if lines[i].upper() < title),
               key=lambda i: lines[i].upper())
    nxt = next(i for i in headings if i > prev)
    lines.insert(nxt, MAINTAINERS_ENTRY + '\n')
    path.write_text(''.join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[1])
    parser.add_argument('--keep', action='store_true',
                        help='keep the worktree with the commit')
    parser.add_argument('linux', type=pathlib.Path)
    parser.add_argument('outdir', type=pathlib.Path, nargs='?',
                        default=pathlib.Path('.'))
    args = parser.parse_args()

    linux = args.linux.resolve()
    if not (linux / 'drivers/media/usb/Kconfig').is_file():
        sys.exit(f'{linux} does not look like a Linux tree')

    work = pathlib.Path(tempfile.mkdtemp(prefix='smi2021-upstream-'))
    run('git', 'worktree', 'add', '--detach', '--quiet', str(work), 'HEAD',
        cwd=linux)
    try:
        dest = work / DRIVER_DIR
        dest.mkdir()
        for name in DRIVER_FILES:
            shutil.copy(REPO / name, dest / name)

        insert_kconfig(work / 'drivers/media/usb/Kconfig')
        insert_makefile(work / 'drivers/media/usb/Makefile')
        insert_maintainers(work / 'MAINTAINERS')

        run('git', 'add', DRIVER_DIR, 'drivers/media/usb/Kconfig',
            'drivers/media/usb/Makefile', 'MAINTAINERS', cwd=work)
        run('git', 'commit', '--quiet', '--file=-', cwd=work,
            input=COMMIT_MESSAGE)

        args.outdir.mkdir(parents=True, exist_ok=True)
        patch = run('git', 'format-patch', '-1', '-o',
                    str(args.outdir.resolve()), cwd=work,
                    capture=True).strip()
        print(patch)
    finally:
        if args.keep:
            print(f'worktree kept in {work}', file=sys.stderr)
        else:
            run('git', 'worktree', 'remove', '--force', str(work), cwd=linux)


if __name__ == '__main__':
    main()
