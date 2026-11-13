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

#include <linux/bits.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <media/v4l2-common.h>

#include "smi2021.h"

#define SMI2021_DRIVER_NAME		"smi2021"

#define SMI2021_USB_VENDOR_ID		0x1c88
#define SMI2021_USB_BOOTLOADER_ID	0x0007

/* Timeout for the vendor specific control requests, in milliseconds */
#define SMI2021_CTRL_TIMEOUT		1000

/* First byte of a mode control request */
#define SMI2021_MODE_CTRL_HEAD		0x01
#define SMI2021_MODE_CAPTURE		0x05
#define SMI2021_MODE_STANDBY		0x03

/* First byte of a register access request */
#define SMI2021_REG_CTRL_HEAD		0x0b

/*
 * Values for smi2021_reg_ctrl_transfer.data_cntl and .data_offset. These
 * select the kind of register access being requested. Only the combinations
 * below are known; the meaning of the individual bits is not.
 */
#define SMI2021_CNTL_SMI_WRITE		0x00
#define SMI2021_CNTL_SMI_READ		0x20
#define SMI2021_CNTL_I2C_PREPARE_READ	0x84
#define SMI2021_CNTL_I2C_READ		0xa0
#define SMI2021_CNTL_I2C_WRITE		0xc0

#define SMI2021_OFFSET_SMI		0x82
#define SMI2021_OFFSET_I2C		0x01

/*
 * Registers of the smi2021 bridge itself. The values written to them were
 * taken from the vendor driver and their meaning is not known.
 */
#define SMI2021_REG_AUDIO_EN		0x1740
#define SMI2021_REG_AUDIO_EN_ON		0x1d
#define SMI2021_REG_AUDIO_EN_OFF	0x00
#define SMI2021_REG_STREAM_EN		0x1800
#define SMI2021_REG_STREAM_EN_ON	0x0d

/* I2C address of the saa7113 compatible video decoder */
#define SMI2021_DECODER_I2C_ADDR	0x4a

/*
 * Decoder register 0x0e ("chroma control 1"). Setting CDTO resets the colour
 * subcarrier DTO at the start of every field.
 */
#define SAA7113_REG_CHROMA_CNTL_1	0x0e
#define SAA7113_CHROMA_CNTL_1_CDTO	BIT(7)

/* Magic words identifying the payload of an isochronous chunk */
#define SMI2021_MAGIC_VIDEO		0xaaaa0000
#define SMI2021_MAGIC_AUDIO		0xaaaa0001

/*
 * The smi2021 chip handles two different types of register settings:
 * settings for the video decoder, which are forwarded over i2c, and settings
 * for the smi2021 chip itself. Both are passed with the struct below; the
 * data_cntl and data_offset members tell the device which kind of setting it
 * is receiving and whether it is a read or a write request.
 */
struct smi2021_reg_ctrl_transfer {
	u8 head;
	u8 i2c_addr;
	u8 data_cntl;
	u8 data_offset;
	u8 data_size;
	union data {
		u8 val;
		struct i2c_data {
			u8 reg;
			u8 val;
		} __packed i2c_data;
		struct smi_data {
			__be16 reg;
			u8 val;
		} __packed smi_data;
		u8 reserved[8];
	} __packed data;
} __packed;

static int smi2021_set_mode(struct smi2021 *smi2021, u8 mode)
{
	struct smi2021_set_hw_state state = {
		.head = SMI2021_MODE_CTRL_HEAD,
		.state = mode,
	};

	if (!smi2021->udev)
		return -ENODEV;

	return usb_control_msg_send(smi2021->udev, 0, SMI2021_USB_REQUEST,
				    USB_DIR_OUT | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE,
				    state.head, SMI2021_USB_INDEX,
				    &state, sizeof(state),
				    SMI2021_CTRL_TIMEOUT, GFP_KERNEL);
}

static int smi2021_ctrl_send(struct smi2021 *smi2021,
			     const struct smi2021_reg_ctrl_transfer *msg)
{
	return usb_control_msg_send(smi2021->udev, 0, SMI2021_USB_REQUEST,
				    USB_DIR_OUT | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE,
				    msg->head, SMI2021_USB_INDEX,
				    msg, sizeof(*msg),
				    SMI2021_CTRL_TIMEOUT, GFP_KERNEL);
}

