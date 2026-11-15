// SPDX-License-Identifier: GPL-2.0
/*
 * axiregs - platform driver for the project 02 AXI register block in the PL.
 *
 * Device tree node (added by board/post-image-dt.sh):
 *
 *   axiregs@43c00000 {
 *       compatible = "microzed,axiregs";
 *       reg = <0x43c00000 0x1000>;   <- 4 KB of registers, ioremap()ed in probe
 *       clocks = <&clkc 15>;         <- FCLK_CLK0, the PL clock
 *       fpga-mgr = <&devcfg>;        <- to check that the PL is programmed
 *   };
 *
 * sysfs files in /sys/bus/platform/devices/43c00000.axiregs/:
 *   reg0, reg1   read/write (decimal or 0x hex)
 *   sum          read: reg0 + reg1, computed by the PL
 *   count        read: free-running 100 MHz counter
 *   id           read: 0x4d5a0201
 *
 * Load the bitstream BEFORE this driver (S40axiregs does both). Reading a PL
 * address with no logic behind it hangs the CPU, so probe() first asks the
 * FPGA manager whether the PL is programmed, and then checks the ID register.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* Register offsets, same as axiregs.v */
#define AXIREGS_REG0	0x00
#define AXIREGS_REG1	0x04
#define AXIREGS_SUM	0x08
#define AXIREGS_COUNT	0x0c
#define AXIREGS_ID	0x10

#define AXIREGS_ID_VALUE 0x4d5a0201

struct axiregs {
	void __iomem *base;	/* virtual address of the 4 KB register window */
};

/*
 * sysfs: one small show()/store() pair per file. dev_get_drvdata() gives
 * back the struct that probe() stored with platform_set_drvdata().
 */
static ssize_t show_reg(struct device *dev, char *buf, unsigned int off)
{
	struct axiregs *ar = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", readl(ar->base + off));
}

static ssize_t store_reg(struct device *dev, const char *buf, size_t count,
			 unsigned int off)
{
	struct axiregs *ar = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 0, &val);	/* base 0: "42", "0x2a" both work */
	if (ret)
		return ret;
	writel(val, ar->base + off);
	return count;
}

static ssize_t reg0_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_reg(dev, buf, AXIREGS_REG0);
}

static ssize_t reg0_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	return store_reg(dev, buf, count, AXIREGS_REG0);
}

static ssize_t reg1_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_reg(dev, buf, AXIREGS_REG1);
}

static ssize_t reg1_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	return store_reg(dev, buf, count, AXIREGS_REG1);
}

static ssize_t sum_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_reg(dev, buf, AXIREGS_SUM);
}

static ssize_t count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return show_reg(dev, buf, AXIREGS_COUNT);
}

static ssize_t id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct axiregs *ar = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", readl(ar->base + AXIREGS_ID));
}

/* DEVICE_ATTR_RW(reg0) -> dev_attr_reg0, mode 0644, uses reg0_show/reg0_store */
static DEVICE_ATTR_RW(reg0);
static DEVICE_ATTR_RW(reg1);
static DEVICE_ATTR_RO(sum);
static DEVICE_ATTR_RO(count);
static DEVICE_ATTR_RO(id);

static struct attribute *axiregs_attrs[] = {
	&dev_attr_reg0.attr,
	&dev_attr_reg1.attr,
	&dev_attr_sum.attr,
	&dev_attr_count.attr,
	&dev_attr_id.attr,
	NULL
};
ATTRIBUTE_GROUPS(axiregs);	/* -> axiregs_groups, used in the driver struct */

/*
 * Ask the FPGA manager named in "fpga-mgr" whether the PL is programmed.
 * Returns 0 if it is (or if the node has no fpga-mgr property).
 */
static int axiregs_check_pl(struct device *dev)
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

static int axiregs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct axiregs *ar;
	struct resource *res;
	struct clk *clk;
	u32 id;
	int ret;

	ret = axiregs_check_pl(dev);
	if (ret)
		return ret;

	ar = devm_kzalloc(dev, sizeof(*ar), GFP_KERNEL);
	if (!ar)
		return -ENOMEM;

	/*
	 * The PL logic needs FCLK_CLK0. The kernel turns off clocks nobody
	 * has claimed, so claim it: get + prepare + enable, undone on unbind.
	 */
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot get/enable the PL clock\n");

	/* Map the "reg" range from the node into kernel virtual memory. */
	ar->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(ar->base))
		return PTR_ERR(ar->base);

	id = readl(ar->base + AXIREGS_ID);
	if (id != AXIREGS_ID_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "wrong ID 0x%08x (expected 0x%08x): wrong bitstream?\n",
				     id, AXIREGS_ID_VALUE);

	platform_set_drvdata(pdev, ar);
	dev_info(dev, "axiregs at %pR, ID 0x%08x, PL clock %lu Hz\n",
		 res, id, clk_get_rate(clk));
	return 0;
}

static const struct of_device_id axiregs_of_match[] = {
	{ .compatible = "microzed,axiregs" },
	{ }
};
MODULE_DEVICE_TABLE(of, axiregs_of_match);

static struct platform_driver axiregs_driver = {
	.probe = axiregs_probe,
	.driver = {
		.name = "axiregs",
		.of_match_table = axiregs_of_match,
		.dev_groups = axiregs_groups,	/* sysfs files created at bind */
	},
};
module_platform_driver(axiregs_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MicroZed project 02: AXI register block in the PL");
