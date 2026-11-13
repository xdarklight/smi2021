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

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/cleanup.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <media/i2c/saa7115.h>
#include <media/v4l2-common.h>

#include "smi2021.h"

#define SMI2021_USB_VENDOR_ID		0x1c88
/* All devices enumerate with this id until their firmware is loaded */
#define SMI2021_USB_BOOTLOADER_ID	0x0007

/* Vendor specific control request used for all register access */
#define SMI2021_USB_REQUEST		0x01
#define SMI2021_USB_INDEX		0x00

/* Alternate settings without and with the isochronous endpoint */
#define SMI2021_ALT_IDLE		0
#define SMI2021_ALT_STREAM		2
#define SMI2021_ISOC_PACKETS		10

/*
 * Modes set with a hardware state request. The firmware starts in standby, and
 * also knows the modes 0x02 and 0x04, whose purpose is unknown.
 */
#define SMI2021_MODE_STANDBY		0x03
#define SMI2021_MODE_CAPTURE		0x05

/* Head byte of a register access request */
#define SMI2021_REG_CTRL_HEAD		0x0b

/*
 * Flags in smi2021_reg_msg.flags, whose low bits hold the number of address
 * bytes at the start of the data. A bridge register is addressed by its 16 bit
 * address in the data space of the bridge's microcontroller, otherwise this is
 * an i2c transfer.
 */
#define SMI2021_REG_FLAG_BRIDGE		BIT(7)

/* Commands of the bridge's i2c engine, for smi2021_reg_msg.cntl */
#define SMI2021_I2C_CMD_WRITE		0x84	/* write the data bytes */
#define SMI2021_I2C_CMD_READ		0xa0	/* read the data bytes */
#define SMI2021_I2C_CMD_WRITE_REG	0xc0	/* write subaddress and data */
/*
 * The firmware retries a failed transfer 16 times. If it still fails, this
 * flag is set in the reply, so a failure is only reported for reads.
 */
#define SMI2021_I2C_CMD_NAK		BIT(0)

/*
 * The bridge's microcontroller is an AVR whose I/O registers match those of an
 * ATmega16, so these are its GPIO port registers.
 */
#define SMI2021_REG_DDRC		0x34
#define SMI2021_REG_PORTC		0x35
#define SMI2021_REG_DDRA		0x3a
#define SMI2021_REG_PORTA		0x3b

/*
 * Audio engine control. Writing 0x1d starts the transfer of audio data and
 * 0x00 stops it. When streaming starts, the firmware itself only sets 0x05.
 */
#define SMI2021_REG_AUDIO_CTRL		0x1740
#define SMI2021_REG_AUDIO_CTRL_ON	0x1d
#define SMI2021_REG_AUDIO_CTRL_OFF	0x00

/* 7 bit I2C address of the saa7113 compatible decoder, 4AH in its datasheet */
#define SMI2021_DECODER_I2C_ADDR	0x25

/* Magic words that identify the payload of an isochronous chunk */
#define SMI2021_CHUNK_MAGIC_VIDEO	0xaaaa0000
#define SMI2021_CHUNK_MAGIC_AUDIO	0xaaaa0001

/*
 * Bits of the fourth word of a timing reference code, see Table 2 of ITU-R
 * BT.656. The protection bits are ignored, as the decoder datasheet advises.
 */
#define SMI2021_TRC_H			BIT(4)	/* EAV rather than SAV */
#define SMI2021_TRC_V			BIT(5)	/* vertical blanking */
#define SMI2021_TRC_F			BIT(6)	/* second field */

/*
 * A register access request. The data starts with the address bytes and is
 * followed by the data bytes; the firmware replies to reads in the same layout.
 */
struct smi2021_reg_msg {
	u8 head;
	u8 i2c_addr;	/* 8 bit address, i.e. the 7 bit address shifted */
	u8 cntl;	/* i2c engine command, 0 to write a bridge register */
	u8 flags;	/* SMI2021_REG_FLAG_* | number of address bytes */
	u8 size;	/* number of data bytes */
	union {
		u8 val;
		struct {
			u8 reg;
			u8 val;
		} __packed i2c;
		struct {
			__be16 reg;
			u8 val;
		} __packed bridge;
		u8 reserved[8];
	} __packed data;
} __packed;
/* The size of the request on the wire */
static_assert(sizeof(struct smi2021_reg_msg) == 13);

