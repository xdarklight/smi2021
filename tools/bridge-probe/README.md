# Bridge probe

`probe.py` runs the experiments of [Experiments](../../docs/hardware.md#experiments)
that need only the original firmware: through the firmware's register
access requests, the host can read and write the whole data space of the
bridge, run i2c transfers and switch modes, and that is enough to answer
most of the open questions about the hardware. Experiments 8 to 11 need a
test firmware and are not covered here.

> These scripts have not been run against hardware yet. `test_probe.py`
> runs every experiment against an emulation of the documented firmware,
> which shows that they work and draw the right conclusions for such a
> device, but not how the real one answers.

## Setup

On the board, besides what [`../hw-test`](../hw-test/README.md) needs:

    pacman -S --needed python-libusb1

The tool talks to the device through libusb, so the kernel driver must let
go of it first. The firmware stays in the device until it loses power, so:

1. Load the driver and capture a few frames, which uploads the firmware and
   sets up the decoder; the experiments with streams need a configured
   decoder to see video.
2. `rmmod smi2021`.
3. Run `probe.py` as root, or give your user access to the device with a
   udev rule.

`probe.py upload firmware/smi2021_3f.bin` uploads a firmware itself, to a
device in bootloader mode with the driver unloaded; for example to repeat
the experiments with `smi2021_3f.bin`, after a replug.

## Safety

The experiments only change state they restore afterwards, and leave out
what is known to have side effects:

- 0x1503, the event code of the USB controller, is never read, as reading it
  presumably acknowledges an event.
- The command registers of the i2c engines, 0x1780 and 0x3180, are only
  written as part of an i2c transfer, and the USB controller only by the
  endpoint experiment; `bridge.py` refuses anything else.
- No interrupt enable bit is written, as the firmware has no handlers, and
  neither are `MCUCR`, `MCUSR`, `WDTCR` and `XDIV`, which control sleep,
  external memory, the watchdog and the clock.

Reading unknown registers can still have side effects, which is the point
of finding out. Before each step, the experiments write a breadcrumb to
`results/breadcrumb` and sync it, as `run-tests.sh` does, so after a wedged
device or board it tells which step it was. The breadcrumb is removed when
an experiment completes. A wedged device needs a replug.

Every experiment writes `summary.txt`, `data.json` and its raw data to a
directory of its own in `results/`.

## Experiments

In this order, from the least to the most likely to upset the device:

    ./probe.py info
    ./probe.py exp timer
    ./probe.py exp avr
    ./probe.py exp i2c --commands
    ./probe.py exp modes
    ./probe.py exp endpoints
    ./probe.py exp survey --range 0x1000-0x14ff
    ./probe.py exp survey --range 0x1580-0x3fff
    ./probe.py exp survey --range 0x4000-0xffff

### 1. `exp timer`: CPU clock and Timer0 prescaler

Runs Timer0 with each clock select value for a few seconds and samples
`TCNT0`. A rate is only used where the samples are close enough to unwrap
the 8 bit counter, which at a sample every half millisecond leaves the
slow settings. The summary compares the rates with the prescaler of the
ATmega103 and of the ATmega16.

Expected: the ATmega103 table fits with a deviation of a few percent at
most, and gives a CPU clock of about 30 MHz. If the ATmega16 table fits
instead, `docs/hardware.md` is wrong about the core.

### 2. `exp survey`: the data space

Reads the given range twice, leaving out the USB controller and anything
passed with `--exclude`, and saves both passes. The summary classifies each
256 byte page as constant, stable, changing between the passes, or a copy
of another page, and searches for the start of each firmware image, for a
piece of its code and for USB descriptors. A hit outside SRAM shows where
program memory or the bootloader is visible in the data space.

`--write-test RANGE` also flips and restores every byte of a range, to find
memory. Only use it on ranges the first passes showed to be quiet.

### 3. `exp avr`: other ATmega103 peripherals

Dumps the I/O registers 0x20 to 0x5f, then writes and restores those of the
registers in `AVR_WRITE_TESTS` that can be changed without consequences:
the compare and counter registers of the timers, `UBRR`, `ADMUX`, `SPCR`
(without enabling the SPI), the EEPROM address and data, and `RAMPZ`. A
register that keeps the written value exists. `SPCR` existing makes it
likely that the SPI controller Somagic advertises is the ATmega103's;
`RAMPZ` existing makes `ELPM` likely.

### 4. `exp i2c`: buffers, buses and commands

Writes a pattern into the data buffer of both i2c engines, from +5 up to
`--buffer-end` (default +0x24, 32 bytes), reads it back and restores it.
The number of bytes that keep the pattern is the size of the buffer, if it
is smaller; if all 32 do, repeat with `--buffer-end 0x3f`.

Then reads one byte with command 0xa0 from every address on both buses.
Expected on bus 0: the decoder at 0x25; whatever answers on bus 1 is new.

`--commands` also tries the read commands in `I2C_READ_CMDS` on register 0
of the decoder, next to the reference read with 0xe0, to learn what bits 2,
4, 6 and 7 of a command do.

### 5. `exp endpoints`: endpoint register blocks

First dumps the USB controller registers, without 0x1503, after the mode
request of each mode. Then starts mode 5 with the endpoint register blocks
changed after the mode request, for each variant, and captures for a
second:

| Variant | A: +1, state | B: +1, state | Favoured reading | Opposite reading |
|---------|--------------|--------------|------------------|------------------|
| `baseline` | 0x15, 01 | 0x25, 00 | chunks | chunks |
| `both-active` | 0x15, 00 | 0x25, 00 | chunks | nothing |
| `swap-states` | 0x15, 00 | 0x25, 01 | nothing | nothing |
| `swap-numbers` | 0x25, 01 | 0x15, 00 | nothing | nothing |
| `swap-both` | 0x25, 00 | 0x15, 01 | nothing, or raw audio | nothing, or raw audio |

The favoured reading is the one of [Endpoint register
blocks](../../docs/hardware.md#endpoint-register-blocks): bits 7 to 4 of +1
are the endpoint number, state 00 is normal operation and 01 disables the
endpoint, and block B carries the stream of mode 5. `both-active` tells the
two readings apart. "Raw audio" means 48 byte packets without chunk magic,
as in mode 2, which would show that block A is wired to the audio engine.

### 6. `exp modes`: what each mode streams

Captures modes 2 and 5 as a reference, and modes 3 and 4, at both
alternate settings. The summary gives packet lengths, chunk magic, byte
statistics, and how often 0x47 recurs every 188 bytes, as it would in an
MPEG transport stream. Nothing feeds the transport stream or GPS inputs of
an EasyCAP, so empty packets or constant data in modes 3 and 4 are likely;
what matters is whether, and how, they differ.

### 7. `gpio`: where the pins go

For tracing the pins with a multimeter, with the driver unloaded:

    ./probe.py gpio PA7
    ./probe.py gpio PA7 --toggle 2 --count 10
    ./probe.py gpio PC0 --set 1
    ./probe.py gpio PC4 --input

`--toggle` makes the pin an output and flips it every given number of
seconds. Check what a pin is connected to before driving it: the
candidates are the reset inputs of the decoder and of the audio ADC.

## Other commands

`read`, `write`, `mode`, `i2c` and `capture` do single steps by hand, and
`analyze` prints the statistics of a capture file again. `--unsafe` lifts
the guards of `bridge.py`.

## Offline tests

    python3 -m unittest -v test_probe
