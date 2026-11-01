/*
 * u30ui2: the dashboard - the U30 Pro's 240x240 round screen as a router's control panel, in C on DRM: a status
 * bar, pages side by side (home with the live throughput graph, network, Wi-Fi, clients, traffic, messages,
 * battery, system, settings, power, log), swiped sideways; a swipe down goes home; English, Turkish, Chinese.
 * One static binary, no toolkit; it draws only when something it shows has changed (and never while the screen
 * is off). The launcher-look UI (tools/u30ui) stays as the "classic" interface.
 * Signals as u30ui: SIGUSR1 writes the frame to /tmp/u30ui.png, SIGUSR2 reads gestures from /tmp/u30ui.cmd.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../u30ui/gfx.h"
#include "../u30ui/icons.h"
#include "../u30ui/display.h"
#include "../u30ui/input.h"
#include "../u30ui/png.h"
#include "../u30ui/qr.h"
#include "data.h"
#include "i18n.h"

#define CONF "/etc/u30ui2.conf"
#define UI_FILE "/etc/mu5358/ui"
#define BL "/sys/class/backlight/sprd_backlight/brightness"
#define RELOCK_MS 120000
#define HOLD_MS 1000
#define SETTLE_MS 180

/* the palette: black outside and behind (the panel's bezel is black), cool greys, a blue accent */
#define K_BG 0x000000
#define K_SURF 0x14171c
#define K_SURF2 0x1f242c
#define K_LINE 0x262b34
#define K_FG 0xf2f4f7
#define K_DIM 0x9aa3b2
#define K_DIM2 0x5c6573
#define K_ACC 0x3b82f6
#define K_DOWN 0x22c55e
#define K_UP 0x60a5fa
#define K_WARN 0xf59e0b
#define K_ERR 0xef4444

enum { P_HOME, P_NET, P_WIFI, P_CLIENTS, P_TRAFFIC, P_SMS, P_BATTERY, P_SYSTEM, P_SETTINGS, P_POWER, P_LOG, NPAGES };

static int page = P_HOME, locked = 1, lock_mode_hold = 1;
static int timeout_s = 60, brightness = 50;	/* screen timeout (0 never); backlight 25/50/75/100 */
static long last_touch, flash_until, hint_until;
static int drag_dx, dragging, scroll;
static long settle_t0;
static int settle_from, settle_to;
static int hold_row = -1;
static long hold_t0;
static int wifi_qr, wifi_show_key, sms_open = -1;
static struct data S;
static volatile sig_atomic_t want_shot, want_cmd;

static void on_usr1(int sig) { (void)sig; want_shot = 1; }
static void on_usr2(int sig) { (void)sig; want_cmd = 1; }

/* --- settings */
static void conf_load(void)
{
	FILE *f = fopen(CONF, "r");
	char b[64];

	while (f && fgets(b, sizeof(b), f)) {
		sscanf(b, "timeout=%d", &timeout_s);
		sscanf(b, "brightness=%d", &brightness);
		if (!strncmp(b, "lock=off", 8))
			lock_mode_hold = 0;
	}
	if (f)
		fclose(f);
	if (!lock_mode_hold)
		locked = 0;
	if (brightness < 25 || brightness > 100)
		brightness = 50;
}

static void conf_save(void)
{
	FILE *f = fopen(CONF, "w");

	if (!f)
		return;
	fprintf(f, "timeout=%d\nlock=%s\nbrightness=%d\n", timeout_s, lock_mode_hold ? "hold" : "off", brightness);
	fclose(f);
}

static void apply_brightness(void)
{
	FILE *f = fopen(BL, "w");

	if (f) {
		fprintf(f, "%d\n", 4095 * brightness / 100);
		fclose(f);
	}
}

/* --- helpers */
#define TOP(y, px) ((y) + 0.93f * (px))
#define HW(y) sqrtf(14400.0f - ((y) - 120.0f) * ((y) - 120.0f))	/* half the chord at y: what fits */

static float ease(float t) { return 1 - (1 - t) * (1 - t); }

static void text_fit(int font, float px, float x, float y, int align, uint32_t c, const char *s, int maxw)
{
	char b[256];
	int n = snprintf(b, sizeof(b), "%s", s);

	/* snprintf's answer is the whole string's length, not the buffer's: a longer string is cut here, at a
	 * character boundary, with room for the ellipsis (3 bytes) and the NUL */
	int cut = 0;

	if (n < 0)
		n = 0;
	if (n > (int)sizeof(b) - 4) {
		n = (int)sizeof(b) - 4;
		while (n > 0 && (b[n] & 0xc0) == 0x80)
			n--;
		strcpy(b + n, "…");
		cut = 1;
	}
	if (!cut && gfx_text_width(font, px, b) <= maxw) {
		gfx_text(font, px, x, y, align, c, b);
		return;
	}
	while (n > 0) {
		do
			n--;
		while (n > 0 && (b[n] & 0xc0) == 0x80);
		strcpy(b + n, "…");
		if (gfx_text_width(font, px, b) <= maxw)
			break;
	}
	gfx_text(font, px, x, y, align, c, b);
}

/* wrapped text, centred, line by line until ymax; returns the next y */
static float text_wrap(float px, float y, uint32_t c, const char *t, int maxw, float lh, float ymax)
{
	char line[256];

	while (*t && y < ymax) {
		int n = 0, last_space = -1, w;

		while (t[n]) {
			int m = n + 1;

			while ((t[m] & 0xc0) == 0x80)
				m++;
			snprintf(line, sizeof(line), "%.*s", m, t);
			w = gfx_text_width(FONT_REGULAR, px, line);
			if (w > maxw)
				break;
			if (t[n] == ' ')
				last_space = n;
			n = m;
		}
		if (t[n] && last_space > 0)
			n = last_space;
		if (n == 0)
			n = 1;
		snprintf(line, sizeof(line), "%.*s", n, t);
		gfx_text(FONT_REGULAR, px, 120, y, ALIGN_CENTER, c, line);
		t += n;
		while (*t == ' ')
			t++;
		y += lh;
	}
	return y;
}