int smi2021_ctrl_write(struct usb_device *udev, u16 value, const void *data,
		       u16 size)
{
	return usb_control_msg_send(udev, 0, SMI2021_USB_REQUEST,
				    USB_DIR_OUT | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE, value, SMI2021_USB_INDEX,
				    data, size, USB_CTRL_SET_TIMEOUT,
				    GFP_KERNEL);
}

int smi2021_ctrl_read(struct usb_device *udev, u16 value, void *data,
		      u16 size)
{
	return usb_control_msg_recv(udev, 0, SMI2021_USB_REQUEST,
				    USB_DIR_IN | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE, value, SMI2021_USB_INDEX,
				    data, size, USB_CTRL_GET_TIMEOUT,
				    GFP_KERNEL);
}

static int smi2021_send_reg_msg(struct smi2021 *smi2021,
				const struct smi2021_reg_msg *msg)
{
	return smi2021_ctrl_write(smi2021->udev, msg->head, msg, sizeof(*msg));
}

static int smi2021_set_mode(struct smi2021 *smi2021, u8 mode)
{
	struct smi2021_hw_state state = {
		.head = SMI2021_HW_STATE_HEAD,
		.state = mode,
	};

	guard(mutex)(&smi2021->reg_lock);

	return smi2021_ctrl_write(smi2021->udev, state.head, &state,
				  sizeof(state));
}

static int smi2021_write_reg(struct smi2021 *smi2021, u16 reg, u8 val)
{
	struct smi2021_reg_msg msg = {
		.head = SMI2021_REG_CTRL_HEAD,
		.flags = SMI2021_REG_FLAG_BRIDGE | sizeof(msg.data.bridge.reg),
		.size = sizeof(msg.data.bridge.val),
		.data.bridge.reg = cpu_to_be16(reg),
		.data.bridge.val = val,
	};

	guard(mutex)(&smi2021->reg_lock);

	return smi2021_send_reg_msg(smi2021, &msg);
}

static int smi2021_i2c_write(struct smi2021 *smi2021, u8 addr, u8 reg, u8 val)
{
	struct smi2021_reg_msg msg = {
		.head = SMI2021_REG_CTRL_HEAD,
		.i2c_addr = addr << 1,
		.cntl = SMI2021_I2C_CMD_WRITE_REG,
		.flags = sizeof(msg.data.i2c.reg),
		.size = sizeof(msg.data.i2c.val),
		.data.i2c.reg = reg,
		.data.i2c.val = val,
	};

	guard(mutex)(&smi2021->reg_lock);

	return smi2021_send_reg_msg(smi2021, &msg);
}

static int smi2021_i2c_read(struct smi2021 *smi2021, u8 addr, u8 reg, u8 *val)
{
	struct smi2021_reg_msg msg = {
		.head = SMI2021_REG_CTRL_HEAD,
		.i2c_addr = addr << 1,
		.cntl = SMI2021_I2C_CMD_WRITE,
		.size = sizeof(reg),
		.data.val = reg,
	};
	int rc;

	guard(mutex)(&smi2021->reg_lock);

	/* Set the register pointer, then read one byte */
	rc = smi2021_send_reg_msg(smi2021, &msg);
	if (rc)
		return rc;

	msg.cntl = SMI2021_I2C_CMD_READ;
	rc = smi2021_send_reg_msg(smi2021, &msg);
	if (rc)
		return rc;

	rc = smi2021_ctrl_read(smi2021->udev, msg.head, &msg, sizeof(msg));
	if (rc)
		return rc;

	if (msg.cntl & SMI2021_I2C_CMD_NAK)
		return -EIO;

	*val = msg.data.val;

	return 0;
}

static int smi2021_smbus_xfer(struct i2c_adapter *adap, u16 addr,
			      unsigned short flags, char read_write, u8 command,
			      int size, union i2c_smbus_data *data)
{
	struct smi2021 *smi2021 = i2c_get_adapdata(adap);

	if (size != I2C_SMBUS_BYTE_DATA)
		return -EOPNOTSUPP;

	if (read_write == I2C_SMBUS_READ)
		return smi2021_i2c_read(smi2021, addr, command, &data->byte);

	return smi2021_i2c_write(smi2021, addr, command, data->byte);
}

