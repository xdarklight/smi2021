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

#if FW_ISR_BARE == 6
	/*
	 * Interrupt test: a handler built the way the vendor's is, which
	 * keeps its registers on a software stack: nothing goes onto the
	 * hardware stack here, the registers are saved to fixed places in
	 * SRAM. Reads 0x1503 and 0x1502, as the vendor's handler does, and
	 * counts itself; the main loop does the rest, polling.
	 */
	.section .bss
isr_save:
	.skip	3
	.text
	.global	__vector_11
__vector_11:
	sts	isr_save, r24
	in	r24, SREG_IO
	sts	isr_save + 1, r24
	sts	isr_save + 2, r25
	lds	r24, 0x1503
	lds	r24, 0x1502
	lds	r24, irq_census + 22
	lds	r25, irq_census + 23
	adiw	r24, 1
	breq	1f
	sts	irq_census + 22, r24
	sts	irq_census + 23, r25
1:	lds	r25, isr_save + 2
	lds	r24, isr_save + 1
	out	SREG_IO, r24
	lds	r24, isr_save
	reti
#endif

#if FW_ISR_BARE == 7
	/*
	 * Interrupt test: the vendor's handler, as docs/hardware.md
	 * "Interrupt" describes it, again without the hardware stack. On a
	 * setup packet (code 0x01 with 0x1502 bit 4): withdraw the pending
	 * packet of the control endpoint, record EV_SETUP for the main loop
	 * and clear 0x1502 bit 4. The other events are left to the polling.
	 */
	.section .bss
isr_save7:
	.skip	2
	.text
	.global	__vector_11
__vector_11:
	sts	isr_save7, r24
	in	r24, SREG_IO
	sts	isr_save7 + 1, r24
	lds	r24, 0x1503
	cpi	r24, 0x01
	brne	9f
	lds	r24, 0x1502
	sbrs	r24, 4
	rjmp	9f
	lds	r24, 0x151c
	andi	r24, 0x7f
	sts	0x151c, r24
	lds	r24, usb_events
	ori	r24, 0x01		/* EV_SETUP */
	sts	usb_events, r24
	lds	r24, 0x1502
	andi	r24, 0xef
	sts	0x1502, r24
9:	lds	r24, irq_census + 22
	inc	r24
	sts	irq_census + 22, r24
	lds	r24, isr_save7 + 1
	out	SREG_IO, r24
	lds	r24, isr_save7
	reti
#endif

#if FW_ISR_BARE == 5
#define RETLOG_SIZE	32
	/*
	 * Interrupt test: note the return address that the core pushed, for
	 * the host to check against the listing, then read the same registers
	 * as the handler of ISR_EVENTS=0, which got through enumerations.
	 */
	.global	__vector_11
__vector_11:
	push	r24
	in	r24, SREG_IO
	push	r24
	push	r25
	push	r30
	push	r31
	push	r23
	/* nesting: depth in retlog + 1, its maximum in retlog + 2 */
	lds	r24, retlog + 1
	inc	r24
	sts	retlog + 1, r24
	lds	r25, retlog + 2
	cp	r25, r24
	brsh	2f
	sts	retlog + 2, r24
2:	/* above the 6 bytes pushed here: the return address; SREG at +5 */
	in	r30, SPL_IO
	in	r31, SPH_IO
	ldd	r23, Z + 5
	ldd	r24, Z + 7
	ldd	r25, Z + 8
	lds	r30, retlog
	push	r30
	andi	r30, RETLOG_SIZE - 1
	mov	r31, r30
	lsl	r30
	add	r30, r31
	clr	r31
	subi	r30, lo8(-(retlog + 4))
	sbci	r31, hi8(-(retlog + 4))
	st	Z+, r24
	st	Z+, r25
	st	Z, r23
	pop	r30
	inc	r30
	sts	retlog, r30
	lds	r24, 0x1503
	lds	r24, 0x1502
	lds	r24, 0x151c
	lds	r24, 0x1518
	lds	r24, irq_census + 22
	lds	r25, irq_census + 23
	adiw	r24, 1
	breq	1f
	sts	irq_census + 22, r24
	sts	irq_census + 23, r25
1:	lds	r24, retlog + 1
	dec	r24
	sts	retlog + 1, r24
	pop	r23
	pop	r31
	pop	r30
	pop	r25
	pop	r24
	out	SREG_IO, r24
	pop	r24
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