static int smi2021_get_reg(struct smi2021 *smi2021, u8 i2c_addr,
			   u16 reg, u8 *val)
{
	struct smi2021_reg_ctrl_transfer msg = {
		.head = SMI2021_REG_CTRL_HEAD,
		.data_size = sizeof(u8),
	};
	int rc;

	*val = 0;

	if (!smi2021->udev)
		return -ENODEV;

	if (i2c_addr) {
		msg.i2c_addr = i2c_addr;
		msg.data_cntl = SMI2021_CNTL_I2C_PREPARE_READ;
		msg.data.i2c_data.reg = reg;

		rc = smi2021_ctrl_send(smi2021, &msg);
		if (rc)
			return rc;

		msg.data_cntl = SMI2021_CNTL_I2C_READ;
	} else {
		msg.data_cntl = SMI2021_CNTL_SMI_READ;
		msg.data_offset = SMI2021_OFFSET_SMI;
		msg.data.smi_data.reg = cpu_to_be16(reg);
	}

	rc = smi2021_ctrl_send(smi2021, &msg);
	if (rc)
		return rc;

	rc = usb_control_msg_recv(smi2021->udev, 0, SMI2021_USB_REQUEST,
				  USB_DIR_IN | USB_TYPE_VENDOR |
				  USB_RECIP_DEVICE,
				  msg.head, SMI2021_USB_INDEX,
				  &msg, sizeof(msg),
				  SMI2021_CTRL_TIMEOUT, GFP_KERNEL);
	if (rc)
		return rc;

	*val = msg.data.val;

	return 0;
}

static int smi2021_set_reg(struct smi2021 *smi2021, u8 i2c_addr,
			   u16 reg, u8 val)
{
	struct smi2021_reg_ctrl_transfer msg = {
		.head = SMI2021_REG_CTRL_HEAD,
		.data_size = sizeof(u8),
	};

	if (!smi2021->udev)
		return -ENODEV;

	if (i2c_addr) {
		msg.i2c_addr = i2c_addr;
		msg.data_cntl = SMI2021_CNTL_I2C_WRITE;
		msg.data_offset = SMI2021_OFFSET_I2C;
		msg.data.i2c_data.reg = reg;
		msg.data.i2c_data.val = val;
	} else {
		msg.data_cntl = SMI2021_CNTL_SMI_WRITE;
		msg.data_offset = SMI2021_OFFSET_SMI;
		msg.data.smi_data.reg = cpu_to_be16(reg);
		msg.data.smi_data.val = val;
	}

	return smi2021_ctrl_send(smi2021, &msg);
}

static int smi2021_i2c_xfer(struct i2c_adapter *i2c_adap,
			    struct i2c_msg msgs[], int num)
{
	struct smi2021 *smi2021 = i2c_adap->algo_data;
	int rc;

	switch (num) {
	case 2: /* Read register */
		if (msgs[0].len != 1 || msgs[1].len != 1)
			return -EOPNOTSUPP;

		if (!(msgs[1].flags & I2C_M_RD))
			return -EOPNOTSUPP;

		rc = smi2021_get_reg(smi2021, msgs[0].addr, msgs[0].buf[0],
				     msgs[1].buf);
		break;
	case 1: /* Write register */
		if (msgs[0].len == 0)
			return num;

		if (msgs[0].len != 2)
			return -EOPNOTSUPP;

		rc = smi2021_set_reg(smi2021, msgs[0].addr, msgs[0].buf[0],
				     msgs[0].buf[1]);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return rc < 0 ? rc : num;
}

static u32 smi2021_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm smi2021_algo = {
	.master_xfer = smi2021_i2c_xfer,
	.functionality = smi2021_i2c_functionality,
};

static int smi2021_initialize(struct smi2021 *smi2021)
{
	/*
	 * These registers initialize the smi2021 chip. Their exact meaning is
	 * unknown; the sequence is taken from the vendor driver and is
	 * believed to toggle the reset pins of the audio ADC and the video
	 * decoder.
	 */
	static const u8 init[][2] = {
		{ 0x3a, 0x80 },
		{ 0x3b, 0x00 },
		{ 0x34, 0x01 },
		{ 0x35, 0x00 },
		{ 0x34, 0x11 },
		{ 0x35, 0x11 },
		{ 0x3b, 0x80 },
		{ 0x3b, 0x00 },
	};
	unsigned int i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(init); i++) {
		rc = smi2021_set_reg(smi2021, 0, init[i][0], init[i][1]);
		if (rc < 0)
			return rc;
	}

