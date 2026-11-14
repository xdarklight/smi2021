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

/* Version of the test request interface, see README.md */
#define TEST_IF_VERSION		2

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

/* Interrupt census, filled by the handlers in isr.S and the USB handler */
#define NUM_VECTORS		24
extern volatile uint16_t irq_census[NUM_VECTORS];
extern volatile uint8_t irq_last;

/* USB event log, see the 0xf5 test request */
#define EVLOG_SIZE		16
extern volatile uint8_t evlog[EVLOG_SIZE * 2];
extern volatile uint8_t evlog_pos;
extern volatile uint8_t evlog_count;
extern volatile uint16_t usb_unknown_events;

/* Survives a reset, counts the starts of the firmware */
extern uint16_t boot_count;

/*
 * Crash record, test request 0xf6. dbg_now is kept across a restart that
 * doesn't lose power; main() copies it to dbg_crash when it finds one, as
 * the enumeration after the restart overwrites it.
 */
#define DBG_SETUPS	8
struct dbg_record {
	uint16_t reset_sp;		/* SP at reset, see isr.S */
	uint8_t phase;			/* of the last control request */
	uint8_t pos;			/* next entry of setup[] */
	uint8_t setup[DBG_SETUPS][8];
};
extern struct dbg_record dbg_now, dbg_crash;
extern uint16_t reset_sp;

/* usb.c */
extern uint8_t usb_state;
extern uint8_t usb_alt;
void usb_init(void);
void usb_poll(void);

/* vendor.c */
extern uint8_t cur_mode;
void stream_init(void);
void stream_start(void);
void stream_stop(void);
uint8_t vendor_in(const struct setup *s, uint16_t *len);
uint8_t vendor_out(const struct setup *s, uint16_t len);

/* test.c */
uint8_t test_in(const struct setup *s, uint16_t *len);

/* main.c */
void delay_160(uint16_t units);			/* about 160 cycles per unit */

/* asm.S */
void delay_1000(uint16_t units);		/* exactly 1000 cycles per unit */
void insn_mul(uint8_t *out);
void insn_movw(uint8_t *out);
void insn_lpm_z(uint8_t *out);
void insn_lpm_zp(uint8_t *out);
void insn_elpm(uint8_t *out, uint8_t rampz);
void insn_elpm_z(uint8_t *out, uint8_t rampz);
void insn_elpm_zp(uint8_t *out, uint8_t rampz);
extern const uint8_t insn_pattern[];
void pm_read(uint8_t *dst, uint16_t addr, uint8_t len, uint16_t elpm);
uint8_t flash_byte(const void *addr);	/* plain LPM */

/* Linker symbols */
extern const uint8_t __image_end[];

static inline void irq_off(void)
{
	cli();
}

/* Interrupt test, request 0xf7 */
#define IRQLOG_SIZE	8
struct irq_record {
	uint32_t v11;			/* times vector 11 was taken */
	uint8_t storm;			/* interrupts were switched off for good */
	uint8_t n;			/* entries in log[] */
	uint8_t log[IRQLOG_SIZE][4];	/* 0x1503, 0x1502, 0x151c, 0x1518 */
	/*
	 * For the first setup packets the main loop finds: 0x1503 and 0x1502,
	 * each read twice in a row, to tell whether reading them changes them.
	 */
	uint8_t nd;
	uint8_t dbl[IRQLOG_SIZE][4];
};
extern volatile struct irq_record irqrec;

/*
 * Interrupt test ISR_BARE=5, request 0xf8: at each entry of vector 11, the
 * return address on the stack, as two raw bytes, and SREG as found on
 * entry, in a ring; and how deep the handler nested. Kept across a restart
 * without power loss and copied aside then, see main().
 */
#define RETLOG_SIZE	32
struct ret_record {
	uint8_t pos;			/* entries ever written, wraps */
	uint8_t depth;			/* handlers running right now */
	uint8_t maxdepth;		/* the most that ever ran at once */
	uint8_t pad;
	uint8_t addr[RETLOG_SIZE][3];
};
extern volatile struct ret_record retlog;
extern struct ret_record retlog_crash;

static inline void irq_on(void)
{
#if FW_IRQ
	if (!irqrec.storm)
		sei();
#else
	__asm__ __volatile__ ("" ::: "memory");
#endif
}

#endif /* OPENSMI21_FW_H */
