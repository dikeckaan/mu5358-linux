# Mainline kernel on the U30 Pro (UMS9632)

Following mu300-linux's `upstream/` port (UMS9620, F50/U30 Air), step by step. Everything boots from slot b as a
one-shot trial: boot_b holds the image under test, misc points at b once, LK falls back to Android when b fails.
The GKI image to put back afterwards is `work/unit2/linux-b/boot.img`.

## What differs from the UMS9620 (vendor DTB, 2026-10-29)

| | U30 Pro (UMS9632) | MU300 (UMS9620) |
|---|---|---|
| CPUs | 6x A55 + 2x A78 | 4x A55 + 4x A76 |
| PMIC | UMP9650 / 9651 / 9652 | UMP9620 / 9621 |
| ADI (PMIC bus) | `sprd,ums9632-adi` at 0x64600000 | 0x64400000 |
| eMMC | `sprd,sdhci-r11p3` | sdhci-r11 |
| USB | MUSB (`sprd,ums9632-musb`), no mainline glue | DWC3 |
| ramoops | 0xfff80000, 256 KiB | the same method |
| LK kernel placement | a different 64 KiB-aligned address every boot (0x80090000, 0x80610000, ...) | always 0x80080000 |

The placement means no `wrap-image.py`: a kernel since 5.8 runs from any such address, as GKI 6.6 does here.

UMP9650 reset registers (from `ums9632_data` in the vendor `spi-sprd-adi.ko`, same restart path as
`ums9620_data`): RST_STATUS 0x21d4, SWRST_CTRL0 0x2220, SOFT_RST 0x2024. LK reads the low byte of RST_STATUS as the
boot mode (`check_reboot_mode:get raw rst_mode is 5040`: 0x40 normal, 0x30 fastboot, 0x20 recovery).

## Steps

1. `stub/`: LK entry probe. If LK runs the image, it sets the boot mode to fastboot and resets through the PMIC:
   the board comes back in fastboot, and LK's next log shows `rst_mode is ..30`.
   `mkboot.py work/unit2/img/boot_a.img stub/pmic-reset.bin work/mainline/boot-probe.img`
   **Done 2026-10-29: LK runs our own image, and the UMP9650 reset works.** uboot_log after the trial:
   `Booting slot_b`, then the next boot `check_reboot_mode:get raw rst_mode is 5030` ... `reset_mode: (5)-(fastboot)`
   ... `shutdown device`: this ZTE LK powers the board off in fastboot mode instead of entering it, and the next
   start is a cold reset into slot a. So 0x30 is a marker for the log only; a probe that should come back by
   itself uses 0x40 (normal). The LK log is the `uboot_log` partition (16 MiB ring), readable from Android.
2. Mainline 6.18 with mu300's `upstream/` config and the stock vendor DTB; the log from ramoops, read in Android.
   **First run 2026-10-29** (`build.sh`, `mu5358-mainline.config`, patches 0001 from mu300 + 0002 UMS9632 restart;
   boot-ml1.img 91e6f60affa73bc7): LK booted slot b, and its next boot read `rst_mode is 5040`, the normal mode
   0002's restart writes (Android's reboot leaves 4F40): the kernel ran to its panic (no init it can unpack) and
   reset through the UMP9650. **pstore stayed empty**, and will on every failed trial: the boot after one b attempt
   is LK's rollback (`slot 1 booted fail, rolling back spl and reboot into normal`), and switching the SPL boot flag
   back is a power-off (`shutdown device`), so RAM is gone before Android. Ways out: the kernel log into LK's
   minidump (the structure at sysdumpinfo-mem@80001000 that the vendor sysdump.ko fills; LK saves it to sysdumpdb
   on an abnormal reset), or eMMC from mainline (a ums9632 clock driver, or a slot-b DTB with stand-in clocks).
   **The log channel, done 2026-10-29: patches/0003 (`drivers/soc/sprd/sprd-minidump.c`).** It publishes the printk
   ring and a kmsg_dump text copy in the minidump record at sysdumpinfo-mem (layout of Unisoc's sysdump.c; the
   vendor 6.6 sysdump.ko uses the same offsets and size 0x6fb0) and cleans them to DRAM (the ring every 2 s, the
   text at panic): without that, LK - reading with the caches off - got the log with holes. The kernel runs with
   panic=0; the PMIC watchdog resets it after ~5 min as "abnormal" (reset mode 0x1B, the only mode LK allows a
   minidump for; normal/panic/fastboot are refused), and LK saves `002_minidump_extend_printk_ring` and
   `003_minidump_extend_kmsg_text` to sysdumpdb before the rollback. Read in Android:
   `cat /dev/block/by-name/sysdumpdb > ...; tools/sysdumpdb-extract.py sysdumpdb.bin OUTDIR`.
   Third run (boot-ml3.img, log in work/mainline/ml3-kmsg.txt): 8 CPUs, GICv3, reserved memory, initramfs unpacked
   (vendor_boot + init_boot, LZ4), our boot/init ran: init-start, devfs-ready, misc-not-found (no eMMC),
   usb-no-udc (no USB), then exit -> "Attempted to kill init". hwspinlock: no clock (no ums9632 clock driver).
