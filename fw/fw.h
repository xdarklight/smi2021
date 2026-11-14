/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OpenSMI21: open source firmware for the Somagic SMI2021 USB video bridge
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 */

#ifndef OPENSMI21_FW_H
#define OPENSMI21_FW_H

#include <stdint.h>
#include <avr/interrupt.h>

#define FW_VERSION_MAJOR	0
#define FW_VERSION_MINOR	1

#define STR_(x)			#x
#define STR(x)			STR_(x)
#define FW_VERSION_STR		STR(FW_VERSION_MAJOR) "." STR(FW_VERSION_MINOR)

/* USB product id, set with "make PID=0x...", 0x003c binds the Linux driver */
#ifndef FW_PID
#define FW_PID			0x003c
#endif

/* "make ALT0_STOP=0" makes alternate setting 0 a no-op like the original */
#ifndef FW_ALT0_STOP
#define FW_ALT0_STOP		1
#endif

#define SETUP_DIR_IN		0x80
#define SETUP_TYPE_MASK		0x60
#define SETUP_TYPE_STD		0x00
#define SETUP_TYPE_VENDOR	0x40
#define SETUP_RECIP_MASK	0x1f

struct setup {
	uint8_t bmRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};

/*
 * Buffer of the control transfer, for the data of OUT requests (up to a
 * register access with 15 address bytes and 255 data bytes) and for replies.
 */
#define XBUF_SIZE		288
extern uint8_t xbuf[XBUF_SIZE];

/* USB events recorded by the interrupt handler */
#define EV_SETUP		0x01
#define EV_RESET		0x02
#define EV_SPEED		0x04
#define EV_SUSPEND		0x08
extern volatile uint8_t usb_events;

/* usb.c */
extern uint8_t usb_state;
extern uint8_t usb_alt;
void usb_init(void);
void usb_poll(void);

/* vendor.c; modes of docs/protocol.md "Modes" */
#define MODE_AUDIO		2	/* audio only */
#define MODE_TS			3	/* presumably transport stream input */
#define MODE_4			4	/* not known what for */
#define MODE_CAPTURE		5	/* video and audio */
extern uint8_t cur_mode;
void stream_init(void);
void stream_start(void);
void stream_stop(void);
uint8_t vendor_in(const struct setup *s, uint16_t *len);
uint8_t vendor_out(const struct setup *s, uint16_t len);

#if FW_IRQ
/*
 * isr.S: 1 while interrupts may be enabled. irq_on() sets it before the
 * SEI and the handler of vector 11 before it returns; irq_off() clears it
 * after the CLI, and so does the handler on entry. The check at the reset
 * vector only takes an entry there for an interrupt while it is set, so
 * that a jump to 0x0000 from code running with interrupts disabled
 * restarts the firmware instead of being resumed.
 */
extern volatile uint8_t irq_armed;

/* usb.c: set once vector 11 kept firing, see irq_storm_guard() */
extern volatile uint8_t irq_storm;
#endif

static inline void irq_off(void)
{
	cli();
#if FW_IRQ
	irq_armed = 0;
#endif
}

static inline void irq_on(void)
{
#if FW_IRQ
	if (!irq_storm) {
		irq_armed = 1;
		sei();
	}
#else
	__asm__ __volatile__ ("" ::: "memory");
#endif
}

#endif /* OPENSMI21_FW_H */
