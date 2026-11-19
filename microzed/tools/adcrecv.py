#!/usr/bin/env python3
"""Receive the sample stream from adcserve on the board (project 08).

    board: /dev/adcstream --> adcserve --TCP/UDP port 5000--> adcrecv.py (here)

Checks every sample's sequence number (bits 31..16, +1 per sample), prints
rate and losses once per second, optionally records the raw words to a file
and shows a live plot with an FFT.

    python3 adcrecv.py                        TCP from 10.0.0.134, until Ctrl-C
    python3 adcrecv.py -t 300                 5 minutes, then a summary
    python3 adcrecv.py -t 10 -o rec.bin       record 10 s (raw 32-bit words)
    python3 adcrecv.py --plot                 live plot + FFT (needs matplotlib)
    python3 adcrecv.py --udp                  UDP instead (board: adcserve -u)
    python3 adcrecv.py --host 10.0.0.135 --port 6000
    python3 adcrecv.py -t 10 -q               summary line only (adcnet-bench)

A recording is the same format as iio_readdev's, so iio_check.py reads it:

    python3 iio_check.py rec.bin --rate 1000000 --plot

Exit status: 0 if nothing was lost, 1 otherwise.

Needs numpy (sudo apt install python3-numpy python3-matplotlib).
"""
import argparse
import signal
import socket
import sys
import threading
import time

import numpy as np

DGRAM_MAGIC = 0x31445A4D  # b"MZD1" read as a little-endian u32
DGRAM_HDR = 12            # magic, datagram number, sample count (3 x u32)
PLOT_SAMPLES = 1 << 16    # samples kept for the live plot and its FFT


class Checker:
    """Sequence check and statistics over a stream of 32-bit sample words."""

    def __init__(self):
        self.next_seq = None      # sequence number expected next
        self.samples = 0
        self.lost = 0
        self.gaps = 0
        self.vmin, self.vmax = 32767, -32768
        self.latest = np.zeros(0, dtype=np.int16)  # newest values, for the plot
        self.lock = threading.Lock()

    def feed(self, words):
        if words.size == 0:
            return
        seq = (words >> 16).astype(np.uint16)
        val = (words & 0xFFFF).astype(np.uint16).view(np.int16)
        # Each step of the 16-bit sequence number should be exactly 1
        # (uint16 arithmetic wraps 65535 -> 0 like the PL's counter). A step
        # of k means k - 1 samples lost. Also check the step from the end of
        # the previous chunk to the start of this one.
        if self.next_seq is not None:
            first = (int(seq[0]) - self.next_seq) & 0xFFFF
            if first:
                self.lost += int(first)
                self.gaps += 1
        step = np.diff(seq) - np.uint16(1)      # 0 where nothing was lost
        bad = np.nonzero(step)[0]
        if bad.size:
            self.lost += int(step[bad].astype(np.int64).sum())
            self.gaps += bad.size
        self.next_seq = (int(seq[-1]) + 1) & 0xFFFF
        self.samples += words.size
        self.vmin = min(self.vmin, int(val.min()))
        self.vmax = max(self.vmax, int(val.max()))
        with self.lock:
            self.latest = np.concatenate((self.latest, val))[-PLOT_SAMPLES:]


def parse_header(line):
    """'ADCSERVE 1 proto=tcp rate=1000000 ...' -> dict."""
    if line.startswith("ADCSERVE error"):
        sys.exit(f"board: {line.strip()}")
    if not line.startswith("ADCSERVE "):
        sys.exit(f"not an adcserve header: {line!r}")
    return dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)


