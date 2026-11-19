# MicroZed project briefs

Each file here is one self-contained project, sized for one Claude Code session (or a few).
The order matters: each project builds on the ones before it.

| # | Brief | Needs extra hardware? |
|---|---|---|
| 01 | [Vivado setup](01-vivado-setup.md) (done) | No |
| 02 | [AXI register block](02-axi-registers.md) (done) | No |
| 03 | [PL timer with interrupt](03-pl-timer-interrupt.md) (done) | No |
| 04 | [AXI DMA loopback](04-axi-dma-loopback.md) (done) | No |
| 05 | [DMA data stream (simulated ADC)](05-dma-data-stream.md) (done) | No |
| 06 | [Buildroot package for our modules](06-buildroot-package.md) (files written, test on the board) | No |
| 07 | [ADC stream as an IIO device](07-iio-adc-driver.md) | No |
| 08 | [Stream samples to the laptop over the network](08-network-stream.md) | No |

Later, with a MicroHeader breakout or carrier card (all PL pins only reach the MicroHeaders):
PWM outputs (LED dimming, servos), an SPI ADC or pressure sensor sampled by the PL, a FIR filter in the DSP slices.

## Starting a session

Start a new session on this repo and say, for example:

> Read `buildroot-microzed-project.md` and `microzed/projects/04-axi-dma-loopback.md`, then let's do it.

- `buildroot-microzed-project.md` (repo root) is the full background: board, Buildroot setup, repo workflow, lessons learned, current state.
- The brief is the task. Its "Done when" list is the finish line.
- Same working style as before: commands given in chat, one per code block; files Claude writes go in `microzed/` and reach the laptop with `git pull`.
- At the end of the session: "update the md", so the next session knows what happened.

## Brief format

Goal, why it matters, what it builds on, steps (outline only: the session works out details against the real tools and kernel), done when, pitfalls.
