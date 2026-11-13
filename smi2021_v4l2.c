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

#include <linux/cleanup.h>

#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "smi2021.h"

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

	if (i->index >= smi2021->board->num_inputs)
		return -EINVAL;

	strscpy(i->name, smi2021->board->inputs[i->index].name,
		sizeof(i->name));
	i->type = V4L2_INPUT_TYPE_CAMERA;
	i->std = smi2021->vdev.tvnorms;

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
	int rc;

	if (i >= smi2021->board->num_inputs)
		return -EINVAL;

	rc = v4l2_subdev_call(smi2021->decoder, video, s_routing,
			      smi2021->board->inputs[i].type, 0, 0);
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

/* The format is fixed, so this implements G_FMT, TRY_FMT and S_FMT */
static int smi2021_fmt_vid_cap(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	f->fmt.pix.width = SMI2021_WIDTH;
	f->fmt.pix.height = smi2021->cur_height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
	f->fmt.pix.field = V4L2_FIELD_INTERLACED;
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

static int smi2021_s_std(struct file *file, void *priv, v4l2_std_id norm)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	if (!(norm & V4L2_STD_ALL))
		return -EINVAL;

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

	.vidioc_log_status		= v4l2_ctrl_log_status,
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

static const struct vb2_ops smi2021_video_qops = {
	.queue_setup		= smi2021_queue_setup,
	.buf_queue		= smi2021_buf_queue,
	.start_streaming	= smi2021_start_streaming,
	.stop_streaming		= smi2021_stop_streaming,
};

/* Must be called with vb_queue_lock held and the URBs stopped */
void smi2021_clear_queue(struct smi2021 *smi2021, enum vb2_buffer_state state)
{
	struct smi2021_buf *buf, *tmp;

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

int smi2021_vb2_setup(struct smi2021 *smi2021)
{
	struct vb2_queue *q = &smi2021->vb_vidq;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_READ | VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	q->drv_priv = smi2021;
	q->buf_struct_size = sizeof(struct smi2021_buf);
	q->ops = &smi2021_video_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->lock = &smi2021->vb_queue_lock;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	return vb2_queue_init(q);
}

int smi2021_video_register(struct smi2021 *smi2021)
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
