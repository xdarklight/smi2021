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

#if FW_IRQ
/*
 * An interrupt that ends at the reset vector instead of its own. The core
 * presumably takes the vector address from the interrupt request lines as
 * they are when it jumps, and finds none left if the request ended in the
 * cycle after the interrupt started (docs/hardware.md "Interrupt faults"):
 * then it enters the reset vector, with I clear and the return address on
 * the stack. Seen from here: I clear, the firmware running already (boot_magic
 * set, SP in its stack, 0x0fxx) and a return address into the program on
 * top of the stack. Then this is no restart: count it, note it, and do what
 * the core should have done, enter the handler of vector 11. Nothing goes
 * onto the stack before SP is known to be the firmware's.
 */
	.section .bss
	.global	vec0_rec
vec0_rec:			/* count, SREG, SPL, SPH (less 3), return address */
	.skip	6
vec0_tmp:
	.skip	2
#if FW_IRQDIAG
	.global	vec0_flag
	.global	vec0_regs
vec0_flag:			/* set here, taken by the handler */
	.skip	1
vec0_regs:			/* 0x1503 and 0x1502 as found here */
	.skip	2
#endif
	.section .init1, "ax", @progbits
	sts	vec0_tmp, r24
	in	r24, SREG_IO
	sts	vec0_tmp + 1, r24
	sbrc	r24, 7
	rjmp	9f
	in	r24, SPH_IO
	cpi	r24, 0x0f
	brne	9f
	lds	r24, boot_magic
	cpi	r24, 0x53
	brne	9f
	lds	r24, boot_magic + 1
	cpi	r24, 0x4f
	brne	9f
	push	r25
	push	r30
	push	r31
	in	r30, SPL_IO
	in	r31, SPH_IO
	ldd	r25, Z + 4		/* the return address, high byte */
	ldd	r24, Z + 5
	ldi	r30, pm_hi8(__data_load_start)
	cpi	r24, pm_lo8(__data_load_start)
	cpc	r25, r30
	brsh	8f
	sts	vec0_rec + 4, r25
	sts	vec0_rec + 5, r24
	in	r24, SPL_IO
	sts	vec0_rec + 2, r24
	in	r24, SPH_IO
	sts	vec0_rec + 3, r24
	lds	r24, vec0_tmp + 1
	sts	vec0_rec + 1, r24
#if FW_IRQDIAG
	lds	r24, 0x1503
	sts	vec0_regs, r24
	lds	r24, 0x1502
	sts	vec0_regs + 1, r24
	ldi	r24, 1
	sts	vec0_flag, r24
#endif
	lds	r24, vec0_rec
	inc	r24
	breq	1f
	sts	vec0_rec, r24
1:	pop	r31
	pop	r30
	pop	r25
	lds	r24, vec0_tmp + 1
	out	SREG_IO, r24
	lds	r24, vec0_tmp
	jmp	__vector_11
8:	pop	r31
	pop	r30
	pop	r25
9:
#endif

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

#if FW_ISR_BARE == 9
#include "win.h"
	/*
	 * Interrupt test with irq_window() in asm.S: count the returns to
	 * each word of the window; log, for the first WLOG_SIZE interrupts,
	 * the return address and the SPL that the interrupted code had, less
	 * the SPL that the window started with; read the registers that the
	 * mode asks for;
	 * and return with interrupts disabled, by RET, when the window is
	 * closed or after too many interrupts in one round, so that nothing
	 * but the window ever runs with interrupts enabled. The main loop
	 * polls the controller as in a build with IRQ=0.
	 *
	 * A return address outside irq_window is one that the core did not
	 * write, see asm.S: count it, note what the interrupt before it
	 * returned to, and return to win_lost instead, by RET.
	 */
	.section .bss
	.global	wprev
wprev:
	.skip	1
	.text
	.global	__vector_11
__vector_11:
	push	r24
	in	r24, SREG_IO
	push	r24
	push	r25
	push	r26
	push	r27
	push	r30
	push	r31
	/* above the 7 bytes pushed here: the return address, high byte first */
	in	r30, SPL_IO
	in	r31, SPH_IO
	/* count it */
	lds	r24, wrec + WREC_TOTAL
	lds	r25, wrec + WREC_TOTAL + 1
	adiw	r24, 1
	sts	wrec + WREC_TOTAL, r24
	sts	wrec + WREC_TOTAL + 1, r25
	/* histogram of the words of the window returned to */
	ldd	r25, Z + 8
	ldd	r24, Z + 9
	subi	r24, pm_lo8(win_first)
	sbci	r25, pm_hi8(win_first)
	tst	r25
	brne	11f
	cpi	r24, WHIST_SIZE
	brsh	11f
	mov	r26, r24
	clr	r27
	subi	r26, lo8(-(wrec + WREC_HIST))
	sbci	r27, hi8(-(wrec + WREC_HIST))
	ld	r24, X
	inc	r24
	breq	11f
	st	X, r24
