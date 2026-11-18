// SPDX-License-Identifier: GPL-2.0
/*
 * dmaloop - DMA engine client for the project 04 AXI DMA loopback.
 *
 *   tx_buf (DDR) --MM2S--> AXI4-Stream FIFO (PL) --S2MM--> rx_buf (DDR)
 *
 * The AXI DMA itself is driven by the kernel's Xilinx DMA driver
 * (xilinx_dma.ko). This driver is a "client": it asks the DMA engine
 * framework for two channels and hands it buffers to move. It never touches
 * the DMA's registers.
 *
 * Device tree node (added by board/post-image-dt.sh):
 *
 *   dmaloop {
 *       compatible = "microzed,dmaloop";
 *       dmas = <&axi_dma 0>, <&axi_dma 1>;   <- channel 0 = MM2S, 1 = S2MM
 *       dma-names = "tx", "rx";              <- names for dma_request_chan()
 *   };
 *
 * sysfs files in /sys/bus/platform/devices/dmaloop/:
 *   size     read/write: bytes per transfer (multiple of 4, 4 .. 4 MiB)
 *   run      write N: do N loopbacks of "size" bytes, check every one
 *   result   read: outcome of the last run, as key=value pairs
 *
 * Load order: bitstream, then xilinx_dma, then this (S40pl does all three).
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>

/*
 * Largest transfer. The DMA's length register is 23 bits (8 MiB - 1, set in
 * Vivado and in xlnx,sg-length-width). Both buffers come from the CMA pool
 * (16 MiB by default), so 2 x 4 MiB fits.
 */
#define DMALOOP_MAX_SIZE	(4 * 1024 * 1024)
#define DMALOOP_DEF_SIZE	(64 * 1024)
#define DMALOOP_MAX_COUNT	100000
#define DMALOOP_TIMEOUT_MS	1000

/* One direction: its completion and the result the DMA driver reported. */
struct dmaloop_xfer {
	struct completion done;
	enum dmaengine_tx_result res;
};

struct dmaloop {
	struct device *dev;
	struct dma_chan *tx, *rx;	/* MM2S and S2MM channels */
	struct device *dma_dev;		/* the DMA controller (does the memory access) */
	u32 *tx_buf, *rx_buf;		/* CPU addresses of the buffers */
	dma_addr_t tx_dma, rx_dma;	/* the same buffers as the DMA sees them */
	struct dmaloop_xfer tx_x, rx_x;
	struct mutex lock;		/* one run at a time; protects size, result */
	u32 size;
	u32 seed;			/* changes every transfer, so old data never passes */
	char result[192];
};

/*
 * Completion callback. The Xilinx DMA driver calls it from its tasklet
 * (bottom half, may not sleep) after the DMA's interrupt says "done".
 */
static void dmaloop_callback(void *param, const struct dmaengine_result *result)
{
	struct dmaloop_xfer *x = param;

	x->res = result ? result->result : DMA_TRANS_NOERROR;
	complete(&x->done);
}

/*
 * Queue one transfer on a channel: prepare a descriptor ("move len bytes
 * at buf in this direction"), attach the callback, submit it. Submitting only
 * queues it; nothing moves until dma_async_issue_pending().
 */
static int dmaloop_queue(struct dma_chan *chan, dma_addr_t buf, u32 len,
			 enum dma_transfer_direction dir, struct dmaloop_xfer *x)
{
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;

