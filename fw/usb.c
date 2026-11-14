// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenSMI21: USB device controller, control endpoint and standard requests
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * The controller is programmed as docs/hardware.md "USB device controller"
 * describes it. The interrupt handler only records events; the main loop
 * handles them, and handles each control transfer from its setup packet to
 * its status stage before it looks at the next event. Every wait of a
 * control transfer gives up when a new setup packet or a bus reset arrives.
 */

#include <util/delay.h>

#include "fw.h"
#include "regs.h"

#define PROGMEM		__attribute__((section(".progmem.data")))

/* Standard requests */
#define REQ_GET_STATUS		0x00
#define REQ_CLEAR_FEATURE	0x01
#define REQ_SET_FEATURE		0x03
#define REQ_SET_ADDRESS		0x05
#define REQ_GET_DESCRIPTOR	0x06
#define REQ_GET_CONFIGURATION	0x08
#define REQ_SET_CONFIGURATION	0x09
#define REQ_GET_INTERFACE	0x0a
#define REQ_SET_INTERFACE	0x0b

#define RECIP_DEVICE		0
#define RECIP_INTERFACE		1
#define RECIP_ENDPOINT		2

#define DT_DEVICE		1
#define DT_CONFIG		2
#define DT_STRING		3
#define DT_QUALIFIER		6
#define DT_OTHER_SPEED		7

#define FEAT_ENDPOINT_HALT	0

#define STREAM_EP		0x82

#define STATE_DEFAULT		0
#define STATE_ADDRESS		1
#define STATE_CONFIGURED	2

uint8_t xbuf[XBUF_SIZE];
volatile uint8_t usb_events;
volatile uint8_t evlog[EVLOG_SIZE * 2];
volatile uint8_t evlog_pos;
volatile uint8_t evlog_count;
volatile uint16_t usb_unknown_events;

uint8_t usb_state;
uint8_t usb_alt;
static uint8_t usb_config;
static uint8_t ep82_halted;

#define LE16(x)		((x) & 0xff), ((x) >> 8)
#define BCD_DEVICE	((FW_VERSION_MAJOR << 8) | FW_VERSION_MINOR)

static const uint8_t dev_desc[18] PROGMEM = {
	18, DT_DEVICE,
	LE16(0x0200),		/* bcdUSB */
	0x00, 0x00, 0x00,	/* class in the interface descriptor */
	EP0_MPS,
	LE16(0x1c88),		/* idVendor */
	LE16(FW_PID),		/* idProduct */
	LE16(BCD_DEVICE),
	1, 2, 3,		/* manufacturer, product, serial number */
	1,			/* bNumConfigurations */
};

static const uint8_t qual_desc[10] PROGMEM = {
	10, DT_QUALIFIER,
	LE16(0x0200),
	0x00, 0x00, 0x00,
	EP0_MPS,
	1,			/* bNumConfigurations */
	0,
};

#define CFG_LEN		(9 + 9 + 9 + 7 + 9 + 7)

#define CONFIG_HEAD \
	9, DT_CONFIG, LE16(CFG_LEN), \
	1,			/* bNumInterfaces */ \
	1,			/* bConfigurationValue */ \
	0,			/* iConfiguration */ \
	0x80,			/* bus powered, no remote wakeup */ \
	250			/* 500 mA */

#define INTERFACE(alt, neps) \
	9, 4, 0, (alt), (neps), 0xff, 0xff, 0xff, 0

#define ISO_IN_EP(mps) \
	7, 5, STREAM_EP, 0x01, LE16(mps), 1

/* High speed: 1023 bytes and 3 x 1024 bytes per microframe */
static const uint8_t cfg_hs[CFG_LEN] PROGMEM = {
	CONFIG_HEAD,
	INTERFACE(0, 0),
	INTERFACE(1, 1),
	ISO_IN_EP(0x03ff),
	INTERFACE(2, 1),
	ISO_IN_EP(0x1400),
};

/* Full speed: 1023 bytes per frame for both alternate settings */
static const uint8_t cfg_fs[CFG_LEN] PROGMEM = {
	CONFIG_HEAD,
	INTERFACE(0, 0),
	INTERFACE(1, 1),
	ISO_IN_EP(0x03ff),
	INTERFACE(2, 1),
	ISO_IN_EP(0x03ff),
};

