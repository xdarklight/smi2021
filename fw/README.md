# OpenSMI21: open source firmware for the SMI2021

A minimal replacement for the firmware of the Somagic SMI2021 USB video
bridge (EasyCAP DC60, EasyCAP 002), written from the documentation in this
repository. It implements the host interface of
[`docs/protocol.md`](../docs/protocol.md), so that the Linux driver in this
repository and [`tools/bridge-probe`](../tools/bridge-probe/README.md) work
with it unchanged. The Linux driver uploads it.

> **Partly tested.** On an EasyCAP 002, the Linux driver works on version
> 0.1: video and audio capture in PAL and NTSC, mode switches and the
> driver's stress tests pass, with and without interrupts. Modes 3 and 4,
> the second i2c engine and a DC60 have not been tried. See [On
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

`IRQ=0`, the default, never enables interrupts and polls the controller.
`IRQ=1` enables them and services the USB events in vector 11; the main
loop only takes what the handler recorded, and polls the controller only
if the storm guard has switched interrupts off for good. The core has two
faults that a firmware with interrupts enabled has to take care of
(`docs/hardware.md` "Interrupt faults"), and `IRQ=1` does:

- every run of `PUSH`es that avr-gcc writes, and every `rcall .`, is put
  between `CLI` and the `OUT` that restores SREG, by `pushguard.awk`,
  which the build runs on the compiler's assembly; with `IRQ=0` it costs
  a few bytes and is harmless. Interrupt handlers (`__vector_N`) are left
  alone, as they run with I clear;
- an interrupt requested while interrupts are disabled for more than a
  few dozen cycles may enter the reset vector when they are enabled again.
  So the main loop disables them for some 22 cycles only, to take
  `usb_events`, and `ctrl_aborted()`, which the waits of the control
  endpoint call, likewise; the storm guard reads its counter of interrupts
  with interrupts enabled;
- as a safety net, the startup code takes an entry at the reset vector
  with I clear, `irq_armed` set, `boot_magic` set, SP within the stack
  (from `__noinit_end` to 0x0ffd) and a return address into the program
  on top of the stack for an interrupt that ended up there, and enters
  vector 11's handler instead of restarting. `irq_armed`
  is set before every `SEI` and before the handler returns, and cleared by
  `irq_off()` and on the handler's entry, so that a jump to 0x0000 from
  code running with interrupts disabled restarts the firmware.

Without the first, `IRQ=1` fails within a few control transfers. With the
main loop polling the controller with interrupts disabled for some 60
cycles or more, as `IRQ=0` does, about one interrupt in 4000 came through
the reset vector; the safety net kept the firmware running then.

The result is `build/opensmi21.bin`: a plain image from address 0, padded
with 0xff to a multiple of 62 bytes, as the bootloader takes it. The build
fails if the image grows beyond 7502 bytes, the largest image known to
work, though program memory is 8 KB. `make` also runs `check.sh`, see
[Checks](#checks). Built with avr-gcc 7.3.0 and the defaults, version 0.1
is 3398 bytes, 3410 padded (55 upload units); with `IRQ=1`, 3780 bytes,
3782 padded (61 upload units). It also builds with avr-gcc 16.2.0,
binutils 2.47 and avr-libc 2.3.2 (Arch Linux), to 3298 bytes, 3348 padded
(54 upload units), and 3682 bytes with `IRQ=1`.

The Linux driver uploads it from `smi2021/opensmi21.bin` in the firmware
directory, the name it would have in linux-firmware:

    install -D -m 644 build/opensmi21.bin /lib/firmware/smi2021/opensmi21.bin

or upload it by hand to a device in bootloader mode, with the driver
unloaded:

    tools/bridge-probe/probe.py upload fw/build/opensmi21.bin

## Files

| File | Contents |
|------|----------|
| `main.c` | bring-up and main loop |
| `usb.c` | USB controller, control endpoint, standard requests, descriptors; with `IRQ=1`, the handler of vector 11 |
| `vendor.c` | vendor requests: modes, streaming engines, register access, i2c |
| `isr.S` | the handler of the vectors other than 11, which returns; with `IRQ=1`, the check for an interrupt that entered the reset vector |
| `pushguard.awk` | puts avr-gcc's `PUSH`es under `CLI`, see [Building](#building) |
| `regs.h` | register addresses from `docs/hardware.md` |
| `fw.h` | shared declarations, version, build options |
| `opensmi21.ld` | memory layout |
| `check.sh` | checks of the disassembly and the image |

## What is implemented

- **Core**: built with `-mmcu=atmega103` and avr-libc, as the core decodes
  the ATmega103's instruction set (`docs/hardware.md` "CPU core").
  avr-libc's startup code: 24 vectors of one `JMP` each, `SP` = 0x0fff,
  `.data` copied with the plain `ELPM`, `.bss` cleared, then `main()`.
  SRAM 0x0060–0x0fff; the linker script is the firmware's own, for the
  info block at 0x0060 and the limits. Only vector 11 is ever taken; the
  others return at once. The descriptors are constants in program memory,
  copied with avr-libc's `memcpy_P()`, which uses the plain `LPM`; the
  string descriptors are wide string literals, which are UTF-16LE on AVR,
  as USB wants them.
- **Bring-up**: the sequence of `docs/hardware.md` "Startup": `DDRA` = 0xff,
  status of both i2c engines 0, mode 3 prepared (0x1580 = 0x03, 0x1880 = 0),
  the USB controller set up with the documented values, `PC0` driven low,
  and the soft disconnect: 200 units of the original's delay loop of about
  160 cycles, 0x1500 bit 3 set, 2000 units, bit 3 cleared, waited out with
  avr-libc's `_delay_us()` as 1067 and 10667 µs, some 32000 and 320000
  cycles; only the order of magnitude matters.
- **USB events**: polled, as the original firmware does: the main loop and
  every wait of the control endpoint read 0x1503, which reading doesn't
  acknowledge, and record the event; the main loop handles it. With
  `IRQ=1`, vector 11 records the events instead, and the main loop only
  takes them, see [Building](#building). Setup packet (0x01 with 0x1502 bit 4: the handler
  clears the bit and withdraws a pending packet by clearing 0x151c bit 7),
  speed detected (0x80: maximum packet size of blocks A and B 0x0400 at high
  speed, 0x03ff otherwise), bus reset (0x81), suspend (0x85: `PC0` low,
  0x3080 bit 4, 0x1500 bit 2). Other codes are ignored. As the code stays
  until the next event, a reset, speed or suspend event is handled again
  on every pass until another one arrives.
- **Control endpoint**: setup packet from 0x2000, OUT data packets from
  0x2000 with their length from 0x1517 (a short packet ends the data
  stage, and the request is handled with the bytes that arrived), IN data
  packets of up to 64 bytes at 0x2040 with the length in
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
   identity block; the firmware identifies itself through its strings and
   its serial number instead.
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
   30 MHz, and counts as a failed try. Reads of more than one byte go to
   the engine as one transfer, as in the original, although on an EasyCAP
   002 the bytes after the first can be 0xff without a failure (see
   `docs/protocol.md` "I2C engines"). Splitting them into reads of one
   byte would avoid that, but was left out so that a host sees the same
   on both firmwares; the Linux driver only reads single bytes.
7. **Mode request**: modes other than 2 to 5 and a request with only one
   byte are stalled and leave the mode alone, so the mode reply never reads
   0x07, which hosts take for the bootloader. The inactive block is written
   first, then the active one, then block B +8/+12, then the engines, then
   0x1880. Endpoint +1 is written before the state bits in +0.
8. **Start of streaming**: the engine writes first, then 0x1600 bit 7.
   `SET_INTERFACE` to a non-zero setting starts again even if one is
   already selected, as the vendor sequence "mode 2, setting 2, then mode
   5, setting 2" needs.
9. **IN vendor requests** need `bRequest` 0x01; only the low byte of
   `wValue` is used. The recipient bits and `wIndex` are ignored for all
   vendor requests.
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
    Functionally the two are the same. `IRQ=1` only records an event
    earlier: in vector 11 as it arrives, where `IRQ=0` reads 0x1503 on
    every pass of the main loop, a few microseconds apart when idle, and
    in every wait of the control endpoint. Either way the main loop
    handles the event, so the host sees the same: 4000 control transfers
    take 1.4 s with either, and both pass the same tests with the Linux
    driver. The one case where `IRQ=1` records an event much earlier is a
    wait that doesn't poll the controller, that of an i2c engine (up to
    some 20 ms per try); the event is still handled after it. `IRQ=1`
    costs some 380 bytes and the workarounds of [Building](#building). It
    is there for what may need it later: a main loop that sleeps while
    idle, or work that takes long, such as decoding an IR remote, during
    which events are still recorded.

## Debugging

The test requests (`bRequest` 0xf0 to 0xf9: identity, instruction tests,
program memory read, interrupt census, timing, event log, crash record,
interrupt and return address records, interrupt window), their host
script `test-requests.py`, and the test builds (`ISR_BARE`, `ISR_EVENTS`,
`PUSH_GUARD=0`, the interrupt window) with which this firmware ran
experiments 8 to 12 of `docs/hardware.md` were removed once those were
done. The last commit that has them is "media: smi2021: upload
OpenSMI21 instead of a vendor firmware"; the diagnostic build for the
interrupts that entered the reset vector is in commit "fw: diagnostic
build for interrupts that enter the reset vector". To look at the
device now, the register access requests reach its whole data space, see
[`tools/bridge-probe`](../tools/bridge-probe/README.md).

## Checks

`check.sh`, run by `make`, checks the output of `avr-objdump -d`:

- the vector table: 24 `JMP`s at 0x0000–0x005c, vector 0 to the startup
  code at 0x0060, the others to their handler or to avr-libc's
  `__bad_interrupt`, which goes on to `__vector_default`;
- the startup code: `SP` set to 0x0fff, `.data` copied with exactly one
  plain `ELPM`;
- every `PUSH` and `rcall .` outside the interrupt handlers and the
  startup code's check for an interrupt that entered the reset vector
  (both run with I clear) follows a `CLI` or another such `PUSH`; it
  counts them;
- no `MUL`, `MULS`, `MULSU`, `FMUL*`, `MOVW`, `ELPM` with a register,
  `LPM Rd, Z(+)`, `SPM`, `SLEEP`, `WDR`, `EIJMP`, `EICALL`, `BREAK` or `DES`
  anywhere, and the plain `ELPM` only in the startup code;
- the image: a multiple of 62 bytes, at most 7502, starting with a `JMP`.

## Clean-room statement

This firmware was written from these sources only:

- `docs/protocol.md`, `docs/hardware.md` and `README.md` of this
  repository, the documents as of commit "tools: add a probe for the
  bridge's hardware" and the revision of both
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
- **Test requests** (since removed, see [Debugging](#debugging)): all of
  them worked. Experiments 8 to 11 gave: only the plain `ELPM` of the
  instructions tested works, program memory is 8 KB, the CPU clock is
  30.000 MHz; see `docs/hardware.md` "Experiments".
- **avr-libc**: built with avr-libc's startup code and delay loop instead
  of its own, the firmware passed the same tests again.
- **Linux driver**, with `IRQ=1`: steps 2 to 10 of
  `tools/hw-test/run-tests.sh` in PAL and NTSC (load, enumeration, probe,
  `v4l2-compliance`, video, audio, both, standard detection, input sweep)
  and all of `tools/hw-test/stress.sh` in NTSC (rebind, stream cycles,
  mode switches, a second user, the other ways of streaming). With the
  default build of avr-gcc 16, without the test code and uploaded by the
  driver: steps 4 to 10 in PAL. So register
  access, i2c, modes 2 and 5 and the alternate settings work as far as the
  driver uses them.
- **Alternate setting 0** (`ALT0_STOP=1`): stopping the engines when the
  host selects alternate setting 0, which the original firmware never
  does, lets them start again: `stress.sh` started and stopped video and
  audio 20 times each and switched modes as often, and every stream
  restarted.
- **Stalls**: `tools/hw-test/stall-test.py` sent 13 kinds of requests that
  must be refused, 20 times each: unknown vendor and standard requests, a
  missing descriptor, register accesses that break the bounds of design
  decision 3, a register access of 300 bytes, mode 7, `SET_FEATURE` on
  endpoint 0. All 260 were stalled, the valid requests after each one
  worked, the halt feature of 0x82 was set, reported and cleared, and the
  device never enumerated again.
- **OUT data stage**: with the length of each packet from 0x1517,
  `tools/hw-test/out-test.py` wrote 1 to 255 bytes into unused SRAM in
  requests of one to five packets, some padded, and read every one back
  unchanged; the stall test and the driver tests passed as before. No
  host at hand sends a short packet within the data stage, so ending it
  early is not tested.
- **Lost i2c writes**: with the Linux driver, about one write in a hundred
  to the decoder failed in every try and was lost, where
  `smi2021_3c.bin` lost none, and a lost write of the decoder's 0x08 left
  it at 60 Hz after a switch to PAL. The firmware cleared the status of
  the engine within some 10 cycles of the last poll; builds that clear it
  later, as the original does, lost none of 16000 writes, see
  `docs/hardware.md` "I2C engines". It now waits 1 µs and reads the
  status again before it clears it; that build, byte for byte, lost none
  of 16000 writes, left no register wrong in 100 switches of the
  standard, and passed steps 4 to 10 of the hardware tests in PAL.

## Untested and risks

Points most likely to need attention next:

- **Interrupts**: why an interrupt that waited long enters the reset
  vector is not known for certain; the safety net at the reset vector
  takes any that still do. `IRQ=0` stays the default.
- **Event handling inferred from the documents**: enumeration works, so
  clearing 0x1502 bit 4 by read-modify-write, the status stage of IN
  transfers that the hardware completes itself and the address change
  after the status packet are right as far as enumeration, the Linux
  driver and the stall test exercise them.
