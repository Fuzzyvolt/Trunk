// SPDX-License-Identifier: GPL-2.0
/*
 * adcstream - continuous sample stream from the project 05 simulated ADC.
 *
 *   adcsim (PL) --stream--> FIFO --> AXI DMA S2MM --> ring of DDR buffers --> read()
 *   0x43C20000              8192 words   (xilinx_dma)    (this driver)        /dev/adcstream
 *
 * The PL makes one sample every DIV clocks and ends a packet (TLAST) every
 * block_samples samples. The DMA writes one packet into one buffer. This
 * driver owns adcsim's registers and is a DMA engine client for the S2MM
 * channel, like dmaloop in project 04.
 *
 * Device tree node (added by board/post-image-dt.sh):
 *
 *   adcstream@43c20000 {
 *       compatible = "microzed,adcstream";
 *       reg = <0x43c20000 0x1000>;
 *       clocks = <&clkc 15>;            <- FCLK_CLK0, sets the sample clock
 *       fpga-mgr = <&devcfg>;
 *       dmas = <&axi_dma 1>;            <- channel 1 = S2MM (stream -> memory)
 *       dma-names = "rx";
 *   };
 *
 * Sample word: bits 31..16 sequence number (+1 per sample, also for samples
 * the PL had to drop), bits 15..0 the value (signed 16 bit).
 *
 * The buffer ring
 * ---------------
 * nbuf buffers (module parameter, default 8). Each one is in one place:
 *
 *   free --submit--> DMA --done--> full --read()--> reader --all read--> free
 *                                   |
 *                                   +-- no free buffer left: the oldest full one is
 *                                       given back to the DMA (an OVERRUN: that block
 *                                       is lost, the reader sees a sequence gap)
 *
 * Exactly two buffers are with the DMA at any time: one it is filling and
 * one queued behind it. Why two and not all of them: in simple mode (no
 * scatter-gather) xilinx_dma programs only the first queued buffer, but its
 * interrupt handler then marks EVERY buffer it had taken from the queue as
 * done. So at most one may wait in the queue. With one waiting, xilinx_dma
 * starts it from its interrupt handler the moment the current one ends, and
 * the next one is queued from our completion callback a little later.
 * The PL FIFO holds the samples made during that short gap.
 *
 * With no reader (/dev/adcstream closed) every finished block goes straight
 * back to the free list, so the stream keeps running for the web page's
 * snapshot, and nothing counts as an overrun.
 *
 * /dev/adcstream (one reader at a time)
 *   read()     whole or partial blocks of raw sample words; sleeps until a
 *              block is ready. Returns 0 (end of file) once the stream stops.
 *   poll()     readable when a block is waiting. O_NONBLOCK gives -EAGAIN.
 *
 * sysfs files in /sys/bus/platform/devices/43c20000.adcstream/:
 *   enable          1 = stream on, 0 = off (on at load, module parameter autostart)
 *   rate            sample rate in Sa/s (100 MHz / DIV, so it reads back rounded)
 *   freq            signal frequency in Hz
 *   mode            "sine" or "saw"
 *   block_samples   samples per DMA buffer (64 .. 65536), only while the stream is off
 *   blocks          blocks the DMA has finished since the stream started
 *   overruns        blocks lost because the reader was too slow
 *   pl_dropped      samples the PL dropped because its FIFO was full
 *   status          all counters on one line
 *   snapshot        about 256 evenly spaced samples of the newest block (for the web page)
 *
 * Load order: bitstream, xilinx_dma, this (S40pl does it).
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

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
#define STATUS_RUNNING	BIT(0)		/* unused: the stop no longer waits for the packet end */

#define ADC_ID_VALUE	0x4D5A0501

#define MAX_NBUF	32
#define MIN_BLOCK	64
#define MAX_BLOCK	65536			/* samples = 256 KiB per buffer */
#define DEF_BLOCK	4096
#define SNAP_POINTS	256
#define IN_FLIGHT	2			/* buffers with the DMA, see above */

