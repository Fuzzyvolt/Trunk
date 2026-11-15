// SPDX-License-Identifier: GPL-2.0
/*
 * myled_irq - LED D3 + button PB1 platform driver, configured from the device tree.
 *
 *   myled {
 *       compatible = "microzed,myled";
 *       label = "d3";
 *       led-gpios = <&gpio0 47 0>;       MIO47, LED D3
 *       button-gpios = <&gpio0 51 0>;    MIO51, PB1 (1 = pressed)
 *   };
 *
 * /dev/myled     write 0, 1 or t; read the LED state (as before)
 * /dev/mybutton  read() sleeps until PB1 is pressed, then returns "press N".
 *                Supports poll()/select() and O_NONBLOCK.
 * Each press also toggles the LED (done in the interrupt thread).
 *
 * Stop leddaemon and rmmod the other myled modules first (same GPIOs, same names).
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#define DEV_NAME     "myled"
#define DEBOUNCE_MS  20

struct myled {
	struct device *dev;		/* the platform device (for logging) */
	struct gpio_desc *led;
	struct gpio_desc *button;
	struct mutex lock;		/* protects led_state */
	int led_state;
	int btn_stable;			/* last debounced button value (IRQ thread only) */
	atomic_long_t presses;		/* total presses since probe */
	wait_queue_head_t wq;		/* readers of /dev/mybutton sleep here */
	dev_t devno;			/* minor 0 = /dev/myled, minor 1 = /dev/mybutton */
	struct cdev led_cdev;
	struct cdev btn_cdev;
	const char *label;
};

/* Per-open state for /dev/mybutton: which press this reader has already seen. */
struct btn_reader {
	struct myled *ml;
	long seen;
};

static struct class *myled_class;

/* ------------------------------------------------------------------ */
/* /dev/myled: same as before                                          */

static int led_open(struct inode *inode, struct file *file)
{
	file->private_data = container_of(inode->i_cdev, struct myled, led_cdev);
	return 0;
}

static ssize_t led_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	struct myled *ml = file->private_data;
	char kbuf[4];
	int len;

	mutex_lock(&ml->lock);
	len = scnprintf(kbuf, sizeof(kbuf), "%d\n", ml->led_state);
	mutex_unlock(&ml->lock);
	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t led_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct myled *ml = file->private_data;
	char c;

	if (count == 0)
		return 0;
	if (get_user(c, buf))
		return -EFAULT;

	mutex_lock(&ml->lock);
	switch (c) {
	case '0':
		ml->led_state = 0;
		break;
	case '1':
		ml->led_state = 1;
		break;
	case 't':
		ml->led_state = !ml->led_state;
		break;
	default:
		mutex_unlock(&ml->lock);
		return -EINVAL;
	}
	gpiod_set_value_cansleep(ml->led, ml->led_state);
	mutex_unlock(&ml->lock);
	return count;
}

static const struct file_operations led_fops = {
	.owner = THIS_MODULE,
	.open  = led_open,
	.read  = led_read,
	.write = led_write,
};

/* ------------------------------------------------------------------ */
/* /dev/mybutton: blocking read, poll()                                */

static int btn_open(struct inode *inode, struct file *file)
{
	struct myled *ml = container_of(inode->i_cdev, struct myled, btn_cdev);
	struct btn_reader *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->ml = ml;
	r->seen = atomic_long_read(&ml->presses);	/* report presses from now on */
	file->private_data = r;
	return stream_open(inode, file);		/* a stream: no seeking */
}

static int btn_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static ssize_t btn_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	struct btn_reader *r = file->private_data;
	struct myled *ml = r->ml;
	char kbuf[32];
	long now;
	int len, ret;

	if (file->f_flags & O_NONBLOCK) {
		if (atomic_long_read(&ml->presses) == r->seen)
			return -EAGAIN;		/* "try again later" */
	} else {
		/*
		 * Sleep (no CPU used) until the IRQ thread counts a new press.
		 * Returns early with -ERESTARTSYS if a signal (Ctrl-C) arrives.
		 */
		ret = wait_event_interruptible(ml->wq,
				atomic_long_read(&ml->presses) != r->seen);
		if (ret)
			return ret;
	}

	now = atomic_long_read(&ml->presses);
	r->seen = now;
	len = scnprintf(kbuf, sizeof(kbuf), "press %ld\n", now);
	if (len > count)
		len = count;
	if (copy_to_user(buf, kbuf, len))
		return -EFAULT;
	return len;
}

