# smi2021

Linux driver for Somagic SMI2021 based analog video capture devices, sold
under names such as EasyCAP DC60(+) and EasyCAP 002.

The files in the top level of this repository are the driver as it is meant
to be submitted upstream; they drop straight into
`drivers/media/usb/smi2021/` of a Linux tree.

## Hardware

Known devices are the EasyCAP DC60(+), with one composite and one S-Video
input, and the EasyCAP 002, with four composite inputs. Video is decoded by a
Philips SAA7113 or its GM7113C clone, both handled by the in-tree `saa7115`
driver. Audio is captured by a Cirrus Logic CS5340 and arrives interleaved
with the video on the same isochronous endpoint, so it is exposed as an ALSA
PCM capture device by this driver rather than by `snd-usb-audio`. The PCM
device also works on its own: with no video capture running the driver puts
the device into its audio only mode, which leaves the video engine stopped.

| USB ID                  | State                                          |
|-------------------------|------------------------------------------------|
| `1c88:0007`             | bootloader, waiting for a firmware upload      |
| `1c88:003c`–`1c88:003f` | running the firmware with the same number     |

The USB ID after the upload depends on the firmware, not on the board, so the
driver can not tell which inputs a board has. By default it offers all of
them: four composite inputs and S-Video. The input status of
`VIDIOC_ENUMINPUT` shows whether the current input carries a signal.

To offer only the inputs a board actually has, set the `card` module
parameter for it. Devices are numbered from 0 in the order in which they
appear; the kernel log shows each device's number.

| `card` | Inputs                                   |
|--------|------------------------------------------|
| 0      | all inputs (default)                     |
| 1      | EasyCAP DC60: composite and S-Video      |
| 2      | EasyCAP 002: Video 1 to Video 4, as labelled |

```bash
modprobe smi2021 card=2
```

Inputs are selected with the standard V4L2 input API, for example:

```bash
v4l2-ctl -d /dev/video0 --list-inputs
```

## Documentation

[`docs/protocol.md`](docs/protocol.md) specifies the USB protocol of the
device and what its firmware does with the bridge's hardware, as found by
analyzing the firmware images. It contains no code from the firmware, so
that it can serve as the basis for a driver or a replacement firmware
written without looking at the original.
[`docs/hardware.md`](docs/hardware.md) describes the inside of the bridge
for the authors of such a replacement: the processor, the memory map,
which of its blocks are standard AVR peripherals, how the USB controller
and the other blocks are programmed, and the experiments on hardware that
would settle what is still unknown.

## Building

### In a kernel tree

`tools/upstream-patch.py` turns this repository into the patch that adds the
driver to a Linux tree, including the `Kconfig`, `Makefile` and `MAINTAINERS`
hunks. It works in a temporary git worktree, so the tree itself is left
alone:

```bash
tools/upstream-patch.py ../linux /tmp/smi2021-patch
```

Apply the resulting patch with `git am`, enable
`CONFIG_MEDIA_ANALOG_TV_SUPPORT` and `CONFIG_VIDEO_SMI2021`, and build as
usual.

### Out of tree

The in-tree kbuild `Makefile` doubles as an external module Makefile; the
only thing it needs is the config symbol on the command line:

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD CONFIG_VIDEO_SMI2021=m modules
```

Dependencies that must be enabled in the target kernel: `VIDEO_DEV`, `I2C`,
`USB`, `SND`, `SND_PCM`, `VIDEOBUF2_VMALLOC` and `VIDEO_SAA711X`. The driver
follows the current kernel APIs, so older kernels may lack some of them.

## Firmware

These devices have no on-board flash. They enumerate as `1c88:0007` and need
a firmware upload before the capture device appears. By default the driver
uploads `/lib/firmware/smi2021_3c.bin`. The `firmware_version` module
parameter selects `smi2021_3e.bin` or `smi2021_3f.bin` instead:

```bash
modprobe smi2021 firmware_version=0x3f
```

## Upstream submission

Because the driver is not in mainline yet, the upstream posting is a single
"add new driver" patch, as was requested during the
[2013 review](https://lkml.iu.edu/hypermail/linux/kernel/1303.1/03761.html)
of the original submission. `tools/upstream-patch.py`, see above, generates
it with its commit message.

That patch carries no `Signed-off-by:`. Only the human submitter can certify
the Developer's Certificate of Origin, see
`Documentation/process/coding-assistants.rst`; add yours with `git am -s`
after reviewing it. The same document asks for an `Assisted-by:` tag
instead of the `Co-Authored-By:` used in this repository, and
`Documentation/process/generated-content.rst` asks to be transparent about
what a tool generated, which belongs in the cover letter. A draft, to be
adjusted to what you reviewed and tested:

> This driver is based on the out of tree driver by Jon Arne Jørgensen. An
> AI coding assistant (Claude, by Anthropic) rewrote most of it for the
> current kernel APIs, fixed the bugs it found, addressed the comments from
> the review of the 2013 submission, and analyzed the firmware images to
> name the register values the driver uses. It did so on a series of
> requests to prepare the driver for mainline, to review it against the
> current tree and the old review, to replace magic numbers, and to find out
> unknown values from the firmware. I reviewed the result and tested it on
> an EasyCAP 002 (...).

Before posting, the patch passes `checkpatch.pl --strict` apart from the
missing `Signed-off-by:`, and builds without warnings with `W=1`. Still to
do:

- Test on real hardware and attach `v4l2-compliance` output, which reviewers
  asked for in 2013. See [Testing on hardware](#testing-on-hardware) for the
  cases that cannot be covered without a device.
- Run sparse, which needs a version built from git, as the kernel requires
  `__typeof_unqual__` support that no sparse release has yet.
- Get the firmware blobs into `linux-firmware`. Their redistribution licence
  is not clearly established, which is the main open blocker.

## Testing on hardware

### Audio with and without video

The PCM device can be used with or without a video capture, and either of the
two may start and stop while the other one is streaming. Every such transition
restarts the isochronous stream in the other mode, so all four orders are
worth going through:

```bash
arecord -D hw:CARD=SMI2021 -f S32_LE -r 48000 -c 2 -d 10 audio-only.wav
```

```bash
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=250
```

```bash
arecord -D hw:CARD=SMI2021 -f S32_LE -r 48000 -c 2 -d 30 both.wav & \
	sleep 5 && v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=250