static unsigned int nbuf = 8;
module_param(nbuf, uint, 0444);
MODULE_PARM_DESC(nbuf, "number of DMA buffers in the ring (4..32, default 8)");

static bool autostart = true;
module_param(autostart, bool, 0444);
MODULE_PARM_DESC(autostart, "start the stream when the driver loads (default 1)");

struct adcstream;

/* Callback parameter: which buffer just finished. */
struct adc_slot {
	struct adcstream *as;
	unsigned int idx;
};

struct adcstream {
	struct device *dev;
	void __iomem *base;
	unsigned long clk_hz;
	struct dma_chan *rx;
	struct device *dma_dev;		/* the DMA controller: buffers are allocated for it */

	unsigned int nbuf;
	u32 *buf[MAX_NBUF];		/* CPU addresses (uncached) */
	dma_addr_t buf_dma[MAX_NBUF];	/* the same buffers as the DMA sees them */
	struct adc_slot slot[MAX_NBUF];

	/* Settings; cfg_lock (a mutex: start/stop sleep) */
	struct mutex cfg_lock;
	bool enabled;			/* the user asked for the stream to run */
	u32 div;			/* clocks per sample */
	u32 block_samples;
	u32 freq_hz;
	bool sine;

	/* Ring state; lock (a spinlock: the DMA callback runs in a tasklet) */
	spinlock_t lock;
	bool running;			/* callbacks resubmit buffers only while true */
	bool error;
	bool reader;			/* /dev/adcstream is open */
	unsigned int inflight;		/* buffers the DMA holds (0..2) */
	u32 block_bytes;		/* fixed while running */
	u8 freeq[MAX_NBUF];		/* stack of free buffers */
	unsigned int nfree;
	u8 fullq[MAX_NBUF];		/* FIFO of full buffers, oldest at fhead */
	unsigned int fhead, fcount;
	u64 blocks, overruns;

	/* The block the reader is copying out; read_lock */
	struct mutex read_lock;
	int cur;			/* buffer index, -1 = none */
	u32 cur_off;			/* bytes already read from it */

	wait_queue_head_t wq;		/* readers wait here for a full block */
	atomic_t open;

	/* Snapshot for the web page: filled by the callback on request */
	struct mutex snap_lock;
	struct completion snap_done;
	bool snap_want;			/* under lock */
	u32 snap[SNAP_POINTS];
	unsigned int snap_n, snap_step;
	u64 snap_block;
	u32 snap_rate;

	struct miscdevice misc;
};

static inline u32 adc_rd(struct adcstream *as, u32 reg)
{
	return readl(as->base + reg);
}

static inline void adc_wr(struct adcstream *as, u32 reg, u32 val)
{
	writel(val, as->base + reg);
}

static u32 adc_rate(struct adcstream *as)
{
	return as->clk_hz / as->div;
}

/*
 * PHASE_INC for the wanted signal frequency: the phase (2^32 = one period)
 * moves PHASE_INC per sample, so f = rate * PHASE_INC / 2^32, and
 * PHASE_INC = f * 2^32 / rate = f * 2^32 * DIV / clk. The product needs more
 * than 64 bits, which mul_u64_u64_div_u64() handles.
 */
static u32 adc_phase_inc(struct adcstream *as)
{
	return (u32)mul_u64_u64_div_u64((u64)as->freq_hz << 32, as->div, as->clk_hz);
}

static u32 adc_ctrl_mode(struct adcstream *as)
{
	return as->sine ? CTRL_SINE : 0;
}

/* Empty the PL FIFO and stop the generator at once. */
static void adc_flush(struct adcstream *as)
{
	adc_wr(as, ADC_CTRL, adc_ctrl_mode(as) | CTRL_FLUSH);
	adc_rd(as, ADC_CTRL);		/* make sure the write arrived */
	udelay(1);			/* 100 clocks of FIFO reset */
	adc_wr(as, ADC_CTRL, adc_ctrl_mode(as));
}

/* ------------------------------------------------------------------ */
/* The ring                                                            */

