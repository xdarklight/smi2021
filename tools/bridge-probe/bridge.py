# SPDX-License-Identifier: GPL-2.0-or-later
"""Talk to an SMI2021 bridge through the requests of its original firmware.

The protocol is specified in docs/protocol.md, the hardware it reaches in
docs/hardware.md. Everything here goes through a transport with three
methods, ctrl_out(), ctrl_in() and iso_capture(), so that the experiments can
run against FakeTransport in test_probe.py as well as against a device.

Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
"""

import json
import struct
import time

VENDOR_ID = 0x1c88
BOOTLOADER_ID = 0x0007
FIRMWARE_IDS = (0x003c, 0x003d, 0x003e, 0x003f)

REQUEST = 0x01
STREAM_ENDPOINT = 0x82

# Heads of register access requests, and the length of their replies
REPLY_LENGTH = {0x0b: 13, 0x0c: 21, 0x0d: 37}
REPLY_HEADER = 5
MODE_HEAD = 0x01

FLAG_BRIDGE = 0x80
FLAG_BUS1 = 0x40
FLAG_INFO = 0x10
CMD_READ = 0x20     # command bit 5: read, for bridge registers and i2c
CMD_NAK = 0x01      # set in the reply to a read that failed 16 times

FW_CHUNK = 62

# Reading the event code acknowledges a USB event (inferred), which would
# leave the firmware without it.
UNSAFE_READ = frozenset({0x1503})
# Writing the command register of an i2c engine starts a transfer.
I2C_ENGINES = (0x1780, 0x3180)
USB_CONTROLLER = range(0x1500, 0x1580)


class ProtocolError(Exception):
    pass


class UnsafeAccess(Exception):
    pass


def max_packet_bytes(w_max_packet_size):
    """Bytes per microframe of an endpoint, including high bandwidth."""
    mult = ((w_max_packet_size >> 11) & 3) + 1
    return (w_max_packet_size & 0x7ff) * mult


def _pick_head(reply_bytes):
    for head in (0x0b, 0x0c, 0x0d):
        if reply_bytes <= REPLY_LENGTH[head] - REPLY_HEADER:
            return head
    raise ValueError(f'a reply of {reply_bytes} bytes does not fit')


def _pad(req):
    # The driver always sends at least the 13 bytes of its request structure.
    return bytes(req) + bytes(max(0, 13 - len(req)))


