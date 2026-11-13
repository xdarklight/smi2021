# SPDX-License-Identifier: GPL-2.0-or-later
#
# Userspace harness for the smi2021 frame parser. The parser is extracted
# verbatim from the driver source so that the tested code is the shipped code.

DRIVER_SRC := ../../smi2021-main.c

.PHONY: check clean
check: test
	./test

test: test.c parser_extract.h
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -o $@ test.c

parser_extract.h: $(DRIVER_SRC)
	awk '/^static struct smi2021_buf \*smi2021_get_buf\(/,/^static void smi2021_process_packet\(/' \
		$< | head -n -1 > $@

clean:
	rm -f test parser_extract.h
