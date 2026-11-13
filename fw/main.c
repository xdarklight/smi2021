// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenSMI21: open source firmware for the Somagic SMI2021 USB video bridge
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * Brings the device up as docs/hardware.md "Startup" describes; avr-libc's
 * startup code has
 * already done step 1.
 */

#include <util/delay_basic.h>

#include "fw.h"
#include "regs.h"

#define BOOT_MAGIC	0x4f53

volatile uint16_t irq_census[NUM_VECTORS];
volatile uint8_t irq_last = 0xff;

/* Kept across a jump to the reset vector; random after power up */
uint16_t boot_count __attribute__((section(".noinit")));
static uint16_t boot_magic __attribute__((section(".noinit")));
uint16_t reset_sp __attribute__((section(".noinit")));
struct dbg_record dbg_now __attribute__((section(".noinit")));
struct dbg_record dbg_crash;

/*
 * The delay unit of the original bring-up, about 160 cycles: 39 passes of
 * avr-libc's 4 cycle loop and the loop around it. Only the order of
 * magnitude matters, see docs/hardware.md "Startup".
 */
void delay_160(uint16_t units)
{
	while (units--)
		_delay_loop_2(39);
}

int main(void)
{
	uint8_t i;

	if (boot_magic != BOOT_MAGIC) {
		boot_magic = BOOT_MAGIC;
		boot_count = 0;
	} else {
		/* a restart without power loss: keep what led to it */
		for (i = 0; i < sizeof(dbg_crash); i++)
			((uint8_t *)&dbg_crash)[i] = ((uint8_t *)&dbg_now)[i];
		dbg_crash.reset_sp = reset_sp;
	}
	for (i = 0; i < sizeof(dbg_now); i++)
		((uint8_t *)&dbg_now)[i] = 0;
	boot_count++;
	irq_census[0] = boot_count;

	DDRA = 0xff;

	REG8(I2C0_BASE + I2C_STATUS) = 0;
	REG8(I2C1_BASE + I2C_STATUS) = 0;

	stream_init();
	usb_init();

	irq_on();

	for (;;)
		usb_poll();
}