/* bits a second as "2.4" + "Mbit/s" */
static void rate(double bps, char *num, char *unit)
{
	if (bps >= 1e6) {
		sprintf(num, bps >= 1e7 ? "%.0f" : "%.1f", bps / 1e6);
		strcpy(unit, "Mbit/s");
	} else {
		sprintf(num, "%.0f", bps / 1e3);
		strcpy(unit, "kbit/s");
	}
}

static void bytes(unsigned long long b, char *out)
{
	if (b >= 1ULL << 30)
		sprintf(out, "%.2f GB", b / 1073741824.0);
	else if (b >= 1ULL << 20)
		sprintf(out, "%.1f MB", b / 1048576.0);
	else
		sprintf(out, "%llu KB", b >> 10);
}

static void duration(long s, char *out)
{
	if (s >= 86400)
		sprintf(out, T(S_DAYS_H), (int)(s / 86400), (int)(s % 86400 / 3600));
	else if (s >= 3600)
		sprintf(out, T(S_HOURS_MIN), (int)(s / 3600), (int)(s % 3600 / 60));
	else
		sprintf(out, T(S_MINUTES), (int)(s / 60));
}

static uint32_t bat_colour(int pct)
{
	return pct < 0 ? K_DIM2 : pct > 30 ? K_DOWN : pct > 15 ? K_WARN : K_ERR;
}

static void signal_bars(float x, float top, float scale, int level)
{
	int i;

	for (i = 0; i < 5; i++) {
		float h = (4 + 3 * i) * scale, w = 4 * scale;

		gfx_round_rect(x + i * 7 * scale, top + 16 * scale - h, w, h, 1.5f * scale, i < level ? K_FG : K_LINE);
	}
}

static void battery_icon(float x, float y, float w, float h, int pct, int charging)
{
	float fw = (w - 4) * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100.0f;

	gfx_round_rect_stroke(x, y, w, h, 2, 1.2f, K_DIM);
	gfx_round_rect(x + w, y + h / 2 - 2, 1.5f, 4, 0.7f, K_DIM);
	if (fw > 0)
		gfx_round_rect(x + 2, y + 2, fw, h - 4, 1, charging ? K_ACC : bat_colour(pct));
}

/* a label on the left, a value on the right, on one row */
static void row(float y, const char *label, const char *value, uint32_t vc)
{
	float hw = HW(y) - 14;

	gfx_text(FONT_REGULAR, 10, 120 - hw, TOP(y, 10), ALIGN_LEFT, K_DIM, label);
	text_fit(FONT_MEDIUM, 11, 120 + hw, TOP(y - 1, 11), ALIGN_RIGHT, vc, value, (int)(2 * hw) - gfx_text_width(FONT_REGULAR, 10, label) - 8);
}

static void hairline(float y)
{
	float hw = HW(y) - 14;

	gfx_fill_rect((int)(120 - hw), (int)y, (int)(2 * hw), 1, K_LINE);
}

/* a horizontal bar of pct */
static void bar(float x, float y, float w, float h, int pct, uint32_t c)
{
	gfx_round_rect(x, y, w, h, h / 2, K_LINE);
	if (pct > 0)
		gfx_round_rect(x, y, w * (pct > 100 ? 100 : pct) / 100.0f, h, h / 2, c);
}

/* the throughput graph: rx as an area, tx as a line, the last n seconds, in the box */
static void graph(float x, float y, float w, float h, int n)
{
	float mx = 1e5f, cw = w / n;
	int i;

	for (i = HIST - n; i < HIST; i++) {
		if (S.rx_hist[i] > mx) mx = S.rx_hist[i];
		if (S.tx_hist[i] > mx) mx = S.tx_hist[i];
	}
	gfx_fill_rect((int)x, (int)(y + h), (int)w, 1, K_LINE);
	for (i = 0; i < n; i++) {
		float rv = S.rx_hist[HIST - n + i] / mx * h, tv = S.tx_hist[HIST - n + i] / mx * h;
		int cx = (int)(x + i * cw), px, py;

		for (py = (int)(y + h - rv); py < (int)(y + h); py++)
			for (px = cx; px < (int)(x + (i + 1) * cw); px++)
				gfx_blend(px + gfx_ox, py, K_DOWN, 70);
		if (i > 0) {
			float prv = S.rx_hist[HIST - n + i - 1] / mx * h, ptv = S.tx_hist[HIST - n + i - 1] / mx * h;

			gfx_line(cx, y + h - prv, cx + cw, y + h - rv, 1.5f, K_DOWN);
			gfx_line(cx, y + h - ptv, cx + cw, y + h - tv, 1.5f, K_UP);
		}
	}
}

static void arrow(float cx, float cy, float s, int down, uint32_t c)
{
	if (down)
		gfx_triangle(cx - s, cy - s * 0.8f, cx + s, cy - s * 0.8f, cx, cy + s * 0.8f, c);
	else
		gfx_triangle(cx - s, cy + s * 0.8f, cx + s, cy + s * 0.8f, cx, cy - s * 0.8f, c);
}

static void page_title(int id)
{
	gfx_text(FONT_MEDIUM, 11, 120, TOP(42, 11), ALIGN_CENTER, K_DIM, T(id));
	gfx_fill_rect(100, 58, 40, 1, K_LINE);
}

/* --- the chrome: status bar (always), page dots or the padlock */
static void draw_status_bar(void)
{
	time_t wall = time(NULL);
	struct tm tm;
	char b[16];

	localtime_r(&wall, &tm);
	strftime(b, sizeof(b), "%H:%M", &tm);
	gfx_text(FONT_MEDIUM, 13, 120, TOP(14, 13), ALIGN_CENTER, K_FG, b);
	if (S.sim && S.bars) {
		signal_bars(50, 16, 0.65f, S.bars);
		gfx_text(FONT_REGULAR, 9, 76, TOP(18, 9), ALIGN_LEFT, K_DIM, S.net);
	} else {
		gfx_text(FONT_REGULAR, 9, 50, TOP(18, 9), ALIGN_LEFT, K_DIM2, T(S.sim ? S_SEARCHING : S_NO_SIM));
	}
	battery_icon(150, 17, 18, 9, S.bat_pct, S.charging);
	snprintf(b, sizeof(b), "%d%%", S.bat_pct < 0 ? 0 : S.bat_pct);
	gfx_text(FONT_REGULAR, 9, 191, TOP(18, 9), ALIGN_RIGHT, S.bat_pct >= 0 && S.bat_pct <= 15 ? K_ERR : K_DIM, b);
}

