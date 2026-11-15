# 03: PL timer with interrupt

## Goal

A timer in the PL that raises an interrupt to the ARM every N milliseconds (N set from a register), handled by our driver, with a blocking `read()` like `/dev/mybutton`.

## Why

Interrupts from the PL are how custom hardware tells Linux "data is ready". This connects project 02 (registers) with the `myled-irq` driver (threaded handler, wait queue).

## Builds on

- 02 (custom AXI IP, bitstream loading, `ioremap`).
- `microzed/modules/myled-irq/` (interrupt handler, wait queue, `poll()`).

## Steps (outline)

1. Extend the AXI IP: a counter with a period register, an enable bit, an interrupt status bit (write 1 to clear), and an interrupt output.
2. In the PS configuration enable the PL-to-PS interrupt (IRQ_F2P) and connect the IP's interrupt to it (via a Concat block if several).
3. Device tree: `interrupts = <0 29 4>` for IRQ_F2P[0] (GIC ID 61 = SPI 29; IRQ_F2P[0..7] are IDs 61-68, [8..15] are 84-91), `interrupt-parent = <&intc>`.
4. Driver: request the IRQ (here a plain top-half handler is fine: clear the status register, count, wake readers), `/dev/pltimer` that blocks until the next tick and returns the tick count, sysfs attribute for the period.
5. Measure: compare tick counts with time (jitter, missed ticks), watch `/proc/interrupts`.

## Done when

- `cat /dev/pltimer` prints a line every N ms, N changeable at run time.
- `/proc/interrupts` shows the count rising at the expected rate.
- Notes in the md: interrupt numbering, top half vs threaded handler choice.

## Pitfalls

- If the handler does not clear the IP's interrupt status, a level interrupt fires forever and the board locks up.
- GIC numbering: device tree SPI number = GIC ID - 32.
- Top-half handlers may not sleep (no mutex, no `msleep`); use a spinlock if state is shared with `read()`.
