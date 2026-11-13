# SMI2021 hardware and firmware interface

This document specifies how the Somagic SMI2021 USB capture bridge behaves
towards the host, and what its firmware does with the bridge's hardware. It
is meant for people who write a driver for the device, or a replacement for
its firmware, without studying the original firmware themselves.

## About this document

It was written from a static analysis of the two firmware images that the
driver uploads, `smi2021_3c.bin` (MD5 `90f78491e831e8db44cfdd6204a2b602`)
and `smi2021_3f.bin` (MD5 `6454abe92e7d60a3157d158f94afda9b`), and of the
SAA7113H datasheet and ITU-R BT.656-5. It describes interfaces and
behaviour only; it contains no code from the firmware.

Statements are marked by how they were established:

- **Observed**: read directly from what the firmware does.
- **Inferred**: the most plausible interpretation of observed behaviour,
  not confirmed on hardware.
- **Reported**: found by others on real hardware, or in captures of the
  USB traffic of the vendor's Windows driver; see [Sources](#sources).

Unmarked statements are observed. Where nothing is known, the document
says so.

## Overview

The bridge combines a microcontroller with fixed function hardware: a USB
2.0 high speed device controller, two i2c engines, a video engine and an
audio engine. A saa7113 compatible video decoder, a GM7113C on most
devices, sits on the first i2c engine and feeds the video engine with its
BT.656 output. A Cirrus Logic CS5340 audio ADC feeds the audio engine.

The device has no non-volatile memory for its firmware:

1. After power up it enumerates as `1c88:0007`, served by a bootloader in
   ROM (inferred, as the uploaded image starts at the reset vector).
