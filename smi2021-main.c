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
#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "smi2021.h"

#define SMI2021_USB_VENDOR_ID		0x1c88
/* All devices enumerate with this id until their firmware is loaded */
#define SMI2021_USB_BOOTLOADER_ID	0x0007

/* Vendor specific control request used for all register access */
#define SMI2021_USB_REQUEST		0x01
#define SMI2021_USB_INDEX		0x00

/*
 * Alternate settings without and with the isochronous endpoint. The capture
 * mode frames the stream in chunks of SMI2021_CHUNK_SIZE bytes, which only
 * fit whole into the packets of alternate setting 2. The audio only mode
 * sends a few frames of audio per packet, with no chunks, which alternate
 * setting 2 carries as well; the vendor driver picks alternate setting 1 for
 * it.
 */
#define SMI2021_ALT_IDLE		0
#define SMI2021_ALT_STREAM		2
#define SMI2021_ISOC_PACKETS		10

/*
 * Modes set with a hardware state request. The firmware starts in standby and
 * also knows the mode 0x04, whose purpose is unknown.
 */
#define SMI2021_MODE_AUDIO		0x02
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
#define SMI2021_I2C_CMD_WRITE_REG	0xc0	/* write subaddress and data */
#define SMI2021_I2C_CMD_READ_REG	0xe0	/* write subaddress, read data */
/*
 * The firmware retries a failed transfer 16 times. If it still fails, this
 * flag is set in the reply, so a failure is only reported for reads.
 */
#define SMI2021_I2C_CMD_NAK		BIT(0)
/*
 * Even so, about one read in five fails, independently of the ones before
 * it, so a read is tried again a few times. Writes have not been seen to
 * fail.
 */
#define SMI2021_I2C_READ_TRIES		8

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

/*
 * Video engine control. The firmware sets 0x0d itself when streaming starts in
 * capture mode; writing 0x00 stops the engine again.
 */
#define SMI2021_REG_VIDEO_CTRL		0x1800
#define SMI2021_REG_VIDEO_CTRL_OFF	0x00

/* Magic words that identify the payload of an isochronous chunk */
#define SMI2021_CHUNK_MAGIC_VIDEO	0xaaaa0000
#define SMI2021_CHUNK_MAGIC_AUDIO	0xaaaa0001

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
	lockdep_assert_held(&smi2021->reg_lock);

	/*
	 * The i2c adapter stays registered after a disconnect until the driver
	 * state is released, so an access can still come in through i2c-dev.
	 */
	if (!smi2021->udev)
		return -ENODEV;

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

/*
 * Read a register with one transfer that writes the subaddress and then reads
 * with a repeated start. Setting the subaddress with a write of its own and
 * then reading, as the vendor driver does, returns the register that an
 * earlier transfer left the subaddress at about a quarter of the time, and
 * nothing reports that.
 */
