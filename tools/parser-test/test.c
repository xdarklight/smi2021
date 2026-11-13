// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Userspace test for the smi2021 frame parser.
 *
 * The code under test, from smi2021_get_buf() to smi2021_parse_video(), is
 * extracted verbatim from smi2021-v4l2.c into parser_extract.h. This file
 * provides the few kernel interfaces it uses, and synthetic ITU-R BT.656
 * streams to feed it.
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define BIT(n)			(1u << (n))
#define min(a, b)		((a) < (b) ? (a) : (b))
#define container_of(p, t, m)	((t *)((char *)(p) - offsetof(t, m)))
/* Locking is irrelevant here, guard(x)(&lock) becomes (void)(&lock) */
#define guard(x)		(void)

#define SMI2021_BYTES_PER_LINE	1440
#define SMI2021_TRC_H		BIT(4)
#define SMI2021_TRC_V		BIT(5)
#define SMI2021_TRC_F		BIT(6)

struct list_head {
	struct list_head *next, *prev;
};

static void list_init(struct list_head *h)
{
	h->next = h;
	h->prev = h;
}

static void list_add_tail(struct list_head *e, struct list_head *h)
{
	e->prev = h->prev;
	e->next = h;
	h->prev->next = e;
	h->prev = e;
}

static void list_del(struct list_head *e)
{
	e->prev->next = e->next;
	e->next->prev = e->prev;
}

#define list_first_entry_or_null(h, t, m) \
	((h)->next != (h) ? container_of((h)->next, t, m) : NULL)

enum vb2_buffer_state { VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR };
#define V4L2_FIELD_INTERLACED	4

struct vb2_buffer {
	unsigned long long timestamp;
};

struct vb2_v4l2_buffer {
	struct vb2_buffer vb2_buf;
	u32 sequence;
	u32 field;
};

struct smi2021_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	void *mem;
	bool second_field;
	bool in_blank;
	unsigned int pos;
};

struct smi2021 {
	int buf_lock;
	struct list_head avail_bufs;
	struct smi2021_buf *cur_buf;
	u8 trc_prefix_len;
	bool last_field2;
	u32 sequence;
	unsigned int cur_height;
};

static int frames_done, frames_error;
static u32 sequences[8];
static int nsequences;

static unsigned long long ktime_get_ns(void)
{
	return 0;
}

static void vb2_set_plane_payload(struct vb2_buffer *vb, unsigned int plane,
				  unsigned long size)
{
}

static void vb2_buffer_done(struct vb2_buffer *vb, enum vb2_buffer_state state)
{
	if (state == VB2_BUF_STATE_DONE)
		frames_done++;
	else
		frames_error++;

	if (nsequences < (int)ARRAY_SIZE(sequences))
		sequences[nsequences++] =
			((struct vb2_v4l2_buffer *)vb)->sequence;
}

/* Comes from the format code, which is not extracted */
static unsigned int smi2021_field(const struct smi2021 *smi2021)
{
	return V4L2_FIELD_INTERLACED;
}

#include "parser_extract.h"

/* --- synthetic stream generation --------------------------------------- */

static u8 *stream;
static size_t stream_len, stream_cap;

static void emit(const u8 *p, size_t n)
{
	while (stream_len + n > stream_cap) {
		stream_cap = stream_cap ? stream_cap * 2 : 65536;
		stream = realloc(stream, stream_cap);
	}
	memcpy(stream + stream_len, p, n);
	stream_len += n;
}

static void emit_trc(u8 xy)
{
	const u8 t[4] = { 0xff, 0x00, 0x00, 0x80 | xy };

	emit(t, sizeof(t));
}

/* Pixel value encoding frame, field, line and column; never 0x00 or 0xff */
static u8 pix(int frame, int field, int line, int col)
{
	return 1 + (frame * 5 + field * 7 + line * 13 + col * 31) % 254;
}