static const char str_manufacturer[] PROGMEM = "OpenSMI21";
static const char str_product[] PROGMEM = "SMI2021 open source firmware";
static const char str_serial[] PROGMEM = "OPENSMI21-" FW_VERSION_STR;

/* -- events --------------------------------------------------------------- */

static uint8_t last_code;

/*
 * docs/hardware.md "Interrupt": the event code in 0x1503 is not
 * acknowledged by reading it and stays until the next event, so this is
 * called from vector 11, from the main loop and from every wait for the
 * control endpoint. A setup packet is told by 0x1502 bit 4; the other codes
 * are recorded again on every call, which their handlers don't mind. Only
 * a change of the code goes into the log. Called with interrupts off.
 */
static void usb_check_events(void)
{
	uint8_t code = USB_EVENT;
	uint8_t status = 0;
	uint8_t pos = evlog_pos;
	uint8_t log = code != last_code;

	switch (code) {
	case USB_EVT_SETUP:
		status = USB_STATUS;
		if (status & USB_STATUS_SETUP) {
			uint8_t nd = irqrec.nd;

			if (nd < IRQLOG_SIZE) {
				irqrec.dbl[nd][0] = code;
				irqrec.dbl[nd][1] = USB_EVENT;
				irqrec.dbl[nd][2] = status;
				irqrec.dbl[nd][3] = USB_STATUS;
				irqrec.nd = nd + 1;
			}
			USB_STATUS &= ~USB_STATUS_SETUP;
			/* withdraw a packet of the previous transfer */
			EP0_TX_STAT &= ~EP_FULL;
			usb_events |= EV_SETUP;
			log = 1;
		}
		break;
	case USB_EVT_SPEED:
		usb_events |= EV_SPEED;
		break;
	case USB_EVT_RESET:
		/* a setup packet from before the reset is stale */
		usb_events = (usb_events & ~EV_SETUP) | EV_RESET;
		break;
	case USB_EVT_SUSPEND:
		usb_events |= EV_SUSPEND;
		break;
	default:
		if (log && usb_unknown_events != 0xffff)
			usb_unknown_events++;
		break;
	}

	last_code = code;
	if (!log)
		return;
	evlog[pos * 2] = code;
	evlog[pos * 2 + 1] = status;
	evlog_pos = (pos + 1) & (EVLOG_SIZE - 1);
	if (evlog_count != 0xff)
		evlog_count++;
}

volatile struct irq_record irqrec;

#if FW_IRQ
/* interrupts taken, for the storm guard: read with interrupts enabled */
static volatile uint16_t irq_ticks;
#endif

#if FW_ISR_BARE != 1 && FW_ISR_BARE != 5 && FW_ISR_BARE != 6 && \
	FW_ISR_BARE != 7 && FW_ISR_BARE != 9
#if FW_ISR_BARE == 10
/*
 * Interrupt test: vector 11 in isr.S notes its return address and jumps
 * here; __vector_24 is no vector of the table, the name only makes avr-gcc
 * build an interrupt handler (and pushguard.awk leave it alone)
 */
#define USB_ISR	__vector_24
#else
#define USB_ISR	__vector_11
#endif
static inline __attribute__((always_inline)) void usb_isr(void)
{
	uint8_t n = irqrec.n;

	if (irq_census[11] != 0xffff)
		irq_census[11]++;
	irqrec.v11++;
#if FW_ISR_BARE == 2
	/* interrupt test: touch no register of the controller */
	(void)n;
	return;
#elif FW_ISR_BARE == 3
	/* interrupt test: read the event code and nothing else */
	if (n < IRQLOG_SIZE) {
		irqrec.log[n][0] = USB_EVENT;
		irqrec.n = n + 1;
	} else {
		(void)USB_EVENT;
	}
	return;
#elif FW_ISR_BARE == 4
	/* interrupt test: read the status register and nothing else */
	if (n < IRQLOG_SIZE) {
		irqrec.log[n][1] = USB_STATUS;
		irqrec.n = n + 1;
	} else {
		(void)USB_STATUS;
	}
	return;
#endif
	if (n < IRQLOG_SIZE) {
		irqrec.log[n][0] = USB_EVENT;
		irqrec.log[n][1] = USB_STATUS;
		irqrec.log[n][2] = EP0_TX_STAT;
		irqrec.log[n][3] = EP0_RX_STAT;
		irqrec.n = n + 1;
	}
#if FW_ISR_EVENTS
	usb_check_events();
#endif
}

