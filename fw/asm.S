/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OpenSMI21: delay loops, program memory access and instruction tests
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * The cycle counts assume the instruction timing of the classic AVR core
 * (NOP, LDI, DEC 1 cycle; SBIW 2; BRNE 2 taken, 1 not taken).
 *
 * The instructions of the instruction tests are emitted as raw opcodes with
 * .word, so that each test runs exactly the opcode it names; avr-as for the
 * ATmega103 would refuse most of their mnemonics, as the ATmega103, like
 * this core, lacks them (docs/hardware.md "CPU core").
 */

#define SREG_IO		0x3f
#define SPH_IO		0x3e
#define SPL_IO		0x3d
#define RAMPZ_IO	0x3b

#define OP_MUL_R18_R19		0x9f23	/* mul r18, r19 */
#define OP_MOVW_R18_R20		0x019a	/* movw r18, r20 */
#define OP_LPM_R18_Z		0x9124	/* lpm r18, Z */
#define OP_LPM_R18_ZP		0x9125	/* lpm r18, Z+ */
#define OP_ELPM			0x95d8	/* elpm (r0, RAMPZ:Z) */
#define OP_ELPM_R18_Z		0x9126	/* elpm r18, Z */
#define OP_ELPM_R18_ZP		0x9127	/* elpm r18, Z+ */

	.text

/*
 * void delay_1000(uint16_t units): exactly 1000 * units - 1 cycles from
 * the first LDI to the RET, for the timing request
 */
	.global	delay_1000
delay_1000:
	sbiw	r24, 0
	breq	3f
1:	ldi	r18, 249	; 1
2:	nop			; 1, and with the next two
	dec	r18		; 1, 249 * 4 - 1 = 995
	brne	2b		; 2
	sbiw	r24, 1		; 2
	brne	1b		; 2, 1000 per unit
3:	ret

/* uint8_t flash_byte(const void *addr), with the LPM that loads r0 */
	.global	flash_byte
flash_byte:
	mov	r30, r24
	mov	r31, r25
	lpm
	mov	r24, r0
	ret

/*
 * void pm_read(uint8_t *dst, uint16_t addr, uint8_t len, uint16_t elpm)
 *
 * Copies len bytes of program memory from addr. With bit 8 of elpm set,
 * it uses ELPM with RAMPZ = the low byte of elpm instead of LPM and puts
 * the previous value of RAMPZ back afterwards. Z wraps from 0xffff to 0
 * without a carry into RAMPZ.
 */
	.global	pm_read
pm_read:
	mov	r26, r24
	mov	r27, r25
	mov	r30, r22
	mov	r31, r23
	tst	r20
	breq	9f
	sbrc	r19, 0
	rjmp	5f
1:	lpm
	adiw	r30, 1
	st	X+, r0
	dec	r20
	brne	1b
	ret
5:	in	r21, RAMPZ_IO
	out	RAMPZ_IO, r18
6:	.word	OP_ELPM
	adiw	r30, 1
	st	X+, r0
	dec	r20
	brne	6b
	out	RAMPZ_IO, r21
9:	ret

/*
 * Instruction tests. Each one fills out[] with
 *
 *   0-3   the registers A, B, C, D before the instruction
 *   4-7   the same registers after it
 *   8     SREG before the instruction
 *   9     SREG after it
 *   10    RAMPZ read back after writing it (ELPM tests only)
 *   11    RAMPZ after the instruction (ELPM tests only)
 *
 * and runs the instruction once, between two known register states. The
 * expected values are listed in README.md.
 */

	.section .progmem.insn, "a", @progbits
	.global	insn_pattern
	.type	insn_pattern, @object
insn_pattern:
	.byte	0x3c, 0xc3, 0x69, 0x96
	.size	insn_pattern, . - insn_pattern

	.text

	/* X = out */
	.macro	OUT_PTR
	mov	r26, r24
	mov	r27, r25
	.endm

	.macro	STORE4 a, b, c, d
	st	X+, \a
	st	X+, \b
	st	X+, \c
	st	X+, \d
	.endm

	.macro	Z_PATTERN
	ldi	r30, lo8(insn_pattern)
	ldi	r31, hi8(insn_pattern)
	.endm

