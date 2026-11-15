# What still needs testing

The tests on hardware that are still to do, as of November 2026, on an
EasyCAP 002 on a Le Potato with the rig of
[`tools/hw-test`](../tools/hw-test/README.md). What has been tested is in
the commit messages, [`fw/README.md`](../fw/README.md) "On hardware" and
[Experiments](hardware.md#experiments); the questions that no test is known
for yet are in [Open questions](protocol.md#open-questions).

## Regression on the current tree

The driver has changed since most of its tests ran: one lock for the
ioctls and the queue (e571a20), register accesses refused once the device
is gone (77c99d9), the stream stopped before the teardown (f8e363b), the
delays of the vendor's initialization (eaaa164), the board identified by
its straps (44d1cfe) and OpenSMI21 uploaded instead of a vendor firmware
(d144698). With all of them, and the default build of OpenSMI21
(`IRQ=0`), only steps 4 to 10 of `run-tests.sh` in PAL have run. Still to
run, with the driver and the firmware as they are:

- `run-tests.sh` steps 1 to 10 in NTSC, and 1 to 3 in PAL.
- `v4l2-compliance -s` in both standards, which adds the streaming and
  read/write tests to step 5; this is the output to attach to the
  upstream posting.
- All of `stress.sh` in PAL and in NTSC. The rebind test runs the new
  initialization delays and the board identification each round.
- `dmabuf-test.py`.
- `soak.py` for an hour, see [Long runs](#long-runs).

`IRQ=1` passed steps 2 to 10 in both standards and `stress.sh` in NTSC,
but not with the driver as it is now; it is not the default, so this is
only worth repeating before it becomes one.

## With somebody at the board

These need the cable or the device pulled, or can take the board down.
They all ran before f8e363b, which changed what the disconnect and the
unbind do, so they need to run again:

- Step 11, the composite cable pulled while video and audio stream.
- Step 12, unbinding while streaming, in the capture mode and in the
  audio only mode.
- Step 13, unplugging while streaming, in the capture mode and in the
  audio only mode, and with both streams running. No process may be left
  stuck, and the kernel log must stay free of oopses and of "urb resubmit
  failed".
- New with 77c99d9: with `i2c-dev` loaded and the video device held open
  (for example `v4l2-ctl --sleep-forever`), unplug the device and run
  `i2cdetect -y N` on the smi2021 adapter. Every access must fail, without
  an oops, and closing the video device afterwards must free everything.
- The transitions of "PAL frames are accepted only at the full 288 lines
  per field" in [`README.md`](../README.md): count the buffers flagged
  with `V4L2_BUF_FLAG_ERROR` in PAL after switching the standard,
  switching the input, pulling the cable while streaming and starting the
  capture before the source has locked, against the same in NTSC. A
  steady PAL source already delivers every frame (step 6), so only the
  transitions are open.

Each needs a replug at most; batch them.

## Long runs

The second soak in NTSC lost 37 frames in four bursts around its 40th
minute: the frames arrived, but no buffer was queued, so userspace stalled
for 0.2 to 0.4 s. Whether `soak.py` (a Python reader on a pipe from
`v4l2-ctl`) or the board stalled is open:

1. Capture for an hour to `/dev/null` with `v4l2-ctl --stream-mmap
   --verbose --stream-to /dev/null`, flag every gap in the sequence
   numbers, and run a loop beside it that sleeps 1 ms and reports every
   wakeup more than 50 ms late. Ten minutes of that in PAL with four
   buffers were clean.
2. If the hour is clean as well, the reader is at fault: give the
   `v4l2-ctl` of `soak.py` more buffers (`--stream-mmap=N`, an option of
   `soak.py` to be added) and run the soak again in both standards.
3. If the hour drops frames too, find what stalls the board, for example
   with `trace-cmd` around the gaps.

The source of the test signal also slips: mpv shows a frame for one field
and the next for three, for minutes on end. `soak.py` tells this from a
drop, but a clean run needs a restart of `play-signal.sh` when it starts.

## With other hardware

- **An independent signal source**: source and capture share a clock on
  the Le Potato, so drift of the audio sample rate against the device, and
  any fault of the resync that follows from it, cannot show. A drop of
  frames is also worth confirming against a source that does not share
  memory bandwidth with the USB host. One soak against a DVD player or a
  camera would do.
- **An EasyCAP DC60**: the board identification takes a board with `PC2`
  low for it, with composite and S-Video, inferred from the Windows
  driver. To test: the board found, the inputs offered, the S-Video input
  in the input sweep, and the inputs in the right order.
- **Other boards**: one with a GM7113C instead of the SAA7113H, which the
  `saa7115` driver handles apart; one whose straps (bits 7-6 of 0x31a5 at
  `10`) mark another decoder, for which the driver offers all inputs.
- **A full speed host**, or a USB 1.1 hub in between: OpenSMI21 offers
  1023 bytes every frame then, which is too little for video. What the
  driver does there is untested; it should at least fail cleanly.
- **A host that can suspend**: the Le Potato cannot. On suspend, OpenSMI21
  drives `PC0` low and sets bit 4 of 0x3080, and the Windows driver runs
  the initialization again when it finds that bit set; this driver has no
  suspend or resume handling. Whether streaming works after a resume, and
  whether the driver has to initialize the device again, is not known.
- **A scope on the decoder's I2C bus**: whether SDA rises before SCL has
  fallen when the bridge releases its acknowledge in a read of two bytes,
  and why writes with 0x84 are lost; a logic analyzer can't show it, as
  its lead on SDA makes both go away. See
  [I2C engines](protocol.md#i2c-engines); the hookup is in
  [`tools/bridge-probe`](../tools/bridge-probe/README.md#a-logic-analyzer-on-the-i2c-bus).
- **The cheap USB cable of 2026-11-13**, on a port of a hub: whether the
  one in five failed reads of 0xe0 of that day come back with it. Only
  worth it if the retries of the driver are to go.
- **A multimeter or scope**, experiment 7 of [Experiments](hardware.md#experiments):
  where `PC0` and `PC4` go (neither chip's reset nor the LED), and `PC1`,
  which was not driven.

## Firmware

What OpenSMI21 implements but no test reaches yet:

- A short packet within the data stage of an OUT request, which ends it
  early. No host at hand sends one.
- Modes 3 and 4, which send nothing on an EasyCAP, as with the original;
  programming the PID filters of mode 3 and reading them back would at
  least confirm the registers.
- Alternate setting 0 with `ALT0_STOP=0`, as in the original; the Linux
  driver stops the engines itself, so it should not matter.
- Images over 7502 bytes, the largest known to work, though program memory
  is 8 KB, and `SPM`, the one instruction of experiment 8 not tried. Both
  only matter once the firmware outgrows the limit.
- Why an interrupt that waited long with interrupts disabled enters the
  reset vector. `IRQ=1` avoids it, and has a safety net for it, but the
  cause is not known.
