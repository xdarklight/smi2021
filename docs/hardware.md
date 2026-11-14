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
for other controllers in Linux, U-Boot and Zephyr, with Somagic's own
descriptions of its other chips, and with the vendor's Windows drivers, see
[Sources](#sources). It contains no
code from the firmware. Statements use the same markers as
[`protocol.md`](protocol.md#about-this-document): unmarked statements are
**observed** in the firmware, or on hardware where they name the device,
**inferred** ones are interpretations that have not been checked on
hardware, and **reported** ones come from others' experiments. Where a
statement needs a test on hardware, the test is listed in
[Experiments](#experiments).

Addresses are data space addresses unless noted otherwise. Program memory
addresses are byte addresses, as in `avr-objdump` output.

## Summary of blocks

| Block | Address | Standard? |
|-------|---------|-----------|
| CPU core | | AVR instruction set; see [CPU core](#cpu-core) |
| General purpose registers, `SREG`, stack pointer | 0x0000–0x001f, 0x005d–0x005f | standard AVR |
| Port A, port C | 0x0033–0x0035, 0x003a–0x003b | ATmega103 layout, but port C can also be an input |
| Timer/Counter0 | 0x0052–0x0053 | ATmega103, prescaler measured |
| Timer/Counter2 | 0x0043–0x0045 | ATmega103 registers present, unused |
| UART | 0x0029–0x002c | ATmega103, registers present, only in unused code |
| SPI | | not at the ATmega103 addresses, see [SPI](#spi) |
| `RAMPZ` | 0x005b | ATmega103 register present, unused |
| SRAM | 0x0060–0x0fff | ATmega103 |
| USB 2.0 device controller | 0x1500–0x157f | Somagic's own |
| Transport stream input with PID filters, mode 3 (inferred) | 0x1580–0x15a1 | Somagic's own |
| Stream and IR control | 0x1600 | Somagic's own |
| Audio engine | 0x1740 | Somagic's own |
| First i2c engine | 0x1780–0x17a9, repeated every 0x40 | Somagic's own, **not** Atmel's TWI |
| Video engine | 0x1800 | Somagic's own |
| Stream source | 0x1880 | Somagic's own |
| USB buffer memory | 0x2000– | Somagic's own |
| System control | 0x3080 | Somagic's own |
| Engine of mode 4 | 0x3100 | Somagic's own |
| Second i2c engine | 0x3180–0x31a9 | same as the first one |
| Board identification | 0x31a5, and 0x17a5 likewise | Somagic's own, see [Board identification](protocol.md#board-identification) |

The processor and its peripherals are those of an Atmel ATmega103. None
of the hardware specific to the device could be matched to a controller
with a driver in Linux, U-Boot or Zephyr, see [Comparison with known
controllers](#comparison-with-known-controllers).

The firmware uses none of the other ATmega103 peripherals: SPI, ADC,
analog comparator, EEPROM, watchdog, Timer/Counter1 and 2, external
interrupts and the external memory interface. `SLEEP` and `SPM` never
appear either. Writing their registers from the host shows which of them
exist (observed, on an EasyCAP 002): `OCR0`, `OCR2`, `TCNT2`, `UBRR` and
`RAMPZ` keep a written value; `OCR1AL`, `OCR1BL`, `TCNT1L`, `ADMUX`,
`SPCR`, `EEARL` and `EEDR` read back 0. So Timer/Counter2 and the UART are
there, and `RAMPZ` makes `ELPM` likely, while Timer/Counter1, the ADC, the
EEPROM interface and an SPI at the ATmega103's addresses are presumably
absent.

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
`MOVW` fits the ATmega103, which doesn't have them.

A test firmware ran each of these instructions once on an EasyCAP 002
(observed, see [Experiments](#experiments)). The core decodes the
instruction set of the ATmega103 and nothing more:

| Instruction | Result |
|-------------|--------|
| `MUL` | does nothing: registers and flags unchanged |
| `MOVW` | does nothing |
| `LPM Rd, Z`, `LPM Rd, Z+` | do nothing |
| `ELPM` (into `r0`) | works |
| `ELPM Rd, Z`, `ELPM Rd, Z+` | do nothing |

None of them crashed the core; an instruction it lacks acts as a `NOP`.
`SPM` was not tried. `RAMPZ` exists, but as program memory repeats every
8 KB (see [Program memory](#program-memory)), `ELPM` reads the same bytes
whatever it holds. A replacement can therefore be built for the
ATmega103, `-mmcu=atmega103` with avr-gcc and avr-libc, whose startup code
copies initialized data with `ELPM`, using the register names of the
ATmega103 plus `DDRC` and `PINC`; or for the classic core with `JMP` and
`CALL`, `-mmcu=avr3`.

### Program memory

The bootloader writes the uploaded image to program memory from address 0
and starts it there, see [Bootloader](protocol.md#bootloader). Program
memory is 8 KB (observed, on an EasyCAP 002): read with `LPM` from 0 to
0xffff, it repeats every 0x2000 bytes, the uploaded image at 0 and at every
repeat. After a power-up, the part beyond the uploaded image holds what
looks like random data, nothing of an image uploaded before the power
cycle, so program memory is presumably RAM that the bootloader fills on
every upload (inferred). An image can be up to 8192 bytes; the image of
2013, with 7812 bytes (see [Differences between the
images](protocol.md#differences-between-the-images)), fits. The bootloader
appears nowhere in program memory, so it runs from a ROM of its own that
the CPU doesn't see there once the image runs (inferred); whether it is
visible in the data space is not known, and searching it costs a power
cycle per gap, see [Data space](#data-space).

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

A test firmware with a counting handler on each of the 48 words of
0x0000–0x005f, so that it would catch vectors 2 bytes apart as well as 4,
saw only 0x002c taken, about once per USB event during an enumeration
(observed, on an EasyCAP 002). Whether the other vectors exist is not known:
their sources may just never have been enabled.

Interrupts are taken as on an AVR, but the core has two faults that a
firmware built with avr-gcc runs into, see [Interrupt
faults](#interrupt-faults).

After the vector table, both images place the initial values of their
SRAM variables, from 0x0060 to 0x019b (`3c`) or 0x0197 (`3f`), which the
startup code copies to the same addresses in SRAM. Code follows.

### Interrupt faults

The core takes vector 11 the way an AVR takes an interrupt (observed, on an
EasyCAP 002, with test builds of [OpenSMI21](../fw/README.md)): it clears
I on entry, handlers never nest, and an interrupt that is pending when
`SEI`, `RETI` or an `OUT` to SREG sets I is taken after the instruction
that follows. The USB controller's request outlasts a short handler:
interrupts come in bursts of two or three, with one instruction of the
interrupted program between them.

With interrupts enabled, the test firmware first failed with every handler
tried on vector 11, while the same firmware polling the controller worked
(observed):

| Handler on vector 11 | Result |
|----------------------|--------|
| the event handling of [Interrupt](#interrupt), in C | through an enumeration at best, then fails within a few control transfers |
| reads 0x1503, 0x1502, 0x151c and 0x1518 and counts, in C or in assembly | as above |
| a bare `RETI`; counts in SRAM only; reads 0x1503 only; reads 0x1502 only | does not get through an enumeration |
| reads 0x1503 and 0x1502, with its registers saved to fixed SRAM instead of the stack | does not get through an enumeration |
| what the original handler does, also without the stack: on a setup packet, withdraws the pending packet, flags it and clears 0x1502 bit 4 | does not get through an enumeration |

Before failing, the firmware was found running code it can only have
jumped to, such as a census handler of another vector with I set. Two
faults of the core explain these failures. With both worked around, the
same firmware runs with interrupts enabled (observed, see below).

**An interrupt that starts on a `PUSH` loses its return address.** The
core moves SP down by two, as for any interrupt, but writes neither byte
of the return address, and the handler's `RETI` jumps to whatever the two
bytes held before (observed). The test: a window of the main loop enables
interrupts with a `SEI` directly in front of one instruction, while an
interrupt is pending, so that the interrupt is taken on exactly that
instruction; the stack below is filled with 0xff first. On a `PUSH`, the
handler found 0xffff instead of a return address 22 times out of 22. The
same test with each of these instructions found the right return address,
the right SP afterwards and the handler entered every time, 10 to 14
interrupts each:

| Instructions an interrupt may start on |
|----------------------------------------|
| `NOP`, `POP`, `RCALL`, `CALL` (its own return address undamaged), `RET` (as `SEI; RET`), `RJMP`, `JMP`, `BREQ` taken, `SBRS` skipping one and two words |
| `LDS`/`STS` of SRAM, `LDS` of 0x1503, `STS` into the i2c buffer at 0x17a0, `LD`/`ST` through X, `LDD`/`STD` through Y, `LD` of 0x1503 through Z |
| `LPM`, `ELPM`, `ADIW`, `SBIW`, `SBI`/`CBI` on an I/O register |

`CLI` and an `OUT` to SREG that clears I are never interrupted: `SEI`
directly followed by either, with an interrupt pending, took none in 250
tries each, so `CLI` protects the instruction after it (observed).

As the handler returns with an interrupt still requested, the one
instruction it runs before the next interrupt of a burst is where the
next one is taken. If that instruction is a `PUSH`, as in the prologue of
nearly every function avr-gcc writes, the return address is lost. That
happened in the earlier runs: interrupts that returned to the first `PUSH`
of a function were followed by one whose return address was stale.

**About one interrupt in 4000 enters the reset vector instead of vector
11.** It arrives there with I clear and the return address pushed, which
looks like a restart (observed). With every `PUSH` protected, the firmware
still restarted by itself, after some hundred to a few thousand control
transfers, with the C handler as well as with an assembly handler that
only reads the controller's registers while the main loop polls, and with
requests of any one kind, the identity alone for example, which uses no
`LPM`. Each time SP was 0x0fe4 at the reset vector: the SP of the
main loop's function (0x0fe6) less the two bytes of a return address. A
check at the reset vector that treated such an entry as vector 11 found
46 of them in 191608 interrupts, each with I clear, SP 0x0fe4 and the
return address on the stack pointing right after the call that enabled
interrupts again in the main loop. Every one of them was the interrupt that
had waited while the main loop had interrupts disabled, and was taken right
after `SEI; RET`; it is not known why only that one. It may take a request
that waited that long, and ends right as it is taken (inferred).

**Cause (inferred).** The core appears to be the AVR Core of opencores.org,
see [CPU core](#cpu-core-1) under the comparison. In its 2006 sources, an
interrupt starts in the first cycle of any instruction that isn't marked
busy (`cpu_busy`), and `PUSH` and `POP` were only added to that list in
2005 ("cpu_busy(push/pop) + irq bug was fixed 14.07.05"). Without them in
the list, an interrupt that starts on a `PUSH` writes nothing: the RAM write
enable that the `PUSH` set is cleared at the end of the `PUSH`'s second
cycle, which is the cycle in which the interrupt asks for it, and the
interrupt's later cycles don't ask again. `POP` doesn't write and is
harmless, as observed. The vector address comes from a priority encoder
over the request lines, which gives 0x0000, the reset vector, when no line
is active. The 2006 sources latch it in the cycle the interrupt starts; a
version that takes it a cycle later enters the reset vector if the
request ends in that cycle. This was not checked against an older version
of the sources, which were not available.

**The original firmware** meets neither fault as far as its code shows: its
compiler keeps saved registers on a software stack, so the image has no
`PUSH` or `POP`; its handler too saves its registers there, with `ST -Y`.
It has one `SEI`, at the start, and no `CLI`: the main loop's call of the
handler returns with `RETI` like the interrupt does, so I stays set except
inside the handler, and the stack pointer is written only at reset. So an
interrupt there is taken as soon as it is requested and never waits long,
which may be why it doesn't meet the second fault (inferred); whether it
ever enters the reset vector from an interrupt is not known.

**For a replacement**, polling with interrupts disabled avoids both. One
that enables interrupts has to:

- never let an interrupt start on a `PUSH`: with avr-gcc, put every run of
  `PUSH`es (and `rcall .`, which it uses to make room on the stack)
  between `CLI` and an `OUT` that restores SREG, as OpenSMI21's
  `pushguard.awk` does to the compiler's assembly;
- take an entry at the reset vector with I clear, its own state intact
  (OpenSMI21 checks a magic number in SRAM that only it writes, and that
  SP is in its stack) and a return address into its program on top of the
  stack for an interrupt, and enter vector 11's handler, which returns
  there. The check must not push anything before it knows SP is its own:
  coming from the bootloader, SP points elsewhere.

With both, OpenSMI21 built with `IRQ=1` ran 25000 control requests of
five kinds, then 40000 more of four kinds in turn, without a restart
(observed), with 191608 interrupts taken.

### Data space

The data space is 16 bits wide and holds the AVR registers, the SRAM, and
the hardware specific to the device above 0x1000, where an ATmega103 has
its external memory. The firmware sets its
stack pointer to 0x0fff, so SRAM reaches at least there. Which parts of
0x1000–0xffff not listed in this document contain anything is unknown.

A read of an address that nothing answers stops the firmware until the
device loses power: from then on, it answers no control request (observed,
on an EasyCAP 002). The first read at 0x1000 did so, and so did a read of 30
bytes from 0x1600, while 0x1600 and 0x1605 on their own read fine, so not
even the blocks of known registers are decoded throughout; 0x1580–0x15bf
reads fine. A firmware, or a host, must only read addresses known to
exist. That fits a core that stretches each access to the data space
until the addressed block answers, as the AVR Core of opencores.org does
with its `cpuwait` input (inferred, see [CPU core](#cpu-core-1) under the
comparison). This rules out searching the data space for program memory or the
bootloader from the host.

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
(inferred, see [Timer/Counter0](#timercounter0)). Timer0, sampled from the
host, counts at 29.3 kHz with clock select 7 and 117 kHz with clock select
6, and, with the aliasing of a counter that wraps between samples
accounted for, at 232, 467 and 939 kHz with clock selects 5, 4 and 3: the
ATmega103's prescaler at a CPU clock of 30.0 MHz (observed, on an EasyCAP
002). A loop of a known number of cycles in a test firmware, timed by the
host, gives 30.000 MHz (observed). This document uses 30 MHz where it
converts cycles to time.

The EasyCAP 002 has two crystals, one marked 24.576 and one without a
marking; how they are connected was not traced (reported). 24.576 MHz is
the crystal frequency of the SAA7113H. 30 MHz is a sixteenth of the 480 MHz
of a high speed USB PHY, the usual clock of its 16 bit UTMI interface,
which would make the PHY the source of the CPU clock (inferred).

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
they exist is unknown, and their input registers read 0. On an EasyCAP 002
running `smi2021_3c.bin`, after the driver's initialization, `DDRC` is
0x11, `PORTC` 0x11 and `PINC` 0x15: `PC2`, an input, reads high
(observed). What the pins are connected to is in [GPIO and board
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
for 7, the timing would need a CPU clock of over 180 MHz. The measured
rates, see [Clock](#clock), fit the ATmega103's prescaler for all seven
clock select values, and not the ATmega16's (observed).

### UART

Both images contain functions that set `UBRR`, enable the receiver and
transmitter in `UCR`, and send or receive a byte by polling `UDRE` and
`RXC` in `USR`, all at the ATmega103 addresses and bit positions. Nothing
calls them, so they are no evidence that the chip has a UART. The registers
are there, though: `UBRR` keeps a written value, and `USR` reads 0x20,
`UDRE`, the transmit buffer empty, as on an ATmega103 at reset (observed).
Whether its pins come out of the chip is unknown.

### SPI

Somagic specifies an "SPI-compatible controller, running at 1 MHz" for the
SMI-2020CBE (reported). The firmware has no code for it. It is not the
ATmega103's SPI, or not at its addresses: `SPCR`, data space 0x2d, reads
back 0 after a write of 0x03 (observed).

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
6. Enable interrupts, and loop forever: call the event handler, then
   handle the USB events it recorded, see [Interrupt](#interrupt).

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
| 0x1502 | bit 4: a setup packet was received; the firmware clears it in the event handler |
| 0x1503 | event code, see [Interrupt](#interrupt) |
| 0x1504 | device address |
| 0x1507 | written 0xc4 at start (meaning unknown; maybe the enable mask of the interrupt) |
| 0x1509 | test mode selector (inferred, see [Device state](#device-state)) |
| 0x1510–0x151e | control endpoint, see [Control endpoint](#control-endpoint) |
| 0x1520–0x152f | endpoint block A |
| 0x1530–0x153f | endpoint block B |
| 0x1540–0x154f | endpoint block C, only used by `smi2021_3c.bin` |

Reading 0x1503 does not acknowledge an event: the event handler reads it
up to four times in a row, once for each code it compares, and the later
comparisons could never match if the first read had cleared it. Nothing
writes it either. So it presumably holds the code of the last event until
the next one (inferred); a setup packet is told apart from a stale code by
0x1502 bit 4, which the handler clears. Read twice in a row on a setup
packet, by a test firmware, 0x1503 and 0x1502 each gave the same value
both times (observed, on an EasyCAP 002).

### Interrupt

Vector 11 goes to the event handler, and the main loop also calls the
same handler at the start of every pass, so both images poll the
controller and work whether the interrupt fires or not. The controller
does raise vector 11, about once per USB event and in bursts (observed, see
[Interrupt faults](#interrupt-faults)). A replacement can poll as well,
which avoids the faults of the core described there. The handler reads the
event code from 0x1503, records the event for the main loop and returns:

| Code | Event | What the firmware does |
|------|-------|-------------------------|
| 0x01, with 0x1502 bit 4 set | setup packet | clears 0x1502 bit 4, withdraws a pending packet of the control endpoint by clearing 0x151c bit 7, and has the main loop handle the request; from the first register access request on (not the info query) until the next bus reset, the handler leaves the pending packet alone, and the main loop withdraws it instead, after it has copied the setup packet |
| 0x80 | speed detected | sets the maximum packet size of endpoint blocks A and B to 0x0400 if 0x1501 bit 1 is set, otherwise to 0x03ff |
| 0x81 | bus reset | resets its own state only: device state default, control endpoint idle, endpoints halted in its bookkeeping |
| 0x85 | suspend | drives `PC0` low (and makes `PC2` an output in `3f`), sets bit 4 of 0x3080 and bit 2 of 0x1500 |

Code 0x01 without bit 4 of 0x1502 is ignored, as are all other codes. As
the code stays, every pass of the main loop records the last bus reset,
speed detection or suspend again until another event arrives, and the
firmware handles it again; that is harmless for these three.
Nothing handles a resume, and nothing ever clears the bits set on suspend,
so presumably the hardware clears them by itself on resume (inferred).

### Control endpoint

| Address | Use |
|---------|-----|
| 0x1510 | bits 2–1: 00 for normal operation, 10 to stall |
| 0x1511 | written 0x03 at start |
| 0x1513, 0x1515 | written 0x40 at start (inferred: the maximum packet size of each direction) |
| 0x1517 | never read by the firmware; by the layout of the endpoint blocks, presumably the length of a received packet (inferred) |
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

Blocks A, B and C have the layout of the control endpoint. Relative to its
base:

| Offset | Use |
|--------|-----|
| +0 | bits 2–1: state, see below; bit 0: cleared, after bits 2–1, when the host clears the halt feature (inferred: resets the data toggle) |
| +1 | endpoint number, type and direction, see below |
| +3, +4 | maximum packet size, little endian |
| +7 | length of the packet in buffer 0 |
| +8 | bit 7: buffer 0 holds a packet |
| +9, +10 | address of buffer 0, little endian |
| +11 | length of the packet in buffer 1 |
| +12 | bit 7: buffer 1 holds a packet |
| +13, +14 | address of buffer 1, little endian |

The control endpoint uses buffer 0 to receive and buffer 1 to send, see
[Control endpoint](#control-endpoint). The image of 2013 (see [Differences
between the images](protocol.md#differences-between-the-images)) sets up
block C for its HID interrupt endpoint `0x83`: 0x34 in +1, 64 bytes as the
maximum packet size, bit 7 of +8 and +12 cleared, and buffers at 0x2080 and
0x20c0. It sends a report by writing the length to +7 or +11 and setting
bit 7 of +8 or +12, using the two buffers in turn, so for an IN endpoint
both buffers send (inferred: double buffering). It also sets bit 0 of +0
for that endpoint. The buffer memory reaches at least to 0x20ff.

The firmware names the blocks after endpoints in its handling of the halt
feature and of `GET_STATUS`: A is `0x81`, B is `0x82` and C is `0x83`. The
descriptors of `smi2021_3c.bin` and `smi2021_3f.bin` only have `0x82`.

The mode request (see [Modes](protocol.md#modes)) writes 0x25 to +1 of one
block and sets its state to 00, and writes 0x15 to +1 of the other one and
sets its state to 01:

| Mode | Block with 0x25 and state 00 | Block with 0x15 and state 01 |
|------|-----------------------------|------------------------------|
| 3, 5 | B | A |
| 2, 4 | A | B |

Bits 7–4 of +1 are the endpoint number: 0x34 for endpoint `0x83` in the
image of 2013 makes that all but certain, and it fits the names the
firmware gives the blocks. Bit 2 is presumably the direction, IN, and bits
1–0 the type, `01` for isochronous and `00` for interrupt (inferred). So
0x25 makes a block endpoint `0x82` and 0x15 endpoint `0x81`. The rest of
the most consistent reading is (inferred):

- State 00 is normal operation, 01 disables the endpoint, and 10 stalls
  it. This is how the firmware treats the control endpoint, where 00 is
  normal operation and 10 stalls, and clearing the halt feature of an
  endpoint sets its state to 00.
- Each block takes its data from fixed streaming engines: block B from
  those of modes 3 and 5, block A from those of modes 2 and 4. The mode
  request makes the right block endpoint `0x82` and moves the other one
  out of the way as a disabled endpoint `0x81`.

With 01 as the enabled state instead, the endpoint in use in modes 3 and 5
would be block A, at endpoint `0x81`, which the host never reads.

The experiment settles it in favour of the first reading (observed, twice,
on an EasyCAP 002 with `smi2021_3c.bin`). Changing the blocks after the
mode request of mode 5:

| A: +1, state | B: +1, state | Endpoint `0x82` |
|--------------|--------------|-----------------|
| 0x15, 01 (as set) | 0x25, 00 (as set) | the stream of mode 5 |
| 0x15, 00 | 0x25, 00 | the stream of mode 5 |
| 0x15, 00 | 0x25, 01 | nothing |
| 0x25, 01 | 0x15, 00 | nothing |
| 0x25, 00 | 0x15, 01 | nothing |

So state 00 enables, 01 disables, bits 7–4 of +1 are the endpoint number,
and the stream of mode 5 comes from block B: made endpoint `0x82` and
enabled, block A doesn't send it. "Nothing" means that the host saw no
response at all to its isochronous tokens, as opposed to packets without
data.

Other accesses:

- `SET_CONFIGURATION` with a non-zero value sets the state of all blocks to
  00. The start of the firmware does the same for blocks A and B.
- Setting the halt feature sets the state to 10, clearing it sets the state
  to 00 and then clears bit 0.
- On the speed event, the maximum packet size of blocks A and B becomes
  0x0400 at high speed and 0x03ff at full speed. How the controller learns
  that alternate setting 2 sends three packets per microframe is unknown;
  the firmware writes nothing for it.
- The mode request clears +8 and +12 of block B, which presumably drops
  packets left in its buffers (inferred).

Block C is endpoint `0x83` in `smi2021_3c.bin` only, where nothing sets it
up; the initial values of that image contain unused pointers to 0x1547,
0x1548 and 0x1549, the registers of its buffer 0.

## I2C engines

The two engines are at 0x1780 and 0x3180 and are identical:

| Offset | Use |
|--------|-----|
| +0 | command; writing it starts the transfer |
| +1 | number of data bytes |
| +2 | status: bit 4 busy, bit 6 or 7 set when the transfer has ended, bit 5 set if it failed |
| +3 | subaddress |
| +4 | slave address, 8 bit |
| +5–+0x24 | data buffer, 32 bytes |
| +0x25 | bits 7–6 fixed, presumably straps, 11 on an EasyCAP 002; bits 5–0 keep a written value; see [Board identification](protocol.md#board-identification) |
| +0x26–+0x29 | registers of unknown use; +0x26 and +0x28 keep a written value |

The block repeats every 0x40 bytes: 0x17c0 reads as 0x1780 (observed).
Each engine has its own +0x25 to +0x29: a write at 0x17a6 doesn't show at
0x31a6 (observed). After power-up, both engines read the same there with
`smi2021_3c.bin`: c8 32 d0 dd 06 at one time, c8 32 06 dd d0 at another,
in reads that both started at +0x25 (observed). So +0x27 and +0x29 trade
places, between power cycles or between reads; why is unknown. +0x25 of the
second engine reads 0xc8 with `smi2021_3f.bin` as well (observed).

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
(inferred). The buffer holds 32 bytes: the next byte is the register at
+0x25 (observed). The host can ask for transfers of up to 255 bytes, and
replies hold up to 32 bytes of data.

A command with bit 7 clear, 0x20 or 0x60 with a subaddress, froze the
first engine until the device lost power (observed): none of its
registers took a write any more, the command register kept 0x60, the
status read 0, and so every transfer the firmware ran on it ended at once,
without a failure, leaving the buffer as it was. A scan of the bus then
found a device at every address.

The meaning of the command bits, as far as it is known, is in [I2C
engines](protocol.md#i2c-engines). The bus clock is never configured. The
firmware only uses the engines on the host's request. On an EasyCAP 002,
the first bus has only the decoder at 0x25 (7 bit), and nothing answers on
the second one (observed).

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

The engine of mode 3 is presumably a transport stream input with 16 PID
filters at 0x1582 to 0x15a1, which the Windows drivers know how to program,
see [Transport stream input](protocol.md#transport-stream-input). What the
engine of mode 4 serves is unknown; Somagic's SMI-2020CBE also has a 1-bit
stream input for a GPS receiver (see [Related chips](#related-chips)),
which would be a candidate (inferred). The image of 2013 drops mode 4.

On an EasyCAP 002 with `smi2021_3c.bin`, endpoint `0x82` sends nothing in
modes 3 and 4, at either alternate setting: the host sees no response to
its tokens, apart from a single packet of 1023 bytes in mode 4 (observed).
The descriptors only have `0x82`, so there is nowhere else to look.

### System control register 0x3080

| Bit | Use |
|-----|-----|
| 7 | cleared at the host's request (`wValue` 0xfb); never set by the firmware, and used by none of the Windows drivers |
| 4 | set on suspend; the Windows driver initializes the device again when it finds it set, see [Initialization](protocol.md#initialization) |
| 2 | pulsed by the unused stop function, and for 10 ms by the Windows driver before it programs the PID filters, after which it writes 0x1600 again; so it presumably resets the stream paths, including 0x1600 (inferred) |

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

### CPU core

The core presumably is the "AVR Core" by Ruslan Lepetenok, published on
opencores.org from 2001, which is compatible with the ATmega103
(inferred). Its published versions are a VHDL one of 2006 and a Verilog
one of 2012; the core in the SMI2021 is apparently older than the one of
2006. What fits:

- it implements the ATmega103's instruction set; the later versions add
  the other instructions, which this core doesn't execute (see [CPU
  core](#cpu-core));
- its data space accesses wait on a `cpuwait` signal from the addressed
  block, which would explain why a read of an address that nothing answers
  stops the firmware for good (see [Data space](#data-space));
- its history names a bug fixed in 2005 that, by its sources, has exactly
  the effect seen in the first of the [Interrupt
  faults](#interrupt-faults), and its way of forming the vector address
  explains the second one if an older version takes it a cycle later.

The peripherals that come with it were not compared with the ATmega103
peripherals found here. Its SMBus module, which keeps the registers of
Atmel's TWI in the I/O space, has nothing in common with the I2C engines.

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

Apart from the ATmega103 core, presumably the AVR Core of opencores.org,
and its peripherals, the bridge is Somagic's own design, or licensed from a vendor whose controllers have no open
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
can have side effects, so the registers of the USB controller should be
left out of blind surveys.

Experiments 1 to 6 ran on an EasyCAP 002 with `smi2021_3c.bin` in November
2026; the results are in the sections named after each item.

With the original firmware:

1. **CPU clock and Timer0 prescaler**: write each clock select value to
   `TCCR0` and sample `TCNT0` repeatedly, timing the samples on the host.
   The rate for each value gives the prescaler table and the CPU clock,
   expected to be those of an ATmega103 and 30 MHz. **Done**: the
   ATmega103's, at 30.0 MHz, see [Clock](#clock).
2. **Data space survey**: read 0x1000–0xffff in blocks and note what reads
   back as something other than a constant, what keeps a written value,
   and what mirrors other addresses. Look for a copy of the firmware image,
   which would show that program memory is visible in data space, and for
   the bootloader. **Stopped**: a read of an address that nothing answers
   stops the firmware, see [Data space](#data-space), so a blind survey
   costs a power cycle per gap. Experiment 9 covers program memory instead.
3. **Other AVR peripherals**: write and read back I/O registers of the
   ATmega103 that the firmware doesn't use, for example `OCR0`, `OCR2`,
   `UBRR`, `SPCR`, `ADMUX` and `RAMPZ`, to find which exist; for ports B,
   D, E and F, only read the input registers and leave the direction
   registers alone. `MCUCR` (external memory and sleep), `MCUSR`,
   `WDTCR` and `XDIV` (clock divider) should not be written. **Done**, see
   [Standard AVR peripherals](#standard-avr-peripherals).
4. **I2C buffer**: write a pattern to 0x1785 and up and read it back to
   find the size of the buffer; scan the second bus for devices. **Done**:
   32 bytes, and no device on the second bus, see [I2C
   engines](#i2c-engines); the command bits are in [I2C
   engines](protocol.md#i2c-engines).
5. **Endpoint blocks**: in mode 5, swap the values of +1 or the states of
   blocks A and B, and see which block's data arrives at endpoint `0x82`.
   **Done**, see [Endpoint register blocks](#endpoint-register-blocks).
6. **Modes 3 and 4**: stream in each mode and look at what arrives. Mode 3
   should be a transport stream input, which an EasyCAP doesn't feed, so
   empty packets are likely; programming a PID filter and seeing whether
   0x1580 and the filters read back as written would confirm the
   registers. **Done** as far as streaming goes: nothing arrives, see
   [Streaming engines](#streaming-engines); the PID filters were not
   programmed.
7. **GPIO**: find what `PA7`, `PC0`, `PC1`, `PC2` and `PC4` are connected
   to. **Partly done** with a multimeter: `PA7` is the reset of the decoder
   and the audio ADC; `PC0` and `PC4` reach neither chip's reset nor the
   LED; see [Initialization](protocol.md#initialization). `PC1` and `PC2`
   were not driven, `PC2` being the input of the board identification.

With a test firmware, [`fw/`](../fw/README.md), on an EasyCAP 002 in
November 2026:

8. Check whether the core executes `MUL`, `MOVW`, `LPM Rd, Z`, `ELPM` and
   `SPM`. **Done** except for `SPM`: only the plain `ELPM` works, see [CPU
   core](#cpu-core).
9. Read program memory beyond the uploaded image to find its size and the
   bootloader. **Done**: 8 KB, and the bootloader is not in it, see
   [Program memory](#program-memory).
10. Install handlers on all vectors to find which interrupts exist, and
    check whether the USB interrupt is really vector 11 only. **Done** as
    far as it goes: only 0x002c is taken, see [Program
    memory](#program-memory). Why enabling the interrupt made the test
    firmware fail is item 12.
11. Measure the CPU clock with a loop of known length against the USB
    frame timing or a GPIO toggled into a scope. **Done**: a loop of 5 × 10⁷
    cycles took 1333.29 ms longer than one of 10⁷, timed by the host
    through the request it ran in, so 4 × 10⁷ cycles take 1333.29 ms, a CPU
    clock of 30.000 MHz, see [Clock](#clock).
12. Find why the test firmware fails with interrupts enabled: with
    interrupts enabled for one instruction at a time, record what each
    interrupt finds on the stack, and with the firmware's own interrupt
    handling, where it restarts. **Done**: two faults of the core, an
    interrupt that starts on a `PUSH` and an interrupt that enters the
    reset vector; with both worked around, the firmware runs with
    interrupts enabled, see [Interrupt faults](#interrupt-faults).

## Sources

- Atmel ATmega103 and ATmega16 datasheets, and the device headers
  `iom103.h` and `iom16.h` of avr-libc.
- Somagic's product pages on `www.somagic.com.cn`, from 2009 to 2016, as
  kept by the Internet Archive:
  <http://web.archive.org/web/20101018145745/http://www.somagic.com.cn:80/product_info.asp?id=60>
  (SMI-2020CBE),
  <http://web.archive.org/web/20101018145750/http://www.somagic.com.cn:80/product_info.asp?id=61>
  (SMI-2030) and
  <http://web.archive.org/web/20101018145734/http://www.somagic.com.cn:80/product_info.asp?id=51>
  (SMI-2022CBG reference design).
- The vendor's Windows drivers and the image of 2013 that came with one,
  as listed in [Sources](protocol.md#sources).
- The drivers in Linux (as of October 2026, and removed ones from its
  history), U-Boot and Zephyr named in [Comparison with known
  controllers](#comparison-with-known-controllers).
- The AVR Core of opencores.org, <https://opencores.org/projects/avr_core>,
  as mirrored by FreeCores: the Verilog version 14 of 2012 in
  <https://github.com/freecores/avr_core>, and the VHDL `pm_fetch_dec.vhd`
  version 2.6 of 2006 in `rtl/rtl_orig` of
  <https://github.com/freecores/avr_hp>.
