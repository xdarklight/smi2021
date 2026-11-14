/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OpenSMI21: what avr-libc's startup code doesn't do
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * avr-libc's crt for the ATmega103 provides the 24 vectors of one JMP each,
 * sets SP to 0x0fff, copies .data with the plain ELPM and clears .bss. Only
 * vector 11, the USB controller's, is ever taken (docs/hardware.md "Program
 * memory"); avr-libc sends the others to __vector_default, which returns.
 * With IRQ=1, this adds a check at the reset vector.
 */

#define SREG_IO		0x3f
#define SPH_IO		0x3e
#define SPL_IO		0x3d

	.text
	.global	__vector_default
__vector_default:
	reti

#if FW_IRQ
/*
 * An interrupt that ends at the reset vector instead of its own
 * (docs/hardware.md "Interrupt faults"): the core enters the reset vector
 * with I clear and the return address on the stack. The firmware keeps its
 * windows with interrupts disabled short, which avoids it, so this is the
 * safety net. Seen from here: I clear, interrupts armed (irq_armed, see
 * fw.h), the firmware running already (boot_magic set), SP within its
 * stack, from the end of the data to 0x0ffd, and a return address into the
 * program on top of the stack. Then this is no restart: do what the core
 * should have done, enter the handler of vector 11. Nothing goes onto the
 * stack before SP is known to be the firmware's.
 */
	.section .bss
vec0_tmp:
	.skip	3
	.global	irq_armed
irq_armed:
	.skip	1
	.section .init1, "ax", @progbits
	sts	vec0_tmp, r24
	in	r24, SREG_IO
	sts	vec0_tmp + 1, r24
	sbrc	r24, 7
	rjmp	9f
	lds	r24, irq_armed
	tst	r24
	breq	9f
	lds	r24, boot_magic
	cpi	r24, 0x53
	brne	9f
	lds	r24, boot_magic + 1
	cpi	r24, 0x4f
	brne	9f
	sts	vec0_tmp + 2, r25
	in	r24, SPL_IO		/* SP at or above the end of the data */
	in	r25, SPH_IO
	subi	r24, lo8(__noinit_end)
	sbci	r25, hi8(__noinit_end)
	brlo	9f
	in	r24, SPL_IO		/* and below 0x0ffe */
	in	r25, SPH_IO
	subi	r24, 0xfe
	sbci	r25, 0x0f
	brsh	9f
	lds	r25, vec0_tmp + 2
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
	pop	r31			/* POP leaves the flags alone */
	pop	r30
	pop	r25
	brsh	9f
	lds	r24, vec0_tmp + 1
	out	SREG_IO, r24
	lds	r24, vec0_tmp
	jmp	__vector_11
9:
#endif
