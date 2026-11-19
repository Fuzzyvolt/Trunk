/*
 * adcserve - send the project 05 sample stream to the laptop over the
 * network (project 08). The receiver is tools/adcrecv.py.
 *
 *   /dev/adcstream --> adcserve --TCP or UDP port 5000--> adcrecv.py (laptop)
 *
 *   adcserve                 TCP: wait for a connection, send every block
 *   adcserve -u              UDP: wait for "start", send datagrams
 *   adcserve -p 6000         another port
 *   adcserve -m              zero copy from the DMA buffers (mmap + ioctl):
 *                            one copy less (DMA buffer -> socket directly)
 *   adcserve -S 1048576      socket send buffer size (SO_SNDBUF), bytes
 *   adcserve -u -d 1024      UDP: samples per datagram (default 365: one
 *                            Ethernet frame; more = IP fragments)
 *   adcserve -q              no per-second lines, only one per session
 *
 * Start, stop, rate, frequency and block size stay in sysfs
 * (/sys/bus/platform/devices/43c20000.adcstream/). /dev/adcstream is opened
 * only while a laptop is connected, so adcread works the rest of the time.
 * One laptop at a time.
 *
 * TCP: on connect the server sends one text line, then raw sample words
 * until the laptop disconnects or the stream stops:
 *
 *   ADCSERVE 1 proto=tcp rate=1000000 block=4096 freq=1000 mode=sine dgram=0 format=u32le\n
 *   <32-bit little-endian words: bits 31..16 sequence number, 15..0 value>
 *
 * TCP never loses a byte. If the network is too slow, send() waits, the
 * driver's ring fills and the driver drops whole blocks: "overruns" here,
 * sequence gaps on the laptop.
 *
 * UDP: the laptop sends "start" to the port; the server answers with the same
 * text line (proto=udp) as one datagram, then sends every block as datagrams:
 *
 *   magic "MZD1" (4 bytes) | datagram number (u32, from 0) | sample count (u32) | samples
 *
 * The laptop sends "keep" every second; without one for 3 s the server
 * stops sending (laptop gone). "stop" ends at once. Nothing is resent: a
 * lost datagram is a gap in the datagram numbers and in the sequence numbers.
 *
 * Every second (unless -q): samples/s, MB/s, the driver's overruns and PL
 * drops, CPU load of the whole board (top's view: 50% = one of the two
 * cores busy), and for UDP datagrams the board itself could not send.
 *
 * Never open this port to the internet: there is no authentication.
 */
#define _GNU_SOURCE		/* sendmmsg() */
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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "adcstream.h"

#ifndef DEV
#define DEV   "/dev/adcstream"
#endif
#ifndef SYSFS
#define SYSFS "/sys/bus/platform/devices/43c20000.adcstream/"
#endif

#define DEF_PORT	5000
/* 1500 (Ethernet MTU) - 20 (IP header) - 8 (UDP header) = 1472 = 12 + 365 x 4 */
#define DEF_DGRAM	365
#define MAX_DGRAM	16373	/* 12 + 16373 x 4 = 65504, under UDP's 65507 limit */
#define DGRAM_MAGIC	0x31445A4D	/* bytes 'M' 'Z' 'D' '1' in memory (little-endian) */
#define BATCH		64	/* datagrams per sendmmsg() call */
#define KEEP_TIMEOUT	3.0	/* s without "keep" from the laptop: stop sending */

/* Header in front of every UDP data datagram. Little-endian, like the ARM. */
struct dgram_hdr {
	uint32_t magic;
	uint32_t seq;		/* datagram number, +1 per datagram, 0 after "start" */
	uint32_t count;		/* samples in this datagram */
};

static volatile sig_atomic_t stop;
static int quiet, use_mmap, dgram_samples = DEF_DGRAM, sndbuf;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Read a sysfs file of the driver into buf (first word only). 0 or -1. */
static int sysfs_str(const char *name, char *buf, size_t len)
{
	char path[128];
	FILE *f;
	int ok;

	snprintf(path, sizeof(path), SYSFS "%s", name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	ok = fscanf(f, "%31s", buf) == 1 && len > 31;
	fclose(f);
	return ok ? 0 : -1;
}

static long long sysfs_num(const char *name)
{
	char s[32];

	return sysfs_str(name, s, sizeof(s)) ? -1 : atoll(s);
}

/*
 * CPU time of the whole board from /proc/stat (first line: all cores
 * together). busy = everything except idle and iowait. The network stack
 * works partly in interrupts and softirqs, which are not charged to this
 * process, so the whole board is the honest number.
 */
static void cpu_read(unsigned long long *busy, unsigned long long *total)
{
	unsigned long long v[8] = { 0 };
	FILE *f = fopen("/proc/stat", "r");

	*busy = *total = 0;
	if (!f)
		return;
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
		for (int i = 0; i < 8; i++)
			*total += v[i];
		*busy = *total - v[3] - v[4];
	}
	fclose(f);
}