/* void insn_mul(uint8_t *out): A = r0, B = r1, C = r18, D = r19 */
	.global	insn_mul
insn_mul:
	OUT_PTR
	ldi	r18, 0xa5
	mov	r0, r18
	ldi	r18, 0x5a
	mov	r1, r18
	ldi	r18, 0xc3
	ldi	r19, 0x5b
	STORE4	r0, r1, r18, r19
	sec			; MUL clears both for 0xc3 * 0x5b
	sez
	in	r20, SREG_IO
	.word	OP_MUL_R18_R19
	in	r21, SREG_IO
	STORE4	r0, r1, r18, r19
	st	X+, r20
	st	X+, r21
	clr	r1
	ret

/* void insn_movw(uint8_t *out): A = r18, B = r19, C = r20, D = r21 */
	.global	insn_movw
insn_movw:
	OUT_PTR
	ldi	r18, 0x11
	ldi	r19, 0x22
	ldi	r20, 0x33
	ldi	r21, 0x44
	STORE4	r18, r19, r20, r21
	in	r22, SREG_IO
	.word	OP_MOVW_R18_R20
	in	r23, SREG_IO
	STORE4	r18, r19, r20, r21
	st	X+, r22
	st	X+, r23
	ret

/* LPM tests: A = r18, B = r0, C = ZL, D = ZH */
	.macro	LPM_TEST name, op
	.global	\name
\name:
	OUT_PTR
	ldi	r18, 0x5a
	mov	r0, r18
	ldi	r18, 0xa5
	Z_PATTERN
	STORE4	r18, r0, r30, r31
	in	r20, SREG_IO
	.word	\op
	in	r21, SREG_IO
	STORE4	r18, r0, r30, r31
	st	X+, r20
	st	X+, r21
	ret
	.endm

	LPM_TEST insn_lpm_z, OP_LPM_R18_Z
	LPM_TEST insn_lpm_zp, OP_LPM_R18_ZP

/*
 * ELPM tests, (uint8_t *out, uint8_t rampz): A = r0, B = r18 for the
 * plain ELPM, A = r18, B = r0 for the others; C = ZL, D = ZH
 */
	.macro	ELPM_TEST name, op, a, b
	.global	\name
\name:
	OUT_PTR
	in	r23, RAMPZ_IO
	out	RAMPZ_IO, r22
	in	r19, RAMPZ_IO
	ldi	r18, 0x5a
	mov	r0, r18
	ldi	r18, 0xa5
	Z_PATTERN
	STORE4	\a, \b, r30, r31
	in	r20, SREG_IO
	.word	\op
	in	r21, SREG_IO
	in	r24, RAMPZ_IO
	out	RAMPZ_IO, r23
	STORE4	\a, \b, r30, r31
	st	X+, r20
	st	X+, r21
	st	X+, r19
	st	X+, r24
	ret
	.endm

	ELPM_TEST insn_elpm, OP_ELPM, r0, r18
	ELPM_TEST insn_elpm_z, OP_ELPM_R18_Z, r18, r0
	ELPM_TEST insn_elpm_zp, OP_ELPM_R18_ZP, r18, r0

#if FW_IRQDIAG
/*
 * void irq_enable_diag(uint8_t knob), IRQDIAG=1: irq_on() for the place in
 * usb_poll(), with the switches of request 0xfa: bits 4-2 of knob, k, a
 * delay before the SEI, with interrupts still disabled, of 4^(k-1) loops
 * of 4 cycles (none for 0, 16384 cycles for 7); bits 1-0 what follows the
 * SEI: 0 RET, as irq_on(), 1 NOP and RET, 2 two NOPs and RET; 3 sets I
 * with an OUT to SREG instead of SEI, as pushguard.awk's code does, then
 * RET. Labels named, not numbered: see the window code below.
 */
	.global	irq_enable_diag