void USB_ISR(void) __attribute__((signal, used, externally_visible));
void USB_ISR(void)
{
#if FW_IRQ
	irq_armed = 0;
	irq_ticks++;
#endif
	usb_isr();
#if FW_IRQ
	irq_armed = 1;			/* RETI enables interrupts again */
#endif
}
#endif

/*
 * Should vector 11 keep firing, the main loop would barely run: switch
 * interrupts off for good once it was taken more than 50 times between two
 * passes of the main loop.
 */
#if FW_IRQ
/*
 * With interrupts enabled: it must not disable them, see usb_poll(), so it
 * reads irq_ticks twice, until both reads agree.
 */
static void irq_storm_guard(void)
{
	static uint16_t last;
	uint16_t now, again;

	do {
		now = irq_ticks;
		again = irq_ticks;
	} while (now != again);
	if ((uint16_t)(now - last) > 50) {
		irq_off();
		irqrec.storm = 1;
	}
	last = now;
}
#else
static void irq_storm_guard(void)
{
	static uint32_t last;
	uint32_t now;

	irq_off();
	now = irqrec.v11;
	if (now - last > 50)
		irqrec.storm = 1;
	last = now;
}
#endif

/* -- control endpoint ----------------------------------------------------- */

static uint8_t ctrl_aborted(void)
{
	uint8_t sreg = SREG;
	uint8_t r;

	irq_off();
#if FW_IRQ
	/* the handler services the events, see usb_poll() */
	if (irqrec.storm)
		usb_check_events();
	r = usb_events & (EV_SETUP | EV_RESET);
	if (sreg & (1 << SREG_I))
		irq_armed = 1;
#else
	usb_check_events();
	r = usb_events & (EV_SETUP | EV_RESET);
#endif
	SREG = sreg;

	return r;
}

static void ep0_stall(void)
{
	EP0_CTRL = (EP0_CTRL & ~EP_STATE_MASK) | EP_STATE_STALL;
}

static uint8_t ep0_wait_tx_idle(void)
{
	while (EP0_TX_STAT & EP_FULL)
		if (ctrl_aborted())
			return 0;

	return 1;
}

/* Send one packet of up to 64 bytes, 0 for a packet of zero bytes */
static uint8_t ep0_tx(const uint8_t *p, uint8_t n)
{
	uint8_t i;

	if (!ep0_wait_tx_idle())
		return 0;

	for (i = 0; i < n; i++)
		REG8(EP0_TX_BUF + i) = p[i];
	EP0_TX_LEN = n;

	/* the interrupt handler withdraws packets, don't race with it */
	irq_off();
	if (ctrl_aborted()) {
		irq_on();
		return 0;
	}
	EP0_TX_STAT |= EP_FULL;
	irq_on();

	return 1;
}

/* Receive one packet; there is no length register, n is what is expected */
static uint8_t ep0_rx(uint8_t *p, uint8_t n)
{
	uint8_t i;

	while (!(EP0_RX_STAT & EP_FULL))
		if (ctrl_aborted())
			return 0;

	for (i = 0; i < n; i++)
		p[i] = REG8(EP0_RX_BUF + i);
	EP0_RX_STAT &= ~EP_FULL;

	return 1;
}

/*
 * IN data stage of len bytes from xbuf, cut to wLength. A transfer that ends
 * on a full packet before wLength gets a packet of zero bytes. With wLength
 * 0 this sends the zero length status packet of a request without data.
 */
static void ep0_send(uint16_t len, uint16_t wlength)
{
	const uint8_t *p = xbuf;
	uint8_t chunk;

	if (len > wlength)
		len = wlength;

	do {
		chunk = len > EP0_MPS ? EP0_MPS : len;
		if (!ep0_tx(p, chunk))
			return;
		p += chunk;
		len -= chunk;
	} while (len);

	if (chunk == EP0_MPS && (uint16_t)(p - xbuf) < wlength)
		ep0_tx(p, 0);
}

static void copy_flash(const uint8_t *src, uint8_t n)
{
	uint8_t i;

	for (i = 0; i < n; i++)
		xbuf[i] = flash_byte(src + i);
}