static u32 smi2021_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_BYTE_DATA;
}

static const struct i2c_algorithm smi2021_i2c_algo = {
	.smbus_xfer = smi2021_smbus_xfer,
	.functionality = smi2021_i2c_functionality,
};

static int smi2021_initialize(struct smi2021 *smi2021)
{
	/*
	 * Pulse the GPIO lines PA7, PC0 and PC4, as the vendor driver does.
	 * What they are connected to is not known; presumably these are the
	 * reset inputs of the audio ADC and of the video decoder.
	 */
	static const struct {
		u16 reg;
		u8 val;
	} init[] = {
		{ SMI2021_REG_DDRA, BIT(7) },
		{ SMI2021_REG_PORTA, 0 },
		{ SMI2021_REG_DDRC, BIT(0) },
		{ SMI2021_REG_PORTC, 0 },
		{ SMI2021_REG_DDRC, BIT(4) | BIT(0) },
		{ SMI2021_REG_PORTC, BIT(4) | BIT(0) },
		{ SMI2021_REG_PORTA, BIT(7) },
		{ SMI2021_REG_PORTA, 0 },
	};
	unsigned int i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(init); i++) {
		rc = smi2021_write_reg(smi2021, init[i].reg, init[i].val);
		if (rc)
			return rc;
	}

	return 0;
}

static struct smi2021_buf *smi2021_get_buf(struct smi2021 *smi2021)
{
	struct smi2021_buf *buf;

	guard(spinlock_irqsave)(&smi2021->buf_lock);

	buf = list_first_entry_or_null(&smi2021->avail_bufs,
				       struct smi2021_buf, list);
	if (buf)
		list_del(&buf->list);

	return buf;
}

/* Each of the two interlaced fields carries every other line of a frame */
static unsigned int smi2021_field_lines(const struct smi2021 *smi2021)
{
	return smi2021->cur_height / 2;
}

/* Whether all lines of the current field have been received */
static bool smi2021_field_done(struct smi2021 *smi2021,
			       const struct smi2021_buf *buf)
{
	return buf->pos >= SMI2021_BYTES_PER_LINE * smi2021_field_lines(smi2021);
}

static void smi2021_buf_done(struct smi2021 *smi2021)
{
	struct smi2021_buf *buf = smi2021->cur_buf;

	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.sequence = smi2021->sequence++;
	buf->vb.field = V4L2_FIELD_INTERLACED;
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0,
			      SMI2021_BYTES_PER_LINE * smi2021->cur_height);
	vb2_buffer_done(&buf->vb.vb2_buf,
			smi2021_field_done(smi2021, buf) ?
			VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);

	smi2021->cur_buf = NULL;
}

/*
 * Parse the fourth word of a timing reference code.
 *
 * A frame starts with the first SAV of the first field that follows a second
 * field. Hand the current buffer back there and start filling the next one.
 */
static void smi2021_parse_trc(struct smi2021 *smi2021, u8 trc)
{
	struct smi2021_buf *buf = smi2021->cur_buf;
	bool field2 = trc & SMI2021_TRC_F;
	bool frame_start;

	if (trc & SMI2021_TRC_H) {
		/* EAV: horizontal blanking follows */
		if (buf)
			buf->in_blank = true;
		return;
	}

	frame_start = smi2021->last_field2 && !field2;
	smi2021->last_field2 = field2;

	if (frame_start) {
		if (buf)
			smi2021_buf_done(smi2021);

		buf = smi2021_get_buf(smi2021);
		smi2021->cur_buf = buf;
	}

	if (!buf)
		return;

	buf->in_blank = trc & SMI2021_TRC_V;

	if (field2 && !buf->second_field) {
		if (!smi2021_field_done(smi2021, buf)) {
			/* The first field was short, drop the frame */
			smi2021_buf_done(smi2021);
			return;
		}

		buf->second_field = true;
		buf->pos = 0;
	}
}

/*
 * Copy active video data into the current buffer.
 *
 * The device sends the two fields of a frame one after the other while the
 * buffer holds them interleaved, so line n of a field goes to line 2n (first
 * field) or 2n + 1 (second field) of the buffer.
 */
