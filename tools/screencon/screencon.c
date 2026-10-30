/*
 * screencon: lines of text from stdin on the U30 Pro's screen (DRM, no fbcon in the GKI kernel), the newest at the
 * bottom. The first connected connector, its preferred mode, one dumb buffer; every new line redraws it and
 * DIRTYFB pushes it out (the panel is an SPI one, NV3002C 320x240). The backlight is switched on while it runs.
 *
 *   (dmesg -w & logread -f) | screencon [-b BRIGHTNESS] [-r 0|180]   (default 180: the panel is upside down)
 *
 * The font is the kernel's 6x10 (lib/fonts/font_6x10.c, put next to this file by build.sh).
 * SPDX-License-Identifier: GPL-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "font_6x10.h"

#define FW 6
#define FH 10
#define MAXROWS 64
#define MAXCOLS 128
#define BACKLIGHT_DIR "/sys/class/backlight/sprd_backlight"

static int fd;
static uint32_t *fb, fb_id, pitch, width, height;
static struct drm_mode_fb_dirty_cmd dirty;
static struct drm_mode_crtc crtc_set;
static int flush_way = -1;	/* how the panel takes a new frame: 0 DIRTYFB, 1 PAGE_FLIP, 2 SETCRTC again */
static char text[MAXROWS][MAXCOLS + 1];
static int rows, cols, head;	/* text[head] is the oldest line */

static void die(const char *what)
{
	fprintf(stderr, "screencon: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void *xcalloc(size_t n, size_t s)
{
	void *p = calloc(n ? n : 1, s);

	if (!p)
		die("calloc");
	return p;
}

static void sysfs(const char *path, const char *value)
{
	int b = open(path, O_WRONLY);

	if (b < 0)
		return;
	if (write(b, value, strlen(value)) < 0)
		fprintf(stderr, "screencon: %s: %s\n", path, strerror(errno));
	close(b);
}

/* on: bl_power 0 first - boot/init leaves 4 (FB_BLANK_POWERDOWN), and the driver ignores the level until it is 0 */
static void backlight(const char *value)
{
	if (strcmp(value, "0")) {
		sysfs(BACKLIGHT_DIR "/bl_power", "0");
		sysfs(BACKLIGHT_DIR "/brightness", value);
	} else {
		sysfs(BACKLIGHT_DIR "/brightness", "0");
		sysfs(BACKLIGHT_DIR "/bl_power", "4");
	}
}

static void setup(void)
{
	struct drm_mode_card_res res = {0};
	struct drm_mode_get_connector conn;
	struct drm_mode_get_encoder enc = {0};
	struct drm_mode_create_dumb cd = {0};
	struct drm_mode_map_dumb md = {0};
	struct drm_mode_fb_cmd fbc = {0};
	struct drm_mode_crtc crtc = {0};
	struct drm_mode_modeinfo *modes = NULL, mode;
	uint32_t *conns, *crtcs, *encs, i, c, conn_id = 0;

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("/dev/dri/card0");
	ioctl(fd, DRM_IOCTL_SET_MASTER, 0);

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		die("GETRESOURCES");
	conns = xcalloc(res.count_connectors, 4);
	crtcs = xcalloc(res.count_crtcs, 4);
	encs = xcalloc(res.count_encoders, 4);
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.count_fbs = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		die("GETRESOURCES");

	for (c = 0; c < res.count_connectors && !conn_id; c++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conns[c];
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn))
			continue;	/* the first call probes and counts */
		if (conn.connection != 1 || !conn.count_modes)
			continue;
		free(modes);
		modes = xcalloc(conn.count_modes, sizeof(*modes));
		conn.modes_ptr = (uintptr_t)modes;
		conn.count_props = 0;
		conn.count_encoders = 0;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn))
			die("GETCONNECTOR");
		conn_id = conns[c];
	}
	if (!conn_id) {
		errno = ENODEV;
		die("no connected connector with a mode");
	}
	mode = modes[0];
	for (i = 0; i < conn.count_modes; i++)
		if (modes[i].type & DRM_MODE_TYPE_PREFERRED)
			mode = modes[i];
	width = mode.hdisplay;
	height = mode.vdisplay;

	/* its encoder's CRTC, else the first one */
	enc.encoder_id = conn.encoder_id ? conn.encoder_id : encs[0];
	if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
		crtc.crtc_id = enc.crtc_id;
	else
		crtc.crtc_id = crtcs[0];

	cd.width = width;
	cd.height = height;
	cd.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
		die("CREATE_DUMB");
	pitch = cd.pitch;
	fbc.width = width;
	fbc.height = height;
	fbc.pitch = pitch;
	fbc.bpp = 32;
	fbc.depth = 24;
	fbc.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbc))
		die("ADDFB");
	fb_id = fbc.fb_id;
	md.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
		die("MAP_DUMB");
	fb = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (fb == MAP_FAILED)
		die("mmap");
	memset(fb, 0, cd.size);

	crtc.fb_id = fb_id;
	crtc.set_connectors_ptr = (uintptr_t)&conn_id;
	crtc.count_connectors = 1;
	crtc.mode = mode;
	crtc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc))
		die("SETCRTC");
	crtc_set = crtc;

	dirty.fb_id = fb_id;
	cols = width / FW;
	rows = height / FH;
	if (cols > MAXCOLS)
		cols = MAXCOLS;
	if (rows > MAXROWS)
		rows = MAXROWS;
	fprintf(stderr, "screencon: %ux%u, %d x %d characters\n", width, height, cols, rows);
}