static void draw_dots(long now)
{
	int i;
	float x0 = 120 - (NPAGES - 1) * 3.5f;

	if (locked) {
		if (now < hint_until)
			gfx_text(FONT_REGULAR, 9, 120, TOP(219, 9), ALIGN_CENTER, K_FG, T(S_UNLOCK_HINT));
		else
			icon_padlock(120, 226, now < flash_until ? 12 : 9, now < flash_until ? K_FG : K_DIM2);
		return;
	}
	for (i = 0; i < NPAGES; i++)
		gfx_circle(x0 + i * 7, 226, i == page ? 2.2f : 1.4f, i == page ? K_FG : K_DIM2);
}

/* --- pages */
static void draw_home(void)
{
	char num[16], unit[16], b[64], c[32];

	if (!S.sim)
		gfx_text(FONT_MEDIUM, 14, 120, TOP(40, 14), ALIGN_CENTER, K_DIM, T(S_NO_SIM));
	else if (!S.oper[0])
		gfx_text(FONT_MEDIUM, 14, 120, TOP(40, 14), ALIGN_CENTER, K_DIM, T(S_SEARCHING));
	else
		text_fit(FONT_MEDIUM, 14, 120, TOP(40, 14), ALIGN_CENTER, K_FG, S.oper, 150);
	if (S.rsrp) {
		if (S.band)
			snprintf(b, sizeof(b), "%s B%d · %d dBm%s", S.net, S.band, S.rsrp, S.roaming ? " · R" : "");
		else
			snprintf(b, sizeof(b), "%s · %d dBm", S.net, S.rsrp);
	} else {
		snprintf(b, sizeof(b), "%s", T(S.sim ? S_NO_SERVICE : S_DISCONNECTED));
	}
	gfx_text(FONT_REGULAR, 10, 120, TOP(58, 10), ALIGN_CENTER, K_DIM, b);
	graph(40, 72, 160, 44, HIST);
	/* the two rates, in two rows */
	rate(S.rx_bps, num, unit);
	arrow(52, 144, 6, 1, K_DOWN);
	gfx_text(FONT_MEDIUM, 28, 66, 154, ALIGN_LEFT, K_DOWN, num);
	gfx_text(FONT_REGULAR, 11, 66 + gfx_text_width(FONT_MEDIUM, 28, num) + 5, 154, ALIGN_LEFT, K_DIM, unit);
	rate(S.tx_bps, num, unit);
	arrow(52, 178, 6, 0, K_UP);
	gfx_text(FONT_MEDIUM, 28, 66, 188, ALIGN_LEFT, K_UP, num);
	gfx_text(FONT_REGULAR, 11, 66 + gfx_text_width(FONT_MEDIUM, 28, num) + 5, 188, ALIGN_LEFT, K_DIM, unit);
	if (S.nclients == 1)
		snprintf(b, sizeof(b), "%s", T(S_ONE_CLIENT));
	else
		snprintf(b, sizeof(b), T(S_N_CLIENTS), S.nclients);
	bytes(S.today_rx + S.today_tx ? S.today_rx + S.today_tx : S.rx_total + S.tx_total, c);
	snprintf(num, sizeof(num), "%s", b);
	snprintf(b, sizeof(b), "%s · %s %s", num, c, T(S.today_rx + S.today_tx ? S_TODAY_LC : S_SINCE_BOOT));
	gfx_text(FONT_REGULAR, 10, 120, TOP(202, 10), ALIGN_CENTER, K_DIM, b);
}

static void draw_net(void)
{
	char b[64];
	int i, have = 0;

	page_title(S_NETWORK);
	/* the signal's last 10 minutes as a sparkline */
	for (i = 0; i < SIGHIST; i++)
		if (S.sig_hist[i])
			have = 1;
	if (have) {
		float x0 = 56, w = 128, h = 14, y0 = 62;

		for (i = 1; i < SIGHIST; i++) {
			float a = S.sig_hist[i - 1], c = S.sig_hist[i];

			if (!a || !c)
				continue;
			gfx_line(x0 + (i - 1) * w / (SIGHIST - 1), y0 + h - (a + 125) / 45.0f * h,
				 x0 + i * w / (SIGHIST - 1), y0 + h - (c + 125) / 45.0f * h, 1.5f, K_ACC);
		}
		gfx_text(FONT_REGULAR, 7, 184, TOP(60, 7), ALIGN_RIGHT, K_DIM2, T(S_SIGNAL_10MIN));
	}
	row(90, T(S_OPERATOR), S.oper[0] ? S.oper : T(S.sim ? S_SEARCHING : S_NO_SIM), K_FG);
	if (S.band)
		snprintf(b, sizeof(b), "%s B%d · %d", S.net, S.band, S.earfcn);
	else
		snprintf(b, sizeof(b), "%s", S.net[0] ? S.net : "-");
	row(108, T(S_BAND), b, K_FG);
	if (S.cell[0])
		snprintf(b, sizeof(b), "%s · PCI %d", S.cell, S.pci);
	else
		strcpy(b, "-");
	row(126, T(S_CELL), b, K_FG);
	if (S.rsrp)
		snprintf(b, sizeof(b), "%d dBm · %d dB", S.rsrp, S.rsrq);
	else
		snprintf(b, sizeof(b), "%s", T(S_NO_SERVICE));
	row(144, T(S_SIGNAL), b, S.rsrp >= -95 ? K_DOWN : S.rsrp >= -110 ? K_WARN : K_ERR);
	row(162, T(S_IP), S.wan_ip[0] ? S.wan_ip : T(S_DISCONNECTED), S.wan_ip[0] ? K_FG : K_DIM);
	row(180, T(S_APN), S.apn[0] ? S.apn : "-", K_FG);
	/* the mobile data switch */
	gfx_round_rect(70, 196, 100, 18, 9, S.data_on ? K_ACC : K_SURF2);
	snprintf(b, sizeof(b), "%s: %s", T(S_MOBILE_DATA), T(S.data_on ? S_ON : S_OFF));
	text_fit(FONT_MEDIUM, 9, 120, TOP(201, 9), ALIGN_CENTER, S.data_on ? K_FG : K_DIM, b, 94);
}

