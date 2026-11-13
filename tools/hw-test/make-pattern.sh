#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the test clip that the board plays out of its composite output for
# the capture device to record. The clip carries an SMPTE-like background, a
# binary frame counter (see pattern.conf) and a different tone per audio
# channel, so that a capture can be checked for dropped, duplicated and
# reordered frames and for gaps in the audio.
#
# The clip is encoded losslessly: the counter blocks have to come out of the
# player exactly as drawn, so that anything smeared in the capture came from
# the analog path or the driver, not from the test signal.
#
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
set -eu

std=ntsc
seconds=60
out=

usage() {
	cat <<EOF
usage: $0 [--std ntsc|pal] [--seconds N] [--out FILE]

  --std      signal standard to generate for (default: $std)
  --seconds  length of the clip; it is played on a loop (default: $seconds)
  --out      output file (default: pattern-<std>.mkv)
EOF
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case $1 in
	--std)		std=$2; shift 2 ;;
	--seconds)	seconds=$2; shift 2 ;;
	--out)		out=$2; shift 2 ;;
	-h|--help)	usage 0 ;;
	*)		echo "$0: unknown argument '$1'" >&2; usage 1 ;;
	esac
done

. "$(dirname "$0")/pattern.conf"

case $std in
ntsc)	width=720; height=480; rate=30000/1001 ;;
pal)	width=720; height=576; rate=25 ;;
*)	echo "$0: --std must be ntsc or pal" >&2; exit 1 ;;
esac

[ -n "$out" ] || out=pattern-$std.mkv

# The frame counter. One always-on black strip underneath, so that a clear
# bit reads as black rather than as whatever the background happens to be
# there, then one box per bit, enabled when that bit of the frame number is
# set. ffmpeg's expression parser needs the commas of bitand() escaped.
strip_w=$((BITS * BLOCK + (BITS - 1) * GAP + 2 * MARGIN))
filter="drawbox=x=$((ORIGIN_X - MARGIN)):y=$((ORIGIN_Y - MARGIN))"
filter="$filter:w=$strip_w:h=$((BLOCK + 2 * MARGIN)):color=black:t=fill"

bit=0
while [ "$bit" -lt "$BITS" ]; do
	x=$((ORIGIN_X + bit * (BLOCK + GAP)))
	mask=$((1 << bit))
	filter="$filter,drawbox=x=$x:y=$ORIGIN_Y:w=$BLOCK:h=$BLOCK"
	filter="$filter:color=white:t=fill:enable='gt(bitand(n\\,$mask)\\,0)'"
	bit=$((bit + 1))
done

echo "$0: generating $out ($std, ${width}x${height}, $seconds s)" >&2

ffmpeg -hide_banner -y \
	-f lavfi -i "testsrc2=size=${width}x${height}:rate=$rate:duration=$seconds" \
	-f lavfi -i "sine=frequency=$TONE_LEFT:sample_rate=$SAMPLE_RATE:duration=$seconds" \
	-f lavfi -i "sine=frequency=$TONE_RIGHT:sample_rate=$SAMPLE_RATE:duration=$seconds" \
	-filter_complex "[0:v]$filter[v];[1:a][2:a]join=inputs=2:channel_layout=stereo[a]" \
	-map '[v]' -map '[a]' \
	-c:v ffv1 -pix_fmt yuv420p -c:a pcm_s16le \
	"$out"

echo "$0: wrote $out" >&2
