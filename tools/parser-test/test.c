/* Userspace harness: exercises the driver's frame parser verbatim. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
#define min(a, b) ((a) < (b) ? (a) : (b))

#define SMI2021_BYTES_PER_LINE	1440
#define SMI2021_NTSC_LINES	480
#define SMI2021_TRC_EAV		0x10
#define SMI2021_TRC_VBI		0x20
#define SMI2021_TRC_FIELD_2	0x40

enum smi2021_sync { HSYNC, SYNCZ1, SYNCZ2, TRC };

struct smi2021_buf {
	void *mem;
	unsigned int length;
	bool second_field;
	bool in_blank;
	unsigned int pos;
};

struct smi2021 {
	unsigned int cur_height;
	enum smi2021_sync sync_state;
	struct smi2021_buf *cur_buf;
	unsigned int sequence;
};

/* --- harness state --- */
#define GUARD 4096
static unsigned char backing[GUARD + SMI2021_BYTES_PER_LINE * SMI2021_NTSC_LINES + GUARD];
static struct smi2021_buf the_buf;
static int bufs_available;
static int frames_done, frames_error;
static unsigned char *frame_mem;

static struct smi2021_buf *smi2021_get_buf(struct smi2021 *s)
{
	if (!bufs_available)
		return NULL;
	bufs_available--;
	memset(&the_buf, 0, sizeof(the_buf));
	the_buf.mem = frame_mem;
	the_buf.length = SMI2021_BYTES_PER_LINE * SMI2021_NTSC_LINES;
	the_buf.in_blank = true;
	return &the_buf;
}

static void smi2021_buf_done(struct smi2021 *s)
{
	struct smi2021_buf *b = s->cur_buf;
	unsigned int lines_per_field = s->cur_height / 2;

	if (b->pos < SMI2021_BYTES_PER_LINE * lines_per_field)
		frames_error++;
	else
		frames_done++;

	s->sequence++;
	s->cur_buf = NULL;
}

#include "parser_extract.h"

/* --- synthetic BT.656 stream generation --- */
static unsigned char *stream;
static size_t stream_len, stream_cap;

static void emit(const unsigned char *p, size_t n)
{
	while (stream_len + n > stream_cap) {
		stream_cap = stream_cap ? stream_cap * 2 : 65536;
		stream = realloc(stream, stream_cap);
	}
	memcpy(stream + stream_len, p, n);
	stream_len += n;
}

static void emit_trc(u8 trc)
{
	unsigned char t[4] = { 0xff, 0x00, 0x00, trc };
	emit(t, 4);
}

/* Pixel value encoding field/line/offset; never 0x00 or 0xff */
static unsigned char pix(int field, int line, int off)
{
	return (unsigned char)(1 + ((field * 7 + line * 13 + off * 31) % 254));
}

static void emit_line(int field, int line, int vbi, int inject_ff)
{
	unsigned char data[SMI2021_BYTES_PER_LINE];
	u8 base = (field ? SMI2021_TRC_FIELD_2 : 0) | (vbi ? SMI2021_TRC_VBI : 0);
	int i;

	emit_trc(0x80 | base);			/* SAV */
	for (i = 0; i < SMI2021_BYTES_PER_LINE; i++)
		data[i] = pix(field, line, i);
	if (inject_ff) {
		/* false-alarm sync prefixes that are really video data */
		data[100] = 0xff; data[101] = 0x12;
		data[200] = 0xff; data[201] = 0x00; data[202] = 0x34;
		data[300] = 0xff; data[301] = 0xff; data[302] = 0x56;
	}
	emit(data, SMI2021_BYTES_PER_LINE);
	emit_trc(0x80 | base | SMI2021_TRC_EAV);	/* EAV */
}

static void emit_field(int field, int lines, int inject_ff)
{
	int i;
	for (i = 0; i < 3; i++)			/* a few VBI lines */
		emit_line(field, i, 1, 0);
	for (i = 0; i < lines; i++)
		emit_line(field, i, 0, inject_ff);
}

