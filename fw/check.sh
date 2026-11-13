#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# OpenSMI21: check the disassembly and the image
#
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
#
#   check.sh build/opensmi21.lst build/opensmi21.bin
#
# - the vector table: 24 JMPs at 0x0000-0x005c, vector 0 to __reset,
#   vector 11 to the USB handler, all others to their census stub
# - the startup code: SP = 0x0fff, .data copied with the plain LPM
# - no instruction that the core may lack (MUL, MOVW, LPM Rd, ELPM, SPM,
#   ...) outside the instruction tests and the ELPM path of pm_read
# - the image: a multiple of 62 bytes, at most 7502, starting with the table
# - the delay loops: each inner loop counts down. The assembler sources go
#   through the C preprocessor, which takes a backslash at the end of a
#   comment for a line continuation and so swallowed the DEC of both loops
#   once, leaving them to spin forever.

set -e
lst=$1
bin=$2

for fn in delay_160 delay_1000; do
	if ! awk "/<$fn>:/,/ret\$/" "$lst" | grep -q '	dec	'; then
		echo "check: $fn has no DEC, its loop never ends"
		exit 1
	fi
done

awk '
function fail(msg) { print "check: " msg; bad = 1 }
function hex(s,   i, v) {
	v = 0
	for (i = 1; i <= length(s); i++)
		v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
	return v
}

/^[0-9a-f]+ <[^>]+>:$/ {
	fn = $2; gsub(/[<>:]/, "", fn)
	next
}

/^ +[0-9a-f]+:\t/ {
	split($0, f, "\t")
	addr = f[1]; gsub(/[ :]/, "", addr); addr = hex(addr)
	insn = f[3]; args = f[4]

	if (fn == "__vectors") {
		# 24 JMPs, or 48 RJMPs for the interrupt test (VECTORS=rjmp48)
		if (addr == 0)
			rjmp_table = insn == "rjmp"
		size = rjmp_table ? 2 : 4
		n = addr / size
		if (insn != (rjmp_table ? "rjmp" : "jmp") || addr % size)
			fail(sprintf("vector table entry at 0x%x is not a %s",
				     addr, rjmp_table ? "RJMP" : "JMP"))
		usb = rjmp_table ? 22 : 11
		want = n == 0 ? "__reset" : n == usb ? "__vector_11" : "__stub_" n
		if ($0 !~ "<" want ">$")
			fail(sprintf("vector %d does not go to %s", n, want))
		vectors++
	}

	if (fn == "__reset") {
		if (insn == "ldi" && args ~ /^r28, 0xFF/) sp++
		if (insn == "ldi" && args ~ /^r29, 0x0F/) sp++
		if (insn == "out" && args ~ /^0x3e, r29/) sp++
		if (insn == "out" && args ~ /^0x3d, r28/) sp++
	}
	if (fn == "__do_copy_data" && insn ~ /^lpm/) {
		if (args != "") fail("startup uses LPM with a register")
		copy_lpm++
	}

	forbidden = insn ~ /^(mul|muls|mulsu|fmul|fmuls|fmulsu|movw|elpm|spm|sleep|wdr|eijmp|eicall|break|des)$/ ||
		    (insn ~ /^lpm/ && args != "")
	if (forbidden) {
		ok = (fn == "insn_mul" && insn == "mul") ||
		     (fn == "insn_movw" && insn == "movw") ||
		     (fn ~ /^insn_lpm_zp?$/ && insn ~ /^lpm/) ||
		     (fn ~ /^insn_elpm/ && insn == "elpm") ||
		     (fn == "pm_read" && insn == "elpm" && args == "")
		if (!ok)
			fail(sprintf("%s %s at 0x%x in %s", insn, args, addr, fn))
		else
			tests[fn] = tests[fn] " " insn (args != "" ? " " args : "")
	}
}

END {
	if (vectors != (rjmp_table ? 48 : 24))
		fail("vector table has " vectors " entries")
	if (sp != 4) fail("startup does not set SP to 0x0fff")
	if (copy_lpm != 1) fail("startup does not copy .data with one LPM")
	for (t in tests) printf("check: deliberate in %s:%s\n", t, tests[t])
	if (bad) exit 1
	print "check: vector table, startup and instruction set ok"
}
' "$lst"

size=$(stat -c %s "$bin")
if [ $((size % 62)) -ne 0 ] || [ "$size" -gt 7502 ]; then
	echo "check: image size $size is not a multiple of 62 up to 7502"
	exit 1
fi
first=$(head -c 2 "$bin" | od -An -tx1 | tr -d ' ')
case $first in
0c94)	;;
??c?)	# an RJMP (little endian, 0xcxxx), the table of the interrupt test
	;;
*)	echo "check: image does not start with a JMP or an RJMP"
	exit 1 ;;
esac
echo "check: image $size bytes ok"