	return 0;
}

static struct smi2021_buf *smi2021_get_buf(struct smi2021 *smi2021)
{
	struct smi2021_buf *buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&smi2021->buf_lock, flags);
	if (!list_empty(&smi2021->avail_bufs)) {
		buf = list_first_entry(&smi2021->avail_bufs,
				       struct smi2021_buf, list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&smi2021->buf_lock, flags);

	return buf;
}

static void smi2021_buf_done(struct smi2021 *smi2021)
{
	unsigned int lines_per_field = smi2021->cur_height / 2;
	struct smi2021_buf *buf = smi2021->cur_buf;

	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.sequence = smi2021->sequence++;
	buf->vb.field = V4L2_FIELD_INTERLACED;

	/*
	 * buf->pos holds the number of active video bytes received for the
	 * field that just ended. If the device did not send a full field the
	 * frame is incomplete, so hand it back with an error.
	 */
	if (buf->pos < SMI2021_BYTES_PER_LINE * lines_per_field) {
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0, 0);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	} else {
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0,
				      SMI2021_BYTES_PER_LINE *
				      smi2021->cur_height);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}

	smi2021->cur_buf = NULL;
}

static bool is_sav(u8 trc)
{
	return !(trc & SMI2021_TRC_EAV);
}

static bool is_active_video(u8 trc)
{
	return !(trc & SMI2021_TRC_VBI);
}

static bool is_field2(u8 trc)
{
	return trc & SMI2021_TRC_FIELD_2;
}

/*
 * Parse a timing reference code.
 *
 * Grab a new buffer from the queue when the device starts sending a new
 * frame, and hand the buffer back once both of its fields are complete.
 */
static void parse_trc(struct smi2021 *smi2021, u8 trc)
{
	unsigned int lines_per_field = smi2021->cur_height / 2;
	struct smi2021_buf *buf = smi2021->cur_buf;

	if (!buf) {
		/*
		 * Only start filling a buffer at the start of the first
		 * field, every other starting point would yield an
		 * incomplete frame.
		 */
		if (!is_sav(trc) || !is_active_video(trc) || is_field2(trc))
			return;

		buf = smi2021_get_buf(smi2021);
		if (!buf)
			return;

		smi2021->cur_buf = buf;
	}

	if (!is_sav(trc)) {
		/* End of VBI or of active video */
		buf->in_blank = true;
		return;
	}

	/* Start of VBI or of active video */
	buf->in_blank = !is_active_video(trc);

	if (!buf->second_field && is_field2(trc)) {
		if (buf->pos < SMI2021_BYTES_PER_LINE * lines_per_field) {
			/* The first field was short, drop the frame */
			smi2021_buf_done(smi2021);
			return;
		}

		buf->second_field = true;
		buf->pos = 0;
		return;
	}

	if (buf->second_field && !is_field2(trc))
		smi2021_buf_done(smi2021);
}

/*
 * Copy a run of active video data into the current buffer.
 *
 * The device sends the two fields of a frame one after the other while the
 * buffer holds them interleaved, so line n of a field is written to line
 * 2n (first field) or 2n + 1 (second field) of the buffer.
 */
static void copy_video(struct smi2021 *smi2021, const u8 *src,
		       unsigned int len)
{
	struct smi2021_buf *buf = smi2021->cur_buf;

	if (!buf || buf->in_blank)
		return;

	while (len) {
		unsigned int pos_in_line = buf->pos % SMI2021_BYTES_PER_LINE;
		unsigned int line = buf->pos / SMI2021_BYTES_PER_LINE;
		unsigned int offset, chunk;

		offset = (line * 2 + buf->second_field) *
			 SMI2021_BYTES_PER_LINE + pos_in_line;
		if (offset >= buf->length)
			return;

		/*
		 * Copy at most up to the end of the current line, so that a
		 * run spanning a line boundary does not spill into the line
		 * of the other field that follows it in the buffer.
		 */
		chunk = min(len, SMI2021_BYTES_PER_LINE - pos_in_line);
		chunk = min(chunk, buf->length - offset);

		memcpy(buf->mem + offset, src, chunk);

		buf->pos += chunk;
		src += chunk;
		len -= chunk;
	}
}