/* poll()/select(): "readable" when there is a press this reader hasn't seen. */
static __poll_t btn_poll(struct file *file, poll_table *wait)
{
	struct btn_reader *r = file->private_data;

	poll_wait(file, &r->ml->wq, wait);
	if (atomic_long_read(&r->ml->presses) != r->seen)
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations btn_fops = {
	.owner   = THIS_MODULE,
	.open    = btn_open,
	.release = btn_release,
	.read    = btn_read,
	.poll    = btn_poll,
};

/* ------------------------------------------------------------------ */
/* Interrupt                                                           */

/*
 * Threaded interrupt handler ("bottom half"). It runs in a kernel thread,
 * so it may sleep (msleep, mutex). The line stays masked until it returns
 * (IRQF_ONESHOT). Runs on every edge, rising and falling.
 */
static irqreturn_t btn_irq_thread(int irq, void *data)
{
	struct myled *ml = data;
	long n;
	int v;

	msleep(DEBOUNCE_MS);			/* let the contacts stop bouncing */
	v = gpiod_get_value_cansleep(ml->button);
	if (v < 0 || v == ml->btn_stable)
		return IRQ_HANDLED;		/* bounce, or no real change */
	ml->btn_stable = v;

	if (v == 1) {				/* released -> pressed */
		mutex_lock(&ml->lock);
		ml->led_state = !ml->led_state;
		gpiod_set_value_cansleep(ml->led, ml->led_state);
		mutex_unlock(&ml->lock);

		n = atomic_long_inc_return(&ml->presses);
		wake_up_interruptible(&ml->wq);	/* wake sleeping readers */
		dev_info(ml->dev, "press %ld, LED %s\n", n, ml->led_state ? "on" : "off");
	}
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Probe / remove                                                      */

static int myled_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *d;
	struct myled *ml;
	int irq, ret, major;

	ml = devm_kzalloc(dev, sizeof(*ml), GFP_KERNEL);
	if (!ml)
		return -ENOMEM;
	ml->dev = dev;
	mutex_init(&ml->lock);
	init_waitqueue_head(&ml->wq);
	atomic_long_set(&ml->presses, 0);

	if (device_property_read_string(dev, "label", &ml->label))
		ml->label = "led";

	ml->led = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(ml->led))
		return dev_err_probe(dev, PTR_ERR(ml->led), "cannot get led-gpios\n");

	ml->button = devm_gpiod_get(dev, "button", GPIOD_IN);
	if (IS_ERR(ml->button))
		return dev_err_probe(dev, PTR_ERR(ml->button),
				     "cannot get button-gpios (missing in the DT node, or in use?)\n");
	ml->btn_stable = gpiod_get_value_cansleep(ml->button);
	if (ml->btn_stable < 0)
		ml->btn_stable = 0;

	/* Everything the IRQ thread uses is ready, so the interrupt can be requested. */
	irq = gpiod_to_irq(ml->button);
	if (irq < 0)
		return dev_err_probe(dev, irq, "button has no interrupt\n");
	ret = devm_request_threaded_irq(dev, irq, NULL, btn_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"myled-button", ml);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request IRQ %d\n", irq);

	/* Two device numbers: minor 0 = myled, minor 1 = mybutton. */
	ret = alloc_chrdev_region(&ml->devno, 0, 2, DEV_NAME);
	if (ret)
		return ret;
	major = MAJOR(ml->devno);

	d = device_create(myled_class, dev, MKDEV(major, 0), ml, "myled");
	if (IS_ERR(d)) {
		ret = PTR_ERR(d);
		goto err_region;
	}
	d = device_create(myled_class, dev, MKDEV(major, 1), ml, "mybutton");
	if (IS_ERR(d)) {
		ret = PTR_ERR(d);
		goto err_dev0;
	}

	cdev_init(&ml->led_cdev, &led_fops);
	ml->led_cdev.owner = THIS_MODULE;
	ret = cdev_add(&ml->led_cdev, MKDEV(major, 0), 1);
	if (ret)
		goto err_dev1;

	cdev_init(&ml->btn_cdev, &btn_fops);
	ml->btn_cdev.owner = THIS_MODULE;
	ret = cdev_add(&ml->btn_cdev, MKDEV(major, 1), 1);
	if (ret)
		goto err_cdev0;

	platform_set_drvdata(pdev, ml);
	dev_info(dev, "bound to %pOF: LED '%s', button IRQ %d, /dev/myled + /dev/mybutton (major %d)\n",
		 dev->of_node, ml->label, irq, major);
	return 0;

err_cdev0:
	cdev_del(&ml->led_cdev);
err_dev1:
	device_destroy(myled_class, MKDEV(major, 1));
err_dev0:
	device_destroy(myled_class, MKDEV(major, 0));
err_region:
	unregister_chrdev_region(ml->devno, 2);
	return ret;
}

static void myled_remove_common(struct platform_device *pdev)
{
	struct myled *ml = platform_get_drvdata(pdev);
	int major = MAJOR(ml->devno);

	cdev_del(&ml->btn_cdev);
	cdev_del(&ml->led_cdev);
	device_destroy(myled_class, MKDEV(major, 1));
	device_destroy(myled_class, MKDEV(major, 0));
	unregister_chrdev_region(ml->devno, 2);

	mutex_lock(&ml->lock);
	gpiod_set_value_cansleep(ml->led, 0);
	mutex_unlock(&ml->lock);
	/* The IRQ and the GPIOs are devm_*: freed automatically after this returns. */
	dev_info(&pdev->dev, "unbound after %ld presses\n", atomic_long_read(&ml->presses));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void myled_remove(struct platform_device *pdev)
{
	myled_remove_common(pdev);
}
#else
static int myled_remove(struct platform_device *pdev)
{
	myled_remove_common(pdev);
	return 0;
}
#endif

static const struct of_device_id myled_of_match[] = {
	{ .compatible = "microzed,myled" },
	{ }
};
MODULE_DEVICE_TABLE(of, myled_of_match);

static struct platform_driver myled_driver = {
	.probe  = myled_probe,
	.remove = myled_remove,
	.driver = {
		.name = "myled",
		.of_match_table = myled_of_match,
	},
};

static int __init myled_init(void)
{
	int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	myled_class = class_create(DEV_NAME);
#else
	myled_class = class_create(THIS_MODULE, DEV_NAME);
#endif
	if (IS_ERR(myled_class))
		return PTR_ERR(myled_class);

	ret = platform_driver_register(&myled_driver);
	if (ret)
		class_destroy(myled_class);
	return ret;
}

static void __exit myled_exit(void)
{
	platform_driver_unregister(&myled_driver);
	class_destroy(myled_class);
}

module_init(myled_init);
module_exit(myled_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LED + button (interrupt) platform driver for the MicroZed");
