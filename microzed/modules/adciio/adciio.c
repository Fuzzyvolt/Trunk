// SPDX-License-Identifier: GPL-2.0
/*
 * adciio - the project 05 simulated ADC as a standard IIO device (project 07).
 *
 * Same hardware as adcstream (project 05), same device tree node, different
 * interface to user space:
 *
 *   adcsim (PL) --stream--> FIFO --> AXI DMA S2MM --> IIO DMA buffer --> /dev/iio:deviceN
 *   0x43C20000              8192 words  (xilinx_dma)    (kernel helper)    iio_readdev, libiio
 *
 * adcstream had its own ring, its own /dev node and its own sysfs files. Here
 * the IIO core provides all of that, the way every ADC driver in the kernel
 * does it (the XADC is iio:device0 on this board):
 *
 *   /sys/bus/iio/devices/iio:deviceN/
 *     name                          "adcsim"
 *     sampling_frequency            Sa/s, read/write, also while streaming
 *     sampling_frequency_available  [min step max]
 *     in_voltage0_scale             mV per LSB (value x scale = mV)
 *     in_voltage0_signal_frequency  test signal frequency in Hz (ours)
 *     in_voltage0_signal_mode       "sine" or "saw" (ours)
 *     in_voltage0_signal_mode_available
 *     pl_samples, pl_dropped, pl_packets, block_samples   PL counters (ours)
 *     scan_elements/in_voltage0_en, _type ("le:s16/32>>0"), _index
 *     buffer/length, buffer/enable, buffer/watermark, buffer/data_available
 *   /dev/iio:deviceN                read() returns the samples
 *
 * Sample word (32 bits, as the PL makes it): bits 15..0 the value (signed),
 * bits 31..16 a sequence number. The channel says "s16 stored in 32 bits",
 * so IIO tools use the low half and ignore the top. The sequence number is
 * still in the raw data for loss checks (adcread -i, tools/iio_check.py).
 *
 * The buffer: the kernel's IIO DMA engine buffer
 * ----------------------------------------------
 * iio_dmaengine_buffer_setup() asks for our "rx" DMA channel and does the
 * rest: it allocates the memory, queues it with the DMA, hands finished
 * blocks to read(). With read() (libiio 0.x, cat) it splits the buffer into
 * exactly TWO blocks of buffer/length / 2 samples each (at most 64 KiB, the
 * DMA device's segment limit): one being filled, one being read.
 *
 * That fits xilinx_dma in simple mode (project 05 notes: never more than
 * one buffer queued behind the running one). It also means the slack is
 * one block: if the reader is late, the DMA has no free block, stops taking
 * data, the PL FIFO fills and the PL drops samples (pl_dropped, and gaps in
 * the sequence numbers). There is no "overrun" count as in adcstream.
 *
 * What this driver adds: setup_ops, the hooks the IIO core calls around
 * starting and stopping the buffer:
 *
 *   echo 1 > buffer/enable:  preenable   check the block size, empty the PL FIFO
 *                            (core)      queue both blocks with the DMA
 *                            postenable  PKT_LEN = block size, start the PL
 *   echo 0 > buffer/enable:  predisable  empty the DMA, then stop the PL (see below)
 *                            (core)      dmaengine_terminate_sync(): halt + reset
 *
 * The PL's packet (TLAST every PKT_LEN samples) must be exactly one block:
 * in simple mode xilinx_dma reports no residue, so a short packet would end
 * a block early with stale data in the rest, and a long one would run over.
 *
 * Stopping (predisable), in this order:
 *   1. no more blocks to the DMA (the core's queue->active = false)
 *   2. PL at 100 MSa/s (DIV = 1) until the DMA holds no block (< 1 ms)
 *   3. PL stopped, its FIFO emptied
 * Then the core's dmaengine_terminate_sync() halts and resets an idle DMA.
 * Why each step:
 *   - An S2MM channel that holds a block and gets no data never halts, and
 *     xilinx_dma's halt timeout never expires (project 05: the board hung).
 *     Steps 1-2 make sure it holds none.
 *   - Words that reach the DMA after its reset stay inside it and land in
 *     front of the next stream's first packet, which then no longer fits its
 *     block: DMA internal error ("has errors 10"), the channel stops, the
 *     next reader waits forever. Found on the board: first version stopped
 *     the PL only after the reset. Step 3 before the reset avoids it (any
 *     words the idle DMA took in between are cleared by the reset).
 * The last block(s) of a stream hold samples taken at 100 MSa/s.
 *
 * Load order: bitstream, xilinx_dma, this (S40pl does it, when
 * ADC_DRIVER=adciio). Never together with adcstream: both want the same
 * registers and the same DMA channel.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/buffer-dma.h>
#include <linux/iio/buffer-dmaengine.h>

/* Register offsets, same as adcsim.v */
#define ADC_CTRL	0x00
#define ADC_STATUS	0x04
#define ADC_DIV		0x08
#define ADC_PKT_LEN	0x0C
#define ADC_PHASE_INC	0x10
#define ADC_SAMPLES	0x14
#define ADC_DROPPED	0x18
#define ADC_PACKETS	0x1C
#define ADC_ID		0x20

