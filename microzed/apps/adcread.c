/*
 * adcread - read the project 05 sample stream from /dev/adcstream.
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

#ifndef DEV
#define DEV   "/dev/adcstream"
#endif
#ifndef SYSFS
#define SYSFS "/sys/bus/platform/devices/43c20000.adcstream/"
#endif

static volatile sig_atomic_t stop;

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

	snprintf(path, sizeof(path), SYSFS "%s", name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%lld", &v) != 1)
		v = -1;
	fclose(f);
	return v;
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
	int quiet = 0, opt, fd;
	FILE *csv = NULL;
	struct sigaction sa;

	while ((opt = getopt(argc, argv, "t:c:n:q")) != -1) {
		switch (opt) {
		case 't': seconds = atof(optarg); break;
		case 'c': csv_name = optarg; break;
		case 'n': csv_left = atoll(optarg); break;
		case 'q': quiet = 1; break;
		default:
			fprintf(stderr, "usage: %s [-t seconds] [-c file.csv [-n samples]] [-q]\n", argv[0]);
			return 2;
		}
	}

	long long bs = sysfs_num("block_samples");
	long long rate = sysfs_num("rate");
	if (bs <= 0 || rate <= 0) {
		fprintf(stderr, "cannot read " SYSFS "block_samples / rate: is adcstream loaded?\n");
		return 2;
	}
	size_t block_bytes = bs * 4;
	uint32_t *buf = malloc(block_bytes);
	if (!buf)
		return 2;

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

	fd = open(DEV, O_RDONLY);
	if (fd < 0) {
		perror(DEV);
		return 2;
	}

	long long ovr0 = sysfs_num("overruns"), drop0 = sysfs_num("pl_dropped");
	long long total = 0, lost = 0, blocks = 0;
	/* Per-interval statistics */
	long long n_i = 0, lost_i = 0;
	int vmin = 32767, vmax = -32768;
	double sum = 0;
	int have_seq = 0;
	uint16_t next_seq = 0;

	if (!quiet)
		printf("%lld samples per block, rate %lld Sa/s. Ctrl-C to stop.\n", bs, rate);

	t0 = t_last = now();
	while (!stop) {
		ssize_t r = read_block(fd, buf, block_bytes);

		if (r <= 0) {
			if (r < 0 && errno != EINTR)
				perror("read");
			if (r == 0)
				fprintf(stderr, "end of stream (enable = 0?)\n");
			break;
		}
		blocks++;

		for (long long i = 0; i < bs; i++) {
			uint16_t seq = buf[i] >> 16;
			int16_t v = (int16_t)(buf[i] & 0xffff);

			/* A jump in the sequence = samples lost (PL drop or overrun). */
			if (have_seq && seq != next_seq)
				lost_i += (uint16_t)(seq - next_seq);
			have_seq = 1;
			next_seq = seq + 1;

			if (v < vmin) vmin = v;
			if (v > vmax) vmax = v;
			sum += v;

			if (csv && csv_left > 0) {
				fprintf(csv, "%u,%d\n", seq, v);
				if (--csv_left == 0) {
					fclose(csv);
					csv = NULL;
					if (!quiet)
						printf("CSV written: %s\n", csv_name);
				}
			}
		}
		n_i += bs;

		t = now();
		if (t - t_last >= 1.0) {
			if (!quiet)
				printf("t=%6.1f s  %9.0f Sa/s  min %6d  max %6d  mean %8.1f  lost %lld  overruns %lld  pl_dropped %lld\n",
				       t - t0, n_i / (t - t_last), vmin, vmax, sum / n_i, lost_i,
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
	close(fd);
	if (csv)
		fclose(csv);

	printf("summary seconds=%.1f rate=%lld blocks=%lld samples=%lld lost=%lld overruns=%lld pl_dropped=%lld Sa/s=%.0f MB/s=%.1f\n",
	       t, sysfs_num("rate"), blocks, total, lost, sysfs_num("overruns") - ovr0,
	       sysfs_num("pl_dropped") - drop0, total / t, total * 4 / t / 1e6);
	free(buf);
	return lost ? 1 : 0;
}
