# SPDX-License-Identifier: GPL-2.0-only
smi2021-y := smi2021-main.o \
	     smi2021-bootloader.o \
	     smi2021-v4l2.o \
	     smi2021-audio.o

obj-$(CONFIG_VIDEO_SMI2021) += smi2021.o
