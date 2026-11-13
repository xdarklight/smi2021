#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the hardware tests in order of how likely they are to take the board
# down, so that a session can get through the safe ones unattended and stop
# before the ones that need somebody at the desk.
#
# Before every step a breadcrumb is written and synced. If the board stops
# answering, the breadcrumb of the last started step survives the reboot and
# says which one did it, even when no console was being watched. Any oops
# recorded by ramoops is collected from the previous boot on the next start.
#
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
set -eu

here=$(cd "$(dirname "$0")" && pwd)
results=${SMI2021_RESULTS:-$here/results}
breadcrumb=$results/breadcrumb
module=${SMI2021_MODULE:-$here/../../smi2021.ko}
std=${SMI2021_STD:-ntsc}
duration=${SMI2021_DURATION:-10}
python=${SMI2021_PYTHON:-python3}
# 2 is the EasyCAP 002, the board with four composite inputs labelled Video 1
# to Video 4 and a single audio connector.
card=${SMI2021_CARD:-2}
# Which of those connectors the composite cable is actually in, counted from
# 0, so that the input sweep knows which one should see a signal.
connected=${SMI2021_INPUT:-0}

attended=0
from=1
only=

STEPS='build load-unload enumerate probe compliance capture-video capture-audio
       capture-both switch-std input-sweep input-status
       unbind-while-streaming unplug-while-streaming'

usage() {
	cat <<EOF
usage: $0 [--from N] [--only N] [--risky] [--list]

  --list   number and name every step, then exit
  --from   start at step N (default: $from)
  --only   run just step N
  --attended
           also run the steps that need somebody at the desk: step 10 wants
           a cable pulled, and steps 11 and 12 can wedge the board. Without
           it the run stops before them.

Environment: SMI2021_RESULTS, SMI2021_MODULE, SMI2021_STD (ntsc|pal),
SMI2021_DURATION, SMI2021_CARD, SMI2021_INPUT, SMI2021_CONNECTOR,
SMI2021_AUDIO_OUT, SMI2021_PYTHON
EOF
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case $1 in
	--from)		from=$2; shift 2 ;;
	--only)		only=$2; shift 2 ;;
	--attended)	attended=1; shift ;;
	--list)		n=0
			for s in $STEPS; do
				n=$((n + 1))
				case $n in
				11)	tag=' (needs a cable pulled)' ;;
				1[23])	tag=' (can wedge the board)' ;;
				*)	tag= ;;
				esac
				printf '%2d  %s%s\n' "$n" "$s" "$tag"
			done
			exit 0 ;;
	-h|--help)	usage 0 ;;
	*)		echo "$0: unknown argument '$1'" >&2; usage 1 ;;
	esac
done

mkdir -p "$results"

