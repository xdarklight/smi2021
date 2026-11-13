#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Repeat the operations whose rare failures a single test run would miss:
# binding the driver, starting and stopping the streams, switching between
# the audio only and the capture mode, sharing the devices, and the less
# common ways of streaming. Each test checks what it captured with
# check-capture.py, and dmesg is checked for warnings at the end.
#
# The test signal must be playing, see README.md.
#
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
set -eu

here=$(cd "$(dirname "$0")" && pwd)
std=${SMI2021_STD:-ntsc}
connected=${SMI2021_INPUT:-0}
python=${SMI2021_PYTHON:-python3}
rounds=20
work=$(mktemp -d /tmp/smi2021-stress.XXXXXX)
trap 'rm -rf "$work"' EXIT

ALL='rebind video-cycles audio-cycles mode-switch exclusive variants'

usage() {
	cat <<EOF
usage: $0 [--rounds N] [TEST...]

Tests: $ALL (default: all)
Environment: SMI2021_STD (ntsc|pal), SMI2021_INPUT, SMI2021_PYTHON
EOF
	exit "${1:-0}"
}

tests=
while [ $# -gt 0 ]; do
	case $1 in
	--rounds)	rounds=$2; shift 2 ;;
	-h|--help)	usage 0 ;;
	-*)		echo "$0: unknown argument '$1'" >&2; usage 1 ;;
	*)		tests="$tests $1"; shift ;;
	esac
done
[ -n "$tests" ] || tests=$ALL

failures=0
fail() { printf 'FAIL: %s\n' "$*"; failures=$((failures + 1)); }
say() { printf '\n== %s\n' "$*"; }

video_device() {
	for sysdev in /sys/class/video4linux/video*; do
		case $(cat "$sysdev/name" 2>/dev/null) in
		smi2021*)	echo "/dev/$(basename "$sysdev")"; return 0 ;;
		esac
	done
	return 1
}

audio_device() {
	for card in /proc/asound/card*; do
		case $(cat "$card/id" 2>/dev/null) in
		*SMI2021*|*smi2021*)
			echo "hw:$(basename "$card" | tr -dc 0-9),0"; return 0 ;;
		esac
	done
	return 1
}

frame_rate() { [ "$std" = ntsc ] && echo 30 || echo 25; }

prepare() {
	dev=$(video_device) || { fail 'no video device'; return 1; }
	v4l2-ctl -d "$dev" --set-standard "$std" >/dev/null
	v4l2-ctl -d "$dev" --set-input "$connected" >/dev/null
	sleep 1
}

# check NAME ARGS...: run check-capture.py quietly, print its verdict
check() {
	name=$1; shift
	if "$python" "$here/check-capture.py" --std "$std" "$@" \
		> "$work/check.log" 2>&1; then
		return 0
	fi
	fail "$name"
	sed 's/^/    /' "$work/check.log" | grep -v -E '^    *$' | head -8
	return 1
}

capture() {
	v4l2-ctl -d "$dev" "${3:---stream-mmap}" --stream-count="$2" \
		--stream-to="$work/$1.uyvy" >/dev/null 2>&1
}

record() {
	adev=$(audio_device) || { fail 'no audio device'; return 1; }
	name=$1; secs=$2; shift 2
	arecord -q -D "$adev" -f S32_LE -c 2 -r 48000 -d "$secs" -t raw \
		"$@" "$work/$name.s32"
}

