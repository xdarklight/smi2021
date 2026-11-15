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
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/ktime.h>

#include <media/i2c/saa7115.h>
#include <media/v4l2-common.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "smi2021.h"

/* 7 bit I2C address of the saa7113 compatible decoder, 4AH in its datasheet */
#define SMI2021_DECODER_I2C_ADDR	0x25

/*
 * Bits of the fourth word of a timing reference code, see Table 2 of ITU-R
 * BT.656. The protection bits are ignored, as the decoder datasheet advises.
 */
#define SMI2021_TRC_H			BIT(4)	/* EAV rather than SAV */
#define SMI2021_TRC_V			BIT(5)	/* vertical blanking */
#define SMI2021_TRC_F			BIT(6)	/* second field */

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

/*
 * All inputs of the decoder that a board may have connected, for a board
 * that smi2021_identify_board() does not know.
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

static const struct smi2021_board smi2021_boards[] = {
	[SMI2021_BOARD_UNKNOWN] = {
		.name = "unknown board, all inputs",
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

static const struct v4l2_file_operations smi2021_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static int smi2021_querycap(struct file *file, void *priv,
			    struct v4l2_capability *cap)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strscpy(cap->card, "Somagic SMI2021", sizeof(cap->card));
	usb_make_path(smi2021->udev, cap->bus_info, sizeof(cap->bus_info));

	return 0;
}

static int smi2021_enum_input(struct file *file, void *priv,
			      struct v4l2_input *i)
{
	struct smi2021 *smi2021 = video_drvdata(file);
	const struct smi2021_board *board = &smi2021_boards[smi2021->board_type];

	if (i->index >= board->num_inputs)
		return -EINVAL;

	strscpy(i->name, board->inputs[i->index].name, sizeof(i->name));
	i->type = V4L2_INPUT_TYPE_CAMERA;
	i->std = smi2021->vdev.tvnorms;

	/* The decoder can only tell the signal status of the current input */
	if (i->index == smi2021->cur_input)
		v4l2_subdev_call(smi2021->decoder, video, g_input_status,
				 &i->status);

	return 0;
}

static int smi2021_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	*i = smi2021->cur_input;

	return 0;
}

static int smi2021_s_input(struct file *file, void *priv, unsigned int i)
{
	struct smi2021 *smi2021 = video_drvdata(file);
	const struct smi2021_board *board = &smi2021_boards[smi2021->board_type];
	int rc;

	if (i >= board->num_inputs)
		return -EINVAL;

	rc = v4l2_subdev_call(smi2021->decoder, video, s_routing,
			      board->inputs[i].route, 0, 0);
	if (rc)
		return rc;

	smi2021->cur_input = i;

	return 0;
}

static int smi2021_enum_fmt_vid_cap(struct file *file, void *priv,
				    struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_UYVY;

	return 0;
}

/*
 * A frame starts with the field flagged as the first one in the timing
 * reference codes, and that field goes to the even lines: the top field is
 * the older one with either standard. V4L2_FIELD_INTERLACED says that the
 * bottom field is the older one in M/NTSC, so be explicit with 525 lines.
 */
static enum v4l2_field smi2021_field(const struct smi2021 *smi2021)
{
	return smi2021->cur_norm & V4L2_STD_525_60 ? V4L2_FIELD_INTERLACED_TB :
						     V4L2_FIELD_INTERLACED;
}

/* The format is fixed, so this implements G_FMT, TRY_FMT and S_FMT */
static int smi2021_fmt_vid_cap(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	f->fmt.pix.width = SMI2021_WIDTH;
	f->fmt.pix.height = smi2021->cur_height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
	f->fmt.pix.field = smi2021_field(smi2021);
	f->fmt.pix.bytesperline = SMI2021_BYTES_PER_LINE;
	f->fmt.pix.sizeimage = SMI2021_BYTES_PER_LINE * smi2021->cur_height;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int smi2021_enum_framesizes(struct file *file, void *priv,
				   struct v4l2_frmsizeenum *fsize)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	if (fsize->index || fsize->pixel_format != V4L2_PIX_FMT_UYVY)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = SMI2021_WIDTH;
	fsize->discrete.height = smi2021->cur_height;

	return 0;
}