irq_enable_diag:
	lds	r25, irqrec + 4		/* the storm guard tripped */
	tst	r25
	brne	ied_out
	mov	r25, r24
	andi	r25, 0x1c
	breq	ied_go
	ldi	r26, 1
	ldi	r27, 0
ied_shift:
	subi	r25, 4
	breq	ied_wait
	lsl	r26
	rol	r27
	lsl	r26
	rol	r27
	rjmp	ied_shift
ied_wait:
	sbiw	r26, 1
	brne	ied_wait
ied_go:
	sbrs	r24, 1
	rjmp	ied_zero_one
	sbrs	r24, 0
	rjmp	ied_two
	in	r25, 0x3f
	ori	r25, 0x80
	out	0x3f, r25
	ret
ied_zero_one:
	sbrc	r24, 0
	rjmp	ied_one
	sei
	ret
ied_one:
	sei
	nop
	ret
ied_two:
	sei
	nop
	nop
ied_out:
	ret
#endif

#if FW_ISR_BARE == 9
#include "win.h"

/*
 * void irq_window(void), for docs/hardware.md "Experiments", item 12: the
 * only code of this build that runs with interrupts enabled. WIN_ROUNDS
 * times, it opens a window on a fixed sequence of instructions, among them
 * PUSH, POP, RCALL, CALL and RET, and closes it again. The handler of
 * vector 11 in isr.S logs where each interrupt returns to and what the
 * stack pointer was there, so that the host sees which instructions an
 * interrupt may not return to.
 *
 * Nothing here relies on the stack: SPL is set anew for every round, to
 * WIN_REDZONE bytes below the caller's, so that neither a PUSH nor a POP
 * too many does harm; a round that ends with another SPL is counted. The
 * stack is nowhere near a multiple of 256 bytes deep, so SPH stays.
 *
 * Nor does it rely on CLI: the handler returns with interrupts disabled
 * when it finds the window closed, and counts that.
 *
 * Nor does it rely on the core writing the return address: before every
 * round, the WIN_FILL bytes below the window's stack are set to 0xff, and
 * so is r24, which the handler saves where the next return address would
 * go. A return address the core did not write then reads 0xffxx, outside
 * the window; the handler counts it and returns to win_lost instead, with
 * interrupts disabled, which ends the round.
 */
	.section .bss
wscratch:
	.skip	4
wysave:
	.skip	2
	.text
	.global	irq_window
	.global	win_first
	.global	win_lost
	.global	win_end
irq_window:
	sts	wysave, r28		/* Y is the caller's */
	sts	wysave + 1, r29
	ldi	r28, lo8(wscratch)
	ldi	r29, hi8(wscratch)
	in	r18, SPL_IO
	subi	r18, WIN_REDZONE
	sts	wrec + WREC_BASE, r18
	lds	r23, wrec + WREC_MODE
	ldi	r19, WIN_ROUNDS
	ldi	r22, 1
	ldi	r26, lo8(wscratch)
	ldi	r27, hi8(wscratch)
	rjmp	1f
win_first:
win_sub:
	nop
	ret

/*
 * WMODE2_INSN. Each test opens the window with SEI right in front of the
 * instruction under test, so that a pending interrupt is taken on it,
 * notes SREG right after it in r21, closes the window and rebalances what
 * it did to the stack. wi_tail counts a clear I there, which only an
 * interrupt that didn't reach the handler can cause. A lost return
 * address ends in win_lost, a wrong SPL is counted by the round's end.
 * X and Y point to wscratch, r20 is 0xff.
 */
wi_start:
	andi	r21, 0x1f
	cpi	r21, WIN_INSNS
	brlo	wi_l1
	ldi	r21, 0
