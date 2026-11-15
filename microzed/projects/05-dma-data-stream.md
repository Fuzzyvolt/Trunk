# 05: DMA data stream (simulated ADC)

## Goal

The PL generates a continuous data stream (counter, or a sine wave from a lookup table) at a fixed sample rate set by a register, the DMA writes it into DDR, a driver hands blocks to user space, and the web page shows a live chart.

## Why

This is the shape of a real instrument: hardware samples at an exact rate, DMA moves the data, Linux processes and serves it. Later a real ADC on a breakout replaces the generator with almost no software change.

## Builds on

- 04 (AXI DMA, DMA engine client driver).
- 03 (registers for rate control).
- The web page (`microzed/overlay/srv/www/cgi-bin/`).

## Steps (outline)

1. PL: an AXI4-Stream generator with TLAST every N samples, sample rate from a register, feeding AXI DMA S2MM.
2. Driver: a ring of DMA buffers, continuous transfers, `read()` returns full blocks, `poll()` support, an overrun counter in sysfs. Optional: expose it as an IIO buffered device instead.
3. User space: a small C program that reads blocks and computes min/max/mean, or writes a CSV.
4. Web page: a chart of the latest block (simple SVG or a small JavaScript chart library).
5. Measure the highest sample rate without overruns.

## Done when

- A steady stream at a chosen rate (for example 1 MSa/s) with zero overruns over several minutes.
- The web page shows a live waveform.

## Pitfalls

- Avoid copying large blocks through `read()` at high rates; `mmap()` of the DMA buffers is the next step if the CPU becomes the limit.
- The web page refresh is not real time; decimate the data before sending it to the browser.