/* All buffers free, nothing full, nothing being read. Caller holds lock. */
static void adc_ring_reset(struct adcstream *as)
{
	unsigned int i;

	for (i = 0; i < as->nbuf; i++)
		as->freeq[i] = i;
	as->nfree = as->nbuf;
	as->fhead = 0;
	as->fcount = 0;
}

static void adc_full_push(struct adcstream *as, unsigned int idx)
{
	as->fullq[(as->fhead + as->fcount) % as->nbuf] = idx;
	as->fcount++;
}

static unsigned int adc_full_pop(struct adcstream *as)
{
	unsigned int idx = as->fullq[as->fhead];

	as->fhead = (as->fhead + 1) % as->nbuf;
	as->fcount--;
	return idx;
}

static void adc_dma_done(void *param, const struct dmaengine_result *result);

/*
 * Give one buffer to the DMA: describe it ("receive block_bytes into this
 * buffer"), attach the callback, queue it, and start the queue. Called with
 * lock held, from process context or from the callback: the xilinx_dma
 * functions used here do not sleep.
 *
 * dma_async_issue_pending() right after each submit matters: if two were
 * waiting in the queue when xilinx_dma starts it, both would be marked done
 * at the next interrupt (see the comment at the top).
 */
static int adc_submit(struct adcstream *as, unsigned int idx)
{
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;

	desc = dmaengine_prep_slave_single(as->rx, as->buf_dma[idx], as->block_bytes,
					   DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -ENOMEM;
	desc->callback_result = adc_dma_done;
	desc->callback_param = &as->slot[idx];

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie))
		return -EIO;
	dma_async_issue_pending(as->rx);
	as->inflight++;
	return 0;
}

/* Copy evenly spaced samples of a finished block for the snapshot file. */
static void adc_snap_take(struct adcstream *as, unsigned int idx)
{
	unsigned int bs = as->block_bytes / 4;
	unsigned int step = max(1u, bs / SNAP_POINTS);
	unsigned int i, n = min(bs / step, (unsigned int)SNAP_POINTS);

	/* Uncached memory: about 0.1 us per word, 30 us for 256. */
	for (i = 0; i < n; i++)
		as->snap[i] = READ_ONCE(as->buf[idx][i * step]);
	as->snap_n = n;
	as->snap_step = step;
	as->snap_block = as->blocks;
	as->snap_rate = adc_rate(as);
	as->snap_want = false;
	complete(&as->snap_done);
}

/*
 * DMA completion callback: one buffer is full. Runs in xilinx_dma's tasklet
 * (softirq: may not sleep), after its interrupt handler has already started
 * the buffer that was queued behind this one.
 */
static void adc_dma_done(void *param, const struct dmaengine_result *result)
{
	struct adc_slot *slot = param;
	struct adcstream *as = slot->as;
	unsigned int next;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&as->lock, flags);
	if (as->inflight)
		as->inflight--;
	if (!as->running)
		goto out;			/* stopping: the DMA is being emptied */

	if (result && result->result != DMA_TRANS_NOERROR) {
		dev_err_ratelimited(as->dev, "DMA error %d, stream stopped\n", result->result);
		goto fail;
	}

	as->blocks++;
	if (as->snap_want)
		adc_snap_take(as, slot->idx);

	/* The finished block goes to the reader, or straight back if there is none. */
	if (as->reader)
		adc_full_push(as, slot->idx);
	else
		as->freeq[as->nfree++] = slot->idx;

	/* Replace it at the DMA, so two stay in flight. */
	if (as->nfree) {
		next = as->freeq[--as->nfree];
	} else if (as->fcount) {
		next = adc_full_pop(as);	/* the reader is behind: drop its oldest block */
		as->overruns++;
	} else {
		dev_err_ratelimited(as->dev, "no buffer to give the DMA (nbuf too small?)\n");
		goto fail;
	}
	ret = adc_submit(as, next);
	if (ret) {
		dev_err_ratelimited(as->dev, "DMA submit failed (%d), stream stopped\n", ret);
		goto fail;
	}
	goto out;

fail:
	as->running = false;
	as->error = true;
