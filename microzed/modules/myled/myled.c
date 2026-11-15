// SPDX-License-Identifier: GPL-2.0
/*
 * myled - character driver for LED D3 (MIO47) on the MicroZed.
 *
 *   echo 1 > /dev/myled     LED on
 *   echo 0 > /dev/myled     LED off
 *   echo t > /dev/myled     toggle
 *   cat /dev/myled          prints 0 or 1
 *
 * Only one user of a GPIO line at a time: stop leddaemon before loading
 * (/etc/init.d/S99leddaemon stop), or insmod fails with "Device or resource busy".
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define DEV_NAME   "myled"
#define GPIO_CHIP  "zynq_gpio"	/* label shown by gpiodetect */
#define LED_HWNUM  47		/* MIO47 = LED D3 */

static dev_t devno;			/* major:minor number of /dev/myled */
static struct cdev myled_cdev;		/* connects devno to our file_operations */
static struct class *myled_class;	/* /sys/class/myled */
static struct device *myled_dev;	/* /sys/class/myled/myled -> /dev/myled */
static struct gpio_desc *led;		/* the LED line, from the GPIO subsystem */
static DEFINE_MUTEX(myled_lock);	/* protects led_state */
static int led_state;

/*
 * Tell the GPIO subsystem which line "led" means for the device named "myled".
 * (A device tree node would normally do this; that is the next exercise.)
 */
static struct gpiod_lookup_table myled_gpios = {
	.dev_id = DEV_NAME,
	.table = {
		GPIO_LOOKUP(GPIO_CHIP, LED_HWNUM, "led", GPIO_ACTIVE_HIGH),
		{ }
	},
};

/* cat /dev/myled: return "0\n" or "1\n", then end-of-file. */
static ssize_t myled_read(struct file *file, char __user *buf,
			  size_t count, loff_t *ppos)
{
	char kbuf[4];
	int len;

	mutex_lock(&myled_lock);
	len = scnprintf(kbuf, sizeof(kbuf), "%d\n", led_state);
	mutex_unlock(&myled_lock);

	/* Copies to user space, advances *ppos, returns 0 (EOF) on the next read. */
	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

/* echo 1 > /dev/myled: only the first character counts ("1\n" is fine). */
static ssize_t myled_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	char c;

	if (count == 0)
		return 0;
	if (get_user(c, buf))		/* never dereference a user pointer directly */
		return -EFAULT;

	mutex_lock(&myled_lock);
	switch (c) {
	case '0':
		led_state = 0;
		break;
	case '1':
		led_state = 1;
		break;
	case 't':
		led_state = !led_state;
		break;
	default:
		mutex_unlock(&myled_lock);
		return -EINVAL;		/* "Invalid argument" in the shell */
	}
	gpiod_set_value_cansleep(led, led_state);
	mutex_unlock(&myled_lock);

	dev_info(myled_dev, "LED %s\n", led_state ? "on" : "off");
	return count;			/* tell user space we consumed everything */
}

static const struct file_operations myled_fops = {
	.owner = THIS_MODULE,
	.read  = myled_read,
	.write = myled_write,
};

static int __init myled_init(void)
{
	int ret;

	/* 1. Get a free major:minor number from the kernel. */
	ret = alloc_chrdev_region(&devno, 0, 1, DEV_NAME);
	if (ret)
		return ret;

	/* 2. Create /sys/class/myled. class_create() lost its first argument in 6.4. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	myled_class = class_create(DEV_NAME);
#else
	myled_class = class_create(THIS_MODULE, DEV_NAME);
#endif
	if (IS_ERR(myled_class)) {
		ret = PTR_ERR(myled_class);
		goto err_region;
	}

	/* 3. Create the device; devtmpfs makes /dev/myled appear. */
	myled_dev = device_create(myled_class, NULL, devno, NULL, DEV_NAME);
	if (IS_ERR(myled_dev)) {
		ret = PTR_ERR(myled_dev);
		goto err_class;
	}

	/* 4. Request the LED line through the GPIO subsystem (starts off). */
	gpiod_add_lookup_table(&myled_gpios);
	led = gpiod_get(myled_dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(led)) {
		ret = PTR_ERR(led);
		pr_err("cannot get LED GPIO (%d). Is leddaemon running?\n", ret);
		goto err_lookup;
	}

	/* 5. Last: connect our file_operations. From here on, open() works. */
	cdev_init(&myled_cdev, &myled_fops);
	myled_cdev.owner = THIS_MODULE;
	ret = cdev_add(&myled_cdev, devno, 1);
	if (ret)
		goto err_gpio;

	pr_info("ready: /dev/myled (major %d, minor %d)\n", MAJOR(devno), MINOR(devno));
	return 0;

	/* Undo in reverse order whatever succeeded before the failure. */
err_gpio:
	gpiod_put(led);
err_lookup:
	gpiod_remove_lookup_table(&myled_gpios);
	device_destroy(myled_class, devno);
err_class:
	class_destroy(myled_class);
err_region:
	unregister_chrdev_region(devno, 1);
	return ret;
}

static void __exit myled_exit(void)
{
	/* Exactly the reverse of init. */
	cdev_del(&myled_cdev);
	gpiod_set_value_cansleep(led, 0);	/* leave the LED off */
	gpiod_put(led);
	gpiod_remove_lookup_table(&myled_gpios);
	device_destroy(myled_class, devno);
	class_destroy(myled_class);
	unregister_chrdev_region(devno, 1);
	pr_info("removed\n");
}

module_init(myled_init);
module_exit(myled_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Character driver for LED D3 on the MicroZed");
