#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# OpenSMI21: check the disassembly and the image
#
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
#
#   check.sh build/opensmi21.lst build/opensmi21.bin [1]
#
# - the vector table of avr-libc's crt: 24 JMPs at 0x0000-0x005c, vector 0
#   to the startup code right after the table, vector 11 to the USB
#   handler, all others to their census handler in isr.S
# - the startup code: SP = 0x0fff, .data copied with the plain ELPM, which
#   is the one the core has, and the stack pointer saved for the crash
#   record before it is set
# - no instruction that the core lacks (MUL, MOVW, LPM Rd, ELPM Rd, SPM,
#   ...) outside the instruction tests; the plain ELPM only in the startup
#   code, the instruction tests and pm_read
# - the delay loop of the timing request counts down. The assembler sources
#   go through the C preprocessor, which takes a backslash at the end of a
#   comment for a line continuation and so swallowed the DEC of both delay
#   loops once, leaving them to spin forever.
# - with a third argument of 1, the PUSH guard of pushguard.awk: outside
#   the interrupt handlers and the startup code's check for an interrupt
#   that entered the reset vector, which run with interrupts disabled,
#   every PUSH and every "rcall .+0" comes right after a CLI or after
#   another guarded one, as an interrupt must not start on a PUSH
#   (docs/hardware.md "Interrupt faults"); the window of the interrupt
#   test, which is there to show that, excepted
# - the image: a multiple of 62 bytes, at most 7502, starting with a JMP

set -e
lst=$1
bin=$2
guard=${3:-0}

if ! awk '/<delay_1000>:/,/ret$/' "$lst" | grep -q '	dec	'; then
	echo "check: delay_1000 has no DEC, its loop never ends"
	exit 1
fi

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
		} else if ($0 !~ "<__vector_" n ">$") {
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
		if (insn == "in" && args ~ /^(r24, 0x3d|r25, 0x3e)/ && sp == 0)
			saved++
		if (insn == "elpm" && args == "" && fn == "__do_copy_data")
			copy_elpm++
		if (insn == "call" && $0 ~ /<main>$/) in_main = 1
	}

	# a PUSH is safe after a CLI or after a safe PUSH
	if (insn == "push" || (insn == "rcall" && args ~ /^\.\+0[^0-9]/)) {
		# __init: only after it found I clear, see isr.S
		if (fn ~ /^__vector_[0-9]+$/ || fn == "census_common" ||
		    fn == "__init")
			;
		else if (fn == "win_first" || fn ~ /^wi_/)
			unguarded_test++
		else if (!safe) {
			unguarded++
			if (guard)
				fail(sprintf("unguarded %s at 0x%x in %s", insn, addr, fn))
		} else
			guarded++
	} else {
		safe = (insn == "cli")
	}

	forbidden = insn ~ /^(mul|muls|mulsu|fmul|fmuls|fmulsu|movw|spm|sleep|wdr|eijmp|eicall|break|des)$/ ||
		    (insn ~ /^e?lpm/ && args != "") || insn == "elpm"
	if (forbidden) {
		ok = (fn == "insn_mul" && insn == "mul") ||
		     (fn == "insn_movw" && insn == "movw") ||
		     (fn ~ /^insn_lpm_zp?$/ && insn ~ /^lpm/) ||
		     (fn ~ /^insn_elpm/ && insn == "elpm") ||
		     (fn == "wi_elpm" && insn == "elpm" && args == "") ||
		     (fn == "pm_read" && insn == "elpm" && args == "") ||
		     (fn == "__do_copy_data" && insn == "elpm" && args == "")
		if (!ok)
			fail(sprintf("%s %s at 0x%x in %s", insn, args, addr, fn))
		else if (fn != "__do_copy_data")
			tests[fn] = tests[fn] " " insn (args != "" ? " " args : "")
	}
}

END {
	if (vectors != 24) fail("vector table has " vectors " entries, not 24")
	if (sp != 4) fail("startup does not set SP to 0x0fff")
	if (saved < 2) fail("startup does not save SP before setting it")
	if (copy_elpm != 1) fail("startup does not copy .data with one ELPM")
	for (t in tests) printf("check: deliberate in %s:%s\n", t, tests[t])
	if (unguarded_test)
		printf("check: deliberate in irq_window: %d unguarded push\n", unguarded_test)
	if (bad) exit 1
	if (guard)
		printf("check: %d push under CLI, none without\n", guarded)
	else if (unguarded)
		printf("check: %d unguarded push, do not enable interrupts\n", unguarded)
	print "check: vector table, startup and instruction set ok"
}
' guard="$guard" "$lst"

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
