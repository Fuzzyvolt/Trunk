// SPDX-License-Identifier: GPL-2.0
/*
 * pltimer - driver for the project 03 PL timer: an interrupt every N microseconds.
 *
 * Device tree node (added by board/post-image-dt.sh):
 *
 *   pltimer@43c10000 {
 *       compatible = "microzed,pltimer";
 *       reg = <0x43c10000 0x1000>;      <- 4 KB of registers
 *       interrupt-parent = <&intc>;      <- the GIC
 *       interrupts = <0 29 4>;           <- SPI 29 = GIC ID 61 = IRQ_F2P[0], 4 = level high
 *       clocks = <&clkc 15>;             <- FCLK_CLK0 (100 MHz), the timer counts it
 *       fpga-mgr = <&devcfg>;            <- to check that the PL is programmed
 *   };
 *
 * /dev/pltimer   read() sleeps until the next tick, then returns one line:
 *                  "tick 42 pl 42 t 1234.567890"
 *                tick = interrupts handled, pl = the PL's own tick counter,
 *                t = kernel time of the interrupt (seconds since boot).
 *                Supports poll()/select() and O_NONBLOCK, like /dev/mybutton.
 *
 * sysfs files in /sys/bus/platform/devices/43c10000.pltimer/:
 *   period_us    read/write: tick period in microseconds (default 1000000 = 1 s)
 *   enable       read/write: 1 = timer running, 0 = stopped
 *   ticks        read: interrupts handled since the driver loaded
 *   pl_ticks     read: ticks counted by the PL since the last start
 *   missed       read: ticks the PL made that did not get their own interrupt
 *   stats        read: interval between interrupts (min/avg/max, ns); write anything to reset
 *
 * The interrupt handler is a plain (top-half) handler: it only reads and writes
 * a few registers, so there is no reason to sleep and no need for a thread.
 * Shared state is protected by a spinlock, because a top half may not take a mutex.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
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
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

/* Register offsets, same as pltimer.v */
#define PLT_CTRL	0x00
#define PLT_PERIOD	0x04
#define PLT_STATUS	0x08
#define PLT_TICKS	0x0c
#define PLT_COUNT	0x10
#define PLT_ID		0x14

#define CTRL_EN		BIT(0)	/* timer runs */
#define CTRL_IE		BIT(1)	/* interrupt output on */
#define STATUS_PENDING	BIT(0)	/* write 1 to clear */

#define PLT_ID_VALUE	0x4d5a0301

/* Below ~20 us the CPU spends most of its time in the handler. */
#define PERIOD_US_MIN	20

static unsigned int period_us = 1000000;
module_param(period_us, uint, 0444);
MODULE_PARM_DESC(period_us, "Tick period at load time, in microseconds (default 1000000)");

struct pltimer {
	struct device *dev;
	void __iomem *base;
	unsigned long clk_hz;		/* FCLK_CLK0 rate: cycles per second */
	struct miscdevice misc;		/* /dev/pltimer */
	wait_queue_head_t wq;		/* readers sleep here */

	struct mutex cfg_lock;		/* sysfs writers: period_us, running */
	u32 period_us;
	bool running;

	spinlock_t lock;		/* shared with the interrupt handler: */
	unsigned long count;		/*   interrupts handled since probe */
	unsigned long missed;		/*   PL ticks with no interrupt of their own */
	u32 last_pl;			/*   PL TICKS value at the last interrupt */
	u64 last_ns;			/*   time of the last interrupt (0 = none yet) */
	u64 min_ns, max_ns, sum_ns;	/*   intervals between interrupts */
	unsigned long n_intervals;
};

/* Per-open state for /dev/pltimer: the last tick this reader has seen. */
struct plt_reader {
	struct pltimer *pt;
	unsigned long seen;
};

/* ------------------------------------------------------------------ */
/* Interrupt                                                           */

/*
 * Top-half handler. Runs in interrupt context with this IRQ masked:
 * it must not sleep (no mutex, no msleep, no copy_to_user).
 *
 * The interrupt is level-triggered: the PL keeps the line high until we
 * write 1 to STATUS. Forget that and the GIC calls us again the moment
 * we return, forever: the board locks up.
 */