static void draw_wifi_qr(void)
{
	static unsigned char m[QR_MAX * QR_MAX];
	char payload[256];
	int n, mod, x, y;
	float x0, y0;

	qr_wifi_payload(payload, sizeof(payload), S.ssid, S.key);
	n = qr_encode(payload, m);
	if (!n)
		return;
	/* black on white whatever the theme: some scanners do not read it the other way round */
	mod = 160 / (n + 2);
	x0 = 120 - mod * n / 2.0f;
	y0 = 116 - mod * n / 2.0f;
	gfx_round_rect(x0 - mod * 1.5f, y0 - mod * 1.5f, mod * (n + 3), mod * (n + 3), 6, K_FG);
	for (y = 0; y < n; y++)
		for (x = 0; x < n; x++)
			if (m[y * n + x])
				gfx_fill_rect((int)x0 + x * mod, (int)y0 + y * mod, mod, mod, K_BG);
	gfx_text(FONT_REGULAR, 10, 120, TOP(206, 10), ALIGN_CENTER, K_DIM, T(S_SCAN_TO_JOIN));
}

static void draw_wifi(void)
{
	char b[80];

	if (wifi_qr) {
		draw_wifi_qr();
		return;
	}
	page_title(S_WIFI);
	icon_draw(IC_WIFI, 120, 80, 26, S.wifi_on ? K_ACC : K_DIM2);
	text_fit(FONT_MEDIUM, 16, 120, TOP(98, 16), ALIGN_CENTER, S.wifi_on ? K_FG : K_DIM, S.ssid[0] ? S.ssid : "-", 170);
	if (S.wifi_on && S.channel)
		snprintf(b, sizeof(b), "%d GHz · %s %d · %d MHz", S.band_ghz, T(S_CHANNEL), S.channel, S.width_mhz);
	else
		snprintf(b, sizeof(b), "%s", T(S.wifi_on ? S_ON : S_OFF));
	gfx_text(FONT_REGULAR, 10, 120, TOP(120, 10), ALIGN_CENTER, K_DIM, b);
	if (S.nclients == 1)
		snprintf(b, sizeof(b), "%s", T(S_ONE_CLIENT));
	else
		snprintf(b, sizeof(b), T(S_N_CLIENTS), S.nclients);
	gfx_text(FONT_REGULAR, 10, 120, TOP(134, 10), ALIGN_CENTER, K_DIM, b);
	hairline(152);
	row(166, T(S_PASSWORD), wifi_show_key ? S.key : "••••••••", K_FG);
	/* two buttons: the hotspot switch and the QR code */
	gfx_round_rect(36, 184, 80, 22, 11, S.wifi_on ? K_SURF2 : K_ACC);
	snprintf(b, sizeof(b), "%s: %s", T(S_HOTSPOT), T(S.wifi_on ? S_ON : S_OFF));
	text_fit(FONT_MEDIUM, 9, 76, TOP(191, 9), ALIGN_CENTER, K_FG, b, 76);
	gfx_round_rect(124, 184, 80, 22, 11, K_SURF2);
	icon_draw(IC_QR, 142, 195, 11, K_FG);
	text_fit(FONT_MEDIUM, 9, 176, TOP(191, 9), ALIGN_CENTER, K_FG, "QR", 40);
}

static void draw_clients(void)
{
	int i, n = 0;

	page_title(S_CLIENTS);
	if (!S.nclients) {
		gfx_text(FONT_REGULAR, 12, 120, TOP(118, 12), ALIGN_CENTER, K_DIM, T(S_NO_CLIENTS));
		return;
	}
	for (i = scroll; i < S.nclients && n < 4; i++, n++) {
		const struct client *c = &S.clients[i];
		float y = 68 + n * 36, hw = HW(y + 10) - 16;
		char rx[24], tx[24], b[64];

		gfx_circle(120 - hw + 5, y + 8, 4, c->signal > -60 ? K_DOWN : c->signal > -75 ? K_WARN : K_DIM);
		text_fit(FONT_MEDIUM, 11, 120 - hw + 16, TOP(y, 11), ALIGN_LEFT, K_FG, c->name[0] ? c->name : c->ip, (int)hw);
		gfx_text(FONT_REGULAR, 8, 120 - hw + 16, TOP(y + 14, 8), ALIGN_LEFT, K_DIM2, c->name[0] ? c->ip : c->mac);
		bytes(c->tx, rx);	/* what we sent the station: its download */
		bytes(c->rx, tx);
		snprintf(b, sizeof(b), "↓%s", rx);
		gfx_text(FONT_REGULAR, 9, 120 + hw, TOP(y + 1, 9), ALIGN_RIGHT, K_DOWN, b);
		snprintf(b, sizeof(b), "↑%s", tx);
		gfx_text(FONT_REGULAR, 9, 120 + hw, TOP(y + 13, 9), ALIGN_RIGHT, K_UP, b);
		if (n < 3)
			hairline(y + 29);
	}
}