static int check_guards(void)
{
	size_t i;
	for (i = 0; i < GUARD; i++)
		if (backing[i] != 0xA5 ||
		    backing[sizeof(backing) - 1 - i] != 0xA5)
			return 0;
	return 1;
}

static int verify_frame(int inject_ff)
{
	int field, line, off, bad = 0;

	for (field = 0; field < 2; field++) {
		for (line = 0; line < SMI2021_NTSC_LINES / 2; line++) {
			unsigned char *dst = frame_mem +
				(line * 2 + field) * SMI2021_BYTES_PER_LINE;
			for (off = 0; off < SMI2021_BYTES_PER_LINE; off++) {
				unsigned char want = pix(field, line, off);
				if (inject_ff) {
					if (off == 100) want = 0xff;
					if (off == 101) want = 0x12;
					if (off == 200) want = 0xff;
					if (off == 201) want = 0x00;
					if (off == 202) want = 0x34;
					if (off == 300) want = 0xff;
					if (off == 301) want = 0xff;
					if (off == 302) want = 0x56;
				}
				if (dst[off] != want) {
					if (bad < 6)
						fprintf(stderr,
						  "  mismatch f%d l%d o%d: got %02x want %02x\n",
						  field, line, off, dst[off], want);
					bad++;
				}
			}
		}
	}
	return bad;
}

static int run(const char *name, unsigned int chunk, int inject_ff)
{
	struct smi2021 s;
	size_t i;
	int bad, fail = 0;

	memset(backing, 0xA5, sizeof(backing));
	frame_mem = backing + GUARD;
	memset(frame_mem, 0, SMI2021_BYTES_PER_LINE * SMI2021_NTSC_LINES);

	memset(&s, 0, sizeof(s));
	s.cur_height = SMI2021_NTSC_LINES;
	s.sync_state = HSYNC;
	bufs_available = 1;
	frames_done = frames_error = 0;

	stream_len = 0;
	emit_field(0, SMI2021_NTSC_LINES / 2, inject_ff);
	emit_field(1, SMI2021_NTSC_LINES / 2, inject_ff);
	/* next frame's first field triggers completion of this one */
	emit_field(0, 4, 0);

	for (i = 0; i < stream_len; i += chunk) {
		unsigned int n = chunk;
		if (i + n > stream_len)
			n = stream_len - i;
		parse_video(&s, stream + i, n);
	}

	bad = inject_ff ? verify_frame(1) : verify_frame(0);

	printf("%-46s chunk=%-5u done=%d err=%d badbytes=%-6d guards=%s  %s\n",
	       name, chunk, frames_done, frames_error, bad,
	       check_guards() ? "ok" : "CORRUPT",
	       (frames_done == 1 && frames_error == 0 && bad == 0 &&
	        check_guards()) ? "PASS" : "FAIL");

	if (!(frames_done == 1 && frames_error == 0 && bad == 0 && check_guards()))
		fail = 1;
	return fail;
}

int main(void)
{
	int fail = 0;

	/* 0x400 - 4 is the real payload size of a USB chunk */
	fail |= run("clean stream, real USB chunking", 0x400 - 4, 0);
	fail |= run("clean stream, 1-byte chunks", 1, 0);
	fail |= run("clean stream, 3-byte chunks (splits TRCs)", 3, 0);
	fail |= run("clean stream, 7-byte chunks", 7, 0);
	fail |= run("clean stream, whole stream at once", 1u << 30, 0);
	fail |= run("0xff in video data, real USB chunking", 0x400 - 4, 1);
	fail |= run("0xff in video data, 1-byte chunks", 1, 1);
	fail |= run("0xff in video data, 3-byte chunks", 3, 1);

	printf("\n%s\n", fail ? "*** SOME TESTS FAILED ***" : "all parser tests passed");
	return fail;
}
