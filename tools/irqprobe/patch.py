#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn a copy of fw/ into the IRQPROBE build: see README.md.

usage: patch.py DIR    (DIR: a copy of fw/, changed in place)
"""
import pathlib, sys
d = pathlib.Path(sys.argv[1])

def rep(f, old, new, count=1):
    p = d / f; s = p.read_text()
    assert old in s, (f, old)
    p.write_text(s.replace(old, new, count))

rep('Makefile', '-DFW_IRQ=$(IRQ) \\', '-DFW_IRQ=$(IRQ) -DFW_IRQPROBE=1 \\')
rep('Makefile', 'ASFLAGS\t\t:= $(ARCH) -DFW_IRQ=$(IRQ) -g', 'ASFLAGS\t\t:= $(ARCH) -DFW_IRQ=$(IRQ) -DFW_IRQPROBE=1 -g')

# the reset vector's check marks its entries for the handler
rep('isr.S', """	lds	r24, vec0_tmp + 1
	out	SREG_IO, r24
	lds	r24, vec0_tmp
	jmp	__vector_11""", """	lds	r24, vec0_tmp + 1
	out	SREG_IO, r24
	ldi	r24, 1
	sts	probe_via0, r24
	lds	r24, vec0_tmp
	jmp	__vector_11""")

# interrupts stay off but for the probe's window: the storm flag makes
# every IRQ=1 path poll and irq_on() a no-op
rep('main.c', """	boot_magic = BOOT_MAGIC;
#endif""", """	boot_magic = BOOT_MAGIC;
	irq_storm = 1;
#endif""")

rep('usb.c', """void __vector_11(void)
{
	irq_armed = 0;
	irq_ticks++;
	usb_check_events();
	irq_armed = 1;			/* RETI enables interrupts again */
}""", """void __vector_11(void)
{
	irq_armed = 0;
	probe_hit = 1;			/* the main loop polls the events */
	irq_armed = 1;
}""")

