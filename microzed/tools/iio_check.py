#!/usr/bin/env python3
"""Check a recording of the simulated ADC (project 07), made on the board with

    iio_readdev -b 8192 -s 1000000 adcsim > /tmp/adc.bin

and copied to the laptop (scp root@10.0.0.134:/tmp/adc.bin .).

Each sample is a 32-bit little-endian word, as the PL makes it:
    bits 15..0   value, signed 16 bit (what IIO calls the channel: le:s16/32>>0)
    bits 31..16  sequence number, +1 per sample (lost samples leave a gap)

Prints: samples, lost samples (from the sequence numbers), min/max/mean,
the strongest frequency (FFT) and the RMS error against a fitted sine.
--plot shows the first samples (needs matplotlib).

    python3 iio_check.py adc.bin                  rate 1 MSa/s assumed
    python3 iio_check.py adc.bin --rate 5000000
    python3 iio_check.py adc.bin --plot

Needs numpy (sudo apt install python3-numpy python3-matplotlib).
"""
import argparse
import sys

import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file", help="raw recording (32-bit words)")
    ap.add_argument("--rate", type=float, default=1e6, help="sample rate in Sa/s (default 1e6)")
    ap.add_argument("--scale", type=float, default=1000 / 32768,
                    help="mV per LSB (in_voltage0_scale, default 0.030517578)")
    ap.add_argument("--plot", type=int, nargs="?", const=2000, default=0,
                    help="plot the first N samples (default 2000)")
    a = ap.parse_args()

    words = np.fromfile(a.file, dtype="<u4")
    if words.size < 2:
        sys.exit("file too short")
    seq = (words >> 16).astype(np.uint16)
    val = (words & 0xFFFF).astype(np.uint16).view(np.int16).astype(np.int32)

    # Lost samples: each step of the sequence number should be exactly 1.
    # uint16 arithmetic wraps like the PL counter does (65535 -> 0).
    step = np.diff(seq).astype(np.uint16)
    gaps = np.nonzero(step != 1)[0]
    lost = int(np.sum(step[gaps].astype(np.int64) - 1))
    print(f"samples   {words.size}  ({words.size / a.rate:.3f} s at {a.rate:.0f} Sa/s)")
    print(f"lost      {lost} in {gaps.size} gap(s)" + ("" if gaps.size == 0 else
          f", first at sample {gaps[0] + 1} (seq {seq[gaps[0]]} -> {seq[gaps[0] + 1]})"))
    print(f"values    min {val.min()}  max {val.max()}  mean {val.mean():.1f}"
          f"  ({val.min() * a.scale:.1f} .. {val.max() * a.scale:.1f} mV)")

    # Strongest frequency: FFT of the longest gap-free run (a gap breaks the
    # timing), with a window to keep the peak narrow.
    edges = np.concatenate(([0], gaps + 1, [words.size]))
    i = int(np.argmax(np.diff(edges)))
    run = val[edges[i]:edges[i + 1]].astype(float)
    if run.size < 64:
        print("too few gap-free samples for an FFT")
    else:
        run -= run.mean()
        spec = np.abs(np.fft.rfft(run * np.hanning(run.size)))
        k = int(np.argmax(spec[1:])) + 1
        f = k * a.rate / run.size
        n_fft = run.size
        # Least-squares sine at that frequency: how close is the data to it?
        # Only over the first 0.1 s (at least 20 periods): over a long run a
        # frequency error far below the FFT bin adds up to a phase drift and
        # spoils the fit even for a perfect sine.
        run = run[:max(int(0.1 * a.rate), int(20 * a.rate / f))]
        t = np.arange(run.size) / a.rate
        m = np.column_stack((np.sin(2 * np.pi * f * t), np.cos(2 * np.pi * f * t)))
        coef, *_ = np.linalg.lstsq(m, run, rcond=None)
        amp = float(np.hypot(*coef))
        rms = float(np.sqrt(np.mean((run - m @ coef) ** 2)))
        print(f"frequency {f:.1f} Hz (FFT bin {a.rate / n_fft:.2f} Hz, {n_fft} samples)")
        print(f"sine fit  amplitude {amp:.0f} LSB, RMS error {rms:.1f} LSB"
              f"  ({'looks like a sine' if rms < 0.02 * amp else 'not a clean sine (saw mode?)'})")

    if a.plot:
        import matplotlib.pyplot as plt
        n = min(a.plot, val.size)
        t_ms = np.arange(n) / a.rate * 1e3
        plt.plot(t_ms, val[:n] * a.scale)
        plt.xlabel("time (ms)")
        plt.ylabel("mV")
        plt.title(f"{a.file}: first {n} samples")
        plt.grid(True)
        plt.show()

    return 1 if lost else 0


if __name__ == "__main__":
    sys.exit(main())
