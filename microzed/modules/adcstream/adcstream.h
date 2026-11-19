/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * adcstream user interface for mmap() mode, shared by the driver and
 * user-space programs (apps/adcread.c includes it).
 *
 * Zero-copy reading:
 *
 *   fd = open("/dev/adcstream", O_RDONLY);
 *   ioctl(fd, ADCSTREAM_IOC_INFO, &info);          nbuf, buf_size, block_bytes
 *   for i in 0 .. nbuf-1:
 *       ring[i] = mmap(NULL, info.buf_size, PROT_READ, MAP_SHARED, fd, i * info.buf_size);
 *   loop:
 *       ioctl(fd, ADCSTREAM_IOC_GET, &idx);         sleeps until a block is full;
 *                                                  the block is now yours
 *       ... use ring[idx][0 .. block_bytes/4 - 1] ...
 *       ioctl(fd, ADCSTREAM_IOC_PUT, &idx);         give it back for the DMA
 *
 * The data never gets copied: ring[idx] IS the DMA buffer, mapped read-only
 * into the program. While a block is held (between GET and PUT) the driver
 * never gives it to the DMA, so it cannot change under you. At most
 * info.max_held blocks may be held at once (GET returns EBUSY beyond that).
 * GET fails with ENODATA once the stream is stopped (like end of file).
 */
#ifndef ADCSTREAM_H
#define ADCSTREAM_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct adcstream_info {
	__u32 nbuf;		/* buffers in the ring: mmap each one */
	__u32 buf_size;		/* bytes per buffer = mmap length and offset step */
	__u32 block_bytes;	/* bytes of samples in each full block */
	__u32 max_held;		/* blocks a program may hold at once */
};

/* 'z': an arbitrary "magic" letter that makes our command numbers unique. */
#define ADCSTREAM_IOC_INFO	_IOR('z', 1, struct adcstream_info)
#define ADCSTREAM_IOC_GET	_IOR('z', 2, __u32)
#define ADCSTREAM_IOC_PUT	_IOW('z', 3, __u32)

#endif
