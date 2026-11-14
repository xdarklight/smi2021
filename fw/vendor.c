// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenSMI21: vendor requests, modes and the streaming engines
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * Implements docs/protocol.md "Vendor requests", "Modes" and "Register
 * access requests" with the register writes of docs/hardware.md.
 */

#include "fw.h"
#include "regs.h"

#define MODE_HEAD		0x01

#define FLAG_NADDR		0x0f
#define FLAG_INFO		0x10
#define FLAG_BUS1		0x40
#define FLAG_BRIDGE		0x80
#define CMD_READ		0x20
#define CMD_FAILED		0x01

/* Address and data bytes that the largest reply (wValue 0x0d) holds */
#define REPLY_DATA		32
/* Largest i2c transfer: the buffer at +5 to +0x24 of the engine (inferred) */
#define I2C_MAX			32
/* One try and up to 16 retries */
#define I2C_TRIES		17
/* Polls of the status register, some 20 ms at 30 MHz */
#define I2C_TIMEOUT		50000u

uint8_t cur_mode = 3;
static uint8_t started_mode;

/*
 * Bytes 1 to 36 of the reply to a register access: i2c address, command,
 * flags, size, then the address bytes and the data
 */
static uint8_t reply[4 + REPLY_DATA];

/* SRAM 0x0060-0x006f, returned by the info query; see README.md */
uint8_t infoblock[16] __attribute__((section(".infoblock"), used)) = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

/* -- modes and engines ---------------------------------------------------- */

/* docs/hardware.md "Startup", step 4: mode 3 is the initial mode */
void stream_init(void)
{
	ENG_MODE3 = 0x03;
	STREAM_SRC = 0x00;
}

static void set_block(uint16_t base, uint8_t cfg, uint8_t state)
{
	EPX_CFG(base) = cfg;
	EPX_CTRL(base) = (EPX_CTRL(base) & ~EP_STATE_MASK) | state;
}

static uint8_t set_mode(uint8_t mode)
{
	uint16_t on, off;

	switch (mode) {
	case 2:
	case 4:
		on = EPA_BASE;
		off = EPB_BASE;
		break;
	case 3:
	case 5:
		on = EPB_BASE;
		off = EPA_BASE;
		break;
	default:
		return 1;
	}

	set_block(off, EPX_CFG_EP81, EP_STATE_OFF);
	set_block(on, EPX_CFG_EP82, EP_STATE_NORMAL);
	EPB_REG8 = 0;
	EPB_REG12 = 0;

	switch (mode) {
	case 2:
		AUDIO_CTRL = 0;
		STREAM_SRC = 0;
		break;
	case 3:
		ENG_MODE3 = 0x03;
		STREAM_SRC = 0;
		break;
	case 4:
		ENG_MODE4 = 0;
		STREAM_SRC = 2;
		break;
	case 5:
		VIDEO_CTRL = 0;
		AUDIO_CTRL = 0;
		STREAM_SRC = 1;
		break;
	}

	cur_mode = mode;

	return 0;
}

/* SET_INTERFACE to a non-zero alternate setting */
void stream_start(void)
{
	switch (cur_mode) {
	case 2:
		AUDIO_CTRL |= 0x05;
		break;
	case 3:
		ENG_MODE3 |= ENG_MODE3_ON;
		break;
	case 4:
		ENG_MODE4 = 1;
		break;
	case 5:
		VIDEO_CTRL |= 0x0d;
		AUDIO_CTRL |= 0x05;
		break;
	}
	STREAM_CTRL |= STREAM_CTRL_ON;
	started_mode = cur_mode;
}

/*
 * Going back to alternate setting 0 clears what stream_start() set for the
 * mode it started, see README.md. The original firmware does nothing here,
 * which "make ALT0_STOP=0" restores.
 */
void stream_stop(void)
{
#if FW_ALT0_STOP
	switch (started_mode) {
	case 2:
		AUDIO_CTRL &= ~0x05;
		break;
	case 3:
		ENG_MODE3 &= ~ENG_MODE3_ON;
		break;
	case 4:
		ENG_MODE4 = 0;
		break;
	case 5:
		VIDEO_CTRL &= ~0x0d;
		AUDIO_CTRL &= ~0x05;
		break;
	}
	if (started_mode)
		STREAM_CTRL &= ~STREAM_CTRL_ON;
	started_mode = 0;
#endif
}

/* -- i2c ------------------------------------------------------------------ */