say() { printf '\n== %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; failures=$((failures + 1)); }
failures=0

# A breadcrumb is only useful if it is on the disk before the step runs, not
# in the page cache when the board dies.
mark() {
	printf '%s step %s %s\n' "$(date -Is)" "$1" "$2" > "$breadcrumb"
	sync
}

harvest_pstore() {
	[ -d /sys/fs/pstore ] || return 0
	set -- /sys/fs/pstore/*
	[ -e "$1" ] || return 0
	stamp=$(date +%Y%m%d-%H%M%S)
	mkdir -p "$results/pstore-$stamp"
	for record in /sys/fs/pstore/*; do
		cp "$record" "$results/pstore-$stamp/" 2>/dev/null || :
	done
	echo "collected a crash record from the previous boot into" \
	     "$results/pstore-$stamp"
	echo "the last breadcrumb was: $(cat "$breadcrumb" 2>/dev/null ||
					 echo none)"
	# Left in place deliberately: clearing is a decision for whoever has
	# read it, not for the next run.
	echo "clear it with: rm /sys/fs/pstore/*"
}

# insmod does not load what the module depends on, and after a reboot nothing
# else may have loaded videobuf2-vmalloc, for one
load_module() {
	for dep in $(modinfo -F depends "$module" | tr , ' '); do
		modprobe "$dep" || { fail "could not load $dep"; return 1; }
	done
	insmod "$module" "card=$card" || { fail 'insmod failed'; return 1; }
}

video_device() {
	for sysdev in /sys/class/video4linux/video*; do
		[ -r "$sysdev/name" ] || continue
		case $(cat "$sysdev/name") in
		smi2021*)	echo "/dev/$(basename "$sysdev")"; return 0 ;;
		esac
	done
	return 1
}

audio_device() {
	for card in /proc/asound/card*; do
		[ -r "$card/id" ] || continue
		case $(cat "$card/id") in
		*smi2021*|*SMI2021*)
			echo "hw:$(basename "$card" | tr -dc 0-9),0"; return 0 ;;
		esac
	done
	return 1
}

# Frames in the given number of seconds, in the standard being tested
frames() {
	case $std in
	ntsc)	echo $(($1 * 30000 / 1001)) ;;
	*)	echo $(($1 * 25)) ;;
	esac
}

input_count() {
	v4l2-ctl -d "$1" --list-inputs | grep -c '^[[:space:]]*Input[[:space:]]*:'
}

# The driver only fills in the status of the current input; for any other one
# it leaves the field at zero, which V4L2 reads as a signal being present. So
# select an input before believing what it says about itself.
input_status() {
	v4l2-ctl -d "$1" --set-input="$2" >/dev/null 2>&1 || return 1
	# The decoder needs a moment to lock onto the new input
	sleep 1
	v4l2-ctl -d "$1" --list-inputs | awk -v want="$2" '
		/^[[:space:]]*Input[[:space:]]*:/ { index_seen = $NF }
		/^[[:space:]]*Status[[:space:]]*:/ && index_seen == want {
			print $3; exit
		}'
}

# Select the standard and the input to capture, and let the decoder lock
# onto them: the frames it sends before that are garbage
prepare_video() {
	device=$(video_device) || { fail "no smi2021 video device"; return 1; }
	v4l2-ctl -d "$device" --set-standard "$std" >/dev/null 2>&1 || :
	v4l2-ctl -d "$device" --set-input "$connected" >/dev/null 2>&1 || :
	sleep 1
}

capture_video() {
	device=$(video_device) || { fail "no smi2021 video device"; return 1; }
	out=$results/$1.uyvy
	timeout $((duration + 20)) v4l2-ctl -d "$device" \
		--stream-mmap --stream-count="$2" --stream-to="$out" \
		>/dev/null || { fail "$1: capture failed"; return 1; }
	echo "captured $(stat -c%s "$out") bytes to $out"
}

capture_audio() {
	device=$(audio_device) || { fail "no smi2021 audio device"; return 1; }
	out=$results/$1.s32
	timeout $((duration + 20)) arecord -D "$device" -f S32_LE -c 2 -r 48000 \
		-d "$duration" -t raw "$out" 2>/dev/null \
		|| { fail "$1: arecord failed"; return 1; }
	echo "captured $(stat -c%s "$out") bytes to $out"
}

check() {
	"$python" "$here/check-capture.py" --std "$std" "$@" \
		|| fail "check-capture reported problems"
}

run_step() {
	case $1 in
	build)
		say 'building the module'
		make -C "/lib/modules/$(uname -r)/build" \
			M="$(cd "$here/../.." && pwd)" \
			CONFIG_VIDEO_SMI2021=m modules || fail 'build failed'
		;;
	load-unload)
		say 'loading and unloading'
		# Start from a clean slate if an earlier run left it loaded
		! grep -q '^smi2021 ' /proc/modules || rmmod smi2021 ||
			fail 'could not unload the module that was loaded'
		modprobe saa7115 || fail 'saa7115 missing, CONFIG_VIDEO_SAA711X'
		load_module && { rmmod smi2021 || fail 'rmmod failed'; }
		;;
	enumerate)
		say 'firmware upload and re-enumeration'
		echo 'before:'; lsusb -d 1c88: || echo '  no 1c88 device'
		load_module
		sleep 5
		echo 'after:'; lsusb -d 1c88: || fail 'device did not re-enumerate'
		;;
	probe)
		say 'what probe registered'
		video_device || fail 'no video device'
		audio_device || fail 'no audio device'
		dmesg | tail -30
		;;
	compliance)
		say 'v4l2-compliance'
		device=$(video_device) || { fail 'no video device'; return; }
		v4l2-compliance -d "$device" || fail 'v4l2-compliance failed'
		;;
	capture-video)
		say 'video only'
		prepare_video && capture_video video "$(frames "$duration")" &&
			check --video "$results/video.uyvy"
		;;
	capture-audio)
		say 'audio only, with the video engine stopped'
		capture_audio audio && check --audio "$results/audio.s32"
		;;
	capture-both)
		say 'video and audio at the same time'
		# Video first, so that the audio joins the running capture
		# rather than making the driver restart the stream for it
		prepare_video || return
		capture_video both "$(frames $((duration + 2)))" &
		video_pid=$!
		sleep 1
		capture_audio both
		wait "$video_pid" || fail 'video capture failed'
		check --video "$results/both.uyvy" --audio "$results/both.s32"
		;;
	switch-std)
		say 'standard detection, and capturing in the wrong standard'
		device=$(video_device) || { fail 'no video device'; return; }
		# One standard of the detected family: V4L2_STD_NTSC_M, V4L2_STD_PAL_B
		case $std in
		ntsc)	other=pal; bit=0x1000 ;;
		*)	other=ntsc; bit=0x1 ;;
		esac
		detected=$(v4l2-ctl -d "$device" --get-detected-standard |
			   sed -n 's/^Video Standard = //p')
		if [ -n "$detected" ] && [ $((detected & bit)) -ne 0 ]; then
			echo "detected $detected for the $std source"
		else
			fail "detected '${detected:-nothing}' for the $std source"
		fi
		# The parser must cope with fields of the wrong length, by
		# returning the buffers, if as errors, rather than hanging
		v4l2-ctl -d "$device" --set-standard "$other" >/dev/null
		timeout 20 v4l2-ctl -d "$device" --stream-mmap --stream-count=50 \
			--stream-to=/dev/null >/dev/null 2>&1 ||
			fail "capturing in $other from a $std source did not finish"
		prepare_video && capture_video std "$(frames 4)" &&
			check --video "$results/std.uyvy"
		;;
	input-sweep)
		say 'which input sees the signal'
		device=$(video_device) || { fail 'no video device'; return; }
		count=$(input_count "$device")
		echo "the cable is said to be in input $connected of $count"
		was=$(v4l2-ctl -d "$device" --get-input | sed 's/.*: *//')
		seen=
		index=0
		while [ "$index" -lt "$count" ]; do
			status=$(input_status "$device" "$index") || status=
			# V4L2_IN_ST_NO_SIGNAL is 0x2.
			if [ -n "$status" ] && [ $(( status & 2 )) -eq 0 ]; then
				state='signal'
				seen="$seen $index"
			else
				state='no signal'
			fi
			printf '  input %s: %s (status %s)\n' "$index" "$state" \
				"${status:-unreadable}"
			index=$((index + 1))
		done
		v4l2-ctl -d "$device" --set-input="${was:-0}" >/dev/null 2>&1 || :
		# The labels on the connectors do not map to the decoder's inputs
		# in order, so this is a real check of that table: exactly the one
		# the cable is in should see the signal.
		case $seen in
		" $connected")	echo "  the expected input, and only that one" ;;
		'')		fail 'no input saw the signal' ;;
		*)		fail "signal on inputs$seen, expected only $connected" ;;
		esac
		;;
	input-status)
		say 'input enumeration and signal detection'
		device=$(video_device) || { fail 'no video device'; return; }
		v4l2-ctl -d "$device" --list-inputs
		echo 'unplug the composite cable, then press enter'
		read -r _
		v4l2-ctl -d "$device" --list-inputs
		echo 'plug it back in, then press enter'
		read -r _
		;;
	unbind-while-streaming)
		say 'rmmod while a capture is running'
		device=$(video_device) || { fail 'no video device'; return; }
		v4l2-ctl -d "$device" --stream-mmap --stream-count=100000 \
			>/dev/null 2>&1 &
		streamer=$!
		sleep 3
		rmmod smi2021 || fail 'rmmod during streaming failed'
		kill "$streamer" 2>/dev/null || :
		wait "$streamer" 2>/dev/null || :
		;;
	unplug-while-streaming)
		say 'physical disconnect while a capture is running'
		device=$(video_device) || { fail 'no video device'; return; }
		v4l2-ctl -d "$device" --stream-mmap --stream-count=100000 \
			>/dev/null 2>&1 &
		streamer=$!
		sleep 3
		echo 'unplug the capture device now, then press enter'
		read -r _
		kill "$streamer" 2>/dev/null || :
		wait "$streamer" 2>/dev/null || :
		dmesg | tail -40
		;;
	esac
}

harvest_pstore

number=0
for step in $STEPS; do
	number=$((number + 1))
	[ -z "$only" ] || [ "$only" = "$number" ] || continue
	[ -n "$only" ] || [ "$number" -ge "$from" ] || continue
	if [ "$number" -ge 11 ] && [ "$attended" -eq 0 ]; then
		echo
		echo "stopping before step $number ($step): pass --attended to" \
		     "run the steps that need somebody at the desk"
		break
	fi
	mark "$number" "$step"
	run_step "$step"
done

mark done -
echo
echo "failures: $failures"
exit $([ "$failures" -eq 0 ] && echo 0 || echo 1)
