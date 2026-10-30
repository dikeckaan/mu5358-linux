/*
 * display: the panel through DRM (the GKI kernel has no fbcon), as tools/screencon learned it on 2026-10-30: the
 * first connected connector and its mode (240x240), two dumb buffers, PAGE_FLIP to push a frame (DIRTYFB is not
 * implemented by sprd's swdispc), the backlight through bl_power (4 off: the driver ignores the level then) and the
 * panel mounted upside down (turned by the panel's MADCTL, else by the copy).
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include "display.h"
#include "gfx.h"

#define BL "/sys/class/backlight/sprd_backlight"

static int fd = -1;
/* two buffers: one on the panel (the SPI transfer reads it), the other drawn into - one buffer tore (2026-10-30) */
static uint32_t *fbs[2], fb_ids[2], pitch, crtc_id, fbw, fbh;
static int back, flip_pending;
/* The panel turns the picture itself (MADCTL 0x36 = 0xc0 through tools/spispeed/panelcmd): the copy is straight
 * and the SPI writes the rows in the direction the panel scans them. Turned by the CPU instead, the rows went in
 * the other direction and every frame tore where the two met (2026-10-30). U30UI_PANEL_ROT=0, or no panelcmd: the
 * CPU turns it. The driver's ESD recovery initialises the panel again (MADCTL 0): esd_num says when. */
#define PANELCMD "/opt/mu5358/bin/busybox insmod /lib/mu5358/modules/panelcmd.ko cmd=0x36 data=0xc0 2>/dev/null && " \
	"/opt/mu5358/bin/busybox rmmod panelcmd"
#define ESD "/sys/lcd_sys/esd_num"
static int panel_rot, esd_num;
static long esd_checked;

static long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* "esd num:0x3" */
static int esd_read(void)
{
	char b[64] = "", *c;
	int f = open(ESD, O_RDONLY), n;

	if (f < 0)
		return -1;
	n = read(f, b, sizeof(b) - 1);
	close(f);
	c = n > 0 ? strchr(b, ':') : NULL;
	return c ? (int)strtol(c + 1, NULL, 0) : -1;
}

/* only while no frame is on the way: the command shares the bus and the cd line with the frames */
static void panel_turn(void)
{
	panel_rot = system(PANELCMD) == 0;
	fprintf(stderr, "u30ui: the picture turned by the %s\n", panel_rot ? "panel" : "CPU");
}

static int sysfs_write(const char *path, const char *v)
{
	int f = open(path, O_WRONLY), r;

	if (f < 0)
		return -1;
	r = write(f, v, strlen(v)) < 0 ? -1 : 0;
	close(f);
	return r;
}

static int sysfs_int(const char *path)
{
	char b[32] = "";
	int f = open(path, O_RDONLY), n;

	if (f < 0)
		return -1;
	n = read(f, b, sizeof(b) - 1);
	close(f);
	return n > 0 ? atoi(b) : -1;
}

