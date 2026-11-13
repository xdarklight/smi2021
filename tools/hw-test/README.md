# Hardware tests

These drive the driver against a real capture device on a LibreComputer Le
Potato (AML-S905X-CC), using the board's own composite output as the signal
source. The board plays a test clip out of its CVBS and analog audio jack,
the capture device records it back over USB, and the capture is checked
against what was sent. One board, one cable, no second signal source.

The offline tests in `../parser-test` and `../audio-test` cover the parser
and the audio ring buffer against synthetic input. These cover what those
can not: real isochronous timing, the firmware upload, probe and teardown,
and whether a frame that went in comes back out.

> These scripts have not been run against hardware yet. The pattern
> generator and `check-capture.py` are tested, including against captures
> with faults injected into them; everything that talks to the board is not.

## Wiring

The TRRS jack carries composite video and stereo audio, so one breakout
cable feeds both inputs of the capture device. Pin order on these jacks
varies, and Amlogic's is its own, so check the breakout before blaming the
driver for a black picture.

The device under test is the EasyCAP 002: four composite inputs labelled
Video 1 to Video 4, and a single audio connector. `run-tests.sh` therefore
loads the driver with `card=2`, and `SMI2021_INPUT` says which of the four
connectors the cable is in, counted from zero.

Because there is one audio connector and the CS5340 is a stereo ADC, the
two channels of the ALSA device cannot be independent. Whether the one input
ends up duplicated onto both, or on one with the other silent, is a property
of the board. `check-capture.py` measures it and says which it found instead
of assuming, and only looks for gaps in channels that carry a tone, so a mono
or single channel wiring is not reported as a fault. `--audio-expect stereo`
or `mono` turns the finding into a requirement where that is wanted.

Only the original AML-S905X-CC has this. The V2, sold as Sweet Potato,
dropped both the composite video and the analog audio output.

## Setup

Packages, on Arch:

    pacman -S --needed base-devel bc v4l-utils alsa-utils ffmpeg mpv \
        libdrm sox usbutils python-numpy python-pillow tmux rsync

plus the `-headers` package matching the running kernel. `wireshark-cli` and
`trace-cmd` are worth having to check the device's traffic against
`../../docs/protocol.md`.

The kernel needs `CONFIG_VIDEO_SAA711X` for the decoder, `CONFIG_PSTORE_RAM`
for crash capture and `CONFIG_USB_MON` to watch the bus.

Composite has no hotplug detect, so the connector has to be forced on from
the kernel command line; `./play-signal.sh --list` shows what the kernel
settled on, and the connector is named `Composite-1` on some kernels and
`CVBS-1` on others.

The analog output of the board is the internal ACODEC behind the AIU, and
it needs `AIU ACODEC OUT EN` on, `AIU ACODEC SRC` set to `I2S`, a volume
above 0, and `AIU ACODEC I2S Lane Select` at 0. A stereo stream is on the
first I2S lane, so any other lane plays silence; a capture then shows only
a 25 Hz hum, which is the video crosstalking into an audio input with
nothing on it.

    amixer -c 0 sset 'AIU ACODEC I2S Lane Select' 0

### Getting an oops back without a console

With the serial console on a machine the test runner can not read,
`/sys/fs/pstore` is the way a crash gets reported, since it survives the
reboot and is an ordinary file read afterwards. It wants a `reserved-memory`
node with compatible `ramoops`, and `panic=10` on the command line so that a
panic reboots itself rather than waiting for somebody to notice.

That combination makes most crashes self service: panic, reboot, read the
record, carry on. It has one limit worth knowing before trusting it. The
records live in DRAM, so they survive the warm reboot that `panic=10` does,
but a cold power cycle probably scrubs them. A hang that needs the power
pulled is therefore the case where the console on the other machine is still
the only witness, and `results/breadcrumb` names the step that caused it.

## Running

Build the clip once per standard. It is played on a loop, so a minute is
plenty:

    ./make-pattern.sh --std ntsc
    ./make-pattern.sh --std pal

Play it, in its own `tmux` window so it survives an SSH drop:

    ./play-signal.sh pattern-ntsc.mkv

Then, in another window:

    ./run-tests.sh --list
    ./run-tests.sh

The run stops before step 10. Steps 10 to 12 need somebody at the desk --
step 10 wants the composite cable pulled, and steps 11 and 12 are the ones
that can take the board down -- so they only run with `--attended`. After a
crash, `--from N` picks up where the breadcrumb says it stopped.

A capture can also be checked on its own:

    ./check-capture.py --video results/both.uyvy --audio results/both.s32

### The input sweep

Step 10 is worth more on this board than on the two input one. The labels on
the connectors do not map to the decoder's inputs in order -- Video 1 is
`COMPOSITE2` and Video 3 is `COMPOSITE0` -- so with the cable in a known
connector, sweeping the inputs checks that table against the hardware:
exactly the one the cable is in should see a signal.

The sweep selects each input before reading its status, which it has to do.
The V4L2 specification defines `status` as only valid for the current input,
and the driver fills it in for that one alone, since that is all the decoder
can report. For the others the field stays zero -- and zero is what V4L2
uses for a signal being present. Reading `--list-inputs` without selecting
first would say all four have signal.

## What the pattern proves, and what it does not

The frame counter is a row of binary blocks rather than rendered digits, so
decoding it needs a luma threshold instead of OCR and survives a soft analog
picture. It is decoded separately for each interlaced field, which catches
fields woven from the wrong pair or in the wrong order -- something a whole
frame comparison cannot see.

The frame sequence is judged on the first field. The second one may show the
next source frame instead of the same one: the vblank of an interlaced mode
comes once per field, so the player's page flip can land between the two.
That is reported as a note. A second field showing an older frame cannot
come from the source and is a failure.

`play-signal.sh` turns off zimg for mpv's software conversion. With it, mpv
manages under 10 frames a second on this board, and a capture of that shows
every frame two or three times, which is easy to mistake for a driver bug.

The two channels carry different tones. On a board with stereo input that
shows up a swap of left and right; on this one it identifies which channel
the single connector feeds. Both divide 48 kHz exactly, so a gap in the audio
appears as a jump in phase. Where a gap is, is exact. How big it is, is only known modulo one
period of the tone: a 100 sample gap and a 4 sample gap look alike at 1 kHz.

The blind spot is the loop itself. Source and capture share one board and one
clock, so sample rate drift between the device and the signal source, and any
resync bug that follows from it, cannot appear here. That needs one pass
against an independent source. The same goes for timing: the VPU generating
the signal and the USB capture share memory bandwidth, so a dropped frame is
worth confirming against an external source before it is called a driver bug.