wi_l1:	ldi	r30, pm_lo8(wi_table)
	ldi	r31, pm_hi8(wi_table)
	add	r30, r21
	adc	r31, r1
	ijmp
wi_table:
	rjmp	wi_nop
	rjmp	wi_push
	rjmp	wi_pop
	rjmp	wi_rcall
	rjmp	wi_call
	rjmp	wi_ret
	rjmp	wi_lds
	rjmp	wi_sts
	rjmp	wi_lds_usb
	rjmp	wi_sts_i2c
	rjmp	wi_ld
	rjmp	wi_st
	rjmp	wi_ldd
	rjmp	wi_std
	rjmp	wi_lpm
	rjmp	wi_elpm
	rjmp	wi_adiw
	rjmp	wi_sbiw
	rjmp	wi_rjmp
	rjmp	wi_jmp
	rjmp	wi_breq
	rjmp	wi_skip1
	rjmp	wi_skip2
	rjmp	wi_sbi
	rjmp	wi_cbi
	rjmp	wi_ld_usb

wi_nop:
	sei
	nop
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_push:
	sei
	push	r20
	in	r21, SREG_IO
	cli
	pop	r20
	rjmp	wi_tail
wi_pop:
	sei
	pop	r20
	in	r21, SREG_IO
	cli
	push	r20
	rjmp	wi_tail
wi_rcall:
	ldi	r30, pm_lo8(wi_rcall_ret)
	ldi	r31, pm_hi8(wi_rcall_ret)
	sei
	rcall	wi_csub
wi_rcall_ret:
	rjmp	wi_tail
wi_call:
	ldi	r30, pm_lo8(wi_call_ret)
	ldi	r31, pm_hi8(wi_call_ret)
	sei
	call	wi_csub
wi_call_ret:
	rjmp	wi_tail
wi_ret:
	rcall	wi_rsub
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_lds:
	sei
	lds	r20, wscratch
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_sts:
	sei
	sts	wscratch, r20
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_lds_usb:
	sei
	lds	r20, 0x1503
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_sts_i2c:
	sei
	sts	0x17a0, r20		/* first i2c engine, buffer byte 27 */
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_ld:
	sei
	ld	r20, X
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_st:
	sei
	st	X, r20
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_ldd:
	sei
	ldd	r20, Y + 1
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_std:
	sei
	std	Y + 1, r20
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_lpm:
	ldi	r30, lo8(insn_pattern)
	ldi	r31, hi8(insn_pattern)
	sei
	lpm
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_elpm:
	out	RAMPZ_IO, r1
	ldi	r30, lo8(insn_pattern)
	ldi	r31, hi8(insn_pattern)
	sei
	.word	OP_ELPM
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_adiw:
	sei
	adiw	r26, 0
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_sbiw:
	sei
	sbiw	r26, 0
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_rjmp:
	sei
	rjmp	wi_l2
wi_l2:	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_jmp:
	sei
	jmp	wi_l3
wi_l3:	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_breq:
	sez
	sei
	breq	wi_l4
wi_l4:	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_skip1:
	sei
	sbrs	r20, 0			/* r20 is 0xff: skips */
	nop
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_skip2:
	sei
	sbrs	r20, 0
	lds	r20, wscratch
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_sbi:
	sei
	sbi	0x09, 0			/* UBRR, unused */
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_cbi:
	sei
	cbi	0x09, 0
	in	r21, SREG_IO
	cli
	rjmp	wi_tail
wi_ld_usb:
	ldi	r30, 0x03
	ldi	r31, 0x15
	sei
	ld	r20, Z
	in	r21, SREG_IO
	cli
	rjmp	wi_tail

/*
 * Called by wi_rcall and wi_call with the return address they expect in
 * Z: notes SREG, and checks and repairs the return address on the stack,
 * counting a wrong one as lost
 */
