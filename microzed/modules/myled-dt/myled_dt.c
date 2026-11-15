// SPDX-License-Identifier: GPL-2.0
/*
 * myled_dt - platform driver for LED D3, configured from the device tree.
 *
 * Same /dev/myled interface as the myled module, but the hardware comes
 * from this device tree node instead of being hard-coded:
 *
 *   myled {
 *       compatible = "microzed,myled";   <- the kernel matches this to our driver
 *       label = "d3";                    <- optional, read with device_property_*
 *       led-gpios = <&gpio0 47 0>;       <- devm_gpiod_get(dev, "led", ...)
 *   };
 *
 *   echo 1 > /dev/myled     on        echo 0 > /dev/myled     off
 *   echo t > /dev/myled     toggle    cat /dev/myled          0 or 1
 *
 * Stop leddaemon first and rmmod the old myled module (same GPIO, same /dev name).
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define DEV_NAME "myled"

/* Everything one LED needs. probe() allocates one per matching DT node. */
struct myled {
	struct gpio_desc *gpio;
	struct mutex lock;
	int state;
	dev_t devno;
	struct cdev cdev;
	struct device *chardev;
	const char *label;
};

static struct class *myled_class;

/* open(): find our struct from the cdev, and keep it for read/write. */
static int myled_open(struct inode *inode, struct file *file)
{
	file->private_data = container_of(inode->i_cdev, struct myled, cdev);
	return 0;
}

static ssize_t myled_read(struct file *file, char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct myled *ml = file->private_data;
	char kbuf[4];
	int len;

	mutex_lock(&ml->lock);
	len = scnprintf(kbuf, sizeof(kbuf), "%d\n", ml->state);
	mutex_unlock(&ml->lock);
	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t myled_write(struct file *file, const char __user *buf,
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
		ml->state = 0;
		break;
	case '1':
		ml->state = 1;
		break;
	case 't':
		ml->state = !ml->state;
		break;
	default:
		mutex_unlock(&ml->lock);
		return -EINVAL;
	}
	gpiod_set_value_cansleep(ml->gpio, ml->state);
	mutex_unlock(&ml->lock);

	dev_info(ml->chardev, "LED %s %s\n", ml->label, ml->state ? "on" : "off");
	return count;
}

static const struct file_operations myled_fops = {
	.owner = THIS_MODULE,
	.open  = myled_open,
	.read  = myled_read,
	.write = myled_write,
};

/*
 * probe(): called by the kernel when a DT node's "compatible" matches our
 * of_match_table. pdev->dev is that node's device. devm_* resources are
 * released automatically when the device goes away (no manual cleanup).
 */
static int myled_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct myled *ml;
	int ret;

	ml = devm_kzalloc(dev, sizeof(*ml), GFP_KERNEL);
	if (!ml)
		return -ENOMEM;
	mutex_init(&ml->lock);

	/* Optional property: label = "d3"; */
	if (device_property_read_string(dev, "label", &ml->label))
		ml->label = "led";

	/* Required property: led-gpios = <&gpio0 47 0>; */
	ml->gpio = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(ml->gpio))
		return dev_err_probe(dev, PTR_ERR(ml->gpio),
				     "cannot get led-gpios (is leddaemon or myled running?)\n");

	ret = alloc_chrdev_region(&ml->devno, 0, 1, DEV_NAME);
	if (ret)
		return ret;

	ml->chardev = device_create(myled_class, dev, ml->devno, ml, DEV_NAME);
	if (IS_ERR(ml->chardev)) {
		ret = PTR_ERR(ml->chardev);
		goto err_region;
	}

	cdev_init(&ml->cdev, &myled_fops);
	ml->cdev.owner = THIS_MODULE;
	ret = cdev_add(&ml->cdev, ml->devno, 1);
	if (ret)
		goto err_device;

	platform_set_drvdata(pdev, ml);	/* so remove() can find it */
	dev_info(dev, "bound to DT node %pOF, LED '%s', /dev/%s (major %d)\n",
		 dev->of_node, ml->label, DEV_NAME, MAJOR(ml->devno));
	return 0;

err_device:
	device_destroy(myled_class, ml->devno);
err_region:
	unregister_chrdev_region(ml->devno, 1);
	return ret;
}

/* remove(): undo probe (the devm_* parts are freed automatically afterwards). */
static void myled_remove_common(struct platform_device *pdev)
{
	struct myled *ml = platform_get_drvdata(pdev);

	cdev_del(&ml->cdev);
	device_destroy(myled_class, ml->devno);
	unregister_chrdev_region(ml->devno, 1);
	gpiod_set_value_cansleep(ml->gpio, 0);
	dev_info(&pdev->dev, "unbound\n");
}

/* platform_driver.remove changed from returning int to void in kernel 6.11. */
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

/* The compatible strings this driver handles. */
static const struct of_device_id myled_of_match[] = {
	{ .compatible = "microzed,myled" },
	{ }
};
/* Exports the table so tools (modinfo, udev/mdev) can load the module automatically. */
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

	/* Registering the driver makes the kernel call probe() for each matching node. */
	ret = platform_driver_register(&myled_driver);
	if (ret)
		class_destroy(myled_class);
	return ret;
}

static void __exit myled_exit(void)
{
	platform_driver_unregister(&myled_driver);	/* calls remove() */
	class_destroy(myled_class);
}

module_init(myled_init);
module_exit(myled_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Device tree platform driver for LED D3 on the MicroZed");
