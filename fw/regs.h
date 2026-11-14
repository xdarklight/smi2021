/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register map of the Somagic SMI2021, as described in docs/hardware.md
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * The core is an ATmega103 (docs/hardware.md "CPU core"), whose registers
 * come from avr-libc's <avr/io.h>, with a bidirectional port C: avr-libc
 * follows the real ATmega103, whose port C is an output only, so DDRC and
 * PINC are defined here. Everything above 0x1000 is Somagic's own hardware
 * and accessed as volatile memory.
 */

#ifndef OPENSMI21_REGS_H
#define OPENSMI21_REGS_H

#include <stdint.h>
#include <avr/io.h>

#define REG8(addr)	(*(volatile uint8_t *)(uint16_t)(addr))

/* Port C as an input too, at the I/O addresses next to PORTC */
#define PINC		_SFR_IO8(0x13)
#define DDRC		_SFR_IO8(0x14)

/* USB device controller, 0x1500-0x157f */
#define USB_CTRL	REG8(0x1500)	/* bit 3: disconnect, bit 2: suspend */
#define USB_CTRL_DISCONNECT	0x08
#define USB_CTRL_SUSPEND	0x04
#define USB_SPEED	REG8(0x1501)	/* bit 1: high speed */
#define USB_SPEED_HIGH		0x02
#define USB_STATUS	REG8(0x1502)	/* bit 4: setup packet received */
#define USB_STATUS_SETUP	0x10
#define USB_EVENT	REG8(0x1503)	/* code of the last event, see docs */
#define USB_ADDR	REG8(0x1504)
#define USB_IRQ_CFG	REG8(0x1507)	/* 0xc4 at start, meaning unknown */
#define USB_TESTMODE	REG8(0x1509)	/* inferred, unused */

#define USB_EVT_SETUP		0x01
#define USB_EVT_SPEED		0x80
#define USB_EVT_RESET		0x81
#define USB_EVT_SUSPEND		0x85

/* Control endpoint */
#define EP0_CTRL	REG8(0x1510)	/* bits 2-1: 00 normal, 10 stall */
#define EP0_CFG		REG8(0x1511)
#define EP0_MPS_A	REG8(0x1513)
#define EP0_MPS_B	REG8(0x1515)
#define EP0_RX_STAT	REG8(0x1518)	/* bit 7: receive buffer full */
#define EP0_RX_ADDR_L	REG8(0x1519)
#define EP0_RX_ADDR_H	REG8(0x151a)
#define EP0_TX_LEN	REG8(0x151b)
#define EP0_TX_STAT	REG8(0x151c)	/* bit 7: packet armed */
#define EP0_TX_ADDR_L	REG8(0x151d)
#define EP0_TX_ADDR_H	REG8(0x151e)

#define EP_STATE_MASK	0x06		/* bits 2-1 of +0 */
#define EP_STATE_NORMAL	0x00
#define EP_STATE_OFF	0x02		/* "01" */
#define EP_STATE_STALL	0x04		/* "10" */
#define EP_FULL		0x80

/* USB buffer memory of the control endpoint */
#define EP0_RX_BUF	0x2000
#define EP0_TX_BUF	0x2040
#define EP0_MPS		64

/* Endpoint register blocks A and B */
#define EPA_BASE	0x1520
#define EPB_BASE	0x1530
#define EPX_CTRL(b)	REG8((b) + 0)
#define EPX_CFG(b)	REG8((b) + 1)
#define EPX_MPS_L(b)	REG8((b) + 3)
#define EPX_MPS_H(b)	REG8((b) + 4)
#define EPB_REG8	REG8(EPB_BASE + 8)
#define EPB_REG12	REG8(EPB_BASE + 12)

#define EPX_CFG_EP82	0x25		/* written to the block that sends */
#define EPX_CFG_EP81	0x15		/* written to the other block */

/* Streaming engines */
#define ENG_MODE3	REG8(0x1580)
#define STREAM_CTRL	REG8(0x1600)	/* bit 7: streaming started */
#define AUDIO_CTRL	REG8(0x1740)
#define VIDEO_CTRL	REG8(0x1800)
#define STREAM_SRC	REG8(0x1880)
#define SYS_CTRL	REG8(0x3080)	/* bit 7: cleared on request, 4: suspend */
#define ENG_MODE4	REG8(0x3100)

/* I2C engines */
#define I2C0_BASE	0x1780
#define I2C1_BASE	0x3180
#define I2C_CMD		0
#define I2C_COUNT	1
#define I2C_STATUS	2
#define I2C_SUBADDR	3
#define I2C_SLAVE	4
#define I2C_DATA	5
#define I2C_ST_BUSY	0x10
#define I2C_ST_FAIL	0x20
#define I2C_ST_DONE	0xc0

#endif /* OPENSMI21_REGS_H */