/* docs/hardware.md "I2C engines"; returns 1 if the last try failed */
static uint8_t i2c_xfer(uint16_t base, uint8_t slave, uint8_t cmd,
			uint8_t sub, const uint8_t *wdata, uint8_t n,
			uint8_t *rdata)
{
	uint8_t i, tries, st = 0, failed = 1;
	uint16_t t;

	if (!(cmd & CMD_READ))
		for (i = 0; i < n; i++)
			REG8(base + I2C_DATA + i) = wdata[i];

	for (tries = 0; tries < I2C_TRIES && failed; tries++) {
		if (REG8(base + I2C_STATUS) & I2C_ST_BUSY) {
			for (t = I2C_TIMEOUT; t; t--)
				if (REG8(base + I2C_STATUS) & I2C_ST_DONE)
					break;
			REG8(base + I2C_STATUS) = 0;
		}

		REG8(base + I2C_SLAVE) = slave;
		REG8(base + I2C_SUBADDR) = sub;
		REG8(base + I2C_COUNT) = n;
		REG8(base + I2C_CMD) = cmd;

		for (t = I2C_TIMEOUT; t; t--) {
			st = REG8(base + I2C_STATUS);
			if (!(st & I2C_ST_BUSY) || (st & I2C_ST_DONE))
				break;
		}

		failed = !t || (st & I2C_ST_FAIL);
		REG8(base + I2C_STATUS) = 0;
	}

	if (cmd & CMD_READ)
		for (i = 0; i < n; i++)
			rdata[i] = REG8(base + I2C_DATA + i);

	return failed;
}

/* -- register access ------------------------------------------------------ */

static uint8_t reg_access(uint16_t len)
{
	const uint8_t *p = xbuf + 5;
	uint8_t *data = reply + 4;
	uint8_t cmd, flags, size, naddr, i, n;
	uint16_t addr;

	if (len < 5)
		return 1;

	cmd = xbuf[2];
	flags = xbuf[3];
	size = xbuf[4];
	naddr = flags & FLAG_NADDR;

	if (flags & FLAG_INFO) {
		for (i = 0; i < 4; i++)
			reply[i] = xbuf[1 + i];
		for (i = 0; i < REPLY_DATA; i++)
			data[i] = i < sizeof(infoblock) ? infoblock[i] : 0;
		return 0;
	}

	/* the checks of docs/protocol.md "Register access requests" */
	if ((flags & FLAG_BRIDGE) && naddr != 2)
		return 1;
	if ((cmd & 0xc0) == 0x80 && naddr != 0)
		return 1;
	if ((cmd & 0xc0) == 0xc0 && naddr != 1)
		return 1;

	/* and bounds: a read must fit the reply, a write the request */
	if (cmd & CMD_READ) {
		if (naddr + size > REPLY_DATA)
			return 1;
	} else if ((uint16_t)(5 + naddr + size) > len) {
		return 1;
	}
	if (!(flags & FLAG_BRIDGE) && size > I2C_MAX)
		return 1;

	for (i = 0; i < 4; i++)
		reply[i] = xbuf[1 + i];
	for (i = 0; i < REPLY_DATA; i++)
		data[i] = i < naddr ? p[i] : 0;

	/* a write repeats as much of its data in the reply as fits */
	n = REPLY_DATA - naddr;
	if (!(cmd & CMD_READ))
		for (i = 0; i < size && i < n; i++)
			data[naddr + i] = p[naddr + i];

	if (flags & FLAG_BRIDGE) {
		addr = ((uint16_t)p[0] << 8) | p[1];
		if (cmd & CMD_READ)
			for (i = 0; i < size; i++)
				data[2 + i] = REG8(addr + i);
		else
			for (i = 0; i < size; i++)
				REG8(addr + i) = p[2 + i];
		return 0;
	}

	if (i2c_xfer((flags & FLAG_BUS1) ? I2C1_BASE : I2C0_BASE, xbuf[1], cmd,
		     p[0], p + naddr, size, data + naddr))
		reply[1] |= CMD_FAILED;

	return 0;
}

/* -- requests ------------------------------------------------------------- */

uint8_t vendor_out(const struct setup *s, uint16_t len)
{
	(void)s;	/* bRequest and wValue are ignored, as in the original */

	/* nothing to do for a request without data, but acknowledge it */
	if (len == 0)
		return 0;

	if (xbuf[0] == MODE_HEAD)
		return len < 2 ? 1 : set_mode(xbuf[1]);

	return reg_access(len);
}

uint8_t vendor_in(const struct setup *s, uint16_t *len)
{
	uint8_t v = s->wValue & 0xff;
	uint8_t i, n;

	if (s->bRequest != 0x01)
		return 1;

	switch (v) {
	case MODE_HEAD:
		xbuf[1] = cur_mode;
		n = 2;
		break;
	case 0x0b:
		n = 13;
		break;
	case 0x0c:
		n = 21;
		break;
	case 0x0d:
		n = 37;
		break;
	case 0xfb:
		SYS_CTRL &= ~SYS_CTRL_REQ_FB;
		xbuf[1] = 0x01;
		n = 2;
		break;
	default:
		return 1;
	}

	if (v >= 0x0b && v <= 0x0d)
		for (i = 1; i < n; i++)
			xbuf[i] = reply[i - 1];

	xbuf[0] = v;
	*len = n;

	return 0;
}