/*
 * Scan the video data coming from the decoder.
 *
 * Active video is framed by timing reference codes:
 *	0xff 0x00 0x00 <TRC>
 * where TRC is described in the saa7113 datasheet. Everything in between is
 * video data. A timing reference code may be split across two chunks, so the
 * scanner state is kept in smi2021->sync_state.
 */
static void parse_video(struct smi2021 *smi2021, const u8 *p,
			unsigned int size)
{
	static const u8 trc_prefix[] = { 0xff, 0x00, 0x00 };
	unsigned int i = 0;

	while (i < size) {
		unsigned int consumed, start;
		u8 byte;

		if (smi2021->sync_state == HSYNC) {
			/* Copy video data up to the next potential TRC */
			start = i;
			while (i < size && p[i] != 0xff)
				i++;

			copy_video(smi2021, p + start, i - start);

			if (i < size) {
				i++;
				smi2021->sync_state = SYNCZ1;
			}
			continue;
		}

		byte = p[i++];

		switch (smi2021->sync_state) {
		case SYNCZ1:
			if (byte == 0x00) {
				smi2021->sync_state = SYNCZ2;
				continue;
			}
			consumed = 1;
			break;
		case SYNCZ2:
			if (byte == 0x00) {
				smi2021->sync_state = TRC;
				continue;
			}
			consumed = 2;
			break;
		default: /* TRC */
			smi2021->sync_state = HSYNC;
			parse_trc(smi2021, byte);
			continue;
		}

		/*
		 * This was not a timing reference code after all, so the
		 * prefix bytes already consumed are video data. Emit them and
		 * rescan the current byte, which may start a new code.
		 */
		smi2021->sync_state = HSYNC;
		copy_video(smi2021, trc_prefix, consumed);
		i--;
	}
}

/*
 * The device delivers data in chunks of SMI2021_CHUNK_SIZE bytes, each
 * starting with a magic word identifying its payload:
 *	0xaa 0xaa 0x00 0x00 = video data from the decoder
 *	0xaa 0xaa 0x00 0x01 = 24 bit 2 channel PCM audio data
 */
static void process_packet(struct smi2021 *smi2021, u8 *p, unsigned int size)
{
	unsigned int i;

	if (size % SMI2021_CHUNK_SIZE) {
		dev_dbg_ratelimited(smi2021->dev,
				    "unexpected packet size %u\n", size);
		return;
	}

	for (i = 0; i < size; i += SMI2021_CHUNK_SIZE) {
		u8 *chunk = p + i;

		switch (get_unaligned_be32(chunk)) {
		case SMI2021_MAGIC_VIDEO:
			parse_video(smi2021, chunk + 4,
				    SMI2021_CHUNK_SIZE - 4);
			break;
		case SMI2021_MAGIC_AUDIO:
			smi2021_audio(smi2021, chunk + 4,
				      SMI2021_CHUNK_SIZE - 4);
			break;
		}
	}
}

static void smi2021_iso_cb(struct urb *urb)
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
		/* The URB was killed or the device is gone, do not resubmit */
		return;
	default:
		dev_warn_ratelimited(smi2021->dev, "urb error %d\n",
				     urb->status);
		break;
	}

	for (i = 0; i < urb->number_of_packets; i++) {
		int size = urb->iso_frame_desc[i].actual_length;
		u8 *data = urb->transfer_buffer +
			   urb->iso_frame_desc[i].offset;

		if (urb->iso_frame_desc[i].status == 0 && size)
			process_packet(smi2021, data, size);

		urb->iso_frame_desc[i].status = 0;
		urb->iso_frame_desc[i].actual_length = 0;
	}

	urb->status = 0;
	rc = usb_submit_urb(urb, GFP_ATOMIC);
	if (rc)
		dev_warn_ratelimited(smi2021->dev,
				     "urb re-submit failed (%d)\n", rc);
}

static void smi2021_cancel_isoc(struct smi2021 *smi2021)
{
	unsigned int i;

	for (i = 0; i < smi2021->num_urbs; i++)
		usb_kill_urb(smi2021->urbs[i]);
}

/*
 * Release all URBs and transfer buffers.
 * All URBs must have been killed before calling this.
 */