out:
	spin_unlock_irqrestore(&as->lock, flags);
	wake_up(&as->wq);		/* readers, and adc_stop() waiting for inflight == 0 */
}

/* ------------------------------------------------------------------ */
/* Start / stop (cfg_lock held)                                        */

static void adc_stop(struct adcstream *as);

static int adc_start(struct adcstream *as)
{
	unsigned int i;
	int ret = 0;

	if (as->enabled)
		return 0;

	/* Clean start: empty FIFO, generator stopped, settings written. */
	adc_flush(as);
	adc_wr(as, ADC_DIV, as->div);
	adc_wr(as, ADC_PKT_LEN, as->block_samples);
	adc_wr(as, ADC_PHASE_INC, adc_phase_inc(as));

	mutex_lock(&as->read_lock);
	as->cur = -1;
	mutex_unlock(&as->read_lock);

	spin_lock_irq(&as->lock);
	adc_ring_reset(as);
	as->inflight = 0;
	as->block_bytes = as->block_samples * 4;
	as->blocks = 0;
	as->overruns = 0;
	as->error = false;
	as->running = true;
	/* Receive side first: two buffers ready before the first sample. */
	for (i = 0; i < IN_FLIGHT && !ret; i++)
		ret = adc_submit(as, as->freeq[--as->nfree]);
	spin_unlock_irq(&as->lock);
	as->enabled = true;		/* so adc_stop() below cleans up */
	if (ret) {
		dev_err(as->dev, "cannot queue DMA buffers (%d)\n", ret);
		adc_stop(as);
		return ret;
	}

	/* Go: EN 0 -> 1 also clears the PL's counters and sequence number. */
	adc_wr(as, ADC_CTRL, adc_ctrl_mode(as) | CTRL_EN);
	return 0;
}

/*
 * Stopping must leave the DMA with NO buffer before dmaengine_terminate_sync().
 * terminate asks the channel to halt and waits for it, but an S2MM channel
 * that holds a buffer waits for stream data and never halts. xilinx_dma's
 * wait has a "1 s" timeout that is really years: readl_poll_timeout_atomic()
 * with delay 0 counts only 1 ns per loop. So: stop feeding the DMA, let the
 * generator fill what it still holds (at full speed, about 1 ms), and only
 * then stop the PL and terminate. Found the hard way on the board.
 */
static void adc_stop(struct adcstream *as)
{
	if (!as->enabled)
		return;

	/* 1. Callbacks stop handing buffers to the DMA; readers see the end. */
	spin_lock_irq(&as->lock);
	as->running = false;
	spin_unlock_irq(&as->lock);
	wake_up(&as->wq);

	/*
	 * 2. Fill the (at most two) buffers the DMA still holds with throw-away
	 * samples at 100 MSa/s: 2 x 65536 samples take 1.3 ms. After a DMA
	 * error the channel is already halted and this times out harmlessly.
	 */
	adc_wr(as, ADC_DIV, 1);
	if (!wait_event_timeout(as->wq, READ_ONCE(as->inflight) == 0, HZ))
		dev_warn(as->dev, "DMA still holds %u buffer(s) after 1 s\n",
			 READ_ONCE(as->inflight));

	/* 3. Generator off at once, FIFO emptied, normal rate back. */
	adc_flush(as);
	adc_wr(as, ADC_DIV, as->div);

	/* 4. The channel is idle now, so it halts at once. */
	dmaengine_terminate_sync(as->rx);

	/* Wait for a reader in the middle of a copy, then reset the ring. */
	mutex_lock(&as->read_lock);
	spin_lock_irq(&as->lock);
	as->cur = -1;
	adc_ring_reset(as);
	spin_unlock_irq(&as->lock);
	mutex_unlock(&as->read_lock);

	as->enabled = false;
}

/* ------------------------------------------------------------------ */
/* /dev/adcstream                                                      */

static struct adcstream *file_as(struct file *f)
{
	/* misc_open() put the miscdevice in private_data. */
	return container_of(f->private_data, struct adcstream, misc);
}