static irqreturn_t pltimer_irq(int irq, void *data)
{
	struct pltimer *pt = data;
	u64 now = ktime_get_ns();
	u32 status, pl, delta;

	status = readl(pt->base + PLT_STATUS);
	if (!(status & STATUS_PENDING))
		return IRQ_NONE;		/* not ours (or already cleared) */

	writel(STATUS_PENDING, pt->base + PLT_STATUS);	/* clear: line goes low */
	/*
	 * A write to a device can be "posted": the CPU moves on before it
	 * reaches the PL. Reading a register back waits for it, so the line
	 * is really low before the GIC looks at it again.
	 */
	pl = readl(pt->base + PLT_TICKS);

	spin_lock(&pt->lock);
	/* The PL counts every tick. If two ticks came between two interrupts
	 * (we were too late), they merged into one pending bit: count the gap. */
	delta = pl - pt->last_pl;
	if (delta > 1)
		pt->missed += delta - 1;
	pt->last_pl = pl;

	if (pt->last_ns) {
		u64 iv = now - pt->last_ns;

		if (!pt->n_intervals || iv < pt->min_ns)
			pt->min_ns = iv;
		if (iv > pt->max_ns)
			pt->max_ns = iv;
		pt->sum_ns += iv;
		pt->n_intervals++;
	}
	pt->last_ns = now;
	pt->count++;
	spin_unlock(&pt->lock);

	wake_up_interruptible(&pt->wq);		/* wake sleeping readers */
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Timer control (called with cfg_lock held)                           */

static u32 us_to_cycles(struct pltimer *pt, u32 us)
{
	/* 64-bit division on a 32-bit CPU: use div_u64, not "/" */
	return (u32)div_u64((u64)us * pt->clk_hz, 1000000);
}

/* Forget the interval statistics (after a start or a period change). */
static void pltimer_reset_stats(struct pltimer *pt)
{
	spin_lock_irq(&pt->lock);
	pt->last_ns = 0;
	pt->min_ns = pt->max_ns = pt->sum_ns = 0;
	pt->n_intervals = 0;
	spin_unlock_irq(&pt->lock);
}

static void pltimer_hw_stop(struct pltimer *pt)
{
	writel(0, pt->base + PLT_CTRL);
	writel(STATUS_PENDING, pt->base + PLT_STATUS);
	readl(pt->base + PLT_STATUS);		/* make sure both writes arrived */
	pt->running = false;
}

static void pltimer_hw_start(struct pltimer *pt)
{
	pltimer_hw_stop(pt);
	writel(0, pt->base + PLT_TICKS);	/* any write zeroes the PL counter */
	spin_lock_irq(&pt->lock);
	pt->last_pl = 0;
	spin_unlock_irq(&pt->lock);
	pltimer_reset_stats(pt);
	writel(us_to_cycles(pt, pt->period_us), pt->base + PLT_PERIOD);
	writel(CTRL_EN | CTRL_IE, pt->base + PLT_CTRL);
	pt->running = true;
}

/* ------------------------------------------------------------------ */
/* /dev/pltimer                                                        */

static bool pltimer_has_new(struct pltimer *pt, unsigned long seen)
{
	return READ_ONCE(pt->count) != seen;
}

static int pltimer_open(struct inode *inode, struct file *file)
{
	/* misc_open() put our struct miscdevice in private_data. */
	struct pltimer *pt = container_of(file->private_data, struct pltimer, misc);
	struct plt_reader *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->pt = pt;
	r->seen = READ_ONCE(pt->count);		/* report ticks from now on */
	file->private_data = r;
	return stream_open(inode, file);
}

static int pltimer_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static ssize_t pltimer_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct plt_reader *r = file->private_data;
	struct pltimer *pt = r->pt;
	unsigned long n;
	u32 pl, usec, rem;
	u64 t, sec;
	char kbuf[64];
	int len, ret;

	if (file->f_flags & O_NONBLOCK) {
		if (!pltimer_has_new(pt, r->seen))
			return -EAGAIN;
	} else {
		/* Sleep until the handler counts a new tick, or a signal (Ctrl-C). */
		ret = wait_event_interruptible(pt->wq, pltimer_has_new(pt, r->seen));
		if (ret)
			return ret;
	}

	/* Take a consistent snapshot. _irq: the handler takes this lock too,
	 * so keep interrupts off while we hold it or it could deadlock. */
	spin_lock_irq(&pt->lock);
	n = pt->count;
	pl = pt->last_pl;
	t = pt->last_ns;
	spin_unlock_irq(&pt->lock);

	/* If this reader was too slow, n jumps by more than 1: it sees the gap. */
	r->seen = n;

	sec = div_u64_rem(t, NSEC_PER_SEC, &rem);
	usec = rem / 1000;
	len = scnprintf(kbuf, sizeof(kbuf), "tick %lu pl %u t %llu.%06u\n", n, pl, sec, usec);
	if (len > count)
		len = count;
	if (copy_to_user(buf, kbuf, len))
		return -EFAULT;
	return len;
}

