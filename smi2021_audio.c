// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for the Somagic SMI2021 USB video capture bridge
 *
 * Copyright (C) 2011-2013 Jon Arne Jørgensen <jonjon.arnearne@gmail.com>
 * Copyright (C) 2011, 2012 Tony Brown, Michal Demin, Jeffry Johnston
 *
 * This driver is heavily influenced by the stk1160 driver:
 * Copyright (C) 2012 Ezequiel Garcia <elezegarcia@gmail.com>
 */

#include <sound/initval.h>
#include <sound/pcm_params.h>

#include "smi2021.h"

static const struct snd_pcm_hardware smi2021_pcm_hw = {
	.info = SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.period_bytes_min = 992,
	.period_bytes_max = 15872,
	.periods_min = 1,
	.periods_max = 16,
	.buffer_bytes_max = 65280,
};

static int smi2021_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int rc;

	runtime->hw = smi2021_pcm_hw;

	rc = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (rc < 0)
		return rc;

	return snd_pcm_hw_constraint_pow2(runtime, 0,
					  SNDRV_PCM_HW_PARAM_PERIODS);
}

/*
 * The sound card is freed when it is closed, which may be after the driver
 * state is gone, so this must not touch it. The stream was stopped before.
 */
static int smi2021_pcm_close(struct snd_pcm_substream *substream)
{
	return 0;
}

static int smi2021_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct smi2021 *smi2021 = snd_pcm_substream_chip(substream);

	smi2021->pcm_write_ptr = 0;
	smi2021->pcm_period_bytes = 0;
	smi2021->pcm_read_offset = 0;

	return 0;
}

static void smi2021_pcm_trigger_work(struct work_struct *work)
{
	struct smi2021 *smi2021 = container_of(work, struct smi2021,
					       pcm_trigger_work);

	smi2021_toggle_audio(smi2021, atomic_read(&smi2021->pcm_capturing));
}

/* Called with the stream lock held, so this must not sleep */
static int smi2021_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct smi2021 *smi2021 = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_START:
		atomic_set(&smi2021->pcm_capturing, 1);
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		atomic_set(&smi2021->pcm_capturing, 0);
		break;
	default:
		return -EINVAL;
	}

	schedule_work(&smi2021->pcm_trigger_work);

	return 0;
}

static snd_pcm_uframes_t
smi2021_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct smi2021 *smi2021 = snd_pcm_substream_chip(substream);

	return bytes_to_frames(substream->runtime, smi2021->pcm_write_ptr);
}

static const struct snd_pcm_ops smi2021_pcm_ops = {
	.open = smi2021_pcm_open,
	.close = smi2021_pcm_close,
	.prepare = smi2021_pcm_prepare,
	.trigger = smi2021_pcm_trigger,
	.pointer = smi2021_pcm_pointer,
};

static void smi2021_pcm_advance(struct smi2021 *smi2021,
				struct snd_pcm_runtime *runtime,
				unsigned int bytes)
{
	smi2021->pcm_write_ptr = (smi2021->pcm_write_ptr + bytes) %
				 runtime->dma_bytes;
	smi2021->pcm_period_bytes += bytes;
}

/*
 * Copy the audio data of one chunk into the ring buffer. Called with the
 * stream lock held. Returns whether a period has elapsed.
 *
 * The device sends 24 bit samples, each of them preceded by a 0x00 header
 * byte. Use this to detect bytes that got lost, and in that case mark the
 * partial frame at the head of the ring buffer as complete, so that the new
 * data starts on a frame boundary.
 */
static bool smi2021_audio_copy(struct smi2021 *smi2021,
			       struct snd_pcm_runtime *runtime,
			       const u8 *data, unsigned int len)
{
	unsigned int stride = frames_to_bytes(runtime, 1);
	unsigned int period = frames_to_bytes(runtime, runtime->period_size);
	unsigned int ptr = smi2021->pcm_write_ptr;
	unsigned int offset = smi2021->pcm_read_offset;
	unsigned int skipped = 0;

	/* Check that the previous chunk ended with a complete sample */
	if (ptr > offset + 4 && runtime->dma_area[ptr - offset - 4]) {
		smi2021_pcm_advance(smi2021, runtime, stride - ptr % stride);
		offset = 0;
	}

	while (len > stride && (data[offset] || data[offset + stride / 2])) {
		skipped++;
		data++;
		len--;
	}

	if (len <= stride) {
		/* No header byte found in this chunk */
		smi2021->pcm_read_offset = 0;
		return false;
	}

	if (skipped) {
		smi2021_pcm_advance(smi2021, runtime,
				    stride - smi2021->pcm_write_ptr % stride);
		offset = skipped % (stride / 2);
	}

	smi2021->pcm_read_offset = offset;

	while (len) {
		unsigned int n = min(len, runtime->dma_bytes -
					  smi2021->pcm_write_ptr);

		memcpy(runtime->dma_area + smi2021->pcm_write_ptr, data, n);
		smi2021_pcm_advance(smi2021, runtime, n);
		data += n;
		len -= n;
	}

	if (smi2021->pcm_period_bytes < period)
		return false;

	smi2021->pcm_period_bytes %= period;

	return true;
}

void smi2021_audio(struct smi2021 *smi2021, const u8 *data, unsigned int len)
{
	struct snd_pcm_substream *substream = smi2021->pcm_substream;

	if (!substream)
		return;

	/*
	 * pcm_capturing only changes in the trigger callback, which runs with
	 * the stream lock held, so the runtime is valid while it is set.
	 */
	guard(pcm_stream_lock_irqsave)(substream);

	if (atomic_read(&smi2021->pcm_capturing) &&
	    smi2021_audio_copy(smi2021, substream->runtime, data, len))
		snd_pcm_period_elapsed_under_stream_lock(substream);
}

int smi2021_snd_register(struct smi2021 *smi2021)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int rc;

	INIT_WORK(&smi2021->pcm_trigger_work, smi2021_pcm_trigger_work);

	rc = snd_card_new(smi2021->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			  THIS_MODULE, 0, &card);
	if (rc)
		return rc;

	strscpy(card->driver, KBUILD_MODNAME, sizeof(card->driver));
	strscpy(card->shortname, "SMI2021", sizeof(card->shortname));
	strscpy(card->longname, "Somagic SMI2021", sizeof(card->longname));

	rc = snd_pcm_new(card, "SMI2021", 0, 0, 1, &pcm);
	if (rc)
		goto err_free_card;

	pcm->private_data = smi2021;
	strscpy(pcm->name, "Somagic SMI2021 Capture", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &smi2021_pcm_ops);

	rc = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
					    0, 0);
	if (rc)
		goto err_free_card;

	rc = snd_card_register(card);
	if (rc)
		goto err_free_card;

	smi2021->snd_card = card;
	smi2021->pcm_substream = pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;

	return 0;

err_free_card:
	snd_card_free(card);

	return rc;
}

void smi2021_snd_unregister(struct smi2021 *smi2021)
{
	if (!smi2021->snd_card)
		return;

	/*
	 * This stops the stream and prevents any further pcm callback except
	 * for close, without waiting for userspace to close the device.
	 */
	snd_card_free_when_closed(smi2021->snd_card);
	cancel_work_sync(&smi2021->pcm_trigger_work);

	smi2021->snd_card = NULL;
	smi2021->pcm_substream = NULL;
}
