# Bridge probe

`probe.py` runs the experiments of [Experiments](../../docs/hardware.md#experiments)
that need only the original firmware: through the firmware's register
access requests, the host can read and write the whole data space of the
bridge, run i2c transfers and switch modes, and that is enough to answer
most of the open questions about the hardware. Experiments 8 to 11 need a
test firmware and are not covered here.

> The experiments ran on an EasyCAP 002 with firmware 3c in November 2026;
> what they found is in [`docs/hardware.md`](../../docs/hardware.md).
> `test_probe.py` runs every experiment against an emulation of the
> documented firmware, which now includes the two ways the real device
> wedged during that run.

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
of finding out, and on the real device it is worse than that:

- Reading an address that nothing answers stops the firmware until the
  device is unplugged; control requests time out from then on. That was
  the first read of 0x1000, and a read of 30 bytes from 0x1600, while the
  single registers at 0x1600 and 0x1605 read fine. So even the blocks of
  known registers are not decoded throughout.
- An i2c command with bit 7 clear (0x20, 0x60) froze the first engine until
  the device was unplugged: its registers no longer took writes, and every
  transfer "succeeded" with stale data, so a scan found a device at every
  address.
- 0x64 written to 0x17a6 (+0x26 of the first engine) left the next i2c
  request and every one after it unanswered until the device was
  unplugged.

Before each step, the experiments write a breadcrumb to
`results/breadcrumb` and sync it, as `run-tests.sh` does, so after a wedged
device or board it tells which step it was. The breadcrumb is removed when
an experiment completes, and kept when it fails. A wedged device needs a
replug.

Every experiment writes `summary.txt`, `data.json` and its raw data to a
directory of its own in `results/`.

## Experiments

In this order, from the least to the most likely to upset the device:

    ./probe.py info
    ./probe.py exp timer
    ./probe.py exp avr
    ./probe.py exp i2c --buffer-end 0x3f
    ./probe.py exp modes
    ./probe.py exp endpoints
    ./probe.py exp i2c --commands

The survey is left out: see below.

### 1. `exp timer`: CPU clock and Timer0 prescaler

Runs Timer0 with each clock select value for a few seconds and samples
`TCNT0`. A rate is only used where the samples are close enough to unwrap
the 8 bit counter, which at a sample every 0.8 ms or so leaves the slow
settings. The host reads in steps of USB microframes, which makes the steps
of a counter that wraps between samples regular enough to pass for a slower
rate; so each setting is sampled again about a microframe slower, and a
rate that moves with the interval is an alias. The summary compares the
rates with the prescaler of the ATmega103 and of the ATmega16.

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

Don't run it on unknown ranges of a real device: a read of an address that
nothing answers stops the firmware, see [Safety](#safety). Of the blocks
around the known registers, 0x1580–0x15bf read fine and 0x1600–0x163f did
not.

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
is smaller; if all 32 do, repeat with `--buffer-end 0x3f`. On the real
device it is 32, and +0x25 to +0x29 are registers.

Then reads one byte with command 0xa0 from every address on both buses.
Expected on bus 0: the decoder at 0x25; whatever answers on bus 1 is new.

`--commands` also tries the read commands in `I2C_READ_CMDS` on register 0
of the decoder, next to the reference read with 0xe0, to learn what bits 2,
4 and 6 of a command do. Commands with bit 7 clear are left out, as they
froze the engine. After each command the decoder must still read as before
and an address without a device must still fail, or the experiment stops
there: the engine then needs a replug, and the later results would be
meaningless.

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

## A logic analyzer on the i2c bus

The measurements of [I2C engines](../../docs/protocol.md#i2c-engines)
were taken with an fx2lafw analyzer and sigrok on the Le Potato, with the
driver unloaded and `probe.py i2c` running the transfers:

- On the SAA7113H (QFP44) SDA is pin 23 and SCL pin 24; ground to the
  board's ground. On the analyzer SDA was on PB4 (D4) and SCL on PB6 (D6).
- A lead on SDA makes the 0xff second bytes and the lost 0x84 writes go
  away. To see them, connect SCL and ground only.
- The leads come off easily: a capture without a single edge means no
  contact, not an idle bus. Check before trusting a run.
- The analyzer shares the USB host with the EasyCAP: 2 and 4 MHz capture
  without loss for the first 6 s or so, 12 MHz for 3 s, and 24 MHz wrote
  no file.
- `sigrok-cli --scan` without `-d fx2lafw` probes every USB device, the
  EasyCAP included; don't.

```
sigrok-cli -d fx2lafw --config samplerate=4m \
    --channels D4=SDA,D6=SCL --time 5s -o cap.sr
sigrok-cli -i cap.sr -P i2c:scl=SCL:sda=SDA --protocol-decoder-samplenum
```

## Offline tests

    python3 -m unittest -v test_probe