#define CTRL_EN		BIT(0)
#define CTRL_SINE	BIT(1)
#define CTRL_FLUSH	BIT(2)

#define ADC_ID_VALUE	0x4D5A0501

/* Simulated full scale: +-1000 mV over the 16-bit range (1000 / 2^15 mV per LSB). */
#define SCALE_MV	1000
#define SCALE_BITS	15

struct adciio {
	struct device *dev;
	void __iomem *base;
	unsigned long clk_hz;
	struct iio_buffer *buffer;	/* from iio_dmaengine_buffer_setup() */
	int rate_avail[3];		/* sampling_frequency_available: min, step, max */

	struct mutex lock;		/* settings and register writes below */
	u32 div;			/* clocks per sample */
	u32 freq_hz;			/* test signal frequency */
	bool sine;
	bool streaming;			/* between postenable and predisable */
	u32 pkt_len;			/* samples per DMA block, last stream */
};

static inline u32 adc_rd(struct adciio *st, u32 reg)
{
	return readl(st->base + reg);
}

static inline void adc_wr(struct adciio *st, u32 reg, u32 val)
{
	writel(val, st->base + reg);
}

static u32 adc_rate(struct adciio *st)
{
	return st->clk_hz / st->div;
}

/* f = rate * PHASE_INC / 2^32, see adcstream.c */
static u32 adc_phase_inc(struct adciio *st)
{
	return (u32)mul_u64_u64_div_u64((u64)st->freq_hz << 32, st->div, st->clk_hz);
}

static u32 adc_ctrl_mode(struct adciio *st)
{
	return st->sine ? CTRL_SINE : 0;
}

/* Generator off at once, FIFO emptied. */
static void adc_flush(struct adciio *st)
{
	adc_wr(st, ADC_CTRL, adc_ctrl_mode(st) | CTRL_FLUSH);
	adc_rd(st, ADC_CTRL);		/* make sure the write arrived */
	udelay(1);			/* 100 clocks of FIFO reset */
	adc_wr(st, ADC_CTRL, adc_ctrl_mode(st));
}

/* ------------------------------------------------------------------ */
/* Channel attributes: read_raw / write_raw / read_avail               */

/*
 * The IIO core turns the bits in info_mask_* into sysfs files and calls
 * these functions for them. "mask" says which one.
 */
static int adciio_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct adciio *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = adc_rate(st);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/* val / 2^val2 = 0.030517578 mV per LSB */
		*val = SCALE_MV;
		*val2 = SCALE_BITS;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

/* Can change while streaming: the PL uses the new DIV from the next sample. */
static int adciio_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			    int val, int val2, long mask)
{
	struct adciio *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (val < 1 || val > st->clk_hz)
			return -EINVAL;
		mutex_lock(&st->lock);
		st->div = max_t(u32, 1, DIV_ROUND_CLOSEST(st->clk_hz, val));
		adc_wr(st, ADC_DIV, st->div);
		adc_wr(st, ADC_PHASE_INC, adc_phase_inc(st));	/* same signal frequency */
		mutex_unlock(&st->lock);
		return 0;
	default:
		return -EINVAL;
	}
}

