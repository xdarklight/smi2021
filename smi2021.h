/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Driver for the Somagic SMI2021 USB video capture bridge
 *
 * Copyright (C) 2011-2013 Jon Arne Jørgensen <jonjon.arnearne@gmail.com>
 * Copyright (C) 2011, 2012 Tony Brown, Michal Demin, Jeffry Johnston
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
	u32				type;
};

struct smi2021_board {
	const struct smi2021_input	*inputs;
	unsigned int			num_inputs;
};

struct smi2021 {
	struct device			*dev;
	struct usb_device		*udev;
	const struct smi2021_board	*board;

	/* Serializes register accesses, some of which take several requests */
	struct mutex			reg_lock;

	struct i2c_adapter		i2c_adap;
	struct v4l2_subdev		*decoder;

	struct v4l2_ctrl_handler	ctrl_handler;
	struct v4l2_device		v4l2_dev;
	struct video_device		vdev;
	struct vb2_queue		vb_vidq;

	/* Protects the v4l2 ioctls */
	struct mutex			v4l2_lock;
	/* Protects the videobuf2 queue and the isochronous transfers */
	struct mutex			vb_queue_lock;

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
	u8				pcm_read_offset;
	/* Only changed by the trigger callback, under the stream lock */
	atomic_t			pcm_capturing;
	struct work_struct		pcm_trigger_work;
};

/* Provided by smi2021_bootloader.c */
int smi2021_bootloader_probe(struct usb_interface *intf);

/* Provided by smi2021_main.c */
int smi2021_ctrl_write(struct usb_device *udev, u16 value, const void *data,
		       u16 size);
int smi2021_ctrl_read(struct usb_device *udev, u16 value, void *data,
		      u16 size);
void smi2021_toggle_audio(struct smi2021 *smi2021, bool enable);
int smi2021_start_streaming(struct vb2_queue *vq, unsigned int count);
void smi2021_stop_streaming(struct vb2_queue *vq);

/* Provided by smi2021_v4l2.c */
int smi2021_vb2_setup(struct smi2021 *smi2021);
int smi2021_video_register(struct smi2021 *smi2021);
void smi2021_clear_queue(struct smi2021 *smi2021, enum vb2_buffer_state state);

/* Provided by smi2021_audio.c */
int smi2021_snd_register(struct smi2021 *smi2021);
void smi2021_snd_unregister(struct smi2021 *smi2021);
void smi2021_audio(struct smi2021 *smi2021, const u8 *data, unsigned int len);

#endif /* SMI2021_H */
