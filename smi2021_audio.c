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
		SNDRV_PCM_INFO_INTERLEAVED    |
		SNDRV_PCM_INFO_MMAP           |
		SNDRV_PCM_INFO_MMAP_VALID     |
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
	struct smi2021 *smi2021 = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int rc;

	rc = snd_pcm_hw_constraint_pow2(runtime, 0,
					SNDRV_PCM_HW_PARAM_PERIODS);
	if (rc < 0)
		return rc;

	rc = snd_pcm_hw_constraint_integer(runtime,
					   SNDRV_PCM_HW_PARAM_PERIODS);
	if (rc < 0)
		return rc;

	runtime->hw = smi2021_pcm_hw;
	smi2021->pcm_substream = substream;

	return 0;
}

static int smi2021_pcm_close(struct snd_pcm_substream *substream)
{
	struct smi2021 *smi2021 = snd_pcm_substream_chip(substream);

	if (atomic_read(&smi2021->adev_capturing)) {
		atomic_set(&smi2021->adev_capturing, 0);
		schedule_work(&smi2021->adev_capture_trigger);
	}

	return 0;
}

static int smi2021_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct smi2021 *smi2021 = snd_pcm_substream_chip(substream);

	smi2021->pcm_complete_samples = 0;
	smi2021->pcm_read_offset = 0;
	smi2021->pcm_write_ptr = 0;

	return 0;
}

static void capture_trigger(struct work_struct *work)
{
	struct smi2021 *smi2021 = container_of(work, struct smi2021,
					       adev_capture_trigger);

	smi2021_toggle_audio(smi2021,
			     atomic_read(&smi2021->adev_capturing) != 0);
}

/* This callback is ATOMIC, it must not sleep */
static int smi2021_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct smi2021 *smi2021 = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_START:
		atomic_set(&smi2021->adev_capturing, 1);
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		atomic_set(&smi2021->adev_capturing, 0);
		break;
	default:
		return -EINVAL;
	}

	schedule_work(&smi2021->adev_capture_trigger);

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

/*
 * Mark the partial frame at the head of the ring buffer as complete, so that
 * the next chunk of audio starts on a frame boundary.
 */
static void smi2021_pcm_skip_partial(struct smi2021 *smi2021,
				     struct snd_pcm_runtime *runtime,
				     unsigned int stride)
{
	unsigned long flags;

	snd_pcm_stream_lock_irqsave(smi2021->pcm_substream, flags);

	smi2021->pcm_write_ptr += stride - (smi2021->pcm_write_ptr % stride);
	if (smi2021->pcm_write_ptr >= runtime->dma_bytes)
		smi2021->pcm_write_ptr -= runtime->dma_bytes;

	snd_pcm_stream_unlock_irqrestore(smi2021->pcm_substream, flags);
}

void smi2021_audio(struct smi2021 *smi2021, const u8 *data, unsigned int len)
{
	struct snd_pcm_runtime *runtime;
	unsigned int new_offset = 0;
	unsigned int stride, oldptr;
	bool period_elapsed = false;
	unsigned long flags;
	int diff, samples;
	u8 offset;

	if (!smi2021->udev)
		return;

	if (!atomic_read(&smi2021->adev_capturing))
		return;

	if (!smi2021->pcm_substream)
		return;

	runtime = smi2021->pcm_substream->runtime;
	if (!runtime || !runtime->dma_area)
		return;

	stride = runtime->frame_bits >> 3;
	if (!stride)
		return;

	offset = smi2021->pcm_read_offset;
	diff = smi2021->pcm_write_ptr;

	/*
	 * Check that the end of the last buffer was correct. If it was not,
	 * mark any partial frame in the buffer as complete.
	 */
	if (smi2021->pcm_write_ptr > (unsigned int)offset + 4) {
		unsigned int headptr = smi2021->pcm_write_ptr - offset - 4;

		if (runtime->dma_area[headptr] != 0x00) {
			smi2021_pcm_skip_partial(smi2021, runtime, stride);
			smi2021->pcm_read_offset = 0;
			offset = 0;
		}
	}

	/*
	 * The device sends 24 bit PCM data with 0x00 as the header byte
	 * before each sample. Look for that byte to make sure no bytes were
	 * lost during the transfer.
	 */
	while (len > stride && (data[offset] != 0x00 ||
				data[offset + (stride / 2)] != 0x00)) {
		new_offset++;
		data++;
		len--;
	}

	if (len <= stride) {
		/* The buffer was exhausted while looking for 0x00 */
		smi2021->pcm_read_offset = 0;
		return;
	}

	if (new_offset) {
		/*
		 * This buffer can not be appended to the current one, so mark
		 * any partial frame in the buffer as complete.
		 */
		smi2021_pcm_skip_partial(smi2021, runtime, stride);
		smi2021->pcm_read_offset = new_offset % (stride / 2);
		offset = smi2021->pcm_read_offset;
	}

	oldptr = smi2021->pcm_write_ptr;
	if (oldptr + len >= runtime->dma_bytes) {
		unsigned int cnt = runtime->dma_bytes - oldptr;

		memcpy(runtime->dma_area + oldptr, data, cnt);
		memcpy(runtime->dma_area, data + cnt, len - cnt);
	} else {
		memcpy(runtime->dma_area + oldptr, data, len);
	}

	snd_pcm_stream_lock_irqsave(smi2021->pcm_substream, flags);

	smi2021->pcm_write_ptr += len;
	if (smi2021->pcm_write_ptr >= runtime->dma_bytes)
		smi2021->pcm_write_ptr -= runtime->dma_bytes;

	samples = smi2021->pcm_write_ptr - diff;
	if (samples < 0)
		samples += runtime->dma_bytes;

	samples /= (stride / 2);

	smi2021->pcm_complete_samples += samples;
	if (smi2021->pcm_complete_samples / 2 >= runtime->period_size) {
		smi2021->pcm_complete_samples -= runtime->period_size * 2;
		period_elapsed = true;
	}

	snd_pcm_stream_unlock_irqrestore(smi2021->pcm_substream, flags);

	if (period_elapsed)
		snd_pcm_period_elapsed(smi2021->pcm_substream);
}

int smi2021_snd_register(struct smi2021 *smi2021)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int rc;

	INIT_WORK(&smi2021->adev_capture_trigger, capture_trigger);

	rc = snd_card_new(smi2021->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			  THIS_MODULE, 0, &card);
	if (rc < 0)
		return rc;

	rc = snd_pcm_new(card, "smi2021 Audio", 0, 0, 1, &pcm);
	if (rc < 0)
		goto err_free_card;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &smi2021_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	pcm->info_flags = 0;
	pcm->private_data = smi2021;
	strscpy(pcm->name, "Somagic smi2021 Capture", sizeof(pcm->name));

	strscpy(card->driver, "smi2021-Audio", sizeof(card->driver));
	strscpy(card->shortname, "smi2021 Audio", sizeof(card->shortname));
	strscpy(card->longname, "Somagic smi2021 Audio", sizeof(card->longname));

	rc = snd_card_register(card);
	if (rc < 0)
		goto err_free_card;

	smi2021->snd_card = card;

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
	 * Disconnect first, so that the pcm callbacks stop running and the
	 * trigger work can not be scheduled again.
	 */
	snd_card_disconnect(smi2021->snd_card);
	cancel_work_sync(&smi2021->adev_capture_trigger);

	snd_card_free(smi2021->snd_card);
	smi2021->snd_card = NULL;
}