int display_init(void)
{
	struct drm_mode_card_res res = {0};
	struct drm_mode_get_connector conn;
	struct drm_mode_get_encoder enc = {0};
	struct drm_mode_create_dumb cd = {0};
	struct drm_mode_map_dumb md = {0};
	struct drm_mode_fb_cmd fbc = {0};
	struct drm_mode_crtc crtc = {0};
	struct drm_mode_modeinfo *modes = NULL, mode;
	uint32_t conns[8], crtcs[8], encs[8], c, i, conn_id = 0;

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || res.count_connectors > 8 || res.count_crtcs > 8 ||
	    res.count_encoders > 8)
		return -1;
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.count_fbs = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -1;
	for (c = 0; c < res.count_connectors && !conn_id; c++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conns[c];
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || conn.connection != 1 || !conn.count_modes)
			continue;
		free(modes);
		modes = calloc(conn.count_modes, sizeof(*modes));
		if (!modes)
			return -1;
		conn.modes_ptr = (uintptr_t)modes;
		conn.count_props = 0;
		conn.count_encoders = 0;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn))
			return -1;
		conn_id = conns[c];
	}
	if (!conn_id)
		return -1;
	mode = modes[0];
	for (i = 0; i < conn.count_modes; i++)
		if (modes[i].type & DRM_MODE_TYPE_PREFERRED)
			mode = modes[i];
	fbw = mode.hdisplay;
	fbh = mode.vdisplay;
	enc.encoder_id = conn.encoder_id ? conn.encoder_id : encs[0];
	crtc_id = ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id ? enc.crtc_id : crtcs[0];

	for (i = 0; i < 2; i++) {
		memset(&cd, 0, sizeof(cd));
		memset(&fbc, 0, sizeof(fbc));
		memset(&md, 0, sizeof(md));
		cd.width = fbw;
		cd.height = fbh;
		cd.bpp = 32;
		if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
			return -1;
		pitch = cd.pitch;
		fbc.width = fbw;
		fbc.height = fbh;
		fbc.pitch = pitch;
		fbc.bpp = 32;
		fbc.depth = 24;
		fbc.handle = cd.handle;
		if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbc))
			return -1;
		fb_ids[i] = fbc.fb_id;
		md.handle = cd.handle;
		if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
			return -1;
		fbs[i] = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
		if (fbs[i] == MAP_FAILED)
			return -1;
		memset(fbs[i], 0, cd.size);
	}
	back = 1;
	crtc.crtc_id = crtc_id;
	crtc.fb_id = fb_ids[0];
	crtc.set_connectors_ptr = (uintptr_t)&conn_id;
	crtc.count_connectors = 1;
	crtc.mode = mode;
	crtc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc))
		return -1;
	fprintf(stderr, "u30ui: panel %ux%u\n", fbw, fbh);
	esd_num = esd_read();
	if (!getenv("U30UI_PANEL_ROT") || atoi(getenv("U30UI_PANEL_ROT")))
		panel_turn();
	return 0;
}

/* the flip's completion: the panel shows the new buffer, the old one may be drawn into again */
static void wait_flip(int ms)
{
	struct pollfd p = {fd, POLLIN, 0};
	char buf[256];

	while (flip_pending && poll(&p, 1, ms) > 0) {
		int n = read(fd, buf, sizeof(buf)), off = 0;

		while (n > 0 && off + (int)sizeof(struct drm_event) <= n) {
			struct drm_event *e = (struct drm_event *)(buf + off);

			if (e->type == DRM_EVENT_FLIP_COMPLETE)
				flip_pending = 0;
			off += e->length;
		}
	}
	if (flip_pending) {
		static int said;

		if (!said++)
			fprintf(stderr, "u30ui: no flip event within %d ms\n", ms);
	}
	flip_pending = 0;	/* no event within ms: go on rather than hang */
}

void display_present(void)
{
	struct drm_mode_crtc_page_flip pf = {0};
	uint32_t x, y, w = fbw < W ? fbw : W, h = fbh < H ? fbh : H, *fb;
	int tries;

	wait_flip(100);
	if (panel_rot && mono_ms() - esd_checked >= 2000) {
		int n = esd_read();

		esd_checked = mono_ms();
		if (n != esd_num) {
			fprintf(stderr, "u30ui: panel initialised again (esd_num %d -> %d)\n", esd_num, n);
			esd_num = n;
			panel_turn();
		}
	}
	fb = fbs[back];
	if (panel_rot)
		for (y = 0; y < h; y++)
			memcpy(fb + y * (pitch / 4), gfx_buf + y * W, w * 4);
	else	/* turned by 180 degrees: the panel is upside down */
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				fb[(fbh - 1 - y) * (pitch / 4) + (fbw - 1 - x)] = gfx_buf[y * W + x];
	pf.crtc_id = crtc_id;
	pf.fb_id = fb_ids[back];
	pf.flags = DRM_MODE_PAGE_FLIP_EVENT;
	for (tries = 0; tries < 50; tries++) {
		if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) {
			flip_pending = 1;
			back = !back;
			break;
		}
		if (errno != EBUSY)
			break;
		usleep(5000);
	}
}

void display_backlight(int on)
{
	if (on) {
		sysfs_write(BL "/bl_power", "0");
		sysfs_write(BL "/brightness", "2048");
	} else {
		sysfs_write(BL "/brightness", "0");
		sysfs_write(BL "/bl_power", "4");
	}
}

int display_is_on(void)
{
	return sysfs_int(BL "/bl_power") == 0;
}