static void draw_traffic(void)
{
	char a[24], b[24], c[64];

	page_title(S_TRAFFIC);
	graph(32, 66, 176, 52, HIST);
	gfx_text(FONT_REGULAR, 8, 32, TOP(120, 8), ALIGN_LEFT, K_DIM2, T(S_LAST_60S));
	gfx_circle(120, 124, 2.5f, K_DOWN);
	gfx_text(FONT_REGULAR, 8, 125, TOP(120, 8), ALIGN_LEFT, K_DIM2, T(S_DOWNLOAD));
	gfx_circle(125 + gfx_text_width(FONT_REGULAR, 8, T(S_DOWNLOAD)) + 10, 124, 2.5f, K_UP);
	gfx_text(FONT_REGULAR, 8, 125 + gfx_text_width(FONT_REGULAR, 8, T(S_DOWNLOAD)) + 15, TOP(120, 8), ALIGN_LEFT, K_DIM2, T(S_UPLOAD));
	bytes(S.today_rx, a);
	bytes(S.today_tx, b);
	snprintf(c, sizeof(c), "↓%s ↑%s", a, b);
	row(142, T(S_TODAY), c, K_FG);
	bytes(S.month_rx, a);
	bytes(S.month_tx, b);
	snprintf(c, sizeof(c), "↓%s ↑%s", a, b);
	row(164, T(S_THIS_MONTH), c, K_FG);
	bytes(S.rx_total, a);
	bytes(S.tx_total, b);
	snprintf(c, sizeof(c), "↓%s ↑%s", a, b);
	row(186, T(S_SINCE_BOOT), c, K_DIM);
	if (!S.data_on)
		gfx_text(FONT_REGULAR, 9, 120, TOP(204, 9), ALIGN_CENTER, K_WARN, T(S_DATA_OFF));
}

static void draw_sms(void)
{
	int i, n = 0;

	if (sms_open >= 0 && sms_open < S.nsms) {
		const struct sms *m = &S.sms[sms_open];

		text_fit(FONT_MEDIUM, 13, 120, TOP(42, 13), ALIGN_CENTER, K_FG, m->sender, 150);
		gfx_text(FONT_REGULAR, 9, 120, TOP(60, 9), ALIGN_CENTER, K_DIM2, m->time);
		gfx_fill_rect(100, 74, 40, 1, K_LINE);
		text_wrap(11, TOP(82, 11), K_FG, m->text, 180, 15, 200);
		gfx_text(FONT_REGULAR, 8, 120, TOP(208, 8), ALIGN_CENTER, K_DIM2, T(S_SWIPE_DOWN_CLOSE));
		return;
	}
	page_title(S_MESSAGES);
	if (!S.nsms) {
		gfx_text(FONT_REGULAR, 12, 120, TOP(118, 12), ALIGN_CENTER, K_DIM, T(S_NO_MESSAGES));
		return;
	}
	for (i = scroll; i < S.nsms && n < 4; i++, n++) {
		const struct sms *m = &S.sms[i];
		float y = 68 + n * 36, hw = HW(y + 10) - 16;

		text_fit(FONT_MEDIUM, 11, 120 - hw, TOP(y, 11), ALIGN_LEFT, K_FG, m->sender, (int)(hw * 1.2f));
		gfx_text(FONT_REGULAR, 8, 120 + hw, TOP(y + 2, 8), ALIGN_RIGHT, K_DIM2, strlen(m->time) > 10 ? m->time + 11 : m->time);
		text_fit(FONT_REGULAR, 9, 120 - hw, TOP(y + 14, 9), ALIGN_LEFT, K_DIM, m->text, (int)(2 * hw));
		if (n < 3)
			hairline(y + 29);
	}
}

static void draw_battery(void)
{
	char b[64];
	uint32_t c = S.charging ? K_ACC : bat_colour(S.bat_pct);

	page_title(S_BATTERY);
	gfx_arc(120, 128, 54, 7, 0, 360, K_LINE);
	if (S.bat_pct > 0)
		gfx_arc(120, 128, 54, 7, 0, 3.6f * S.bat_pct, c);
	snprintf(b, sizeof(b), "%d%%", S.bat_pct < 0 ? 0 : S.bat_pct);
	gfx_text(FONT_MEDIUM, 30, 120, 138, ALIGN_CENTER, K_FG, b);
	gfx_text(FONT_REGULAR, 10, 120, TOP(144, 10), ALIGN_CENTER, K_DIM,
		 T(S.bat_full ? S_FULL : S.charging ? S_CHARGING : S.usb_online ? S_USB_POWER : S_ON_BATTERY));
	snprintf(b, sizeof(b), "%+d mA · %d.%02d V · %d°C", S.bat_ma, S.bat_mv / 1000, S.bat_mv % 1000 / 10,
		 S.bat_temp10 / 10);
	gfx_text(FONT_REGULAR, 10, 120, TOP(192, 10), ALIGN_CENTER, K_DIM, b);
	/* a rough estimate from the current, assuming 4000 mAh */
	if (S.bat_ma < -50 && S.bat_pct > 0) {
		duration(S.bat_pct * 40L * 3600 / -S.bat_ma, b);
		strcat(b, " ");
		strcat(b, T(S_LEFT));
		gfx_text(FONT_REGULAR, 9, 120, TOP(206, 9), ALIGN_CENTER, K_DIM2, b);
	} else if (S.charging && S.bat_ma > 50 && S.bat_pct < 100) {
		duration((100 - S.bat_pct) * 40L * 3600 / S.bat_ma, b);
		strcat(b, " ");
		strcat(b, T(S_TO_FULL));
		gfx_text(FONT_REGULAR, 9, 120, TOP(206, 9), ALIGN_CENTER, K_DIM2, b);
	}
}