/* sampling_frequency is a whole number: "1000000", not "1000000.000000" */
static int adciio_write_raw_get_fmt(struct iio_dev *indio_dev,
				    struct iio_chan_spec const *chan, long mask)
{
	return IIO_VAL_INT;
}

static int adciio_read_avail(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			     const int **vals, int *type, int *length, long mask)
{
	struct adciio *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = st->rate_avail;		/* printed as "[1 1 99999999]" */
		*type = IIO_VAL_INT;
		*length = ARRAY_SIZE(st->rate_avail);
		return IIO_AVAIL_RANGE;
	default:
		return -EINVAL;
	}
}

/* ------------------------------------------------------------------ */
/* Extended channel attributes: the test signal                        */

static ssize_t adciio_freq_read(struct iio_dev *indio_dev, uintptr_t private,
				struct iio_chan_spec const *chan, char *buf)
{
	struct adciio *st = iio_priv(indio_dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(st->freq_hz));
}

/* 0 .. rate/2. Lowering the rate later can push it above rate/2: aliasing. */
static ssize_t adciio_freq_write(struct iio_dev *indio_dev, uintptr_t private,
				 struct iio_chan_spec const *chan, const char *buf, size_t len)
{
	struct adciio *st = iio_priv(indio_dev);
	u32 f;
	int ret;

	ret = kstrtou32(buf, 0, &f);
	if (ret)
		return ret;

	mutex_lock(&st->lock);
	if (f > adc_rate(st) / 2) {
		ret = -EINVAL;
	} else {
		st->freq_hz = f;
		adc_wr(st, ADC_PHASE_INC, adc_phase_inc(st));
	}
	mutex_unlock(&st->lock);
	return ret ? ret : len;
}

static const char * const adciio_modes[] = { "sine", "saw" };

static int adciio_get_mode(struct iio_dev *indio_dev, const struct iio_chan_spec *chan)
{
	struct adciio *st = iio_priv(indio_dev);

	return READ_ONCE(st->sine) ? 0 : 1;	/* index into adciio_modes */
}

static int adciio_set_mode(struct iio_dev *indio_dev, const struct iio_chan_spec *chan,
			   unsigned int mode)
{
	struct adciio *st = iio_priv(indio_dev);

	mutex_lock(&st->lock);
	st->sine = (mode == 0);
	adc_wr(st, ADC_CTRL, adc_ctrl_mode(st) | (st->streaming ? CTRL_EN : 0));
	mutex_unlock(&st->lock);
	return 0;
}

/* IIO_ENUM: the core parses "sine"/"saw" and lists them in _available. */
static const struct iio_enum adciio_mode_enum = {
	.items = adciio_modes,
	.num_items = ARRAY_SIZE(adciio_modes),
	.get = adciio_get_mode,
	.set = adciio_set_mode,
};

static const struct iio_chan_spec_ext_info adciio_ext_info[] = {
	{
		.name = "signal_frequency",
		.shared = IIO_SEPARATE,
		.read = adciio_freq_read,
		.write = adciio_freq_write,
	},
	IIO_ENUM("signal_mode", IIO_SEPARATE, &adciio_mode_enum),
	IIO_ENUM_AVAILABLE("signal_mode", IIO_SEPARATE, &adciio_mode_enum),
	{ }
};

/*
 * One channel: voltage 0. In the buffer each sample takes 32 bits
 * (storagebits), of which the low 16 are the value (realbits, signed,
 * shift 0). scan_elements/in_voltage0_type shows this as "le:s16/32>>0".
 */
static const struct iio_chan_spec adciio_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = adciio_ext_info,
		.scan_index = 0,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 32,
			.shift = 0,
			.endianness = IIO_LE,
		},
	},
};

/* The PL always sends this one channel: the only valid scan mask. */
static const unsigned long adciio_scan_masks[] = { BIT(0), 0 };