static int smi2021_g_std(struct file *file, void *priv, v4l2_std_id *norm)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	*norm = smi2021->cur_norm;

	return 0;
}

/*
 * The decoder decodes one colour system at a time, while S_STD may ask for
 * standards of several, for example what QUERYSTD returned: the saa7113 only
 * tells the line count, so that is all the standards with it. The saa7115
 * driver then picks SECAM over PAL, which leaves a PAL signal without colour.
 * Pick the most common system of the set instead, of the line count that the
 * saa7115 driver would go for.
 */
static v4l2_std_id smi2021_pick_std(v4l2_std_id norm)
{
	if (norm & V4L2_STD_525_60) {
		norm &= V4L2_STD_525_60;
		if (norm & V4L2_STD_NTSC)
			norm &= V4L2_STD_NTSC;
	} else if (norm & V4L2_STD_PAL) {
		norm &= V4L2_STD_PAL;
	}

	return norm;
}

static int smi2021_s_std(struct file *file, void *priv, v4l2_std_id norm)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	if (!(norm & V4L2_STD_ALL))
		return -EINVAL;

	norm = smi2021_pick_std(norm);

	if (norm == smi2021->cur_norm)
		return 0;

	if (vb2_is_busy(&smi2021->vb_vidq))
		return -EBUSY;

	smi2021->cur_norm = norm;
	smi2021->cur_height = norm & V4L2_STD_525_60 ? SMI2021_NTSC_LINES :
						       SMI2021_PAL_LINES;

	return v4l2_subdev_call(smi2021->decoder, video, s_std, norm);
}

static int smi2021_querystd(struct file *file, void *priv, v4l2_std_id *norm)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	return v4l2_subdev_call(smi2021->decoder, video, querystd, norm);
}

static int smi2021_log_status(struct file *file, void *priv)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	v4l2_info(&smi2021->v4l2_dev, "device: %s\n",
		  smi2021_boards[smi2021->board_type].name);
	v4l2_ctrl_log_status(file, priv);
	v4l2_device_call_all(&smi2021->v4l2_dev, 0, core, log_status);

	return 0;
}

static const struct v4l2_ioctl_ops smi2021_ioctl_ops = {
	.vidioc_querycap		= smi2021_querycap,
	.vidioc_enum_input		= smi2021_enum_input,
	.vidioc_g_input			= smi2021_g_input,
	.vidioc_s_input			= smi2021_s_input,
	.vidioc_enum_fmt_vid_cap	= smi2021_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= smi2021_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= smi2021_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= smi2021_fmt_vid_cap,
	.vidioc_enum_framesizes		= smi2021_enum_framesizes,
	.vidioc_g_std			= smi2021_g_std,
	.vidioc_s_std			= smi2021_s_std,
	.vidioc_querystd		= smi2021_querystd,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,

	.vidioc_log_status		= smi2021_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static int smi2021_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			       unsigned int *nplanes, unsigned int sizes[],
			       struct device *alloc_devs[])
{
	struct smi2021 *smi2021 = vb2_get_drv_priv(vq);
	unsigned int size = SMI2021_BYTES_PER_LINE * smi2021->cur_height;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;

	return 0;
}