2. The host uploads a firmware image, see [Bootloader](#bootloader).
3. The firmware disconnects from the bus for a moment and enumerates again
   with its own descriptors, as `1c88:003c` to `1c88:003f`.

## Microcontroller

The microcontroller executes the AVR instruction set. Its I/O registers
match those of an ATmega16: the firmware uses ports A and C (`PINC` 0x13,
`DDRC` 0x14, `PORTC` 0x15, `DDRA` 0x1a), Timer0 (`TCNT0` 0x32, `TCCR0`
0x33), the stack pointer (0x3d, 0x3e) and `SREG` (0x3f) at their ATmega16
addresses, and contains unused UART helpers for its UART registers (0x09 to
0x0c). As on any AVR, an I/O register is also visible in the data space at
its I/O address plus 0x20.

Unlike an ATmega16, it has SRAM up to address 0x0fff: the firmware sets up
its stack there.

The firmware image is a plain AVR program image, starting at address 0 with
the interrupt vector table, 4 bytes per vector. The firmware uses two
vectors:

| Vector | Byte address | Use |
|--------|--------------|-----|
| 0      | 0x0000       | reset |
| 11     | 0x002c       | USB device controller events (inferred from what the handler does) |

All other vector entries are `0xffff`.

## Data space

Besides the AVR I/O registers and SRAM, the data space contains the
registers of the bridge's hardware blocks. The host can read and write all
of it with [register access requests](#register-access-requests).

| Address | Use |
|---------|-----|
| 0x0020–0x005f | AVR I/O registers, see above |
| 0x0060–0x0fff | SRAM |
| 0x1500–0x15ff | USB device controller, see below |
| 0x1600 | bit 7 is set when streaming starts in any mode; in `smi2021_3c.bin`, bits 1–0 also control the [IR receiver](#ir-receiver) |
| 0x1740 | audio engine control, see [Isochronous stream](#isochronous-stream) |
| 0x1780–0x178f | first i2c engine, see [I2C engines](#i2c-engines) |
| 0x1800 | video engine control |
| 0x1880 | stream source; written as 0, 1 or 2 depending on the mode |
| 0x2000, 0x2040 | buffers of the control endpoint (inferred, from the pointer registers below) |
| 0x3080 | bit 4 is set on suspend; bit 7 is cleared by the `wValue` 0xfb request and never set by the firmware, so presumably by the hardware (inferred). The Windows driver reads this register during initialization (reported) |
| 0x3100 | engine of mode 4, written 0 and 1 |
| 0x3180–0x318f | second i2c engine |

### USB device controller

| Address | Use |
|---------|-----|
| 0x1500 | bit 3: soft disconnect while set (inferred: the firmware sets it for a while after start, then clears it); bit 2: set on suspend |
| 0x1501 | bit 1: set when the device is connected at high speed |
| 0x1502 | bit 4: setup packet received |
| 0x1503 | event code, see [USB events](#usb-events) |
| 0x1504 | device address, written after `SET_ADDRESS` |
| 0x1507 | initialized to 0xc4 |
| 0x1510 | control endpoint: bits 1 and 2 are cleared, then bit 2 is set to stall |
| 0x1511 | initialized to 0x03 |
| 0x1513, 0x1515 | initialized to 0x40 (inferred: the control endpoint's packet size) |
| 0x1518 | bit 7: data received on the control endpoint |
| 0x1519–0x151a | 0x2000, little endian (inferred: receive buffer address) |
| 0x151b | length of the data to send on the control endpoint |
| 0x151c | bit 7: data ready to send on the control endpoint; the firmware waits for it to clear before it queues more |
| 0x151d–0x151e | 0x2040, little endian (inferred: transmit buffer address) |
| 0x1520–0x1524 | endpoint register block A, see below |
| 0x1530–0x1534 | endpoint register block B, laid out like block A |
| 0x1538, 0x153c | cleared on every mode request |
| 0x1540 | endpoint register block C (inferred): handled like blocks A and B by `SET_CONFIGURATION` and the endpoint halt feature |
| 0x1580 | engine of mode 3: written 0x03 by the mode request, bit 7 set when streaming starts |

The firmware uses blocks A and B for the isochronous stream. Relative to
the base of a block:

| Offset | Use |
|--------|-----|
| +0 | bits 2–1: state of the endpoint. The firmware clears them, then sets bit 1 to enable it (inferred); the endpoint halt feature changes them as well |
| +1 | 0x15 or 0x25, depending on the [mode](#modes) |
| +3, +4 | maximum packet size, little endian: 0x0400 at high speed, 0x03ff at full speed |

## Bootloader

The bootloader in ROM is not part of the firmware images. Its protocol, as
used by the driver, consists of vendor requests (`bmRequestType` 0x40 or
0xc0) with `bRequest` 0x01 and `wIndex` 0:

1. **Read the state**: IN, `wValue` 0x01, 2 bytes. The second byte is 0x07
   when the bootloader is ready to receive a firmware.
2. **Upload**: OUT, `wValue` 0x05, 64 bytes each: 0x05, 0xff, then 62 bytes
   of the image, in order. Images are padded with 0xff to a multiple of 62
   bytes.
3. **Start the firmware**: OUT, `wValue` 0x07, 2 bytes: 0x07, 0x00.

All variants use the same USB id while in the bootloader, so the host can't
tell which firmware a device came with.

## USB device

| Image | USB id | Serial number string |
|-------|--------|----------------------|
| `smi2021_3c.bin` | `1c88:003c` | `SMIGRABBER9876543210` |
| `smi2021_3f.bin` | `1c88:003f` | `SMIVIDEO3E9876543210` |

Both report the manufacturer " Somagic, Inc.  " and a product string of the
form "SMI Grabber Dev", USB 2.0, a control endpoint packet size of 64, and
one configuration with one vendor specific interface (class, subclass and
protocol 0xff) with three alternate settings:

| Alternate setting | Endpoint |
|-------------------|----------|
| 0 | none |
| 1 | 0x82, isochronous IN, 1023 bytes, every microframe |
| 2 | 0x82, isochronous IN, 3 × 1024 bytes (`wMaxPacketSize` 0x1400), every microframe |

The descriptors contain one more endpoint descriptor with 1023 bytes, which
presumably replaces the one of alternate setting 2 at full speed.

### USB events

The firmware handles everything in its USB interrupt, which reads the event
code at 0x1503 and defers the work to its main loop:

| Code | Event |
|------|-------|
| 0x01 with 0x1502 bit 4 | setup packet on the control endpoint |
| 0x80 | speed known: sets the maximum packet size of endpoint blocks A and B from 0x1501 bit 1 |
| 0x81 | bus reset: resets the control endpoint state |
| 0x85 | suspend (inferred): drives `PC0` low and sets 0x3080 bit 4 and 0x1500 bit 2 |

### Standard requests

`GET_STATUS`, `CLEAR_FEATURE`, `SET_FEATURE`, `SET_ADDRESS`,
`GET_DESCRIPTOR`, `GET_CONFIGURATION`, `SET_CONFIGURATION`,
`GET_INTERFACE` and `SET_INTERFACE` are supported; any other standard
request is stalled.

`SET_INTERFACE` to a non-zero alternate setting starts the engines of the
current [mode](#modes). Selecting alternate setting 0 stops nothing.

## Vendor requests

All vendor requests use `bRequest` 0x01, except one, and `wIndex` 0.

### OUT requests

The firmware ignores `bRequest` and `wValue` of OUT requests and looks at
the first data byte instead. The driver sets `wValue` to that byte, too.

| First byte | Request |
|------------|---------|
| 0x01 | [set the mode](#modes): 0x01, mode |
| any other | [register access](#register-access-requests) |

### IN requests

`wValue` selects the reply, whose first byte repeats it:

| `wValue` | Length | Reply |
|----------|--------|-------|
| 0x01 | 2 | 0x01, current mode |
| 0x0b | 13 | reply to a register read, with up to 8 bytes of data |
| 0x0c | 21 | the same with up to 16 bytes of data |
| 0x0d | 37 | the same with up to 32 bytes of data |
| 0xfb | 2 | 0xfb, 0x01; clears bit 7 of 0x3080, which the host could also do with a register write (purpose unknown) |

Any other `wValue` is stalled. `bRequest` 0x91 is the exception, see
[IR receiver](#ir-receiver).

## Modes

The firmware starts in mode 3. Setting a mode configures endpoint blocks A
and B, and resets the engines of that mode; the engines start once the host
selects a non-zero alternate setting. The block in the second column is set
to 0x15 and enabled, the other one is set to 0x25.

| Mode | Endpoint block | On the mode request | On `SET_INTERFACE` | 0x1880 |
|------|----------------|---------------------|--------------------|--------|
| 0x02 | B | 0x1740 = 0 | 0x1740 \|= 0x05 | 0 |
| 0x03 | A | 0x1580 = 0x03 | 0x1580 \|= 0x80 | 0 |
| 0x04 | B | 0x3100 = 0 | 0x3100 = 1 | 2 |
| 0x05 | A | 0x1800 = 0, 0x1740 = 0 | 0x1800 \|= 0x0d, 0x1740 \|= 0x05 | 1 |

Mode 5 is video and audio capture from the decoder and the audio ADC. Mode
2 presumably is audio only. What the engines of modes 3 and 4 do is not
known. The driver uses mode 3 as standby, which is harmless as it selects
alternate setting 0 at the same time.

Changing the mode does not stop the engines of the previous one, except for
the engines that the new mode resets.

The Windows driver uses these sequences (reported):

- Video: mode 5, alternate setting 2, then 0x00 to 0x1740 to stop the audio
  data, and after queueing the transfers, 0x0d to 0x1800.
- Audio only: mode 2, alternate setting 1, then 0x1d to 0x1740.
- Video and audio: mode 2 and alternate setting 2, then, while streaming,
  mode 5 and alternate setting 2 again.

Before the mode request, it writes 0x40 and then twice 0x00 to 0x1740, with
a pause of 250 ms between the writes.

## Register access requests

A register access request is an OUT request:

| Offset | Field | Meaning |
|--------|-------|---------|
| 0 | head | 0x0b, 0x0c or 0x0d (inferred: selects the size of the reply, see above) |
| 1 | i2c address | 8 bit slave address, i.e. the 7 bit address shifted left by one; unused for bridge registers |
| 2 | command | see below |
| 3 | flags | bits 3–0: number of address bytes; bit 4: info query; bit 6: second i2c engine; bit 7: bridge register |
| 4 | size | number of data bytes |
| 5 | data | the address bytes, followed by the data bytes |

The firmware rejects (stalls) a request

- for a bridge register with a number of address bytes other than 2,
- with command bits 7–6 equal to `10` and any address bytes,
- with command bits 7–6 equal to `11` and other than one address byte.

To read, the host sends the request, then fetches the reply with an IN
request with `wValue` equal to the head. The reply has the same layout as
the request: the head, the i2c address, the command, the flags, the size,
the address bytes and the data that was read.

### Bridge registers

With flag bit 7 set, the two address bytes are a 16 bit address in the
data space, most significant byte first. Command bit 5 selects a read;
otherwise this is a write. The firmware copies `size` bytes to or from
consecutive addresses, so this reaches the I/O registers, the SRAM and all
hardware blocks.

Exceptions in `smi2021_3c.bin`:

- Writing 0x81 to 0x1600 writes 0x01 instead and arms the
  [IR receiver](#ir-receiver).
- While bits 1–0 of 0x1600 are `01`, reading 0x1600 to 0x1605 returns the
  receiver's buffer instead of the registers, and marks it as read.

### I2C transfers

Without flag bit 7, the request runs a transfer on the first i2c engine, or
on the second one with flag bit 6. The firmware fills in the engine's
registers from the request: the command, the slave address, the size, the
first data byte as subaddress, and the data bytes. The command byte is
written to the engine unchanged, so its meaning is that of the engine; see
[I2C engines](#i2c-engines).

The firmware retries a failed transfer up to 16 times. If the last try
fails too, it sets bit 0 of the command in the reply to a read. A failed
write can't be detected by the host.

With flag bit 4, the request doesn't access the i2c bus; the reply instead
contains 16 bytes from SRAM 0x0060 to 0x006f. Their meaning is unknown;
their initial values are 0x00 to 0x0f.

## I2C engines

The engines are at 0x1780 and 0x3180, with the same layout:

| Offset | Use |
|--------|-----|
| +0 | command; writing it starts the transfer |
| +1 | number of data bytes |
| +2 | status: bit 4 busy, bits 6 or 7 set on completion, bit 5 set on failure (NAK); the firmware clears it before each transfer |
| +3 | subaddress |
| +4 | slave address, 8 bit |
| +5… | data bytes |

Commands used by the driver:

| Command | Transfer |
|---------|----------|
| 0x84 | write the data bytes |
| 0xa0 | read the data bytes |
| 0xc0 | write the subaddress, then the data bytes |

Bit 5 selects a read, which is the only bit whose meaning is certain. The
firmware's checks above suggest that bit 7 starts a transfer and that bit 6
adds the subaddress byte.

To read a register of the decoder, the driver first writes the register
number with 0x84, then reads one byte with 0xa0. The Windows driver does the
same, but sends 0x00 instead of the register number with 0xa0 (reported),
which suggests that 0xa0 does not use the subaddress. Bit 2, only set in
0x84, may keep the bus for a repeated start, which would make the pair the
combined write and read of the SAA7113H datasheet (inferred).

The firmware completes a transfer, including its retries, before it
acknowledges the request, so the host needs no delays between requests.

## Isochronous stream

The firmware doesn't touch the stream data; the hardware engines produce it.
The stream consists of chunks of 1024 bytes. Each chunk starts with a big
endian 32 bit magic word that identifies its payload:

| Magic | Payload |
|-------|---------|
| 0xaaaa0000 | 1020 bytes of video data |
| 0xaaaa0001 | 1020 bytes of audio data |

The driver relies on every isochronous packet holding a whole number of
chunks.

**Video data** is the 8 bit ITU-R BT.656 output of the decoder: UYVY 4:2:2
lines of 1440 bytes, framed by timing reference codes (0xff 0x00 0x00 and a
word with the F, V and H bits), and the two fields of each frame one after
the other. Lines and codes may be split across chunks. How many lines the
decoder flags as active depends on its configuration; with the settings the
driver uses, these are 288 lines per field for 625 line standards and 242
for 525 line standards.

**Audio data** is a stream of 32 bit little endian containers, each holding
a 24 bit sample in its upper three bytes and 0x00 in its lowest byte,
alternating between two channels, at 48 kHz. Frames may be split across
chunks. This format comes from the host side; the firmware doesn't
configure it.

The audio engine is controlled by 0x1740 (reported, from experiments):

| Bit | Effect |
|-----|--------|
| 0, 2 | must both be set for audio data to be sent |
| 1 | swaps the left and the right channel |
| 3, 4 | with both set, the samples are aligned as described above; otherwise the data starts 2 bytes into a sample |
| 6 | written as 0x40 and then 0 by the Windows driver during initialization; effect unknown |

As the firmware sets 0x05 when streaming starts in modes 2 and 5, audio
chunks are sent unless the host clears 0x1740. The driver writes 0x1d to
start audio and 0x00 to stop it.

## GPIO and board signals

| Pin | Use |
|-----|-----|
| Port A | all outputs after start |
| `PA7` | pulsed by the host during initialization |
| `PC0` | output, driven low when the USB controller is set up and on suspend; driven high by the host during initialization |
| `PC4` | driven high by the host during initialization |
| `PC5` | IR receiver input (`smi2021_3c.bin` only) |

### Inputs

The decoder's four analog inputs are wired as follows (reported):

| Decoder mode | Pins | EasyCAP 002 | EasyCAP DC60 |
|--------------|------|-------------|--------------|
| 0 | AI11 | VIDEO 3 | composite |
| 1 | AI12 | VIDEO 4 | |
| 2 | AI21 | VIDEO 1 | |
| 3 | AI22 | VIDEO 2 | |
| 7 | Y on AI12, C on AI22 | | S-Video |

### Initialization

The host's initialization sequence, a series of bridge register writes, is:
`DDRA` = 0x80, `PORTA` = 0x00, `DDRC` = 0x01, `PORTC` = 0x00, `DDRC` = 0x11,
`PORTC` = 0x11, `PORTA` = 0x80, `PORTA` = 0x00. What the pins are connected
to is not known; the authors of easycap-somagic-linux took `PC0` and `PC4`
for the reset of the audio ADC and `PA7` for the reset of the decoder
(reported, unconfirmed).

## IR receiver

Only `smi2021_3c.bin` contains this. It decodes pulses on `PC5`, timed with
Timer0, into a 32 bit code, least significant bit first, after a long
leading pulse (inferred: the NEC infrared remote protocol, which has this
shape). The decoder runs while bits 1–0 of 0x1600 are `01`.

`bRequest` 0x91, IN, arms the decoder and returns 5 bytes: a counter of the
decoder, its status, and three bytes of the decoded code. The status is
0x05 when a complete code was received; reading the buffer through 0x1600
to 0x1605 sets it to 0x01. Whether any device with this
firmware has an IR receiver is not known.

## Differences between the images

`smi2021_3f.bin` lacks the IR receiver: the `bRequest` 0x91 request and the
special handling of 0x1600 to 0x1605. Otherwise both images implement the
same requests, modes and register accesses, and differ only in their
descriptors and data layout.

## Open questions

- The meaning of the bits of 0x1800, 0x1880 and of the endpoint register
  blocks, beyond the values listed here, and bit 6 of 0x1740.
- What sets bit 7 of 0x3080, and why the host would clear it.
- What modes 3 and 4 stream.
- The individual bits of the i2c engine commands, beyond bit 5.
- Which chips the GPIO lines `PA7`, `PC0` and `PC4` are connected to.
- The bootloader's behaviour beyond the requests listed above.

## Sources

Besides the firmware images and the datasheets, the statements marked as
reported come from the userspace tools of the easycap-somagic-linux project
(GPL-2.0-or-later), whose authors captured the USB traffic of the vendor's
Windows driver and experimented with the hardware:
<https://github.com/killerkalamari/easycap-somagic-linux>, in particular
`somagic-capture.c`, `somagic-audio-capture.c` and `somagic-both.c`.