/* ------------------------------------------------------------------ */
/* Device attributes: PL counters                                      */

static ssize_t adciio_reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adciio *st = iio_priv(dev_to_iio_dev(dev));

	/* this_attr->address holds the register offset (last IIO_DEVICE_ATTR argument) */
	return sysfs_emit(buf, "%u\n", adc_rd(st, to_iio_dev_attr(attr)->address));
}

static ssize_t block_samples_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adciio *st = iio_priv(dev_to_iio_dev(dev));

	return sysfs_emit(buf, "%u\n", READ_ONCE(st->pkt_len));
}

static IIO_DEVICE_ATTR(pl_samples, 0444, adciio_reg_show, NULL, ADC_SAMPLES);
static IIO_DEVICE_ATTR(pl_dropped, 0444, adciio_reg_show, NULL, ADC_DROPPED);
static IIO_DEVICE_ATTR(pl_packets, 0444, adciio_reg_show, NULL, ADC_PACKETS);
static IIO_DEVICE_ATTR_RO(block_samples, 0);

static struct attribute *adciio_attrs[] = {
	&iio_dev_attr_pl_samples.dev_attr.attr,
	&iio_dev_attr_pl_dropped.dev_attr.attr,
	&iio_dev_attr_pl_packets.dev_attr.attr,
	&iio_dev_attr_block_samples.dev_attr.attr,
	NULL
};

static const struct attribute_group adciio_attr_group = {
	.attrs = adciio_attrs,
};

static const struct iio_info adciio_info = {
	.read_raw = adciio_read_raw,
	.write_raw = adciio_write_raw,
	.write_raw_get_fmt = adciio_write_raw_get_fmt,
	.read_avail = adciio_read_avail,
	.attrs = &adciio_attr_group,
};

/* ------------------------------------------------------------------ */
/* Buffer start / stop                                                 */

/*
 * The IIO DMA buffer's private state. We only read the two read() blocks'
 * size from it, nothing else: there is no function that returns it.
 */
static struct iio_dma_buffer_queue *adciio_queue(struct adciio *st)
{
	return container_of(st->buffer, struct iio_dma_buffer_queue, buffer);
}

/* Number of read() blocks the DMA holds right now (0..2). */
static int adciio_blocks_active(struct iio_dma_buffer_queue *q)
{
	int i, n = 0;

	spin_lock_irq(&q->list_lock);
	for (i = 0; i < ARRAY_SIZE(q->fileio.blocks); i++)
		if (q->fileio.blocks[i] &&
		    q->fileio.blocks[i]->state == IIO_BLOCK_STATE_ACTIVE)
			n++;
	spin_unlock_irq(&q->list_lock);
	return n;
}

/*
 * Before the core queues the blocks with the DMA. A failure here is safe
 * (nothing is queued yet); a failure later would make the core stop a DMA
 * that waits for data, which hangs (see the top of the file).
 */
static int adciio_preenable(struct iio_dev *indio_dev)
{
	struct adciio *st = iio_priv(indio_dev);
	struct iio_dma_buffer_queue *q = adciio_queue(st);

	if (!q->fileio.enabled || !q->fileio.blocks[0] || !q->fileio.blocks[1]) {
		dev_err(st->dev, "only the read() interface is supported (no DMABUF)\n");
		return -EOPNOTSUPP;
	}
	/* block = buffer/length * 4 bytes / 2: whole samples only if length is even */
	if (q->fileio.block_size % 4 || q->fileio.block_size < 8) {
		dev_err(st->dev, "buffer/length must be even and at least 4 (is %zu)\n",
			q->fileio.block_size / 2);
		return -EINVAL;
	}

	/*
	 * The core never resets which block read() takes next. If the last
	 * stream ended after an odd number of blocks, read() would start with
	 * block 1 while the DMA fills block 0 first: the first two blocks come
	 * out swapped. Both blocks are idle here (QUEUED, not yet submitted).
	 */
	mutex_lock(&q->lock);
	q->fileio.next_dequeue = 0;
	mutex_unlock(&q->lock);

	mutex_lock(&st->lock);
	adc_flush(st);			/* no old samples in the FIFO */
	adc_wr(st, ADC_DIV, st->div);
	adc_wr(st, ADC_PHASE_INC, adc_phase_inc(st));
	mutex_unlock(&st->lock);
	return 0;
}

