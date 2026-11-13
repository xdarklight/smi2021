// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Userspace test for the smi2021 audio ring buffer handling.
 *
 * smi2021_pcm_advance() and smi2021_audio_copy() are extracted verbatim from
 * smi2021_audio.c into audio_extract.h. This file provides the few kernel
 * interfaces they use, and synthetic audio chunks to feed them.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;

#define min(a, b)		((a) < (b) ? (a) : (b))
#define CHUNK_DATA_SIZE		1020	/* SMI2021_CHUNK_DATA_SIZE */
#define SMI2021_AUDIO_SAMPLE_SIZE	4
#define SMI2021_AUDIO_FRAME_SIZE	(2 * SMI2021_AUDIO_SAMPLE_SIZE)

struct snd_pcm_runtime {
	u8 *dma_area;
	unsigned int dma_bytes;
	unsigned long period_size;	/* in frames */
};

static unsigned int frames_to_bytes(struct snd_pcm_runtime *r,
				    unsigned long frames)
{
	return frames * SMI2021_AUDIO_FRAME_SIZE;
}

struct smi2021 {
	unsigned int pcm_write_ptr;
	unsigned int pcm_period_bytes;
	u8 pcm_read_offset;
};

#include "audio_extract.h"

#define GUARD		4096
#define MAX_BUFFER	(256 * 1024)

static u8 backing[GUARD + MAX_BUFFER + GUARD];

/* Sample n of the stream: 0x00 header byte, then 24 bits that are never 0 */
static void sample(unsigned long n, u8 out[4])
{
	out[0] = 0x00;
	out[1] = 1 + n % 251;
	out[2] = 1 + (n >> 8) % 251;
	out[3] = 1 + (n * 7) % 251;
}

static u8 stream_byte(unsigned long pos)
{
	u8 s[4];

	sample(pos / 4, s);
	return s[pos % 4];
}

struct setup {
	struct smi2021 s;
	struct snd_pcm_runtime rt;
};

static void init(struct setup *st, unsigned int buffer, unsigned int period)
{
	memset(backing, 0xa5, sizeof(backing));
	memset(st, 0, sizeof(*st));
	st->rt.dma_area = backing + GUARD;
	st->rt.dma_bytes = buffer;
	st->rt.period_size = period / SMI2021_AUDIO_FRAME_SIZE;
	memset(st->rt.dma_area, 0, buffer);
}

static bool guards_ok(const struct setup *st)
{
	const u8 *end = st->rt.dma_area + st->rt.dma_bytes;
	size_t i;

	for (i = 0; i < sizeof(backing); i++) {
		const u8 *p = backing + i;

		if ((p < st->rt.dma_area || p >= end) && *p != 0xa5)
			return false;
	}
	return true;
}

static int report(const char *name, bool pass, const char *detail)
{
	printf("%-52s %s%s%s\n", name, pass ? "PASS" : "FAIL",
	       detail ? "  " : "", detail ? detail : "");
	return !pass;
}

/*
 * A clean stream must land in the ring buffer byte for byte. Periods are only
 * counted exactly when they are no shorter than a chunk, as the driver does
 * not allow shorter ones and one chunk elapses at most one period.
 */
static int test_clean(unsigned int buffer, unsigned int period)
{
	bool count = period >= CHUNK_DATA_SIZE;
	unsigned long total = 0, elapsed = 0, chunks = 200, pos, i;
	struct setup st;
	u8 chunk[CHUNK_DATA_SIZE];
	char name[64], detail[96] = "";
	int bad = 0;

	init(&st, buffer, period);

	for (i = 0; i < chunks; i++) {
		for (pos = 0; pos < CHUNK_DATA_SIZE; pos++)
			chunk[pos] = stream_byte(total + pos);
		elapsed += smi2021_audio_copy(&st.s, &st.rt, chunk,
					      CHUNK_DATA_SIZE);
		total += CHUNK_DATA_SIZE;
	}

	/* The last buffer's worth of the stream must be in the ring buffer */
	for (pos = total - min(total, buffer); pos < total; pos++)
		if (st.rt.dma_area[pos % buffer] != stream_byte(pos))
			bad++;

	snprintf(name, sizeof(name), "clean stream, buffer %u, period %u",
		 buffer, period);
	if (count && elapsed != total / period)
		bad++;
	if (bad || st.s.pcm_write_ptr != total % buffer)
		snprintf(detail, sizeof(detail),
			 "bad %d, periods %lu want %lu, ptr %u want %lu",
			 bad, elapsed, total / period, st.s.pcm_write_ptr,
			 total % buffer);

	return report(name, !bad && st.s.pcm_write_ptr == total % buffer &&
		      guards_ok(&st), detail[0] ? detail : NULL);
}