static int adc_open(struct inode *inode, struct file *f)
{
	struct adcstream *as = file_as(f);

	if (atomic_cmpxchg(&as->open, 0, 1))
		return -EBUSY;		/* one reader at a time */

	spin_lock_irq(&as->lock);
	as->reader = true;		/* from now on, finished blocks are kept */
	spin_unlock_irq(&as->lock);
	return stream_open(inode, f);
}

static int adc_release(struct inode *inode, struct file *f)
{
	struct adcstream *as = file_as(f);

	mutex_lock(&as->read_lock);
	spin_lock_irq(&as->lock);
	as->reader = false;
	/* Give everything the reader held back to the free list. */
	while (as->fcount)
		as->freeq[as->nfree++] = adc_full_pop(as);
	if (as->cur >= 0)
		as->freeq[as->nfree++] = as->cur;
	as->cur = -1;
	spin_unlock_irq(&as->lock);
	mutex_unlock(&as->read_lock);

	atomic_set(&as->open, 0);
	return 0;
}

static bool adc_readable(struct adcstream *as)
{
	return READ_ONCE(as->fcount) || !READ_ONCE(as->running);
}

static ssize_t adc_read(struct file *f, char __user *ubuf, size_t count, loff_t *ppos)
{
	struct adcstream *as = file_as(f);
	size_t n;
	ssize_t ret;

	if (mutex_lock_interruptible(&as->read_lock))
		return -ERESTARTSYS;

	/* Get a full block, waiting for one if needed. */
	while (as->cur < 0) {
		bool stopped, err;

		spin_lock_irq(&as->lock);
		if (as->fcount) {
			as->cur = adc_full_pop(as);
			as->cur_off = 0;
			spin_unlock_irq(&as->lock);
			break;
		}
		stopped = !as->running;
		err = as->error;
		spin_unlock_irq(&as->lock);

		if (stopped) {
			ret = err ? -EIO : 0;	/* 0 = end of file */
			goto out;
		}
		if (f->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}
		/*
		 * Sleep without read_lock? Not needed: only stop and release
		 * take it, and stop wakes us first (running = false).
		 */
		ret = wait_event_interruptible(as->wq, adc_readable(as));
		if (ret)
			goto out;
	}

	/*
	 * Copy (part of) the block. The buffer is ours: it is in neither queue,
	 * so neither the DMA nor an overrun can take it while we copy.
	 * Reading uncached memory is the slow part at high sample rates.
	 */
	n = min_t(size_t, count, as->block_bytes - as->cur_off);
	if (copy_to_user(ubuf, (u8 *)as->buf[as->cur] + as->cur_off, n)) {
		ret = -EFAULT;
		goto out;
	}
	as->cur_off += n;
	ret = n;

	if (as->cur_off == as->block_bytes) {
		spin_lock_irq(&as->lock);
		as->freeq[as->nfree++] = as->cur;
		spin_unlock_irq(&as->lock);
		as->cur = -1;
	}
out:
	mutex_unlock(&as->read_lock);
	return ret;
}

static __poll_t adc_poll(struct file *f, poll_table *wait)
{
	struct adcstream *as = file_as(f);

	poll_wait(f, &as->wq, wait);
	if (READ_ONCE(as->cur) >= 0 || adc_readable(as))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations adc_fops = {
	.owner = THIS_MODULE,
	.open = adc_open,
	.release = adc_release,
	.read = adc_read,
	.poll = adc_poll,
};

/* ------------------------------------------------------------------ */
/* sysfs                                                               */

static ssize_t enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(as->enabled));
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct adcstream *as = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	mutex_lock(&as->cfg_lock);
	if (on)
		ret = adc_start(as);
	else
		adc_stop(as);
	mutex_unlock(&as->cfg_lock);
	return ret ? ret : count;
}

static ssize_t rate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", adc_rate(as));
}

