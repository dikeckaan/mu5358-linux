/*
 * sim: u30ui2 on the host (tests/test_u30ui2.py). The real ui.c with a virtual clock, stubs for the display, the touch
 * panel and the data thread, commands on stdin, PNG screenshots into the directory given as the argument:
 *   tap X Y | down X Y | move X Y | up | swipe X1 Y1 X2 Y2 MS | hold X Y MS | key CODE short|long | wait MS
 *   page NAME | lang en|tr|zh | set FIELD VALUE | uevent NAME 0|1 | screen on|off | cmd LINE | shot NAME | state
 * Every data_run() is printed as "RUN cmd" instead of running; "state" prints one "STATE k=v ..." line. An unknown
 * line ends the run with exit status 2. The event language follows the fork's u30pro-panel simulator
 * (kanoqwq/mu300-linux u30pro-bringup, MIT). SPDX-License-Identifier: MIT
 */
#define UI_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "data.h"
#include "sample.h"
#include "../u30ui/png.h"

/* what the device would do */
struct data D;
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int data_screen_on = 1;
volatile int data_want_fast;
void data_start(void) {}
void data_poke(void) {}
void data_run(const char *c) { printf("RUN %s\n", c); fflush(stdout); }
static int sim_screen = 1, sim_down, sim_moved, sim_long_sent, sim_x0, sim_y0, sim_x, sim_y;
static long sim_now = 1000000, sim_t0;
int display_init(void) { return 0; }
void display_present(void) {}
void display_backlight(int on) { sim_screen = on; }
int display_is_on(void) { return sim_screen; }
long now_ms(void) { return sim_now; }
int input_open(void) { return -1; }
int input_ms_to_long(void) { return -1; }
int input_is_down(void) { return sim_down; }
struct gesture;
int input_read(struct gesture *g) { (void)g; return 0; }
int input_poll_long(struct gesture *g) { (void)g; return 0; }

#define main u30ui_main
#include "ui.c"
#undef main

#define SIM_LONG_MS 600		/* tools/u30ui/input.c */
#define SIM_SWIPE 40
#define SIM_SLOP 12
static const char *page_names[NPAGES] = { "home", "net", "wifi", "clients", "traffic", "sms", "battery", "system", "settings", "power", "log" };
static char outdir[512];

static void bump(void) { D.serial++; }

static void event(int type, int x, int y, int dx, int dy)
{
	struct gesture g = { type, x, y, dx, dy };

	if (sim_screen)
		gesture(&g, sim_now);
	ui_redraw = 1;
}

/* the clock runs in 16 ms steps, ui_step() at each, a long press when the finger stayed long enough */
static void advance(long ms)
{
	long end = sim_now + ms;

	while (sim_now < end) {
		sim_now += end - sim_now < 16 ? end - sim_now : 16;
		if (sim_down && !sim_long_sent && !sim_moved && sim_now - sim_t0 >= SIM_LONG_MS) {
			sim_long_sent = 1;
			event(G_LONG, sim_x0, sim_y0, 0, 0);
		}
		ui_step(sim_now);
	}
}

static void finger_down(int x, int y)
{
	sim_down = 1; sim_moved = sim_long_sent = 0;
	sim_x0 = sim_x = x; sim_y0 = sim_y = y; sim_t0 = sim_now;
	event(G_DOWN, x, y, 0, 0);
}

static void finger_move(int x, int y)
{
	if (!sim_down)
		return;
	sim_x = x; sim_y = y;
	if (abs(x - sim_x0) > SIM_SLOP || abs(y - sim_y0) > SIM_SLOP)
		sim_moved = 1;
	event(G_MOVE, x, y, x - sim_x0, y - sim_y0);
}

static void finger_up(void)
{
	int dx = sim_x - sim_x0, dy = sim_y - sim_y0, type;

	if (!sim_down)
		return;
	sim_down = 0;
	if (sim_long_sent)
		type = G_UP;
	else if (abs(dx) >= SIM_SWIPE && abs(dx) > abs(dy))
		type = dx < 0 ? G_SWIPE_LEFT : G_SWIPE_RIGHT;
	else if (abs(dy) >= SIM_SWIPE && abs(dy) > abs(dx))
		type = dy < 0 ? G_SWIPE_UP : G_SWIPE_DOWN;
	else if (!sim_moved)
		type = G_TAP;
	else
		type = G_UP;
	event(type, sim_x0, sim_y0, dx, dy);
}

static int page_of(const char *name)
{
	int i;

	for (i = 0; i < NPAGES; i++)
		if (!strcmp(name, page_names[i]))
			return i;
	return -1;
}