static int smi2021_i2c_read(struct smi2021 *smi2021, u8 addr, u8 reg, u8 *val)
{
	const struct smi2021_reg_msg req = {
		.head = SMI2021_REG_CTRL_HEAD,
		.i2c_addr = addr << 1,
		.cntl = SMI2021_I2C_CMD_READ_REG,
		.flags = sizeof(req.data.i2c.reg),
		.size = sizeof(req.data.i2c.val),
		.data.i2c.reg = reg,
	};
	struct smi2021_reg_msg reply;
	unsigned int i;
	int rc;

	guard(mutex)(&smi2021->reg_lock);

	for (i = 0; i < SMI2021_I2C_READ_TRIES; i++) {
		rc = smi2021_send_reg_msg(smi2021, &req);
		if (rc)
			return rc;

		rc = smi2021_ctrl_read(smi2021->udev, req.head, &reply,
				       sizeof(reply));
		if (rc)
			return rc;

		if (!(reply.cntl & SMI2021_I2C_CMD_NAK)) {
			*val = reply.data.i2c.val;
			return 0;
		}
	}

	return -EIO;
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
	 * Set the GPIO lines as the vendor driver does, with its delays. On an
	 * EasyCAP 002, PA7 high holds the video decoder and the audio ADC in
	 * reset, so this pulses their reset for 100 ms and gives the decoder
	 * 10 ms to come out of it before the first i2c access. Neither
	 * datasheet gives the time they need; it is presumably much shorter,
	 * but this is the timing the boards were made for. Where PC0 and PC4
	 * go is not known.
	 */
	static const struct {
		u16 reg;
		u8 val;
		unsigned int delay_us;
	} init[] = {
		{ SMI2021_REG_DDRA, BIT(7) },
		{ SMI2021_REG_PORTA, 0 },
		{ SMI2021_REG_DDRC, BIT(0) },
		{ SMI2021_REG_PORTC, 0 },
		{ SMI2021_REG_DDRC, BIT(4) | BIT(0) },
		{ SMI2021_REG_PORTC, BIT(4) | BIT(0), 10 * USEC_PER_MSEC },
		{ SMI2021_REG_PORTA, BIT(7), 100 * USEC_PER_MSEC },
		{ SMI2021_REG_PORTA, 0, 10 * USEC_PER_MSEC },
	};
	unsigned int i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(init); i++) {
		rc = smi2021_write_reg(smi2021, init[i].reg, init[i].val);
		if (rc)
			return rc;
		if (init[i].delay_us)
			fsleep(init[i].delay_us);
	}

	return 0;
}

