// SPDX-License-Identifier: GPL-2.0
/*
 * panelcmd: one command to the SPI panel (spi3.0, NV3002C), the way its driver sends them: the cd GPIO low for the
 * command byte, high for the data bytes, the bus locked meanwhile. Only while nothing draws (the panel driver
 * sends nothing without a client: u30ui sends it before it draws). Stays loaded only when sent, so
 *   insmod panelcmd.ko cmd=0x36 data=0xc0 && rmmod panelcmd     MADCTL: the panel turns the picture by 180 degrees
 * says by its status whether the panel has it.
 */
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/spi/spi.h>

static char *dev = "spi3.0";
module_param(dev, charp, 0);
static int cmd = -1;
module_param(cmd, int, 0);
static int data[16];
static int ndata;
module_param_array(data, int, &ndata, 0);

static int send(struct spi_device *spi, u8 *buf, int len)
{
	struct spi_transfer t = { .tx_buf = buf, .len = len, .bits_per_word = 8 };
	struct spi_message m;

	spi_message_init_with_transfers(&m, &t, 1);
	return spi_sync_locked(spi, &m);
}

static int __init panelcmd_init(void)
{
	struct device *d = bus_find_device_by_name(&spi_bus_type, NULL, dev);
	struct spi_device *spi;
	struct gpio_desc *cd;
	u8 c, buf[16];
	int gpio, old, i, r;

	if (!d)
		return -ENODEV;
	spi = to_spi_device(d);
	gpio = of_get_named_gpio(spi->dev.of_node, "cd-gpio", 0);
	cd = gpio_is_valid(gpio) ? gpio_to_desc(gpio) : NULL;
	if (!cd || cmd < 0 || cmd > 0xff) {
		pr_info("panelcmd: cd-gpio %d, cmd %d: nothing sent\n", gpio, cmd);
		put_device(d);
		return -EINVAL;
	}
	for (i = 0; i < ndata; i++)
		buf[i] = data[i];
	c = cmd;
	spi_bus_lock(spi->controller);
	old = gpiod_get_raw_value(cd);
	gpiod_set_raw_value(cd, 0);
	r = send(spi, &c, 1);
	gpiod_set_raw_value(cd, 1);
	if (!r && ndata)
		r = send(spi, buf, ndata);
	gpiod_set_raw_value(cd, old);
	spi_bus_unlock(spi->controller);
	pr_info("panelcmd: cd-gpio %d (was %d), cmd 0x%02x + %d bytes: %d\n", gpio, old, cmd, ndata, r);
	put_device(d);
	return r ? -EIO : 0;
}

static void __exit panelcmd_exit(void)
{
}
module_init(panelcmd_init);
module_exit(panelcmd_exit);
MODULE_DESCRIPTION("send one command to the SPI panel");
MODULE_LICENSE("GPL");