static void smi2021_copy_video(struct smi2021 *smi2021, const u8 *src,
			       unsigned int len)
{
	unsigned int lines_per_field = smi2021_field_lines(smi2021);
	struct smi2021_buf *buf = smi2021->cur_buf;

	if (!buf || buf->in_blank)
		return;

	while (len) {
		unsigned int line = buf->pos / SMI2021_BYTES_PER_LINE;
		unsigned int col = buf->pos % SMI2021_BYTES_PER_LINE;
		unsigned int n = min(len, SMI2021_BYTES_PER_LINE - col);

		/* The decoder may flag more lines as active than we capture */
		if (line >= lines_per_field)
			return;

		memcpy(buf->mem + (2 * line + buf->second_field) *
		       SMI2021_BYTES_PER_LINE + col, src, n);

		buf->pos += n;
		src += n;
		len -= n;
	}
}

/*
 * Scan the video data from the decoder for ITU-R BT.656 timing reference
 * codes: 0xff 0x00 0x00 followed by a word with the field and blanking flags.
 * Everything else is video data. A code may be split across two chunks.
 */
static void smi2021_parse_video(struct smi2021 *smi2021, const u8 *p,
				unsigned int size)
{
	static const u8 trc_prefix[] = { 0xff, 0x00, 0x00 };
	unsigned int i = 0;

	while (i < size) {
		if (!smi2021->trc_prefix_len) {
			/* Copy video data up to the next potential code */
			const u8 *start = memchr(p + i, trc_prefix[0], size - i);
			unsigned int len = start ? start - (p + i) : size - i;

			smi2021_copy_video(smi2021, p + i, len);
			i += len;
			if (start) {
				smi2021->trc_prefix_len = 1;
				i++;
			}
		} else if (smi2021->trc_prefix_len == sizeof(trc_prefix)) {
			smi2021->trc_prefix_len = 0;
			smi2021_parse_trc(smi2021, p[i++]);
		} else if (p[i] == trc_prefix[smi2021->trc_prefix_len]) {
			smi2021->trc_prefix_len++;
			i++;
		} else {
			/*
			 * Not a code after all, so the prefix bytes seen so
			 * far are video data. Rescan the current byte, it may
			 * start a new code.
			 */
			smi2021_copy_video(smi2021, trc_prefix,
					   smi2021->trc_prefix_len);
			smi2021->trc_prefix_len = 0;
		}
	}
}

static void smi2021_process_packet(struct smi2021 *smi2021, const u8 *p,
				   unsigned int size)
{
	unsigned int i;

	if (size % SMI2021_CHUNK_SIZE) {
		dev_dbg_ratelimited(smi2021->dev,
				    "unexpected packet size %u\n", size);
		return;
	}

	for (i = 0; i < size; i += SMI2021_CHUNK_SIZE) {
		const u8 *data = p + i + SMI2021_CHUNK_HDR_SIZE;

		switch (get_unaligned_be32(p + i)) {
		case SMI2021_CHUNK_MAGIC_VIDEO:
			smi2021_parse_video(smi2021, data,
					    SMI2021_CHUNK_DATA_SIZE);
			break;
		case SMI2021_CHUNK_MAGIC_AUDIO:
			smi2021_audio(smi2021, data, SMI2021_CHUNK_DATA_SIZE);
			break;
		}
	}
}

static void smi2021_isoc_complete(struct urb *urb)
{
	struct smi2021 *smi2021 = urb->context;
	int i, rc;

	switch (urb->status) {
	case 0:
		break;
	case -ENODEV:
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* The URB was killed or the device is gone */
		return;
	default:
		dev_warn_ratelimited(smi2021->dev, "urb error %d\n",
				     urb->status);
		break;
	}

	for (i = 0; i < urb->number_of_packets; i++) {
		const struct usb_iso_packet_descriptor *desc =
			&urb->iso_frame_desc[i];

		if (!desc->status)
			smi2021_process_packet(smi2021,
					       urb->transfer_buffer +
					       desc->offset,
					       desc->actual_length);
	}

	rc = usb_submit_urb(urb, GFP_ATOMIC);
	if (rc)
		dev_warn_ratelimited(smi2021->dev,
				     "urb resubmit failed: %d\n", rc);
}