/* ---- The sample source: /dev/adcstream, with read() or mmap ---------- */

struct src {
	int fd;
	long bs;			/* samples per block */
	size_t block_bytes;
	uint32_t *buf;			/* read() mode: the copy */
	const uint32_t **ring;		/* mmap mode: ring[i] = DMA buffer i */
	struct adcstream_info info;
};

static void src_close(struct src *s)
{
	if (s->ring)
		for (unsigned int i = 0; i < s->info.nbuf; i++)
			if (s->ring[i])
				munmap((void *)s->ring[i], s->info.buf_size);
	free(s->ring);
	free(s->buf);
	if (s->fd >= 0)
		close(s->fd);
	memset(s, 0, sizeof(*s));
	s->fd = -1;
}

/* Open the device for one session. 0, or -1 with a message. */
static int src_open(struct src *s)
{
	memset(s, 0, sizeof(*s));
	s->bs = sysfs_num("block_samples");
	if (s->bs <= 0) {
		fprintf(stderr, "cannot read " SYSFS "block_samples: is adcstream loaded?\n");
		s->fd = -1;
		return -1;
	}
	s->block_bytes = s->bs * 4;
	s->fd = open(DEV, O_RDONLY);
	if (s->fd < 0) {
		perror(DEV " (adcread running?)");
		return -1;
	}
	if (!use_mmap) {
		s->buf = malloc(s->block_bytes);
		if (!s->buf)
			goto fail;
		return 0;
	}
	/* Zero copy: map every DMA buffer once, as adcread -m does. */
	if (ioctl(s->fd, ADCSTREAM_IOC_INFO, &s->info) < 0) {
		perror("ADCSTREAM_IOC_INFO");
		goto fail;
	}
	if (s->info.block_bytes != s->block_bytes) {
		fprintf(stderr, "block size changed, try again\n");
		goto fail;
	}
	s->ring = calloc(s->info.nbuf, sizeof(*s->ring));
	if (!s->ring)
		goto fail;
	for (unsigned int i = 0; i < s->info.nbuf; i++) {
		void *p = mmap(NULL, s->info.buf_size, PROT_READ, MAP_SHARED, s->fd,
			       (off_t)i * s->info.buf_size);
		if (p == MAP_FAILED) {
			perror("mmap");
			goto fail;
		}
		s->ring[i] = p;
	}
	return 0;
fail:
	src_close(s);
	return -1;
}

/*
 * Next full block, waiting for it. NULL at the end of the stream
 * (enable = 0), on error or Ctrl-C. In mmap mode the block is held until
 * src_put().
 */
static const uint32_t *src_get(struct src *s, __u32 *idx)
{
	if (use_mmap) {
		if (ioctl(s->fd, ADCSTREAM_IOC_GET, idx) < 0) {
			if (errno == ENODATA)
				fprintf(stderr, "end of stream (enable = 0?)\n");
			else if (errno != EINTR)
				perror("ADCSTREAM_IOC_GET");
			return NULL;
		}
		return s->ring[*idx];
	}
	for (size_t got = 0; got < s->block_bytes; ) {
		ssize_t n = read(s->fd, (char *)s->buf + got, s->block_bytes - got);

		if (n <= 0) {
			if (n == 0)
				fprintf(stderr, "end of stream (enable = 0?)\n");
			else if (errno != EINTR)
				perror("read");
			return NULL;
		}
		got += n;
	}
	return s->buf;
}

/* Give an mmap block back to the driver (nothing to do in read() mode). */
static void src_put(struct src *s, __u32 idx)
{
	if (use_mmap && ioctl(s->fd, ADCSTREAM_IOC_PUT, &idx) < 0)
		perror("ADCSTREAM_IOC_PUT");
}

/* The text line sent first in every session. */
static void make_header(char *h, size_t len, const char *proto, long bs)
{
	char mode[32] = "?";

	sysfs_str("mode", mode, sizeof(mode));
	snprintf(h, len, "ADCSERVE 1 proto=%s rate=%lld block=%ld freq=%lld mode=%s dgram=%d format=u32le\n",
		 proto, sysfs_num("rate"), bs, sysfs_num("freq"), mode,
		 strcmp(proto, "udp") == 0 ? dgram_samples : 0);
}