/* Can change while running: the PL picks up the new DIV at once. */
static ssize_t rate_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct adcstream *as = dev_get_drvdata(dev);
	u32 rate;
	int ret;

	ret = kstrtou32(buf, 0, &rate);
	if (ret)
		return ret;
	if (rate < 1 || rate > as->clk_hz)
		return -EINVAL;

	mutex_lock(&as->cfg_lock);
	as->div = max_t(u32, 1, DIV_ROUND_CLOSEST(as->clk_hz, rate));
	adc_wr(as, ADC_DIV, as->div);
	adc_wr(as, ADC_PHASE_INC, adc_phase_inc(as));	/* same signal frequency */
	mutex_unlock(&as->cfg_lock);
	return count;
}

static ssize_t freq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(as->freq_hz));
}

/* 0 .. rate/2. Lowering the rate later can push it above rate/2: aliasing. */
static ssize_t freq_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct adcstream *as = dev_get_drvdata(dev);
	u32 f;
	int ret;

	ret = kstrtou32(buf, 0, &f);
	if (ret)
		return ret;

	mutex_lock(&as->cfg_lock);
	if (f > adc_rate(as) / 2) {
		ret = -EINVAL;
	} else {
		as->freq_hz = f;
		adc_wr(as, ADC_PHASE_INC, adc_phase_inc(as));
	}
	mutex_unlock(&as->cfg_lock);
	return ret ? ret : count;
}

static ssize_t mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", READ_ONCE(as->sine) ? "sine" : "saw");
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct adcstream *as = dev_get_drvdata(dev);
	bool sine;

	if (sysfs_streq(buf, "sine"))
		sine = true;
	else if (sysfs_streq(buf, "saw"))
		sine = false;
	else
		return -EINVAL;

	mutex_lock(&as->cfg_lock);
	as->sine = sine;
	adc_wr(as, ADC_CTRL, adc_ctrl_mode(as) | (as->enabled ? CTRL_EN : 0));
	mutex_unlock(&as->cfg_lock);
	return count;
}

static ssize_t block_samples_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(as->block_samples));
}

static ssize_t block_samples_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct adcstream *as = dev_get_drvdata(dev);
	u32 n;
	int ret;

	ret = kstrtou32(buf, 0, &n);
	if (ret)
		return ret;
	if (n < MIN_BLOCK || n > MAX_BLOCK)
		return -EINVAL;

	mutex_lock(&as->cfg_lock);
	if (as->enabled)
		ret = -EBUSY;		/* write 0 to enable first */
	else
		as->block_samples = n;
	mutex_unlock(&as->cfg_lock);
	return ret ? ret : count;
}

static ssize_t blocks_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);
	u64 v;

	spin_lock_irq(&as->lock);
	v = as->blocks;
	spin_unlock_irq(&as->lock);
	return sysfs_emit(buf, "%llu\n", v);
}

static ssize_t overruns_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);
	u64 v;

	spin_lock_irq(&as->lock);
	v = as->overruns;
	spin_unlock_irq(&as->lock);
	return sysfs_emit(buf, "%llu\n", v);
}

static ssize_t pl_dropped_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", adc_rd(as, ADC_DROPPED));
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);
	bool running, error, reader;
	u64 blocks, overruns;

	spin_lock_irq(&as->lock);
	running = as->running;
	error = as->error;
	reader = as->reader;
	blocks = as->blocks;
	overruns = as->overruns;
	spin_unlock_irq(&as->lock);

	return sysfs_emit(buf,
			  "running=%d error=%d reader=%d rate=%u freq=%u mode=%s block_samples=%u nbuf=%u blocks=%llu overruns=%llu pl_samples=%u pl_dropped=%u pl_packets=%u\n",
			  running, error, reader, adc_rate(as), as->freq_hz,
			  as->sine ? "sine" : "saw", as->block_samples, as->nbuf,
			  blocks, overruns, adc_rd(as, ADC_SAMPLES),
			  adc_rd(as, ADC_DROPPED), adc_rd(as, ADC_PACKETS));
}

