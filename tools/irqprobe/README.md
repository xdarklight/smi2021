# Probe of the interrupt faults

A build of OpenSMI21 and a host script that map how the USB controller's
interrupt request behaves around the events of a control transfer, and when
an interrupt goes through the reset vector instead of vector 11; see
`docs/hardware.md` "Interrupt faults" for what it found.

The build keeps interrupts disabled but for windows of one instruction
(`SEI`, `NOP`, `CLI`); the main loop polls the events as with `IRQ=0`, and
the handler of vector 11 only notes that it ran, and whether it came
through the reset vector, whose check in `isr.S` marks such entries. The
host chooses what to probe by writing variables in SRAM, and reads the
counts back, all through register access requests:

- `probe_addr`, `probe_mask`, `probe_inv`: in the main loop, wait for
  this bit to go from 0 to 1 (after an XOR with `probe_inv`), then open
  the window some cycles later, the delay cycling through 128 steps of
  4 * `probe_step` cycles; `probe_mask` 0 turns this off;
- `probe_in` 1: right after `ep0_tx()` queued a packet, see whether a
  request is pending; 2: open the window at a varying time after that,
  and count by its distance from the moment the host took the packet;
  3: trace 0x1502, 0x151c and 0x1503 after the packet was queued;
- before each window that waits for an event, a first window tells
  whether a request is pending already; those are counted by the values
  of 0x1503, 0x1502, 0x151c and 0x1518 read just before, or, with
  `probe_noread`, without reading them.

Build it from a copy of `fw/`, with the toolchain of `fw/README.md`:

    cp -r fw /tmp/irqprobe-fw
    tools/irqprobe/patch.py /tmp/irqprobe-fw
    make -C /tmp/irqprobe-fw IRQ=1

`patch.py` changes the sources by exact text, so it may need adjusting
when `fw/` changes. Install `build/opensmi21.bin` as the driver's firmware
and replug the device, or upload it with `tools/bridge-probe/probe.py
upload`. Then, with the driver unloaded:

    rmmod smi2021
    PYTHONPATH=tools/bridge-probe tools/irqprobe/irqprobe.py \
        /tmp/irqprobe-fw/build/opensmi21.elf 10000

Each run of the script takes a few minutes. Put the normal firmware back
afterwards; the probe build works with the driver, as it polls, but delays
every control request by up to some 35 µs.