static void smi2021_kill_isoc(struct smi2021 *smi2021)
{
	unsigned int i;

	for (i = 0; i < smi2021->num_urbs; i++)
		usb_kill_urb(smi2021->urbs[i]);
}

/* All URBs must have been killed before calling this */
static void smi2021_free_isoc(struct smi2021 *smi2021)
{
	while (smi2021->num_urbs)
		usb_free_urb(smi2021->urbs[--smi2021->num_urbs]);
}

static int smi2021_alloc_isoc(struct smi2021 *smi2021)
{
	unsigned int size = SMI2021_ISOC_PACKETS * smi2021->iso_size;
	unsigned int i;

	while (smi2021->num_urbs < SMI2021_ISOC_TRANSFERS) {
		struct urb *urb = usb_alloc_urb(SMI2021_ISOC_PACKETS,
						GFP_KERNEL);

		if (!urb)
			goto err_free;

		smi2021->urbs[smi2021->num_urbs++] = urb;

		urb->transfer_buffer = kmalloc(size, GFP_KERNEL);
		if (!urb->transfer_buffer)
			goto err_free;

		urb->dev = smi2021->udev;
		urb->pipe = usb_rcvisocpipe(smi2021->udev, smi2021->iso_ep);
		urb->transfer_buffer_length = size;
		urb->transfer_flags = URB_ISO_ASAP | URB_FREE_BUFFER;
		urb->complete = smi2021_isoc_complete;
		urb->context = smi2021;
		urb->interval = smi2021->iso_interval;
		urb->number_of_packets = SMI2021_ISOC_PACKETS;

		for (i = 0; i < SMI2021_ISOC_PACKETS; i++) {
			urb->iso_frame_desc[i].offset = i * smi2021->iso_size;
			urb->iso_frame_desc[i].length = smi2021->iso_size;
		}
	}

	return 0;

err_free:
	smi2021_free_isoc(smi2021);

	return -ENOMEM;
}

void smi2021_toggle_audio(struct smi2021 *smi2021, bool enable)
{
	smi2021_write_reg(smi2021, SMI2021_REG_AUDIO_CTRL,
			  enable ? SMI2021_REG_AUDIO_CTRL_ON :
				   SMI2021_REG_AUDIO_CTRL_OFF);
}

int smi2021_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct smi2021 *smi2021 = vb2_get_drv_priv(vq);
	unsigned int i;
	int rc;

	smi2021->sequence = 0;
	smi2021->trc_prefix_len = 0;
	smi2021->last_field2 = false;

	rc = smi2021_alloc_isoc(smi2021);
	if (rc)
		goto err_clear_queue;

	v4l2_subdev_call(smi2021->decoder, video, s_stream, 1);

	rc = smi2021_set_mode(smi2021, SMI2021_MODE_CAPTURE);
	if (rc)
		goto err_stop_decoder;

	/* In capture mode this also makes the firmware start the video engine */
	rc = usb_set_interface(smi2021->udev, smi2021->ifnum,
			       SMI2021_ALT_STREAM);
	if (rc)
		goto err_standby;

	/* Only let audio data flow while the PCM device is capturing */
	smi2021_toggle_audio(smi2021, atomic_read(&smi2021->pcm_capturing));

	for (i = 0; i < smi2021->num_urbs; i++) {
		rc = usb_submit_urb(smi2021->urbs[i], GFP_KERNEL);
		if (rc) {
			dev_err(smi2021->dev, "cannot submit urb %u: %d\n",
				i, rc);
			goto err_kill_urbs;
		}
	}

	return 0;

err_kill_urbs:
	smi2021_kill_isoc(smi2021);
	usb_set_interface(smi2021->udev, smi2021->ifnum, SMI2021_ALT_IDLE);
err_standby:
	smi2021_set_mode(smi2021, SMI2021_MODE_STANDBY);
err_stop_decoder:
	v4l2_subdev_call(smi2021->decoder, video, s_stream, 0);
	smi2021_free_isoc(smi2021);
err_clear_queue:
	smi2021_clear_queue(smi2021, VB2_BUF_STATE_QUEUED);

	return rc;
}