static int rot180 = 1;	/* the U30 Pro's panel is mounted upside down (2026-10-30) */

static void glyph(int x, int y, unsigned char ch, uint32_t colour)
{
	const unsigned char *g = fontdata_6x10 + ch * FH;
	int r, b;

	for (r = 0; r < FH; r++) {
		for (b = 0; b < FW; b++) {
			uint32_t v = (g[r] & (0x80 >> b)) ? colour : 0;

			if (rot180)
				fb[(height - 1 - (y + r)) * (pitch / 4) + (width - 1 - (x + b))] = v;
			else
				fb[(y + r) * (pitch / 4) + x + b] = v;
		}
	}
}

/* the panel is an SPI one behind a software display controller: find out once which call makes it take the frame */
static int flush_with(int way)
{
	struct drm_mode_crtc_page_flip pf = {0};

	switch (way) {
	case 0:
		return ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
	case 1: {
		int tries, ret = -1;

		pf.crtc_id = crtc_set.crtc_id;
		pf.fb_id = fb_id;
		/* busy while the last frame is still on its way over SPI: wait for it, briefly */
		for (tries = 0; tries < 50; tries++) {
			ret = ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf);
			if (ret == 0 || errno != EBUSY)
				break;
			usleep(10000);
		}
		return ret;
	}
	default:
		return ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc_set);
	}
}

static void flush(void)
{
	int way;

	if (flush_way >= 0) {
		flush_with(flush_way);
		return;
	}
	for (way = 0; way < 3; way++) {
		if (flush_with(way) == 0) {
			fprintf(stderr, "screencon: frames by %s\n", way == 0 ? "DIRTYFB" : way == 1 ? "PAGE_FLIP" : "SETCRTC");
			if (way == 0 && getenv("SCREENCON_FLUSH"))
				continue;
			flush_way = getenv("SCREENCON_FLUSH") ? atoi(getenv("SCREENCON_FLUSH")) : way;
			return;
		}
		fprintf(stderr, "screencon: %s: %s\n", way == 0 ? "DIRTYFB" : way == 1 ? "PAGE_FLIP" : "SETCRTC", strerror(errno));
	}
	flush_way = 2;
}

static void redraw(void)
{
	int r, c;

	for (r = 0; r < rows; r++) {
		const char *s = text[(head + r) % rows];
		/* kernel lines (they start with "[") grey, the rest white */
		uint32_t colour = s[0] == '[' ? 0x00a0a0a0 : 0x00ffffff;

		for (c = 0; c < cols && s[c]; c++)
			glyph(c * FW, r * FH, (unsigned char)s[c], colour);
		for (; c < cols; c++)
			glyph(c * FW, r * FH, ' ', colour);
	}
	flush();
}

/* a line into the ring, wrapped at the width; tabs become a space, other control characters go */
static void add(const char *line)
{
	char buf[MAXCOLS + 1];
	int n = 0;

	for (;; line++) {
		if (!*line || n == cols) {
			buf[n] = 0;
			memcpy(text[head], buf, n + 1);
			head = (head + 1) % rows;
			n = 0;
			if (!*line)
				break;
		}
		if (*line == '\t')
			buf[n++] = ' ';
		else if ((unsigned char)*line >= ' ' && (unsigned char)*line < 0x7f)
			buf[n++] = *line;
	}
}

int main(int argc, char **argv)
{
	char line[1024];
	const char *bright = "2048";

	int i;

	for (i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-b"))
			bright = argv[i + 1];
		else if (!strcmp(argv[i], "-r"))
			rot180 = !strcmp(argv[i + 1], "180");
	}
	setup();
	backlight(bright);
	add("screencon: console");
	redraw();
	while (fgets(line, sizeof(line), stdin)) {
		line[strcspn(line, "\r\n")] = 0;
		add(line);
		redraw();
	}
	backlight("0");
	return 0;
}