class Receiver:
    def __init__(self, a):
        self.a = a
        self.chk = Checker()
        self.out = open(a.output, "wb") if a.output else None
        self.stop = False
        self.info = {}
        self.dgrams = 0           # UDP: datagrams received
        self.dg_lost = 0          # UDP: gaps in the datagram numbers
        self.dg_odd = 0           # UDP: out of order / duplicates / bad magic
        self.next_dg = None
        self.t0 = None

    # ---- TCP -----------------------------------------------------------
    def run_tcp(self):
        a = self.a
        s = socket.create_connection((a.host, a.port), timeout=5)
        # TCP: leave the receive buffer to Linux (it grows it by itself);
        # setting SO_RCVBUF turns that autotuning off.
        if a.rcvbuf:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, a.rcvbuf)
        line = b""
        while not line.endswith(b"\n") and len(line) < 300:
            c = s.recv(1)
            if not c:
                sys.exit("connection closed before the header")
            line += c
        self.start(parse_header(line.decode(errors="replace")))
        s.settimeout(1.0)
        buf = bytearray(4 << 20)
        mv = memoryview(buf)
        fill = 0                  # bytes waiting in buf (an incomplete word)
        while not self.stop:
            try:
                n = s.recv_into(mv[fill:])
            except socket.timeout:
                self.tick()
                continue
            if n == 0:
                print("board closed the connection (stream stopped?)")
                break
            fill += n
            whole = fill - fill % 4
            words = np.frombuffer(buf, dtype="<u4", count=whole // 4)
            self.chk.feed(words)
            if self.out:
                self.out.write(mv[:whole])
            del words             # release the view before buf is changed
            buf[:fill - whole] = buf[whole:fill]
            fill -= whole
            self.tick()
        s.close()

    # ---- UDP -----------------------------------------------------------
    def run_udp(self):
        a = self.a
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # UDP: a big receive buffer holds datagrams while Python is busy;
        # when it is full, new datagrams are dropped (not counted anywhere
        # but in our gaps, and in "netstat -su": receive buffer errors).
        want = a.rcvbuf or 8 << 20
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, want)
        got = s.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF) // 2  # Linux reports double
        if got < want:
            print(f"warning: receive buffer {got} bytes, asked {want}: the kernel caps it at\n"
                  f"  net.core.rmem_max. Raise it with: sudo sysctl -w net.core.rmem_max={want}")
        board = (a.host, a.port)
        s.settimeout(0.5)
        # "start" until the header comes back (a datagram can get lost too).
        info = None
        for _ in range(10):
            s.sendto(b"start", board)
            try:
                while True:
                    d = s.recv(65536)
                    if d.startswith(b"ADCSERVE"):
                        info = parse_header(d.decode(errors="replace"))
                        break
            except socket.timeout:
                continue
            break
        if info is None:
            sys.exit(f"no answer from {a.host}:{a.port}: is 'adcserve -u' running on the board?")
        self.start(info)

        ds = int(info.get("dgram", 365))
        slot = DGRAM_HDR + 4 * ds               # bytes per datagram, at most
        nslot = max(16, (2 << 20) // slot)      # datagrams per batch, ~2 MB
        big = bytearray(nslot * slot)
        mv = memoryview(big)
        lens = np.zeros(nslot, dtype=np.int64)
        last_keep = time.monotonic()
        s.settimeout(0.2)
        try:
            while not self.stop:
                # Receive a batch: each datagram into its own slot of big.
                k = 0
                while k < nslot and not self.stop:
                    try:
                        lens[k] = s.recv_into(mv[k * slot:(k + 1) * slot])
                    except socket.timeout:
                        break
                    k += 1
                    if k % 256 == 0 and time.monotonic() - last_keep >= 1.0:
                        break     # time for "keep" and the per-second line
                if k:
                    self.udp_batch(big, lens[:k], slot, ds)
                if time.monotonic() - last_keep >= 1.0:
                    s.sendto(b"keep", board)
                    last_keep = time.monotonic()
                self.tick()
        finally:
            s.sendto(b"stop", board)
            s.close()

    def udp_batch(self, big, lens, slot, ds):
        """Check k datagrams at once with numpy (a Python loop per datagram is slow)."""
        k = lens.size
        rows = np.frombuffer(big, dtype="<u4", count=k * slot // 4).reshape(k, slot // 4)
        good = (rows[:, 0] == DGRAM_MAGIC) & (lens >= DGRAM_HDR) & \
               (rows[:, 2] <= ds) & (lens == DGRAM_HDR + 4 * rows[:, 2])
        if not good.all():
            self.dg_odd += int((~good).sum())   # header from a restart, or junk
            rows = rows[good]
            if rows.shape[0] == 0:
                return
        dseq = rows[:, 1]
        count = rows[:, 2]
        # Lost datagrams: gaps in their numbers (u32, never wraps in practice).
        nums = dseq.astype(np.int64)
        if self.next_dg is not None:
            nums = np.concatenate(([self.next_dg - 1], nums))
        step = np.diff(nums)
        self.dg_lost += int(step[step > 1].sum() - (step > 1).sum())
        self.dg_odd += int((step < 1).sum())
        self.next_dg = int(dseq[-1]) + 1
        self.dgrams += rows.shape[0]
        # The samples of all datagrams, in order: the first count[i] words
        # after each header (the last datagram of a block is shorter).
        data = rows[:, 3:3 + ds]
        words = data[np.arange(ds) < count[:, None]]
        self.chk.feed(words)
        if self.out:
            words.astype("<u4").tofile(self.out)

    # ---- Both ----------------------------------------------------------
    def start(self, info):
        self.info = info
        self.rate = float(info.get("rate", 1e6))
        print(f"{info.get('proto', '?').upper()} from {self.a.host}:{self.a.port}: "
              f"rate {info.get('rate')} Sa/s, block {info.get('block')}, "
              f"signal {info.get('freq')} Hz {info.get('mode')}"
              + (f", {info.get('dgram')} samples per datagram" if info.get("proto") == "udp" else "")
              + (f", recording to {self.a.output}" if self.out else "") + ". Ctrl-C to stop.")
        self.t0 = self.t_last = time.monotonic()
        self.n_last = self.lost_last = self.dg_last = 0

    def tick(self):
        t = time.monotonic()
        c = self.chk
        if t - self.t_last >= 1.0:
            dt = t - self.t_last
            n = c.samples - self.n_last
            if not self.a.quiet:
                line = (f"t={t - self.t0:6.1f} s  {n / dt:10.0f} Sa/s  {n * 4 / dt / 1e6:6.1f} MB/s  "
                        f"lost {c.lost - self.lost_last}  (total {c.lost})")
                if self.info.get("proto") == "udp":
                    line += f"  datagrams lost {self.dg_lost - self.dg_last}"
                print(line, flush=True)
            self.t_last, self.n_last, self.lost_last, self.dg_last = t, c.samples, c.lost, self.dg_lost
        if self.a.time and t - self.t0 >= self.a.time:
            self.stop = True

    def summary(self):
        c = self.chk
        t = max(time.monotonic() - (self.t0 or time.monotonic()), 1e-9)
        line = (f"summary proto={self.info.get('proto')} seconds={t:.1f} rate={self.info.get('rate')} "
                f"samples={c.samples} lost={c.lost} gaps={c.gaps} Sa/s={c.samples / t:.0f} "
                f"MB/s={c.samples * 4 / t / 1e6:.1f}")
        if self.info.get("proto") == "udp":
            line += f" datagrams={self.dgrams} dgram_lost={self.dg_lost} dgram_odd={self.dg_odd}"
        print(line)
        if c.samples:
            print(f"values min {c.vmin} max {c.vmax}")
        if self.out:
            self.out.close()
            print(f"recorded {self.a.output}: check and plot with\n"
                  f"  python3 iio_check.py {self.a.output} --rate {self.info.get('rate')} --plot")


def live_plot(rx):
    """Main thread: redraw the newest samples and their FFT 5 times a second."""
    import matplotlib.pyplot as plt

    fig, (ax_t, ax_f) = plt.subplots(2, 1, figsize=(9, 7))
    fig.canvas.manager.set_window_title("adcrecv")
    line_t, = ax_t.plot([], [])
    line_f, = ax_f.semilogy([], [])
    ax_t.set_xlabel("time (ms)")
    ax_t.set_ylabel("value (LSB)")
    ax_t.set_ylim(-34000, 34000)
    ax_t.grid(True)
    ax_f.set_xlabel("frequency (Hz)")
    ax_f.set_ylabel("magnitude")
    ax_f.grid(True)
    plt.tight_layout()
    plt.show(block=False)
    while not rx.stop and plt.fignum_exists(fig.number):
        with rx.chk.lock:
            v = rx.chk.latest.astype(float)
        if v.size >= 1024 and rx.t0:
            rate = rx.rate
            n_show = min(v.size, 2000)
            t_ms = np.arange(n_show) / rate * 1e3
            line_t.set_data(t_ms, v[-n_show:])
            ax_t.set_xlim(0, t_ms[-1])
            v -= v.mean()
            spec = np.abs(np.fft.rfft(v * np.hanning(v.size))) + 1e-3
            freqs = np.fft.rfftfreq(v.size, 1 / rate)
            line_f.set_data(freqs, spec)
            ax_f.set_xlim(0, freqs[-1])
            ax_f.set_ylim(spec.max() * 1e-7, spec.max() * 3)
            peak = freqs[int(np.argmax(spec[1:])) + 1]
            ax_f.set_title(f"FFT of the last {v.size} samples: peak {peak:.1f} Hz "
                           f"(bin {rate / v.size:.1f} Hz)   lost so far {rx.chk.lost}")
        plt.pause(0.2)
    rx.stop = True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="10.0.0.134", help="board address (default 10.0.0.134)")
    ap.add_argument("--port", type=int, default=5000, help="port (default 5000)")
    ap.add_argument("--udp", action="store_true", help="UDP (board: adcserve -u)")
    ap.add_argument("-t", "--time", type=float, default=0, help="seconds, then stop (default: Ctrl-C)")
    ap.add_argument("-o", "--output", help="record raw 32-bit words to this file")
    ap.add_argument("-q", "--quiet", action="store_true", help="summary line only")
    ap.add_argument("--plot", action="store_true", help="live plot + FFT (matplotlib)")
    ap.add_argument("--rcvbuf", type=int, default=0,
                    help="socket receive buffer, bytes (default: UDP 8 MiB, TCP automatic)")
    a = ap.parse_args()

    rx = Receiver(a)
    run = rx.run_udp if a.udp else rx.run_tcp

    def on_signal(sig, frame):
        rx.stop = True
    signal.signal(signal.SIGINT, on_signal)

    try:
        if a.plot:
            # matplotlib must run in the main thread: receive in a second one.
            th = threading.Thread(target=run, daemon=True)
            th.start()
            live_plot(rx)
            th.join(timeout=3)
        else:
            run()
    except (ConnectionError, socket.timeout, OSError) as e:
        print(f"{a.host}:{a.port}: {e}" + ("" if a.udp else
              " (is adcserve running on the board?)"))
        if rx.t0 is None:
            return 2
    rx.summary()
    return 1 if rx.chk.lost or rx.dg_lost else 0


if __name__ == "__main__":
    sys.exit(main())
