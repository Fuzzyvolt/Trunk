# 08: Stream the samples to the laptop over the network

## Goal

Send every sample from the board to the laptop over Ethernet, and receive, check, record and plot it in Python. The web page stays as the quick status view; this is the real data channel of the instrument.

## Why

The web chart shows about 500 of the 1,000,000 samples per second, as snapshots with gaps. Real instruments acquire on the box and record and analyse on a PC. This also measures what the network path can carry (Gigabit Ethernet: 125 MB/s in theory, about 31 MSa/s at 4 bytes per sample).

## Builds on

- 05: `/dev/adcstream`, `adcread`, sequence numbers in every sample.
- 07 (IIO): optional. Read from the IIO buffer if 07 is done, else from `/dev/adcstream`.

## Steps (outline)

1. Board: `adcserve` in `microzed/apps/`: listens on a TCP port, and for each connection sends a small header (rate, block size, format), then every block it reads. Start, stop and settings stay in sysfs (or a tiny command at connect).
2. Laptop: `microzed/host/adcrecv.py`: connects, checks sequence numbers, prints rate and losses, saves to a file (raw or NumPy `.npy`), optional live plot (matplotlib) and FFT.
3. TCP first: every byte arrives in order; a slow network shows up as driver overruns on the board.
4. Then UDP: one block (or part of one) per datagram, no waiting; losses show as sequence gaps on the laptop. Compare with TCP.
5. Measure the highest lossless rate for each, and CPU load on the board (`top`). Try bigger blocks and socket buffer sizes (`SO_SNDBUF`).
6. Optional: `sendfile()`/`splice()` or the mmap mode to avoid copies; an init script to start `adcserve` at boot.

## Done when

- 1 MSa/s recorded on the laptop for several minutes with zero lost samples (sequence check on the laptop).
- The highest lossless rate over TCP and over UDP is measured and written down.
- A recording can be plotted and its FFT shows the sine frequency set on the board.

## Pitfalls

- The laptop must be on Ethernet, not Wi-Fi, for the speed test (Wi-Fi is far slower than the board's Gigabit port).
- Ethernet pause frames (see "Wi-Fi disruption" in `buildroot-microzed-project.md`): heavy streaming may trigger them; watch `ethtool -S eth0 | grep -i pause`.
- UDP datagrams over 1472 bytes are split into IP fragments; losing one fragment loses the whole datagram. Keep datagrams under the MTU or accept it and measure.
- Never expose the port beyond the home network (no authentication).
