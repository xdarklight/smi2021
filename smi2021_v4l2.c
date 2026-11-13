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

/*
 * vidioc ioctls
 */
static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	strscpy(cap->driver, "smi2021", sizeof(cap->driver));
	strscpy(cap->card, "smi2021", sizeof(cap->card));
	usb_make_path(smi2021->udev, cap->bus_info, sizeof(cap->bus_info));

	return 0;
}

static int vidioc_enum_input(struct file *file, void *priv,
			     struct v4l2_input *i)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	if (i->index >= smi2021->vid_input_count)
		return -EINVAL;

	strscpy(i->name, smi2021->vid_inputs[i->index].name, sizeof(i->name));
	i->type = V4L2_INPUT_TYPE_CAMERA;
	i->std = smi2021->vdev.tvnorms;

	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_UYVY;

	return 0;
}

static int vidioc_fmt_vid_cap(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	f->fmt.pix.width = SMI2021_BYTES_PER_LINE / 2;
	f->fmt.pix.height = smi2021->cur_height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
	f->fmt.pix.field = V4L2_FIELD_INTERLACED;
	f->fmt.pix.bytesperline = SMI2021_BYTES_PER_LINE;
	f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	if (fsize->index != 0)
		return -EINVAL;

	if (fsize->pixel_format != V4L2_PIX_FMT_UYVY)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = SMI2021_BYTES_PER_LINE / 2;
	fsize->discrete.height = smi2021->cur_height;

	return 0;
}

static int vidioc_g_std(struct file *file, void *priv, v4l2_std_id *norm)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	*norm = smi2021->cur_norm;

	return 0;
}

static int vidioc_s_std(struct file *file, void *priv, v4l2_std_id norm)
{
	struct smi2021 *smi2021 = video_drvdata(file);
	unsigned int height;

	if (norm & V4L2_STD_525_60)
		height = SMI2021_NTSC_LINES;
	else if (norm & V4L2_STD_625_50)
		height = SMI2021_PAL_LINES;
	else
		return -EINVAL;

	if (norm == smi2021->cur_norm)
		return 0;

	if (vb2_is_busy(&smi2021->vb_vidq))
		return -EBUSY;

	smi2021->cur_norm = norm;
	smi2021->cur_height = height;

	return v4l2_subdev_call(smi2021->decoder, video, s_std, norm);
}

static int vidioc_querystd(struct file *file, void *priv, v4l2_std_id *norm)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	return v4l2_subdev_call(smi2021->decoder, video, querystd, norm);
}

static int vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct smi2021 *smi2021 = video_drvdata(file);

	*i = smi2021->cur_input;

	return 0;
}

static int vidioc_s_input(struct file *file, void *priv, unsigned int i)
{
	struct smi2021 *smi2021 = video_drvdata(file);
	int rc;

	if (i >= smi2021->vid_input_count)
		return -EINVAL;

	rc = v4l2_subdev_call(smi2021->decoder, video, s_routing,
			      smi2021->vid_inputs[i].type, 0, 0);
	if (rc < 0 && rc != -ENOIOCTLCMD)
		return rc;

	smi2021->cur_input = i;

	return 0;
}

static const struct v4l2_ioctl_ops smi2021_ioctl_ops = {
	.vidioc_querycap		= vidioc_querycap,
	.vidioc_enum_input		= vidioc_enum_input,
	.vidioc_enum_fmt_vid_cap	= vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= vidioc_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= vidioc_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= vidioc_fmt_vid_cap,
	.vidioc_enum_framesizes		= vidioc_enum_framesizes,
	.vidioc_g_std			= vidioc_g_std,
	.vidioc_s_std			= vidioc_s_std,
	.vidioc_querystd		= vidioc_querystd,
	.vidioc_g_input			= vidioc_g_input,
	.vidioc_s_input			= vidioc_s_input,

	/* vb2 handles these */
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,

	/* v4l2-event and v4l2-ctrl handle these */
	.vidioc_log_status		= v4l2_ctrl_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

/*
 * Videobuf2 operations
 */
static int queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
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

static void buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct smi2021 *smi2021 = vb2_get_drv_priv(vb->vb2_queue);
	struct smi2021_buf *buf = container_of(vbuf, struct smi2021_buf, vb);
	unsigned long flags;

	spin_lock_irqsave(&smi2021->buf_lock, flags);

	if (!smi2021->udev) {
		/*
		 * If the device is disconnected return the buffer to
		 * userspace directly. The next QBUF call will fail with
		 * -ENODEV.
		 */
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
	} else {
		buf->mem = vb2_plane_vaddr(vb, 0);
		buf->length = vb2_plane_size(vb, 0);
		buf->pos = 0;
		buf->in_blank = true;
		buf->second_field = false;

		list_add_tail(&buf->list, &smi2021->avail_bufs);
	}

	spin_unlock_irqrestore(&smi2021->buf_lock, flags);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct smi2021 *smi2021 = vb2_get_drv_priv(vq);

	return smi2021_start(smi2021);
}

static void stop_streaming(struct vb2_queue *vq)
{
	struct smi2021 *smi2021 = vb2_get_drv_priv(vq);

	smi2021_stop(smi2021);
}

static const struct vb2_ops smi2021_video_qops = {
	.queue_setup		= queue_setup,
	.buf_queue		= buffer_queue,
	.start_streaming	= start_streaming,
	.stop_streaming		= stop_streaming,
};

static const struct video_device v4l_template = {
	.name			= "smi2021",
	.tvnorms		= V4L2_STD_ALL,
	.fops			= &smi2021_fops,
	.ioctl_ops		= &smi2021_ioctl_ops,
	.release		= video_device_release_empty,
	.device_caps		= V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
				  V4L2_CAP_READWRITE,
};

/* Must be called with both v4l2_lock and vb_queue_lock held */
void smi2021_clear_queue(struct smi2021 *smi2021, enum vb2_buffer_state state)
{
	struct smi2021_buf *buf;
	unsigned long flags;

	spin_lock_irqsave(&smi2021->buf_lock, flags);

	while (!list_empty(&smi2021->avail_bufs)) {
		buf = list_first_entry(&smi2021->avail_bufs,
				       struct smi2021_buf, list);
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}

	/* It is important to clear the current buffer as well */
	if (smi2021->cur_buf) {
		vb2_buffer_done(&smi2021->cur_buf->vb.vb2_buf, state);
		smi2021->cur_buf = NULL;
	}

	spin_unlock_irqrestore(&smi2021->buf_lock, flags);
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
	int rc;

	/* Initialize video_device with a template structure */
	smi2021->vdev = v4l_template;
	smi2021->vdev.queue = &smi2021->vb_vidq;

	/*
	 * Provide a mutex for the v4l2 core. The videobuf2 queue has its own,
	 * which was set up by smi2021_vb2_setup().
	 */
	smi2021->vdev.lock = &smi2021->v4l2_lock;
	smi2021->vdev.v4l2_dev = &smi2021->v4l2_dev;

	video_set_drvdata(&smi2021->vdev, smi2021);

	rc = video_register_device(&smi2021->vdev, VFL_TYPE_VIDEO, -1);
	if (rc < 0) {
		dev_err(smi2021->dev, "video_register_device failed (%d)\n",
			rc);
		return rc;
	}

	v4l2_info(&smi2021->v4l2_dev, "V4L2 device registered as %s\n",
		  video_device_node_name(&smi2021->vdev));

	return 0;
}
