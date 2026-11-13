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

#include "smi2021.h"

#define SMI2021_FW_CHUNK_SIZE		62
#define SMI2021_FW_CHUNK_HEAD		0x05
#define SMI2021_FW_CHUNK_HEAD_1		0xff

/* Hardware state that the bootloader reports, also used to acknowledge */
#define SMI2021_FW_READY		0x07

static unsigned int firmware_version;
module_param(firmware_version, uint, 0644);
MODULE_PARM_DESC(firmware_version,
		 "Firmware to upload to a device in bootloader mode, as all of them report the same USB product id. Accepted values: 0x3c, 0x3e, 0x3f");

static int smi2021_load_firmware(struct usb_device *udev,
				 const struct firmware *fw)
{
	u8 chunk[2 + SMI2021_FW_CHUNK_SIZE] = {
		SMI2021_FW_CHUNK_HEAD, SMI2021_FW_CHUNK_HEAD_1,
	};
	struct smi2021_hw_state state;
	size_t i;
	int rc;

	if (fw->size % SMI2021_FW_CHUNK_SIZE) {
		dev_err(&udev->dev, "firmware has an invalid size\n");
		return -EINVAL;
	}

	rc = smi2021_ctrl_read(udev, SMI2021_HW_STATE_HEAD, &state,
			       sizeof(state));
	if (rc) {
		dev_err(&udev->dev, "failed to read the device state: %pe\n",
			ERR_PTR(rc));
		return rc;
	}

	if (state.state != SMI2021_FW_READY) {
		dev_err(&udev->dev, "device is not ready for a firmware upload\n");
		return -ENODEV;
	}

	for (i = 0; i < fw->size; i += SMI2021_FW_CHUNK_SIZE) {
		memcpy(chunk + 2, fw->data + i, SMI2021_FW_CHUNK_SIZE);

		rc = smi2021_ctrl_write(udev, chunk[0], chunk, sizeof(chunk));
		if (rc) {
			dev_err(&udev->dev, "firmware upload failed: %pe\n",
				ERR_PTR(rc));
			return rc;
		}
	}

	state.head = SMI2021_FW_READY;
	state.state = 0x00;

	rc = smi2021_ctrl_write(udev, state.head, &state, sizeof(state));
	if (rc)
		dev_err(&udev->dev, "device failed to ack the firmware: %pe\n",
			ERR_PTR(rc));

	return rc;
}

/*
 * There are at least three hardware versions of the smi2021 devices that
 * need different firmwares. They all report the same USB product id while in
 * bootloader mode, so there is no way to tell them apart. If only one smi2021
 * firmware is installed it is probably the one for the device, otherwise the
 * firmware_version module parameter selects one.
 */
int smi2021_bootloader_probe(struct usb_interface *intf)
{
	static const struct {
		unsigned int	version;
		const char	*name;
	} firmwares[] = {
		{ 0x3f, "smi2021_3f.bin" },
		{ 0x3e, "smi2021_3e.bin" },
		{ 0x3c, "smi2021_3c.bin" },
	};
	struct usb_device *udev = interface_to_usbdev(intf);
	const struct firmware *fw;
	unsigned int i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(firmwares); i++) {
		if (firmware_version && firmware_version != firmwares[i].version)
			continue;

		rc = request_firmware_direct(&fw, firmwares[i].name,
					     &udev->dev);
		if (rc)
			continue;

		dev_info(&udev->dev, "uploading %s\n", firmwares[i].name);
		rc = smi2021_load_firmware(udev, fw);
		release_firmware(fw);

		return rc;
	}

	dev_err(&udev->dev, "no suitable firmware found\n");

	return -ENOENT;
}

MODULE_FIRMWARE("smi2021_3c.bin");
MODULE_FIRMWARE("smi2021_3e.bin");
MODULE_FIRMWARE("smi2021_3f.bin");
