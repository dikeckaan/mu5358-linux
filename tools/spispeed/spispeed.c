// SPDX-License-Identifier: GPL-2.0
/*
 * spispeed: the speed of one SPI device (the panel, spi3.0) and its controller's clocks, and optionally a new
 * maximum for the device. Loads, reports, sets and fails on purpose: nothing stays in the kernel.
 *   insmod spispeed.ko               report only
 *   insmod spispeed.ko hz=48000000   the device's max_speed_hz (the panel driver's transfers follow it)
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/spi/spi.h>

static char *dev = "spi3.0";
module_param(dev, charp, 0);
static uint hz;
module_param(hz, uint, 0);

static void clk_report(struct device *cd, const char *id)
{
	struct clk *c = clk_get(cd, id), *p;

	if (IS_ERR(c)) {
		pr_info("spispeed: clock %s: %ld\n", id, PTR_ERR(c));
		return;
	}
	p = clk_get_parent(c);
	pr_info("spispeed: clock %s = %s %lu Hz, parent %s\n", id, __clk_get_name(c), clk_get_rate(c),
		p ? __clk_get_name(p) : "-");
	clk_put(c);
}

static int __init spispeed_init(void)
{
	struct device *d = bus_find_device_by_name(&spi_bus_type, NULL, dev);
	struct spi_device *spi;
	struct device *cd;

	if (!d)
		return -ENODEV;
	spi = to_spi_device(d);
	cd = spi->controller->dev.parent;
	pr_info("spispeed: %s max_speed_hz %u mode 0x%x bpw %u; controller %s max_speed_hz %u\n", dev,
		spi->max_speed_hz, spi->mode, spi->bits_per_word, dev_name(cd), spi->controller->max_speed_hz);
	clk_report(cd, "spi");
	clk_report(cd, "source");
	if (hz) {
		spi->max_speed_hz = hz;
		pr_info("spispeed: %s max_speed_hz now %u\n", dev, spi->max_speed_hz);
	}
	put_device(d);
	return -EAGAIN;
}
module_init(spispeed_init);
MODULE_DESCRIPTION("report and set an SPI device's speed");
MODULE_LICENSE("GPL");
