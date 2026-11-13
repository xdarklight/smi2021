#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Play the test clip out of the board's composite output, for the capture
# device wired to it to record. mpv's DRM output drives a named connector
# directly, with no display server in the way, which also keeps the clip off
# HDMI when both connectors are attached.
#
# Composite has no hotplug detect, so the connector has to be forced on by the
# kernel command line; --list shows what the kernel ended up with.
#
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
set -eu

connector=${SMI2021_CONNECTOR:-}
audio=${SMI2021_AUDIO_OUT:-}
mode=
clip=
loop=inf

usage() {
	cat <<EOF
usage: $0 [--connector NAME] [--audio DEVICE] [--mode WxH] [--loop N] CLIP
       $0 --list

  --connector  DRM connector to drive, for example Composite-1. Defaults to
               \$SMI2021_CONNECTOR, else the first composite one found.
  --audio      mpv audio device, for example alsa/hw:0,1. Defaults to
               \$SMI2021_AUDIO_OUT, else mpv's own default.
  --mode       DRM mode to set, for example 720x480. Defaults to the size of
               the clip, so that an NTSC clip goes out as NTSC whatever mode
               the kernel command line picked.
  --loop       how often to repeat the clip (default: $loop)
  --list       show the DRM connectors and ALSA outputs, then exit
EOF
	exit "${1:-0}"
}

list_outputs() {
	echo "DRM connectors:"
	for card in /sys/class/drm/card*-*; do
		[ -e "$card/status" ] || continue
		printf '  %-16s %s\n' "$(basename "$card" | sed 's/^card[0-9]*-//')" \
			"$(cat "$card/status")"
	done
	echo
	echo "ALSA playback devices:"
	aplay -l 2>/dev/null | sed 's/^/  /' || echo "  aplay not available"
}

while [ $# -gt 0 ]; do
	case $1 in
	--connector)	connector=$2; shift 2 ;;
	--audio)	audio=$2; shift 2 ;;
	--mode)		mode=$2; shift 2 ;;
	--loop)		loop=$2; shift 2 ;;
	--list)		list_outputs; exit 0 ;;
	-h|--help)	usage 0 ;;
	-*)		echo "$0: unknown argument '$1'" >&2; usage 1 ;;
	*)		clip=$1; shift ;;
	esac
done

[ -n "$clip" ] || { echo "$0: no clip given" >&2; usage 1; }
[ -r "$clip" ] || { echo "$0: can not read '$clip'" >&2; exit 1; }

# The connector is named Composite-1 on some kernels and CVBS-1 on others,
# so find it rather than hard coding either.
if [ -z "$connector" ]; then
	for card in /sys/class/drm/card*-*; do
		name=$(basename "$card" | sed 's/^card[0-9]*-//')
		case $name in
		Composite-*|CVBS-*|TV-*)	connector=$name; break ;;
		esac
	done
fi
[ -n "$connector" ] || {
	echo "$0: no composite connector found, see --list" >&2
	exit 1
}

if [ -z "$mode" ]; then
	mode=$(ffprobe -v error -select_streams v:0 \
		-show_entries stream=width,height -of csv=s=x:p=0 "$clip")
fi

echo "$0: playing $clip on $connector in $mode" >&2

# mpv converts each frame for the DRM plane in software, and by default it
# does so with zimg, which on the Le Potato manages under 10 frames a second.
# The capture then sees every frame two or three times, which looks like a
# driver bug but is not. libswscale keeps up with room to spare.
set -- --no-config --really-quiet \
	--vo=drm --drm-connector="$connector" --drm-mode="$mode" \
	--loop-file="$loop" \
	--video-unscaled=yes --sws-allow-zimg=no --sws-scaler=point \
	--framedrop=no
[ -n "$audio" ] && set -- "$@" --audio-device="$audio"

exec mpv "$@" "$clip"