/* Turn some pixels into false timing reference code prefixes */
static const struct {
	int col;
	u8 val;
} injected[] = {
	{ 100, 0xff }, { 101, 0x12 },
	{ 200, 0xff }, { 201, 0x00 }, { 202, 0x34 },
	{ 300, 0xff }, { 301, 0xff }, { 302, 0x56 },
};

static u8 expected(int frame, int field, int line, int col, bool inject)
{
	size_t i;

	for (i = 0; inject && i < ARRAY_SIZE(injected); i++)
		if (injected[i].col == col)
			return injected[i].val;

	return pix(frame, field, line, col);
}

static void emit_line(int frame, int field, int line, bool vbi, bool inject)
{
	u8 xy = (field ? SMI2021_TRC_F : 0) | (vbi ? SMI2021_TRC_V : 0);
	u8 data[SMI2021_BYTES_PER_LINE];
	int col;

	for (col = 0; col < SMI2021_BYTES_PER_LINE; col++)
		data[col] = expected(frame, field, line, col, inject);

	emit_trc(xy);
	emit(data, sizeof(data));
	emit_trc(xy | SMI2021_TRC_H);
}

/* Emit lines [first, last) of a field, preceded by blanking if first is 0 */
static void emit_field(int frame, int field, int first, int last, bool inject)
{
	int line;

	if (!first)
		for (line = 0; line < 3; line++)
			emit_line(frame, field, line, true, false);

	for (line = first; line < last; line++)
		emit_line(frame, field, line, false, inject);
}

/* --- test driver -------------------------------------------------------- */

#define MAX_LINES	576
#define GUARD		(8 * SMI2021_BYTES_PER_LINE)

static u8 backing[GUARD + SMI2021_BYTES_PER_LINE * MAX_LINES + GUARD];
static struct smi2021_buf bufs[2];

struct test {
	const char *name;
	unsigned int height;		/* captured frame height */
	int active;			/* active lines per field sent */
	int first_field_lines;		/* lines of field 1 of frame 1 */
	bool start_mid_field1;		/* stream starts in field 1 */
	bool inject;
	int nbufs;
	int want_done, want_error;
};

static int run(const struct test *t, unsigned int chunk)
{
	unsigned int lines_per_field = t->height / 2;
	size_t frame_size = SMI2021_BYTES_PER_LINE * t->height;
	/* The frame ends right at the trailing guard, to catch overruns */
	u8 *frame_mem = backing + sizeof(backing) - GUARD - frame_size;
	struct smi2021 s = { .cur_height = t->height };
	int bad = 0, field, line, col, i;
	bool guards_ok = true;
	size_t pos;

	memset(backing, 0xa5, sizeof(backing));
	memset(frame_mem, 0, frame_size);

	list_init(&s.avail_bufs);
	for (i = 0; i < t->nbufs; i++) {
		memset(&bufs[i], 0, sizeof(bufs[i]));
		bufs[i].mem = frame_mem;
		bufs[i].in_blank = true;
		list_add_tail(&bufs[i].list, &s.avail_bufs);
	}
	frames_done = frames_error = 0;

	stream_len = 0;
	/* The stream starts somewhere in the middle of frame 0 */
	if (t->start_mid_field1)
		emit_field(0, 0, t->active / 2, t->active, false);
	emit_field(0, 1, t->active / 2, t->active, false);
	/* Frame 1, possibly with a short first field */
	if (t->first_field_lines != t->active) {
		emit_field(1, 0, 0, t->first_field_lines, false);
		emit_field(1, 1, 0, t->active, false);
	}
	/* Frame 2, which is checked */
	emit_field(2, 0, 0, t->active, t->inject);
	emit_field(2, 1, 0, t->active, t->inject);
	/* The start of frame 3 completes frame 2 */
	emit_field(3, 0, 0, 2, false);

	for (pos = 0; pos < stream_len; pos += chunk)
		smi2021_parse_video(&s, stream + pos,
				    min(chunk, stream_len - pos));

	for (field = 0; field < 2; field++) {
		for (line = 0; line < (int)lines_per_field; line++) {
			u8 *dst = frame_mem + (line * 2 + field) *
				  SMI2021_BYTES_PER_LINE;

			for (col = 0; col < SMI2021_BYTES_PER_LINE; col++) {
				u8 want = expected(2, field, line, col,
						   t->inject);

				if (dst[col] == want)
					continue;
				if (bad++ < 3)
					fprintf(stderr,
						"  f%d l%d c%d: got %02x want %02x\n",
						field, line, col, dst[col],
						want);
			}
		}
	}

	for (pos = 0; pos < sizeof(backing); pos++)
		if ((backing + pos < frame_mem ||
		     backing + pos >= frame_mem + frame_size) &&
		    backing[pos] != 0xa5)
			guards_ok = false;

	bool pass = frames_done == t->want_done &&
		    frames_error == t->want_error && !bad && guards_ok;

	printf("%-44s chunk %-5u done %d error %d bad %-6d guard %-3s %s\n",
	       t->name, chunk, frames_done, frames_error, bad,
	       guards_ok ? "ok" : "BAD", pass ? "PASS" : "FAIL");

	return !pass;
}

