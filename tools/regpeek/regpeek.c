// SPDX-License-Identifier: GPL-2.0
/*
 * regpeek: read a few MMIO registers of the running board and print them, for comparing the vendor kernel's
 * hardware state with the mainline port's (mainline/). The GKI kernel has no /dev/mem, and regmap's debugfs reads
 * every register of a block, unclocked ones too (an SError, as clk_summary did). Only the addresses given are read:
 *
 *   insmod regpeek.ko addr=0x64900190,0x64900204 [width=32|8]
 *
 * Load fails with -EAGAIN on purpose, so nothing stays loaded.
 */
#include <linux/io.h>
#include <linux/module.h>

static unsigned long addr[32];
static int naddr;
static int width = 32;
module_param_array(addr, ulong, &naddr, 0);
module_param(width, int, 0);

static int __init regpeek_init(void)
{
	int i;

	for (i = 0; i < naddr; i++) {
		void __iomem *p = ioremap(addr[i] & PAGE_MASK, PAGE_SIZE);

		if (!p) {
			pr_info("regpeek: %#lx: ioremap failed\n", addr[i]);
			continue;
		}
		if (width == 8)
			pr_info("regpeek: %#lx = %#04x\n", addr[i], readb(p + (addr[i] & ~PAGE_MASK)));
		else
			pr_info("regpeek: %#lx = %#010x\n", addr[i], readl(p + ((addr[i] & ~PAGE_MASK) & ~3UL)));
		iounmap(p);
	}
	return -EAGAIN;
}
module_init(regpeek_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("read given MMIO registers once");
