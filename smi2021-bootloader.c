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

/*
 * OpenSMI21, the open source firmware for the SMI2021. It implements the
 * protocol of the vendor's firmware that enumerates as 0x3c, and does so on
 * every board, whichever firmware the board came with.
 */
#define SMI2021_FW_NAME			"smi2021/opensmi21.bin"

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

	if (fw->size % sizeof(chunk.data))
		return dev_err_probe(&udev->dev, -EINVAL,
				     "firmware has an invalid size\n");

	rc = smi2021_ctrl_read(udev, SMI2021_HW_STATE_HEAD, &state,
			       sizeof(state));
	if (rc)
		return dev_err_probe(&udev->dev, rc,
				     "failed to read the device state\n");

	if (state.state != SMI2021_FW_READY)
		return dev_err_probe(&udev->dev, -ENODEV,
				     "device is not ready for a firmware upload\n");

	for (i = 0; i < fw->size; i += sizeof(chunk.data)) {
		memcpy(chunk.data, fw->data + i, sizeof(chunk.data));

		rc = smi2021_ctrl_write(udev, chunk.head[0], &chunk,
					sizeof(chunk));
		if (rc)
			return dev_err_probe(&udev->dev, rc,
					     "firmware upload failed\n");
	}

	rc = smi2021_ctrl_write(udev, ack.head, &ack, sizeof(ack));
	if (rc)
		return dev_err_probe(&udev->dev, rc,
				     "device failed to ack the firmware\n");

	return 0;
}

int smi2021_bootloader_probe(struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	const struct firmware *fw;
	int rc;

	rc = request_firmware(&fw, SMI2021_FW_NAME, &udev->dev);
	if (rc)
		return dev_err_probe(&udev->dev, rc,
				     "failed to load %s\n", SMI2021_FW_NAME);

	rc = smi2021_load_firmware(udev, fw);
	release_firmware(fw);

	return rc;
}

MODULE_FIRMWARE(SMI2021_FW_NAME);