/*
 * Ask the callback for a copy of the next finished block, wait up to 1 s for
 * it (at slow rates a block takes a while), then print it. If no new block
 * came, the previous snapshot is printed again (its block number shows that).
 *
 *   block=1234 rate=1000000 step=16 n=256
 *   seq,value seq,value ...     (n pairs, every step-th sample of the block)
 */
static ssize_t snapshot_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adcstream *as = dev_get_drvdata(dev);
	unsigned int i;
	ssize_t len;

	if (mutex_lock_interruptible(&as->snap_lock))
		return -ERESTARTSYS;

	if (READ_ONCE(as->running)) {
		reinit_completion(&as->snap_done);
		spin_lock_irq(&as->lock);
		as->snap_want = true;
		spin_unlock_irq(&as->lock);
		wait_for_completion_interruptible_timeout(&as->snap_done, HZ);
		spin_lock_irq(&as->lock);
		as->snap_want = false;
		spin_unlock_irq(&as->lock);
	}

	if (!as->snap_n) {
		mutex_unlock(&as->snap_lock);
		return -ENODATA;	/* never ran */
	}

	/* 256 pairs of at most 12 characters: fits in the 4 KiB sysfs page. */
	len = sysfs_emit(buf, "block=%llu rate=%u step=%u n=%u\n",
			 as->snap_block, as->snap_rate, as->snap_step, as->snap_n);
	for (i = 0; i < as->snap_n; i++)
		len += sysfs_emit_at(buf, len, "%u,%d%c", as->snap[i] >> 16,
				     (s16)(as->snap[i] & 0xffff),
				     i + 1 < as->snap_n ? ' ' : '\n');
	mutex_unlock(&as->snap_lock);
	return len;
}

static DEVICE_ATTR_RW(enable);
static DEVICE_ATTR_RW(rate);
static DEVICE_ATTR_RW(freq);
static DEVICE_ATTR_RW(mode);
static DEVICE_ATTR_RW(block_samples);
static DEVICE_ATTR_RO(blocks);
static DEVICE_ATTR_RO(overruns);
static DEVICE_ATTR_RO(pl_dropped);
static DEVICE_ATTR_RO(status);
static DEVICE_ATTR_RO(snapshot);

static struct attribute *adcstream_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_rate.attr,
	&dev_attr_freq.attr,
	&dev_attr_mode.attr,
	&dev_attr_block_samples.attr,
	&dev_attr_blocks.attr,
	&dev_attr_overruns.attr,
	&dev_attr_pl_dropped.attr,
	&dev_attr_status.attr,
	&dev_attr_snapshot.attr,
	NULL
};
ATTRIBUTE_GROUPS(adcstream);

/* ------------------------------------------------------------------ */
/* Probe                                                               */

/* Same check as axiregs/pltimer: is the PL programmed? */
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

/* devm actions: run on unbind/rmmod in reverse order of registration. */
static void adc_action_release_chan(void *data)
{
	dma_release_channel(data);
}

static void adc_action_free_bufs(void *data)
{
	struct adcstream *as = data;
	unsigned int i;

	for (i = 0; i < as->nbuf; i++)
		if (as->buf[i])
			dma_free_coherent(as->dma_dev, MAX_BLOCK * 4, as->buf[i], as->buf_dma[i]);
}

static void adc_action_stop(void *data)
{
	struct adcstream *as = data;

	mutex_lock(&as->cfg_lock);
	adc_stop(as);
	mutex_unlock(&as->cfg_lock);
}

static void adc_action_misc(void *data)
{
	misc_deregister(data);
}

