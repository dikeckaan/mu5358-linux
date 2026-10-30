/*
 * input: the touch panel (tlsc6x_touch, evdev) as gestures. Its coordinates are scaled from the kernel's range to
 * 240x240 and mirrored to match the picture (see flipx/flipy).
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "input.h"

#define LONG_MS 600	/* the launcher's long press (UnlockPage) */
#define SLOP 12		/* movement that still counts as a tap */
#define SWIPE 40	/* horizontal travel that is a page swipe */

/* the touch panel against the picture: Y mirrored, X not (the user, 2026-10-30: with both turned left and right
 * were the wrong way round); U30UI_TOUCH_FLIPX / U30UI_TOUCH_FLIPY=0|1 change it */
static int fd = -1, flipx, flipy = 1;
static int xmin, xmax = 239, ymin, ymax = 239;
static int down, x, y, x0, y0, long_sent, moved;
static long t0;

long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

int input_open(void)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *e;
	char path[300], name[64];
	struct input_absinfo ai;

	if (getenv("U30UI_TOUCH_FLIPX"))
		flipx = atoi(getenv("U30UI_TOUCH_FLIPX"));
	if (getenv("U30UI_TOUCH_FLIPY"))
		flipy = atoi(getenv("U30UI_TOUCH_FLIPY"));
	while (d && (e = readdir(d))) {
		FILE *f;

		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		name[0] = 0;
		if (!fgets(name, sizeof(name), f))
			name[0] = 0;
		fclose(f);
		if (!strstr(name, "touch"))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		break;
	}
	if (d)
		closedir(d);
	if (fd < 0)
		return -1;
	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0 && ai.maximum > ai.minimum) {
		xmin = ai.minimum;
		xmax = ai.maximum;
	}
	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0 && ai.maximum > ai.minimum) {
		ymin = ai.minimum;
		ymax = ai.maximum;
	}
	fprintf(stderr, "u30ui: touch %s, x %d..%d, y %d..%d, mirrored x %d y %d\n", path, xmin, xmax, ymin, ymax,
		flipx, flipy);
	return fd;
}

static int sx(int v)
{
	int p = (v - xmin) * 239 / (xmax - xmin);

	return flipx ? 239 - p : p;
}

static int sy(int v)
{
	int p = (v - ymin) * 239 / (ymax - ymin);

	return flipy ? 239 - p : p;
}

/* a long press is due while the finger stays: the main loop asks with its timeout */
int input_poll_long(struct gesture *g)
{
	if (down && !long_sent && !moved && now_ms() - t0 >= LONG_MS) {
		long_sent = 1;
		g->type = G_LONG;
		g->x = x0;
		g->y = y0;
		return 1;
	}
	return 0;
}

int input_ms_to_long(void)
{
	long left;

	if (!down || long_sent || moved)
		return -1;
	left = LONG_MS - (now_ms() - t0);
	return left < 0 ? 0 : (int)left;
}

int input_read(struct gesture *g)
{
	struct input_event ev;
	int got = 0, touch = -1;

	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_ABS && (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X))
			x = sx(ev.value);
		else if (ev.type == EV_ABS && (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y))
			y = sy(ev.value);
		else if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
			touch = ev.value;
		else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID)
			touch = ev.value >= 0;
		else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			if (touch == 1 && !down) {
				down = 1;
				x0 = x;
				y0 = y;
				t0 = now_ms();
				long_sent = moved = 0;
				g->type = G_DOWN;
				g->x = x;
				g->y = y;
				got = 1;
			} else if (touch == 0 && down) {
				int dx = x - x0, dy = y - y0;

				down = 0;
				if (long_sent)
					g->type = G_UP;
				else if (abs(dx) >= SWIPE && abs(dx) > abs(dy))
					g->type = dx < 0 ? G_SWIPE_LEFT : G_SWIPE_RIGHT;
				else if (abs(dy) >= SWIPE && abs(dy) > abs(dx))
					g->type = dy < 0 ? G_SWIPE_UP : G_SWIPE_DOWN;
				else if (!moved)
					g->type = G_TAP;
				else
					g->type = G_UP;
				g->x = x0;
				g->y = y0;
				g->dx = dx;
				g->dy = dy;
				got = 1;
			} else if (down) {
				if (abs(x - x0) > SLOP || abs(y - y0) > SLOP)
					moved = 1;
				g->type = G_MOVE;
				g->x = x;
				g->y = y;
				g->dx = x - x0;
				g->dy = y - y0;
				got = 1;
			}
			touch = -1;
			if (got)
				return 1;
		}
	}
	return got;
}

int input_is_down(void)
{
	return down;
}