static void smi2021_free_isoc(struct smi2021 *smi2021)
{
	unsigned int i;

	for (i = 0; i < smi2021->num_urbs; i++) {
		struct urb *urb = smi2021->urbs[i];

		if (!urb)
			continue;

		kfree(urb->transfer_buffer);
		usb_free_urb(urb);
		smi2021->urbs[i] = NULL;
	}

	kfree(smi2021->urbs);
	smi2021->urbs = NULL;
	smi2021->num_urbs = 0;
}

static void smi2021_uninit_isoc(struct smi2021 *smi2021)
{
	smi2021_cancel_isoc(smi2021);
	smi2021_free_isoc(smi2021);
}

static int smi2021_alloc_isoc(struct smi2021 *smi2021)
{
	unsigned int i, j, sb_size;

	/*
	 * It may be necessary to release the URBs here, since they are
	 * otherwise only released on disconnect.
	 */
	if (smi2021->num_urbs)
		smi2021_uninit_isoc(smi2021);

	sb_size = SMI2021_ISOC_PACKETS * smi2021->iso_size;

	smi2021->urbs = kcalloc(SMI2021_ISOC_TRANSFERS,
				sizeof(*smi2021->urbs), GFP_KERNEL);
	if (!smi2021->urbs)
		return -ENOMEM;

	for (i = 0; i < SMI2021_ISOC_TRANSFERS; i++) {
		struct urb *urb;

		urb = usb_alloc_urb(SMI2021_ISOC_PACKETS, GFP_KERNEL);
		if (!urb)
			goto err_free;

		smi2021->urbs[i] = urb;
		smi2021->num_urbs = i + 1;

		urb->transfer_buffer = kzalloc(sb_size, GFP_KERNEL);
		if (!urb->transfer_buffer)
			goto err_free;

		urb->dev = smi2021->udev;
		urb->pipe = usb_rcvisocpipe(smi2021->udev, smi2021->isoc_ep);
		urb->transfer_buffer_length = sb_size;
		urb->complete = smi2021_iso_cb;
		urb->context = smi2021;
		urb->interval = 1;
		urb->start_frame = 0;
		urb->number_of_packets = SMI2021_ISOC_PACKETS;
		urb->transfer_flags = URB_ISO_ASAP;

		for (j = 0; j < SMI2021_ISOC_PACKETS; j++) {
			urb->iso_frame_desc[j].offset = j * smi2021->iso_size;
			urb->iso_frame_desc[j].length = smi2021->iso_size;
		}
	}

	dev_dbg(smi2021->dev, "allocated %u urbs of %u bytes\n",
		smi2021->num_urbs, sb_size);

	return 0;

err_free:
	smi2021_free_isoc(smi2021);

	return -ENOMEM;
}

void smi2021_toggle_audio(struct smi2021 *smi2021, bool enable)
{
	/*
	 * This register enables and disables the transfer of audio data over
	 * USB. The meaning of the value 0x1d is unknown.
	 */
	smi2021_set_reg(smi2021, 0, SMI2021_REG_AUDIO_EN,
			enable ? SMI2021_REG_AUDIO_EN_ON :
				 SMI2021_REG_AUDIO_EN_OFF);
}

