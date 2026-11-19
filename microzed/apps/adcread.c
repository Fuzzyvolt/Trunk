/*
 * adcread - read the project 05 sample stream from /dev/adcstream
 * (or from the project 07 IIO device, or a file: see -i and -f below).
 *
 * Every second it prints the measured sample rate, min/max/mean of the
 * values, and how many samples were lost. Lost samples are found from the
 * sequence number in each word (bits 31..16, +1 per sample), so this checks
 * the whole chain independently of the driver's own counters.
 *
 *   adcread                  run until Ctrl-C
 *   adcread -t 60            run 60 s, then print a summary
 *   adcread -c /tmp/a.csv    also write the first 100000 samples as CSV (seq,value)
 *   adcread -c f.csv -n 5000 ... only the first 5000
 *   adcread -t 10 -q         only the summary line (used by adcstream-bench)
 *   adcread -m               zero copy: mmap() the DMA buffers, take/return
 *                            blocks with ioctl (see modules/adcstream/adcstream.h)
 *   adcread -s               skim: sequence check only, no min/max/mean, no CSV.
 *                            Shows what the transport alone can do.
 *
 * The same checks on other sources (project 07):
 *   adcread -i               the IIO driver (adciio): sets up and enables the
 *                            buffer of the IIO device named "adcsim", reads
 *                            /dev/iio:deviceN, disables the buffer at the end
 *   adcread -i -b 16384      ... reading 16384 samples at a time (buffer/length
 *                            = twice that, so each DMA block is one read)
 *   adcread -f data.bin      raw 32-bit words from a file, e.g. recorded with
 *                            iio_readdev; -f - reads standard input:
 *                            iio_readdev -b 8192 adcsim | adcread -f -
 * With the IIO driver a slow reader shows up as pl_dropped (the DMA has no
 * free block, the PL FIFO overflows), not as overruns.
 *
 * The four combinations measure where the time goes:
 *   (none)  read() copy + min/max/mean of every sample
 *   -s      read() copy only
 *   -m      every sample read straight from the (uncached) DMA buffer: slow,
 *           each load goes to DDR (6.5 MSa/s max on the board)
 *   -m -s   neither: only the hand-over of blocks (reaches the DMA's 98 MSa/s)
 * read() is the fast way to look at every sample: the kernel copies the
 * uncached buffer in bursts, the loop then runs on cached memory.
 *
 * The sequence number has 16 bits, so a loss of exactly 65536 samples (one
 * whole block of 65536) would not show; the driver's overrun counter, printed
 * next to it, catches that case.
 *
 * Exit status: 0 if no sample was lost, 1 otherwise.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "adcstream.h"

#ifndef DEV
#define DEV   "/dev/adcstream"
#endif
#ifndef SYSFS
#define SYSFS "/sys/bus/platform/devices/43c20000.adcstream/"
#endif
#define IIO_DEVICES "/sys/bus/iio/devices/"
#define IIO_NAME    "adcsim"

static volatile sig_atomic_t stop;

/* Where the driver's sysfs files are; NULL = none (reading a file). */
static const char *sysdir = SYSFS;
static char iio_dir[64];

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* Read a number from a sysfs file, -1 on error. */
static long long sysfs_num(const char *name)
{
	char path[128];
	long long v = -1;
	FILE *f;

	if (!sysdir)
		return -1;
	snprintf(path, sizeof(path), "%s%s", sysdir, name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%lld", &v) != 1)
		v = -1;
	fclose(f);
	return v;
}

/* Write a string to a file in sysdir. 0 on success, -1 on error (message printed). */
static int sysfs_write(const char *name, const char *val)
{
	char path[128];
	FILE *f;
	int ok;

	snprintf(path, sizeof(path), "%s%s", sysdir, name);
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		return -1;
	}
	ok = fputs(val, f) >= 0;
	ok = (fclose(f) == 0) && ok;	/* sysfs reports errors at write/close */
	if (!ok) {
		fprintf(stderr, "%s: cannot write %s: %s\n", path, val, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Find the IIO device named "adcsim": /sys/bus/iio/devices/iio:deviceN/name.
 * Fills iio_dir and dev ("/dev/iio:deviceN"). The XADC is usually device 0,
 * ours device 1, but the number depends on load order: look it up.
 */
static int find_iio(char *dev, size_t devlen)
{
	for (int n = 0; n < 16; n++) {
		char path[96], name[32] = "";
		FILE *f;

		snprintf(path, sizeof(path), IIO_DEVICES "iio:device%d/name", n);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fscanf(f, "%31s", name) != 1)
			name[0] = 0;
		fclose(f);
		if (strcmp(name, IIO_NAME) == 0) {
			snprintf(iio_dir, sizeof(iio_dir), IIO_DEVICES "iio:device%d/", n);
			snprintf(dev, devlen, "/dev/iio:device%d", n);
			return 0;
		}
	}
	return -1;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Read exactly len bytes (one block). Returns len, 0 at end of stream, -1 on error. */
static ssize_t read_block(int fd, void *buf, size_t len)
{
	size_t got = 0;

	while (got < len) {
		ssize_t n = read(fd, (char *)buf + got, len - got);

		if (n < 0)
			return -1;	/* includes EINTR from Ctrl-C */
		if (n == 0)
			return 0;
		got += n;
	}
	return got;
}

int main(int argc, char **argv)
{
	double seconds = 0, t0, t_last, t;
	const char *csv_name = NULL;
	long long csv_left = 100000;
	int quiet = 0, use_mmap = 0, skim = 0, use_iio = 0, opt, fd;
	const char *file_name = NULL;
	long long bs = 0;
	FILE *csv = NULL;
	struct sigaction sa;
	char dev[32] = DEV;
	const char *rate_file = "rate";

	while ((opt = getopt(argc, argv, "t:c:n:qmsib:f:")) != -1) {
		switch (opt) {
		case 't': seconds = atof(optarg); break;
		case 'c': csv_name = optarg; break;
		case 'n': csv_left = atoll(optarg); break;
		case 'q': quiet = 1; break;
		case 'm': use_mmap = 1; break;
		case 's': skim = 1; break;
		case 'i': use_iio = 1; break;
		case 'b': bs = atoll(optarg); break;
		case 'f': file_name = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-t seconds] [-c file.csv [-n samples]] [-q] [-s]\n"
				"         [-m | -i [-b samples] | -f file|- [-b samples]]\n", argv[0]);
			return 2;
		}
	}
	if ((use_iio || file_name) && use_mmap) {
		fprintf(stderr, "-m works with adcstream only\n");
		return 2;
	}
	if (use_iio && file_name) {
		fprintf(stderr, "-i or -f, not both\n");
		return 2;
	}

	if (use_iio) {
		/* The IIO device: rate from its standard attribute, block size our choice. */
		if (find_iio(dev, sizeof(dev)) < 0) {
			fprintf(stderr, "no IIO device named " IIO_NAME ": is adciio loaded?\n");
			return 2;
		}
		sysdir = iio_dir;
		rate_file = "sampling_frequency";
		if (bs <= 0)
			bs = 8192;
	} else if (file_name) {
		sysdir = NULL;		/* no driver counters, no rate */
		if (bs <= 0)
			bs = 8192;
	} else {
		bs = sysfs_num("block_samples");
	}
	long long rate = sysfs_num(rate_file);
	if (bs <= 0 || (!file_name && rate <= 0)) {
		fprintf(stderr, "cannot read " SYSFS "block_samples / rate: is adcstream loaded?\n");
		return 2;
	}
	if (use_iio && bs % 2) {
		fprintf(stderr, "-b must be even\n");
		return 2;
	}
	size_t block_bytes = bs * 4;
	uint32_t *buf = malloc(block_bytes);
	if (!buf)
		return 2;

	if (csv_name && skim) {
		fprintf(stderr, "-c needs every sample: not with -s\n");
		return 2;
	}
	if (csv_name) {
		csv = fopen(csv_name, "w");
		if (!csv) {
			perror(csv_name);
			return 2;
		}
		fprintf(csv, "seq,value\n");
	}

	/* No SA_RESTART: Ctrl-C interrupts the blocking read(). */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (file_name && strcmp(file_name, "-") == 0) {
		fd = 0;			/* standard input */
	} else {
		const char *path = file_name ? file_name : dev;

		fd = open(path, O_RDONLY);
		if (fd < 0) {
			perror(path);
			return 2;
		}
	}

	/*
	 * IIO buffer setup, the same steps libiio does:
	 *   buffer/enable 0                  (stop, in case it ran)
	 *   scan_elements/in_voltage0_en 1   (which channels go into the buffer)
	 *   buffer/length 2 x bs             (the kernel splits it into 2 DMA blocks)
	 *   buffer/enable 1                  (preenable, DMA, postenable: PL starts)
	 */
	if (use_iio) {
		char len[24];

		snprintf(len, sizeof(len), "%lld", 2 * bs);
		sysfs_write("buffer/enable", "0");
		if (sysfs_write("scan_elements/in_voltage0_en", "1") ||
		    sysfs_write("buffer/length", len) ||
		    sysfs_write("buffer/enable", "1"))
			return 2;
	}

	/* mmap mode: map every ring buffer once; ring[i] is DMA buffer i. */
	struct adcstream_info info;
	const uint32_t **ring = NULL;
	if (use_mmap) {
		if (ioctl(fd, ADCSTREAM_IOC_INFO, &info) < 0) {
			perror("ADCSTREAM_IOC_INFO (driver too old for -m?)");
			return 2;
		}
		if (info.block_bytes != block_bytes) {
			fprintf(stderr, "block size changed, run again\n");
			return 2;
		}
		ring = calloc(info.nbuf, sizeof(*ring));
		if (!ring)
			return 2;
		for (unsigned int i = 0; i < info.nbuf; i++) {
			void *p = mmap(NULL, info.buf_size, PROT_READ, MAP_SHARED, fd,
				       (off_t)i * info.buf_size);
			if (p == MAP_FAILED) {
				perror("mmap");
				return 2;
			}
			ring[i] = p;
		}
	}

	long long ovr0 = sysfs_num("overruns"), drop0 = sysfs_num("pl_dropped");
	long long total = 0, lost = 0, blocks = 0;
	/* Per-interval statistics */
	long long n_i = 0, lost_i = 0;
	int vmin = 32767, vmax = -32768;
	int64_t sum = 0;
	int have_seq = 0;
	uint16_t next_seq = 0;

	if (!quiet)
		printf("%lld samples per block, rate %lld Sa/s, %s, %s. Ctrl-C to stop.\n", bs, rate,
		       use_iio ? "IIO read()" : file_name ? "file" :
		       use_mmap ? "mmap (zero copy)" : "read() (copy)",
		       skim ? "skim (no min/max/mean)" : "min/max/mean of every sample");

	t0 = t_last = now();
	while (!stop) {
		const uint32_t *blk;
		__u32 idx = 0;

		if (use_mmap) {
			/* Take the oldest full block: no copy, only its number. */
			if (ioctl(fd, ADCSTREAM_IOC_GET, &idx) < 0) {
				if (errno == ENODATA)
					fprintf(stderr, "end of stream (enable = 0?)\n");
				else if (errno != EINTR)
					perror("ADCSTREAM_IOC_GET");
				break;
			}
			blk = ring[idx];
		} else {
			ssize_t r = read_block(fd, buf, block_bytes);

			if (r <= 0) {
				if (r < 0 && errno != EINTR)
					perror("read");
				if (r == 0)
					fprintf(stderr, file_name ? "end of file\n" : "end of stream (enable = 0?)\n");
				break;
			}
			blk = buf;
		}
		blocks++;

		/*
		 * Lost samples, from the sequence numbers at the two ends of the
		 * block. Numbers only ever move forward (+1 per sample, a jump for
		 * every lost one), so inside a block last - first - (bs - 1) is
		 * exactly the number lost, and first - next_seq the number lost
		 * between blocks. Same answer as checking every sample, without a
		 * compare and branch per sample. (Blind only to a loss of exactly
		 * 65536: the numbers have 16 bits.)
		 */
		uint16_t first = blk[0] >> 16, last = blk[bs - 1] >> 16;

		if (have_seq && first != next_seq)
			lost_i += (uint16_t)(first - next_seq);
		lost_i += (uint16_t)(last - (uint16_t)(first + bs - 1));
		have_seq = 1;
		next_seq = last + 1;

		/*
		 * Min, max, sum of every value. Kept tight: integers only (no
		 * double per sample), min/max as conditional moves (no branch).
		 * An int32 block sum cannot overflow: at most 65536 x 32768 = 2^31.
		 */
		if (!skim) {
			int bmin = 32767, bmax = -32768;
			int32_t bsum = 0;

			for (long long i = 0; i < bs; i++) {
				int v = (int16_t)blk[i];

				bmin = v < bmin ? v : bmin;
				bmax = v > bmax ? v : bmax;
				bsum += v;
			}
			vmin = bmin < vmin ? bmin : vmin;
			vmax = bmax > vmax ? bmax : vmax;
			sum += bsum;
		}

		/* CSV: its own loop, so the fast loop above has no extra test. */
		for (long long i = 0; csv && i < bs; i++) {
			fprintf(csv, "%u,%d\n", blk[i] >> 16, (int16_t)blk[i]);
			if (--csv_left == 0) {
				fclose(csv);
				csv = NULL;
				if (!quiet)
					printf("CSV written: %s\n", csv_name);
			}
		}
		n_i += bs;

		/* Give the block back so the driver can reuse it for the DMA. */
		if (use_mmap && ioctl(fd, ADCSTREAM_IOC_PUT, &idx) < 0) {
			perror("ADCSTREAM_IOC_PUT");
			break;
		}

		t = now();
		if (t - t_last >= 1.0) {
			if (!quiet && skim)
				printf("t=%6.1f s  %9.0f Sa/s  lost %lld  overruns %lld  pl_dropped %lld\n",
				       t - t0, n_i / (t - t_last), lost_i,
				       sysfs_num("overruns") - ovr0, sysfs_num("pl_dropped") - drop0);
			else if (!quiet)
				printf("t=%6.1f s  %9.0f Sa/s  min %6d  max %6d  mean %8.1f  lost %lld  overruns %lld  pl_dropped %lld\n",
				       t - t0, n_i / (t - t_last), vmin, vmax, (double)sum / n_i, lost_i,
				       sysfs_num("overruns") - ovr0, sysfs_num("pl_dropped") - drop0);
			total += n_i;
			lost += lost_i;
			n_i = lost_i = 0;
			vmin = 32767;
			vmax = -32768;
			sum = 0;
			t_last = t;
			fflush(stdout);
		}
		if (seconds > 0 && t - t0 >= seconds)
			break;
	}
	total += n_i;
	lost += lost_i;
	t = now() - t0;
	if (use_mmap)
		for (unsigned int i = 0; i < info.nbuf; i++)
			munmap((void *)ring[i], info.buf_size);
	/* Counters before stopping: the IIO stop floods the PL, which adds drops. */
	long long ovr_end = sysfs_num("overruns") - ovr0;
	long long drop_end = sysfs_num("pl_dropped") - drop0;
	long long rate_end = sysfs_num(rate_file);
	if (use_iio)
		sysfs_write("buffer/enable", "0");	/* DMA stops, PL stops */
	if (fd > 0)
		close(fd);
	if (csv)
		fclose(csv);

	printf("summary seconds=%.1f rate=%lld blocks=%lld samples=%lld lost=%lld overruns=%lld pl_dropped=%lld Sa/s=%.0f MB/s=%.1f\n",
	       t, rate_end, blocks, total, lost, ovr_end, drop_end, total / t, total * 4 / t / 1e6);
	free(buf);
	return lost ? 1 : 0;
}