test_rebind() {
	say "unbinding and binding the driver, $rounds times"
	set -- /sys/bus/usb/drivers/smi2021/*:*
	[ -e "$1" ] || { fail 'smi2021 is bound to nothing'; return; }
	iface=$(basename "$1")
	i=0
	while [ "$i" -lt "$rounds" ]; do
		i=$((i + 1))
		echo "$iface" > /sys/bus/usb/drivers/smi2021/unbind
		sleep 0.5
		echo "$iface" > /sys/bus/usb/drivers/smi2021/bind
		n=0
		until video_device >/dev/null && audio_device >/dev/null; do
			n=$((n + 1))
			[ "$n" -lt 50 ] || break
			sleep 0.2
		done
		prepare || { fail "round $i: no device after binding"; continue; }
		capture rebind 25 || fail "round $i: capture failed"
		check "round $i" --video "$work/rebind.uyvy" || :
	done
	echo "$i rounds"
}

test_video_cycles() {
	say "starting and stopping video, $rounds times"
	prepare || return
	i=0
	while [ "$i" -lt "$rounds" ]; do
		i=$((i + 1))
		capture cycle 10 || fail "round $i: capture failed"
		check "round $i" --video "$work/cycle.uyvy" || :
	done
	echo "$i rounds"
}

test_audio_cycles() {
	say "starting and stopping audio, $rounds times"
	i=0
	while [ "$i" -lt "$rounds" ]; do
		i=$((i + 1))
		record cycle 1 || fail "round $i: arecord failed"
		check "round $i" --audio "$work/cycle.s32" || :
	done
	echo "$i rounds"
}

test_mode_switch() {
	say "audio throughout, video started and stopped $rounds times"
	prepare || return
	secs=$((rounds * 4 + 4))
	record switch "$secs" &
	audio_pid=$!
	sleep 2
	i=0
	while [ "$i" -lt "$rounds" ]; do
		i=$((i + 1))
		capture switch $(($(frame_rate) * 2)) || fail "round $i: capture failed"
		check "round $i, video" --video "$work/switch.uyvy" || :
		sleep 1
	done
	wait "$audio_pid" || fail 'arecord failed'
	# Every switch restarts the stream and leaves a gap; the audio must
	# still be complete and in step everywhere else.
	"$python" "$here/check-capture.py" --audio "$work/switch.s32" \
		> "$work/check.log" 2>&1 || :
	got=$(stat -c %s "$work/switch.s32")
	want=$((secs * 48000 * 8))
	gaps=$(grep -c 'at sample' "$work/check.log" || :)
	echo "audio: $got of $want bytes, $gaps discontinuities over $((rounds * 2)) switches"
	[ "$got" -eq "$want" ] || fail 'audio incomplete'
	# Each channel reports each gap, and a gap may span two blocks
	[ "$gaps" -le $((rounds * 2 * 2 * 2)) ] || fail 'more gaps than switches'
}

test_exclusive() {
	say 'a second user while streaming'
	prepare || return
	adev=$(audio_device) || { fail 'no audio device'; return; }
	v4l2-ctl -d "$dev" --stream-mmap --stream-count=$(($(frame_rate) * 6)) \
		--stream-to="$work/first.uyvy" >/dev/null 2>&1 &
	video_pid=$!
	record first 6 &
	audio_pid=$!
	sleep 2
	# v4l2-ctl exits with 0 even when it can not stream, so go by what it
	# says and by what it captured
	v4l2-ctl -d "$dev" --stream-mmap --stream-count=1 \
		--stream-to="$work/second.uyvy" > "$work/second.log" 2>&1 || :
	if grep -q 'REQBUFS.*busy' "$work/second.log" &&
	   [ ! -s "$work/second.uyvy" ]; then
		echo 'second video capture refused'
	else
		fail 'a second video capture was allowed'
		sed 's/^/    /' "$work/second.log" | head -4
	fi
	v4l2-ctl -d "$dev" --all >/dev/null 2>&1 ||
		fail 'querying the device while it streams failed'
	if arecord -q -D "$adev" -f S32_LE -c 2 -r 48000 -d 1 -t raw \
		/dev/null 2>/dev/null; then
		fail 'a second audio capture was allowed'
	else
		echo 'second audio capture refused'
	fi
	wait "$video_pid" || fail 'first video capture failed'
	wait "$audio_pid" || fail 'first audio capture failed'
	check 'first video' --video "$work/first.uyvy" || :
	check 'first audio' --audio "$work/first.s32" || :
}

test_variants() {
	say 'other ways of streaming'
	prepare || return
	capture userptr 50 --stream-user || fail 'USERPTR capture failed'
	check 'USERPTR video' --video "$work/userptr.uyvy" && echo 'USERPTR video'
	record mmap 3 -M || fail 'mmap arecord failed'
	check 'mmap audio' --audio "$work/mmap.s32" && echo 'mmap audio'
	record small 3 --period-size=128 --buffer-size=1024 ||
		fail 'arecord with the smallest periods failed'
	check 'smallest periods' --audio "$work/small.s32" && echo 'smallest periods'
	record large 3 --period-size=16384 --buffer-size=32768 ||
		fail 'arecord with the largest buffer failed'
	check 'largest buffer' --audio "$work/large.s32" && echo 'largest buffer'
}

dmesg -C 2>/dev/null || :
for t in $tests; do
	case $t in
	rebind|video-cycles|audio-cycles|mode-switch|exclusive|variants)
		"test_$(echo "$t" | tr - _)" ;;
	*)	fail "unknown test $t" ;;
	esac
done

say 'kernel log'
if dmesg | grep -i -E 'warn|error|fail|bug|oops|call trace' |
	grep -v -i -E 'status|saa7115 .*: (Audio|Video)'; then
	fail 'suspicious kernel messages'
else
	echo 'nothing suspicious'
fi

echo
echo "failures: $failures"
exit $([ "$failures" -eq 0 ] && echo 0 || echo 1)