wi_csub:
	in	r21, SREG_IO
	cli
	push	r28
	push	r29
	in	r28, SPL_IO
	in	r29, SPH_IO
	ldd	r20, Y + 3		/* high byte of the return address */
	cp	r20, r31
	ldd	r20, Y + 4
	cpc	r20, r30
	breq	wi_l5
	ldd	r20, Y + 3
	sts	wrec + WREC_LOSTW, r20
	ldd	r20, Y + 4
	sts	wrec + WREC_LOSTW + 1, r20
	lds	r20, wrec + WREC_LOST
	inc	r20
	breq	wi_l6
	sts	wrec + WREC_LOST, r20
wi_l6:	std	Y + 3, r31
	std	Y + 4, r30
wi_l5:	pop	r29
	pop	r28
	ldi	r20, 0xff
	ret

/* Called by wi_ret: the RET is the instruction under test */
wi_rsub:
	sei
	ret

wi_tail:
	sbrc	r21, 7
	rjmp	win_lost
	lds	r21, wrec + WREC_SILENT
	inc	r21
	breq	wi_l7
	sts	wrec + WREC_SILENT, r21
wi_l7:	rjmp	win_lost
1:	in	r31, SPH_IO		/* fill from the window's stack down */
	mov	r30, r18
	subi	r30, -1
	ldi	r20, 0xff
	ldi	r21, WIN_FILL
0:	st	-Z, r20
	dec	r21
	brne	0b
	ldi	r24, 0xff
	sts	wprev, r24		/* no return seen in this round yet */
	out	SPL_IO, r18
	sts	wrec + WREC_BURST, r1
	sts	wrec + WREC_OPEN, r22
	/* WMODE2_INSN: one instruction right after SEI, see wi_table */
	lds	r21, wrec + WREC_MODE2
	sbrc	r21, 7
	rjmp	wi_start
	/* WMODE2_CLOSE: is the instruction right after SEI safe to close? */
	sbrs	r21, 0
	rjmp	11f
	in	r21, SREG_IO		/* I is clear here */
	sbrc	r23, 3
	rjmp	12f
	sei
	cli
	rjmp	6f
12:	sei
	out	SREG_IO, r21
	rjmp	6f
	/* nops, unless a class of instructions is chosen */
11:	mov	r21, r23
	andi	r21, 0x70
	brne	2f
	sei
	nop
	nop
	nop
	nop
	/*
	 * Each class opens the window itself: a pending interrupt is taken
	 * in the decode cycle of the instruction right after SEI, so it hits
	 * the first one of the class
	 */
2:	sbrs	r23, 4
	rjmp	3f
	sei
	push	r20
	push	r20
	push	r20
	nop
3:	sbrs	r23, 5
	rjmp	4f
	sei
	pop	r20
	pop	r20
	pop	r20
	nop
4:	sbrs	r23, 6
	rjmp	9f
	sei
	rcall	win_sub
	nop
	call	win_sub
	nop
	lds	r20, wscratch + 2
	sts	wscratch + 3, r20
	st	X+, r20
	ld	r20, -X
	adiw	r24, 1
	sbiw	r24, 1
9:	sbrs	r23, 3
	rjmp	5f
	in	r21, SREG_IO
	andi	r21, 0x7f
	out	SREG_IO, r21
	rjmp	6f
5:	cli
win_lost:
6:	nop
	nop
	sts	wrec + WREC_OPEN, r1
	sbrs	r23, 3
	cli
	nop
	in	r21, SPL_IO
	cp	r21, r18
	breq	7f
	sub	r21, r18
	sts	wrec + WREC_LAST, r21
	lds	r24, wrec + WREC_BAD
	lds	r25, wrec + WREC_BAD + 1
	adiw	r24, 1
	sts	wrec + WREC_BAD, r24
	sts	wrec + WREC_BAD + 1, r25
7:	dec	r19
	breq	8f
	rjmp	1b
8:	subi	r18, -WIN_REDZONE
	out	SPL_IO, r18
	lds	r28, wysave
	lds	r29, wysave + 1
win_end:
	ret
#endif