static void smi2021_process_packet(struct smi2021 *smi2021, const u8 *p,
				   unsigned int size)
{
	unsigned int i;

	/*
	 * The audio only mode sends the audio data as it is, the same stream
	 * that the capture mode splits up into audio chunks.
	 */
	if (smi2021->stream_mode == SMI2021_MODE_AUDIO) {
		smi2021_parse_audio(smi2021, p, size);
		return;
	}

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
			smi2021_parse_audio(smi2021, data,
					    SMI2021_CHUNK_DATA_SIZE);
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
	switch (rc) {
	case 0:
	/*
	 * The stream is stopping, and usb_kill_urb() refuses the resubmission
	 * of a URB that completed just before it.
	 */
	case -EPERM:
	/* The device is gone */
	case -ENODEV:
	case -ESHUTDOWN:
		break;
	default:
		dev_warn_ratelimited(smi2021->dev,
				     "urb resubmit failed: %d\n", rc);
		break;
	}
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

static void smi2021_toggle_audio(struct smi2021 *smi2021, bool enable)
{
	smi2021_write_reg(smi2021, SMI2021_REG_AUDIO_CTRL,
			  enable ? SMI2021_REG_AUDIO_CTRL_ON :
				   SMI2021_REG_AUDIO_CTRL_OFF);
}

/*
 * Neither the standby mode nor alternate setting 0 stops the engines that a
 * capture mode started, so clear their control registers the way the firmware
 * does when it enters the mode.
 */
static void smi2021_stop_engines(struct smi2021 *smi2021)
{
	smi2021_write_reg(smi2021, SMI2021_REG_VIDEO_CTRL,
			  SMI2021_REG_VIDEO_CTRL_OFF);
	smi2021_toggle_audio(smi2021, false);
}

static void smi2021_stop_stream(struct smi2021 *smi2021)
{
	u8 mode = smi2021->stream_mode;

	lockdep_assert_held(&smi2021->stream_lock);

	if (mode == SMI2021_MODE_STANDBY)
		return;

	smi2021_kill_isoc(smi2021);
	smi2021->stream_mode = SMI2021_MODE_STANDBY;

	if (mode == SMI2021_MODE_CAPTURE)
		v4l2_subdev_call(smi2021->decoder, video, s_stream, 0);

	smi2021_stop_engines(smi2021);
	smi2021_set_mode(smi2021, SMI2021_MODE_STANDBY);
	usb_set_interface(smi2021->udev, smi2021->ifnum, SMI2021_ALT_IDLE);

	smi2021_free_isoc(smi2021);
}

static int smi2021_start_stream(struct smi2021 *smi2021, u8 mode)
{
	unsigned int i;
	int rc;

	lockdep_assert_held(&smi2021->stream_lock);

	rc = smi2021_alloc_isoc(smi2021);
	if (rc)
		return rc;

	if (mode == SMI2021_MODE_CAPTURE) {
		/* The transfers are stopped, so the parser state is ours */
		smi2021->sequence = 0;
		smi2021->trc_prefix_len = 0;
		smi2021->last_field2 = false;

		v4l2_subdev_call(smi2021->decoder, video, s_stream, 1);
	}

	rc = smi2021_set_mode(smi2021, mode);
	if (rc)
		goto err_stop_decoder;

	/* This also makes the firmware start the engines of the mode */
	rc = usb_set_interface(smi2021->udev, smi2021->ifnum,
			       SMI2021_ALT_STREAM);
	if (rc)
		goto err_standby;

	smi2021->stream_mode = mode;

	/* Only let audio data flow while the pcm device is capturing */
	smi2021_toggle_audio(smi2021, smi2021->audio_wanted);

	for (i = 0; i < smi2021->num_urbs; i++) {
		rc = usb_submit_urb(smi2021->urbs[i], GFP_KERNEL);
		if (rc) {
			dev_err(smi2021->dev, "cannot submit urb %u: %d\n",
				i, rc);
			smi2021_stop_stream(smi2021);
			return rc;
		}
	}

	return 0;

err_standby:
	smi2021_set_mode(smi2021, SMI2021_MODE_STANDBY);
err_stop_decoder:
	if (mode == SMI2021_MODE_CAPTURE)
		v4l2_subdev_call(smi2021->decoder, video, s_stream, 0);
	smi2021_free_isoc(smi2021);

	return rc;
}

/*
 * Bring the stream in line with what the video device and the pcm device
 * want. The capture mode carries the audio data as well, so audio only needs
 * a mode of its own while no video capture is running.
 */
static int smi2021_set_stream(struct smi2021 *smi2021)
{
	u8 want = SMI2021_MODE_STANDBY;

	lockdep_assert_held(&smi2021->stream_lock);

	/* Nothing can be started once the device is gone */
	if (smi2021->udev) {
		if (smi2021->video_wanted)
			want = SMI2021_MODE_CAPTURE;
		else if (smi2021->audio_wanted)
			want = SMI2021_MODE_AUDIO;
	}

	if (want == smi2021->stream_mode) {
		/*
		 * A video capture keeps streaming when the pcm device starts
		 * or stops, only the audio engine has to follow it.
		 */
		if (want != SMI2021_MODE_STANDBY)
			smi2021_toggle_audio(smi2021, smi2021->audio_wanted);

		return 0;
	}

	smi2021_stop_stream(smi2021);

	if (want == SMI2021_MODE_STANDBY)
		return 0;

	return smi2021_start_stream(smi2021, want);
}

/* Start or stop the transfer of audio data, see smi2021_set_stream() */
int smi2021_request_audio(struct smi2021 *smi2021, bool enable)
{
	guard(mutex)(&smi2021->stream_lock);

	smi2021->audio_wanted = enable;

	return smi2021_set_stream(smi2021);
}

int smi2021_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct smi2021 *smi2021 = vb2_get_drv_priv(vq);
	int rc;

	guard(mutex)(&smi2021->stream_lock);

	smi2021->video_wanted = true;

	rc = smi2021_set_stream(smi2021);
	if (rc) {
		smi2021->video_wanted = false;

		/* The transfers are stopped, so the buffers are ours again */
		smi2021_clear_queue(smi2021, VB2_BUF_STATE_QUEUED);

		/* Go back to streaming audio alone, if the pcm device wants it */
		smi2021_set_stream(smi2021);
	}

	return rc;
}

void smi2021_stop_streaming(struct vb2_queue *vq)
{
	struct smi2021 *smi2021 = vb2_get_drv_priv(vq);

	guard(mutex)(&smi2021->stream_lock);

	smi2021->video_wanted = false;

	/*
	 * Stop the transfers before handing the buffers back: the completion
	 * handler owns the current one while they are running.
	 */
	smi2021_stop_stream(smi2021);
	smi2021_clear_queue(smi2021, VB2_BUF_STATE_ERROR);

	/* Keep the audio data flowing if the pcm device is capturing */
	smi2021_set_stream(smi2021);
}

