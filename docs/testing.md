# What still needs testing

The tests on hardware that are still to do, as of November 2026, on an
EasyCAP 002 on a Le Potato with the rig of
[`tools/hw-test`](../tools/hw-test/README.md). What has been tested is in
the commit messages, [`fw/README.md`](../fw/README.md) "On hardware" and
[Experiments](hardware.md#experiments); the questions that no test is known
for yet are in [Open questions](protocol.md#open-questions).

## Regression on the current tree

Done on 2026-11-15 with the driver of 1349e8e and the default build of
OpenSMI21 (`IRQ=0`), uploaded by the driver: in PAL and in NTSC,
`run-tests.sh` steps 1 to 10, `v4l2-compliance -s` (132 of 132, no
warnings), all of `stress.sh` with 20 rounds and `dmabuf-test.py`;
`stall-test.py` and `out-test.py` once. See [Long runs](#long-runs) for
the hour of each. Nothing open here until the driver or the firmware
changes again.

`IRQ=1` passed steps 2 to 10 in both standards and `stress.sh` in NTSC,
but not with the driver as it is now; it is not the default, so this is
only worth repeating before it becomes one.

## Decoder writes that get lost

The driver doesn't look at the reply to an i2c write, so a write that the
firmware reports as failed goes unnoticed. With OpenSMI21, about one write
in a hundred fails that way; with `smi2021_3c.bin`, none of 9600 did, see
[I2C engines](protocol.md#i2c-engines).
When it is the write of the decoder's register 0x08 in a switch to PAL,
the decoder stays at 60 Hz: the input reports no signal, every buffer
comes back with `V4L2_BUF_FLAG_ERROR`, and as S_STD to the standard that
is set does nothing, only binding the driver again gets it out. Seen in 6
of 100 switches from NTSC to PAL straight after a switch to NTSC.

Still to do:

- Find which difference of OpenSMI21's i2c transfers from the original
  ([I2C engines](hardware.md#i2c-engines)) loses the writes, and makes
  reads fail more often (53 tries in 1000 against none in 947). The order
  of the steps is the same; OpenSMI21 differs in that it
  - starts the next try some 24 cycles (0.8 µs) after it cleared the
    status of a failed one, where the original takes some 100 (3.3 µs);
    at 100 kHz, the bus should be free for 4.7 µs between a stop and the
    next start, which the engine may or may not see to itself;
  - checks bit 5 in the value of its last poll instead of reading the
    status again;
  - polls the status every 9 cycles or so instead of 12;
  - makes 17 tries instead of 16, and gives up waiting after 50000 polls;
  - reports a failed write in the reply, which the original doesn't.

  The first is the likeliest cause of a write whose every try fails. The
  second could let a try pass before the engine sets bit 5, which the
  clearing of the status may then not catch, so that the next transfer
  starts with a failure; that would fit the more frequent failed first
  tries. Each variant needs a replug to upload, then the write test of
  2026-11-15.
- Read the reply after each write in the driver, and retry a failed one,
  as reads do. With the vendor's images that reports nothing (see
  [Register access requests](protocol.md#register-access-requests)), so
  reading the register back is the check that works with both.

## With somebody at the board

Done on 2026-11-15 with the driver and the firmware of the regression
above:

- Step 11, the composite cable pulled for some seconds while video and
  audio streamed, in NTSC and in PAL: the input status followed, no
  buffer was flagged, no frame was dropped, and the audio had no gap at
  the pulls.
- A capture started without a cable, which was then plugged in, in NTSC
  and in PAL: no buffer flagged.
- Step 12, unbinding while streaming video, audio only, and both: every
  process ended within 10 s, and binding again brought both devices back.
- Step 13, unplugging while streaming video and audio with the video
  device held open by another process, while streaming video, and while
  streaming audio only: nothing was left stuck, and the kernel log had
  nothing but the disconnect.
- With the video device held open after the unplug, register reads of the
  decoder through `i2c-dev` failed cleanly (`i2cget`; `i2cdetect` needs
  transfers the adapter doesn't offer), and both devices went once it was
  closed.
- The transitions of "PAL frames are accepted only at the full 288 lines
  per field" in [`README.md`](../README.md): a switch of the standard, of
  the input to each of the others and back, the cable pulled and a capture
  started without a signal, in PAL: no buffer flagged, apart from the lost
  writes above.

## Long runs

The second soak in NTSC, on 2026-11-13, lost 37 frames in four bursts
around its 40th minute: the frames arrived, but no buffer was queued, so
userspace stalled for 0.2 to 0.4 s. On 2026-11-15, with the driver and
OpenSMI21 of the regression above, in NTSC:

- An hour of `v4l2-ctl --stream-mmap=4 --verbose --stream-to /dev/null`,
  with every gap in the sequence numbers flagged and a loop beside it
  that slept 1 ms and reported every wakeup more than 50 ms late: 107890
  frames, no gap, no late wakeup.
- An hour of `soak.py`: 107892 buffers, no frame dropped, 3596 s of audio
  with a discontinuity only where the clip loops, memory flat, nothing in
  the kernel log.

So neither the board nor the reader stalls now, and what did then is not
known. A soak in PAL, and one that drops frames again, are what is left;
the latter would want `trace-cmd` around the gaps.

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