class Bridge:
    """Requests of the original firmware, with guards against side effects."""

    def __init__(self, transport, allow_unsafe=False):
        self.t = transport
        self.allow_unsafe = allow_unsafe

    # -- modes and state ----------------------------------------------------

    def set_mode(self, mode):
        self.t.ctrl_out(MODE_HEAD, bytes([MODE_HEAD, mode]))

    def get_mode(self):
        reply = self.t.ctrl_in(MODE_HEAD, 2)
        if len(reply) != 2 or reply[0] != MODE_HEAD:
            raise ProtocolError(f'unexpected mode reply {reply.hex()}')
        return reply[1]

    def info_query(self):
        """The 16 bytes of SRAM 0x0060 that flag bit 4 returns."""
        head = 0x0c
        self.t.ctrl_out(head, _pad([head, 0, 0, FLAG_INFO, 16]))
        reply = self.t.ctrl_in(head, REPLY_LENGTH[head])
        return bytes(reply[REPLY_HEADER:REPLY_HEADER + 16])

    # -- bridge registers ---------------------------------------------------

    def _check_read(self, addr, n):
        if addr < 0 or addr + n > 0x10000:
            raise ValueError(f'read beyond the data space: 0x{addr:04x}+{n}')
        if self.allow_unsafe:
            return
        for a in UNSAFE_READ:
            if addr <= a < addr + n:
                raise UnsafeAccess(f'reading 0x{a:04x} has side effects')

    def _check_write(self, addr, n, allow_usb=False):
        if addr < 0 or addr + n > 0x10000:
            raise ValueError(f'write beyond the data space: 0x{addr:04x}+{n}')
        if self.allow_unsafe:
            return
        for base in I2C_ENGINES:
            if addr <= base < addr + n:
                raise UnsafeAccess(
                    f'writing 0x{base:04x} starts an i2c transfer')
        if not allow_usb and (addr < USB_CONTROLLER.stop and
                              addr + n > USB_CONTROLLER.start):
            raise UnsafeAccess('writing the USB controller needs allow_usb')

    def read(self, addr, n=1):
        self._check_read(addr, n)
        out = bytearray()
        while n:
            # Two of the reply's bytes go to the address.
            chunk = min(n, REPLY_LENGTH[0x0d] - REPLY_HEADER - 2)
            out += self._read_chunk(addr, chunk)
            addr += chunk
            n -= chunk
        return bytes(out)

    def _read_chunk(self, addr, n):
        head = _pick_head(2 + n)
        req = [head, 0, CMD_READ, FLAG_BRIDGE | 2, n, addr >> 8, addr & 0xff]
        self.t.ctrl_out(head, _pad(req))
        reply = self.t.ctrl_in(head, REPLY_LENGTH[head])
        want = bytes([head, 0, CMD_READ, FLAG_BRIDGE | 2, n,
                      addr >> 8, addr & 0xff])
        if bytes(reply[:7]) != want:
            raise ProtocolError(
                f'reply {bytes(reply[:7]).hex()} to a read of '
                f'0x{addr:04x}+{n}, expected {want.hex()}')
        return bytes(reply[7:7 + n])

    def read8(self, addr):
        return self.read(addr, 1)[0]

    def write(self, addr, data, allow_usb=False):
        data = bytes(data)
        self._check_write(addr, len(data), allow_usb)
        while data:
            chunk = data[:32]
            req = [0x0b, 0, 0, FLAG_BRIDGE | 2, len(chunk),
                   addr >> 8, addr & 0xff] + list(chunk)
            self.t.ctrl_out(0x0b, _pad(req))
            addr += len(chunk)
            data = data[len(chunk):]

    def write8(self, addr, value, allow_usb=False):
        self.write(addr, [value], allow_usb)

    def update8(self, addr, clear, set_, allow_usb=False):
        """Read, modify and write back one register; returns the old value."""
        old = self.read8(addr)
        self.write8(addr, (old & ~clear & 0xff) | set_, allow_usb)
        return old

    # -- i2c ----------------------------------------------------------------

    def i2c(self, addr7, cmd, sub=None, data=b'', read_len=0, bus=0):
        """Run one i2c transfer; returns (data, failed) for reads."""
        sub = b'' if sub is None else bytes([sub]) if isinstance(sub, int) \
            else bytes(sub)
        data = bytes(data)
        if cmd & 0xc0 == 0x80 and sub:
            raise ValueError('the firmware stalls commands 10xxxxxx with '
                             'address bytes')
        if cmd & 0xc0 == 0xc0 and len(sub) != 1:
            raise ValueError('the firmware stalls commands 11xxxxxx without '
                             'exactly one address byte')
        reading = bool(cmd & CMD_READ)
        size = read_len if reading else len(data)
        flags = len(sub) | (FLAG_BUS1 if bus else 0)
        head = _pick_head(len(sub) + size)
        req = [head, (addr7 << 1) & 0xff, cmd, flags, size] + list(sub)
        if not reading:
            req += list(data)
        self.t.ctrl_out(head, _pad(req))
        if not reading:
            return None, None
        reply = self.t.ctrl_in(head, REPLY_LENGTH[head])
        if reply[0] != head or reply[3] != flags or reply[4] != size:
            raise ProtocolError(f'unexpected i2c reply {bytes(reply).hex()}')
        start = REPLY_HEADER + len(sub)
        return bytes(reply[start:start + size]), bool(reply[2] & CMD_NAK)

    # -- bootloader ---------------------------------------------------------

    def upload_firmware(self, image):
        image = bytes(image)
        if len(image) % FW_CHUNK:
            image += b'\xff' * (FW_CHUNK - len(image) % FW_CHUNK)
        state = self.t.ctrl_in(0x01, 2)
        if len(state) != 2 or state[1] != 0x07:
            raise ProtocolError(f'bootloader not ready: {bytes(state).hex()}')
        for i in range(0, len(image), FW_CHUNK):
            self.t.ctrl_out(0x05, bytes([0x05, 0xff]) + image[i:i + FW_CHUNK])
        self.t.ctrl_out(0x07, bytes([0x07, 0x00]))


# -- captures --------------------------------------------------------------
#
# A capture file starts with one line of JSON describing it, followed by one
# record per isochronous packet: status (u8), length (u16 little endian), and
# the data.

CAPTURE_MAGIC = b'SMI2021-CAPTURE '


def write_capture(path, meta, packets):
    with open(path, 'wb') as f:
        f.write(CAPTURE_MAGIC + json.dumps(meta).encode() + b'\n')
        for status, data in packets:
            f.write(struct.pack('<BH', status, len(data)))
            f.write(data)


def read_capture(path):
    with open(path, 'rb') as f:
        line = f.readline()
        if not line.startswith(CAPTURE_MAGIC):
            raise ValueError(f'{path} is not a capture')
        meta = json.loads(line[len(CAPTURE_MAGIC):])
        packets = []
        while True:
            hdr = f.read(3)
            if len(hdr) < 3:
                break
            status, length = struct.unpack('<BH', hdr)
            packets.append((status, f.read(length)))
    return meta, packets