/*
 * Stop streaming for good, as the device is gone or the probe failed. The
 * stream is stopped while the device can still be told, which a disconnect
 * by unbinding the driver needs: alternate setting 0, which the USB core
 * selects then, does not stop the engines. Once this returns, no URB is
 * running and smi2021_set_stream() does not start any, so the pcm device can
 * be unregistered and the register accesses refused.
 */
static void smi2021_shutdown(struct smi2021 *smi2021)
{
	guard(mutex)(&smi2021->stream_lock);

	smi2021->video_wanted = false;
	smi2021->audio_wanted = false;
	smi2021_stop_stream(smi2021);
	smi2021_clear_queue(smi2021, VB2_BUF_STATE_ERROR);

	scoped_guard(mutex, &smi2021->reg_lock)
		smi2021->udev = NULL;
}

/* Allocates the device numbers */
static DEFINE_IDA(smi2021_ida);

void smi2021_release(struct v4l2_device *v4l2_dev)
{
	struct smi2021 *smi2021 = container_of(v4l2_dev, struct smi2021,
					       v4l2_dev);

	/* Unregister the decoder before the i2c adapter it lives on */
	smi2021_v4l2_cleanup(smi2021);
	i2c_del_adapter(&smi2021->i2c_adap);

	mutex_destroy(&smi2021->reg_lock);
	mutex_destroy(&smi2021->v4l2_lock);
	mutex_destroy(&smi2021->stream_lock);

	ida_free(&smi2021_ida, smi2021->nr);
	kfree(smi2021);
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
	smi2021->ifnum = alt->desc.bInterfaceNumber;
	smi2021->iso_ep = usb_endpoint_num(&ep->desc);
	/* 3072 bytes, as the endpoint has two additional transactions */
	smi2021->iso_size = usb_endpoint_max_periodic_payload(udev, ep);
	/* In microframes, the descriptor holds the exponent plus one */
	smi2021->iso_interval = 1 << (ep->desc.bInterval - 1);
	/* The firmware starts in standby, so nothing is streaming yet */
	smi2021->stream_mode = SMI2021_MODE_STANDBY;

	mutex_init(&smi2021->reg_lock);
	mutex_init(&smi2021->v4l2_lock);
	mutex_init(&smi2021->stream_lock);
	spin_lock_init(&smi2021->buf_lock);
	INIT_LIST_HEAD(&smi2021->avail_bufs);

	rc = smi2021_v4l2_init(smi2021);
	if (rc)
		goto err_free;

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

	/*
	 * Audio arrives in the same URBs as the video, so set up the audio
	 * device before streaming can start.
	 */
	rc = smi2021_snd_register(smi2021);
	if (rc)
		dev_warn(smi2021->dev, "failed to register audio device: %pe\n",
			 ERR_PTR(rc));

	rc = smi2021_v4l2_register(smi2021);
	if (rc)
		goto err_snd;

	usb_set_intfdata(intf, smi2021);

	dev_info(smi2021->dev, "device %u, %s\n", smi2021->nr,
		 smi2021->board->name);

	return 0;

err_snd:
	/* The pcm device may have started a stream already */
	smi2021_shutdown(smi2021);
	smi2021_snd_unregister(smi2021);
err_put:
	v4l2_device_put(&smi2021->v4l2_dev);

	return rc;

err_free:
	mutex_destroy(&smi2021->reg_lock);
	mutex_destroy(&smi2021->v4l2_lock);
	mutex_destroy(&smi2021->stream_lock);
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
	 * unregistered, no new ones can start, so none can queue a buffer
	 * after smi2021_shutdown() handed them all back.
	 */
	scoped_guard(mutex, &smi2021->v4l2_lock) {
		smi2021_shutdown(smi2021);
		smi2021_v4l2_unregister(smi2021);
	}

	smi2021_snd_unregister(smi2021);

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