/*
 * After bytes got lost, the following data must be copied sample aligned
 * again: every complete sample in what gets written afterwards starts with
 * its 0x00 header byte at a multiple of the sample size.
 */
static int test_resync(unsigned int lost)
{
	unsigned int buffer = 64 * 1024, period = 4096;
	unsigned long total = 0, pos, i;
	unsigned int start_ptr = 0;
	struct setup st;
	u8 chunk[CHUNK_DATA_SIZE];
	char name[64], detail[64] = "";
	int bad = 0;

	init(&st, buffer, period);

	for (i = 0; i < 40; i++) {
		unsigned int len = CHUNK_DATA_SIZE;
		unsigned long src = total;

		/* Chunk 10 loses its first bytes */
		if (i == 10) {
			src += lost;
			len -= lost;
		}
		for (pos = 0; pos < len; pos++)
			chunk[pos] = stream_byte(src + pos);
		if (i == 11)
			start_ptr = st.s.pcm_write_ptr;
		smi2021_audio_copy(&st.s, &st.rt, chunk, len);
		total += CHUNK_DATA_SIZE;
	}

	/* Check what was written from chunk 11 on */
	for (pos = start_ptr; pos != st.s.pcm_write_ptr; pos = (pos + 4) % buffer)
		if (st.rt.dma_area[pos] != 0x00)
			bad++;

	snprintf(name, sizeof(name), "resync after losing %u bytes", lost);
	if (bad)
		snprintf(detail, sizeof(detail), "%d misaligned samples", bad);
	return report(name, !bad && guards_ok(&st), detail[0] ? detail : NULL);
}

/* Random corruption must never make the code write outside the buffer */
static int test_fuzz(void)
{
	unsigned int iter, buffers[] = { 8, 16, 992, 1016, 1024, 2048, 4000 };
	u8 chunk[CHUNK_DATA_SIZE];
	int fail = 0;

	srand(1);
	for (iter = 0; iter < 2000 && !fail; iter++) {
		unsigned int buffer = buffers[iter % 7], i, j;
		struct setup st;

		init(&st, buffer, buffer);
		for (i = 0; i < 50; i++) {
			unsigned int len = 1 + rand() % CHUNK_DATA_SIZE;

			for (j = 0; j < len; j++)
				chunk[j] = rand() % 4 ? stream_byte(i * 1020 + j)
						      : rand();
			smi2021_audio_copy(&st.s, &st.rt, chunk, len);
			if (st.s.pcm_write_ptr >= buffer ||
			    st.s.pcm_read_offset >= SMI2021_AUDIO_SAMPLE_SIZE ||
			    !guards_ok(&st)) {
				fail = 1;
				break;
			}
		}
	}

	return report("random corruption, buffers 8 to 4000 bytes", !fail,
		      fail ? "state out of range or guard overwritten" : NULL);
}

int main(void)
{
	int fail = 0;

	/* The driver's limits: 1024 to 128 KiB periods, up to 256 KiB */
	fail |= test_clean(2048, 1024);
	fail |= test_clean(4096, 1024);
	fail |= test_clean(65536, 16384);
	fail |= test_clean(256 * 1024, 128 * 1024);
	/* Buffers smaller than one chunk, which the old limits allowed */
	fail |= test_clean(992, 496);
	fail |= test_clean(1016, 508);
	fail |= test_resync(1);
	fail |= test_resync(2);
	fail |= test_resync(3);
	fail |= test_resync(5);
	fail |= test_fuzz();

	printf("\n%s\n", fail ? "SOME TESTS FAILED" : "all audio tests passed");

	return fail;
}
