// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenSMI21: test requests for docs/hardware.md "Experiments", items 8 to 11
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * All test requests are vendor IN requests with bRequest 0xf0 and up; their
 * layout is specified in README.md. Nothing here writes program memory.
 */

#include "fw.h"
#include "regs.h"

#define TEST_IDENTITY		0xf0
#define TEST_INSN		0xf1
#define TEST_PROGMEM		0xf2
#define TEST_CENSUS		0xf3
#define TEST_TIMING		0xf4
#define TEST_EVLOG		0xf5
#define TEST_CRASH		0xf6
#define TEST_IRQ		0xf7
#define TEST_RETLOG		0xf8
#define TEST_WIN		0xf9
#define TEST_IRQDIAG		0xfa

#define INSN_MUL		0
#define INSN_MOVW		1
#define INSN_LPM_Z		2
#define INSN_LPM_ZP		3
#define INSN_ELPM		4
#define INSN_ELPM_Z		5
#define INSN_ELPM_ZP		6

#define PROGMEM_MAX		64
#define PROGMEM_ELPM		0x0100

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

static uint16_t identity(void)
{
	static const char magic[9] = "OpenSMI21";
	uint8_t i;

	for (i = 0; i < sizeof(magic); i++)
		xbuf[i] = magic[i];
	xbuf[9] = TEST_IF_VERSION;
	xbuf[10] = FW_VERSION_MAJOR;
	xbuf[11] = FW_VERSION_MINOR;
	put16(xbuf + 12, FW_PID);
	put16(xbuf + 14, (uint16_t)__image_end);
	xbuf[16] = (FW_ALT0_STOP ? 0x01 : 0) | (FW_IRQ ? 0x02 : 0) |
		   (FW_ISR_EVENTS ? 0x04 : 0) |
		   (FW_ISR_BARE == 1 ? 0x10 : 0) | (FW_ISR_BARE == 2 ? 0x20 : 0) |
		   (FW_ISR_BARE == 3 ? 0x40 : 0) | (FW_ISR_BARE == 4 ? 0x80 : 0);
	xbuf[17] = cur_mode;
	put16(xbuf + 18, boot_count);
	xbuf[20] = irq_last;
	xbuf[21] = usb_state;
	xbuf[22] = usb_alt;
	xbuf[23] = USB_SPEED;
	irq_off();
	put16(xbuf + 24, usb_unknown_events);
	put16(xbuf + 26, irq_census[11]);
	irq_on_at(SPOT_TEST);
	xbuf[28] = (FW_PUSH_GUARD ? 0x01 : 0) | (FW_ISR_BARE << 4);

	return 29;
}

#if FW_ISR_BARE == 9
#if FW_IRQ
#error "ISR_BARE=9 enables interrupts itself, in its window: build with IRQ=0"
#endif

volatile uint8_t wrec[WREC_SIZE];

/* One window per pass of the main loop, for as many as the host asked for */
void irq_window_pass(void)
{
	if (!(wrec[WREC_MODE] & WMODE_RUN))
		return;

	irq_window();
	if (!++wrec[WREC_PASSES])
		wrec[WREC_PASSES + 1]++;
	if (!--wrec[WREC_LEFT])
		wrec[WREC_MODE] &= ~WMODE_RUN;
}
#endif

static uint8_t insn(uint16_t test, uint8_t rampz, uint16_t *len)
{
	uint8_t *out = xbuf + 2;
	uint8_t i;

	if (test > INSN_ELPM_ZP)
		return 1;

	for (i = 0; i < 18; i++)
		xbuf[i] = 0;

	switch (test) {
	case INSN_MUL:
		insn_mul(out);
		break;
	case INSN_MOVW:
		insn_movw(out);
		break;
	case INSN_LPM_Z:
		insn_lpm_z(out);
		break;
	case INSN_LPM_ZP:
		insn_lpm_zp(out);
		break;
	case INSN_ELPM:
		insn_elpm(out, rampz);
		break;
	case INSN_ELPM_Z:
		insn_elpm_z(out, rampz);
		break;
	case INSN_ELPM_ZP:
		insn_elpm_zp(out, rampz);
		break;
	default:
		return 1;
	}

	xbuf[0] = test;
	xbuf[1] = test >= INSN_ELPM ? rampz : 0;
	/* the pattern as plain LPM sees it, for comparison */
	xbuf[14] = flash_byte(insn_pattern);
	xbuf[15] = flash_byte(insn_pattern + 1);
	put16(xbuf + 16, (uint16_t)insn_pattern);
	*len = 18;

	return 0;
}

