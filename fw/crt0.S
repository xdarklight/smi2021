/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OpenSMI21: vector table, startup code and interrupt census stubs
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * Replaces avr-libc's crt: 24 vectors of one 4 byte JMP each, as on the
 * ATmega103, then SP = 0x0fff, .bss cleared, .data copied with the plain
 * LPM that loads r0, and main(). Vector 11 is the USB controller; every
 * other vector goes to a stub that counts its hits and returns.
 */

#define SREG_IO		0x3f
#define SPH_IO		0x3e
#define SPL_IO		0x3d

	.section .vectors, "ax", @progbits
	.global	__vectors
__vectors:
#if FW_RJMP_TABLE
	/*
	 * Interrupt test: 48 one word entries cover 0x0000-0x005f whether the
	 * core spaces its vectors 4 bytes apart, as the ATmega103, or 2 bytes,
	 * as the small AVRs. Slot n is address 2n; slot 22 is 0x002c, vector
	 * 11 of 4 bytes, the USB handler.
	 */
	rjmp	__reset
	.irp	n, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21
	rjmp	__stub_\n
	.endr
	rjmp	__vector_11
	.irp	n, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47
	rjmp	__stub_\n
	.endr
#else
	jmp	__reset
	.irp	n, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
	jmp	__stub_\n
	.endr
	jmp	__vector_11
	.irp	n, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23
	jmp	__stub_\n
	.endr
#endif

	.section .init, "ax", @progbits
	.global	__reset
__reset:
	/* for the crash record: where the stack was when we got here */
	in	r24, SPL_IO
	in	r25, SPH_IO
	sts	reset_sp, r24
	sts	reset_sp + 1, r25
	clr	r1
	out	SREG_IO, r1
	ldi	r28, 0xff
	ldi	r29, 0x0f
	out	SPH_IO, r29
	out	SPL_IO, r28

	/* also keeps libgcc's versions out of the link */
	.global	__do_clear_bss
__do_clear_bss:
	ldi	r18, hi8(__bss_end)
	ldi	r26, lo8(__bss_start)
	ldi	r27, hi8(__bss_start)
	rjmp	2f
1:	st	X+, r1
2:	cpi	r26, lo8(__bss_end)
	cpc	r27, r18
	brne	1b

	.global	__do_copy_data
__do_copy_data:
	ldi	r18, hi8(__data_end)
	ldi	r26, lo8(__data_start)
	ldi	r27, hi8(__data_start)
	ldi	r30, lo8(__data_load_start)
	ldi	r31, hi8(__data_load_start)
	rjmp	2f
1:	lpm
	adiw	r30, 1
	st	X+, r0
2:	cpi	r26, lo8(__data_end)
	cpc	r27, r18
	brne	1b

	call	main
3:	rjmp	3b

#if FW_ISR_BARE == 1
	/* interrupt test: vector 11 does nothing at all */
	.global	__vector_11
__vector_11:
	reti
#endif

/*
 * Interrupt census. Each stub pushes r24, loads its vector number and
 * jumps to the common part, which increments irq_census[n], saturating at
 * 0xffff, and remembers n in irq_last.
 */
	.text
#if FW_RJMP_TABLE
	.irp	n, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47
#else
	.irp	n, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23
#endif
__stub_\n:
	push	r24
	ldi	r24, \n
	rjmp	__stub_common
	.endr

__stub_common:
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