/* Both blocks are with the DMA now: start the PL. */
static int adciio_postenable(struct iio_dev *indio_dev)
{
	struct adciio *st = iio_priv(indio_dev);
	struct iio_dma_buffer_queue *q = adciio_queue(st);
	/*
	 * bytes_used = what the DMA was actually given: the block size, cut to
	 * the DMA's 64 KiB segment limit. No block can have finished yet (the
	 * PL is stopped), so reading it without the queue lock is safe.
	 */
	size_t bytes = q->fileio.blocks[0]->bytes_used;

	if (bytes % 4 || bytes != q->fileio.blocks[1]->bytes_used)
		dev_warn(st->dev, "odd DMA block sizes %zu/%zu, data will be wrong\n",
			 bytes, q->fileio.blocks[1]->bytes_used);

	mutex_lock(&st->lock);
	st->pkt_len = bytes / 4;
	adc_wr(st, ADC_PKT_LEN, st->pkt_len);
	/* EN 0 -> 1 also clears the PL's counters and sequence number. */
	adc_wr(st, ADC_CTRL, adc_ctrl_mode(st) | CTRL_EN);
	st->streaming = true;
	mutex_unlock(&st->lock);

	dev_dbg(st->dev, "streaming at %u Sa/s, %u samples per block\n",
		adc_rate(st), st->pkt_len);
	return 0;
}

/* Before the core stops the DMA: empty it, then stop the PL (see the top). */
static int adciio_predisable(struct iio_dev *indio_dev)
{
	struct adciio *st = iio_priv(indio_dev);
	struct iio_dma_buffer_queue *q = adciio_queue(st);
	unsigned long timeout;
	int n;

	/*
	 * 1. Blocks read() hands back now go to the core's "incoming" list
	 * instead of the DMA. The core sets this itself right after us; we need
	 * it earlier. q->lock is what read() holds while handing a block back.
	 */
	mutex_lock(&q->lock);
	q->active = false;
	mutex_unlock(&q->lock);

	/* 2. Fill what the DMA holds: 2 x 16384 samples at 100 MSa/s = 0.33 ms. */
	mutex_lock(&st->lock);
	adc_wr(st, ADC_DIV, 1);		/* st->div keeps the real rate */
	mutex_unlock(&st->lock);
	timeout = jiffies + msecs_to_jiffies(100);
	while ((n = adciio_blocks_active(q)) && time_before(jiffies, timeout))
		usleep_range(100, 200);
	if (n)		/* after a DMA error: the channel is halted, terminate won't hang */
		dev_warn(st->dev, "DMA still holds %d block(s) after 100 ms\n", n);

	/* 3. PL off, FIFO empty, normal rate back. Nothing flows any more. */
	mutex_lock(&st->lock);
	st->streaming = false;
	adc_flush(st);
	adc_wr(st, ADC_DIV, st->div);
	mutex_unlock(&st->lock);
	return 0;
}

static const struct iio_buffer_setup_ops adciio_buffer_ops = {
	.preenable = adciio_preenable,
	.postenable = adciio_postenable,
	.predisable = adciio_predisable,
};

/* ------------------------------------------------------------------ */
/* Probe                                                               */

/* Same check as adcstream: is the PL programmed? */
static int adc_check_pl(struct device *dev)
{
	struct device_node *np;
	struct fpga_manager *mgr;
	enum fpga_mgr_states state;

	np = of_parse_phandle(dev->of_node, "fpga-mgr", 0);
	if (!np) {
		dev_warn(dev, "no fpga-mgr property, cannot check the PL is programmed\n");
		return 0;
	}
	mgr = of_fpga_mgr_get(np);
	of_node_put(np);
	if (IS_ERR(mgr))
		return dev_err_probe(dev, PTR_ERR(mgr), "FPGA manager not found\n");

	state = mgr->state;
	fpga_mgr_put(mgr);
	if (state != FPGA_MGR_STATE_OPERATING)
		return dev_err_probe(dev, -ENODEV,
				     "PL not programmed (fpga state %d): load the bitstream first\n",
				     state);
	return 0;
}