static uint16_t census(uint8_t clear)
{
	uint8_t i;

	irq_off();
	for (i = 0; i < NUM_VECTORS; i++) {
		put16(xbuf + 2 * i, irq_census[i]);
		/* entry 0 is the start counter, which is not cleared */
		if (clear && i)
			irq_census[i] = 0;
	}
	xbuf[2 * NUM_VECTORS] = irq_last;
	if (clear)
		irq_last = 0xff;
	irq_on_at(SPOT_TEST);

	return 2 * NUM_VECTORS + 1;
}

static uint16_t event_log(uint8_t clear)
{
	uint8_t i;

	irq_off();
	xbuf[0] = evlog_pos;
	xbuf[1] = evlog_count;
	for (i = 0; i < 2 * EVLOG_SIZE; i++) {
		xbuf[2 + i] = evlog[i];
		if (clear)
			evlog[i] = 0;
	}
	if (clear) {
		evlog_pos = 0;
		evlog_count = 0;
	}
	irq_on_at(SPOT_TEST);

	return 2 + 2 * EVLOG_SIZE;
}

uint8_t test_in(const struct setup *s, uint16_t *len)
{
#if FW_ISR_BARE == 9
	uint16_t i;
#endif
	uint8_t n;

	switch (s->bRequest) {
	case TEST_IDENTITY:
		*len = identity();
		return 0;
	case TEST_INSN:
		return insn(s->wValue, s->wIndex & 0xff, len);
	case TEST_PROGMEM:
		n = s->wLength > PROGMEM_MAX ? PROGMEM_MAX : s->wLength;
		pm_read(xbuf, s->wValue, n, s->wIndex);
		*len = n;
		return 0;
	case TEST_CENSUS:
		*len = census(s->wValue & 1);
		return 0;
	case TEST_TIMING:
		/* nothing may add cycles to the loop */
		irq_off();
		delay_1000(s->wValue);
		irq_on_at(SPOT_TEST);
		put16(xbuf, s->wValue);
		put16(xbuf + 2, 1000);
		*len = 4;
		return 0;
	case TEST_EVLOG:
		*len = event_log(s->wValue & 1);
		return 0;
	case TEST_RETLOG:
		/* the copy from the last restart, then the current ring */
		for (n = 0; n < sizeof(retlog_crash); n++)
			xbuf[n] = ((uint8_t *)&retlog_crash)[n];
		irq_off();
		for (n = 0; n < sizeof(retlog); n++)
			xbuf[sizeof(retlog_crash) + n] =
				((volatile uint8_t *)&retlog)[n];
#if FW_IRQ
		for (n = 0; n < sizeof(vec0_rec); n++)
			xbuf[2 * sizeof(retlog) + n] = vec0_rec[n];
#endif
		irq_on_at(SPOT_TEST);
		/* 200, see fw.h, and 6 bytes of vec0_rec with IRQ=1 */
		*len = 2 * sizeof(retlog) + (FW_IRQ ? sizeof(vec0_rec) : 0);
		return 0;
#if FW_ISR_BARE == 9
	case TEST_WIN:
		/* interrupts are disabled here, as everywhere in C */
		if (s->wIndex) {
			/* a new run: wValue the mode, wIndex the passes */
			for (i = 0; i < WREC_SIZE; i++)
				wrec[i] = 0;
			wrec[WREC_LEFT] = s->wIndex;
			wrec[WREC_MODE] = s->wValue;
			wrec[WREC_MODE2] = s->wValue >> 8;
		}
		for (i = 0; i < WREC_SIZE; i++)
			xbuf[i] = wrec[i];
		put16(xbuf + WREC_SIZE, (uint16_t)win_first);
		*len = WREC_SIZE + 2;
		return 0;
#endif
#if FW_IRQDIAG
	case TEST_IRQDIAG:
		/* wIndex bit 1 clears the statistics, bit 0 sets the knob */
		irq_off();
		if (s->wIndex & 2)
			for (n = 1; n < sizeof(irqdiag); n++)
				((volatile uint8_t *)&irqdiag)[n] = 0;
		if (s->wIndex & 1)
			irqdiag.knob = s->wValue;
		for (n = 0; n < sizeof(irqdiag); n++)
			xbuf[n] = ((volatile uint8_t *)&irqdiag)[n];
		irq_on_at(SPOT_TEST);
		*len = sizeof(irqdiag);
		return 0;
#endif
	case TEST_IRQ:
		irq_off();
		for (n = 0; n < sizeof(irqrec); n++)
			xbuf[n] = ((volatile uint8_t *)&irqrec)[n];
		irq_on_at(SPOT_TEST);
		*len = sizeof(irqrec);
		return 0;
	case TEST_CRASH:
		put16(xbuf, boot_count);
		for (n = 0; n < sizeof(dbg_crash); n++)
			xbuf[2 + n] = ((uint8_t *)&dbg_crash)[n];
		*len = 2 + sizeof(dbg_crash);
		return 0;
	}

	return 1;
}