static __poll_t pltimer_poll(struct file *file, poll_table *wait)
{
	struct plt_reader *r = file->private_data;

	poll_wait(file, &r->pt->wq, wait);
	if (pltimer_has_new(r->pt, r->seen))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations pltimer_fops = {
	.owner   = THIS_MODULE,
	.open    = pltimer_open,
	.release = pltimer_release,
	.read    = pltimer_read,
	.poll    = pltimer_poll,
};

/* ------------------------------------------------------------------ */
/* sysfs                                                               */

static ssize_t period_us_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pltimer *pt = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(pt->period_us));
}

static ssize_t period_us_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct pltimer *pt = dev_get_drvdata(dev);
	u32 us, max_us;
	int ret;

	ret = kstrtou32(buf, 0, &us);
	if (ret)
		return ret;
	max_us = (u32)div_u64((u64)U32_MAX * 1000000, pt->clk_hz);	/* ~42.9 s */
	if (us < PERIOD_US_MIN || us > max_us)
		return -ERANGE;		/* "Numerical result out of range" */

	mutex_lock(&pt->cfg_lock);
	pt->period_us = us;
	/* Takes effect at once; the PL ends the current period early if needed. */
	writel(us_to_cycles(pt, us), pt->base + PLT_PERIOD);
	pltimer_reset_stats(pt);
	mutex_unlock(&pt->cfg_lock);
	return count;
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pltimer *pt = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(pt->running));
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct pltimer *pt = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	mutex_lock(&pt->cfg_lock);
	if (on && !pt->running)
		pltimer_hw_start(pt);
	else if (!on && pt->running)
		pltimer_hw_stop(pt);
	mutex_unlock(&pt->cfg_lock);
	return count;
}

static ssize_t ticks_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pltimer *pt = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(pt->count));
}

static ssize_t pl_ticks_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pltimer *pt = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", readl(pt->base + PLT_TICKS));
}

static ssize_t missed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pltimer *pt = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", READ_ONCE(pt->missed));
}

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pltimer *pt = dev_get_drvdata(dev);
	unsigned long n;
	u64 mn, mx, sum, avg = 0;

	spin_lock_irq(&pt->lock);
	n = pt->n_intervals;
	mn = pt->min_ns;
	mx = pt->max_ns;
	sum = pt->sum_ns;
	spin_unlock_irq(&pt->lock);

	if (n)
		avg = div_u64(sum, n);
	return sysfs_emit(buf, "intervals %lu min_ns %llu avg_ns %llu max_ns %llu\n",
			  n, mn, avg, mx);
}

static ssize_t stats_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct pltimer *pt = dev_get_drvdata(dev);

	pltimer_reset_stats(pt);
	return count;
}

static DEVICE_ATTR_RW(period_us);
static DEVICE_ATTR_RW(enable);
static DEVICE_ATTR_RO(ticks);
static DEVICE_ATTR_RO(pl_ticks);
static DEVICE_ATTR_RO(missed);
static DEVICE_ATTR_RW(stats);

