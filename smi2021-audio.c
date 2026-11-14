// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for the Somagic SMI2021 USB video capture bridge
 *
 * Copyright (C) 2011-2013 Jon Arne Jørgensen <jonjon.arnearne@gmail.com>
 * Copyright (C) 2011, 2012 Tony Brown, Michal Demin, Jeffry Johnston
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * This driver is heavily influenced by the stk1160 driver:
 * Copyright (C) 2012 Ezequiel Garcia <elezegarcia@gmail.com>
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/math.h>
#include <linux/sizes.h>

#include <sound/initval.h>
#include <sound/pcm_params.h>

#include "smi2021.h"

/*
 * The device sends 24 bit samples in 32 bit containers, the lowest byte of
 * which is a 0x00 header that the driver uses to detect lost bytes.
 */
#define SMI2021_AUDIO_SAMPLE_SIZE	sizeof(u32)
#define SMI2021_AUDIO_CHANNELS		2
#define SMI2021_AUDIO_FRAME_SIZE	(SMI2021_AUDIO_CHANNELS * \
					 SMI2021_AUDIO_SAMPLE_SIZE)

/*
 * During a video capture, audio data arrives in whole chunks, so shorter
 * periods would elapse in bursts. The buffer is in vmalloc memory and not limited by the hardware,
 * 256 KiB hold about 680 ms.
 */
#define SMI2021_AUDIO_PERIOD_MIN	round_up(SMI2021_CHUNK_DATA_SIZE, \
						 SMI2021_AUDIO_FRAME_SIZE)
#define SMI2021_AUDIO_BUFFER_MAX	SZ_256K

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
	.channels_min = SMI2021_AUDIO_CHANNELS,
	.channels_max = SMI2021_AUDIO_CHANNELS,
	.period_bytes_min = SMI2021_AUDIO_PERIOD_MIN,
	.period_bytes_max = SMI2021_AUDIO_BUFFER_MAX / 2,
	.periods_min = 2,
	.periods_max = SMI2021_AUDIO_BUFFER_MAX / SMI2021_AUDIO_PERIOD_MIN,
	.buffer_bytes_max = SMI2021_AUDIO_BUFFER_MAX,
};

static int smi2021_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	runtime->hw = smi2021_pcm_hw;

	return snd_pcm_hw_constraint_integer(runtime,
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

	return 0;
}

/*
 * Starting the stream takes several control requests, so the trigger callback
 * defers it here. If it fails there is no way to report that to the pcm device
 * other than stopping the stream, which beats letting it wait for data that is
 * never going to arrive.
 */
static void smi2021_pcm_trigger_work(struct work_struct *work)
{
	struct smi2021 *smi2021 = container_of(work, struct smi2021,
					       pcm_trigger_work);
	struct snd_pcm_substream *substream = smi2021->pcm_substream;
	bool capturing = atomic_read(&smi2021->pcm_capturing);
	int rc;

	rc = smi2021_request_audio(smi2021, capturing);
	if (rc && capturing && substream) {
		dev_err(smi2021->dev, "cannot start the audio stream: %pe\n",
			ERR_PTR(rc));
		snd_pcm_stop_xrun(substream);
	}
}

/* Called with the pcm stream lock held, so this must not sleep */
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

/* Mark the partial frame at the head of the ring buffer, if any, as complete */
static void smi2021_pcm_align(struct smi2021 *smi2021,
			      struct snd_pcm_runtime *runtime)
{
	unsigned int partial = smi2021->pcm_write_ptr % SMI2021_AUDIO_FRAME_SIZE;

	if (partial)
		smi2021_pcm_advance(smi2021, runtime,
				    SMI2021_AUDIO_FRAME_SIZE - partial);
}

/*
 * Copy the audio data of one chunk, or of one packet in the audio only mode,
 * into the ring buffer. Called with the pcm stream lock held. Returns whether
 * a period has elapsed.
 *
 * The samples in the ring buffer start at multiples of the sample size, so
 * the write pointer tells how many bytes of the last sample the chunk has to
 * complete, and where in the chunk the header bytes of the following samples
 * have to be. When they are not there, bytes got lost: mark the partial frame
 * at the head of the ring buffer as complete, and start again on the frame
 * boundary with the first two samples in a row whose header bytes are found.
 */
static bool smi2021_copy_audio(struct smi2021 *smi2021,
			       struct snd_pcm_runtime *runtime,
			       const u8 *data, unsigned int len)
{
	unsigned int sample = SMI2021_AUDIO_SAMPLE_SIZE;
	unsigned int period = frames_to_bytes(runtime, runtime->period_size);
	unsigned int rest = (sample - smi2021->pcm_write_ptr % sample) % sample;

	if (len <= rest + sample || data[rest] || data[rest + sample]) {
		unsigned int skip;

		for (skip = 0; skip + sample < len; skip++)
			if (!data[skip] && !data[skip + sample])
				break;

		/* No header bytes found in this chunk */
		if (skip + sample >= len)
			return false;

		smi2021_pcm_align(smi2021, runtime);
		data += skip;
		len -= skip;
	}

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

void smi2021_parse_audio(struct smi2021 *smi2021, const u8 *data,
			 unsigned int len)
{
	struct snd_pcm_substream *substream = smi2021->pcm_substream;

	if (!substream)
		return;

	/*
	 * pcm_capturing only changes in the trigger callback, which runs with
	 * the pcm stream lock held, so the runtime is valid while it is set.
	 */
	guard(pcm_stream_lock_irqsave)(substream);

	if (atomic_read(&smi2021->pcm_capturing) &&
	    smi2021_copy_audio(smi2021, substream->runtime, data, len))
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

/*
 * The completion handler uses pcm_substream, so the URBs must have been
 * stopped for good before calling this, see smi2021_shutdown().
 */
void smi2021_snd_unregister(struct smi2021 *smi2021)
{
	if (!smi2021->snd_card)
		return;

	/*
	 * This stops the stream and prevents any further pcm callback except
	 * for close, without waiting for userspace to close the device. The
	 * trigger work uses the substream, so it has to be done with before
	 * the card can be freed.
	 */
	snd_card_disconnect(smi2021->snd_card);
	cancel_work_sync(&smi2021->pcm_trigger_work);
	snd_card_free_when_closed(smi2021->snd_card);

	smi2021->snd_card = NULL;
	smi2021->pcm_substream = NULL;
}
