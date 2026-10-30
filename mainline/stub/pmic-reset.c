/*
 * Bare-metal probe for the U30 Pro's LK entry (UMS9632, UMP9650 PMIC on the ADI bus, MMU off): if LK runs this
 * image, it sets the PMIC reset mode to fastboot (0x30) and resets the board through the PMIC, so the board comes
 * back in fastboot mode - and LK's next log says "rst_mode is ..30" either way. Adapted from mu300-linux's
 * upstream/stub/pmic-reset.c (UMP9620); the UMP9650 registers are those of ums9632_data in the vendor
 * spi-sprd-adi.ko, which takes the same restart path as its ums9620_data:
 *
 *            rst_sts  swrst_ctrl0  soft_rst  ADI base
 *   UMP9620  0x23ac   0x23f8       0x2024    0x64400000
 *   UMP9650  0x21d4   0x2220       0x2024    0x64600000  (aon ranges 0x64000000 + spi@600000)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
typedef unsigned int u32;
typedef unsigned long u64;

#define ADI_BASE	0x64600000UL
#define ADI_SLAVE	(ADI_BASE + 0x20000)	/* 15-bit slave address space */
#define RD_CMD		0x28
#define RD_DATA		0x2c
#define FIFO_STS	0x30
#define FIFO_EMPTY	(1u << 10)
#define RD_BUSY		(1u << 31)

#define RST_STATUS	0x21d4
#define SWRST_CTRL0	0x2220
#define SOFT_RST_HW	0x2024
#define MODE_FASTBOOT	0x30

static inline u32 rl(u64 a) { return *(volatile u32 *)a; }
static inline void wl(u64 a, u32 v) { *(volatile u32 *)a = v; }

static u32 adi_read(u32 reg)
{
	wl(ADI_BASE + RD_CMD, reg);
	for (int i = 0; i < 1000000; i++) {
		u32 v = rl(ADI_BASE + RD_DATA);
		if (!(v & RD_BUSY))
			return v & 0xffff;
	}
	return 0;
}

static void adi_write(u32 reg, u32 val)
{
	for (int i = 0; i < 1000000; i++)
		if (rl(ADI_BASE + FIFO_STS) & FIFO_EMPTY)
			break;
	wl(ADI_SLAVE + reg, val);
	for (int i = 0; i < 1000000; i++)
		if (rl(ADI_BASE + FIFO_STS) & FIFO_EMPTY)
			break;
}

void pmic_reset(void)
{
	u32 v;

	v = adi_read(RST_STATUS);		/* reboot mode: fastboot */
	adi_write(RST_STATUS, (v & ~0xffu) | MODE_FASTBOOT);
	v = adi_read(SWRST_CTRL0);		/* enable register reset */
	adi_write(SWRST_CTRL0, v | (1u << 4));
	v = adi_read(SOFT_RST_HW);		/* reset */
	adi_write(SOFT_RST_HW, v | 1u);
	for (;;)
		;
}