static uint8_t string_desc(const char *s)
{
	uint8_t n = 2;
	uint8_t c;

	while ((c = flash_byte(s++)) != 0) {
		xbuf[n++] = c;
		xbuf[n++] = 0;
	}
	xbuf[0] = n;
	xbuf[1] = DT_STRING;

	return n;
}

/* -- endpoint register blocks --------------------------------------------- */

static void block_set_state(uint16_t base, uint8_t state)
{
	EPX_CTRL(base) = (EPX_CTRL(base) & ~EP_STATE_MASK) | state;
}

/*
 * The block that the current mode made endpoint 0x82 by writing 0x25 to it,
 * see the mode table in docs/hardware.md "Endpoint register blocks".
 */
static uint16_t ep82_block(void)
{
	return (cur_mode == 2 || cur_mode == 4) ? EPA_BASE : EPB_BASE;
}

static void set_block_mps(uint16_t mps)
{
	EPX_MPS_L(EPA_BASE) = mps & 0xff;
	EPX_MPS_H(EPA_BASE) = mps >> 8;
	EPX_MPS_L(EPB_BASE) = mps & 0xff;
	EPX_MPS_H(EPB_BASE) = mps >> 8;
}

/* -- standard requests ---------------------------------------------------- */

static uint8_t get_descriptor(const struct setup *s, uint16_t *len)
{
	uint8_t type = s->wValue >> 8;
	uint8_t index = s->wValue & 0xff;
	uint8_t hs = USB_SPEED & USB_SPEED_HIGH;

	switch (type) {
	case DT_DEVICE:
		copy_flash(dev_desc, sizeof(dev_desc));
		*len = sizeof(dev_desc);
		return 0;
	case DT_QUALIFIER:
		copy_flash(qual_desc, sizeof(qual_desc));
		*len = sizeof(qual_desc);
		return 0;
	case DT_CONFIG:
	case DT_OTHER_SPEED:
		if (index != 0)
			return 1;
		if (type == DT_OTHER_SPEED)
			hs = !hs;
		copy_flash(hs ? cfg_hs : cfg_fs, CFG_LEN);
		xbuf[1] = type;
		*len = CFG_LEN;
		return 0;
	case DT_STRING:
		switch (index) {
		case 0:
			xbuf[0] = 4;
			xbuf[1] = DT_STRING;
			xbuf[2] = 0x09;		/* English (United States) */
			xbuf[3] = 0x04;
			*len = 4;
			return 0;
		case 1:
			*len = string_desc(str_manufacturer);
			return 0;
		case 2:
			*len = string_desc(str_product);
			return 0;
		case 3:
			*len = string_desc(str_serial);
			return 0;
		}
		return 1;
	}

	return 1;
}

static uint8_t std_in(const struct setup *s, uint16_t *len)
{
	uint8_t recip = s->bmRequestType & SETUP_RECIP_MASK;
	uint8_t ep = s->wIndex & 0xff;

	switch (s->bRequest) {
	case REQ_GET_STATUS:
		xbuf[0] = 0;
		xbuf[1] = 0;
		*len = 2;
		if (recip == RECIP_DEVICE)
			return 0;
		if (recip == RECIP_INTERFACE)
			return !(usb_state == STATE_CONFIGURED && s->wIndex == 0);
		if (recip != RECIP_ENDPOINT)
			return 1;
		if (ep == 0x00 || ep == 0x80)
			return 0;
		if (ep == STREAM_EP && usb_state == STATE_CONFIGURED) {
			xbuf[0] = ep82_halted;
			return 0;
		}
		return 1;
	case REQ_GET_DESCRIPTOR:
		return get_descriptor(s, len);
	case REQ_GET_CONFIGURATION:
		xbuf[0] = usb_config;
		*len = 1;
		return 0;
	case REQ_GET_INTERFACE:
		if (usb_state != STATE_CONFIGURED || s->wIndex != 0)
			return 1;
		xbuf[0] = usb_alt;
		*len = 1;
		return 0;
	}

	return 1;
}