/*
 * A frame that finds no free buffer is dropped, but it still counts: with a
 * single buffer, given back after frame 1 and again after frame 2 was
 * dropped, the frames handed out must be numbered 0 and 2.
 */
static int run_sequence(void)
{
	static u8 mem[SMI2021_BYTES_PER_LINE * 480];
	struct smi2021 s = { .cur_height = 480 };
	struct smi2021_buf *buf = &bufs[0];
	size_t first;
	bool pass;

	list_init(&s.avail_bufs);
	memset(buf, 0, sizeof(*buf));
	buf->mem = mem;
	buf->in_blank = true;
	list_add_tail(&buf->list, &s.avail_bufs);
	nsequences = 0;

	stream_len = 0;
	emit_field(0, 1, 121, 242, false);
	emit_field(1, 0, 0, 242, false);
	emit_field(1, 1, 0, 242, false);
	emit_field(2, 0, 0, 242, false);
	emit_field(2, 1, 0, 242, false);
	first = stream_len;
	emit_field(3, 0, 0, 242, false);
	emit_field(3, 1, 0, 242, false);
	emit_field(4, 0, 0, 2, false);

	smi2021_parse_video(&s, stream, first);
	/* Frame 1 is done, frame 2 went without a buffer */
	memset(buf, 0, sizeof(*buf));
	buf->mem = mem;
	buf->in_blank = true;
	list_add_tail(&buf->list, &s.avail_bufs);
	smi2021_parse_video(&s, stream + first, stream_len - first);

	pass = nsequences == 2 && sequences[0] == 0 && sequences[1] == 2;
	printf("%-44s sequence", "dropped frames are counted");
	for (int i = 0; i < nsequences; i++)
		printf(" %u", sequences[i]);
	printf(" %s\n", pass ? "PASS" : "FAIL");

	return !pass;
}

int main(void)
{
	static const struct test tests[] = {
		{ "NTSC, 240 active lines",	  480, 240, 240, false, false, 1, 1, 0 },
		{ "NTSC, 242 active lines (VRLN)", 480, 242, 242, false, false, 1, 1, 0 },
		{ "PAL, 288 active lines",	  576, 288, 288, false, false, 1, 1, 0 },
		{ "start in the middle of field 1", 480, 242, 242, true, false, 1, 1, 0 },
		{ "0xff in video data",		  480, 242, 242, false, true, 1, 1, 0 },
		{ "short first field is dropped",  480, 242, 200, false, false, 2, 1, 1 },
	};
	/* 1020 is the payload size of a real chunk; tiny ones split codes */
	static const unsigned int chunks[] = { 1020, 1, 3, 7, 1u << 30 };
	size_t t, c;
	int fail = 0;

	for (t = 0; t < ARRAY_SIZE(tests); t++)
		for (c = 0; c < ARRAY_SIZE(chunks); c++)
			fail |= run(&tests[t], chunks[c]);
	fail |= run_sequence();

	printf("\n%s\n", fail ? "SOME TESTS FAILED" : "all parser tests passed");

	return fail;
}