static void draw_system(void)
{
	char b[64], c[32];
	int i, n = S.ncpu ? S.ncpu : 8;
	float x0 = 120 - n * 9 / 2.0f;

	page_title(S_SYSTEM);
	/* the cores: a bar each, the little clusters apart */
	for (i = 0; i < n; i++) {
		float x = x0 + i * 9 + (i >= 6 ? 4 : 0), h = 22 * S.core_pct[i] / 100.0f;

		gfx_round_rect(x, 64, 6, 22, 2, K_LINE);
		if (h > 1)
			gfx_round_rect(x, 86 - h, 6, h, 2, S.core_pct[i] > 80 ? K_WARN : K_ACC);
	}
	snprintf(b, sizeof(b), "%s %d%% · %d/%d MHz", T(S_CPU), S.cpu_pct, S.freq_mhz[0], S.freq_mhz[n > 6 ? 6 : n - 1]);
	gfx_text(FONT_REGULAR, 9, 120, TOP(90, 9), ALIGN_CENTER, K_DIM, b);
	snprintf(b, sizeof(b), "%ld / %ld MB", S.mem_total_mb - S.mem_free_mb, S.mem_total_mb);
	row(110, T(S_MEMORY), b, K_FG);
	bar(40, 119, 160, 3, S.mem_pct, K_ACC);
	snprintf(b, sizeof(b), "%d%% · %ld MB", S.disk_pct, S.disk_free_mb);
	row(134, T(S_STORAGE), b, K_FG);
	bar(40, 143, 160, 3, S.disk_pct, K_ACC);
	snprintf(b, sizeof(b), "%s %d° · Wi-Fi %d°", T(S_SOC), S.temp_soc, S.temp_wcn);
	row(158, T(S_TEMPS), b, S.temp_soc > 70 ? K_WARN : K_FG);
	snprintf(b, sizeof(b), "PA %d° · %s %d°", S.temp_pa, T(S_BATTERY), S.temp_bat);
	row(174, "", b, K_DIM);
	duration(S.uptime_s, c);
	snprintf(b, sizeof(b), "%.2f · %s", S.load1, c);
	row(192, T(S_LOAD), b, K_FG);
	snprintf(b, sizeof(b), "%s · %s", S.version, S.kernel);
	text_fit(FONT_REGULAR, 7, 120, TOP(206, 7), ALIGN_CENTER, K_DIM2, b, 150);
}

enum { SET_LANG, SET_TIMEOUT, SET_BRIGHT, SET_LOCK, SET_UI, SET_USB, NSET };
static const int set_label[NSET] = { S_LANGUAGE, S_SCREEN_OFF, S_BRIGHTNESS, S_LOCK, S_INTERFACE, S_USB_DRIVE };

static void setting_value(int i, char *v)
{
	switch (i) {
	case SET_LANG: strcpy(v, T(S_LANG_NAME)); break;
	case SET_TIMEOUT:
		if (!timeout_s)
			strcpy(v, T(S_NEVER));
		else
			sprintf(v, T(timeout_s < 60 ? S_SECONDS : S_MINUTES), timeout_s < 60 ? timeout_s : timeout_s / 60);
		break;
	case SET_BRIGHT: sprintf(v, "%d%%", brightness); break;
	case SET_LOCK: strcpy(v, T(lock_mode_hold ? S_HOLD : S_OFF)); break;
	case SET_UI: strcpy(v, T(S_MODERN)); break;
	case SET_USB: strcpy(v, T(S.usb_storage ? S_ON : S_OFF)); break;
	}
}

static void draw_settings(void)
{
	int i;
	char v[48];

	page_title(S_SETTINGS);
	/* six rows, 24 px apart: the last one stays above the page dots on the round screen */
	for (i = 0; i < NSET; i++) {
		float y = 72 + i * 24;

		setting_value(i, v);
		row(y, T(set_label[i]), v, K_ACC);
		if (i < NSET - 1)
			hairline(y + 13);
	}
}

static void settings_tap(int y)
{
	int i = (y - 60) / 24;

	if (y < 60 || i >= NSET)
		return;
	switch (i) {
	case SET_LANG:
		lang = (lang + 1) % NLANG;
		lang_save();
		break;
	case SET_TIMEOUT:
		timeout_s = timeout_s == 30 ? 60 : timeout_s == 60 ? 300 : timeout_s == 300 ? 0 : 30;
		conf_save();
		break;
	case SET_BRIGHT:
		brightness = brightness >= 100 ? 25 : brightness + 25;
		apply_brightness();
		conf_save();
		break;
	case SET_LOCK:
		lock_mode_hold = !lock_mode_hold;
		conf_save();
		break;
	case SET_UI:
		/* the classic interface: mu5358-screen reads /etc/mu5358/ui and starts the other binary */
		data_run("echo classic > " UI_FILE "; /etc/init.d/mu5358-screen restart");
		break;
	case SET_USB:
		/* the device as a USB flash drive for the computer (mu5358-usb storage); the USB link replugs for a moment */
		data_run(S.usb_storage ? "/opt/mu5358/bin/mu5358-usb storage off" : "/opt/mu5358/bin/mu5358-usb storage on");
		break;
	}
}

/* --- power: the first three want a 1 s hold */
static const struct action {
	int label;
	uint32_t colour;
	int hold;
} actions[] = {
	{S_RESTART, K_ERR, 1},
	{S_POWER_OFF, K_ERR, 1},
	{S_RESTART_MODEM, K_WARN, 1},
	{S_RESTART_WIFI, K_WARN, 1},
	{S_SCREEN_OFF_NOW, K_ACC, 0},
};
#define NACT ((int)(sizeof(actions) / sizeof(actions[0])))
static int action_y(int i) { return 72 + i * 30; }

static void draw_power(long now)
{
	int i;

	page_title(S_POWER);
	for (i = 0; i < NACT; i++) {
		float y = action_y(i), hw = HW(y) - 18;

		gfx_round_rect(120 - hw, y - 11, 2 * hw, 24, 12, K_SURF);
		gfx_circle(120 - hw + 12, y + 1, 4, actions[i].colour);
		text_fit(FONT_MEDIUM, 11, 120 - hw + 24, TOP(y - 5, 11), ALIGN_LEFT, K_FG, T(actions[i].label), (int)(2 * hw) - 30);
	}
	gfx_text(FONT_REGULAR, 8, 120, TOP(214, 8), ALIGN_CENTER, K_DIM2, T(S_HOLD_1S));
	if (hold_row >= 0) {
		float f = (now - hold_t0) / (float)HOLD_MS;

		gfx_arc(120, 120, 116, 4, 0, 360 * (f > 1 ? 1 : f), actions[hold_row].colour);
	}
}

static void action_run(int i)
{
	switch (i) {
	case 0: data_run("sync; reboot"); break;
	case 1: data_run("sync; poweroff"); break;
	case 2: data_run("/etc/init.d/mu5358-modem restart"); break;
	case 3: data_run("wifi reload"); break;
	case 4: display_backlight(0); break;
	}
}

