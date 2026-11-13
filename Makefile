# SPDX-License-Identifier: GPL-2.0-only
smi2021-y := smi2021_main.o \
	     smi2021_bootloader.o \
	     smi2021_v4l2.o \
	     smi2021_audio.o

obj-$(CONFIG_VIDEO_SMI2021) += smi2021.o
