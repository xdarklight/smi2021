/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OpenSMI21: memory layout
 *
 * Copyright (C) 2026 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 *
 * Program memory from 0, at most 7502 bytes (121 upload units of 62 bytes,
 * the largest image known to work). SRAM 0x0060-0x0fff, at 0x800060 in the
 * address space of the GNU tools; the stack grows down from 0x0fff.
 */

OUTPUT_FORMAT("elf32-avr", "elf32-avr", "elf32-avr")
OUTPUT_ARCH(avr:31)
ENTRY(__vectors)

MEMORY
{
	text (rx)	: ORIGIN = 0, LENGTH = 7502
	data (rw!x)	: ORIGIN = 0x800060, LENGTH = 0xfa0
}

SECTIONS
{
	/* avr-libc's crt: .vectors, then .init0 to .init9 up to main() */
	.text : {
		KEEP(*(.vectors))
		KEEP(*(.init0))
		KEEP(*(.init1))
		KEEP(*(.init2))
		KEEP(*(.init3))
		KEEP(*(.init4))
		KEEP(*(.init5))
		KEEP(*(.init6))
		KEEP(*(.init7))
		KEEP(*(.init8))
		KEEP(*(.init9))
		*(.progmem*)
		. = ALIGN(2);
		*(.text .text.*)
		KEEP(*(.fini9))
		KEEP(*(.fini8))
		KEEP(*(.fini7))
		KEEP(*(.fini6))
		KEEP(*(.fini5))
		KEEP(*(.fini4))
		KEEP(*(.fini3))
		KEEP(*(.fini2))
		KEEP(*(.fini1))
		KEEP(*(.fini0))
		. = ALIGN(2);
	} > text

	.data : {
		__data_start = .;
		/* the 16 bytes of the info query, at SRAM 0x0060 */
		KEEP(*(.infoblock))
		*(.data .data.*)
		*(.rodata .rodata.*)
		. = ALIGN(2);
		__data_end = .;
	} > data AT > text

	__data_load_start = LOADADDR(.data);
	__image_end = LOADADDR(.data) + SIZEOF(.data);

	.bss (NOLOAD) : {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		__bss_end = .;
	} > data

	.noinit (NOLOAD) : {
		*(.noinit .noinit.*)
		__noinit_end = .;
	} > data

	/* leave at least 1 KiB to the stack */
	ASSERT(__noinit_end <= 0x800c00, "SRAM is full")
	ASSERT(__vectors == 0, "vector table not at 0")

	.stab 0 : { *(.stab) }
	.stabstr 0 : { *(.stabstr) }
	.comment 0 : { *(.comment) }
	.debug_aranges 0 : { *(.debug_aranges) }
	.debug_info 0 : { *(.debug_info .gnu.linkonce.wi.*) }
	.debug_abbrev 0 : { *(.debug_abbrev) }
	.debug_line 0 : { *(.debug_line .debug_line.* .debug_line_end) }
	.debug_frame 0 : { *(.debug_frame) }
	.debug_str 0 : { *(.debug_str) }
	.debug_loc 0 : { *(.debug_loc) }
	.debug_ranges 0 : { *(.debug_ranges) }
}
