/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OpenSMI21: what avr-libc's startup code doesn't do
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * avr-libc's crt for the ATmega103 provides the 24 vectors of one JMP each,
 * sets SP to 0x0fff, copies .data with the plain ELPM and clears .bss. This
 * adds the stack pointer as found at reset, for the crash record, and the
 * handlers of the vectors other than the USB controller's, vector 11: each
 * counts its hits for the interrupt census and returns.
 */

#define SREG_IO		0x3f
#define SPH_IO		0x3e
#define SPL_IO		0x3d

/* Before avr-libc sets SP in .init2: where the stack was at reset */
	.section .init1, "ax", @progbits
	in	r24, SPL_IO
	in	r25, SPH_IO
	sts	reset_sp, r24
	sts	reset_sp + 1, r25

	.text

#if FW_ISR_BARE == 1
	/* interrupt test: vector 11 does nothing at all */
	.global	__vector_11
__vector_11:
	reti
#endif

/*
 * Interrupt census. Each handler pushes r24, loads its vector number and
 * jumps to the common part, which increments irq_census[n], saturating at
 * 0xffff, and remembers n in irq_last.
 */
	.irp	n, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23
	.global	__vector_\n
__vector_\n:
	push	r24
	ldi	r24, \n
	rjmp	census_common
	.endr

census_common:
	push	r25
	in	r25, SREG_IO
	push	r25
	push	r30
	push	r31
	sts	irq_last, r24
	ldi	r30, lo8(irq_census)
	ldi	r31, hi8(irq_census)
	lsl	r24
	add	r30, r24
	ldi	r24, 0
	adc	r31, r24
	ld	r24, Z
	ldd	r25, Z+1
	adiw	r24, 1
	breq	1f
	st	Z, r24
	std	Z+1, r25
1:	pop	r31
	pop	r30
	pop	r25
	out	SREG_IO, r25
	pop	r25
	pop	r24
	reti