class Usb1Transport:
    """A device, through python-libusb1 (import usb1)."""

    def __init__(self, serial=None, bus=None, address=None,
                 allow_kernel_driver=False, timeout_ms=1000):
        import usb1  # pylint: disable=import-outside-toplevel
        self.usb1 = usb1
        self.timeout = timeout_ms
        self.ctx = usb1.USBContext()
        self.ctx.open()
        found = []
        for dev in self.ctx.getDeviceIterator(skip_on_error=True):
            if dev.getVendorID() != VENDOR_ID:
                continue
            if bus is not None and dev.getBusNumber() != bus:
                continue
            if address is not None and dev.getDeviceAddress() != address:
                continue
            found.append(dev)
        if not found:
            raise ProtocolError('no SMI2021 device found')
        if len(found) > 1:
            raise ProtocolError('several SMI2021 devices; use --bus and '
                                '--address')
        self.dev = found[0]
        self.pid = self.dev.getProductID()
        self.handle = self.dev.open()
        if self.pid != BOOTLOADER_ID and self.handle.kernelDriverActive(0) \
                and not allow_kernel_driver:
            self.close()
            raise ProtocolError(
                'the kernel driver is bound to the device; unload it with '
                '"rmmod smi2021" (the firmware stays in the device)')
        self.claimed = False

    def ids(self):
        return VENDOR_ID, self.pid

    def serial(self):
        try:
            return self.handle.getSerialNumber()
        except self.usb1.USBError:
            return None

    def close(self):
        if getattr(self, 'claimed', False):
            try:
                self.handle.setInterfaceAltSetting(0, 0)
            except self.usb1.USBError:
                pass
            self.handle.releaseInterface(0)
            self.claimed = False
        self.handle.close()
        self.ctx.close()

    def ctrl_out(self, value, data):
        req = self.usb1.TYPE_VENDOR | self.usb1.RECIPIENT_DEVICE
        n = self.handle.controlWrite(req, REQUEST, value, 0, bytes(data),
                                     timeout=self.timeout)
        if n != len(data):
            raise ProtocolError(f'short control write: {n} of {len(data)}')

    def ctrl_in(self, value, length):
        req = self.usb1.TYPE_VENDOR | self.usb1.RECIPIENT_DEVICE
        return bytes(self.handle.controlRead(req, REQUEST, value, 0, length,
                                             timeout=self.timeout))

    def _claim(self):
        if not self.claimed:
            self.handle.claimInterface(0)
            self.claimed = True

    def set_alt(self, alt):
        """SET_INTERFACE; a non-zero setting starts the engines."""
        self._claim()
        self.handle.setInterfaceAltSetting(0, alt)

    def packet_size(self, alt, endpoint=STREAM_ENDPOINT):
        # libusb_get_max_iso_packet_size() takes the first descriptor of the
        # endpoint, which is the one of alternate setting 1, so look it up.
        for setting in self.dev.iterSettings():
            if setting.getNumber() != 0 or \
                    setting.getAlternateSetting() != alt:
                continue
            for ep in setting:
                if ep.getAddress() == endpoint:
                    return max_packet_bytes(ep.getMaxPacketSize())
        raise ProtocolError(f'alternate setting {alt} has no endpoint '
                            f'0x{endpoint:02x}')

    def iso_capture(self, alt, seconds, transfers=8, packets=32,
                    endpoint=STREAM_ENDPOINT):
        """Select alt, keep transfers queued for seconds, select 0 again.

        Returns a list of (status, data), one per packet. Selecting alt
        starts the engines of the current mode."""
        usb1 = self.usb1
        size = self.packet_size(alt, endpoint)
        result = []
        state = {'running': True, 'errors': 0}

        def done(transfer):
            status = transfer.getStatus()
            if status == usb1.TRANSFER_COMPLETED:
                for pkt_status, buf in transfer.iterISO():
                    result.append((pkt_status, bytes(buf)))
            elif status != usb1.TRANSFER_CANCELLED:
                state['errors'] += 1
            if state['running'] and status == usb1.TRANSFER_COMPLETED:
                transfer.submit()

        self.set_alt(alt)
        queued = []
        try:
            for _ in range(transfers):
                transfer = self.handle.getTransfer(iso_packets=packets)
                transfer.setIsochronous(endpoint, size * packets,
                                        callback=done, timeout=1000)
                transfer.submit()
                queued.append(transfer)
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                self.ctx.handleEventsTimeout(0.05)
        finally:
            state['running'] = False
            for transfer in queued:
                if transfer.isSubmitted():
                    try:
                        transfer.cancel()
                    except usb1.USBError:
                        pass
            while any(t.isSubmitted() for t in queued):
                self.ctx.handleEventsTimeout(0.05)
            self.handle.setInterfaceAltSetting(0, 0)
        if state['errors']:
            raise ProtocolError(f'{state["errors"]} transfers failed')
        return result