	desc = dmaengine_prep_slave_single(chan, buf, len, dir,
					   DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -ENOMEM;

	reinit_completion(&x->done);
	x->res = DMA_TRANS_ABORTED;
	desc->callback_result = dmaloop_callback;
	desc->callback_param = x;

	cookie = dmaengine_submit(desc);
	return dma_submit_error(cookie) ? -EIO : 0;
}

/* One loopback of len bytes. Returns 0 and the DMA time in *ns, or an error. */
static int dmaloop_one(struct dmaloop *dl, u32 len, u64 *ns)
{
	ktime_t t0;
	int ret;

	/*
	 * Receive side first: S2MM must be ready before data arrives. (If MM2S
	 * started first, the FIFO would fill and MM2S would just wait, but a
	 * real ADC stream cannot wait, so get the habit right.)
	 */
	ret = dmaloop_queue(dl->rx, dl->rx_dma, len, DMA_DEV_TO_MEM, &dl->rx_x);
	if (ret)
		return ret;
	ret = dmaloop_queue(dl->tx, dl->tx_dma, len, DMA_MEM_TO_DEV, &dl->tx_x);
	if (ret) {
		dmaengine_terminate_sync(dl->rx);
		return ret;
	}

	t0 = ktime_get();
	dma_async_issue_pending(dl->rx);
	dma_async_issue_pending(dl->tx);

	/* Sleep until both callbacks ran (or time out). */
	if (!wait_for_completion_timeout(&dl->rx_x.done, msecs_to_jiffies(DMALOOP_TIMEOUT_MS)) ||
	    !wait_for_completion_timeout(&dl->tx_x.done, msecs_to_jiffies(DMALOOP_TIMEOUT_MS))) {
		dmaengine_terminate_sync(dl->tx);
		dmaengine_terminate_sync(dl->rx);
		return -ETIMEDOUT;
	}
	*ns = ktime_to_ns(ktime_sub(ktime_get(), t0));

	if (dl->tx_x.res != DMA_TRANS_NOERROR || dl->rx_x.res != DMA_TRANS_NOERROR)
		return -EIO;
	return 0;
}

static int dmaloop_run(struct dmaloop *dl, u32 count)
{
	u32 len = dl->size, words = len / 4;
	u64 ns, total_ns = 0, min_ns = U64_MAX, max_ns = 0;
	u32 i, w;
	int ret;

	for (i = 0; i < count; i++) {
		u32 seed = dl->seed++;

		/* Test pattern: different for every word and every transfer. */
		for (w = 0; w < words; w++)
			dl->tx_buf[w] = seed ^ (w * 0x9e3779b1);
		memset(dl->rx_buf, 0, len);

		ret = dmaloop_one(dl, len, &ns);
		if (ret) {
			snprintf(dl->result, sizeof(dl->result),
				 "FAIL size=%u transfer=%u error=%d (%s)\n", len, i, ret,
				 ret == -ETIMEDOUT ? "timeout: no DMA interrupt" : "DMA error");
			return ret;
		}

		if (memcmp(dl->tx_buf, dl->rx_buf, len)) {
			for (w = 0; w < words && dl->rx_buf[w] == dl->tx_buf[w]; w++)
				;
			snprintf(dl->result, sizeof(dl->result),
				 "FAIL size=%u transfer=%u mismatch at byte %u: sent 0x%08x got 0x%08x\n",
				 len, i, w * 4, dl->tx_buf[w], dl->rx_buf[w]);
			return -EIO;
		}

		total_ns += ns;
		min_ns = min(min_ns, ns);
		max_ns = max(max_ns, ns);
	}

	/* MB/s = bytes / us = bytes * 1000 / ns (1 MB = 10^6 bytes). */
	snprintf(dl->result, sizeof(dl->result),
		 "ok size=%u count=%u avg_us=%llu min_us=%llu max_us=%llu MBps=%llu\n",
		 len, count, div_u64(total_ns, count * 1000ULL),
		 div_u64(min_ns, 1000), div_u64(max_ns, 1000),
		 div64_u64((u64)len * count * 1000, total_ns ? total_ns : 1));
	return 0;
}

/* ---------------- sysfs ---------------- */

static ssize_t size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dmaloop *dl = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(dl->size));
}

static ssize_t size_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct dmaloop *dl = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;
	if (val < 4 || val > DMALOOP_MAX_SIZE || val % 4)
		return -EINVAL;

	mutex_lock(&dl->lock);
	dl->size = val;
	mutex_unlock(&dl->lock);
	return count;
}

/* Writing to "run" blocks until all transfers are done (or one fails). */
static ssize_t run_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct dmaloop *dl = dev_get_drvdata(dev);
	u32 n;
	int ret;

	ret = kstrtou32(buf, 0, &n);
	if (ret)
		return ret;
	if (n < 1 || n > DMALOOP_MAX_COUNT)
		return -EINVAL;

	if (mutex_lock_interruptible(&dl->lock))
		return -ERESTARTSYS;
	ret = dmaloop_run(dl, n);
	mutex_unlock(&dl->lock);

	return ret ? ret : count;
}

static ssize_t result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dmaloop *dl = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&dl->lock);
	len = sysfs_emit(buf, "%s", dl->result);
	mutex_unlock(&dl->lock);
	return len;
}

static DEVICE_ATTR_RW(size);
static DEVICE_ATTR_WO(run);
static DEVICE_ATTR_RO(result);

static struct attribute *dmaloop_attrs[] = {
	&dev_attr_size.attr,
	&dev_attr_run.attr,
	&dev_attr_result.attr,
	NULL
};
ATTRIBUTE_GROUPS(dmaloop);

/* ---------------- probe / remove ---------------- */