/* Returns 0 to acknowledge, 1 to stall; *addr is set for SET_ADDRESS */
static uint8_t std_out(const struct setup *s, uint8_t *addr)
{
	uint8_t recip = s->bmRequestType & SETUP_RECIP_MASK;
	uint8_t ep = s->wIndex & 0xff;

	switch (s->bRequest) {
	case REQ_SET_ADDRESS:
		if (s->wValue > 127)
			return 1;
		*addr = s->wValue;
		return 0;
	case REQ_CLEAR_FEATURE:
	case REQ_SET_FEATURE:
		/* only the halt feature of the endpoints is supported */
		if (recip != RECIP_ENDPOINT || s->wValue != FEAT_ENDPOINT_HALT)
			return 1;
		if (ep == 0x00 || ep == 0x80)
			return s->bRequest == REQ_SET_FEATURE;
		if (ep != STREAM_EP || usb_state != STATE_CONFIGURED)
			return 1;
		if (s->bRequest == REQ_SET_FEATURE) {
			block_set_state(ep82_block(), EP_STATE_STALL);
			ep82_halted = 1;
		} else {
			block_set_state(ep82_block(), EP_STATE_NORMAL);
			/* resets the data toggle (inferred) */
			EPX_CTRL(ep82_block()) &= ~0x01;
			ep82_halted = 0;
		}
		return 0;
	case REQ_SET_CONFIGURATION:
		if (s->wValue > 1)
			return 1;
		if (usb_alt)
			stream_stop();
		usb_alt = 0;
		ep82_halted = 0;
		usb_config = s->wValue;
		if (usb_config) {
			block_set_state(EPA_BASE, EP_STATE_NORMAL);
			block_set_state(EPB_BASE, EP_STATE_NORMAL);
			usb_state = STATE_CONFIGURED;
		} else {
			usb_state = STATE_ADDRESS;
		}
		return 0;
	case REQ_SET_INTERFACE:
		if (usb_state != STATE_CONFIGURED || s->wIndex != 0 ||
		    s->wValue > 2)
			return 1;
		if (s->wValue) {
			usb_alt = s->wValue;
			stream_start();
		} else {
			if (usb_alt)
				stream_stop();
			usb_alt = 0;
		}
		return 0;
	}

	return 1;
}

/* -- events --------------------------------------------------------------- */

static void handle_setup(void)
{
	uint8_t *raw;
	struct setup s;
	uint8_t type, chunk, err, addr = 0xff;
	uint16_t len = 0, off;

	while (!(EP0_RX_STAT & EP_FULL))
		if (ctrl_aborted())
			return;

	raw = (uint8_t *)&s;
	for (off = 0; off < sizeof(s); off++) {
		raw[off] = REG8(EP0_RX_BUF + off);
		dbg_now.setup[dbg_now.pos][off] = raw[off];
	}
	dbg_now.pos = (dbg_now.pos + 1) & (DBG_SETUPS - 1);
	dbg_now.phase = 1;
	EP0_RX_STAT &= ~EP_FULL;

	EP0_TX_ADDR_L = EP0_TX_BUF & 0xff;
	EP0_TX_ADDR_H = EP0_TX_BUF >> 8;
	/* in case the hardware does not leave the stall by itself */
	EP0_CTRL &= ~EP_STATE_MASK;

	type = s.bmRequestType & SETUP_TYPE_MASK;

	if (s.bmRequestType & SETUP_DIR_IN) {
		if (type == SETUP_TYPE_STD)
			err = std_in(&s, &len);
		else if (type == SETUP_TYPE_VENDOR && s.bRequest >= 0xf0)
			err = test_in(&s, &len);
		else if (type == SETUP_TYPE_VENDOR)
			err = vendor_in(&s, &len);
		else
			err = 1;

		dbg_now.phase = 2;
		if (err)
			ep0_stall();
		else
			ep0_send(len, s.wLength);
		dbg_now.phase = 0;
		return;
	}

	if (s.wLength > XBUF_SIZE) {
		ep0_stall();
		return;
	}

	/* short requests read as zeros beyond their data */
	for (off = 0; off < XBUF_SIZE; off++)
		xbuf[off] = 0;

	for (off = 0; off < s.wLength; off += chunk) {
		chunk = s.wLength - off > EP0_MPS ? EP0_MPS : s.wLength - off;
		if (!ep0_rx(xbuf + off, chunk))
			return;
	}

	dbg_now.phase = 3;
	if (type == SETUP_TYPE_STD)
		err = std_out(&s, &addr);
	else if (type == SETUP_TYPE_VENDOR)
		err = vendor_out(&s, s.wLength);
	else
		err = 1;
	dbg_now.phase = 4;

	if (err) {
		ep0_stall();
		return;
	}

	if (!ep0_tx(xbuf, 0))
		return;
	dbg_now.phase = 5;

	if (addr != 0xff) {
		/* the controller does not delay the change of address */
		if (!ep0_wait_tx_idle())
			return;
		USB_ADDR = addr;
		usb_state = addr ? STATE_ADDRESS : STATE_DEFAULT;
	}
	dbg_now.phase = 0;
}