int smi2021_start(struct smi2021 *smi2021)
{
	unsigned int i;
	int rc;
	u8 reg;

	if (!smi2021->udev)
		return -ENODEV;

	if (mutex_lock_interruptible(&smi2021->v4l2_lock))
		return -ERESTARTSYS;

	smi2021->sequence = 0;
	smi2021->sync_state = HSYNC;
	smi2021->cur_buf = NULL;

	v4l2_subdev_call(smi2021->decoder, video, s_stream, 1);

	/*
	 * Set CDTO (bit 7 of the decoder's first chroma control register),
	 * which resets the colour subcarrier DTO at the start of every field.
	 * The vendor driver writes this six times; it is not known whether
	 * repeating the write is actually required.
	 */
	rc = smi2021_get_reg(smi2021, SMI2021_DECODER_I2C_ADDR,
			     SAA7113_REG_CHROMA_CNTL_1, &reg);
	if (rc < 0)
		goto err_stop_subdev;

	reg |= SAA7113_CHROMA_CNTL_1_CDTO;

	for (i = 0; i < 6; i++) {
		rc = smi2021_set_reg(smi2021, SMI2021_DECODER_I2C_ADDR,
				     SAA7113_REG_CHROMA_CNTL_1, reg);
		if (rc < 0)
			goto err_stop_subdev;
	}

	rc = smi2021_set_mode(smi2021, SMI2021_MODE_CAPTURE);
	if (rc < 0)
		goto err_stop_subdev;

	rc = usb_set_interface(smi2021->udev, 0, 2);
	if (rc < 0)
		goto err_stop_hw;

	/* Only let audio data flow while the PCM device is capturing */
	smi2021_toggle_audio(smi2021,
			     atomic_read(&smi2021->adev_capturing) != 0);

	if (!smi2021->num_urbs) {
		rc = smi2021_alloc_isoc(smi2021);
		if (rc < 0)
			goto err_stop_hw;
	}

	for (i = 0; i < smi2021->num_urbs; i++) {
		rc = usb_submit_urb(smi2021->urbs[i], GFP_KERNEL);
		if (rc) {
			dev_err(smi2021->dev, "cannot submit urb %u (%d)\n",
				i, rc);
			goto err_uninit;
		}
	}

	/* Start the isochronous stream */
	rc = smi2021_set_reg(smi2021, 0, SMI2021_REG_STREAM_EN,
			     SMI2021_REG_STREAM_EN_ON);
	if (rc < 0)
		goto err_uninit;

	mutex_unlock(&smi2021->v4l2_lock);

	return 0;

err_uninit:
	smi2021_uninit_isoc(smi2021);
err_stop_hw:
	usb_set_interface(smi2021->udev, 0, 0);
	smi2021_set_mode(smi2021, SMI2021_MODE_STANDBY);
err_stop_subdev:
	v4l2_subdev_call(smi2021->decoder, video, s_stream, 0);

	/*
	 * start_streaming() failed, so the buffers that were queued before
	 * it was called have to go back to the queued state.
	 */
	smi2021_clear_queue(smi2021, VB2_BUF_STATE_QUEUED);

	mutex_unlock(&smi2021->v4l2_lock);

	return rc;
}

void smi2021_stop(struct smi2021 *smi2021)
{
	mutex_lock(&smi2021->v4l2_lock);

	smi2021_cancel_isoc(smi2021);

	if (smi2021->udev) {
		v4l2_subdev_call(smi2021->decoder, video, s_stream, 0);
		smi2021_set_mode(smi2021, SMI2021_MODE_STANDBY);
		usb_set_interface(smi2021->udev, 0, 0);
	}

	smi2021_clear_queue(smi2021, VB2_BUF_STATE_ERROR);

	mutex_unlock(&smi2021->v4l2_lock);
}

static void smi2021_release(struct v4l2_device *v4l2_dev)
{
	struct smi2021 *smi2021 = container_of(v4l2_dev, struct smi2021,
					       v4l2_dev);

	/* Unregister the decoder before the i2c adapter it lives on */
	v4l2_device_unregister(&smi2021->v4l2_dev);
	i2c_del_adapter(&smi2021->i2c_adap);

	v4l2_ctrl_handler_free(&smi2021->ctrl_handler);
	vb2_queue_release(&smi2021->vb_vidq);

	mutex_destroy(&smi2021->v4l2_lock);
	mutex_destroy(&smi2021->vb_queue_lock);

	kfree(smi2021);
}

/*
 * DEVICE - PROBE & DISCONNECT
 */

static const struct usb_device_id smi2021_usb_device_id_table[] = {
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, SMI2021_USB_BOOTLOADER_ID) },
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, 0x003c) },
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, 0x003d) },
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, 0x003e) },
	{ USB_DEVICE(SMI2021_USB_VENDOR_ID, 0x003f) },
	{ }
};
MODULE_DEVICE_TABLE(usb, smi2021_usb_device_id_table);

/* EasyCAP DC60 and compatibles */
static const struct smi2021_vid_input dual_input[] = {
	{
		.name = "Composite",
		.type = SAA7115_COMPOSITE0,
	},
	{
		.name = "S-Video",
		.type = SAA7115_SVIDEO1,
	},
};