static void smi2021_buf_queue(struct vb2_buffer *vb)
{
	struct smi2021 *smi2021 = vb2_get_drv_priv(vb->vb2_queue);
	struct smi2021_buf *buf = container_of(to_vb2_v4l2_buffer(vb),
					       struct smi2021_buf, vb);

	buf->mem = vb2_plane_vaddr(vb, 0);
	buf->pos = 0;
	buf->in_blank = true;
	buf->second_field = false;

	guard(spinlock_irqsave)(&smi2021->buf_lock);
	list_add_tail(&buf->list, &smi2021->avail_bufs);
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

/*
 * With 525 lines the decoder flags lines 20-263 and 283-525 as active (Table
 * 9 of the SAA7113H datasheet). The first two of each field are the VBI line
 * 20 and line 21, which carries closed captions, and the half line 283 and
 * line 284. Skip them, so that the lines captured are 22-261 and 285-524, as
 * with the vendor's Windows drivers. With 625 lines the decoder flags lines
 * 23-310 and 336-623, which are all captured.
 */
static unsigned int smi2021_skip_lines(const struct smi2021 *smi2021)
{
	return smi2021->cur_height == SMI2021_NTSC_LINES ? 2 : 0;
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
	buf->vb.field = smi2021_field(smi2021);
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

		/*
		 * Without a free buffer the frame is dropped, but it still
		 * counts, so that the gap shows in the sequence numbers.
		 */
		if (!buf)
			smi2021->sequence++;
		else
			buf->skip_lines = smi2021_skip_lines(smi2021);
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
		buf->skip_lines = smi2021_skip_lines(smi2021);
	}

	if (!buf->in_blank && buf->skip_lines) {
		buf->skip_lines--;
		buf->in_blank = true;
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
void smi2021_parse_video(struct smi2021 *smi2021, const u8 *data,
			 unsigned int len)
{
	static const u8 trc_prefix[] = { 0xff, 0x00, 0x00 };
	unsigned int i = 0;

	while (i < len) {
		if (!smi2021->trc_prefix_len) {
			/* Copy video data up to the next potential code */
			const u8 *start = memchr(data + i, trc_prefix[0],
						 len - i);
			unsigned int n = start ? start - (data + i) : len - i;

			smi2021_copy_video(smi2021, data + i, n);
			i += n;
			if (start) {
				smi2021->trc_prefix_len = 1;
				i++;
			}
		} else if (smi2021->trc_prefix_len == sizeof(trc_prefix)) {
			smi2021->trc_prefix_len = 0;
			smi2021_parse_trc(smi2021, data[i++]);
		} else if (data[i] == trc_prefix[smi2021->trc_prefix_len]) {
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

static const struct vb2_ops smi2021_video_qops = {
	.queue_setup		= smi2021_queue_setup,
	.buf_queue		= smi2021_buf_queue,
	.start_streaming	= smi2021_start_streaming,
	.stop_streaming		= smi2021_stop_streaming,
};

/* Must be called with the isochronous transfers stopped */
void smi2021_clear_queue(struct smi2021 *smi2021, enum vb2_buffer_state state)
{
	struct smi2021_buf *buf, *tmp;

	lockdep_assert_held(&smi2021->stream_lock);

	scoped_guard(spinlock_irqsave, &smi2021->buf_lock) {
		list_for_each_entry_safe(buf, tmp, &smi2021->avail_bufs, list) {
			list_del(&buf->list);
			vb2_buffer_done(&buf->vb.vb2_buf, state);
		}
	}

	if (smi2021->cur_buf) {
		vb2_buffer_done(&smi2021->cur_buf->vb.vb2_buf, state);
		smi2021->cur_buf = NULL;
	}
}

static int smi2021_vb2_setup(struct smi2021 *smi2021)
{
	struct vb2_queue *q = &smi2021->vb_vidq;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_READ | VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	q->drv_priv = smi2021;
	q->buf_struct_size = sizeof(struct smi2021_buf);
	q->ops = &smi2021_video_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	/*
	 * The same lock as the other ioctls, so that S_STD can change the size
	 * of the buffers once it has found that none are allocated.
	 */
	q->lock = &smi2021->v4l2_lock;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	return vb2_queue_init(q);
}

static int smi2021_video_register(struct smi2021 *smi2021)
{
	struct video_device *vdev = &smi2021->vdev;
	int rc;

	strscpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
	vdev->v4l2_dev = &smi2021->v4l2_dev;
	vdev->fops = &smi2021_fops;
	vdev->ioctl_ops = &smi2021_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &smi2021->v4l2_lock;
	vdev->queue = &smi2021->vb_vidq;
	vdev->tvnorms = V4L2_STD_ALL;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;
	video_set_drvdata(vdev, smi2021);

	rc = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (rc)
		return rc;

	v4l2_info(&smi2021->v4l2_dev, "registered as %s\n",
		  video_device_node_name(vdev));

	return 0;
}

/*
 * The decoder runs the chrominance comb filter (NTSC) and the PAL delay line
 * only on the lines inside its VREF window. Select the long one, which covers
 * all captured lines with 625 lines and all but the first one of each field
 * with 525 lines (Table 46 of the SAA7113H datasheet), as the vendor's Windows
 * drivers do for 50 Hz. saa7113_init sets VRLN already but gm7113c_init does
 * not. The V flag of the timing reference codes stays as in ITU-R BT.656.
 */
static bool smi2021_decoder_vrln = true;

static struct saa7115_platform_data smi2021_decoder_pdata = {
	.saa7113_r10_vrln = &smi2021_decoder_vrln,
};

/*
 * Create the V4L2 device. Once this succeeds smi2021_release() owns the
 * teardown of the driver state, so the caller gives up its reference with
 * v4l2_device_put() rather than unwinding by hand.
 */
int smi2021_v4l2_init(struct smi2021 *smi2021)
{
	int rc;

	smi2021->cur_norm = V4L2_STD_NTSC;
	smi2021->cur_height = SMI2021_NTSC_LINES;

	rc = smi2021_vb2_setup(smi2021);
	if (rc)
		return rc;

	rc = v4l2_ctrl_handler_init(&smi2021->ctrl_handler, 0);
	if (rc)
		goto err_vb2;

	smi2021->v4l2_dev.release = smi2021_release;
	smi2021->v4l2_dev.ctrl_handler = &smi2021->ctrl_handler;
	rc = v4l2_device_register(smi2021->dev, &smi2021->v4l2_dev);
	if (rc)
		goto err_ctrl;

	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&smi2021->ctrl_handler);
err_vb2:
	vb2_queue_release(&smi2021->vb_vidq);

	return rc;
}

/* Tear down what smi2021_v4l2_init() set up */
void smi2021_v4l2_cleanup(struct smi2021 *smi2021)
{
	vb2_queue_release(&smi2021->vb_vidq);
	v4l2_device_unregister(&smi2021->v4l2_dev);
	v4l2_ctrl_handler_free(&smi2021->ctrl_handler);
}

/* Attach the decoder and offer the video device to userspace */
int smi2021_v4l2_register(struct smi2021 *smi2021)
{
	struct i2c_board_info decoder_info = {
		I2C_BOARD_INFO("saa7115_auto", SMI2021_DECODER_I2C_ADDR),
		.platform_data = &smi2021_decoder_pdata,
	};
	const struct smi2021_board *board = &smi2021_boards[smi2021->board_type];
	int rc;

	/* The saa7115 driver tells a saa7113 from its gm7113c clone itself */
	smi2021->decoder = v4l2_i2c_new_subdev_board(&smi2021->v4l2_dev,
						     &smi2021->i2c_adap,
						     &decoder_info, NULL);
	if (!smi2021->decoder)
		return dev_err_probe(smi2021->dev, -ENODEV,
				     "failed to register the video decoder\n");

	v4l2_subdev_call(smi2021->decoder, video, s_std, smi2021->cur_norm);
	v4l2_subdev_call(smi2021->decoder, video, s_routing,
			 board->inputs[smi2021->cur_input].route, 0, 0);

	rc = smi2021_video_register(smi2021);
	if (rc)
		return dev_err_probe(smi2021->dev, rc,
				     "failed to register video device\n");

	return 0;
}

/* Stop offering the video device, the hardware behind it is gone */
void smi2021_v4l2_unregister(struct smi2021 *smi2021)
{
	video_unregister_device(&smi2021->vdev);
	v4l2_device_disconnect(&smi2021->v4l2_dev);
}