static int adcstream_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct adcstream *as;
	struct resource *res;
	struct clk *clk;
	unsigned int i;
	int ret;
	u32 id;

	ret = adc_check_pl(dev);
	if (ret)
		return ret;

	as = devm_kzalloc(dev, sizeof(*as), GFP_KERNEL);
	if (!as)
		return -ENOMEM;
	as->dev = dev;
	as->nbuf = clamp_t(unsigned int, nbuf, 4, MAX_NBUF);
	as->cur = -1;
	mutex_init(&as->cfg_lock);
	mutex_init(&as->read_lock);
	mutex_init(&as->snap_lock);
	spin_lock_init(&as->lock);
	init_waitqueue_head(&as->wq);
	init_completion(&as->snap_done);
	atomic_set(&as->open, 0);
	for (i = 0; i < MAX_NBUF; i++) {
		as->slot[i].as = as;
		as->slot[i].idx = i;
	}

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot get/enable the PL clock\n");
	as->clk_hz = clk_get_rate(clk);
	if (!as->clk_hz)
		return dev_err_probe(dev, -EINVAL, "PL clock rate is 0\n");

	as->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(as->base))
		return PTR_ERR(as->base);

	id = adc_rd(as, ADC_ID);
	if (id != ADC_ID_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "wrong ID 0x%08x (expected 0x%08x): wrong bitstream?\n",
				     id, ADC_ID_VALUE);

	/* Defaults: 1 MSa/s, 1 kHz sine, blocks of 4096 samples (4.1 ms). */
	as->div = DIV_ROUND_CLOSEST(as->clk_hz, 1000000);
	as->freq_hz = 1000;
	as->sine = true;
	as->block_samples = DEF_BLOCK;
	adc_flush(as);

	/* The S2MM channel. -EPROBE_DEFER if xilinx_dma is not loaded yet. */
	as->rx = dma_request_chan(dev, "rx");
	if (IS_ERR(as->rx))
		return dev_err_probe(dev, PTR_ERR(as->rx), "no rx DMA channel\n");
	ret = devm_add_action_or_reset(dev, adc_action_release_chan, as->rx);
	if (ret)
		return ret;

	/*
	 * The ring: nbuf buffers of the largest block size, from CMA (16 MiB
	 * by default; 8 x 256 KiB = 2 MiB). Coherent = uncached for the CPU,
	 * because the HP port bypasses the CPU caches (as in project 04).
	 */
	as->dma_dev = as->rx->device->dev;
	for (i = 0; i < as->nbuf; i++) {
		as->buf[i] = dma_alloc_coherent(as->dma_dev, MAX_BLOCK * 4,
						&as->buf_dma[i], GFP_KERNEL);
		if (!as->buf[i])
			break;
	}
	ret = devm_add_action_or_reset(dev, adc_action_free_bufs, as);
	if (ret)
		return ret;
	if (i < as->nbuf)
		return dev_err_probe(dev, -ENOMEM,
				     "cannot allocate %u x %u bytes of DMA memory (CMA too small?)\n",
				     as->nbuf, MAX_BLOCK * 4);

	ret = devm_add_action_or_reset(dev, adc_action_stop, as);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, as);	/* sysfs files use it */

	as->misc.minor = MISC_DYNAMIC_MINOR;
	as->misc.name = "adcstream";
	as->misc.fops = &adc_fops;
	as->misc.parent = dev;
	ret = misc_register(&as->misc);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register /dev/adcstream\n");
	ret = devm_add_action_or_reset(dev, adc_action_misc, &as->misc);
	if (ret)
		return ret;

	if (autostart) {
		mutex_lock(&as->cfg_lock);
		ret = adc_start(as);
		mutex_unlock(&as->cfg_lock);
		if (ret)
			dev_warn(dev, "autostart failed (%d)\n", ret);
	}

	dev_info(dev, "adcsim at %pR, ID 0x%08x, rx %s, %u buffers at %pad.., %u Sa/s, %s\n",
		 res, id, dma_chan_name(as->rx), as->nbuf, &as->buf_dma[0],
		 adc_rate(as), as->enabled ? "running" : "stopped");
	return 0;
}

static const struct of_device_id adcstream_of_match[] = {
	{ .compatible = "microzed,adcstream" },
	{ }
};
MODULE_DEVICE_TABLE(of, adcstream_of_match);

static struct platform_driver adcstream_driver = {
	.probe = adcstream_probe,
	.driver = {
		.name = "adcstream",
		.of_match_table = adcstream_of_match,
		.dev_groups = adcstream_groups,
	},
};
module_platform_driver(adcstream_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MicroZed project 05: DMA sample stream from a simulated ADC");
