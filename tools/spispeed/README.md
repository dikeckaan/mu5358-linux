# spispeed, panelcmd - the panel's SPI from a module

The panel (NV3002C on spi3.0, sprd sw-dispc) tore on every moving frame. Measured on 2026-10-30 (spi trace
events, tools: `/sys/kernel/tracing/events/spi`):

- a frame is one 115200-byte transfer (RGB565, after 2a/2b/2c), 27.4 ms (~34 Mbit/s; spi3.0 runs at its 48 MHz
  maximum, spi3 = tgpll-192m);
- the driver starts it on the panel's TE (54 Hz), so a frame every 2 TE (37 ms);
- the panel's scan (18.5 ms) overtakes such a write once - harmless when both go the same way, a tear where they
  meet when they go opposite ways. With the picture turned by the CPU (the panel is mounted upside down) they did.

The fix: the panel turns the picture itself (MADCTL `0x36 = 0xc0`, MY|MX), u30ui copies its frame straight, the
rows go out in the order the panel scans them. Confirmed by eye on the board: no tear.

| | |
|---|---|
| panelcmd.ko | one DCS command to the panel: the cd GPIO low for the command byte, high for the data, the SPI bus locked meanwhile. Only while no frame is on the way (u30ui sends it before it draws, and again when the driver's ESD recovery initialised the panel: `/sys/lcd_sys/esd_num`). Stays loaded only when it was sent: `insmod panelcmd.ko cmd=0x36 data=0xc0 && rmmod panelcmd` |
| spispeed.ko | report spi3.0's speed and its controller's clocks (and set a device maximum with `hz=`); always fails to load, nothing stays |
| build.sh | `docker run --rm -v /Volumes/mu5358kb:/src -v "$PWD/tools/spispeed":/m mu5358-kbuild bash /m/build.sh` (kernel/Dockerfile's image, kernel/build.sh's tree; signed with its key) |

Do not read `/sys/kernel/debug/clk/clk_summary` on the board: on 2026-10-30 it hung the kernel.