static void adciio_action_flush(void *data)
{
	adc_flush(data);
}

static void adciio_action_free_buffer(void *data)
{
	iio_dmaengine_buffer_free(data);
}

static int adciio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct adciio *st;
	struct resource *res;
	struct clk *clk;
	int ret;
	u32 id;

	ret = adc_check_pl(dev);
	if (ret)
		return ret;

	/* One allocation: the iio_dev with our struct behind it (iio_priv()). */
	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;
	st = iio_priv(indio_dev);
	st->dev = dev;
	mutex_init(&st->lock);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot get/enable the PL clock\n");
	st->clk_hz = clk_get_rate(clk);
	if (!st->clk_hz)
		return dev_err_probe(dev, -EINVAL, "PL clock rate is 0\n");

	st->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(st->base))
		return PTR_ERR(st->base);

	id = adc_rd(st, ADC_ID);
	if (id != ADC_ID_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "wrong ID 0x%08x (expected 0x%08x): wrong bitstream?\n",
				     id, ADC_ID_VALUE);

	/* Defaults: 1 MSa/s, 1 kHz sine. Stopped until buffer/enable = 1. */
	st->div = DIV_ROUND_CLOSEST(st->clk_hz, 1000000);
	st->freq_hz = 1000;
	st->sine = true;
	st->rate_avail[0] = 1;
	st->rate_avail[1] = 1;
	st->rate_avail[2] = st->clk_hz;
	adc_flush(st);
	ret = devm_add_action_or_reset(dev, adciio_action_flush, st);
	if (ret)
		return ret;

	indio_dev->name = "adcsim";		/* the hardware, as iio_info shows it */
	indio_dev->info = &adciio_info;
	indio_dev->channels = adciio_channels;
	indio_dev->num_channels = ARRAY_SIZE(adciio_channels);
	indio_dev->available_scan_masks = adciio_scan_masks;
	indio_dev->setup_ops = &adciio_buffer_ops;

	/*
	 * The buffer: requests the "rx" channel from dma-names (S2MM), adds
	 * INDIO_BUFFER_HARDWARE to indio_dev->modes. -EPROBE_DEFER if
	 * xilinx_dma is not loaded yet.
	 */
	st->buffer = iio_dmaengine_buffer_setup(dev, indio_dev, "rx");
	if (IS_ERR(st->buffer))
		return dev_err_probe(dev, PTR_ERR(st->buffer), "cannot set up the DMA buffer\n");
	ret = devm_add_action_or_reset(dev, adciio_action_free_buffer, st->buffer);
	if (ret)
		return ret;

	/* Last: from here on user space can see and use the device. */
	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register the IIO device\n");

	dev_info(dev, "adcsim at %pR, ID 0x%08x, %s, %u Sa/s\n",
		 res, id, dev_name(&indio_dev->dev), adc_rate(st));
	return 0;
}

/*
 * Same compatible as adcstream: it names the hardware, not the driver.
 * Whichever of the two modules is loaded binds; S40pl loads only one.
 */
static const struct of_device_id adciio_of_match[] = {
	{ .compatible = "microzed,adcstream" },
	{ }
};
MODULE_DEVICE_TABLE(of, adciio_of_match);

static struct platform_driver adciio_driver = {
	.probe = adciio_probe,
	.driver = {
		.name = "adciio",
		.of_match_table = adciio_of_match,
	},
};
module_platform_driver(adciio_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MicroZed project 07: simulated ADC stream as an IIO device");
/* The IIO DMA engine buffer exports its functions in a symbol namespace. */
MODULE_IMPORT_NS(IIO_DMAENGINE_BUFFER);