3. eMMC, then the MUSB glue (USB networking and ACM).
   **eMMC read, done 2026-10-29 (boot-ml4.img, log work/mainline/ml4-kmsg.txt):** patches/0004 (stand-in clocks:
   ap-clk 0x01 ap-axi 256 MHz, 0x15 emmc-2x from g1-pll 2 = rpll-390m, apahb-gate 3 emmc-eb; no register access)
   and 0005 (sdhci-sprd takes "sprd,sdhci-r11p3", high speed only, no CQE). The built-in initramfs
   (initramfs/, rdinit=/mlinit) only reads: high speed 52 MHz 8-bit, 2Y18CG 58.3 GiB, 83 partitions,
   vbmeta_a/dtbo_a/uboot_a/boot_a sha256 equal to the backup, boot_a 64 MiB in 1.48 s (~43 MB/s).
   Do not read clk_summary (or most clk_rate files) in Android's debugfs: the vendor kernel panics with an
   asynchronous SError (2026-10-29, twice).
   **HS200/HS400 and writes, done 2026-10-29:** patches/0007 ports the vendor DLL sweep tuning (mainline's
   standard SDHCI tuning silently fails on this IP: HS200 then hangs every read, ml5b) and the DLL phase 2.
   ml7 (hs200): window 24-59 of 60, step 42 - the vendor's own step -, checksums OK, boot_a 64 MiB 0.56 s.
   ml8 (hs400): HS400 at 200 MHz with the DT hs400 delays, checksums OK. ml9: 1 MiB written to the log area
   (sectors 122152960..) with fsync, read back O_DIRECT identical, the marker line read in Android.
   `sdhci_sprd.r11p3_mode=` (hs, hs200, hs400) still picks the mode; hs400 is the working one.
4. USB. patches/0006 (MUSB glue, PIO, peripheral) and 0004's real AON_APB gates/resets (aon-usb2-top-eb
   0x64900004 bit 8, otg-ref-eb 0x64900138 bit 12, resets 0x64900010 bits 8/9): without the gate the MUSB
   registers abort (ml6). ml8: musb-hdrc.0.auto bound, gadget bound, usb0 up, state "not attached" - no PHY.
   patches/0008: the UMS9632 PHY (vendor usb-phy-sprd-hs.ko phy_cfg_7 decoded: power AON 0x350/0x190, ANA 0x18;
   16-bit UTMI; device mode; VBUS valid forced; ANA = anlg_phy_g0l_regs syscon@64300000).
4. PMIC (reset, ADC, fuel gauge, efuse), thermal, cpufreq; then the modem and Wi-Fi, against mu300's parity list.
   ml12 (patches/0010, logs on both sides of the PHY hand-off; mlinit dumps devices_deferred): usb_add_phy_dev
   returned -EPROBE_DEFER on every probe. LK's dtbo gives hsphy `extcon` = the PMIC's Type-C block
   (pmic@1/typec@380, "sprd,ump9651-typec"), which has no mainline driver. patches/0011 drops the property (peripheral
   mode does not need it, the PHY forces VBUS valid). ml13: PHY registered, musb-hdrc.0.auto bound, gadget bound -
   but "not attached" for the whole run, no device on the Mac. The PHY's init dump still shows AON 0x190 and 0x350
   at 0: next, whether init's power writes land, and the musb session (DEVCTL).
   **ml14 (patches/0012, 2026-10-30):** the vendor kernel's PHY and controller, read with tools/regpeek (a module
   that reads only the addresses it is given; GKI has no /dev/mem): every PHY register as ml13's init dump except
   AON 0x350, 0x20 there. With bit 5 set (0012) and CONFIG_DEVMEM, mlinit's dump: DEVCTL 0x99 (session, VBUS
   valid, B-device: the vendor's value), POWER 0xE0 (SOFTCONN), but FADDR 0: no host saw the device.
   **ml15: the host enumerates the board.** The PHY's vdd-supply, LDO_VDDUSB33 of the UMP9650, was off under mainline
   (power-down bit 0 of PMIC 0x2108 set; on at 3.3 V under the vendor kernel). Its registers come from the vendor
   ump9650-regulator.ko's regulator_desc (enable 0x2108 bit 0 inverted, vsel 0x210c, 1.2 V + 10 mV steps); mlinit
   switches it on through the ADI (read: RD_CMD 0x28 / RD_DATA 0x2c of 0x64600000; write: 0x64620000 + register).
   Then: state configured, FADDR 0x03, POWER 0xF0 (high speed) - the vendor's values. Bulk OUT data read as zeros
   ("Wrong NTH SIGN", an NCM header of zeros) while ep0 works: ml16 pauses the controller's DMA engine
   (MUSB_DMA_PAUSE 0x1000, which the vendor core sets around every CPU access of a FIFO).
   **ml16:** DMA paused (MUSB_DMA_PAUSE read back 0x10001, acknowledged): bulk OUT still zeros. The vendor gadget
   never runs PIO on an endpoint other than ep0 (musb_ep_restart: every request goes to the Sprd linked-list DMA,
   drivers/usb/musb/sprd_musbhsdma.c of the vendor tree, GPL-2.0, ~1100 lines). Next: port that DMA controller to
   mainline's musb dma_controller API (MUSB_DMA_SPRD), and the LDO_VDDUSB33 switch-on from mlinit into the PHY
   driver (through the ADI regmap).
