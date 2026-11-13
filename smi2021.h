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

#include <linux/i2c.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <media/i2c/saa7115.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#include <sound/core.h>
#include <sound/pcm.h>

/* Number of isochronous URBs and of packets per URB */
#define SMI2021_ISOC_TRANSFERS		16
#define SMI2021_ISOC_PACKETS		10

/* Vendor specific USB control request used for all register access */
#define SMI2021_USB_REQUEST		0x01
#define SMI2021_USB_INDEX		0x00

/* The device splits the isochronous stream into chunks of this size */
#define SMI2021_CHUNK_SIZE		0x400

/* General video constants */
#define SMI2021_BYTES_PER_LINE		1440
#define SMI2021_PAL_LINES		576
#define SMI2021_NTSC_LINES		480

/* ITU-R BT.656 timing reference codes, see the saa7113 datasheet */
#define SMI2021_TRC_EAV			0x10
#define SMI2021_TRC_VBI			0x20
#define SMI2021_TRC_FIELD_2		0x40
#define SMI2021_TRC			0x80

/* Struct passed over USB during firmware upload */
struct smi2021_set_hw_state {
	u8 head;
	u8 state;
} __packed;

/* A single videobuf2 frame buffer */
struct smi2021_buf {
	/* Common vb2 stuff, must be first */
	struct vb2_v4l2_buffer		vb;
	struct list_head		list;

	void				*mem;
	unsigned int			length;

	/* Set once the device has started sending the second field */
	bool				second_field;
	/* Set while the device is sending blanking instead of video */
	bool				in_blank;
	/* Bytes of active video received for the current field */
	unsigned int			pos;
};

struct smi2021_vid_input {
	const char			*name;
	int				type;
};

/* State of the BT.656 timing reference code scanner */
enum smi2021_sync {
	HSYNC,
	SYNCZ1,
	SYNCZ2,
	TRC,
};

struct smi2021 {
	struct device			*dev;
	struct usb_device		*udev;

	struct i2c_adapter		i2c_adap;
	struct saa7115_platform_data	decoder_pdata;
	enum saa7113_r10_ofts		decoder_r10_ofts;
	bool				decoder_r10_vrln;
	bool				decoder_r13_adlsb;
	struct v4l2_subdev		*decoder;

	struct v4l2_ctrl_handler	ctrl_handler;
	struct v4l2_device		v4l2_dev;
	struct video_device		vdev;
	struct vb2_queue		vb_vidq;

	/* Protects the v4l2 ioctls and all hardware access */
	struct mutex			v4l2_lock;
	/* Protects the videobuf2 queue */
	struct mutex			vb_queue_lock;

	/* Isochronous transfers */
	unsigned int			iso_size;
	u8				isoc_ep;
	unsigned int			num_urbs;
	struct urb			**urbs;

	/* Protects avail_bufs and cur_buf */
	spinlock_t			buf_lock;
	struct list_head		avail_bufs;
	struct smi2021_buf		*cur_buf;

	u32				sequence;

	/* Frame settings */
	unsigned int			cur_height;
	v4l2_std_id			cur_norm;
	enum smi2021_sync		sync_state;

	/* Device settings */
	unsigned int			vid_input_count;
	const struct smi2021_vid_input	*vid_inputs;
	unsigned int			cur_input;

	/* Audio */
	struct snd_card			*snd_card;
	struct snd_pcm_substream	*pcm_substream;
	unsigned int			pcm_write_ptr;
	unsigned int			pcm_complete_samples;
	u8				pcm_read_offset;
	struct work_struct		adev_capture_trigger;
	atomic_t			adev_capturing;
};

/* Provided by smi2021_bootloader.c */
int smi2021_bootloader_probe(struct usb_interface *intf,
			     const struct usb_device_id *devid);

/* Provided by smi2021_main.c */
void smi2021_toggle_audio(struct smi2021 *smi2021, bool enable);
int smi2021_start(struct smi2021 *smi2021);
void smi2021_stop(struct smi2021 *smi2021);

/* Provided by smi2021_v4l2.c */
int smi2021_vb2_setup(struct smi2021 *smi2021);
int smi2021_video_register(struct smi2021 *smi2021);
void smi2021_clear_queue(struct smi2021 *smi2021, enum vb2_buffer_state state);

/* Provided by smi2021_audio.c */
int smi2021_snd_register(struct smi2021 *smi2021);
void smi2021_snd_unregister(struct smi2021 *smi2021);
void smi2021_audio(struct smi2021 *smi2021, const u8 *data, unsigned int len);

#endif /* SMI2021_H */