rep('usb.c', """#if FW_IRQ
volatile uint8_t irq_storm;
""", """#if FW_IRQ
volatile uint8_t irq_storm;

/*
 * IRQPROBE: maps an interrupt request over time. With interrupts disabled,
 * wait for a bit of a register to go from 0 to 1, after an XOR with
 * probe_inv (probe_addr, probe_mask: the host sets them), wait
 * 4 * probe_step * i cycles more, i the bucket, cycling through 128, then
 * enable interrupts for one instruction (SEI, NOP, CLI). Count per bucket:
 * tries, interrupts taken through vector 11, and through the reset vector
 * (isr.S sets probe_via0 before it enters the handler). A window before
 * the wait tells whether a request is pending already; such tries count in
 * probe_pre (and probe_pre_v0) instead. The host reads and clears the
 * tables through register accesses of SRAM.
 */
#define PROBE_N	128
volatile uint8_t probe_step = 2;
volatile uint16_t probe_addr = 0x1502;
volatile uint8_t probe_mask = 0x10, probe_inv;
volatile uint8_t probe_hit, probe_via0;
static uint8_t probe_idx;
static uint16_t probe_d;	/* probe_idx * probe_step, without a multiply */
uint16_t probe_tries[PROBE_N], probe_v11[PROBE_N], probe_v0[PROBE_N];
uint16_t probe_pre, probe_pre_v0;

/*
 * The controller's registers just before each window that found a request
 * pending, and what became of it: per distinct value of the four, counts
 * of interrupts through vector 11 and through the reset vector. The last
 * entry takes all that don't fit.
 */
#define SNAP_N	24
struct probe_snap {
	uint8_t r1503, r1502, r151c, r1518;
	uint16_t v11, v0;
};
struct probe_snap probe_snaps[SNAP_N];
volatile uint8_t probe_in;	/* 1-3: probe the packet in ep0_tx() */
volatile uint8_t probe_noread;	/* 1: no register reads before a window */

/* 0: no interrupt, 1: through vector 11, 2: through the reset vector */
static uint8_t probe_window(void);

static uint8_t probe_pending(void)
{
	uint8_t a, b, c, e, r, k;

	if (probe_noread) {
		r = probe_window();
		k = SNAP_N - 1;
		goto count;
	}
	a = USB_EVENT;
	b = USB_STATUS;
	c = EP0_TX_STAT;
	e = EP0_RX_STAT;
	r = probe_window();
	if (!r)
		return 0;
	for (k = 0; k < SNAP_N - 1; k++) {
		struct probe_snap *p = &probe_snaps[k];

		if (!p->v11 && !p->v0) {
			p->r1503 = a;
			p->r1502 = b;
			p->r151c = c;
			p->r1518 = e;
		}
		if (p->r1503 == a && p->r1502 == b && p->r151c == c &&
		    p->r1518 == e)
			break;
	}
count:
	if (r == 1)
		probe_snaps[k].v11++;
	else if (r == 2)
		probe_snaps[k].v0++;
	return r;
}

static uint8_t probe_window(void)
{
	probe_hit = 0;
	probe_via0 = 0;
	irq_armed = 1;
	__asm__ __volatile__ ("sei\\n\\tnop\\n\\tcli" ::: "memory");
	irq_armed = 0;
	return probe_hit ? (probe_via0 ? 2 : 1) : 0;
}

static void probe_timed(void);

/*
 * probe_in 3: trace 0x1502, 0x151c and 0x1503 after ep0_tx() queued a
 * packet: every change in 2000 polls, up to TRACE_E per packet, for the
 * first TRACE_N packets. A poll takes some 14 cycles.
 */
#define TRACE_N	12
#define TRACE_E	8
struct probe_trace {
	uint16_t poll;
	uint8_t r1502, r151c, r1503;
};
struct probe_trace probe_traces[TRACE_N][TRACE_E];
uint8_t probe_trace_n;

static void probe_trace_packet(void)
{
	struct probe_trace *t;
	uint8_t b, c, a, pb = 0xff, pc = 0xff, pa = 0xff, e = 0;
	uint16_t n;

	if (probe_trace_n >= TRACE_N)
		return;
	t = probe_traces[probe_trace_n++];
	for (n = 0; n < 2000 && e < TRACE_E; n++) {
		b = USB_STATUS;
		c = EP0_TX_STAT;
		a = USB_EVENT;
		if (b != pb || c != pc || a != pa) {
			t[e].poll = n;
			t[e].r1502 = b;
			t[e].r151c = c;
			t[e].r1503 = a;
			e++;
			pb = b;
			pc = c;
			pa = a;
		}
	}
}

/*
 * probe_in 2: after ep0_tx() queued a packet, poll 0x151c for probe_d
 * polls (cycling from 0 to 254 in steps of probe_step), noting the poll at
 * which the host took the packet, then open the window, then, if the
 * packet is still there, poll on until it goes. Count by the distance of
 * the window from that moment, in polls of some 10 cycles: bucket 64 + dt
 * / 2, dt < 0 when the host took it before the window.
 */
static void probe_in_sweep(void)
{
	uint16_t n, d = probe_d, fall = 0xffff;
	int16_t dt;
	uint8_t r, i;

	for (n = 0; n < d; n++)
		if (fall == 0xffff && !(EP0_TX_STAT & EP_FULL))
			fall = n;
	r = probe_window();
	if (fall != 0xffff) {
		dt = (int16_t)fall - (int16_t)d;
	} else {
		for (n = 0; n < 20000; n++)
			if (!(EP0_TX_STAT & EP_FULL))
				break;
		dt = n;
	}
	dt = 64 + dt / 2;
	i = dt < 0 ? 0 : dt > PROBE_N - 1 ? PROBE_N - 1 : dt;
	probe_tries[i]++;
	if (r == 1)
		probe_v11[i]++;
	else if (r == 2)
		probe_v0[i]++;
	probe_d = probe_d + probe_step > 254 ? 0 : probe_d + probe_step;
}

/* 1: the IN packet that ep0_tx() just queued, taken by the host */
static void probe_in_packet(void)
{
	uint16_t n;
	uint8_t r;

	r = probe_pending();
	if (r) {
		probe_pre++;
		if (r == 2)
			probe_pre_v0++;
		return;
	}
	for (n = 0; n < 20000; n++)
		if (!(EP0_TX_STAT & EP_FULL))
			goto hit;
	return;
hit:
	probe_timed();
}

static void probe(void)
{
	volatile uint8_t *reg = (volatile uint8_t *)probe_addr;
	uint8_t mask = probe_mask, inv = probe_inv, n, r;

	if (!mask)
		return;
	if ((*reg ^ inv) & mask)
		return;			/* there already: when is unknown */
	r = probe_pending();
	if (r) {
		probe_pre++;
		if (r == 2)
			probe_pre_v0++;
		return;
	}
	for (n = 0; n < 200; n++)
		if ((*reg ^ inv) & mask)
			goto hit;
	return;
hit:
	probe_timed();
}

static void probe_timed(void)
{
	uint8_t i, r;
	uint16_t d;

	i = probe_idx;
	d = probe_d;
	if (d)
		__asm__ __volatile__ ("1: sbiw %0, 1\\n\\tbrne 1b" : "+w" (d));
	r = probe_window();
	probe_tries[i]++;
	if (r == 1)
		probe_v11[i]++;
	else if (r == 2)
		probe_v0[i]++;
	probe_idx = (i + 1) & (PROBE_N - 1);
	probe_d = probe_idx ? probe_d + probe_step : 0;
}
""")

rep('usb.c', """	irq_storm_guard();
	irq_off();
	if (irq_storm)
		usb_check_events();""", """	irq_off();
	probe();
	if (irq_storm)
		usb_check_events();""")

# the IN probe: right after ep0_tx() queued a packet
rep('usb.c', """	EP0_TX_STAT |= EP_FULL;
	irq_on();
""", """	EP0_TX_STAT |= EP_FULL;
	if (probe_in == 1)
		probe_in_packet();
	else if (probe_in == 2)
		probe_in_sweep();
	else if (probe_in == 3)
		probe_trace_packet();
	irq_on();
""")