/* EasyCAP 002 and compatibles */
static const struct smi2021_vid_input quad_input[] = {
	{
		.name = "Composite 0",
		.type = SAA7115_COMPOSITE0,
	},
	{
		.name = "Composite 1",
		.type = SAA7115_COMPOSITE1,
	},
	{
		.name = "Composite 2",
		.type = SAA7115_COMPOSITE2,
	},
	{
		.name = "Composite 3",
		.type = SAA7115_COMPOSITE3,
	},
};

static int smi2021_usb_probe(struct usb_interface *intf,
			     const struct usb_device_id *devid)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	const struct usb_endpoint_descriptor *ep;
	struct i2c_board_info decoder_info = { };
	struct device *dev = &intf->dev;
	struct smi2021 *smi2021;
	int rc;

	if (le16_to_cpu(udev->descriptor.idProduct) ==
	    SMI2021_USB_BOOTLOADER_ID)
		return smi2021_bootloader_probe(intf, devid);

	if (intf->num_altsetting != 3)
		return -ENODEV;

	if (intf->altsetting[2].desc.bNumEndpoints != 1)
		return -ENODEV;

	ep = &intf->altsetting[2].endpoint[0].desc;
	if (!usb_endpoint_is_isoc_in(ep))
		return -ENODEV;

	smi2021 = kzalloc_obj(*smi2021);
	if (!smi2021)
		return -ENOMEM;

	smi2021->dev = dev;
	smi2021->udev = udev;
	smi2021->isoc_ep = usb_endpoint_num(ep);

	/*
	 * High bandwidth isochronous endpoints can transfer more than one
	 * packet per microframe, so the number of additional transaction
	 * opportunities has to be taken into account. Without this the
	 * driver would ask for a third of the data the device sends.
	 */
	smi2021->iso_size = usb_endpoint_maxp(ep) * usb_endpoint_maxp_mult(ep);

	switch (le16_to_cpu(udev->descriptor.idProduct)) {
	case 0x003e:
	case 0x003f:
		smi2021->vid_inputs = quad_input;
		smi2021->vid_input_count = ARRAY_SIZE(quad_input);
		break;
	default:
		smi2021->vid_inputs = dual_input;
		smi2021->vid_input_count = ARRAY_SIZE(dual_input);
		break;
	}

	spin_lock_init(&smi2021->buf_lock);
	mutex_init(&smi2021->v4l2_lock);
	mutex_init(&smi2021->vb_queue_lock);
	INIT_LIST_HEAD(&smi2021->avail_bufs);

	/* NTSC is the default */
	smi2021->cur_norm = V4L2_STD_NTSC;
	smi2021->cur_height = SMI2021_NTSC_LINES;

	rc = smi2021_vb2_setup(smi2021);
	if (rc < 0) {
		dev_err(dev, "could not initialize videobuf2 queue\n");
		goto err_free;
	}

	rc = v4l2_ctrl_handler_init(&smi2021->ctrl_handler, 0);
	if (rc < 0) {
		dev_err(dev, "could not initialize v4l2 ctrl handler\n");
		goto err_vb2;
	}

	smi2021->v4l2_dev.release = smi2021_release;
	smi2021->v4l2_dev.ctrl_handler = &smi2021->ctrl_handler;
	rc = v4l2_device_register(dev, &smi2021->v4l2_dev);
	if (rc < 0) {
		dev_err(dev, "could not register v4l2 device\n");
		goto err_ctrl;
	}

	/* From here on smi2021_release() tears everything down */

	rc = smi2021_initialize(smi2021);
	if (rc < 0) {
		dev_err(dev, "could not initialize device\n");
		goto err_v4l2;
	}

	smi2021->i2c_adap.owner = THIS_MODULE;
	smi2021->i2c_adap.algo = &smi2021_algo;
	smi2021->i2c_adap.algo_data = smi2021;
	smi2021->i2c_adap.dev.parent = dev;
	strscpy(smi2021->i2c_adap.name, SMI2021_DRIVER_NAME,
		sizeof(smi2021->i2c_adap.name));
	i2c_set_adapdata(&smi2021->i2c_adap, &smi2021->v4l2_dev);

	rc = i2c_add_adapter(&smi2021->i2c_adap);
	if (rc < 0) {
		dev_err(dev, "could not add i2c adapter\n");
		goto err_v4l2;
	}

	/*
	 * These devices ship with either a saa7113 or its gm7113c clone, so
	 * let the decoder driver identify the chip itself. The overrides
	 * below describe what the smi2021 bridge expects from the decoder's
	 * BT.656 output: the field flag in the timing reference codes has to
	 * be derived from VREF rather than from the data type.
	 */
	smi2021->decoder_r10_ofts = SAA7113_OFTS_VFLAG_BY_VREF;
	smi2021->decoder_r10_vrln = true;
	smi2021->decoder_r13_adlsb = true;
	smi2021->decoder_pdata.saa7113_r10_ofts = &smi2021->decoder_r10_ofts;
	smi2021->decoder_pdata.saa7113_r10_vrln = &smi2021->decoder_r10_vrln;
	smi2021->decoder_pdata.saa7113_r13_adlsb = &smi2021->decoder_r13_adlsb;

	decoder_info.addr = SMI2021_DECODER_I2C_ADDR;
	decoder_info.platform_data = &smi2021->decoder_pdata;
	strscpy(decoder_info.type, "saa7115_auto", sizeof(decoder_info.type));

	smi2021->decoder = v4l2_i2c_new_subdev_board(&smi2021->v4l2_dev,
						     &smi2021->i2c_adap,
						     &decoder_info, NULL);
	if (!smi2021->decoder) {
		dev_err(dev, "could not register the video decoder\n");
		rc = -ENODEV;
		goto err_v4l2;
	}

	v4l2_subdev_call(smi2021->decoder, video, s_std, smi2021->cur_norm);
	v4l2_subdev_call(smi2021->decoder, video, s_routing,
			 smi2021->vid_inputs[smi2021->cur_input].type, 0, 0);

	usb_set_intfdata(intf, smi2021);

	rc = smi2021_video_register(smi2021);
	if (rc < 0) {
		dev_err(dev, "could not register video device\n");
		goto err_intfdata;
	}

	rc = smi2021_snd_register(smi2021);
	if (rc < 0)
		dev_warn(dev, "could not register audio device (%d)\n", rc);

	return 0;

