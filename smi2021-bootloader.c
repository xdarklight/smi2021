// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for the Somagic SMI2021 USB video capture bridge
 *
 * Firmware upload for devices that are still in bootloader mode.
 *
 * Copyright (C) 2011-2013 Jon Arne Jørgensen <jonjon.arnearne@gmail.com>
 * Copyright (C) 2011, 2012 Tony Brown, Michal Demin, Jeffry Johnston
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 */

#include <linux/firmware.h>
#include <linux/module.h>

#include "smi2021.h"

#define SMI2021_FW_CHUNK_SIZE		62
#define SMI2021_FW_CHUNK_HEAD		0x05
#define SMI2021_FW_CHUNK_HEAD_1		0xff

/* Hardware state that the bootloader reports, also used to acknowledge */
#define SMI2021_FW_READY		0x07

static unsigned int firmware_version = 0x3c;
module_param(firmware_version, uint, 0644);
MODULE_PARM_DESC(firmware_version,
		 "Firmware to upload to a device in bootloader mode, which becomes the USB product id: 0x3c (default), 0x3e or 0x3f");

/*
 * The firmware is uploaded in requests of 64 bytes, the packet size of the
 * control endpoint, each of them a two byte header and a piece of firmware.
 */
struct smi2021_fw_chunk {
	u8 head[2];
	u8 data[SMI2021_FW_CHUNK_SIZE];
} __packed;

static int smi2021_load_firmware(struct usb_device *udev,
				 const struct firmware *fw)
{
	struct smi2021_fw_chunk chunk = {
		.head = { SMI2021_FW_CHUNK_HEAD, SMI2021_FW_CHUNK_HEAD_1 },
	};
	struct smi2021_hw_state state, ack = {
		.head = SMI2021_FW_READY,
	};
	size_t i;
	int rc;

	if (fw->size % sizeof(chunk.data)) {
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

	for (i = 0; i < fw->size; i += sizeof(chunk.data)) {
		memcpy(chunk.data, fw->data + i, sizeof(chunk.data));

		rc = smi2021_ctrl_write(udev, chunk.head[0], &chunk,
					sizeof(chunk));
		if (rc) {
			dev_err(&udev->dev, "firmware upload failed: %pe\n",
				ERR_PTR(rc));
			return rc;
		}
	}

	rc = smi2021_ctrl_write(udev, ack.head, &ack, sizeof(ack));
	if (rc)
		dev_err(&udev->dev, "device failed to ack the firmware: %pe\n",
			ERR_PTR(rc));

	return rc;
}

/*
 * Devices come with one of several firmwares, but all of them report the same
 * USB product id in bootloader mode, so there is no way to tell which one. The
 * firmware of the 0x3c devices is the default; the firmware_version module
 * parameter selects another one.
 */
int smi2021_bootloader_probe(struct usb_interface *intf)
{
	static const struct {
		unsigned int	version;
		const char	*name;
	} firmwares[] = {
		{ 0x3c, "smi2021_3c.bin" },
		{ 0x3e, "smi2021_3e.bin" },
		{ 0x3f, "smi2021_3f.bin" },
	};
	struct usb_device *udev = interface_to_usbdev(intf);
	unsigned int i, version = READ_ONCE(firmware_version);
	const struct firmware *fw;
	int rc;

	for (i = 0; i < ARRAY_SIZE(firmwares); i++)
		if (firmwares[i].version == version)
			break;

	if (i == ARRAY_SIZE(firmwares)) {
		dev_err(&udev->dev, "invalid firmware_version 0x%x\n", version);
		return -EINVAL;
	}

	rc = request_firmware(&fw, firmwares[i].name, &udev->dev);
	if (rc) {
		dev_err(&udev->dev,
			"failed to load %s, the firmware_version module parameter selects another firmware\n",
			firmwares[i].name);
		return rc;
	}

	dev_info(&udev->dev, "uploading %s\n", firmwares[i].name);
	rc = smi2021_load_firmware(udev, fw);
	release_firmware(fw);

	return rc;
}

MODULE_FIRMWARE("smi2021_3c.bin");
MODULE_FIRMWARE("smi2021_3e.bin");
MODULE_FIRMWARE("smi2021_3f.bin");