static int action_at(int y)
{
	int i;

	for (i = 0; i < NACT; i++)
		if (y >= action_y(i) - 12 && y <= action_y(i) + 14)
			return i;
	return -1;
}

static void draw_log(void)
{
	int i, n = S.nlog - scroll;
	float y = 202;

	page_title(S_LOG);
	if (!S.nlog) {
		gfx_text(FONT_REGULAR, 12, 120, TOP(118, 12), ALIGN_CENTER, K_DIM, T(S_LOG_EMPTY));
		return;
	}
	for (i = n - 1; i >= 0 && y > 66; i--, y -= 12) {
		float hw = HW(y - 4) - 10;

		text_fit(FONT_REGULAR, 8, 120 - hw, y, ALIGN_LEFT, i == n - 1 ? K_FG : K_DIM, S.log[i], (int)(2 * hw));
	}
}

static void draw_page(int p, long now)
{
	switch (p) {
	case P_HOME: draw_home(); break;
	case P_NET: draw_net(); break;
	case P_WIFI: draw_wifi(); break;
	case P_CLIENTS: draw_clients(); break;
	case P_TRAFFIC: draw_traffic(); break;
	case P_SMS: draw_sms(); break;
	case P_BATTERY: draw_battery(); break;
	case P_SYSTEM: draw_system(); break;
	case P_SETTINGS: draw_settings(); break;
	case P_POWER: draw_power(now); break;
	case P_LOG: draw_log(); break;
	}
}

static void draw(long now)
{
	int off = 0;

	gfx_clear(K_BG);
	if (settle_t0) {
		float f = (now - settle_t0) / (float)SETTLE_MS;

		if (f >= 1) {
			page = settle_to;
			settle_t0 = 0;
		} else {
			int target = settle_to == page ? 0 : settle_to > page ? -W : W;

			off = (int)(settle_from + (target - settle_from) * ease(f));
		}
	} else if (dragging) {
		off = drag_dx;
	}
	gfx_ox = off;
	draw_page(page, now);
	if (off > 0 && page > 0) {
		gfx_ox = off - W;
		draw_page(page - 1, now);
	} else if (off < 0 && page < NPAGES - 1) {
		gfx_ox = off + W;
		draw_page(page + 1, now);
	}
	gfx_ox = 0;
	/* the chrome, over everything (the pages keep clear of it) */
	gfx_fill_rect(0, 0, W, 34, K_BG);
	draw_status_bar();
	gfx_fill_rect(0, 216, W, 24, K_BG);
	draw_dots(now);
}

/* --- touches */
static void go_home(void)
{
	wifi_qr = wifi_show_key = 0;
	sms_open = -1;
	page = P_HOME;
	scroll = 0;
	hold_row = -1;
}

static void page_leave(void)
{
	wifi_qr = wifi_show_key = 0;
	sms_open = -1;
	scroll = 0;
	hold_row = -1;
}

static void gesture(struct gesture *g, long now)
{
	last_touch = now;
	if (locked) {
		if (g->type == G_LONG) {
			locked = 0;
			flash_until = hint_until = 0;
		} else if (g->type == G_TAP || g->type == G_UP || g->type == G_SWIPE_LEFT || g->type == G_SWIPE_RIGHT) {
			flash_until = now + 500;
			hint_until = now + 1000;
		}
		return;
	}
	/* sideways: the pages, dragged with the finger */
	if (g->type == G_MOVE && (dragging || (abs(g->dx) > 8 && abs(g->dx) >= abs(g->dy))) && sms_open < 0 && !wifi_qr) {
		dragging = 1;
		hold_row = -1;
		drag_dx = g->dx;
		if ((drag_dx > 0 && page == 0) || (drag_dx < 0 && page == NPAGES - 1))
			drag_dx = (int)(drag_dx * 0.35f);
		return;
	}
	if (dragging && (g->type == G_UP || g->type == G_SWIPE_LEFT || g->type == G_SWIPE_RIGHT || g->type == G_TAP ||
			 g->type == G_SWIPE_UP || g->type == G_SWIPE_DOWN)) {
		int to = page;

		if (g->dx <= -24 && page + 1 < NPAGES)
			to = page + 1;
		else if (g->dx >= 24 && page > 0)
			to = page - 1;
		settle_from = drag_dx;
		settle_to = to;
		settle_t0 = now;
		dragging = 0;
		if (to != page)
			page_leave();
		return;
	}
	if (g->type == G_SWIPE_DOWN) {
		if (sms_open >= 0)
			sms_open = -1;
		else if (wifi_qr)
			wifi_qr = 0;
		else if (scroll > 0)
			scroll--;
		else if (page != P_HOME)
			go_home();
		return;
	}
	if (g->type == G_SWIPE_UP) {
		scroll++;
		return;
	}
	switch (page) {
	case P_NET:
		if (g->type == G_TAP && g->y >= 190)
			data_run(S.data_on ? "uci set network.wan.auto=0; uci commit network; ifdown wan"
					   : "uci set network.wan.auto=1; uci commit network; ifup wan");
		break;
	case P_WIFI:
		if (g->type != G_TAP)
			break;
		if (wifi_qr)
			wifi_qr = 0;
		else if (g->y >= 156 && g->y < 180)
			wifi_show_key = !wifi_show_key;
		else if (g->y >= 180 && g->x < 120)
			data_run(S.wifi_on ? "uci set wireless.ap0.disabled=1; uci commit wireless; wifi reload"
					   : "uci set wireless.ap0.disabled=0; uci commit wireless; wifi reload");
		else if (g->y >= 180)
			wifi_qr = 1;
		break;
	case P_SMS:
		if (g->type == G_TAP) {
			int r = (g->y - 60) / 36 + scroll;

			if (sms_open >= 0)
				sms_open = -1;
			else if (g->y >= 60 && r < S.nsms && r < scroll + 4)
				sms_open = r;
		}
		break;
	case P_SETTINGS:
		if (g->type == G_TAP)
			settings_tap(g->y);
		break;
	case P_POWER: {
		int r = action_at(g->y);

		if (g->type == G_DOWN && r >= 0 && actions[r].hold) {
			hold_row = r;
			hold_t0 = now;
		} else if (g->type == G_TAP && r >= 0 && !actions[r].hold) {
			action_run(r);
		} else if (g->type == G_UP || g->type == G_TAP || (g->type == G_MOVE && action_at(g->y) != hold_row)) {
			hold_row = -1;
		}
		break;
	}
	}
}