err_intfdata:
	usb_set_intfdata(intf, NULL);
err_v4l2:
	v4l2_device_put(&smi2021->v4l2_dev);

	return rc;

err_ctrl:
	v4l2_ctrl_handler_free(&smi2021->ctrl_handler);
err_vb2:
	vb2_queue_release(&smi2021->vb_vidq);
err_free:
	mutex_destroy(&smi2021->v4l2_lock);
	mutex_destroy(&smi2021->vb_queue_lock);
	kfree(smi2021);

	return rc;
}

static void smi2021_usb_disconnect(struct usb_interface *intf)
{
	struct smi2021 *smi2021 = usb_get_intfdata(intf);

	if (!smi2021)
		return;

	usb_set_intfdata(intf, NULL);

	/*
	 * Wait until all current v4l2 operations are finished before
	 * tearing the hardware state down.
	 */
	mutex_lock(&smi2021->vb_queue_lock);
	mutex_lock(&smi2021->v4l2_lock);

	/* This is the only place where the URBs are released */
	smi2021_uninit_isoc(smi2021);
	smi2021_clear_queue(smi2021, VB2_BUF_STATE_ERROR);

	video_unregister_device(&smi2021->vdev);
	v4l2_device_disconnect(&smi2021->v4l2_dev);

	/* This way current users can detect that the device is gone */
	smi2021->udev = NULL;

	mutex_unlock(&smi2021->v4l2_lock);
	mutex_unlock(&smi2021->vb_queue_lock);

	smi2021_snd_unregister(smi2021);

	/*
	 * This calls smi2021_release() if it is the last reference,
	 * otherwise the release is postponed until there are no users left.
	 */
	v4l2_device_put(&smi2021->v4l2_dev);
}

static struct usb_driver smi2021_usb_driver = {
	.name = SMI2021_DRIVER_NAME,
	.id_table = smi2021_usb_device_id_table,
	.probe = smi2021_usb_probe,
	.disconnect = smi2021_usb_disconnect,
};

module_usb_driver(smi2021_usb_driver);

MODULE_AUTHOR("Jon Arne Jørgensen <jonjon.arnearne@gmail.com>");
MODULE_AUTHOR("Martin Blumenstingl <martin.blumenstingl@googlemail.com>");
MODULE_DESCRIPTION("Somagic SMI2021 USB video capture driver");
MODULE_LICENSE("GPL");