static void handle_reset(void)
{
	usb_state = STATE_DEFAULT;
	usb_config = 0;
	usb_alt = 0;
	ep82_halted = 0;
	/* the hardware presumably does this itself, it does not hurt */
	USB_ADDR = 0;
}

static void handle_speed(void)
{
	set_block_mps((USB_SPEED & USB_SPEED_HIGH) ? 0x0400 : 0x03ff);
}

static void handle_suspend(void)
{
	PORTC &= ~(1 << PC0);
	DDRC |= 1 << PC0;
	SYS_CTRL |= 0x10;
	USB_CTRL |= USB_CTRL_SUSPEND;
}

void usb_poll(void)
{
	uint8_t ev;

#if FW_IRQ
	/*
	 * docs/hardware.md "Interrupt faults": an interrupt requested while
	 * interrupts are disabled for more than a few dozen cycles may enter
	 * the reset vector when they are enabled again. So with interrupts
	 * enabled, the handler of vector 11 services the events, as the
	 * original firmware does in effect, and this takes what it recorded
	 * with interrupts disabled for a few cycles only. Polled as with IRQ=0
	 * only once the storm guard has switched interrupts off for good.
	 */
	irq_storm_guard();
	irq_off();
	if (irqrec.storm)
		usb_check_events();
	ev = usb_events;
	usb_events = 0;
	irq_on();
#else
	irq_storm_guard();
	usb_check_events();
	ev = usb_events;
	usb_events = 0;
	irq_on();
#endif

#if FW_ISR_BARE == 9
	irq_window_pass();
#endif

	if (ev & EV_RESET)
		handle_reset();
	if (ev & EV_SPEED)
		handle_speed();
	if (ev & EV_SUSPEND)
		handle_suspend();
	if (ev & EV_SETUP)
		handle_setup();
}

/* docs/hardware.md "Startup", step 5 */
void usb_init(void)
{
	USB_IRQ_CFG = 0xc4;
	EP0_CFG = 0x03;
	EP0_CTRL &= ~EP_STATE_MASK;
	EP0_MPS_A = 0x40;
	EP0_MPS_B = 0x40;
	EP0_RX_ADDR_L = EP0_RX_BUF & 0xff;
	EP0_RX_ADDR_H = EP0_RX_BUF >> 8;
	EP0_TX_ADDR_L = EP0_TX_BUF & 0xff;
	EP0_TX_ADDR_H = EP0_TX_BUF >> 8;

	EPX_CFG(EPA_BASE) = EPX_CFG_EP81;
	block_set_state(EPA_BASE, EP_STATE_NORMAL);
	EPX_MPS_L(EPA_BASE) = 0xff;
	EPX_MPS_H(EPA_BASE) = 0x03;

	EPX_CFG(EPB_BASE) = EPX_CFG_EP82;
	block_set_state(EPB_BASE, EP_STATE_NORMAL);
	EPX_MPS_L(EPB_BASE) = 0xff;
	EPX_MPS_H(EPB_BASE) = 0x03;

	/* drive PC0 low without a glitch: port first, then direction */
	PORTC &= ~(1 << PC0);
	DDRC |= 1 << PC0;

	/*
	 * Soft disconnect, so that the host enumerates the device again. The
	 * original waits 200 and 2000 units of its delay loop of about 160
	 * cycles, at 30 MHz about 1.07 and 10.7 ms; only the order of
	 * magnitude matters, see docs/hardware.md "Startup". Interrupts are
	 * still disabled here, so nothing stretches avr-libc's delays.
	 */
	_delay_us(1067);
	USB_CTRL |= USB_CTRL_DISCONNECT;
	_delay_us(10667);
	USB_CTRL &= ~USB_CTRL_DISCONNECT;
}
