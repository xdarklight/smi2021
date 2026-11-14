# OpenSMI21: open source firmware for the SMI2021

A minimal replacement for the firmware of the Somagic SMI2021 USB video
bridge (EasyCAP DC60, EasyCAP 002), written from the documentation in this
repository. It implements the host interface of
[`docs/protocol.md`](../docs/protocol.md), so that the Linux driver in this
repository and [`tools/bridge-probe`](../tools/bridge-probe/README.md) work
with it unchanged, and adds test requests for the experiments 8 to 11 of
[`docs/hardware.md`](../docs/hardware.md#experiments).

> **Partly tested.** On an EasyCAP 002, version 0.1 enumerates at high
> speed, answers its test requests and ran experiments 8 to 12, also with
> interrupts enabled. Streaming, the register access requests and the Linux
> driver on top of it have not been tried yet. See [On
> hardware](#on-hardware).

## Building

Needs avr-gcc, binutils and avr-libc, for the ATmega103: its startup code,
its register headers, its interrupt macros and its delay loops. On Debian
and Ubuntu that is `gcc-avr`, `binutils-avr` and `avr-libc`; on Arch Linux
`avr-gcc`, `avr-binutils` and `avr-libc`.

    make                              # avr-gcc etc. from PATH
    make CROSS=/path/to/bin/avr-      # another toolchain
    make PID=0x003f                   # USB product id, default 0x003c
    make ALT0_STOP=0                  # alternate setting 0 stops nothing
    make IRQ=1                        # enable interrupts, see below
    make IRQ=1 ISR_EVENTS=0           # vector 11 only counts and logs
    make IRQ=1 ISR_BARE=1             # vector 11 is a bare RETI
    make IRQ=1 ISR_BARE=2             # vector 11 counts, reads no register
    make IRQ=1 ISR_BARE=3             # vector 11 reads 0x1503 only
    make IRQ=1 ISR_BARE=4             # vector 11 reads 0x1502 only
    make IRQ=1 ISR_BARE=5             # vector 11 records its return address
    make IRQ=1 ISR_BARE=6             # reads 0x1503, 0x1502, no stack
    make IRQ=1 ISR_BARE=7             # does what the original does, no stack
    make IRQ=1 ISR_BARE=10            # vector 11 records its return address,
                                      # then runs the C handler
    make ISR_BARE=9                   # interrupts only in a test window (0xf9)
    make PUSH_GUARD=0                 # leave avr-gcc's PUSHes unprotected

`IRQ=0`, the default, never enables interrupts and polls the controller.
`IRQ=1` enables them and services the USB events in vector 11; the main
loop only takes what the handler recorded, and polls the controller only
if the storm guard has switched interrupts off for good. The core has two
faults that a firmware with interrupts enabled has to take care of
(`docs/hardware.md` "Interrupt faults"), and `IRQ=1` does:

- every run of `PUSH`es that avr-gcc writes, and every `rcall .`, is put
  between `CLI` and the `OUT` that restores SREG, by `pushguard.awk`,
  which the build runs on the compiler's assembly (`PUSH_GUARD=1`, the
  default; with `IRQ=0` it costs a few bytes and is harmless). Interrupt
  handlers (`__vector_N`) are left alone, as they run with I clear;
- an interrupt requested while interrupts are disabled for more than a
  few dozen cycles may enter the reset vector when they are enabled again.
  So the main loop disables them for some 22 cycles only, to take
  `usb_events`, and `ctrl_aborted()`, which the waits of the control
  endpoint call, likewise; the storm guard reads its counter of interrupts
  with interrupts enabled. The test requests still disable them for longer
  while they copy their replies;
- as a safety net, the startup code takes an entry at the reset vector
  with I clear, `irq_armed` set, `boot_magic` set, SP within the stack
  (from `__noinit_end` to 0x0ffd) and a return address into the program
  on top of the stack for an interrupt that ended up there, counts it (see
  0xf8) and enters vector 11's handler instead of restarting. `irq_armed`
  is set before every `SEI` and before the handler returns, and cleared by
  `irq_off()` and on the handler's entry, so that a jump to 0x0000 from
  code running with interrupts disabled restarts the firmware.

Without the first, `IRQ=1` fails within a few control transfers. With the
main loop polling the controller with interrupts disabled for some 60
cycles or more, as `IRQ=0` does, about one interrupt in 4000 came through
the reset vector; the safety net kept the firmware running then.

`ISR_EVENTS` and `ISR_BARE` only matter for the interrupt tests, see 0xf3,
0xf7, 0xf8 and 0xf9 under [Test requests](#test-requests).

The result is `build/opensmi21.bin`: a plain image from address 0, padded
with 0xff to a multiple of 62 bytes, as the bootloader takes it. The build
fails if the image grows beyond 7502 bytes, the largest image known to
work, though program memory is 8 KB. `make` also runs `check.sh`, see
[Checks](#checks). Built with avr-gcc 7.3.0 and the defaults, version 0.1
is 5736 bytes, 5766 padded (93 upload units); with `IRQ=1`, 6074 bytes,
6076 padded (98 upload units).

To use it with the Linux driver, install it in place of the vendor image:

    cp build/opensmi21.bin /lib/firmware/smi2021_3c.bin

or upload it by hand to a device in bootloader mode, with the driver
unloaded:

    tools/bridge-probe/probe.py upload fw/build/opensmi21.bin

## Files

| File | Contents |
|------|----------|
| `isr.S` | handlers of the vectors other than 11, for the census; the stack pointer at reset for the crash record; with `IRQ=1`, the check for an interrupt that entered the reset vector; the assembly handlers of `ISR_BARE` |
| `main.c` | bring-up and main loop |
| `usb.c` | USB controller, control endpoint, standard requests, descriptors |
| `vendor.c` | vendor requests: modes, streaming engines, register access, i2c |
| `test.c` | test requests |
| `asm.S` | delay loops, program memory reads, instruction tests, the interrupt window of `ISR_BARE=9` |
| `win.h` | layout of the record of the interrupt window, request 0xf9 |
| `pushguard.awk` | puts avr-gcc's `PUSH`es under `CLI`, see [Building](#building) |
| `regs.h` | register addresses from `docs/hardware.md` |
| `fw.h` | shared declarations, version, build options |
| `opensmi21.ld` | memory layout |
| `check.sh` | checks of the disassembly and the image |
| `test-requests.py` | host side of the test requests, with python-libusb1 |

## What is implemented

- **Core**: built with `-mmcu=atmega103` and avr-libc, as the core decodes
  the ATmega103's instruction set (`docs/hardware.md` "CPU core").
  avr-libc's startup code: 24 vectors of one `JMP` each, `SP` = 0x0fff,
  `.data` copied with the plain `ELPM`, `.bss` cleared, then `main()`.
  SRAM 0x0060–0x0fff; the linker script is the firmware's own, for the
  info block at 0x0060 and the limits.
- **Bring-up**: the sequence of `docs/hardware.md` "Startup": `DDRA` = 0xff,
  status of both i2c engines 0, mode 3 prepared (0x1580 = 0x03, 0x1880 = 0),
  the USB controller set up with the documented values, `PC0` driven low,
  and the soft disconnect: 200 units of 160 cycles, 0x1500 bit 3 set, 2000
  units, bit 3 cleared.
- **USB events**: polled, as the original firmware does: the main loop and
  every wait of the control endpoint read 0x1503, which reading doesn't
  acknowledge, and record the event; the main loop handles it. With
  `IRQ=1`, vector 11 records the events instead, and the main loop only
  takes them, see [Building](#building). Setup packet (0x01 with 0x1502 bit 4: the handler
  clears the bit and withdraws a pending packet by clearing 0x151c bit 7),
  speed detected (0x80: maximum packet size of blocks A and B 0x0400 at high
  speed, 0x03ff otherwise), bus reset (0x81), suspend (0x85: `PC0` low,
  0x3080 bit 4, 0x1500 bit 2). Other codes are counted and ignored. As the
  code stays until the next event, a reset, speed or suspend event is
  handled again on every pass until another one arrives, and only a change
  of the code is logged and counted.
- **Control endpoint**: setup packet from 0x2000, OUT data packets from
  0x2000, IN data packets of up to 64 bytes at 0x2040 with the length in
  0x151b and 0x151c bit 7 to send, zero length status packets, stall with
  0x1510 bits 2–1 = 10. Every wait gives up when a new setup packet or a bus
  reset arrives. Fixed against the original as `docs/hardware.md` "Writing
  a replacement" asks: OUT requests without a data stage are acknowledged,
  `wLength` of OUT requests is checked against the 288 byte buffer (larger
  ones are stalled), and an IN transfer that ends on a full packet before
  `wLength` gets a packet of zero bytes.
- **Device state**: `SET_ADDRESS` writes 0x1504 only after the status
  packet has gone out (0x151c bit 7 clear). Default, address and configured
  state are tracked.
- **Standard requests**: `GET_STATUS`, `CLEAR_FEATURE`, `SET_FEATURE`,
  `SET_ADDRESS`, `GET_DESCRIPTOR`, `GET_CONFIGURATION`, `SET_CONFIGURATION`,
  `GET_INTERFACE`, `SET_INTERFACE`; all others are stalled.
- **Descriptors**: USB 2.0, `1c88:003c` (product id selectable), control
  packet size 64, one configuration with one vendor specific interface
  (0xff/0xff/0xff) with alternate settings 0 (no endpoint), 1 (0x82,
  isochronous IN, 1023 bytes) and 2 (0x82, isochronous IN, `wMaxPacketSize`
  0x1400), every microframe. At full speed both alternate settings have 1023
  bytes every frame. Device qualifier and other speed configuration as
  USB 2.0 requires; 0x1501 bit 1 selects the speed. Strings: manufacturer
  "OpenSMI21", product "SMI2021 open source firmware", serial number
  "OPENSMI21-0.1" (prefix and version), `bcdDevice` 0x0001 (version 0.1).
- **Vendor requests**: as in `docs/protocol.md`. OUT: first data byte 0x01
  sets the mode, anything else is a register access; `bRequest` and
  `wValue` are ignored. IN with `bRequest` 0x01: `wValue` 0x01 (mode),
  0x0b/0x0c/0x0d (reply of 13/21/37 bytes to the last register access),
  0xfb (clears 0x3080 bit 7, replies 0xfb 0x01); anything else is stalled.
- **Modes 2 to 5**: the mode request writes 0x15 and state 01 to one
  endpoint block and 0x25 and state 00 to the other one as in the table of
  `docs/hardware.md` "Endpoint register blocks", clears block B +8 and +12,
  and does the engine writes and the 0x1880 value of `docs/protocol.md`
  "Modes". `SET_INTERFACE` to alternate setting 1 or 2 does the start writes
  of the current mode and sets 0x1600 bit 7.
- **Register access**: bridge register reads and writes of `size` bytes at
  a 16 bit address, i2c transfers on engine 0x1780 or, with flag bit 6,
  0x3180, following the programming sequence of `docs/hardware.md` "I2C
  engines", one try and up to 16 retries, bit 0 of the command set in the
  reply if the last try failed. The info query (flag bit 4) returns the 16
  bytes at SRAM 0x0060. Stalled as documented: a bridge access with other
  than 2 address bytes, command bits 7–6 `10` with address bytes, `11` with
  other than one address byte.
- **Test requests**: see [Test requests](#test-requests).

### Out of scope

- The IR receiver: no decoder, no `bRequest` 0x91, no special handling of
  writing 0x81 to 0x1600 or of reading 0x1600–0x1605. These are plain
  register accesses here.
- The requests of `smi2021_3f.bin` only: the port C request (`bRequest`
  0x50; as an OUT request without data it is acknowledged and does
  nothing) and the IN request with `bRequest` 0 (stalled). `PC2` is not
  touched.
- Endpoint block C (0x1540).
- `SET_FEATURE(TEST_MODE)` and remote wakeup.

## Design decisions

Where the documentation leaves something open, this firmware does the
following:

1. **Alternate setting 0** (`ALT0_STOP=1`, the default): going from a
   non-zero alternate setting to 0, by `SET_INTERFACE` or
   `SET_CONFIGURATION`, clears the bits that the start wrote for the mode
   that was started: mode 2 clears bits 0 and 2 of 0x1740, mode 3 bit 7 of
   0x1580, mode 4 writes 0 to 0x3100, mode 5 clears 0x0d in 0x1800 and 0x05
   in 0x1740; then 0x1600 bit 7 is cleared. The vendor's untested stop
   function (pulse of 0x3080 bit 2) is not used. `ALT0_STOP=0` makes it a
   no-op as in the original. A bus reset does not stop the engines, as in
   the original. The Linux driver stops the engines itself before it
   selects alternate setting 0, so it works either way.
2. **Info query**: the 16 bytes at SRAM 0x0060 keep their documented
   initial values 0x00–0x0f. The documents give them no meaning but don't
   say a host may ignore them either, so they were not replaced by an
   identity block; the firmware identifies itself through its serial
   number and test request 0xf0 instead.
3. **Register access bounds**: requests shorter than the 5 byte header are
   stalled; a read must fit the reply (address bytes + size ≤ 32); a write
   must have its data in the request (5 + address bytes + size ≤
   `wLength`); i2c transfers are limited to 32 data bytes, the buffer that
   the inferred extent of the engines (+5 to +0x24) leaves. Bridge writes
   may have up to 255 bytes. Missing bytes of a short request read as 0
   (the subaddress register gets the first byte after the header, 0 if
   there is none).
4. **Reply contents**: every accepted register access, write or read,
   replaces the reply: request bytes 1 to 4 (i2c address, command, flags,
   size), the address bytes, then the data read, or for a write as much of
   the written data as fits; the rest is 0. The failure bit is also set for
   failed i2c writes, though no host reads that. The info query reply has
   request bytes 1 to 4 and the 16 bytes from offset 5.
5. **Order of checks**: the info flag comes first and skips all checks;
   then the documented stall rules; then the bounds.
6. **i2c**: 17 tries at most ("up to 16 retries"). Each wait for the
   engine gives up after 50000 polls of the status register, some 20 ms at
   30 MHz, and counts as a failed try.
7. **Mode request**: modes other than 2 to 5 and a request with only one
   byte are stalled and leave the mode alone, so the mode reply never reads
   0x07, which hosts take for the bootloader. The inactive block is written
   first, then the active one, then block B +8/+12, then the engines, then
   0x1880. Endpoint +1 is written before the state bits in +0.
8. **Start of streaming**: the engine writes first, then 0x1600 bit 7.
   `SET_INTERFACE` to a non-zero setting starts again even if one is
   already selected, as the vendor sequence "mode 2, setting 2, then mode
   5, setting 2" needs.
9. **IN vendor requests** need `bRequest` 0x01 (0xf0 and up are the test
   requests); only the low byte of `wValue` is used. The recipient bits and
   `wIndex` are ignored for all vendor requests except where a test request
   uses `wIndex`.
10. **Halt feature of endpoint 0x82** acts on the block that the current
    mode made endpoint 0x82 (block B in modes 3 and 5, A in modes 2 and 4),
    not always on block B as in the original; setting it writes state 10,
    clearing it state 00 and then clears bit 0. Endpoint 0x81 and 0x83 are
    not known, as the descriptors don't have them.
11. **`SET_CONFIGURATION(1)`** sets the state of blocks A and B to 00, as
    the original does for all blocks; block C is not touched.
12. **Bus reset** also writes 0 to the address register 0x1504, which the
    hardware presumably does by itself.
13. **Stall**: the stall bits of the control endpoint are also cleared at
    the start of every setup packet, in case the hardware doesn't leave
    the stall by itself.
14. **PC0** is driven low by clearing `PORTC` bit 0 before setting `DDRC`
    bit 0, so it never glitches high.
15. **Descriptors not given by the documents**: device class 0 (class per
    interface), bus powered, no remote wakeup, isochronous endpoints
    without synchronization (`bmAttributes` 0x01), `bInterval` 1, language
    0x0409. The power value, 500 mA, was chosen the same way, and is what
    the vendor firmware reports as well.
16. **Requests without data**: an OUT vendor request without a data stage
    is acknowledged without doing anything; an IN request with `wLength` 0
    gets a zero length packet.
17. **Interrupts** stay disabled by default (`IRQ=0`): it needs nothing
    of the core's faults taken care of, and has been tested the longest.
    `IRQ=1` has been tested with control transfers only.

## Test requests

All test requests are vendor IN requests to the device: `bmRequestType`
0xc0, `bRequest` 0xf0 to 0xf9. They don't clash with `bRequest` 0x01 of
the driver and the bootloader, and work in every device state. A reply
longer than `wLength` is cut to `wLength`. Unknown `bRequest` values from
0xf0 up are stalled. Multi-byte values are little endian. Nothing writes
program memory; there is no `SPM`. `test-requests.py` sends each of them
and decodes the reply, for example:

    ./test-requests.py identity
    ./test-requests.py insn 4
    ./test-requests.py pm 0 65536 > program-memory.bin
    ./test-requests.py timing 10000 50000

### 0xf0: identity

`wValue` 0, `wIndex` 0, `wLength` 29.

| Offset | Size | Contents |
|--------|------|----------|
| 0 | 9 | "OpenSMI21" |
| 9 | 1 | version of the test request interface, 3 |
| 10 | 1 | firmware version, major (0) |
| 11 | 1 | firmware version, minor (1) |
| 12 | 2 | USB product id |
| 14 | 2 | image size in bytes, without padding |
| 16 | 1 | build options: bit 0 `ALT0_STOP`, bit 1 `IRQ`, bit 2 `ISR_EVENTS`, bit 3 `VECTORS=rjmp48` (before avr-libc), bit 4 `ISR_BARE=1`, bit 5 `ISR_BARE=2`, bit 6 `ISR_BARE=3`, bit 7 `ISR_BARE=4`; the other values of `ISR_BARE` are in byte 28 only |
| 17 | 1 | current mode |
| 18 | 2 | start counter: number of starts since the counter was last found invalid (see 0xf3) |
| 20 | 1 | last vector that hit a census stub, 0xff for none |
| 21 | 1 | device state: 0 default, 1 address, 2 configured |
| 22 | 1 | current alternate setting |
| 23 | 1 | 0x1501 (bit 1: high speed) |
| 24 | 2 | number of USB events with an unknown code |
| 26 | 2 | number of times vector 11 was taken, saturating at 0xffff |
| 28 | 1 | build options: bit 0 `PUSH_GUARD`, bits 7–4 `ISR_BARE` |

Interface version 1, of the firmware before the rename, had "SMIOPEN" in 7
bytes and so every field 2 bytes earlier, and ended at offset 24. Version
2 ended at offset 28 and had no request 0xf9.

### 0xf1: instruction test (experiment 8)

`wValue` = test number, `wIndex` bits 7–0 = value written to `RAMPZ` (I/O
0x3b, ATmega103) for the `ELPM` tests, `wLength` 18. A test number above 6
is stalled.

Each test loads known values into the registers involved, runs the
instruction once as a raw opcode, and reports the registers before and
after. A core that lacks an instruction may ignore it, execute something
else, or crash; run each test on its own and check with 0xf0 that the
firmware is still alive (the start counter shows a restart through the
reset vector).

| Offset | Contents |
|--------|----------|
| 0 | test number |
| 1 | `RAMPZ` value written (`ELPM` tests), else 0 |
| 2–5 | registers A, B, C, D before |
| 6–9 | registers A, B, C, D after |
| 10 | `SREG` before the instruction |
| 11 | `SREG` after the instruction |
| 12 | `RAMPZ` read back after writing it (`ELPM` tests), else 0 |
| 13 | `RAMPZ` after the instruction (`ELPM` tests), else 0 |
| 14–15 | first two pattern bytes, read with plain `LPM`: 0x3c, 0xc3 |
| 16–17 | program memory address of the pattern |

| Test | Instruction | Opcode | A, B, C, D | Before | Executed correctly | Ignored |
|------|-------------|--------|------------|--------|--------------------|---------|
| 0 | `MUL r18, r19` | 0x9f23 | r0, r1, r18, r19 | a5 5a c3 5b | 51 45 c3 5b; `SREG` C and Z clear (both set before) | a5 5a c3 5b; C and Z still set |
| 1 | `MOVW r18, r20` | 0x019a | r18, r19, r20, r21 | 11 22 33 44 | 33 44 33 44 | 11 22 33 44 |
| 2 | `LPM r18, Z` | 0x9124 | r18, r0, ZL, ZH | a5 5a Z | 3c 5a Z | a5 5a Z (a5 3c Z: executed as plain `LPM`) |
| 3 | `LPM r18, Z+` | 0x9125 | r18, r0, ZL, ZH | a5 5a Z | 3c 5a Z+1 | a5 5a Z |
| 4 | `ELPM` (r0) | 0x95d8 | r0, r18, ZL, ZH | 5a a5 Z | 3c a5 Z with `RAMPZ` 0 | 5a a5 Z |
| 5 | `ELPM r18, Z` | 0x9126 | r18, r0, ZL, ZH | a5 5a Z | 3c 5a Z with `RAMPZ` 0 | a5 5a Z |
| 6 | `ELPM r18, Z+` | 0x9127 | r18, r0, ZL, ZH | a5 5a Z | 3c 5a Z+1 with `RAMPZ` 0 | a5 5a Z |

Z is the pattern address of bytes 16–17. With a non-zero `RAMPZ`, a working
`ELPM` returns the byte at `RAMPZ` × 0x10000 + Z instead; compare with
request 0xf2. Byte 12 equal to the written value suggests that `RAMPZ`
exists. The firmware puts the previous value of `RAMPZ` back after the
test. `SPM` is deliberately not tested.

### 0xf2: program memory read (experiment 9)

`wValue` = byte address, `wIndex` bits 7–0 = `RAMPZ`, bit 8 = use `ELPM`,
`wLength` = number of bytes, at most 64 (more is cut to 64).

Returns the bytes at `wValue` and up, read with plain `LPM` (`r0`), or with
`ELPM` (`r0`, the raw opcode 0x95d8) if bit 8 is set, after writing
`RAMPZ`; the previous `RAMPZ` is put back afterwards. Z wraps from 0xffff to
0 without a carry into `RAMPZ`. Reading the image itself back is a
reference; where the reads stop repeating the image or turn into a
constant shows the size of program memory, and a different, stable content
may be the boot ROM. `ELPM` should only be used after test 4 of 0xf1 has
shown that it works.

### 0xf3: interrupt census (experiment 10)

`wValue` bit 0 = clear the counters after reading, `wIndex` 0, `wLength`
49.

| Offset | Contents |
|--------|----------|
| 0–1 | entry for vector 0: the start counter (never cleared) |
| 2n–2n+1 | number of times vector n was taken, n = 1 to 23, saturating at 0xffff |
| 48 | last vector that hit a stub, 0xff for none |

The census of vectors 2 bytes apart in `docs/hardware.md` ran with a table
of 48 one word `RJMP`s, `make VECTORS=rjmp48` in commit bcc9f23; avr-libc's
fixed table of 24 `JMP`s leaves no room for it.

Vector 11 is the USB handler and counts its calls itself; every other
vector of the 24 points to a stub that counts and returns with `RETI`. The
firmware enables no interrupt source itself, so any count other than vector
11 comes from a source that was enabled at reset or by the bootloader, or
from the USB controller raising more than one vector. The start counter is
kept in a part of SRAM that the startup code doesn't clear; it is restarted
at 1 whenever its magic word doesn't match, which after power up is the
likely case.

### 0xf4: timing (experiment 11)

`wValue` = N, `wIndex` 0, `wLength` 4.

Runs a loop of exactly 1000 × N − 1 cycles with interrupts disabled before
it answers, plus a constant overhead of the request handling. Reply: N
(2 bytes) and the cycles per unit, 1000 (2 bytes). The host measures the
duration of the request for two values of N; the difference divided by
1000 × ΔN cycles gives the CPU clock, the constant overhead cancels. The
cycle count assumes the timing of the classic AVR core: `LDI`, `NOP`, `DEC`
1 cycle, `SBIW` 2, `BRNE` 2 when taken. At 30 MHz, N = 65535 takes about
2.2 s; keep the host's timeout above that. USB events wait during the loop.

### 0xf5: USB event log

`wValue` bit 0 = clear the log after reading, `wIndex` 0, `wLength` 34.

| Offset | Contents |
|--------|----------|
| 0 | index of the next entry to be written, 0–15 |
| 1 | number of events since the last clear, saturating at 255 |
| 2–33 | 16 entries of 2 bytes: event code from 0x1503, and 0x1502 as read for code 0x01 (0 for other codes) |

The log is a ring; once more than 16 events were logged, the entry at the
index of byte 0 is the oldest one.

### 0xf6: crash record

`wValue` 0, `wIndex` 0, `wLength` 70.

| Offset | Contents |
|--------|----------|
| 0–1 | start counter |
| 2–3 | `SP` at the reset vector, before the startup code set it, at the last restart without power loss |
| 4 | where the last control request was at that restart: 0 done or none, 1 setup packet read, 2 IN request handled, 3 OUT data received, 4 OUT request handled, 5 status packet queued |
| 5 | index of the next entry below |
| 6–69 | the last 8 setup packets before that restart, 8 bytes each, a ring |

The record lives in a part of SRAM that the startup code doesn't clear;
`main()` copies it aside when it finds the start counter valid, which is a
restart without power loss, before the next enumeration overwrites it.
After a power cycle it is zero, unless the SRAM kept its contents over a
short one.

### 0xf7: interrupt record

`wValue` 0, `wIndex` 0, `wLength` 71.

| Offset | Contents |
|--------|----------|
| 0–3 | number of times vector 11 was taken |
| 4 | 1 if the storm guard switched interrupts off for good |
| 5 | number of entries below, up to 8 |
| 6–37 | for the first 8 calls of vector 11: 0x1503, 0x1502, 0x151c and 0x1518 as read on entry |
| 38 | number of entries below, up to 8 |
| 39–70 | for the first 8 setup packets the main loop finds: 0x1503 read twice in a row, then 0x1502 read twice in a row |

The second table is filled in every build; it showed that reading 0x1503
or 0x1502 doesn't change them.

The storm guard switches interrupts off for good when vector 11 was taken
more than 50 times between two passes of the main loop, so that an
interrupt that keeps firing leaves the firmware able to report it.

### 0xf8: return addresses of vector 11

`wValue` 0, `wIndex` 0, `wLength` 200, or 206 with `IRQ=1`. The ring is
filled with `ISR_BARE=5` and `ISR_BARE=10` only.

| Offset | Contents |
|--------|----------|
| 0–99 | the record as it was at the last restart without power loss |
| 100–199 | the record now |
| 200–205 | `IRQ=1` only: interrupts that entered the reset vector and were taken by the safety net, see [Building](#building), 0 so far since the main loop keeps interrupts disabled briefly: their number (saturating at 255), then for the last one SREG, SP less 3 (low byte first) and the return address on the stack (high byte first, a word address) |

Each record:

| Offset | Contents |
|--------|----------|
| 0 | number of entries ever written, wrapping at 256 |
| 1 | number of handlers running right now |
| 2 | the most that ever ran at once |
| 3 | 0 |
| 4–99 | a ring of 32 entries of 3 bytes: the return address the core pushed, as the two bytes on the stack (high byte first, a word address), and SREG as found in the handler |

The record lives in a part of SRAM that the startup code doesn't clear,
and is copied aside at a restart without power loss, before interrupts
are enabled again. The firmware may hang rather than restart, so it is
best read over and over while the firmware runs.

### 0xf9: interrupt window (`ISR_BARE=9`)

`wValue` the mode, `wIndex` the number of main loop passes to run the
window for, `wLength` 282. A build with `ISR_BARE=9` (and `IRQ=0`) never
enables interrupts except in `irq_window()` in `asm.S`, which the main
loop runs once per pass while a window is requested: 50 rounds, each of
which enables interrupts for a few instructions and disables them again.
Its own handler of vector 11 records what each interrupt finds and returns
with interrupts disabled whenever it is not inside the window, so that the
rest of the firmware always runs with them disabled. This is how
`docs/hardware.md` "Interrupt faults" was found.

With `wIndex` not 0, the request clears the record, sets the mode and
starts the window, which ends by itself after `wIndex` passes (up to 255).
With `wIndex` 0 it only reads the record. The reply is the record of
`win.h`, 280 bytes, and the byte address of `win_first`, the start of the
window code, 2 bytes.

The low byte of `wValue` (`WMODE_*` in `win.h`): bit 7 runs the window;
bits 0–2 make the handler read 0x1503, 0x1502, and 0x151c and 0x1518;
bit 3 closes the window with an `OUT` to SREG instead of `CLI`; bits 4–6
put three `PUSH`es, three `POP`s, or calls and memory accesses into the
window, each class opened by its own `SEI`. The high byte (`WMODE2_*`):
bit 0 makes the window just `SEI` and the closing instruction; bit 7 makes
it `SEI` and one instruction out of 26, number bits 4–0, of `wi_table` in
`asm.S`. As the main loop runs with interrupts disabled, an interrupt
pending when the window opens is taken on the instruction right after its
`SEI`.

Before each round the window sets the 24 bytes below its stack and `r24`,
which the handler saves where the next return address would go, to 0xff.
A return address the core did not write then reads 0xffxx; the handler
counts it, notes it and returns to `win_lost`, which ends the round. The
record (`WREC_*`): mode, entries logged, windows run, interrupts taken,
rounds that ended with SP off and by how much, interrupts taken with the
window closed and the return address of the last, rounds given up after
100 interrupts, the window's SP, return addresses not written (count, what
was found instead, and the word of the window the interrupt before it
returned to), the second mode byte, the number of times I was found clear
right after the instruction under test without the handler having run, a
log of the first 64 interrupts (return address and SP relative to the
window's) and the number of returns to each of the first 64 words of the
window. `test-requests.py win` arms the window and decodes the record,
for example `win 0x97 1 10` (`PUSH`es, one pass, ten times) or
`win $(( 0x81 << 8 | 0x87 )) 1 10` (instruction 1, `PUSH`).

## Checks

`check.sh`, run by `make`, checks the output of `avr-objdump -d`:

- the vector table: 24 `JMP`s at 0x0000–0x005c, vector 0 to the startup
  code at 0x0060, vector 11 to the USB handler, all others to their census
  handler;
- the delay loop of the timing request counts down with a `DEC`. The
  assembler sources go through the C preprocessor, and a backslash at the
  end of a comment once made it swallow the `DEC` of both hand-written
  delay loops, so that the first delay of the bring-up never ended; the
  bring-up's delay is avr-libc's now;
- the startup code: the stack pointer saved for the crash record, then
  `SP` set to 0x0fff, `.data` copied with exactly one plain `ELPM`;
- with `PUSH_GUARD=1`, every `PUSH` and `rcall .` outside the interrupt
  handlers, the startup code's check for an interrupt that entered the
  reset vector (which runs with I clear) and the interrupt window follows
  a `CLI` or another such `PUSH`; it counts them;
- no `MUL`, `MULS`, `MULSU`, `FMUL*`, `MOVW`, `ELPM` with a register,
  `LPM Rd, Z(+)`, `SPM`, `SLEEP`, `WDR`, `EIJMP`, `EICALL`, `BREAK` or `DES`
  anywhere except the one deliberate instruction in each instruction test
  function, and the plain `ELPM` only there, in `pm_read` and in the
  startup code; it lists the deliberate ones;
- the image: a multiple of 62 bytes, at most 7502, starting with a `JMP`.

## Clean-room statement

This firmware was written from these sources only:

- `docs/protocol.md`, `docs/hardware.md` and `README.md` of this
  repository, the documents as of commit 4c6a688 and the revision of both
  that appeared, uncommitted, in the working tree during the work;
- the host driver: `smi2021-main.c`, `smi2021-bootloader.c` and
  `smi2021.h`;
- `tools/bridge-probe/README.md`, `bridge.py`, the emulation of the
  documented firmware in `test_probe.py`, and a search of `probe.py` for
  its use of the info query;
- avr-libc's headers `avr/iom103.h` and `avr/common.h`, for the ATmega103
  register addresses; since the move to avr-libc, also its startup code,
  headers and delay loops as a library;
- general public knowledge of the AVR instruction set, of avr-gcc and of
  USB 2.0.

No vendor firmware image, vendor driver or disassembly of either was
looked at, and nothing in `firmware/` was opened or listed. Where the
documents leave a question open, the answer is a decision listed above,
not a look at the original.

That holds for the firmware as first written. The changes made while
bringing it up on hardware, in November 2026, were made by someone who
has read the disassembly of the vendor images, while writing
`docs/hardware.md`. They are of two kinds, and neither copies code:

- fixes of this firmware's own code, found from its own listing or on the
  device: the delay loops that never ended, the crash and interrupt
  records, the build options for the interrupt test;
- the change to polled event handling, made after `docs/hardware.md` was
  corrected (0x1503 is not acknowledged by reading it, and the original
  polls), and following the corrected document.

- the interrupt work (`pushguard.awk`, the check at the reset vector, the
  interrupt window and its handler, `ISR_BARE=9` and `10`): made from what
  the device did in tests and from the published sources of the AVR Core
  of opencores.org, which `docs/hardware.md` identifies as the core; the
  one fact about the original firmware it relies on, that the image has no
  `PUSH`, `POP` or `CLI`, is in `docs/hardware.md`.

Where a test on the device needed a fact about the original firmware, the
fact went into the documents first.

## On hardware

Version 0.1 on an EasyCAP 002, uploaded through the bootloader with
`tools/bridge-probe/probe.py upload`, in November 2026:

- **Bring-up**: the first image never got to its soft disconnect: a
  comment ending in a backslash had made the C preprocessor swallow the
  `DEC` of both delay loops. `check.sh` now checks for it.
- **Enumeration**: the next image disconnected but never answered a setup
  packet, as it took events from vector 11 alone and read 0x1503 once, as
  the documents then said. Polled as the original firmware does, it
  enumerates at high speed and is configured.
- **Interrupts**: with interrupts enabled, the firmware first failed with
  every handler tried on vector 11, from a bare `RETI` to one that does
  what the original handler does without touching the stack (`ISR_BARE=1`
  to `7`): at best it got through an enumeration and stopped answering
  within a few control transfers. The interrupt window (0xf9) found that an
  interrupt that starts on a `PUSH` loses its return address, and no other
  of 26 instructions does. With `pushguard.awk`, the firmware ran, but
  restarted by itself every few hundred to few thousand control transfers;
  the return log (0xf8, `ISR_BARE=5` and `10`) and the SP at the restart
  showed interrupts that had entered the reset vector, about one in 4000.
  With the check at the reset vector as well, `IRQ=1 ISR_BARE=10` ran 5000
  requests each of the device and configuration descriptor, identity,
  event log and program memory read, then `soak 10000`, without a restart:
  191608 interrupts, 46 of them through the reset vector. The plain
  `IRQ=1` build passed the same: 190983 interrupts, 34 of them through the
  reset vector, all at the same place, right after `irq_on()` in
  `usb_poll()`; the timing request still measured 30 MHz. A test build
  that counted interrupts per place that enables them, with switches for
  that place, showed that they were the ones requested while the main
  loop had interrupts disabled for 60 cycles or more, and that disabling
  them for some 25 cycles only avoids it. With that, `IRQ=1` ran 786436
  interrupts, from about 260000 control requests and five port resets in
  between, without a restart and with none through the reset vector, as
  fast as before. See `docs/hardware.md` "Interrupt faults".
- **Test requests**: all of them work. Experiments 8 to 11 gave: only the
  plain `ELPM` of the instructions tested works, program memory is 8 KB,
  the CPU clock is 30.000 MHz; see `docs/hardware.md` "Experiments".
- **avr-libc**: built with avr-libc's startup code and delay loop instead
  of its own, the firmware passed the same tests again: enumeration, all
  test requests, the instruction tests, a read of the whole program memory
  and the timing request.
- **Not tried yet**: streaming in any mode, the register access requests
  and i2c, alternate settings, the Linux driver on top of this firmware.

## Untested and risks

Points most likely to need attention next:

- **Interrupts**: `IRQ=1` has been tested with control transfers only.
  Why an interrupt that waited long enters the reset vector is not known
  for certain, and the test requests still disable interrupts for longer
  than the main loop does; the safety net at the reset vector counts any
  that do (0xf8). Streaming may change the pattern of interrupts. `IRQ=0`
  stays the default.
- **Event handling inferred from the documents**: enumeration works, so
  clearing 0x1502 bit 4 by read-modify-write, the status stage of IN
  transfers that the hardware completes itself and the address change
  after the status packet are right as far as enumeration exercises them.
  Stalls and OUT data stages have not been exercised yet.
- **OUT data packets**: like the original, the firmware takes 64 bytes, or
  what is left of `wLength`, from each, and doesn't read 0x1517, which
  presumably holds the length of a received packet (inferred).
- **Alternate setting 0 stopping the engines** has never been done by the
  vendor firmware; if streaming fails to restart, try `ALT0_STOP=0`.
- **Timing request** keeps interrupts off for 1000 × N cycles, up to about
  2.2 s at 30 MHz; a bus reset during it is handled late.
- **Descriptors**: the fields the documents don't give are guesses that
  should not matter to the host; the power value, 500 mA, matches the
  vendor firmware's.
