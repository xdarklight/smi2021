// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenSMI21: open source firmware for the Somagic SMI2021 USB video bridge
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * Brings the device up as docs/hardware.md "Startup" describes; avr-libc's
 * startup code has already done step 1.
 */

#include "fw.h"
#include "regs.h"

#if FW_IRQ
/*
 * Kept across a jump to the reset vector, random after power up: tells the
 * check at the reset vector in isr.S that the firmware runs already
 */
#define BOOT_MAGIC	0x4f53
uint16_t boot_magic __attribute__((section(".noinit")));
#endif

int main(void)
{
#if FW_IRQ
	boot_magic = BOOT_MAGIC;
#endif

	DDRA = 0xff;

	REG8(I2C0_BASE + I2C_STATUS) = 0;
	REG8(I2C1_BASE + I2C_STATUS) = 0;

	stream_init();
	usb_init();

	irq_on();

	for (;;)
		usb_poll();
}