int main(void)
{
	struct pollfd pfd;
	long now, last_draw = 0;
	unsigned seen = 0, seen_log = 0;
	int on = 1, last_min = -1, redraw = 1;
	FILE *tz = fopen("/etc/TZ", "r");

	if (tz) {
		char b[64];

		if (fgets(b, sizeof(b), tz)) {
			b[strcspn(b, "\r\n")] = 0;
			setenv("TZ", b, 1);
		}
		fclose(tz);
	}
	tzset();
	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);
	conf_load();
	lang_load();
	gfx_init();
	if (display_init()) {
		fprintf(stderr, "u30ui2: no display\n");
		return 1;
	}
	pfd.fd = input_open();
	pfd.events = POLLIN;
	data_start();
	display_backlight(1);
	apply_brightness();
	last_touch = now_ms();

	for (;;) {
		struct gesture g;
		int timeout = 1000, t;
		time_t wall = time(NULL);
		struct tm tm;

		now = now_ms();
		if (display_is_on() != on) {
			on = !on;
			data_screen_on = on;
			if (on) {
				apply_brightness();
				last_touch = now;
				redraw = 1;
				data_poke();
			} else {
				go_home();
				if (lock_mode_hold)
					locked = 1;
			}
		}
		if (on && timeout_s && now - last_touch > timeout_s * 1000L && !input_is_down()) {
			display_backlight(0);
			on = 0;
			data_screen_on = 0;
			go_home();
			if (lock_mode_hold)
				locked = 1;
		}
		if (on && lock_mode_hold && !locked && now - last_touch > RELOCK_MS) {
			locked = 1;
			go_home();
			redraw = 1;
		}
		data_want_fast = on && (page == P_HOME || page == P_TRAFFIC);

		if (on) {
			pthread_mutex_lock(&data_lock);
			if (D.serial != seen || (page == P_LOG && D.logserial != seen_log)) {
				S = D;
				seen = D.serial;
				seen_log = D.logserial;
				redraw = 1;
			}
			pthread_mutex_unlock(&data_lock);
			localtime_r(&wall, &tm);
			if (tm.tm_min != last_min) {
				last_min = tm.tm_min;
				redraw = 1;
			}
			if (settle_t0 || hold_row >= 0 || now < hint_until + 50 || dragging)
				redraw = 1;
			if (hold_row >= 0 && now - hold_t0 >= HOLD_MS) {
				action_run(hold_row);
				hold_row = -1;
			}
			if (redraw && now - last_draw >= 16) {
				draw(now);
				display_present();
				last_draw = now;
				redraw = 0;
			}
			if (settle_t0 || hold_row >= 0 || dragging)
				timeout = 16;
			else if (now < hint_until)
				timeout = 100;
			t = input_ms_to_long();
			if (t >= 0 && t < timeout)
				timeout = t;
		} else {
			timeout = 500;
		}

		/* test gestures: "tap X Y", "long X Y", "swipe left|right|up|down", "screen on", "lang en|tr|zh" */
		if (want_cmd) {
			FILE *cf = fopen("/tmp/u30ui.cmd", "r");
			char line[64], w[16];
			int cx, cy;

			want_cmd = 0;
			while (cf && fgets(line, sizeof(line), cf)) {
				struct gesture tg = {0};

				if (sscanf(line, "tap %d %d", &cx, &cy) == 2) {
					tg.type = G_DOWN; tg.x = cx; tg.y = cy;
					gesture(&tg, now_ms());
					tg.type = G_TAP;
					gesture(&tg, now_ms());
				} else if (sscanf(line, "long %d %d", &cx, &cy) == 2) {
					tg.type = G_DOWN; tg.x = cx; tg.y = cy;
					gesture(&tg, now_ms());
					tg.type = G_LONG;
					gesture(&tg, now_ms());
					tg.type = G_UP;
					gesture(&tg, now_ms());
				} else if (sscanf(line, "swipe %15s", w) == 1) {
					tg.x = tg.y = 120;
					tg.type = G_DOWN;
					gesture(&tg, now_ms());
					tg.dx = !strcmp(w, "left") ? -60 : !strcmp(w, "right") ? 60 : 0;
					tg.dy = !strcmp(w, "up") ? -60 : !strcmp(w, "down") ? 60 : 0;
					tg.type = !strcmp(w, "left") ? G_SWIPE_LEFT : !strcmp(w, "right") ? G_SWIPE_RIGHT :
						  !strcmp(w, "up") ? G_SWIPE_UP : G_SWIPE_DOWN;
					if (tg.dx) {
						struct gesture mv = tg;

						mv.type = G_MOVE;
						gesture(&mv, now_ms());
					}
					gesture(&tg, now_ms());
					if (tg.dx && settle_t0) {
						settle_t0 = 0;
						page = settle_to;
					}
				} else if (sscanf(line, "lang %15s", w) == 1) {
					lang = !strcmp(w, "tr") ? L_TR : !strcmp(w, "zh") ? L_ZH : L_EN;
				} else if (!strncmp(line, "screen on", 9)) {
					display_backlight(1);
				}
			}
			if (cf)
				fclose(cf);
			redraw = 1;
		}
		if (want_shot) {
			want_shot = 0;
			pthread_mutex_lock(&data_lock);
			S = D;
			pthread_mutex_unlock(&data_lock);
			draw(now_ms());
			png_write("/tmp/u30ui.png");
		}
		if (pfd.fd >= 0 && poll(&pfd, 1, timeout) > 0) {
			while (input_read(&g)) {
				if (on) {
					gesture(&g, now_ms());
					redraw = 1;
				}
			}
		} else if (pfd.fd < 0) {
			usleep(timeout * 1000);
		}
		if (on && input_poll_long(&g)) {
			gesture(&g, now_ms());
			redraw = 1;
		}
	}
	return 0;
}
