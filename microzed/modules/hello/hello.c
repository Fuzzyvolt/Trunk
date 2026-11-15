// SPDX-License-Identifier: GPL-2.0
/*
 * hello - the smallest useful Linux kernel module.
 *
 * Load:    insmod hello.ko               (or: insmod hello.ko who=world)
 * Check:   dmesg | tail -3
 *          lsmod
 *          cat /sys/module/hello/parameters/who
 * Remove:  rmmod hello
 */

/* Prefix every pr_info() message with the module name ("hello: ...").
 * Must be defined before the #includes. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

/* A module parameter: set at load time, readable in /sys/module/hello/parameters/ */
static char *who = "MicroZed";
module_param(who, charp, 0444);
MODULE_PARM_DESC(who, "Name to greet");

static unsigned long loaded_at;	/* jiffies when the module was loaded */

/* Runs once, when the module is loaded (insmod). Returning non-zero aborts the load. */
static int __init hello_init(void)
{
	loaded_at = jiffies;
	pr_info("Hello, %s! Kernel tick rate HZ=%d\n", who, HZ);
	return 0;
}

/* Runs once, when the module is removed (rmmod). */
static void __exit hello_exit(void)
{
	pr_info("Goodbye, %s. I was loaded for %u ms\n",
		who, jiffies_to_msecs(jiffies - loaded_at));
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hello world kernel module for the MicroZed");
