#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture into dma-bufs from a DMA-BUF heap and check what arrives.

v4l2-compliance can only test DMABUF with a second V4L2 device that exports
buffers, and the Le Potato has none that will. This allocates the buffers
from /dev/dma_heap/system instead, queues them as V4L2_MEMORY_DMABUF,
streams, and reads every frame back through its dma-buf to decode the
frame counter of the test pattern, so that it checks the import path of
the driver end to end.

Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""

import argparse
import ctypes
import fcntl
import importlib.util
import mmap
import os
import pathlib
import struct
import sys
import time

import numpy as np


def ioc(direction, kind, nr, size):
    return direction << 30 | size << 16 | ord(kind) << 8 | nr


IOW, IOWR = 1, 3
V4L2_BUF_TYPE_VIDEO_CAPTURE = 1
V4L2_MEMORY_DMABUF = 4
V4L2_BUF_FLAG_ERROR = 0x40

# struct v4l2_requestbuffers: count, type, memory, capabilities, flags
REQBUFS = struct.Struct('IIIIB3x')
# struct v4l2_buffer on a 64 bit kernel: index, type, bytesused, flags,
# field, timestamp, timecode, sequence, memory, m, length, reserved2,
# request_fd
BUFFER = struct.Struct('IIIII4xqq16sIIqIIi4x')
assert BUFFER.size == 88

VIDIOC_REQBUFS = ioc(IOWR, 'V', 8, REQBUFS.size)
VIDIOC_QBUF = ioc(IOWR, 'V', 15, BUFFER.size)
VIDIOC_DQBUF = ioc(IOWR, 'V', 17, BUFFER.size)
VIDIOC_STREAMON = ioc(IOW, 'V', 18, 4)
VIDIOC_STREAMOFF = ioc(IOW, 'V', 19, 4)
VIDIOC_S_INPUT = ioc(IOWR, 'V', 39, 4)
# struct dma_heap_allocation_data: len, fd, fd_flags, heap_flags
HEAP_ALLOC = struct.Struct('QIIQ')
DMA_HEAP_IOCTL_ALLOC = ioc(IOWR, 'H', 0, HEAP_ALLOC.size)
DMA_BUF_IOCTL_SYNC = ioc(IOW, 'b', 0, 8)
DMA_BUF_SYNC_READ, DMA_BUF_SYNC_START, DMA_BUF_SYNC_END = 1, 0, 4


def xioctl(fd, request, data):
    buf = bytearray(data)
    fcntl.ioctl(fd, request, buf)
    return bytes(buf)


def buffer(index, dmabuf, length):
    return BUFFER.pack(index, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0, 0, 0, 0, 0,
                       bytes(16), 0, V4L2_MEMORY_DMABUF, dmabuf, length, 0, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--device', default='/dev/video1')
    parser.add_argument('--heap', default='/dev/dma_heap/system')
    parser.add_argument('--std', choices=('ntsc', 'pal'), default='ntsc',
                        help='standard of the signal, which must be set')
    parser.add_argument('--input', type=int, default=0,
                        help='input the signal is on')
    parser.add_argument('--buffers', type=int, default=4)
    parser.add_argument('--frames', type=int, default=100)
    args = parser.parse_args()

    here = pathlib.Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('cc', here / 'check-capture.py')
    cc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cc)
    conf = cc.load_conf(here / 'pattern.conf')

    height = 480 if args.std == 'ntsc' else 576
    size = cc.BYTES_PER_LINE * height

    heap = os.open(args.heap, os.O_RDWR)
    dmabufs, maps = [], []
    for _ in range(args.buffers):
        alloc = xioctl(heap, DMA_HEAP_IOCTL_ALLOC,
                       HEAP_ALLOC.pack(size, 0, os.O_RDWR | os.O_CLOEXEC, 0))
        fd = HEAP_ALLOC.unpack(alloc)[1]
        dmabufs.append(fd)
        maps.append(mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ))

    dev = os.open(args.device, os.O_RDWR)
    xioctl(dev, VIDIOC_S_INPUT, struct.pack('i', args.input))
    # Frames from before the decoder locked onto the input are garbage
    time.sleep(1)
    got = REQBUFS.unpack(xioctl(dev, VIDIOC_REQBUFS, REQBUFS.pack(
        args.buffers, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_DMABUF, 0, 0)))
    if got[0] != args.buffers:
        print(f'REQBUFS gave {got[0]} buffers, asked for {args.buffers}')
        return 1
    for i, fd in enumerate(dmabufs):
        xioctl(dev, VIDIOC_QBUF, buffer(i, fd, size))
    xioctl(dev, VIDIOC_STREAMON, struct.pack('i', V4L2_BUF_TYPE_VIDEO_CAPTURE))

    counters = [[], []]
    errors = short = 0
    for _ in range(args.frames):
        b = BUFFER.unpack(xioctl(dev, VIDIOC_DQBUF, buffer(0, 0, 0)))
        index, bytesused, flags = b[0], b[2], b[3]
        if flags & V4L2_BUF_FLAG_ERROR:
            errors += 1
        if bytesused != size:
            short += 1
        fd = dmabufs[index]
        fcntl.ioctl(fd, DMA_BUF_IOCTL_SYNC,
                    struct.pack('Q', DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
        frame = np.frombuffer(maps[index], dtype=np.uint8).reshape(
            1, height, cc.BYTES_PER_LINE).copy()
        fcntl.ioctl(fd, DMA_BUF_IOCTL_SYNC,
                    struct.pack('Q', DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
        y = cc.luma(frame)
        for field in (0, 1):
            counters[field].append(int(cc.decode_counters(y, conf, field)[0][0]))
        xioctl(dev, VIDIOC_QBUF, buffer(index, fd, size))

    xioctl(dev, VIDIOC_STREAMOFF, struct.pack('i', V4L2_BUF_TYPE_VIDEO_CAPTURE))
    xioctl(dev, VIDIOC_REQBUFS, REQBUFS.pack(
        0, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_DMABUF, 0, 0))

    modulus = 1 << conf['BITS']
    missing, repeated, reordered = cc.check_sequence(np.array(counters[0]),
                                                     modulus)
    skew = (np.array(counters[1]) - np.array(counters[0])) % modulus
    woven = int(np.sum((skew != 0) & (skew != 1)))
    print(f'{args.frames} frames into {args.buffers} dma-bufs from {args.heap}')
    print(f'  {errors} with the error flag, {short} with bytesused != {size}')
    print(f'  {sum(n for _, n in missing)} missing in {len(missing)} gaps, '
          f'{len(repeated)} repeated, {len(reordered)} out of order, '
          f'{woven} woven from the wrong fields')
    bad = errors + short + len(missing) + len(repeated) + len(reordered) + woven
    print('FAIL' if bad else 'PASS')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
