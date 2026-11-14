#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# OpenSMI21: check the disassembly and the image
#
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
#
#   check.sh build/opensmi21.lst build/opensmi21.bin
#
# - the vector table of avr-libc's crt: 24 JMPs at 0x0000-0x005c, vector 0
#   to the startup code right after the table, the others to their handler
#   or to __bad_interrupt, which goes to __vector_default in isr.S
# - the startup code: SP = 0x0fff, .data copied with the plain ELPM, which
#   is the one the core has
# - no instruction that the core lacks (MUL, MOVW, LPM Rd, ELPM Rd, SPM,
#   ...); the plain ELPM only in the startup code
# - the PUSH guard of pushguard.awk: outside the interrupt handlers and
#   the startup code's check for an interrupt that entered the reset
#   vector, which run with interrupts disabled, every PUSH and every
#   "rcall .+0" comes right after a CLI or after another guarded one, as
#   an interrupt must not start on a PUSH (docs/hardware.md "Interrupt
#   faults")
# - the image: a multiple of 62 bytes, at most 7502, starting with a JMP

set -e
lst=$1
bin=$2

awk '
function fail(msg) { print "check: " msg; bad = 1 }
function hex(s,   i, v) {
	v = 0
	for (i = 1; i <= length(s); i++)
		v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
	return v
}

# A function starts at a symbol. Newer binutils (2.47) also show the
# local labels of the assembler (.L...) as symbols; they are part of the
# function before them, so they do not start a new one. No quotes here:
# this is inside the quoted awk program.
/^[0-9a-f]+ <[^>]+>:$/ {
	name = $2; gsub(/[<>:]/, "", name)
	if (name ~ /^\.L/)
		next
	fn = name
	safe = 0
	next
}

/^ +[0-9a-f]+:\t/ {
	split($0, f, "\t")
	addr = f[1]; gsub(/[ :]/, "", addr); addr = hex(addr)
	insn = f[3]; args = f[4]

	if (addr < 0x60) {
		n = addr / 4
		if (insn != "jmp" || addr % 4)
			fail(sprintf("vector table entry at 0x%x is not a JMP", addr))
		if (n == 0) {
			if (args !~ /^0x60[^0-9a-f]/ && args != "0x60")
				fail("vector 0 does not go to 0x0060")
		} else if ($0 !~ "<(__vector_" n "|__bad_interrupt)>$") {
			fail(sprintf("vector %d does not go to __vector_%d", n, n))
		}
		vectors++
	}

	# avr-libc startup, up to the call of main()
	if (addr >= 0x60 && !in_main) {
		if (insn == "ldi" && args ~ /^r28, 0xFF/) sp++
		if (insn == "ldi" && args ~ /^r29, 0x0F/) sp++
		if (insn == "out" && args ~ /^0x3e, r29/) sp++
		if (insn == "out" && args ~ /^0x3d, r28/) sp++
		if (insn == "elpm" && args == "" && fn == "__do_copy_data")
			copy_elpm++
		if (insn == "call" && $0 ~ /<main>$/) in_main = 1
	}

	# a PUSH is safe after a CLI or after a safe PUSH
	if (insn == "push" || (insn == "rcall" && args ~ /^\.\+0[^0-9]/)) {
		# __init, v0_*: only after it found I clear, see isr.S
		if (fn ~ /^__vector_[0-9]+$/ || fn == "__init")
			;
		else if (!safe)
			fail(sprintf("unguarded %s at 0x%x in %s", insn, addr, fn))
		else
			guarded++
	} else {
		safe = (insn == "cli")
	}

	forbidden = insn ~ /^(mul|muls|mulsu|fmul|fmuls|fmulsu|movw|spm|sleep|wdr|eijmp|eicall|break|des)$/ ||
		    (insn ~ /^e?lpm/ && args != "") || insn == "elpm"
	if (forbidden) {
		ok = (fn == "__do_copy_data" && insn == "elpm" && args == "")
		if (!ok)
			fail(sprintf("%s %s at 0x%x in %s", insn, args, addr, fn))
	}
}

END {
	if (vectors != 24) fail("vector table has " vectors " entries, not 24")
	if (sp != 4) fail("startup does not set SP to 0x0fff")
	if (copy_elpm != 1) fail("startup does not copy .data with one ELPM")
	if (bad) exit 1
	printf("check: %d push under CLI, none without\n", guarded)
	print "check: vector table, startup and instruction set ok"
}
' "$lst"

size=$(stat -c %s "$bin")
if [ $((size % 62)) -ne 0 ] || [ "$size" -gt 7502 ]; then
	echo "check: image size $size is not a multiple of 62 up to 7502"
	exit 1
fi
if [ "$(head -c 2 "$bin" | od -An -tx1 | tr -d ' ')" != "0c94" ]; then
	echo "check: image does not start with a JMP"
	exit 1
fi
echo "check: image $size bytes ok"
