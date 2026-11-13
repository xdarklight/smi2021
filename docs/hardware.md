# SMI2021 hardware reference for firmware authors

This document describes the inside of the Somagic SMI2021 bridge as far as
a replacement for its firmware needs to know it: the processor, the memory
map, which blocks are standard AVR peripherals and which are Somagic's own,
and how each block is programmed. What the firmware has to present to the
host (descriptors, vendor requests, modes and the stream format) is
specified in [`protocol.md`](protocol.md); this document refers to it
instead of repeating it.

## About this document

It was written from a static analysis of `smi2021_3c.bin` (MD5
`90f78491e831e8db44cfdd6204a2b602`) and `smi2021_3f.bin` (MD5
`6454abe92e7d60a3157d158f94afda9b`), compared with the ATmega103 and
ATmega16 datasheets and the device headers of avr-libc, with the drivers
for other controllers in Linux, U-Boot and Zephyr, and with Somagic's own
descriptions of its other chips, see [Sources](#sources). It contains no
code from the firmware. Statements use the same markers as
[`protocol.md`](protocol.md#about-this-document): unmarked statements are
**observed** in the firmware, **inferred** ones are interpretations that
have not been checked on hardware, and **reported** ones come from others'
experiments. Where a statement needs a test on hardware, the test is listed
in [Experiments](#experiments).

Addresses are data space addresses unless noted otherwise. Program memory
addresses are byte addresses, as in `avr-objdump` output.

## Summary of blocks

| Block | Address | Standard? |
|-------|---------|-----------|
| CPU core | | AVR instruction set; see [CPU core](#cpu-core) |
| General purpose registers, `SREG`, stack pointer | 0x0000–0x001f, 0x005d–0x005f | standard AVR |
| Port A, port C | 0x0033–0x0035, 0x003a–0x003b | ATmega103 layout, but port C can also be an input |
| Timer/Counter0 | 0x0052–0x0053 | ATmega103 |
| UART | 0x0029–0x002c | ATmega103, only in unused code |
| SPI | 0x002d–0x002f (inferred) | presumably ATmega103, unused, see [SPI](#spi) |
| SRAM | 0x0060–0x0fff | ATmega103 |
| USB 2.0 device controller | 0x1500–0x157f | Somagic's own |
| Engine of mode 3 | 0x1580 | Somagic's own |
| Stream and IR control | 0x1600 | Somagic's own |
| Audio engine | 0x1740 | Somagic's own |
| First i2c engine | 0x1780–0x17bf (inferred extent) | Somagic's own, **not** Atmel's TWI |
| Video engine | 0x1800 | Somagic's own |
| Stream source | 0x1880 | Somagic's own |
| USB buffer memory | 0x2000– | Somagic's own |
| System control | 0x3080 | Somagic's own |
| Engine of mode 4 | 0x3100 | Somagic's own |
| Second i2c engine | 0x3180–0x31bf (inferred extent) | same as the first one |

The processor and its peripherals are those of an Atmel ATmega103. None
of the hardware specific to the device could be matched to a controller
with a driver in Linux, U-Boot or Zephyr, see [Comparison with known
controllers](#comparison-with-known-controllers).

The firmware uses none of the other ATmega103 peripherals: SPI, ADC,
analog comparator, EEPROM, watchdog, Timer/Counter1 and 2, external
interrupts and the external memory interface. `SLEEP` and `SPM` never
appear either. Whether the chip has them is unknown, except that Somagic
lists an SPI controller for its sibling chip.

## CPU core

The firmware is AVR machine code, and everything about it fits an Atmel
ATmega103 (inferred, from the following observations):

| Property | Firmware | ATmega103 | ATmega16, for comparison |
|----------|----------|-----------|--------------------------|
| Vector table | 0x60 bytes, room for 24 vectors | 24 vectors of 4 bytes, `_VECTORS_SIZE` 96 | 21 vectors |
| SRAM | 0x0060–0x0fff | 0x0060–0x0fff | 0x0060–0x045f |
| Data space above SRAM | Somagic's hardware, from 0x1000 | external memory, up to 0xffff | none |
| `MUL`, `MOVW` | never used | not implemented | implemented |
| Timer0 clock select 5 and 7 | timing needs /128 and /1024 | /128 and /1024 | /1024 and external clock |
| UART at 0x09–0x0c | same registers and bits | `UBRR`, `UCR`, `USR`, `UDR` | `UBRRL`, `UCSRB`, `UCSRA`, `UDR` |
| Port C | `DDRC` and `PINC` used | output only, no `DDRC` and `PINC` | bidirectional |

So the vendor most likely built its firmware for an ATmega103 and Somagic's
core is compatible with it, apart from a bidirectional port C. Somagic's
other hardware sits where the ATmega103 has its external memory, although
the firmware never enables an external memory interface. An ATmega128 in
its ATmega103 compatibility mode comes closest to the whole of it, but
that mode keeps the ATmega128's instructions and vectors, which the
firmware gives no sign of.

Both images were built with the ImageCraft ICCAVR C compiler, recognizable
by its startup code and runtime helpers: a hardware stack for return
addresses at the top of SRAM, a separate software stack in `Y` below it,
guarded by a 0xaa byte, and helper functions that save and restore
`r10`–`r23`. This only matters to someone who reads the original images.

Instruction set: the images use `JMP` and `CALL`, and `LPM` only in its
original form that loads `r0`. They contain no `MUL`, `MOVW`, `LPM Rd, Z`,
`ELPM`, `SPM`, `SLEEP` or `WDR`. That the compiler did without `MUL` and
`MOVW` fits the ATmega103, which doesn't have them. Until a test firmware
has shown otherwise (see [Experiments](#experiments)), a replacement should
be built for the classic core with `JMP` and `CALL`, `-mmcu=avr3` with
avr-gcc, using the register names of the ATmega103 plus `DDRC` and `PINC`.
`-mmcu=atmega103` would reproduce the original memory layout, but assumes
`ELPM`, which avr-libc's startup code then uses to copy initialized data;
whether the core has `ELPM` is not known.

### Program memory

The bootloader writes the uploaded image to program memory from address 0
and starts it there, see [Bootloader](protocol.md#bootloader). Program
memory holds at least the 0x1d4e bytes of `smi2021_3c.bin`; its actual size
is unknown. Where the bootloader itself lives is unknown too: it is in ROM
(inferred, as the device keeps no firmware), but whether that ROM is
visible in program memory or data space is not known.

The image starts with the interrupt vector table, one `JMP` of 4 bytes per
vector. Both images leave 0x0000–0x005f to the table, which is room for
the 24 vectors of an ATmega103. Only two are used:

| Vector | Address | Use |
|--------|---------|-----|
| 0 | 0x0000 | reset |
| 11 | 0x002c | [USB device controller](#interrupt) |

On an ATmega103, vector 11 is the input capture interrupt of Timer1
(`TIMER1_CAPT`). Whether Somagic replaced that interrupt with the USB
controller's, or numbers the interrupts differently, is unknown. All other
entries are 0xffff.

After the vector table, both images place the initial values of their
SRAM variables, from 0x0060 to 0x019b (`3c`) or 0x0197 (`3f`), which the
startup code copies to the same addresses in SRAM. Code follows.

### Data space

The data space is 16 bits wide and holds the AVR registers, the SRAM, and
the hardware specific to the device above 0x1000, where an ATmega103 has
its external memory. The firmware sets its
stack pointer to 0x0fff, so SRAM reaches at least there. Which parts of
0x1000–0xffff not listed in this document contain anything is unknown.

The host can read and write the whole data space with
[register access requests](protocol.md#bridge-registers), which makes it
possible to examine most of the hardware with the original firmware.

### Clock

The firmware never configures a clock, a PLL, the USB PHY or the
watchdog. Either their power-up state is what the firmware needs, or the
bootloader has already set them up.

Somagic specifies its sibling chip, the SMI-2020CBE, with "30 MHz for
MCU Core" (reported, see [Related chips](#related-chips)). The timing of
the infrared decoder agrees: with the prescaler of the ATmega103's Timer0,
it fits the NEC remote protocol for CPU clocks between about 24 and 37 MHz
(inferred, see [Timer/Counter0](#timercounter0)). This document assumes
30 MHz where it converts cycles to time.

## Standard AVR peripherals

| Register | Address (I/O address) | Use |
|----------|-----------------------|-----|
| `PINC` | 0x33 (0x13) | `PC5`: infrared receiver input (`3c`) |
| `DDRC` | 0x34 (0x14) | see [GPIO](#gpio) |
| `PORTC` | 0x35 (0x15) | see [GPIO](#gpio) |
| `DDRA` | 0x3a (0x1a) | set to 0xff at start |
| `PORTA` | 0x3b (0x1b) | written by the host only |
| `TCNT0` | 0x52 (0x32) | infrared decoder |
| `TCCR0` | 0x53 (0x33) | infrared decoder |
| `SPL`, `SPH` | 0x5d, 0x5e (0x3d, 0x3e) | stack pointer |
| `SREG` | 0x5f (0x3f) | status register, saved by the interrupt handler |
| `UBRR`, `UCR`, `USR`, `UDR` | 0x29–0x2c (0x09–0x0c) | unused code only |

### GPIO

The firmware uses `SBI`, `CBI`, `SBIS` and `SBIC` on ports A and C, at the
addresses of an ATmega103. Unlike on an ATmega103, port C has a direction
register `DDRC` and an input register `PINC`, like port A, and the firmware
uses both. Ports B, D, E and F of the ATmega103 are never touched; whether
they exist is unknown. What the
pins are connected to is in [GPIO and board
signals](protocol.md#gpio-and-board-signals).

### Timer/Counter0

The infrared decoder of `smi2021_3c.bin` is the only user. It measures
time by clearing `TCNT0`, writing a clock select value to `TCCR0` and
waiting until `TCNT0` reaches a limit, then writes 0 to `TCCR0` to stop the
timer:

| `TCCR0` | Limit | Use |
|---------|-------|-----|
| 0x05 | 11 | sampling period of the infrared input; 150 of them must fit into the 9 ms leading pulse of a NEC code, and 60 of them have to tell the 4.5 ms pause of a code from the 2.25 ms pause of a repeat code |
| 0x07 | 68 | a pause; 30 in a row follow each received code |

On an ATmega103, Timer0 is the timer that can also run from a watch
crystal, and its prescaler offers /1, /8, /32, /64, /128, /256 and /1024
for clock select 1 to 7. So 5 divides by 128 and 7 by 1024. The firmware
never touches `ASSR`, so the timer runs from the CPU clock. Then a sampling
period of 11 counts is about 47 µs at 30 MHz, and the timing above holds
for CPU clocks between about 24 and 37 MHz (inferred). On an ATmega16,
whose Timer0 divides by 1024 for clock select 5 and counts edges on a pin
for 7, the timing would need a CPU clock of over 180 MHz.

### UART

Both images contain functions that set `UBRR`, enable the receiver and
transmitter in `UCR`, and send or receive a byte by polling `UDRE` and
`RXC` in `USR`, all at the ATmega103 addresses and bit positions. Nothing
calls them, so they are no evidence that the chip has a UART.

### SPI

Somagic specifies an "SPI-compatible controller, running at 1 MHz" for the
SMI-2020CBE (reported). The firmware has no code for it. If it is the
ATmega103's SPI, its registers `SPCR`, `SPSR` and `SPDR` are at I/O
addresses 0x0d to 0x0f, data space 0x2d to 0x2f (inferred); see
[Experiments](#experiments).

## Startup

The original firmware brings up the device in this order. Values written
to registers specific to the device are given exactly, because their
meaning is not fully known; a replacement should start from the same
sequence.

1. Set the stack pointer, clear `.bss` and copy the initial values of
   variables to SRAM.
2. `DDRA` = 0xff.
3. Write 0 to the status registers of both i2c engines, 0x1782 and 0x3182.
4. Prepare mode 3, the initial mode: 0x1580 = 0x03, 0x1880 = 0x00. See
   [Streaming engines](#streaming-engines).
5. Set up the USB controller:
   1. 0x1507 = 0xc4, 0x1511 = 0x03.
   2. Clear bits 2–1 of 0x1510 (control endpoint not stalled).
   3. 0x1513 = 0x40, 0x1515 = 0x40.
   4. 0x1519–0x151a = 0x2000 and 0x151d–0x151e = 0x2040, little endian:
      the control endpoint's buffers.
   5. Endpoint block A: 0x1521 = 0x15, clear bits 2–1 of 0x1520, maximum
      packet size 0x03ff in 0x1523–0x1524.
   6. Endpoint block B: 0x1531 = 0x25, clear bits 2–1 of 0x1530, maximum
      packet size 0x03ff in 0x1533–0x1534.
   7. Make `PC0` an output and drive it low. `smi2021_3f.bin` also makes
      `PC2` an output.
   8. Wait for 200 units of the delay loop, set bit 3 of 0x1500, wait for
      2000 units, and clear bit 3 of 0x1500 again. This disconnects the
      device from the bus for a moment, so that the host enumerates it
      again with the firmware's descriptors (inferred). A unit of the
      delay loop takes about 160 CPU cycles, so at 30 MHz the device
      disconnects after about 1.1 ms, for about 11 ms (inferred).
6. Enable interrupts, and loop forever, handling the USB events that the
   interrupt handler records, see [Interrupt](#interrupt).

The firmware never writes the device address register on its own, see
[Device state](#device-state).

## USB device controller

The USB controller occupies 0x1500–0x157f. It is a USB 2.0 high speed
device controller with a control endpoint and at least three more
endpoint register blocks. Isochronous data comes straight from the
streaming engines; the firmware never touches it.

### Registers

| Address | Use |
|---------|-----|
| 0x1500 | bit 3: disconnect from the bus while set (inferred); bit 2: set on suspend |
| 0x1501 | bit 1: set when the device runs at high speed |
| 0x1502 | bit 4: a setup packet was received; the firmware clears it in the interrupt handler |
| 0x1503 | event code, see [Interrupt](#interrupt) |
| 0x1504 | device address |
| 0x1507 | written 0xc4 at start (meaning unknown; maybe the enable mask of the interrupt) |
| 0x1509 | test mode selector (inferred, see [Device state](#device-state)) |
| 0x1510–0x151e | control endpoint, see [Control endpoint](#control-endpoint) |
| 0x1520–0x152f | endpoint block A |
| 0x1530–0x153f | endpoint block B |
| 0x1540–0x154f | endpoint block C, only used by `smi2021_3c.bin` |

The firmware never reads 0x1503 more than once per interrupt, and never
acknowledges an event by writing to it, so reading it presumably
acknowledges the event (inferred).

### Interrupt

The controller raises vector 11. The handler reads the event code from
0x1503, records the event for the main loop and returns:

| Code | Event | What the firmware does |
|------|-------|-------------------------|
| 0x01, with 0x1502 bit 4 set | setup packet | clears 0x1502 bit 4, withdraws a pending packet of the control endpoint by clearing 0x151c bit 7, and has the main loop handle the request |
| 0x80 | speed detected | sets the maximum packet size of endpoint blocks A and B to 0x0400 if 0x1501 bit 1 is set, otherwise to 0x03ff |
| 0x81 | bus reset | resets its own state only: device state default, control endpoint idle, endpoints halted in its bookkeeping |
| 0x85 | suspend | drives `PC0` low (and makes `PC2` an output in `3f`), sets bit 4 of 0x3080 and bit 2 of 0x1500 |

Code 0x01 without bit 4 of 0x1502 is ignored, as are all other codes.
Nothing handles a resume, and nothing ever clears the bits set on suspend,
so presumably the hardware clears them by itself on resume (inferred).

### Control endpoint

| Address | Use |
|---------|-----|
| 0x1510 | bits 2–1: 00 for normal operation, 10 to stall |
| 0x1511 | written 0x03 at start |
| 0x1513, 0x1515 | written 0x40 at start (inferred: the maximum packet size of each direction) |
| 0x1518 | bit 7: the receive buffer holds a packet; the firmware clears it to free the buffer |
| 0x1519–0x151a | address of the receive buffer, little endian, 0x2000 |
| 0x151b | number of bytes to send |
| 0x151c | bit 7: set by the firmware to send; cleared by the hardware once the packet is sent (inferred), or by the firmware to withdraw it |
| 0x151d–0x151e | address of the data to send, little endian, 0x2040 by default |

The buffers are in a separate memory at 0x2000 (inferred: USB buffer
memory). The firmware handles a control transfer as follows:

- **Setup stage**: when the interrupt reports a setup packet and 0x1518
  bit 7 is set, the eight bytes of the setup packet are at 0x2000. The
  firmware copies them and clears bit 7.
- **OUT data stage**: for each packet, the firmware waits for 0x1518
  bit 7, copies the data from 0x2000 and clears bit 7. It never reads a
  length register; it relies on each packet holding 64 bytes, or what
  remains of `wLength`.
- **IN data stage**: for each packet, the firmware waits for 0x151c bit 7
  to clear, puts up to 64 bytes at 0x2040, writes the length to 0x151b and
  sets bit 7. To send a descriptor that it assembled at 0x2040 and up in
  one go, it instead moves 0x151d–0x151e forward by 64 for each packet.
  It also restores 0x151d–0x151e to 0x2040 for every request.
- **Status stage**: after an OUT data stage, or for a request without one,
  the firmware sends a packet of zero bytes. It does nothing for the
  status stage of an IN transfer, so the hardware completes that by
  itself (inferred).
- **Stall**: the firmware sets bits 2–1 of 0x1510 to 10 and never clears
  them again, so the hardware returns to normal operation on the next
  setup packet (inferred).

### Device state

The firmware tracks the USB device state itself (default, address,
configured) and implements the standard requests as listed in
[Standard requests](protocol.md#standard-requests). Points that concern
the hardware:

- **`SET_ADDRESS`**: the firmware sends the status stage first, waits for
  0x151c bit 7 to clear, and only then writes the new address to 0x1504.
  The controller does not delay the change of address by itself. The
  firmware never resets 0x1504 on a bus reset, so the hardware does
  (inferred).
- **`SET_FEATURE`**: the firmware has code to write a test mode selector to
  0x1509 after the status stage, as `TEST_MODE` requires, but never gets to
  run it: it rejects every `SET_FEATURE` for the device. So 0x1509 is
  presumably the test mode register (inferred).
- **`GET_DESCRIPTOR`**: 0x1501 bit 1 selects between the high speed and the
  full speed configuration, and for `OTHER_SPEED_CONFIGURATION`, the other
  way round.

### Endpoint register blocks

Blocks A, B and C share one layout. Relative to its base:

| Offset | Use |
|--------|-----|
| +0 | bits 2–1: state, see below; bit 0: cleared, after bits 2–1, when the host clears the halt feature (inferred: resets the data toggle) |
| +1 | 0x15 or 0x25; see below |
| +3, +4 | maximum packet size, little endian |
| +8, +12 | only for block B: cleared on every mode request |

The firmware names the blocks after endpoints in its handling of the halt
feature and of `GET_STATUS`: A is `0x81`, B is `0x82` and C is `0x83`. The
descriptors only have `0x82`.

The mode request (see [Modes](protocol.md#modes)) writes 0x25 to +1 of one
block and sets its state to 00, and writes 0x15 to +1 of the other one and
sets its state to 01:

| Mode | Block with 0x25 and state 00 | Block with 0x15 and state 01 |
|------|-----------------------------|------------------------------|
| 3, 5 | B | A |
| 2, 4 | A | B |

The most consistent reading of this is (inferred):

- Bits 7–4 of +1 are the endpoint number, and the low bits the type and
  direction (isochronous IN). So 0x25 is endpoint `0x82` and 0x15 is
  endpoint `0x81`.
- State 00 is normal operation, 01 disables the endpoint, and 10 stalls
  it. This is how the firmware treats the control endpoint, where 00 is
  normal operation and 10 stalls, and clearing the halt feature of an
  endpoint sets its state to 00.
- Each block takes its data from fixed streaming engines: block B from
  those of modes 3 and 5, block A from those of modes 2 and 4. The mode
  request makes the right block endpoint `0x82` and moves the other one
  out of the way as a disabled endpoint `0x81`.

The opposite reading, with 01 as the enabled state, needs 0x15 to stand
for endpoint `0x82`, and leaves unexplained why the firmware calls block A
`0x81` while it is the one in use in modes 3 and 5. Either way, the
experiment in [Experiments](#experiments) is needed to be sure.

Other accesses:

- `SET_CONFIGURATION` with a non-zero value sets the state of all blocks to
  00. The start of the firmware does the same for blocks A and B.
- Setting the halt feature sets the state to 10, clearing it sets the state
  to 00 and then clears bit 0.
- On the speed event, the maximum packet size of blocks A and B becomes
  0x0400 at high speed and 0x03ff at full speed. How the controller learns
  that alternate setting 2 sends three packets per microframe is unknown;
  the firmware writes nothing for it.

Block C is endpoint `0x83` in `smi2021_3c.bin` only. The initial values of
that image also contain unused pointers to 0x1547, 0x1548 and 0x1549, so
block C presumably has registers there.

## I2C engines

The two engines are at 0x1780 and 0x3180 and are identical:

| Offset | Use |
|--------|-----|
| +0 | command; writing it starts the transfer |
| +1 | number of data bytes |
| +2 | status: bit 4 busy, bit 6 or 7 set when the transfer has ended, bit 5 set if it failed |
| +3 | subaddress |
| +4 | slave address, 8 bit |
| +5… | data buffer |

To run a transfer, the firmware

1. if the status shows bit 4, waits until bit 6 or 7 is set and writes 0 to
   the status,
2. for a write, puts the data bytes into the buffer,
3. writes the slave address, the subaddress, the number of bytes and,
   last, the command,
4. waits while bit 4 is set and neither bit 6 nor bit 7 is,
5. checks bit 5 for a failure and writes 0 to the status,
6. for a read, takes the data bytes from the buffer.

It retries a failed transfer up to 16 times by repeating steps 1 and 3 to
5, without writing the buffer again. So the buffer is memory that keeps
its contents, not a FIFO (inferred). Step 4 stops right away unless bit 4
is set, so the engine must show bit 4 as soon as the command is written
(inferred). How large the buffer is, is unknown; the host can ask for
transfers of up to 255 bytes, and replies hold up to 32 bytes of data.

The meaning of the command bits, as far as it is known, is in [I2C
engines](protocol.md#i2c-engines). The bus clock is never configured. The
firmware only uses the engines on the host's request; which devices are on
the second bus is unknown.

## Streaming engines

What the mode request and `SET_INTERFACE` write is listed in
[Modes](protocol.md#modes). Besides that:

- Starting the engines, on `SET_INTERFACE` to a non-zero alternate setting,
  also sets bit 7 of 0x1600.
- `smi2021_3c.bin` contains a function to stop the engines that nothing
  calls; `smi2021_3f.bin` doesn't contain it. It shows how the vendor
  meant to stop them, but has never been tested:
  - in mode 2, it clears bits 0 and 2 of 0x1740;
  - in modes 3, 4 and 5, it sets bit 2 of 0x3080, waits for 20 units of
    the delay loop and clears the bit again (inferred: a reset of the video
    path);
  - in all modes, it then clears bit 7 of 0x1600.

What the engines of modes 3 and 4 serve is unknown. Somagic's
SMI-2020CBE has a transport stream input and a 1-bit stream input for a
GPS receiver, see [Related chips](#related-chips), so these are
candidates (inferred).

### System control register 0x3080

| Bit | Use |
|-----|-----|
| 7 | cleared at the host's request (`wValue` 0xfb); never set by the firmware |
| 4 | set on suspend |
| 2 | pulsed by the unused stop function |

## Infrared receiver

Only `smi2021_3c.bin` decodes infrared codes. While bits 1–0 of 0x1600
are `01`, its main loop checks `PC5`, and when it finds the pin low,
decodes a NEC code with busy waiting, timed with
[Timer/Counter0](#timercounter0). This blocks the handling of USB
requests for the duration of a code, up to about 85 ms, and for a pause
after it, about 70 ms at 30 MHz. Writing 0x81 to 0x1600 makes `PC5` an
input; in `smi2021_3f.bin` it makes `PC4` an input instead. The interface
to the host is in [IR receiver](protocol.md#ir-receiver).

## Related chips

Somagic, Inc. described more chips of the same family on its
web site, now only in the Internet Archive (reported, see
[Sources](#sources)); none of the pages names the SMI2021:

| Chip | Interface | Use |
|------|-----------|-----|
| SMI-2020CBE | USB 2.0 | digital TV and GPS dongles: transport stream input for DMB-TH, DVB-T, ATSC and ISDB-T demodulators, 1-bit stream input from a GPS receiver |
| SMI-2022CBG | USB 2.0 | ATSC and analog NTSC dongle with an Auvitek AU8522 demodulator, which decodes analog TV into BT.656 video and I2S audio |
| SMI-2030 | PCIe 1.1 | digital and analog TV and GPS: transport stream, BT.656 video, I2S audio, 1-bit GPS data |

Their published features describe what is likely the same design as the
SMI2021 (inferred):

- "30 MHz for MCU Core";
- USB 2.0 high speed with control, interrupt and isochronous pipes;
- an "I2C-compatible controller, running at 100 kHz";
- an "SPI-compatible controller, running at 1 MHz";
- MPEG-2 transport stream input with up to 34 PID filters;
- reception of BT.656 ancillary data (closed captions, WSS, teletext);
- an infrared receiver for RC-5, RC-6, NEC, Sony SIRC and Microsoft MCE
  remotes. The SMI2021 firmware decodes NEC in software instead, see
  [Infrared receiver](#infrared-receiver).

## Comparison with known controllers

The register layouts of the I2C, SMBus and USB device controllers that
have drivers in Linux, including drivers removed from it, U-Boot and Zephyr
were compared with the blocks described here: first by searching for
register names at matching offsets, then by reading the candidates.

### I2C engines

No controller has the same layout. The closest relatives share single
traits:

| Controller | Driver | Similar | Different |
|------------|--------|---------|-----------|
| SMBus host controllers of PC chipsets: Intel PIIX4 and ICH, VIA | `i2c-piix4`, `i2c-i801`, `i2c-viapro` | the hardware runs a whole transfer; the SMBus command code, which is the subaddress, the slave address and the first data byte are at offsets 3, 4 and 5, as here | status at offset 0 and control at 2, where this engine has the command at 0 and the status at 2; no bit has the same meaning |
| SMBus of ITE embedded controllers | Zephyr `i2c_ite_it51xxx.c` | modelled on the PC chipset design; bit 5 of the status reports a missing acknowledge, as here | status at 0, control at 1, slave address at 3, data at 4; the other status bits |
| Auvitek AU0828 USB bridge | `au0828` | writing the trigger register starts the transfer, and its bit 5 selects a read; bit 4 of the status means busy, as here | separate status bits for each direction, FIFOs instead of a buffer, bit 6 of the trigger keeps the transfer open for more bytes, and a different layout |
| Sunplus SPCA561 webcam bridge | `gspca/spca561.c` | slave address at offset 4 and data at offset 5 | subaddress at 1, command at 2, status at 3 |

The other USB video bridges with an i2c master behind registers (Empia,
Syntek, Vimicro, Z-Star ZC3xx, Sonix, OmniVision OV51x, Etoms, STV06xx,
TP6800, Nogatech NT100x and the AU8522 demodulator) have nothing in
common with it beyond the general idea.

### USB device controller

No controller has the same layout or looks derived from it. The traits
that set this one apart, an event code instead of a bitmask of interrupt
flags, the device address at offset 4, and endpoint blocks of 16 bytes with
buffer address registers and a send bit, appear in none of the drivers.
Mentor MUSB, NetChip NET2272, Maxim MAX3420, Faraday FOTG210 and FUSB300,
Cadence USBHS, Renesas M66592 and R8A66597, NXP ISP1761, ITE IT82xx2 and
Aspeed were looked at most closely, as were the USB controllers of the
microcontrollers supported by Zephyr. The USB
controllers of WCH were not part of the comparison. Some of them share
single ideas, such as registers holding the start address of an
endpoint's buffer in on-chip memory (Cadence USBHS, Nuvoton) or an EP0
with a buffer address, a length and a ready bit (Aspeed).

### Conclusion

Apart from the ATmega103 core and its peripherals, the bridge is Somagic's
own design, or licensed from a vendor whose controllers have no open
source driver. A replacement firmware can reuse AVR libraries for the
core, but needs its own code for everything above 0x1000.

## Writing a replacement

A replacement has to implement the host interface of
[`protocol.md`](protocol.md), with the register writes listed there and in
this document for the engines. These are the pieces of the original
firmware's behaviour a replacement does not need to copy, as no host
depends on them, and some of which are defects:

- An OUT vendor request without a data stage makes the original firmware
  wait forever for a data packet; `wLength` is not checked against the
  size of its buffer either.
- An IN data stage that ends on a full packet before `wLength` is reached
  ends without a packet of zero bytes.
- Selecting alternate setting 0 stops nothing.
- The infrared decoder blocks everything else while it receives a code.
- `smi2021_3f.bin`'s IN request with `bRequest` 0, see
  [Differences between the images](protocol.md#differences-between-the-images).

The host has no way to identify the firmware other than its USB id and
serial number string, so a replacement can take the USB id of the image it
replaces, or a new one if the driver learns it.

## Experiments

These are the open questions, with the tests on hardware that should settle
them. The first group works with the original firmware, through register
access requests from the host; the second one needs a test firmware
uploaded through the bootloader. The first group is implemented by
[`tools/bridge-probe`](../tools/bridge-probe/README.md). Reading a register
can have side effects, for instance 0x1503 presumably acknowledges an
event, so the registers of the USB controller should be left out of blind
surveys.

With the original firmware:

1. **CPU clock and Timer0 prescaler**: write each clock select value to
   `TCCR0` and sample `TCNT0` repeatedly, timing the samples on the host.
   The rate for each value gives the prescaler table and the CPU clock,
   expected to be those of an ATmega103 and 30 MHz.
2. **Data space survey**: read 0x1000–0xffff in blocks and note what reads
   back as something other than a constant, what keeps a written value,
   and what mirrors other addresses. Look for a copy of the firmware image,
   which would show that program memory is visible in data space, and for
   the bootloader.
3. **Other AVR peripherals**: write and read back I/O registers of the
   ATmega103 that the firmware doesn't use, for example `OCR0`, `OCR2`,
   `UBRR`, `SPCR`, `ADMUX` and `RAMPZ`, to find which exist; for ports B,
   D, E and F, only read the input registers and leave the direction
   registers alone. `MCUCR` (external memory and sleep), `MCUSR`,
   `WDTCR` and `XDIV` (clock divider) should not be written.
4. **I2C buffer**: write a pattern to 0x1785 and up and read it back to
   find the size of the buffer; scan the second bus for devices.
5. **Endpoint blocks**: in mode 5, swap the values of +1 or the states of
   blocks A and B, and see which block's data arrives at endpoint `0x82`.
6. **Modes 3 and 4**: stream in each mode and look at what arrives.
7. **GPIO**: find what `PA7`, `PC0`, `PC1`, `PC2` and `PC4` are connected
   to.

With a test firmware:

8. Check whether the core executes `MUL`, `MOVW`, `LPM Rd, Z`, `ELPM` and
   `SPM`.
9. Read program memory beyond the uploaded image to find its size and the
   bootloader.
10. Install handlers on all vectors to find which interrupts exist, and
    check whether the USB interrupt is really vector 11 only.
11. Measure the CPU clock with a loop of known length against the USB
    frame timing or a GPIO toggled into a scope.

## Sources

- Atmel ATmega103 and ATmega16 datasheets, and the device headers
  `iom103.h` and `iom16.h` of avr-libc.
- Somagic's product pages, as kept by the Internet Archive:
  <http://www.somagic.com.cn/product_info.asp?id=60> (SMI-2020CBE),
  <http://www.somagic.com.cn/product_info.asp?id=61> (SMI-2030) and
  <http://www.somagic.com.cn/product_info.asp?id=51> (SMI-2022CBG reference
  design), from 2009 to 2016.
- The drivers in Linux (as of October 2026, and removed ones from its
  history), U-Boot and Zephyr named in [Comparison with known
  controllers](#comparison-with-known-controllers).
