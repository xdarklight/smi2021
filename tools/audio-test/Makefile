# SPDX-License-Identifier: GPL-2.0-or-later
#
# Userspace test for the smi2021 audio ring buffer handling. The code under
# test is extracted verbatim from the driver source, so that the tested code
# is the shipped code.

DRIVER_SRC := ../../smi2021_audio.c

.PHONY: check clean
check: test
	./test

test: test.c audio_extract.h
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -o $@ test.c

audio_extract.h: $(DRIVER_SRC)
	awk '/^static void smi2021_pcm_advance\(/,/^void smi2021_audio\(/' \
		$< | head -n -1 > $@

clean:
	rm -f test audio_extract.h