static void dmaloop_free(struct dmaloop *dl)
{
	if (dl->rx_buf)
		dma_free_coherent(dl->dma_dev, DMALOOP_MAX_SIZE, dl->rx_buf, dl->rx_dma);
	if (dl->tx_buf)
		dma_free_coherent(dl->dma_dev, DMALOOP_MAX_SIZE, dl->tx_buf, dl->tx_dma);
	if (!IS_ERR_OR_NULL(dl->rx))
		dma_release_channel(dl->rx);
	if (!IS_ERR_OR_NULL(dl->tx))
		dma_release_channel(dl->tx);
}

static int dmaloop_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dmaloop *dl;
	int ret;

	dl = devm_kzalloc(dev, sizeof(*dl), GFP_KERNEL);
	if (!dl)
		return -ENOMEM;
	dl->dev = dev;
	dl->size = DMALOOP_DEF_SIZE;
	mutex_init(&dl->lock);
	init_completion(&dl->tx_x.done);
	init_completion(&dl->rx_x.done);
	strscpy(dl->result, "none (write a count to run)\n", sizeof(dl->result));

	/*
	 * Look up "tx" and "rx" in dma-names, follow the matching dmas entry to
	 * the DMA node, and ask its driver (xilinx_dma) for that channel. If
	 * xilinx_dma is not loaded yet this returns -EPROBE_DEFER, and the
	 * kernel retries the probe when it loads.
	 */
	dl->tx = dma_request_chan(dev, "tx");
	if (IS_ERR(dl->tx))
		return dev_err_probe(dev, PTR_ERR(dl->tx), "no tx DMA channel\n");
	dl->rx = dma_request_chan(dev, "rx");
	if (IS_ERR(dl->rx)) {
		ret = dev_err_probe(dev, PTR_ERR(dl->rx), "no rx DMA channel\n");
		goto err;
	}

	/*
	 * Buffers. The PL reaches DDR through the HP port, which does NOT go
	 * through the CPU caches. dma_alloc_coherent() gives memory that is
	 * uncached for the CPU, so both sides always see the same data. It is
	 * allocated for the DMA controller's device, the one that does the
	 * memory access (its DMA mask and address limits apply).
	 */
	dl->dma_dev = dl->tx->device->dev;
	dl->tx_buf = dma_alloc_coherent(dl->dma_dev, DMALOOP_MAX_SIZE, &dl->tx_dma, GFP_KERNEL);
	dl->rx_buf = dma_alloc_coherent(dl->dma_dev, DMALOOP_MAX_SIZE, &dl->rx_dma, GFP_KERNEL);
	if (!dl->tx_buf || !dl->rx_buf) {
		ret = dev_err_probe(dev, -ENOMEM,
				    "cannot allocate 2 x %u bytes of DMA memory (CMA too small?)\n",
				    DMALOOP_MAX_SIZE);
		goto err;
	}

	platform_set_drvdata(pdev, dl);
	dev_info(dev, "tx %s, rx %s, buffers tx %pad rx %pad (%u bytes each)\n",
		 dma_chan_name(dl->tx), dma_chan_name(dl->rx),
		 &dl->tx_dma, &dl->rx_dma, DMALOOP_MAX_SIZE);
	return 0;

err:
	dmaloop_free(dl);
	return ret;
}

static void dmaloop_remove_common(struct platform_device *pdev)
{
	struct dmaloop *dl = platform_get_drvdata(pdev);

	/* sysfs files are already gone here, so no run can be in progress. */
	dmaengine_terminate_sync(dl->tx);
	dmaengine_terminate_sync(dl->rx);
	dmaloop_free(dl);
}

/* platform_driver.remove changed from returning int to void in kernel 6.11. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void dmaloop_remove(struct platform_device *pdev)
{
	dmaloop_remove_common(pdev);
}
#else
static int dmaloop_remove(struct platform_device *pdev)
{
	dmaloop_remove_common(pdev);
	return 0;
}
#endif

static const struct of_device_id dmaloop_of_match[] = {
	{ .compatible = "microzed,dmaloop" },
	{ }
};
MODULE_DEVICE_TABLE(of, dmaloop_of_match);

static struct platform_driver dmaloop_driver = {
	.probe = dmaloop_probe,
	.remove = dmaloop_remove,
	.driver = {
		.name = "dmaloop",
		.of_match_table = dmaloop_of_match,
		.dev_groups = dmaloop_groups,
	},
};
module_platform_driver(dmaloop_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MicroZed project 04: AXI DMA loopback through a PL FIFO");
