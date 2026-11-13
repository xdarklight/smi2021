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