void smi2021_stop_streaming(struct vb2_queue *vq)
{
	struct smi2021 *smi2021 = vb2_get_drv_priv(vq);

	smi2021_kill_isoc(smi2021);

	/* After a disconnect, this is called when the last user goes away */
	if (smi2021->udev) {
		v4l2_subdev_call(smi2021->decoder, video, s_stream, 0);
		smi2021_set_mode(smi2021, SMI2021_MODE_STANDBY);
		usb_set_interface(smi2021->udev, smi2021->ifnum,
				  SMI2021_ALT_IDLE);
	}

	smi2021_free_isoc(smi2021);
	smi2021_clear_queue(smi2021, VB2_BUF_STATE_ERROR);
}

/* Allocates the device numbers */
static DEFINE_IDA(smi2021_ida);

static void smi2021_release(struct v4l2_device *v4l2_dev)
{
	struct smi2021 *smi2021 = container_of(v4l2_dev, struct smi2021,
					       v4l2_dev);

	vb2_queue_release(&smi2021->vb_vidq);

	/* Unregister the decoder before the i2c adapter it lives on */
	v4l2_device_unregister(&smi2021->v4l2_dev);
	i2c_del_adapter(&smi2021->i2c_adap);
	v4l2_ctrl_handler_free(&smi2021->ctrl_handler);

	mutex_destroy(&smi2021->reg_lock);
	mutex_destroy(&smi2021->v4l2_lock);
	mutex_destroy(&smi2021->vb_queue_lock);

	ida_free(&smi2021_ida, smi2021->nr);
	kfree(smi2021);
}

/*
 * The driver tells active video from blanking by the V flag of the timing
 * reference codes. Have the decoder generate it from VREF with the long VREF
 * range: according to Tables 6 and 7 of the SAA7113H datasheet this flags 288
 * lines per field as active for 625 line standards and 242 lines for 525 line
 * ones, of which the driver captures the first 240.
 */
static enum saa7113_r10_ofts smi2021_decoder_ofts = SAA7113_OFTS_VFLAG_BY_VREF;
static bool smi2021_decoder_vrln = true;

static struct saa7115_platform_data smi2021_decoder_pdata = {
	.saa7113_r10_ofts = &smi2021_decoder_ofts,
	.saa7113_r10_vrln = &smi2021_decoder_vrln,
};

/*
 * All inputs of the decoder that a board may have connected. The USB product
 * id depends on the firmware that was uploaded rather than on the board, so
 * the inputs of a board can not be detected; offer all of them unless the
 * card module parameter selects a board.
 */
static const struct smi2021_input smi2021_all_inputs[] = {
	{ "Composite 0", SAA7115_COMPOSITE0 },
	{ "Composite 1", SAA7115_COMPOSITE1 },
	{ "Composite 2", SAA7115_COMPOSITE2 },
	{ "Composite 3", SAA7115_COMPOSITE3 },
	{ "S-Video", SAA7115_SVIDEO1 },
};

static const struct smi2021_input smi2021_dc60_inputs[] = {
	{ "Composite", SAA7115_COMPOSITE0 },
	{ "S-Video", SAA7115_SVIDEO1 },
};

/* Named and ordered after the labels of the connectors */
static const struct smi2021_input smi2021_easycap002_inputs[] = {
	{ "Video 1", SAA7115_COMPOSITE2 },
	{ "Video 2", SAA7115_COMPOSITE3 },
	{ "Video 3", SAA7115_COMPOSITE0 },
	{ "Video 4", SAA7115_COMPOSITE1 },
};

/* Values of the card module parameter */
enum smi2021_board_type {
	SMI2021_BOARD_ALL_INPUTS,
	SMI2021_BOARD_DC60,
	SMI2021_BOARD_EASYCAP002,
};

static const struct smi2021_board smi2021_boards[] = {
	[SMI2021_BOARD_ALL_INPUTS] = {
		.name = "all inputs",
		.inputs = smi2021_all_inputs,
		.num_inputs = ARRAY_SIZE(smi2021_all_inputs),
	},
	[SMI2021_BOARD_DC60] = {
		.name = "EasyCAP DC60",
		.inputs = smi2021_dc60_inputs,
		.num_inputs = ARRAY_SIZE(smi2021_dc60_inputs),
	},
	[SMI2021_BOARD_EASYCAP002] = {
		.name = "EasyCAP 002",
		.inputs = smi2021_easycap002_inputs,
		.num_inputs = ARRAY_SIZE(smi2021_easycap002_inputs),
	},
};

