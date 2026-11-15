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

- **Observed**: read directly from what the firmware does, or, where the
  statement names a device, seen on that device, an EasyCAP 002 for the
  experiments of this project.
- **Inferred**: the most plausible interpretation of observed behaviour,
  not confirmed on hardware.
- **Reported**: found by others on real hardware, or in captures of the
  USB traffic of the vendor's Windows driver; see [Sources](#sources).
- **Windows driver**: read from the code of the vendor's Windows drivers
  for `smi2021_3c.bin` and `smi2021_3f.bin`, from 2010 and 2011; see
  [Sources](#sources).

Unmarked statements are observed. Where nothing is known, the document
says so.

The inside of the bridge, as far as a replacement firmware needs it, is
described in [`hardware.md`](hardware.md): the processor, the memory map,
and how the USB controller and the other blocks are programmed.

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

The microcontroller executes the AVR instruction set, and its core, I/O
registers and SRAM, up to 0x0fff, are those of an ATmega103 (inferred).
The firmware image is a plain AVR program image that starts at address 0
with the interrupt vector table. See [`hardware.md`](hardware.md#cpu-core)
for details.

## Data space

Besides the AVR I/O registers and SRAM, the data space contains the
registers of the bridge's hardware blocks. The host can read and write all
of it with [register access requests](#register-access-requests).

| Address | Use |
|---------|-----|
| 0x0020–0x005f | AVR I/O registers, see above |
| 0x0060–0x0fff | SRAM |
| 0x1500–0x157f | USB device controller, see [`hardware.md`](hardware.md#usb-device-controller) |
| 0x1580–0x15a1 | engine of mode 3, presumably a [transport stream input](#transport-stream-input) |
| 0x1600 | bit 7 is set when streaming starts in any mode; bits 1–0 control the [IR receiver](#ir-receiver) |
| 0x1740 | audio engine control, see [Isochronous stream](#isochronous-stream) |
| 0x1780–0x178f | first i2c engine, see [I2C engines](#i2c-engines) |
| 0x1800 | video engine control |
| 0x1880 | stream source; written as 0, 1 or 2 depending on the mode |
| 0x2000, 0x2040 | buffers of the control endpoint (inferred, from the pointer registers of the [USB device controller](hardware.md#control-endpoint)) |
| 0x3080 | bit 4 is set on suspend, and the Windows driver initializes the device again when it finds it set; bit 2 is pulsed by the Windows driver; bit 7 is cleared by the `wValue` 0xfb request and never set by the firmware, so presumably by the hardware (inferred). See [`hardware.md`](hardware.md#system-control-register-0x3080) |
| 0x3100 | engine of mode 4, written 0 and 1 |
| 0x3180–0x318f | second i2c engine |
| 0x31a5 | read by the Windows driver for `smi2021_3f.bin` to tell boards apart, see [Board identification](#board-identification) |

As on any AVR, an I/O register is also visible in the data space at its I/O
address plus 0x20.

### USB device controller

The registers of the USB device controller, from 0x1500 to 0x157f, are
described in [`hardware.md`](hardware.md#usb-device-controller). The
isochronous stream uses its endpoint register blocks A (0x1520) and B
(0x1530); the [mode](#modes) selects which of them sends.

## Bootloader

The bootloader in ROM is not part of the firmware images. Its protocol, as
used by the driver, consists of vendor requests (`bmRequestType` 0x40 or
0xc0) with `bRequest` 0x01 and `wIndex` 0:

1. **Read the state**: IN, `wValue` 0x01, 2 bytes. The second byte is 0x07
   when the bootloader is ready to receive a firmware.
2. **Upload**: OUT, `wValue` 0x05, 64 bytes each: 0x05, 0xff, then 62 bytes
   of the image, in order. The Windows driver sends the rest of an image
   that is not a multiple of 62 bytes in a shorter last request, so the
   bootloader accepts one (Windows driver); padding the image with 0xff
   works as well.
3. **Start the firmware**: OUT, `wValue` 0x07, 2 bytes: 0x07, 0x00.

The Windows driver takes any second byte other than 0x07 in the state as a
running firmware; a firmware replies to the same request with the mode, see
[IN requests](#in-requests), which is never 0x07 (Windows driver).

The image of `smi2021_3f.bin` is 6620 bytes long, MD5
`17caca24fdd5833b091b1550c371528b`; the file in this repository has 14
more bytes, four 0x00 and the start of another copy of the image, as cut
from the Windows driver. They end up behind the code and do no harm.

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

The firmware handles setup packets, bus reset, the detection of the bus
speed and suspend, see [`hardware.md`](hardware.md#interrupt). It does
nothing on resume.

### Standard requests

`GET_STATUS`, `CLEAR_FEATURE`, `SET_FEATURE`, `SET_ADDRESS`,
`GET_DESCRIPTOR`, `GET_CONFIGURATION`, `SET_CONFIGURATION`,
`GET_INTERFACE` and `SET_INTERFACE` are supported; any other standard
request is stalled.

`SET_INTERFACE` to a non-zero alternate setting starts the engines of the
current [mode](#modes). Selecting alternate setting 0 stops nothing.

## Vendor requests

All vendor requests use `bRequest` 0x01 and `wIndex` 0, apart from the
exceptions given below.

### OUT requests

The firmware ignores `bRequest` and `wValue` of OUT requests and looks at
the first data byte instead. The driver sets `wValue` to that byte, too.
The exception is `bRequest` 0x50 in `smi2021_3f.bin`, see
[Port C request](#port-c-request).

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

Any other `wValue` is stalled. `bRequest` 0x91 in `smi2021_3c.bin` is an
exception, see [IR receiver](#ir-receiver), and so is `bRequest` 0 in
`smi2021_3f.bin`: it returns 5 bytes, the low byte of `wValue` followed by
bytes 1 to 4 of the data of the last OUT request, and the host has no use
for it.

### Port C request

`smi2021_3f.bin` only: an OUT request with `bRequest` 0x50 and no data
stage sets `PC1` and `PC0` from `wValue`:

| `wValue` | `PC1` | `PC0` |
|----------|-------|-------|
| 1 | high | high |
| 2 | low | high |
| 3 | high | low |
| any other | low | low |

The firmware leaves the direction of the pins alone; it makes `PC0` an
output at start, but not `PC1`, so `PC1` is only driven if the host made
it an output. What the pins control is not known.

## Modes

The firmware starts in mode 3. Setting a mode configures endpoint blocks A
and B, and resets the engines of that mode; the engines start once the host
selects a non-zero alternate setting. The block in the second column gets
0x15 and state 01, the other one 0x25 and state 00. Presumably the latter
is the one that sends, as endpoint `0x82`; see [Endpoint register
blocks](hardware.md#endpoint-register-blocks).

| Mode | Block with 0x15 | On the mode request | On `SET_INTERFACE` | 0x1880 |
|------|----------------|---------------------|--------------------|--------|
| 0x02 | B | 0x1740 = 0 | 0x1740 \|= 0x05 | 0 |
| 0x03 | A | 0x1580 = 0x03 | 0x1580 \|= 0x80 | 0 |
| 0x04 | B | 0x3100 = 0 | 0x3100 = 1 | 2 |
| 0x05 | A | 0x1800 = 0, 0x1740 = 0 | 0x1800 \|= 0x0d, 0x1740 \|= 0x05 | 1 |

Mode 5 is video and audio capture from the decoder and the audio ADC. Mode
2 is audio only, and sends the audio data without chunks, see
[Isochronous stream](#isochronous-stream). Mode 3 presumably streams a
transport stream, see [Transport stream input](#transport-stream-input).
What the engine of mode 4 does is not known. This driver uses mode 3 as
standby, which is harmless as it selects alternate setting 0 at the same
time.

Changing the mode does not stop the engines of the previous one, except for
the engines that the new mode resets.

The Windows driver uses mode 5 with alternate setting 2 for video, and
mode 2 with alternate setting 1 for audio only (Windows driver). It starts
a stream by writing 0x1d to 0x1740, waiting 30 ms, writing 0x00 to 0x1740,
and queueing 8 transfers of 64 packets of 3072 bytes each; 300 ms later it
writes 0x0d to 0x1800 and 0x1d to 0x1740. To stop, it writes 0x00 to 0x1740
and aborts the transfers, without a mode request and without selecting
alternate setting 0 (Windows driver). It never uses modes 3 and 4, and none
of the requests `bRequest` 0x91, `bRequest` 0x50 and `wValue` 0xfb.

The captures of the USB traffic show another sequence (reported):

- Video: mode 5, alternate setting 2, then 0x00 to 0x1740 to stop the audio
  data, and after queueing the transfers, 0x0d to 0x1800.
- Audio only: mode 2, alternate setting 1, then 0x1d to 0x1740.
- Video and audio: mode 2 and alternate setting 2, then, while streaming,
  mode 5 and alternate setting 2 again.

Before the mode request, it writes 0x40 and then twice 0x00 to 0x1740, with
a pause of 250 ms between the writes. The Windows drivers that were read
never write 0x40 to 0x1740, so the captures presumably come from another
version.

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

Exceptions, in both images:

- Writing 0x81 to 0x1600 writes 0x01 instead and arms the
  [IR receiver](#ir-receiver). This also makes `PC5` an input in
  `smi2021_3c.bin`, and `PC4` in `smi2021_3f.bin`.
- While bits 1–0 of 0x1600 are `01`, reading 0x1600 to 0x1605 returns the
  receiver's buffer instead of the registers, and marks it as read. In
  `smi2021_3f.bin`, which has no decoder, the buffer never changes.

### I2C transfers

Without flag bit 7, the request runs a transfer on the first i2c engine, or
on the second one with flag bit 6. The firmware fills in the engine's
registers from the request: the command, the slave address, the size, the
first data byte as subaddress, and the data bytes. The command byte is
written to the engine unchanged, so its meaning is that of the engine; see
[I2C engines](#i2c-engines).

The firmware retries a failed transfer up to 16 times, one try right after
the other. If the last try fails too, it sets bit 0 of the command in the
reply to a read. A failed write can't be detected by the host.

The firmware starts no transfers of its own: every access to the decoder
comes from a host request.

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

Commands in use:

| Command | Transfer | Used by |
|---------|----------|---------|
| 0x80 | write the data bytes | the Windows driver, without address bytes (Windows driver) |
| 0x84 | write the data bytes | the Windows driver, to set the subaddress for 0xa0 (reported, Windows driver) |
| 0xa0 | read the data bytes | the Windows driver (reported, Windows driver) |
| 0xc0 | write the subaddress, then the data bytes | this driver, for writes; the Windows driver for writes to the decoder (Windows driver) |
| 0xc4 | presumably as 0xc0 | the Windows driver knows it, with one address byte; what bit 2 changes is not known (Windows driver) |
| 0xe0 | write the subaddress, then read the data bytes | this driver, for reads; found by experiment; the Windows driver knows it, but doesn't use it for the decoder (Windows driver) |

The firmware handles these alike apart from bit 5: it loads the subaddress
register for every command, whether or not the command uses it, and for
reads copies the data bytes into the reply. What bit 6 does, and so the
transfers in this table, is therefore the engine's behaviour, inferred from
the observations below rather than from the firmware.

Bit 5 selects a read, which is the only bit whose meaning is certain. The
firmware's checks above suggest that bit 7 starts a transfer and that bit 6
adds the subaddress byte. Experiments on an EasyCAP 002 with
`smi2021_3c.bin` agree (observed):

- With bit 6, a read starts at the subaddress; without it, it starts where
  an earlier transfer left the decoder's subaddress.
- Bits 2 and 4 make no difference to reads: 0xe0, 0xe4, 0xf0 and 0xf4 return
  the same data with the same rate of failures, over 50 reads each, and so
  do 0xa0, 0xa4, 0xb0 and 0xb4.
- A command with bit 7 clear, 0x20 or 0x60, froze the engine until the
  device lost power, see [I2C engines](hardware.md#i2c-engines). Such
  commands must not be sent.

The Windows driver reads a register of the decoder by writing the register
number with 0x84, then reading one byte with 0xa0, with 0x00 instead of the
register number (reported), and waits 10 ms between the two (Windows
driver), which suggests that 0xa0 does not use the
subaddress. On an EasyCAP 002 with `smi2021_3c.bin`, about a quarter of
these reads return the register that an earlier transfer left the
decoder's subaddress at, without any failure in the reply (observed). So
the setting of the subaddress with 0x84 is lost that often, and as a
write, without a trace.

0xe0 with one address byte, the subaddress, does the combined write and
read of the SAA7113H datasheet in one transfer; the reply has the
subaddress as its address byte, followed by the data. About one in five of
these reads fails despite the firmware's retries, independently of the
ones before and regardless of pauses between the requests, and is reported
in bit 0 of the command in the reply. A read that does not report a
failure has always returned the right value, for reads of one byte. In
reads of two bytes, the second one is 0xff, without a failure, in about one
in five (observed): a host that reads more than one byte can't trust the
bytes after the first. Writes with 0xc0 have not been seen to fail
(observed). The driver reads with 0xe0 and tries again
when a read fails.

The firmware completes a transfer, including its retries, before it
acknowledges the request, so the host needs no delays between requests.

## Isochronous stream

The firmware doesn't touch the stream data; the hardware engines produce it.
In mode 5 the stream consists of chunks of 1024 bytes. Each chunk starts
with a big endian 32 bit magic word that identifies its payload:

| Magic | Payload |
|-------|---------|
| 0xaaaa0000 | 1020 bytes of video data |
| 0xaaaa0001 | 1020 bytes of audio data |

The driver relies on every isochronous packet holding a whole number of
chunks.

In mode 2 there are no chunks (observed, with `smi2021_3c.bin` and alternate
setting 2). Each packet holds the next piece of the audio data as it is, one
packet per microframe of 48 bytes, which is 6 frames, with an occasional
longer one. The data runs on across packets: a packet need not start at a
sample, and different streams were seen to start at 0 and at 2 bytes into
one. The first packet of a stream may also begin with bytes that are not
audio data, about 20 of them. After the audio engine is stopped, the
remaining packets come back as missed.

**Video data** is the 8 bit ITU-R BT.656 output of the decoder: UYVY 4:2:2
lines of 1440 bytes, framed by timing reference codes (0xff 0x00 0x00 and a
word with the F, V and H bits), and the two fields of each frame one after
the other. Lines and codes may be split across chunks. How many lines the
decoder flags as active depends on its configuration; with the settings the
driver uses, the V flag is that of ITU-R BT.656 (OFTS = 00 in decoder
register 0x10), so for 625 line standards these are lines 23–310 and
336–623, 288 per field, and for 525 line standards lines 20–263 and 283–525,
244 and 243 (Tables 9 and 10 of the SAA7113H datasheet).

The Windows driver instead has the decoder generate the V flag from VREF,
and sets the length of VREF per standard (Windows driver): it writes 0x40 to
decoder register 0x10 (OFTS = 01, VRLN = 0) for the 525 line standards and
0x48 (OFTS = 01, VRLN = 1) for the 625 line ones. That flags exactly the
lines it captures: 22–261 and 285–524 with 525 lines, 240 per field, and the
same 288 lines per field as above with 625 lines. The tools of
easycap-somagic-linux write 0x40 for either standard (reported), and so get
286 lines per field with 625 lines. The driver keeps VRLN = 1, which the
saa7115 driver sets for the saa7113 but not for the GM7113C, as the decoder
disables the chrominance comb filter for NTSC and the phase error correction
for PAL outside of VREF. With 525 lines it skips the first two active lines
of each field, so that it captures the same lines as the Windows driver.

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
| `PA7` | pulsed by the host during initialization; on an EasyCAP 002, the reset of the decoder and the audio ADC, see [Initialization](#initialization) |
| `PC0` | output, driven low when the USB controller is set up and on suspend; driven high by the host during initialization; set by the [port C request](#port-c-request) of `smi2021_3f.bin` |
| `PC1` | set by the [port C request](#port-c-request) of `smi2021_3f.bin` |
| `PC2` | made an output, along with `PC0`, by `smi2021_3f.bin` at start and on suspend |
| `PC4` | driven high by the host during initialization; made an input by `smi2021_3f.bin` when the host writes 0x81 to 0x1600 |
| `PC5` | IR receiver input; made an input by `smi2021_3c.bin` when the host writes 0x81 to 0x1600 |

### Inputs

The decoder's four analog inputs are wired as follows (reported). The
EasyCAP 002 column is confirmed on hardware: with a signal on each
connector in turn, only the decoder mode in its row saw it (observed).

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
`PORTC` = 0x11, `PORTA` = 0x80, `PORTA` = 0x00. The Windows driver waits
10 ms after `PORTC` = 0x11, 100 ms after `PORTA` = 0x80 and 10 ms after
the last write, so `PA7` is high for 100 ms (Windows driver). It runs the
sequence again when it finds bit 4 of 0x3080 set after a suspend, clearing
that bit and writing 0x00 to 0x1500 first (Windows driver). The authors of
easycap-somagic-linux took `PC0` and `PC4` for the reset of the audio ADC
and `PA7` for the reset of the decoder (reported).

Measured on an EasyCAP 002 with a multimeter, with each pin driven high
and low in turn from the host (observed):

- `PA7` resets both the decoder and the audio ADC. While it is high, the
  SAA7113H's CE (pin 40) and the CS5340's RST (pin 9) are at 0 V, both
  chips held in reset; while it is low, they are at 3.2 V and 3.3 V. As
  `PA7` reads back low while driven high, it presumably drives the base of
  a transistor that inverts it onto both lines (inferred). So the pulse of
  the initialization resets both chips, and `PA7` must stay low.
- `PC0` and `PC4` change none of these pins, nor the decoder's TRST
  (pin 8), the ADC's mode pins M0 and M1 (pins 1 and 16), or the board's
  LED. Where they go is not known.
- The decoder's TRST is at 0 V, as the datasheet asks for a board without
  boundary scan, and the ADC's M0 and M1 are at 0 V, which selects its
  single speed master mode. The LED is lit all the time.

### Board identification

The Windows driver for `smi2021_3f.bin` tells three boards apart, right
after the upload of the firmware (Windows driver):

1. It reads 0x31a5 and `PINC`. If bits 7–6 of 0x31a5 are `10`, or `00`
   with `PC2` high, this is the first board.
2. Otherwise it makes `PC2` an input and reads `PINC` again: with `PC2`
   high this is the second board, with `PC2` low the third.

As `smi2021_3f.bin` makes `PC2` an output, driven low, when it sets up the
USB controller, the first reading of `PC2` is low on any board, so in
practice bits 7–6 of 0x31a5 pick the first board, and `PC2`, read as an
input, the other two. So `PC2` is presumably a strap, and 0x31a5 holds an
identification that the hardware provides, as no firmware writes it
(inferred). 0x31a5 is the byte after the 32 that the data buffer of the
second i2c engine holds, see [I2C engines](hardware.md#i2c-engines);
bits 5–0 keep a written value, bits 7–6 don't, and the first engine has a
register like it at 0x17a5 (observed). The drivers for `smi2021_3c.bin`
don't read it.

On an EasyCAP 002, 0x31a5 reads 0xc8 after power-up, bits 7–6 `11`, and
`PC2`, which `smi2021_3c.bin` leaves an input, reads high (observed). That
makes it the second board, which matches its decoder and inputs. The third
board is presumably the EasyCAP DC60, a SAA7113 with one composite input on
AI11 and S-Video, see [Inputs](#inputs) (inferred, from how the Windows
driver programs it; no DC60 was tested). The Linux driver identifies the
board this way after the upload, with `PC2` already an input, and offers
the inputs of the EasyCAP 002 or the DC60 accordingly, and all inputs on
the first board or when it can't tell.

The boards differ in the video decoder (Windows driver):

| Board | Decoder | Picture controls | Signal status | Inputs |
|-------|---------|------------------|---------------|--------|
| 1 | a chip at the 8 bit i2c address 0xc0; the driver doesn't access the SAA7113's address on this board | registers 0x2c–0x2f of that chip, each written after 0x01 to its register 0x40 | its register 0x51 | not known; the driver offers two more pins of its crossbar |
| 2 | SAA7113 at 0x4a | SAA7113 registers 0x0a–0x0d | SAA7113 register 0x1f | four, selected with 0xc2, 0xc3, 0xc0 and 0xc1 in SAA7113 register 0x02 for inputs 1 to 4, which is the wiring of the EasyCAP 002, see [Inputs](#inputs) |
| 3 | SAA7113 at 0x4a | none | not known | not known; setting the video standard writes 0xc0, AI11, to SAA7113 register 0x02 for all inputs but one |

On the first board, the driver also sets the video standard through the
chip at 0xc0, in its registers 0x13 and 0x19, instead of through SAA7113
register 0x0e. What that chip is, is not known; that it has registers with
subaddresses rules out a plain tuner PLL.

## Transport stream input

The Windows drivers can program 16 PID filters, although they never select
mode 3 (Windows driver): after pulsing bit 2 of 0x3080 for 10 ms, they
write each PID, ORed with 0x8000, little endian to 0x1582 + 2 × *n*, and
read it back until it reads back as written; then they write 0x83 to
0x1580, or 0xa3 if a flag of the request is set, and read that back, too.
Without PIDs, they only write 0x83 to 0x1580. Together with the transport
stream input that Somagic gives for its sibling chips (see [Related
chips](hardware.md#related-chips)), this makes the engine of mode 3 a
transport stream input with PID filters, and bit 15 of a filter its enable
bit (inferred). The mode request of mode 3 writes 0x03 to 0x1580, and
starting it sets bit 7, which gives the same 0x83.

## IR receiver

Only `smi2021_3c.bin` contains the decoder. It decodes pulses on `PC5`,
timed with Timer0, into a 32 bit code, least significant bit first, after
a long leading pulse (inferred: the NEC infrared remote protocol, which has
this shape). The decoder runs while bits 1–0 of 0x1600 are `01`.

`bRequest` 0x91, IN, arms the decoder and returns 5 bytes: a counter of the
decoder, its status, and three bytes of the decoded code. The status is
0x05 when a complete code was received; reading the buffer through 0x1600
to 0x1605 sets it to 0x01. Whether any device with this
firmware has an IR receiver is not known. The Windows drivers arm it with a
register write to 0x1600 instead, and read the code as six bytes from
0x1600, taking bit 2 of the first byte as the mark of a complete code
(Windows driver).

## Differences between the images

Compared to `smi2021_3c.bin`, `smi2021_3f.bin`

- has no IR decoder and no `bRequest` 0x91 request, but keeps the special
  handling of 0x1600 to 0x1605, see [Bridge registers](#bridge-registers);
- returns 5 bytes for an IN request with `bRequest` 0, see
  [IN requests](#in-requests);
- has the [port C request](#port-c-request);
- makes `PC2` an output when it sets up the USB controller and on suspend;
- knows only the endpoints `0x81` and `0x82` in `GET_STATUS` and the halt
  feature, where `smi2021_3c.bin` also knows `0x83`.

Otherwise both images implement the same requests, modes and register
accesses, and differ only in their descriptors and data layout.

A third image, of 7812 bytes (MD5 `3128c0b8d856c33516f0000ff0565550`), comes
with a Windows driver from 2013 for an "ezcap Video Grabber", whose INF
file binds `1c88:1001`, interface 0 (Windows driver). Its device
descriptor still gives `1c88:003c`; how the device ends up with `1001` is
not known. Compared to `smi2021_3c.bin`, it adds a second interface, a HID
boot keyboard with an interrupt endpoint `0x83` on endpoint register
block C (see [Endpoint register
blocks](hardware.md#endpoint-register-blocks)), presumably for the remote
control; it drops the engine of mode 4; and it drives port A low at start.

## Open questions

See also [Experiments](hardware.md#experiments), which lists tests on
hardware for these and for the questions about the inside of the bridge.

- The meaning of the bits of 0x1800, 0x1880 and of the endpoint register
  blocks, beyond the values listed here, and bit 6 of 0x1740.
- What sets bit 7 of 0x3080, and why the host would clear it.
- What mode 4 streams, and the bits of 0x1580 other than 7.
- The individual bits of the i2c engine commands, beyond bits 5, 6 and 7;
  bits 2 and 4 make no difference to reads.
- What the GPIO lines `PC0` and `PC4` are connected to; `PA7` is the reset
  of the decoder and the audio ADC.
- The bootloader's behaviour beyond the requests listed above.

## Sources

Besides the firmware images and the datasheets, the statements marked as
reported come from the userspace tools of the easycap-somagic-linux project
(GPL-2.0-or-later), whose authors captured the USB traffic of the vendor's
Windows driver and experimented with the hardware:
<https://github.com/killerkalamari/easycap-somagic-linux>, in particular
`somagic-capture.c`, `somagic-audio-capture.c` and `somagic-both.c`.

The statements marked as found in the Windows driver come from reading the
32 bit drivers `SmiUsbGrabber3C.sys` (version 1.0.8.28, January 2011,
whose firmware is identical to `smi2021_3c.bin`, as is that of the older
builds from 2010) and `SmiUsbGrabber3F.sys` (version 1.0.6.6, August
2011, with the 6620 byte image of `smi2021_3f.bin`), and the 2013 driver
mentioned in [Differences between the images](#differences-between-the-images),
as found on driver CDs kept by the Internet Archive:

- <https://archive.org/details/EasyCap_DC60_SMI2021>: `SMI2021C Driver
  v1.0.8.28.zip` and `SMI2021F Driver v1.0.6.6.zip`, the two drivers named
  above, and a CD image with older builds of the first;
- <https://archive.org/details/easycap-somalogic-smi-2021-cd-21c>: install
  CD 2.1C of the EasyCAP DC60, with builds of `SmiUsbGrabber3C.sys` from
  2010;
- <https://archive.org/details/easy-cap-2021_202503>: a CD image with a
  build of `SmiUsbGrabber3F.sys` from October 2011;
- <https://archive.org/details/easycap>: a collection of EasyCAP drivers,
  with copies of the drivers above;
- <https://archive.org/details/autorun_202205>: the driver of 2013 for the
  "ezcap Video Grabber", with the third image.

No driver with the firmware `smi2021_3e.bin` could be found.
