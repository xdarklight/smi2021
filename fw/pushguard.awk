# SPDX-License-Identifier: GPL-2.0-or-later
#
# OpenSMI21: keep interrupts away from the PUSHes of the compiler
#
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
#
#   awk -f pushguard.awk in.s > out.s
#
# On the SMI2021's core, an interrupt that starts on a PUSH loses its
# return address: the core moves SP but doesn't write the address, and
# RETI jumps to stale stack bytes (docs/hardware.md "Interrupt faults").
# CLI is never interrupted, so it protects the instruction after it. The
# original firmware has no PUSH at all: its compiler keeps everything but
# return addresses on a software stack. avr-gcc saves registers with PUSH
# in nearly every function, so this filter, run on the assembly that
# avr-gcc writes, puts each run of them under CLI:
#
#	in	__tmp_reg__, __SREG__
#	cli
#	push	...
#	out	__SREG__, __tmp_reg__
#
# "rcall ." , which avr-gcc may use to make room for a small stack frame,
# counts as a PUSH. Handlers of interrupts (__vector_N) run with interrupts
# disabled and are left alone; they also have a use for __tmp_reg__ there.
#
# It also turns around the end of avr-gcc's stack pointer update,
#
#	out	__SREG__, __tmp_reg__
#	out	__SP_L__, r28
#
# which relies on the instruction after the one that sets I being executed
# before an interrupt is taken. The core does that, but SPL can as well be
# written first, and was when this was not known yet.
#
# check.sh checks the result in the disassembly.

function flush_guard() {
	if (guarded) {
		print "\tout __SREG__,__tmp_reg__"
		guarded = 0
	}
}

function flush_held() {
	if (held != "") {
		print held
		held = ""
	}
}

# Function labels; .L labels are handled below
/^[A-Za-z_][A-Za-z0-9_]*:/ {
	flush_held()
	flush_guard()
	isr = ($0 ~ /^__vector_[0-9]+:/)
	print
	next
}

# Directives and the labels of the debug information end nothing
/^[ \t]*\./ && !/^\.L[0-9]+:/ {
	if (held != "")
		held = held "\n" $0
	else
		print
	next
}
/^[ \t]*$/ || /^[ \t]*\/\*/ || /^[ \t]*;/ || /^[ \t]*#/ {
	if (held != "")
		held = held "\n" $0
	else
		print
	next
}

{
	insn = $0
	sub(/^[ \t]+/, "", insn)

	if (held != "") {
		if (insn ~ /^out __SP_L__,/) {
			print
			flush_held()
			next
		}
		flush_held()
	}

	if (!isr && (insn ~ /^push[ \t]/ || insn ~ /^rcall[ \t]+\.[ \t]*$/)) {
		if (!guarded) {
			print "\tin __tmp_reg__,__SREG__"
			print "\tcli"
			guarded = 1
		}
		print
		next
	}

	flush_guard()

	if (!isr && insn ~ /^out __SREG__,__tmp_reg__/) {
		held = $0
		next
	}

	print
}

END {
	flush_held()
	flush_guard()
}