static struct attribute *pltimer_attrs[] = {
	&dev_attr_period_us.attr,
	&dev_attr_enable.attr,
	&dev_attr_ticks.attr,
	&dev_attr_pl_ticks.attr,
	&dev_attr_missed.attr,
	&dev_attr_stats.attr,
	NULL
};
ATTRIBUTE_GROUPS(pltimer);

/* ------------------------------------------------------------------ */
/* Probe                                                               */

/* Same check as axiregs: is the PL programmed? (Reading it otherwise hangs the CPU.) */
static int pltimer_check_pl(struct device *dev)
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

/* devm "actions": run automatically on unbind/rmmod, in reverse order. */
static void pltimer_action_stop(void *data)
{
	pltimer_hw_stop(data);
}

static void pltimer_action_misc(void *data)
{
	misc_deregister(data);
}

static int pltimer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pltimer *pt;
	struct resource *res;
	struct clk *clk;
	int irq, ret;
	u32 id;

	ret = pltimer_check_pl(dev);
	if (ret)
		return ret;

	pt = devm_kzalloc(dev, sizeof(*pt), GFP_KERNEL);
	if (!pt)
		return -ENOMEM;
	pt->dev = dev;
	mutex_init(&pt->cfg_lock);
	spin_lock_init(&pt->lock);
	init_waitqueue_head(&pt->wq);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot get/enable the PL clock\n");
	pt->clk_hz = clk_get_rate(clk);
	if (!pt->clk_hz)
		return dev_err_probe(dev, -EINVAL, "PL clock rate is 0\n");

	pt->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(pt->base))
		return PTR_ERR(pt->base);

	id = readl(pt->base + PLT_ID);
	if (id != PLT_ID_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "wrong ID 0x%08x (expected 0x%08x): wrong bitstream?\n",
				     id, PLT_ID_VALUE);

	/* Quiet hardware before the interrupt is requested. */
	pltimer_hw_stop(pt);

	/*
	 * The "interrupts" property, translated by the GIC driver into a Linux
	 * IRQ number (not 61: Linux numbers its interrupts its own way).
	 */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	/* Trigger type (level high) comes from the device tree, so flags = 0. */
	ret = devm_request_irq(dev, irq, pltimer_irq, 0, "pltimer", pt);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request IRQ %d\n", irq);

	/* Registered after the IRQ, so it runs before free_irq on unbind. */
	ret = devm_add_action_or_reset(dev, pltimer_action_stop, pt);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, pt);	/* sysfs files use it */

	pt->period_us = clamp_t(u32, period_us, PERIOD_US_MIN, 42000000);
	mutex_lock(&pt->cfg_lock);
	pltimer_hw_start(pt);
	mutex_unlock(&pt->cfg_lock);

	/* A "misc" device: one call gives a /dev node, no cdev/class needed. */
	pt->misc.minor = MISC_DYNAMIC_MINOR;
	pt->misc.name = "pltimer";
	pt->misc.fops = &pltimer_fops;
	pt->misc.parent = dev;
	ret = misc_register(&pt->misc);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register /dev/pltimer\n");
	ret = devm_add_action_or_reset(dev, pltimer_action_misc, &pt->misc);
	if (ret)
		return ret;

	dev_info(dev, "pltimer at %pR, IRQ %d, ID 0x%08x, PL clock %lu Hz, period %u us\n",
		 res, irq, id, pt->clk_hz, pt->period_us);
	return 0;
}

static const struct of_device_id pltimer_of_match[] = {
	{ .compatible = "microzed,pltimer" },
	{ }
};
MODULE_DEVICE_TABLE(of, pltimer_of_match);

static struct platform_driver pltimer_driver = {
	.probe = pltimer_probe,
	.driver = {
		.name = "pltimer",
		.of_match_table = pltimer_of_match,
		.dev_groups = pltimer_groups,
	},
};
module_platform_driver(pltimer_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MicroZed project 03: PL timer with interrupt");