```

Starting or stopping a video capture while audio is running drops a few
milliseconds of audio, because the stream has to be restarted in the other
mode; the driver has to find the sample alignment again by itself, so the
recording must not be left with swapped channels or shifted samples.
Unplugging the device while either or both are streaming must not leave a
stuck process behind.

### PAL frames are accepted only at the full 288 lines per field

With the decoder settings the driver uses, the decoder flags 288 active lines
per field for 625 line standards and 242 for 525 line ones, see
[`docs/protocol.md`](docs/protocol.md). The driver captures 288 lines per field
for PAL and 240 of the 242 for NTSC, so NTSC tolerates a field that is up to
two lines short while PAL tolerates none: `smi2021_field_done()` wants the full
`720 * 2 * 288` bytes before it accepts a field, and a field that falls short
makes the frame come back with `V4L2_BUF_FLAG_ERROR`, or be dropped outright if
it was the first field of the frame.

Whether real hardware ever sends a short PAL field is untested. To find out:

1. Lock a stable PAL source on the selected input and check that the decoder
   agrees before capturing anything:

```bash
v4l2-ctl -d /dev/video0 --set-standard pal --get-standard --get-input --get-fmt-video
```

2. Capture a few hundred frames and count the buffers that came back flagged:

```bash
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=600 --verbose 2>&1 | tee pal.log
```

```bash
grep -c ERROR pal.log
```

3. Run the same capture with an NTSC source as the control, where the two
   spare lines should absorb a short field:

```bash
v4l2-ctl -d /dev/video0 --set-standard ntsc --stream-mmap --stream-count=600 --verbose 2>&1 | tee ntsc.log
```

4. Repeat the PAL run after each of the transitions that are the most likely
   to produce a short field, counting the flagged buffers every time:

   - switching the standard with `--set-standard` and streaming again
   - switching the input with `--set-input=N` between runs
   - pulling the cable out of the source and plugging it back in while
     streaming
   - starting the capture before the source has locked

Expected result: PAL and NTSC report the same, near zero, number of flagged
buffers, and buffers are only flagged while the signal is genuinely absent or
changing.

To see the number of lines the decoder actually sends, rather than infer it,
add a temporary `dev_dbg()` to `smi2021_parse_trc()` that prints
`buf->pos / SMI2021_BYTES_PER_LINE` where a field ends, and turn it on with:

```bash
echo 'file smi2021-v4l2.c +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
```

If PAL reports flagged buffers where NTSC does not, the exact 288 line
requirement is too strict. The fix is to allow a field to end a line or two
early, by giving `smi2021_field_done()` the same kind of tolerance that NTSC
gets for free, rather than by capturing fewer lines.

## Testing without hardware

`tools/parser-test/` builds a userspace harness that feeds synthetic
ITU-R BT.656 streams through the driver's frame parser and buffer handling,
extracted verbatim from `smi2021-v4l2.c`, and checks the resulting frames
byte for byte, including the 242 active lines per field that the decoder
sends for NTSC:

```bash
make -C tools/parser-test check
```

`tools/audio-test/` does the same for the audio ring buffer handling of
`smi2021-audio.c`: a clean stream has to arrive byte for byte with periods
counted exactly, data after lost bytes has to be sample aligned again, and
random corruption must never write outside the buffer, for buffer sizes
down to one frame:

```bash
make -C tools/audio-test check
```

## Credits

Jon Arne Jørgensen, Tony Brown, Michal Demin, Jeffry Johnston, David
Manouchehri, mastervolkov, and the stk1160 driver by Ezequiel Garcia.
