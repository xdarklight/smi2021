# smi2021

Linux driver for Somagic SMI2021 based analog video capture devices, sold
under names such as EasyCAP DC60(+) and EasyCAP 002.

The files in the top level of this repository are the driver as it is meant
to be submitted upstream; they drop straight into
`drivers/media/usb/smi2021/` of a Linux tree.

## Hardware

| USB ID      | Device        | Inputs                      |
|-------------|---------------|-----------------------------|
| `1c88:0007` | bootloader    | needs a firmware upload     |
| `1c88:003c` | EasyCAP DC60+ | 1x composite, 1x S-Video    |
| `1c88:003d` | EasyCAP DC60  | 1x composite, 1x S-Video    |
| `1c88:003e` | EasyCAP 002   | 4x composite                |
| `1c88:003f` | EasyCAP 002   | 4x composite                |

Video is decoded by a Philips SAA7113 or its GM7113C clone, both handled by
the in-tree `saa7115` driver. Audio is captured by a Cirrus Logic CS5340 and
arrives interleaved with the video on the same isochronous endpoint, so it is
exposed as an ALSA PCM capture device by this driver rather than by
`snd-usb-audio`.

Inputs are selected with the standard V4L2 input API, for example:

```bash
v4l2-ctl -d /dev/video0 --list-inputs
```

## Building

### In a kernel tree

Copy `*.c`, `*.h`, `Kconfig` and `Makefile` into
`drivers/media/usb/smi2021/`, then add to `drivers/media/usb/Kconfig`:

```
source "drivers/media/usb/smi2021/Kconfig"
```

and to `drivers/media/usb/Makefile`:

```
obj-$(CONFIG_VIDEO_SMI2021) += smi2021/
```

Enable `CONFIG_VIDEO_SMI2021` and build as usual.

### Out of tree

The in-tree kbuild `Makefile` doubles as an external module Makefile; the
only thing it needs is the config symbol on the command line:

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD CONFIG_VIDEO_SMI2021=m modules
```

Dependencies that must be enabled in the target kernel: `VIDEO_DEV`, `I2C`,
`USB`, `SND`, `SND_PCM`, `VIDEOBUF2_VMALLOC` and `VIDEO_SAA711X`.

## Firmware

These devices have no on-board flash. They enumerate as `1c88:0007` and need
a firmware upload before the capture device appears. Install the blob that
matches your device as `/lib/firmware/smi2021_3c.bin`, `smi2021_3e.bin` or
`smi2021_3f.bin`.

All variants report the same USB ID while in bootloader mode, so the driver
simply uploads the first firmware it finds. If more than one is installed,
pick one explicitly:

```bash
modprobe smi2021 firmware_version=0x3f
```

## Upstream submission

Because the driver is not in mainline yet, the upstream posting is a single
"add new driver" patch, as was requested during the
[2013 review](https://lkml.iu.edu/hypermail/linux/kernel/1303.1/03761.html)
of the original submission. In addition to the files here it needs the two
hunks shown under "In a kernel tree" above, and a `MAINTAINERS` entry:

```
SOMAGIC SMI2021 VIDEO CAPTURE DRIVER
M:	Martin Blumenstingl <martin.blumenstingl@googlemail.com>
L:	linux-media@vger.kernel.org
S:	Maintained
T:	git git://linuxtv.org/media.git
F:	drivers/media/usb/smi2021/
```

The commits here carry no `Signed-off-by:`; add yours with
`git commit --amend -s` before posting.

Still to do before posting:

- Test on real hardware and attach `v4l2-compliance` output, which reviewers
  asked for in 2013.
- Get the firmware blobs into `linux-firmware`. Their redistribution licence
  is not clearly established, which is the main open blocker.

## Testing without hardware

`tools/parser-test/` builds a userspace harness that feeds synthetic
ITU-R BT.656 streams through the driver's frame parser, extracted verbatim
from `smi2021_main.c`, and checks the resulting frame byte for byte:

```bash
make -C tools/parser-test check
```

## Credits

Jon Arne Jørgensen, Tony Brown, Michal Demin, Jeffry Johnston, David
Manouchehri, mastervolkov, and the stk1160 driver by Ezequiel Garcia.