11:	/* the first WLOG_SIZE: return address and SPL */
	lds	r24, wrec + WREC_POS
	cpi	r24, WLOG_SIZE
	brsh	12f
	mov	r26, r24
	inc	r24
	sts	wrec + WREC_POS, r24
	mov	r27, r26
	add	r26, r26
	add	r26, r27
	clr	r27
	subi	r26, lo8(-(wrec + WREC_LOG))
	sbci	r27, hi8(-(wrec + WREC_LOG))
	ldd	r24, Z + 8
	st	X+, r24
	ldd	r24, Z + 9
	st	X+, r24
	mov	r24, r30
	subi	r24, -9
	lds	r25, wrec + WREC_BASE
	sub	r24, r25
	st	X+, r24
12:
	lds	r25, wrec + WREC_MODE
	sbrs	r25, 0
	rjmp	1f
	lds	r24, 0x1503
1:	sbrs	r25, 1
	rjmp	2f
	lds	r24, 0x1502
2:	sbrs	r25, 2
	rjmp	3f
	lds	r24, 0x151c
	lds	r24, 0x1518
3:	lds	r24, irq_census + 22
	lds	r25, irq_census + 23
	adiw	r24, 1
	breq	4f
	sts	irq_census + 22, r24
	sts	irq_census + 23, r25
4:	/* a return address outside irq_window was not written by the core */
	ldd	r25, Z + 8
	ldd	r24, Z + 9
	ldi	r26, pm_lo8(win_first)
	ldi	r27, pm_hi8(win_first)
	cp	r24, r26
	cpc	r25, r27
	brlo	13f
	ldi	r26, pm_lo8(win_end)
	ldi	r27, pm_hi8(win_end)
	cp	r26, r24
	cpc	r27, r25
	brlo	13f
	/* in the window: remember the word, for the next one */
	subi	r24, pm_lo8(win_first)
	sbci	r25, pm_hi8(win_first)
	tst	r25
	breq	14f
	ldi	r24, 0xff
14:	sts	wprev, r24
	lds	r24, wrec + WREC_BURST
	inc	r24
	sts	wrec + WREC_BURST, r24
	lds	r25, wrec + WREC_MAXBURST
	cp	r25, r24
	brsh	10f
	sts	wrec + WREC_MAXBURST, r24
10:
	cpi	r24, WIN_BURST_MAX
	brlo	5f
	ldi	r26, lo8(wrec + WREC_STORM)
	ldi	r27, hi8(wrec + WREC_STORM)
	rjmp	6f
5:	lds	r24, wrec + WREC_OPEN
	tst	r24
	brne	8f
	ldd	r24, Z + 8
	sts	wrec + WREC_ESC_ADDR, r24
	ldd	r24, Z + 9
	sts	wrec + WREC_ESC_ADDR + 1, r24
	ldi	r26, lo8(wrec + WREC_ESC)
	ldi	r27, hi8(wrec + WREC_ESC)
6:	ld	r24, X
	inc	r24
	breq	7f
	st	X, r24
	rjmp	7f
13:	/* lost: count it and land on win_lost */
	lds	r24, wrec + WREC_LOST
	inc	r24
	breq	15f
	sts	wrec + WREC_LOST, r24
15:	lds	r24, wprev
	sts	wrec + WREC_LOSTAT, r24
	ldd	r24, Z + 8
	sts	wrec + WREC_LOSTW, r24
	ldd	r24, Z + 9
	sts	wrec + WREC_LOSTW + 1, r24
	ldi	r24, pm_hi8(win_lost)
	std	Z + 8, r24
	ldi	r24, pm_lo8(win_lost)
	std	Z + 9, r24
7:	pop	r31			/* leave with interrupts disabled */
	pop	r30
	pop	r27
	pop	r26
	pop	r25
	pop	r24
	out	SREG_IO, r24
	pop	r24
	ret
8:	pop	r31
	pop	r30
	pop	r27
	pop	r26
	pop	r25
	pop	r24
	out	SREG_IO, r24
	pop	r24
	reti
#endif

#if FW_ISR_BARE == 10
#define RETLOG_SIZE	32
	/*
	 * Interrupt test: note the return address and SREG in the ring of
	 * ISR_BARE=5, which is kept across a restart, then go on to the C
	 * handler, __vector_24 in usb.c, which returns by RETI
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
	/* above the 6 bytes pushed here: the return address; SREG at +5 */
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
	pop	r23
	pop	r31
	pop	r30
	pop	r25
	pop	r24
	out	SREG_IO, r24
	pop	r24
	jmp	__vector_24
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