/* ---- Statistics, printed once per second ----------------------------- */

struct stats {
	double t0, t_last;
	long long samples, samples_i, ovr0, drop0, ovr_i, drop_i;
	long long dg_sent, dg_failed;
	unsigned long long busy0, total0, busy_i, total_i;
};

static void stats_start(struct stats *st)
{
	memset(st, 0, sizeof(*st));
	st->t0 = st->t_last = now();
	st->ovr0 = st->ovr_i = sysfs_num("overruns");
	st->drop0 = st->drop_i = sysfs_num("pl_dropped");
	cpu_read(&st->busy0, &st->total0);
	st->busy_i = st->busy0;
	st->total_i = st->total0;
}

static double cpu_pct(unsigned long long b0, unsigned long long t0)
{
	unsigned long long b, t;

	cpu_read(&b, &t);
	return t > t0 ? 100.0 * (b - b0) / (t - t0) : 0;
}

static void stats_add(struct stats *st, long samples)
{
	double t = now(), dt = t - st->t_last;

	st->samples += samples;
	st->samples_i += samples;
	if (dt < 1.0)
		return;
	if (!quiet) {
		long long ovr = sysfs_num("overruns"), drop = sysfs_num("pl_dropped");

		printf("t=%6.1f s  %9.0f Sa/s  %6.1f MB/s  overruns %lld  pl_dropped %lld  cpu %3.0f%%",
		       t - st->t0, st->samples_i / dt, st->samples_i * 4 / dt / 1e6,
		       ovr - st->ovr_i, drop - st->drop_i, cpu_pct(st->busy_i, st->total_i));
		if (st->dg_sent || st->dg_failed)
			printf("  datagrams %lld  not sent %lld", st->dg_sent, st->dg_failed);
		printf("\n");
		fflush(stdout);
		st->ovr_i = ovr;
		st->drop_i = drop;
		cpu_read(&st->busy_i, &st->total_i);
	}
	st->samples_i = 0;
	st->t_last = t;
}

static void stats_end(struct stats *st)
{
	double t = now() - st->t0;

	if (t <= 0)
		t = 1e-9;
	printf("session seconds=%.1f samples=%lld Sa/s=%.0f MB/s=%.1f overruns=%lld pl_dropped=%lld cpu=%.0f%%",
	       t, st->samples, st->samples / t, st->samples * 4 / t / 1e6,
	       sysfs_num("overruns") - st->ovr0, sysfs_num("pl_dropped") - st->drop0,
	       cpu_pct(st->busy0, st->total0));
	if (st->dg_sent || st->dg_failed)
		printf(" datagrams=%lld not_sent=%lld", st->dg_sent, st->dg_failed);
	printf("\n");
	fflush(stdout);
}

/* ---- TCP ------------------------------------------------------------- */

/* Send all len bytes. send() may take only part: loop. 0, or -1. */
static int send_all(int c, const void *p, size_t len)
{
	while (len) {
		/* MSG_NOSIGNAL: a closed connection gives EPIPE, not SIGPIPE. */
		ssize_t n = send(c, p, len, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR && !stop)
				continue;
			if (errno != EINTR)
				fprintf(stderr, "send: %s (laptop disconnected?)\n", strerror(errno));
			return -1;
		}
		p = (const char *)p + n;
		len -= n;
	}
	return 0;
}

static int listen_socket(int type, int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
				 .sin_addr.s_addr = htonl(INADDR_ANY) };
	int one = 1, s = socket(AF_INET, type, 0);

	if (s < 0) {
		perror("socket");
		return -1;
	}
	/* Restart at once after Ctrl-C (no "Address already in use"). */
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		perror("bind");
		close(s);
		return -1;
	}
	if (type == SOCK_STREAM && listen(s, 1) < 0) {
		perror("listen");
		close(s);
		return -1;
	}
	return s;
}

/* Set SO_SNDBUF if -S was given and print what the kernel made of it. */
static void set_sndbuf(int s)
{
	int v;
	socklen_t l = sizeof(v);

	if (sndbuf > 0 && setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0)
		perror("SO_SNDBUF");
	/* Linux doubles the value (bookkeeping) and caps it at net.core.wmem_max. */
	if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, &l) == 0)
		printf("socket send buffer %d bytes\n", v);
}

