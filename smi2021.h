/* SPDX-License-Identifier: GPL-2.0-or-later */
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

#ifndef SMI2021_H
#define SMI2021_H

#include <linux/atomic.h>
#include <linux/i2c.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#include <sound/core.h>
#include <sound/pcm.h>

#define SMI2021_ISOC_TRANSFERS		16

/* The isochronous stream consists of chunks starting with a magic word */
#define SMI2021_CHUNK_SIZE		0x400
#define SMI2021_CHUNK_HDR_SIZE		sizeof(__be32)
#define SMI2021_CHUNK_DATA_SIZE		(SMI2021_CHUNK_SIZE - SMI2021_CHUNK_HDR_SIZE)

/* Head byte of the request that reads the hardware state or sets the mode */
#define SMI2021_HW_STATE_HEAD		0x01

/* The decoder sends 720 pixels per line in UYVY, two bytes per pixel */
#define SMI2021_WIDTH			720
#define SMI2021_BYTES_PER_LINE		(SMI2021_WIDTH * 2)
#define SMI2021_PAL_LINES		576
#define SMI2021_NTSC_LINES		480

struct smi2021_hw_state {
	u8 head;
	u8 state;
} __packed;

struct smi2021_buf {
	/* Common vb2 stuff, must be first */
	struct vb2_v4l2_buffer		vb;
	struct list_head		list;

	void				*mem;
	/* Set once the device has started sending the second field */
	bool				second_field;
	/* Set while the device is sending blanking instead of video */
	bool				in_blank;
	/* Bytes of active video received for the current field */
	unsigned int			pos;
};

struct smi2021_input {
	const char			*name;
	/* Input of the decoder that the connector is wired to */
	u32				route;
};

struct smi2021_board {
	const char			*name;
	const struct smi2021_input	*inputs;
	unsigned int			num_inputs;
};

struct smi2021 {
	struct device			*dev;
	/*
	 * Cleared on disconnect with both stream_lock and reg_lock held, so
	 * either of them keeps it from changing.
	 */
	struct usb_device		*udev;
	/* Device number, which selects the card= module parameter */
	unsigned int			nr;
	const struct smi2021_board	*board;

	/* Serializes register accesses, some of which take several requests */
	struct mutex			reg_lock;

	struct i2c_adapter		i2c_adap;
	struct v4l2_subdev		*decoder;

	struct v4l2_ctrl_handler	ctrl_handler;
	struct v4l2_device		v4l2_dev;
	struct video_device		vdev;
	struct vb2_queue		vb_vidq;

	/* Protects the v4l2 ioctls, those of the videobuf2 queue included */
	struct mutex			v4l2_lock;

	/*
	 * Protects the stream state below and the isochronous transfers. Held
	 * by the videobuf2 callbacks and by the pcm trigger work.
	 */
	struct mutex			stream_lock;
	/*
	 * Mode of the running stream, standby while nothing is streaming.
	 * Also read by the URB completion handler, which only runs while it
	 * does not change.
	 */
	u8				stream_mode;
	/* Whether the video device and the pcm device want data */
	bool				video_wanted;
	bool				audio_wanted;

	u8				ifnum;
	u8				iso_ep;
	unsigned int			iso_size;
	unsigned int			iso_interval;
	unsigned int			num_urbs;
	struct urb			*urbs[SMI2021_ISOC_TRANSFERS];

	/* Protects avail_bufs */
	spinlock_t			buf_lock;
	struct list_head		avail_bufs;

	/*
	 * Only used by the URB completion handler while streaming, and with
	 * the URBs stopped otherwise.
	 */
	struct smi2021_buf		*cur_buf;
	/* Number of bytes of the 0xff 0x00 0x00 code prefix seen so far */
	u8				trc_prefix_len;
	bool				last_field2;
	u32				sequence;

	unsigned int			cur_height;
	v4l2_std_id			cur_norm;
	unsigned int			cur_input;

	struct snd_card			*snd_card;
	struct snd_pcm_substream	*pcm_substream;
	unsigned int			pcm_write_ptr;
	unsigned int			pcm_period_bytes;
	/*
	 * Whether the completion handler may write into the ring buffer.
	 * Changed by the trigger callback, under the pcm stream lock that
	 * smi2021_parse_audio() holds as well; atomic only for the lockless
	 * read in the trigger work.
	 */
	atomic_t			pcm_capturing;
	struct work_struct		pcm_trigger_work;
};

/* Provided by smi2021-bootloader.c */
int smi2021_bootloader_probe(struct usb_interface *intf);

/* Provided by smi2021-main.c */
int smi2021_ctrl_write(struct usb_device *udev, u16 value, const void *data,
		       u16 size);
int smi2021_ctrl_read(struct usb_device *udev, u16 value, void *data,
		      u16 size);
int smi2021_request_audio(struct smi2021 *smi2021, bool enable);
int smi2021_start_streaming(struct vb2_queue *vq, unsigned int count);
void smi2021_stop_streaming(struct vb2_queue *vq);
void smi2021_release(struct v4l2_device *v4l2_dev);

/* Provided by smi2021-v4l2.c */
int smi2021_v4l2_init(struct smi2021 *smi2021);
void smi2021_v4l2_cleanup(struct smi2021 *smi2021);
int smi2021_v4l2_register(struct smi2021 *smi2021);
void smi2021_v4l2_unregister(struct smi2021 *smi2021);
void smi2021_clear_queue(struct smi2021 *smi2021, enum vb2_buffer_state state);
void smi2021_parse_video(struct smi2021 *smi2021, const u8 *data,
			 unsigned int len);

/* Provided by smi2021-audio.c */
int smi2021_snd_register(struct smi2021 *smi2021);
void smi2021_snd_unregister(struct smi2021 *smi2021);
void smi2021_parse_audio(struct smi2021 *smi2021, const u8 *data,
			 unsigned int len);

#endif /* SMI2021_H */