static unsigned int card[8];
module_param_array(card, uint, NULL, 0444);
MODULE_PARM_DESC(card,
		 "Board of each device, by device number: 0 = all inputs (default), 1 = EasyCAP DC60, 2 = EasyCAP 002");

static const struct smi2021_board *smi2021_get_board(struct smi2021 *smi2021)
{
	unsigned int type = SMI2021_BOARD_ALL_INPUTS;

	if (smi2021->nr < ARRAY_SIZE(card))
		type = card[smi2021->nr];

	if (type >= ARRAY_SIZE(smi2021_boards)) {
		dev_warn(smi2021->dev, "invalid card %u, offering all inputs\n",
			 type);
		type = SMI2021_BOARD_ALL_INPUTS;
	}

	return &smi2021_boards[type];
}

static const struct usb_device_id smi2021_usb_device_id_table[] = {
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, SMI2021_USB_BOOTLOADER_ID) },
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, 0x003c) },
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, 0x003d) },
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, 0x003e) },
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, 0x003f) },
	{ }
};
MODULE_DEVICE_TABLE(usb, smi2021_usb_device_id_table);

static int smi2021_usb_probe(struct usb_interface *intf,
			     const struct usb_device_id *devid)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct i2c_board_info decoder_info = {
		I2C_BOARD_INFO("saa7115_auto", SMI2021_DECODER_I2C_ADDR),
		.platform_data = &smi2021_decoder_pdata,
	};
	const struct usb_host_endpoint *ep;
	struct usb_host_interface *alt;
	struct smi2021 *smi2021;
	int rc;

	if (devid->idProduct == SMI2021_USB_BOOTLOADER_ID)
		return smi2021_bootloader_probe(intf);

	alt = usb_altnum_to_altsetting(intf, SMI2021_ALT_STREAM);
	if (!alt || alt->desc.bNumEndpoints != 1)
		return -ENODEV;

	ep = &alt->endpoint[0];
	if (!usb_endpoint_is_isoc_in(&ep->desc))
		return -ENODEV;

	smi2021 = kzalloc_obj(*smi2021);
	if (!smi2021)
		return -ENOMEM;

	rc = ida_alloc(&smi2021_ida, GFP_KERNEL);
	if (rc < 0) {
		kfree(smi2021);
		return rc;
	}

	smi2021->nr = rc;
	smi2021->dev = &intf->dev;
	smi2021->udev = udev;
	smi2021->board = smi2021_get_board(smi2021);
	smi2021->ifnum = alt->desc.bInterfaceNumber;
	smi2021->iso_ep = usb_endpoint_num(&ep->desc);
	/* 3072 bytes, as the endpoint has two additional transactions */
	smi2021->iso_size = usb_endpoint_max_periodic_payload(udev, ep);
	/* In microframes, the descriptor holds the exponent plus one */
	smi2021->iso_interval = 1 << (ep->desc.bInterval - 1);

	mutex_init(&smi2021->reg_lock);
	mutex_init(&smi2021->v4l2_lock);
	mutex_init(&smi2021->vb_queue_lock);
	spin_lock_init(&smi2021->buf_lock);
	INIT_LIST_HEAD(&smi2021->avail_bufs);

	smi2021->cur_norm = V4L2_STD_NTSC;
	smi2021->cur_height = SMI2021_NTSC_LINES;

	rc = smi2021_vb2_setup(smi2021);
	if (rc)
		goto err_free;

	rc = v4l2_ctrl_handler_init(&smi2021->ctrl_handler, 0);
	if (rc)
		goto err_vb2;

	smi2021->v4l2_dev.release = smi2021_release;
	smi2021->v4l2_dev.ctrl_handler = &smi2021->ctrl_handler;
	rc = v4l2_device_register(smi2021->dev, &smi2021->v4l2_dev);
	if (rc)
		goto err_ctrl;

	/* From here on smi2021_release() tears everything down */

	rc = smi2021_initialize(smi2021);
	if (rc) {
		rc = dev_err_probe(smi2021->dev, rc,
				   "failed to initialize device\n");
		goto err_put;
	}

	smi2021->i2c_adap.owner = THIS_MODULE;
	smi2021->i2c_adap.algo = &smi2021_i2c_algo;
	smi2021->i2c_adap.dev.parent = smi2021->dev;
	strscpy(smi2021->i2c_adap.name, KBUILD_MODNAME,
		sizeof(smi2021->i2c_adap.name));
	i2c_set_adapdata(&smi2021->i2c_adap, smi2021);

	rc = i2c_add_adapter(&smi2021->i2c_adap);
	if (rc)
		goto err_put;

	/* The saa7115 driver tells a saa7113 from its gm7113c clone itself */
	smi2021->decoder = v4l2_i2c_new_subdev_board(&smi2021->v4l2_dev,
						     &smi2021->i2c_adap,
						     &decoder_info, NULL);
	if (!smi2021->decoder) {
		rc = dev_err_probe(smi2021->dev, -ENODEV,
				   "failed to register the video decoder\n");
		goto err_put;
	}

	v4l2_subdev_call(smi2021->decoder, video, s_std, smi2021->cur_norm);
	v4l2_subdev_call(smi2021->decoder, video, s_routing,
			 smi2021->board->inputs[smi2021->cur_input].type, 0, 0);

	/*
	 * Audio arrives in the same URBs as the video, so set up the audio
	 * device before streaming can start.
	 */
	rc = smi2021_snd_register(smi2021);
	if (rc)
		dev_warn(smi2021->dev, "failed to register audio device: %pe\n",
			 ERR_PTR(rc));

	rc = smi2021_video_register(smi2021);
	if (rc) {
		rc = dev_err_probe(smi2021->dev, rc,
				   "failed to register video device\n");
		goto err_snd;
	}

	usb_set_intfdata(intf, smi2021);

	dev_info(smi2021->dev, "device %u, %s\n", smi2021->nr,
		 smi2021->board->name);

	return 0;