static int serve_tcp(int port)
{
	int ls = listen_socket(SOCK_STREAM, port);

	if (ls < 0)
		return 2;
	printf("TCP port %d: waiting for the laptop (tools/adcrecv.py). Ctrl-C to stop.\n", port);
	fflush(stdout);

	while (!stop) {
		struct sockaddr_in peer;
		socklen_t pl = sizeof(peer);
		struct src s;
		struct stats st;
		char h[200];
		int c = accept(ls, (struct sockaddr *)&peer, &pl);

		if (c < 0) {
			if (errno != EINTR)
				perror("accept");
			continue;
		}
		printf("connection from %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
		set_sndbuf(c);
		if (src_open(&s) < 0) {
			static const char err[] = "ADCSERVE error: cannot open " DEV "\n";

			send_all(c, err, sizeof(err) - 1);
			close(c);
			continue;
		}
		make_header(h, sizeof(h), "tcp", s.bs);
		fputs(h, stdout);
		if (send_all(c, h, strlen(h)) == 0) {
			stats_start(&st);
			for (;;) {
				__u32 idx = 0;
				const uint32_t *blk = src_get(&s, &idx);
				int r;

				if (!blk)
					break;
				r = send_all(c, blk, s.block_bytes);
				src_put(&s, idx);
				if (r < 0)
					break;
				stats_add(&st, s.bs);
			}
			stats_end(&st);
		}
		src_close(&s);
		close(c);
		printf("connection closed\n");
	}
	close(ls);
	return 0;
}

/* ---- UDP ------------------------------------------------------------- */

/*
 * Send one block as datagrams of up to dgram_samples samples, BATCH
 * datagrams per sendmmsg() call (one system call for many datagrams). Each
 * datagram is two pieces (iovec): its header and a slice of the block, so
 * the samples are not copied into a separate buffer first.
 * A datagram the kernel refuses (ENOBUFS: the network driver's queue is
 * full) is skipped and counted: no waiting, like a real ADC.
 * 0, or -1 on a real error or Ctrl-C.
 */
static int send_block_udp(int s, const struct sockaddr_in *to, const uint32_t *blk,
			  long bs, uint32_t *dseq, struct stats *st)
{
	struct dgram_hdr hdr[BATCH];
	struct iovec iov[2 * BATCH];
	struct mmsghdr msg[BATCH];
	long done = 0;

	while (done < bs) {
		long off = done;
		int k = 0;

		/* Build up to BATCH datagrams from sample "done" on. */
		for (; k < BATCH && off < bs; k++) {
			uint32_t n = bs - off < dgram_samples ? bs - off : dgram_samples;

			hdr[k] = (struct dgram_hdr){ DGRAM_MAGIC, *dseq + k, n };
			iov[2 * k] = (struct iovec){ &hdr[k], sizeof(hdr[k]) };
			iov[2 * k + 1] = (struct iovec){ (void *)(blk + off), n * 4 };
			memset(&msg[k], 0, sizeof(msg[k]));
			msg[k].msg_hdr.msg_name = (void *)to;
			msg[k].msg_hdr.msg_namelen = sizeof(*to);
			msg[k].msg_hdr.msg_iov = &iov[2 * k];
			msg[k].msg_hdr.msg_iovlen = 2;
			off += n;
		}

		int r = sendmmsg(s, msg, k, 0);

		if (r < 0) {
			if (errno == EINTR && !stop)
				continue;
			if (errno == ENOBUFS || errno == EAGAIN) {
				/* The first one of the batch failed: skip it. */
				done += hdr[0].count;
				(*dseq)++;
				st->dg_failed++;
				continue;
			}
			if (errno != EINTR)
				perror("sendmmsg");
			return -1;
		}
		/* r datagrams went out (maybe fewer than k): carry on after them. */
		for (int i = 0; i < r; i++)
			done += hdr[i].count;
		*dseq += r;
		st->dg_sent += r;
	}
	return 0;
}

static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/*
 * Commands from the laptop, read without waiting after each block.
 * Returns 1 to keep sending, 0 to end the session.
 */
static int udp_commands(int s, struct sockaddr_in *client, double *last_seen,
			uint32_t *dseq, long bs)
{
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		char cmd[16], h[200];
		ssize_t n = recvfrom(s, cmd, sizeof(cmd) - 1, MSG_DONTWAIT,
				     (struct sockaddr *)&from, &fl);

		if (n < 0)
			break;		/* nothing waiting (EAGAIN) */
		cmd[n] = 0;
		if (strncmp(cmd, "start", 5) == 0) {
			/* The laptop restarted, or another one took over. */
			*client = from;
			*dseq = 0;
			*last_seen = now();
			make_header(h, sizeof(h), "udp", bs);
			sendto(s, h, strlen(h), 0, (struct sockaddr *)client, sizeof(*client));
			printf("restart from %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
		} else if (!same_peer(&from, client)) {
			continue;	/* not our laptop: ignore */
		} else if (strncmp(cmd, "keep", 4) == 0) {
			*last_seen = now();
		} else if (strncmp(cmd, "stop", 4) == 0) {
			printf("stop from the laptop\n");
			return 0;
		}
	}
	if (now() - *last_seen > KEEP_TIMEOUT) {
		printf("no \"keep\" from the laptop for %.0f s: stopping\n", KEEP_TIMEOUT);
		return 0;
	}
	return 1;
}

static int serve_udp(int port)
{
	int s = listen_socket(SOCK_DGRAM, port);

	if (s < 0)
		return 2;
	set_sndbuf(s);
	printf("UDP port %d, %d samples (%d bytes) per datagram: waiting for \"start\" "
	       "(tools/adcrecv.py --udp). Ctrl-C to stop.\n",
	       port, dgram_samples, (int)sizeof(struct dgram_hdr) + 4 * dgram_samples);
	if (sizeof(struct dgram_hdr) + 4 * dgram_samples > 1472)
		printf("note: datagrams over 1472 bytes are split into IP fragments\n");
	fflush(stdout);

	while (!stop) {
		struct sockaddr_in client;
		socklen_t cl = sizeof(client);
		char cmd[16], h[200];
		struct src src;
		struct stats st;
		uint32_t dseq = 0;
		double last_seen;
		ssize_t n = recvfrom(s, cmd, sizeof(cmd) - 1, 0, (struct sockaddr *)&client, &cl);

		if (n < 0) {
			if (errno != EINTR)
				perror("recvfrom");
			continue;
		}
		cmd[n] = 0;
		if (strncmp(cmd, "start", 5) != 0)
			continue;	/* a late "keep" or "stop" from an old session */
		printf("start from %s:%d\n", inet_ntoa(client.sin_addr), ntohs(client.sin_port));
		if (src_open(&src) < 0) {
			static const char err[] = "ADCSERVE error: cannot open " DEV "\n";

			sendto(s, err, sizeof(err) - 1, 0, (struct sockaddr *)&client, sizeof(client));
			continue;
		}
		make_header(h, sizeof(h), "udp", src.bs);
		fputs(h, stdout);
		sendto(s, h, strlen(h), 0, (struct sockaddr *)&client, sizeof(client));
		last_seen = now();
		stats_start(&st);
		for (;;) {
			__u32 idx = 0;
			const uint32_t *blk = src_get(&src, &idx);
			int r;

			if (!blk)
				break;
			r = send_block_udp(s, &client, blk, src.bs, &dseq, &st);
			src_put(&src, idx);
			if (r < 0)
				break;
			stats_add(&st, src.bs);
			if (!udp_commands(s, &client, &last_seen, &dseq, src.bs))
				break;
		}
		stats_end(&st);
		src_close(&src);
	}
	close(s);
	return 0;
}

int main(int argc, char **argv)
{
	int port = DEF_PORT, udp = 0, opt;
	struct sigaction sa;

	while ((opt = getopt(argc, argv, "p:ud:S:mq")) != -1) {
		switch (opt) {
		case 'p': port = atoi(optarg); break;
		case 'u': udp = 1; break;
		case 'd': dgram_samples = atoi(optarg); break;
		case 'S': sndbuf = atoi(optarg); break;
		case 'm': use_mmap = 1; break;
		case 'q': quiet = 1; break;
		default:
			fprintf(stderr, "usage: %s [-u [-d samples_per_datagram]] [-p port] [-S sndbuf] [-m] [-q]\n",
				argv[0]);
			return 2;
		}
	}
	if (dgram_samples < 1 || dgram_samples > MAX_DGRAM) {
		fprintf(stderr, "-d: 1 .. %d samples\n", MAX_DGRAM);
		return 2;
	}

	/* No SA_RESTART: Ctrl-C interrupts accept(), read(), send(). */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	setvbuf(stdout, NULL, _IOLBF, 0);	/* one line at a time, also into a log */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	return udp ? serve_udp(port) : serve_tcp(port);
}
