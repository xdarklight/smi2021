/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OpenSMI21: the record of the interrupt window test, ISR_BARE=9, request
 * 0xf9, for C and assembly; see README.md
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 */

#ifndef OPENSMI21_WIN_H
#define OPENSMI21_WIN_H

#define WREC_MODE	0	/* see WMODE_* */
#define WREC_POS	1	/* log entries written, up to WLOG_SIZE */
#define WREC_PASSES	2	/* 2 bytes: windows run */
#define WREC_TOTAL	4	/* 2 bytes: interrupts taken */
#define WREC_BAD	6	/* 2 bytes: rounds that ended with SPL off */
#define WREC_LAST	8	/* by how much, the last time */
#define WREC_ESC	9	/* interrupts taken with the window closed */
#define WREC_ESC_ADDR	10	/* return address of the last, high byte first */
#define WREC_STORM	12	/* rounds the handler gave up on */
#define WREC_BASE	13	/* SPL of the window */
#define WREC_OPEN	14	/* the window is open */
#define WREC_BURST	15	/* interrupts in this round */
#define WREC_MAXBURST	16	/* the most in any round */
#define WREC_LEFT	17	/* passes left before the window stays shut */
#define WREC_LOST	18	/* interrupts whose return address was not written */
#define WREC_LOSTAT	19	/* the word of the window the interrupt before the
				   last of them returned to, 0xff if none */
#define WREC_LOSTW	20	/* 2 bytes: what that one found instead, high first */
#define WREC_MODE2	22	/* see WMODE2_* */
#define WREC_SILENT	23	/* WMODE2_INSN: I found clear right after the
				   instruction, without the handler (saturates) */
#define WREC_LOG	24	/* WLOG_SIZE entries: return address, SPL */
#define WLOG_SIZE	64
#define WREC_HIST	(WREC_LOG + 3 * WLOG_SIZE)
#define WHIST_SIZE	64	/* returns per word of the window */
#define WREC_SIZE	(WREC_HIST + WHIST_SIZE)

#define WMODE_EVENT	0x01	/* the handler reads 0x1503 */
#define WMODE_STATUS	0x02	/* the handler reads 0x1502 */
#define WMODE_EP0	0x04	/* the handler reads 0x151c and 0x1518 */
#define WMODE_SREG	0x08	/* close the window by writing SREG, not CLI */
#define WMODE_PUSH	0x10	/* PUSHes in the window */
#define WMODE_POP	0x20	/* POPs in the window */
#define WMODE_CALL	0x40	/* RCALL, CALL, RET and memory accesses */
#define WMODE_RUN	0x80

/* The high byte of wValue */
#define WMODE2_CLOSE	0x01	/* only SEI and, right after it, CLI or (with
				   WMODE_SREG) OUT SREG that clears I */
#define WMODE2_INSN	0x80	/* SEI and, right after it, instruction number
				   (wValue >> 8) & 0x1f of the table in asm.S;
				   0x01 then has no meaning */
#define WIN_INSNS	26	/* entries of that table */

#define WIN_ROUNDS	50	/* per window */
#define WIN_REDZONE	16	/* bytes of stack the window stays clear of */
#define WIN_FILL	24	/* bytes below the window set to 0xff each round */
#define WIN_BURST_MAX	100	/* interrupts in a round before giving up */

#endif /* OPENSMI21_WIN_H */