err_snd:
	smi2021_snd_unregister(smi2021);
err_put:
	v4l2_device_put(&smi2021->v4l2_dev);

	return rc;

err_ctrl:
	v4l2_ctrl_handler_free(&smi2021->ctrl_handler);
err_vb2:
	vb2_queue_release(&smi2021->vb_vidq);
err_free:
	mutex_destroy(&smi2021->reg_lock);
	mutex_destroy(&smi2021->v4l2_lock);
	mutex_destroy(&smi2021->vb_queue_lock);
	ida_free(&smi2021_ida, smi2021->nr);
	kfree(smi2021);

	return rc;
}

static void smi2021_usb_disconnect(struct usb_interface *intf)
{
	struct smi2021 *smi2021 = usb_get_intfdata(intf);

	/* Not set for a device in bootloader mode */
	if (!smi2021)
		return;

	/*
	 * Wait for running ioctls to finish. Once the video device is
	 * unregistered, no new ones can start.
	 */
	mutex_lock(&smi2021->vb_queue_lock);
	mutex_lock(&smi2021->v4l2_lock);

	smi2021_kill_isoc(smi2021);
	smi2021_clear_queue(smi2021, VB2_BUF_STATE_ERROR);
	video_unregister_device(&smi2021->vdev);
	v4l2_device_disconnect(&smi2021->v4l2_dev);

	/* No URB is running anymore, so no audio data can arrive either */
	smi2021_snd_unregister(smi2021);

	/* Tell smi2021_stop_streaming() that the device is gone */
	smi2021->udev = NULL;

	mutex_unlock(&smi2021->v4l2_lock);
	mutex_unlock(&smi2021->vb_queue_lock);

	/* This calls smi2021_release() unless the device is still in use */
	v4l2_device_put(&smi2021->v4l2_dev);
}

static struct usb_driver smi2021_usb_driver = {
	.name = KBUILD_MODNAME,
	.id_table = smi2021_usb_device_id_table,
	.probe = smi2021_usb_probe,
	.disconnect = smi2021_usb_disconnect,
};
module_usb_driver(smi2021_usb_driver);

MODULE_AUTHOR("Jon Arne Jørgensen <jonjon.arnearne@gmail.com>");
MODULE_AUTHOR("Martin Blumenstingl <martin.blumenstingl@googlemail.com>");
MODULE_DESCRIPTION("Somagic SMI2021 USB video capture driver");
MODULE_LICENSE("GPL");