static int set_field(const char *f, const char *v)
{
	int n = atoi(v);

	if (!strcmp(f, "lock")) { lock_mode_hold = !strcmp(v, "hold"); locked = lock_mode_hold; return 1; }
	if (!strcmp(f, "timeout")) { timeout_s = n; return 1; }
	if (!strcmp(f, "nfc")) D.nfc = n;
	else if (!strcmp(f, "usb_storage")) D.usb_storage = n;
	else if (!strcmp(f, "battery")) D.bat_pct = n;
	else if (!strcmp(f, "charging")) D.charging = n;
	else if (!strcmp(f, "usb_online")) D.usb_online = n;
	else if (!strcmp(f, "wifi")) D.wifi_on = n;
	else if (!strcmp(f, "signal")) D.bars = n;
	else if (!strcmp(f, "nsms")) D.nsms = n;
	else if (!strcmp(f, "update")) snprintf(D.update, sizeof(D.update), "%s", strcmp(v, "-") ? v : "");
	else return 0;
	bump();
	return 1;
}

static void state(void)
{
	printf("STATE page=%s screen=%s locked=%d lang=%s timeout=%d dragging=%d settle=%d hold=%d\n", page_names[page],
	       sim_screen ? "on" : "off", locked, lang == L_TR ? "tr" : lang == L_ZH ? "zh" : "en", timeout_s, dragging,
	       settle_t0 ? 1 : 0, hold_row);
	fflush(stdout);
}

static int command(char *line)
{
	char a[64], b[64];
	int x, y, x2, y2, ms;

	line[strcspn(line, "\r\n")] = 0;
	if (!line[0] || line[0] == '#')
		return 1;
	if (sscanf(line, "tap %d %d", &x, &y) == 2) { finger_down(x, y); advance(50); finger_up(); return 1; }
	if (sscanf(line, "down %d %d", &x, &y) == 2) { finger_down(x, y); return 1; }
	if (sscanf(line, "move %d %d", &x, &y) == 2) { finger_move(x, y); return 1; }
	if (!strcmp(line, "up")) { finger_up(); return 1; }
	if (sscanf(line, "swipe %d %d %d %d %d", &x, &y, &x2, &y2, &ms) == 5) {
		int i;

		finger_down(x, y);
		for (i = 1; i <= 6; i++) {
			advance(ms / 6);
			finger_move(x + (x2 - x) * i / 6, y + (y2 - y) * i / 6);
		}
		finger_up();
		return 1;
	}
	if (sscanf(line, "hold %d %d %d", &x, &y, &ms) == 3) { finger_down(x, y); advance(ms); finger_up(); return 1; }
	if (sscanf(line, "key %d %63s", &x, a) == 2) {
		/* what the buttons service does with the power key (openwrt/customize.sh): short = backlight, long = the menu */
		if (x == 116 && !strcmp(a, "short")) display_backlight(!display_is_on());
		else if (x == 116 && !strcmp(a, "long")) ui_command("power");
		return 1;
	}
	if (sscanf(line, "uevent %63s %d", a, &x) == 2) {
		/* the kernel's packet for a power_supply change: "change@path", then KEY=VALUE strings, NUL-separated */
		char u[256];
		int n = 0;

		n += 1 + sprintf(u + n, "change@/devices/platform/%s", a);
		n += 1 + sprintf(u + n, "ACTION=change");
		n += 1 + sprintf(u + n, "SUBSYSTEM=power_supply");
		n += 1 + sprintf(u + n, "POWER_SUPPLY_NAME=%s", a);
		n += 1 + sprintf(u + n, "POWER_SUPPLY_ONLINE=%d", x);
		power_uevent(u, n);
		return 1;
	}
	if (sscanf(line, "wait %d", &ms) == 1) { advance(ms); return 1; }
	if (sscanf(line, "page %63s", a) == 1) {
		int p = page_of(a);

		if (p < 0)
			return 0;
		page_leave();
		page = p;
		ui_redraw = 1;
		return 1;
	}
	if (sscanf(line, "lang %63s", a) == 1) { lang = !strcmp(a, "tr") ? L_TR : !strcmp(a, "zh") ? L_ZH : L_EN; ui_redraw = 1; return 1; }
	if (sscanf(line, "set %63s %63s", a, b) == 2) return set_field(a, b);
	if (sscanf(line, "screen %63s", a) == 1) { display_backlight(!strcmp(a, "on")); return 1; }
	if (!strncmp(line, "cmd ", 4)) return ui_command(line + 4);
	if (sscanf(line, "shot %63s", a) == 1) {
		char path[640];

		ui_step(sim_now);
		draw(sim_now);
		snprintf(path, sizeof(path), "%s/%s.png", outdir, a);
		png_write(path);
		printf("SHOT %s\n", a);
		return 1;
	}
	if (!strcmp(line, "state")) { state(); return 1; }
	return 0;
}

int main(int argc, char **argv)
{
	char line[512];

	if (argc != 2) {
		fprintf(stderr, "usage: sim <out-dir> < script\n");
		return 2;
	}
	snprintf(outdir, sizeof(outdir), "%s", argv[1]);
	setenv("TZ", "Europe/Istanbul", 1);
	ui_init();
	sample_data(&D);
	D.serial = 1;
	display_backlight(1);
	last_touch = sim_now;
	ui_step(sim_now);
	while (fgets(line, sizeof(line), stdin)) {
		if (!command(line)) {
			fprintf(stderr, "sim: unknown command: %s\n", line);
			return 2;
		}
		ui_step(sim_now);
	}
	return 0;
}
