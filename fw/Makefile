# SPDX-License-Identifier: GPL-2.0-or-later
#
# OpenSMI21: open source firmware for the Somagic SMI2021 USB video bridge
#
# Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
#
#   make                       build build/opensmi21.bin and check it
#   make CROSS=/path/to/avr-   use another toolchain (default: avr- in PATH)
#   make PID=0x003f            USB product id (default 0x003c)
#   make ALT0_STOP=0           alternate setting 0 stops nothing
#   make IRQ=0                 never enable interrupts, poll only
#   make ISR_EVENTS=0          vector 11 only counts and logs; the main
#                              loop polls the events (interrupt test)
#   make ISR_BARE=1            vector 11 is a bare RETI (interrupt test)
#   make ISR_BARE=2            vector 11 counts but reads no register
#   make ISR_BARE=3            vector 11 reads 0x1503 and nothing else
#   make ISR_BARE=4            vector 11 reads 0x1502 and nothing else
#   make ISR_BARE=5            vector 11 notes its return address (0xf8)
#   make ISR_BARE=6            vector 11 reads as the vendor's, no hw stack
#   make ISR_BARE=7            vector 11 does what the vendor's does, no hw stack

CROSS		?= avr-
CC		:= $(CROSS)gcc
OBJCOPY		:= $(CROSS)objcopy
OBJDUMP		:= $(CROSS)objdump

PID		?= 0x003c
ALT0_STOP	?= 1
IRQ		?= 0
ISR_EVENTS	?= 1
ISR_BARE	?= 0

# Largest image known to work, 121 upload units of 62 bytes
MAX_SIZE	:= 7502

B		:= build
SRCS_C		:= main.c usb.c vendor.c test.c
SRCS_S		:= isr.S asm.S
OBJS		:= $(addprefix $(B)/,$(SRCS_S:.S=.o) $(SRCS_C:.c=.o))

# avr-libc's ATmega103: its startup code, headers and delay loops
ARCH		:= -mmcu=atmega103
CFLAGS		:= $(ARCH) -Os -std=gnu99 -Wall -Wextra -Werror=implicit \
		   -ffreestanding -fno-jump-tables -fno-tree-switch-conversion \
		   -ffunction-sections -fdata-sections -fshort-enums \
		   -DFW_PID=$(PID) -DFW_ALT0_STOP=$(ALT0_STOP) -DFW_IRQ=$(IRQ) -DFW_ISR_EVENTS=$(ISR_EVENTS) \
		   -DFW_ISR_BARE=$(ISR_BARE) -DF_CPU=30000000UL -g
ASFLAGS		:= $(ARCH) -DFW_ISR_BARE=$(ISR_BARE) -g
LDFLAGS		:= $(ARCH) -Wl,-T,opensmi21.ld \
		   -Wl,--gc-sections -Wl,-Map,$(B)/opensmi21.map

all: $(B)/opensmi21.bin check

$(B):
	mkdir -p $@

$(B)/%.o: %.c fw.h regs.h Makefile | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/%.o: %.S Makefile | $(B)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(B)/opensmi21.elf: $(OBJS) opensmi21.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

# Plain image from address 0, padded with 0xff to a multiple of 62 bytes
$(B)/opensmi21.bin: $(B)/opensmi21.elf
	$(OBJCOPY) -O binary -j .text -j .data --gap-fill 0xff $< $@.tmp
	@size=$$(stat -c %s $@.tmp); pad=$$(( (62 - size % 62) % 62 )); \
	head -c $$pad /dev/zero | tr '\000' '\377' >> $@.tmp; \
	total=$$((size + pad)); \
	if [ $$total -gt $(MAX_SIZE) ]; then \
		echo "image of $$total bytes exceeds $(MAX_SIZE)"; rm -f $@.tmp; exit 1; \
	fi; \
	mv $@.tmp $@; \
	echo "$@: $$size bytes, $$total padded ($$((total / 62)) upload units)"

$(B)/opensmi21.lst: $(B)/opensmi21.elf
	$(OBJDUMP) -d $< > $@

check: $(B)/opensmi21.lst $(B)/opensmi21.bin
	./check.sh $(B)/opensmi21.lst $(B)/opensmi21.bin

clean:
	rm -rf $(B)

.PHONY: all check clean
