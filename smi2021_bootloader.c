// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for the Somagic SMI2021 USB video capture bridge
 *
 * Firmware upload for devices that are still in bootloader mode.
 *
 * Copyright (C) 2011-2013 Jon Arne Jørgensen <jonjon.arnearne@gmail.com>
 * Copyright (C) 2011, 2012 Tony Brown, Michal Demin, Jeffry Johnston
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/usb.h>

#include "smi2021.h"

#define FIRMWARE_CHUNK_SIZE	62
#define FIRMWARE_HEADER_SIZE	2

#define FIRMWARE_CHUNK_HEAD_0	0x05
#define FIRMWARE_CHUNK_HEAD_1	0xff
#define FIRMWARE_HW_STATE_HEAD	0x01
#define FIRMWARE_HW_READY_STATE	0x07

/* Timeout for the vendor specific control requests, in milliseconds */
#define FIRMWARE_CTRL_TIMEOUT	1000

#define SMI2021_3C_FIRMWARE	"smi2021_3c.bin"
#define SMI2021_3E_FIRMWARE	"smi2021_3e.bin"
#define SMI2021_3F_FIRMWARE	"smi2021_3f.bin"

static unsigned int firmware_version;
module_param(firmware_version, uint, 0644);
MODULE_PARM_DESC(firmware_version,
		 "Firmware to upload to a device in bootloader mode, as all of them report the same USB product id. Accepted values: 0x3c, 0x3e, 0x3f");

static int smi2021_load_firmware(struct usb_device *udev,
				 const struct firmware *firmware)
{
	u8 chunk[FIRMWARE_HEADER_SIZE + FIRMWARE_CHUNK_SIZE];
	struct smi2021_set_hw_state hw_state;
	size_t i;
	int rc;

	if (firmware->size % FIRMWARE_CHUNK_SIZE) {
		dev_err(&udev->dev, "firmware has an invalid size\n");
		return -EINVAL;
	}

	rc = usb_control_msg_recv(udev, 0, SMI2021_USB_REQUEST,
				  USB_DIR_IN | USB_TYPE_VENDOR |
				  USB_RECIP_DEVICE,
				  FIRMWARE_HW_STATE_HEAD, SMI2021_USB_INDEX,
				  &hw_state, sizeof(hw_state),
				  FIRMWARE_CTRL_TIMEOUT, GFP_KERNEL);
	if (rc) {
		dev_err(&udev->dev, "could not read the device state: %d\n",
			rc);
		return rc;
	}

	if (hw_state.state != FIRMWARE_HW_READY_STATE) {
		dev_err(&udev->dev,
			"device is not ready for a firmware upload\n");
		return -ENODEV;
	}

	chunk[0] = FIRMWARE_CHUNK_HEAD_0;
	chunk[1] = FIRMWARE_CHUNK_HEAD_1;

	for (i = 0; i < firmware->size / FIRMWARE_CHUNK_SIZE; i++) {
		memcpy(chunk + FIRMWARE_HEADER_SIZE,
		       firmware->data + (i * FIRMWARE_CHUNK_SIZE),
		       FIRMWARE_CHUNK_SIZE);

		rc = usb_control_msg_send(udev, 0, SMI2021_USB_REQUEST,
					  USB_DIR_OUT | USB_TYPE_VENDOR |
					  USB_RECIP_DEVICE,
					  FIRMWARE_CHUNK_HEAD_0,
					  SMI2021_USB_INDEX,
					  chunk, sizeof(chunk),
					  FIRMWARE_CTRL_TIMEOUT, GFP_KERNEL);
		if (rc) {
			dev_err(&udev->dev, "firmware upload failed: %d\n", rc);
			return rc;
		}
	}

	hw_state.head = FIRMWARE_HW_READY_STATE;
	hw_state.state = 0x00;

	rc = usb_control_msg_send(udev, 0, SMI2021_USB_REQUEST,
				  USB_DIR_OUT | USB_TYPE_VENDOR |
				  USB_RECIP_DEVICE,
				  FIRMWARE_HW_READY_STATE, SMI2021_USB_INDEX,
				  &hw_state, sizeof(hw_state),
				  FIRMWARE_CTRL_TIMEOUT, GFP_KERNEL);
	if (rc)
		dev_err(&udev->dev, "device failed to ack firmware: %d\n", rc);

	return rc;
}

/*
 * There are at least three different hardware versions of the smi2021 devices
 * that require different firmwares. They all report the same USB product id
 * while they are in bootloader mode, so there is no way to tell which device
 * the user has just plugged in. If only one smi2021 firmware is installed it
 * is probably the correct one for the device.
 *
 * Users with multiple different firmwares and devices have to select the
 * version with the firmware_version module parameter before plugging in each
 * device.
 */
int smi2021_bootloader_probe(struct usb_interface *intf,
			     const struct usb_device_id *devid)
{
	static const struct {
		unsigned int	id;
		const char	*name;
	} hw_versions[] = {
		{ 0x3f, SMI2021_3F_FIRMWARE },
		{ 0x3e, SMI2021_3E_FIRMWARE },
		{ 0x3c, SMI2021_3C_FIRMWARE },
	};
	struct usb_device *udev = interface_to_usbdev(intf);
	const struct firmware *firmware;
	unsigned int i;
	int rc = -ENOENT;

	for (i = 0; i < ARRAY_SIZE(hw_versions); i++) {
		if (firmware_version && firmware_version != hw_versions[i].id)
			continue;

		rc = request_firmware_direct(&firmware, hw_versions[i].name,
					     &udev->dev);
		if (rc == 0) {
			dev_info(&udev->dev, "uploading %s\n",
				 hw_versions[i].name);
			goto load_fw;
		}
	}

	if (firmware_version)
		dev_err(&udev->dev,
			"the specified firmware could not be loaded\n");
	else
		dev_err(&udev->dev,
			"could not load any firmware for this device\n");

	return rc;

load_fw:
	rc = smi2021_load_firmware(udev, firmware);
	release_firmware(firmware);

	return rc;
}

MODULE_FIRMWARE(SMI2021_3C_FIRMWARE);
MODULE_FIRMWARE(SMI2021_3E_FIRMWARE);
MODULE_FIRMWARE(SMI2021_3F_FIRMWARE);
