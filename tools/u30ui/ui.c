/*
 * u30ui: the U30 Pro's screen under Linux - the launcher's papatya and pages (SPEC.md), natively on DRM.
 * Locked at start (a 600 ms press opens it), locked again after 120 s without a touch; a swipe down goes back; the
 * dangerous actions want a 1 s hold. The backlight goes off after the screen timeout and nothing is drawn then; the
 * power key (mu5358-buttons) switches it on again. Settings are kept in /etc/u30ui.conf.
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
#include "data.h"
#include "display.h"
#include "gfx.h"
#include "icons.h"
#include "input.h"
#include "png.h"
#include "qr.h"

#define CONF "/etc/u30ui.conf"
#define RELOCK_MS 120000
#define HOLD_MS 1000

enum { P_HOME, P_STATUS, P_WIFI, P_SMS, P_BATTERY, P_SETTINGS, P_CLIENTS, P_SIGNAL, P_ACTIONS, P_SYSTEM, P_LOG };

static const struct petal {
	int icon, page;
	uint32_t colour;
	const char *label;
} petals[] = {
	{IC_WIFI, P_WIFI, 0x0a84ff, "WiFi"},
	{IC_SMS, P_SMS, 0xff9f0a, "Mesaj"},
	{IC_BATTERY, P_BATTERY, 0x34c759, "Pil"},
	{IC_SETTINGS, P_SETTINGS, 0x8e8e93, "Ayar"},
	{IC_CLIENTS, P_CLIENTS, 0xaf52de, "İstemci"},
	{IC_SIGNAL, P_SIGNAL, 0x30b0c7, "Sinyal"},
	{IC_POWER, P_ACTIONS, 0x8b0000, "Güç"},
	{IC_SYSTEM, P_SYSTEM, 0xa2845e, "Sistem"},
	{IC_SUMMARY, P_LOG, 0x7d7aff, "Konsol"},
};
#define NPETALS ((int)(sizeof(petals) / sizeof(petals[0])))
#define NPAGES ((NPETALS + 3) / 4)
static const int slot_dx[4] = {0, 80, 0, -80}, slot_dy[4] = {-80, 0, 80, 0};

static int page = P_HOME, home_page, locked = 1, lock_mode_hold = 1;
static long last_touch, flash_until, hint_until;
static int timeout_s = 60;	/* screen timeout; 0 = never */
static int pressed = -1;	/* the petal (or 99 the centre) under the finger */
static int drag_dx, dragging, scroll;
static long settle_t0;
static int settle_from, settle_to_page;
static int hold_row = -1;
static long hold_t0;
static struct data S;	/* the snapshot drawn */
static volatile sig_atomic_t want_shot;	/* SIGUSR1: the frame into /tmp/u30ui.png */
static volatile sig_atomic_t want_cmd;	/* SIGUSR2: gestures from /tmp/u30ui.cmd (tests without a finger) */

static void on_usr2(int sig)
{
	(void)sig;
	want_cmd = 1;
}

static void on_usr1(int sig)
{
	(void)sig;
	want_shot = 1;
}

/* --- settings */
static void conf_load(void)
{
	FILE *f = fopen(CONF, "r");
	char b[64];

	while (f && fgets(b, sizeof(b), f)) {
		sscanf(b, "timeout=%d", &timeout_s);
		if (!strncmp(b, "lock=off", 8))
			lock_mode_hold = 0;
	}
	if (f)
		fclose(f);
	if (!lock_mode_hold)
		locked = 0;
}

static void conf_save(void)
{
	FILE *f = fopen(CONF, "w");

	if (!f)
		return;
	fprintf(f, "timeout=%d\nlock=%s\n", timeout_s, lock_mode_hold ? "hold" : "off");
	fclose(f);
}

/* --- small helpers */
static float ease(float t)
{
	return 1 - (1 - t) * (1 - t);
}

/* text cut to a width, with "..." */
static void text_fit(int font, float px, float x, float y, int align, uint32_t c, const char *s, int maxw)
{
	char b[256];
	int n = snprintf(b, sizeof(b), "%s", s);

	if (gfx_text_width(font, px, b) <= maxw) {
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

/* top-of-text y as the launcher gives it, to a baseline */
#define TOP(y, px) ((y) + 0.93f * (px))

static uint32_t bat_colour(int pct)
{
	return pct < 0 ? C_DIM2 : pct > 30 ? C_DOWN : pct > 15 ? C_WARN : C_ERR;
}

static void signal_bars(float x, float top, float scale, int level)
{
	int i;

	for (i = 0; i < 5; i++) {
		float h = (4 + 3 * i) * scale, w = 4 * scale;

		gfx_round_rect(x + i * 7 * scale, top + 16 * scale - h, w, h, 1.5f * scale, i < level ? C_FG : C_TRACK);
	}
}

static void title(const char *t, float y, float hw)
{
	gfx_text(FONT_REGULAR, 10, 120, TOP(y, 10), ALIGN_CENTER, C_DIM2, t);
	gfx_fill_rect(120 - (int)hw, (int)y + 16, (int)(2 * hw), 1, C_HAIRLINE);
}

static void rate(double bps, char *num, char *unit)
{
	double bits = bps * 8;

	if (bits >= 1e6) {
		sprintf(num, bits >= 1e7 ? "%.0f" : "%.1f", bits / 1e6);
		strcpy(unit, "Mbit/s");
	} else {
		sprintf(num, "%.0f", bits / 1e3);
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

/* --- the papatya */
static void draw_center(void)
{
	char clk[8];
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(clk, sizeof(clk), "%H:%M", &tm);
	gfx_circle(120, 120, 42, C_MENU_BG);
	gfx_arc(120, 120, 39.5f, 5, 0, 360, C_TRACK);
	if (S.bat_pct >= 0)
		gfx_arc(120, 120, 39.5f, 5, 0, 3.6f * S.bat_pct, bat_colour(S.bat_pct));
	gfx_text(FONT_MEDIUM, 24, 120, TOP(100, 24), ALIGN_CENTER, C_FG, clk);
	if (!S.sim) {
		gfx_text(FONT_REGULAR, 10, 120, TOP(129, 10), ALIGN_CENTER, C_DIM, "SIM yok");
	} else {
		int tw = gfx_text_width(FONT_REGULAR, 10, S.net);
		float x0 = 120 - (22.4f + tw + 4) / 2;

		signal_bars(x0, 129, 0.7f, S.bars);
		gfx_text(FONT_REGULAR, 10, x0 + 22.4f + 4, TOP(130, 10), ALIGN_LEFT, C_DIM, S.net);
	}
	if (pressed == 99)
		gfx_circle(120, 120, 42, 0x303032);
}

static void draw_petal(int i, float cx, float cy)
{
	const struct petal *p = &petals[i];

	gfx_circle(cx, cy, 32, p->colour);
	if (pressed == i)
		gfx_circle(cx, cy, 32, ((p->colour >> 1) & 0x7f7f7f));
	icon_draw(p->icon, cx, cy - 6.4f, 22.4f, C_FG);
	text_fit(FONT_MEDIUM, 10.5f, cx, cy + 17.6f, ALIGN_CENTER, C_FG, p->label, 53);
	if (p->page == P_SMS && S.nsms > 0) {
		char n[8];
		float bx = cx + 23.04f, by = cy - 23.04f, hw;

		snprintf(n, sizeof(n), S.nsms > 99 ? "99+" : "%d", S.nsms);
		hw = fmaxf(9, gfx_text_width(FONT_MEDIUM, 10, n) / 2.0f + 5);
		gfx_round_rect(bx - hw - 2, by - 11, 2 * hw + 4, 22, 11, C_BG);
		gfx_round_rect(bx - hw, by - 9, 2 * hw, 18, 9, 0xff3b30);
		gfx_text(FONT_MEDIUM, 10, bx, by + 3.6f, ALIGN_CENTER, C_FG, n);
	}
}

static void draw_home_page(int hp)
{
	int s;

	for (s = 0; s < 4; s++) {
		int i = hp * 4 + s;

		if (i < NPETALS)
			draw_petal(i, 120 + slot_dx[s], 120 + slot_dy[s]);
	}
}

/* the page pills, or the padlock while locked: on the bottom-right diagonal */
static void draw_corner(long now)
{
	const float ax = 190.7f, ay = 190.7f;
	int i;

	if (locked) {
		int fl = now < flash_until;

		icon_padlock(ax, ay, fl ? 13 : 10, fl ? C_FG : C_DIM2);
		if (now < hint_until)
			gfx_text(FONT_REGULAR, 10, 150, 212, ALIGN_CENTER, C_FG, "Basılı tut");
		return;
	}
	/* -45 degrees: the pills go up and to the right */
	{
		float total = 12 + (NPAGES - 1) * 9, pos = -total / 2;

		for (i = 0; i < NPAGES; i++) {
			float w = i == home_page ? 12 : 5, mid = pos + w / 2;
			float px = ax + mid * 0.7071f, py = ay - mid * 0.7071f;

			gfx_line(px - (w / 2 - 2.5f) * 0.7071f, py + (w / 2 - 2.5f) * 0.7071f,
				 px + (w / 2 - 2.5f) * 0.7071f, py - (w / 2 - 2.5f) * 0.7071f, 5,
				 i == home_page ? C_FG : C_DIM2);
			pos += w + 4;
		}
	}
}

static void draw_home(long now)
{
	if (settle_t0) {
		float t = (now - settle_t0) / 180.0f;

		if (t >= 1) {
			settle_t0 = 0;
			home_page = settle_to_page;
			drag_dx = 0;
		} else {
			drag_dx = (int)(settle_from + (-settle_from + (settle_to_page - home_page) * -240) * ease(t));
		}
	}
	if (drag_dx) {
		gfx_ox = drag_dx;
		draw_home_page(home_page);
		if (drag_dx < 0 && home_page + 1 < NPAGES) {
			gfx_ox = drag_dx + 240;
			draw_home_page(home_page + 1);
		} else if (drag_dx > 0 && home_page > 0) {
			gfx_ox = drag_dx - 240;
			draw_home_page(home_page - 1);
		}
		gfx_ox = 0;
	} else {
		draw_home_page(home_page);
	}
	draw_center();
	draw_corner(now);
}

/* --- the pages */
static void draw_status(void)
{
	char b[64], num[16], unit[16], clk[8];
	time_t t = time(NULL);
	struct tm tm;
	float w, x;
	int tw;

	localtime_r(&t, &tm);
	strftime(clk, sizeof(clk), "%H:%M", &tm);
	gfx_text(FONT_REGULAR, 13, 120, TOP(26, 13), ALIGN_CENTER, C_DIM2, clk);
	gfx_text(FONT_REGULAR, 17, 120, TOP(48, 17), ALIGN_CENTER, C_FG, !S.sim ? "SIM yok" : S.oper[0] ? S.oper : "…");
	snprintf(b, sizeof(b), "%s  •  %d dBm", S.net[0] ? S.net : "-", S.rsrp);
	tw = gfx_text_width(FONT_REGULAR, 11, b);
	x = 120 - (32 + 8 + tw) / 2.0f;
	signal_bars(x, 72, 1, S.bars);
	gfx_text(FONT_REGULAR, 11, x + 40, TOP(74, 11), ALIGN_LEFT, C_DIM, b);

	/* download, upload */
	rate(S.rx_bps, num, unit);
	w = 20 + gfx_text_width(FONT_REGULAR, 34, num) + 5 + gfx_text_width(FONT_REGULAR, 11, unit);
	x = 120 - w / 2;
	gfx_triangle(x, 100 + 6, x + 14, 100 + 6, x + 7, 100 + 20, C_DOWN);
	gfx_text(FONT_REGULAR, 34, x + 20, TOP(100, 34), ALIGN_LEFT, C_DOWN, num);
	gfx_text(FONT_REGULAR, 11, x + 20 + gfx_text_width(FONT_REGULAR, 34, num) + 5, TOP(117, 11), ALIGN_LEFT, C_DOWN,
		 unit);
	rate(S.tx_bps, num, unit);
	w = 20 + gfx_text_width(FONT_REGULAR, 34, num) + 5 + gfx_text_width(FONT_REGULAR, 11, unit);
	x = 120 - w / 2;
	gfx_triangle(x, 144 + 20, x + 14, 144 + 20, x + 7, 144 + 6, C_UP);
	gfx_text(FONT_REGULAR, 34, x + 20, TOP(144, 34), ALIGN_LEFT, C_UP, num);
	gfx_text(FONT_REGULAR, 11, x + 20 + gfx_text_width(FONT_REGULAR, 34, num) + 5, TOP(161, 11), ALIGN_LEFT, C_UP,
		 unit);

	/* battery row */
	snprintf(b, sizeof(b), "%d%%", S.bat_pct);
	{
		const char *st = S.charging ? "şarj" : S.usb_online ? "USB" : "pil";
		float rw = 25 + 5 + gfx_text_width(FONT_REGULAR, 12, b) + 8 + gfx_text_width(FONT_REGULAR, 10, st);

		x = 120 - rw / 2;
		gfx_round_rect_stroke(x, 186, 22, 11, 3, 1, C_DIM);
		gfx_fill_rect((int)x + 23, 186 + 3, 2, 5, C_DIM);
		gfx_round_rect(x + 2, 188, 18 * (S.bat_pct > 0 ? S.bat_pct : 0) / 100.0f, 7, 1.5f, bat_colour(S.bat_pct));
		x += 30;
		x += gfx_text(FONT_REGULAR, 12, x, TOP(185, 12), ALIGN_LEFT, C_FG, b) + 8;
		gfx_text(FONT_REGULAR, 10, x, TOP(187, 10), ALIGN_LEFT, C_DIM, st);
	}
	if (S.nclients) {
		snprintf(b, sizeof(b), "%d istemci", S.nclients);
		gfx_text(FONT_REGULAR, 10, 120, TOP(206, 10), ALIGN_CENTER, C_DIM, b);
	}
}

static int wifi_qr;	/* the Wi-Fi page shows the QR code (a tap switches) */

static void draw_wifi_qr(void)
{
	static unsigned char m[QR_MAX * QR_MAX];
	char payload[256];
	int n, mod, x, y;
	float x0, y0;

	qr_wifi_payload(payload, sizeof(payload), S.ssid, S.key);
	n = qr_encode(payload, m);
	if (!n) {
		gfx_text(FONT_REGULAR, 12, 120, 124, ALIGN_CENTER, C_DIM, "Karekod sığmıyor");
		return;
	}
	/* black on white whatever the theme: some scanners do not read it the other way round (WifiPage) */
	mod = 168 / (n + 2);
	x0 = 120 - mod * n / 2.0f;
	y0 = 112 - mod * n / 2.0f;
	gfx_round_rect(x0 - mod * 1.5f, y0 - mod * 1.5f, mod * (n + 3), mod * (n + 3), 6, C_FG);
	for (y = 0; y < n; y++)
		for (x = 0; x < n; x++)
			if (m[y * n + x])
				gfx_fill_rect((int)x0 + x * mod, (int)y0 + y * mod, mod, mod, C_BG);
	gfx_text(FONT_REGULAR, 9, 120, TOP(206, 9), ALIGN_CENTER, C_DIM2, "Dokun: geri");
}

static void draw_wifi(void)
{
	char b[48];

	if (wifi_qr) {
		draw_wifi_qr();
		return;
	}

	title("WIFI", 14, 40);
	gfx_text(FONT_REGULAR, 9, 120, TOP(34, 9), ALIGN_CENTER, C_DIM2, "Ağ");
	text_fit(FONT_REGULAR, 15, 120, TOP(45, 15), ALIGN_CENTER, S.wifi_on ? C_FG : C_DIM, S.ssid, 170);
	gfx_text(FONT_REGULAR, 9, 120, TOP(66, 9), ALIGN_CENTER, C_DIM2, "Parola");
	text_fit(FONT_REGULAR, 14, 120, TOP(77, 14), ALIGN_CENTER, C_DOWN, S.key, 180);
	gfx_fill_rect(80, 98, 80, 1, C_HAIRLINE);
	snprintf(b, sizeof(b), S.wifi_on ? "Bağlı %d" : "Hotspot kapalı", S.nclients);
	gfx_text(FONT_REGULAR, 9, 120, TOP(102, 9), ALIGN_CENTER, C_DIM2, b);
	{
		int i;

		for (i = 0; i < S.nclients && i < 3; i++) {
			float y = 118 + i * 30;

			gfx_text(FONT_REGULAR, 11, 120, TOP(y + 1, 11), ALIGN_CENTER, C_FG, S.clients[i].ip);
			text_fit(FONT_REGULAR, 8, 120, TOP(y + 14, 8), ALIGN_CENTER, C_DIM,
				 S.clients[i].name[0] ? S.clients[i].name : S.clients[i].mac, 150);
		}
	}
	gfx_text(FONT_REGULAR, 10, 120, TOP(200, 10), ALIGN_CENTER, C_DIM2, "5 GHz  •  Wi-Fi 6");
	gfx_text(FONT_REGULAR, 9, 120, TOP(214, 9), ALIGN_CENTER, C_DIM2, "Dokun: karekod");
}

static void draw_clients(void)
{
	int i;
	char b[32];

	title("İSTEMCİLER", 16, 60);
	if (!S.nclients) {
		gfx_text(FONT_REGULAR, 12, 120, TOP(112, 12), ALIGN_CENTER, C_DIM, "Bağlı istemci yok");
		return;
	}
	for (i = scroll; i < S.nclients && i < scroll + 5; i++) {
		float y = 38 + (i - scroll) * 34;

		text_fit(FONT_REGULAR, 12, 120, TOP(y, 12), ALIGN_CENTER, C_FG,
			 S.clients[i].name[0] ? S.clients[i].name : S.clients[i].ip, 160);
		snprintf(b, sizeof(b), "%s  %s", S.clients[i].ip, S.clients[i].mac);
		text_fit(FONT_REGULAR, 8, 120, TOP(y + 15, 8), ALIGN_CENTER, C_DIM, b, 170);
	}
}

static int sms_open = -1;	/* the message shown in full, -1 the list */

/* text wrapped at spaces into lines of a width, inside the round screen; returns the y after it */
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
		snprintf(line, sizeof(line), "%.*s", n, t);
		gfx_text(FONT_REGULAR, px, 120, y, ALIGN_CENTER, c, line);
		t += n;
		while (*t == ' ')
			t++;
		y += lh;
	}
	return y;
}

static void draw_sms_one(const struct sms *m)
{
	text_fit(FONT_MEDIUM, 13, 120, TOP(30, 13), ALIGN_CENTER, C_FG, m->sender, 150);
	gfx_text(FONT_REGULAR, 9, 120, TOP(48, 9), ALIGN_CENTER, C_DIM2, m->time);
	gfx_fill_rect(70, 62, 100, 1, C_HAIRLINE);
	text_wrap(11, TOP(70, 11), C_FG, m->text, 176, 15, 210);
}

static void draw_sms(void)
{
	int i;

	if (sms_open >= 0 && sms_open < S.nsms) {
		draw_sms_one(&S.sms[sms_open]);
		return;
	}

	title("MESAJLAR", 16, 60);
	if (!S.nsms) {
		gfx_text(FONT_REGULAR, 12, 120, TOP(112, 12), ALIGN_CENTER, C_DIM, S.sim ? "Mesaj yok" : "SIM yok");
		return;
	}
	for (i = scroll; i < S.nsms && i < scroll + 4; i++) {
		float y = 36 + (i - scroll) * 44;
		const char *t = S.sms[i].text;
		char l1[64];
		int n = 0, w = 0;

		text_fit(FONT_REGULAR, 11, 44, TOP(y, 11), ALIGN_LEFT, C_FG, S.sms[i].sender, 100);
		gfx_text(FONT_REGULAR, 9, 196, TOP(y + 1, 9), ALIGN_RIGHT, C_DIM2, S.sms[i].time + (strlen(S.sms[i].time) > 9 ?
			 strlen(S.sms[i].time) - 8 : 0));
		/* two lines of preview: the first as much as fits, the rest after it */
		while (t[n] && w < 150) {
			n++;
			while ((t[n] & 0xc0) == 0x80)
				n++;
			snprintf(l1, sizeof(l1), "%.*s", n, t);
			w = gfx_text_width(FONT_REGULAR, 9.5f, l1);
		}
		snprintf(l1, sizeof(l1), "%.*s", n, t);
		gfx_text(FONT_REGULAR, 9.5f, 44, TOP(y + 15, 9.5f), ALIGN_LEFT, C_DIM, l1);
		text_fit(FONT_REGULAR, 9.5f, 44, TOP(y + 26, 9.5f), ALIGN_LEFT, C_DIM, t + n, 150);
		gfx_fill_rect(44, (int)y + 40, 152, 1, C_HAIRLINE);
	}
}

static void draw_battery(void)
{
	char b[48];

	gfx_arc(120, 120, 115, 10, 0, 360, C_TRACK);
	if (S.bat_pct >= 0)
		gfx_arc(120, 120, 115, 10, 0, 3.6f * S.bat_pct, bat_colour(S.bat_pct));
	snprintf(b, sizeof(b), "%d%%", S.bat_pct);
	gfx_text(FONT_REGULAR, 40, 120, TOP(46, 40), ALIGN_CENTER, 0x34c759, b);
	gfx_text(FONT_REGULAR, 12, 120, TOP(92, 12), ALIGN_CENTER, C_DIM,
		 S.charging ? "Şarj oluyor" : S.usb_online ? (S.bat_pct >= 100 ? "Dolu, USB'den çalışıyor" : "USB bağlı") :
		 "Pilden çalışıyor");
	snprintf(b, sizeof(b), "%+d mA  •  %.2f V", S.bat_ma, S.bat_mv / 1000.0);
	gfx_text(FONT_REGULAR, 13, 120, TOP(124, 13), ALIGN_CENTER, C_FG, b);
	snprintf(b, sizeof(b), "Pil %.1f °C", S.bat_temp10 / 10.0);
	gfx_text(FONT_REGULAR, 11, 120, TOP(150, 11), ALIGN_CENTER, C_DIM, b);
}

static void draw_signal(void)
{
	char b[64];
	static const char *regs[] = {"Kayıtsız", "Kayıtlı", "Aranıyor", "Reddedildi", "Bilinmiyor", "Dolaşımda"};

	title("SİNYAL", 16, 60);
	gfx_text(FONT_REGULAR, 17, 120, TOP(40, 17), ALIGN_CENTER, C_FG, !S.sim ? "SIM yok" : S.oper[0] ? S.oper : "…");
	signal_bars(120 - 16, 66, 1, S.bars);
	snprintf(b, sizeof(b), "%s  •  %s", S.net[0] ? S.net : "-", S.reg >= 0 && S.reg <= 5 ? regs[S.reg] : "-");
	gfx_text(FONT_REGULAR, 11, 120, TOP(90, 11), ALIGN_CENTER, C_DIM, b);
	snprintf(b, sizeof(b), "RSRP  %d dBm", S.rsrp);
	gfx_text(FONT_REGULAR, 13, 120, TOP(112, 13), ALIGN_CENTER, C_FG, b);
	snprintf(b, sizeof(b), "RSRQ  %d dB", S.rsrq);
	gfx_text(FONT_REGULAR, 13, 120, TOP(132, 13), ALIGN_CENTER, C_FG, b);
	gfx_text(FONT_REGULAR, 9, 120, TOP(158, 9), ALIGN_CENTER, C_DIM2, S.wan_up ? "Mobil veri açık" : "Mobil veri kapalı");
	gfx_text(FONT_REGULAR, 12, 120, TOP(171, 12), ALIGN_CENTER, S.wan_up ? C_DOWN : C_DIM, S.wan_ip[0] ? S.wan_ip : "-");
	{
		char rx[24], tx[24];

		bytes(S.rx_total, rx);
		bytes(S.tx_total, tx);
		/* Roboto has no arrows: small triangles before the two numbers */
		int w1 = gfx_text_width(FONT_REGULAR, 10, rx), w2 = gfx_text_width(FONT_REGULAR, 10, tx);
		float x = 120 - (10 + w1 + 14 + 10 + w2) / 2.0f, by = TOP(192, 10);

		gfx_triangle(x, by - 7, x + 7, by - 7, x + 3.5f, by, C_DOWN);
		gfx_text(FONT_REGULAR, 10, x + 10, by, ALIGN_LEFT, C_DIM, rx);
		x += 10 + w1 + 14;
		gfx_triangle(x, by, x + 7, by, x + 3.5f, by - 7, C_UP);
		gfx_text(FONT_REGULAR, 10, x + 10, by, ALIGN_LEFT, C_DIM, tx);
	}
}

static void bar_cell(float cx, float y, const char *label, int pct, const char *value)
{
	gfx_text(FONT_REGULAR, 9, cx, TOP(y, 9), ALIGN_CENTER, C_DIM2, label);
	gfx_text(FONT_REGULAR, 14, cx, TOP(y + 12, 14), ALIGN_CENTER, C_FG, value);
	gfx_round_rect(cx - 23, y + 32, 46, 3, 1.5f, C_TRACK);
	gfx_round_rect(cx - 23, y + 32, 46 * (pct > 100 ? 100 : pct < 0 ? 0 : pct) / 100.0f, 3, 1.5f,
		       pct >= 90 ? C_ERR : pct >= 70 ? C_WARN : C_DOWN);
}

static void draw_system(void)
{
	char a[32], b[32];

	title("SİSTEM", 22, 60);
	snprintf(a, sizeof(a), "%d%%", S.cpu_pct);
	bar_cell(78, 54, "CPU", S.cpu_pct, a);
	snprintf(b, sizeof(b), "%d%%", S.mem_pct);
	bar_cell(162, 54, "RAM", S.mem_pct, b);
	snprintf(a, sizeof(a), "%ld / %ld MB boş", S.mem_free_mb, S.mem_total_mb);
	gfx_text(FONT_REGULAR, 9, 120, TOP(106, 9), ALIGN_CENTER, C_DIM2, "Bellek");
	gfx_text(FONT_REGULAR, 12, 120, TOP(118, 12), ALIGN_CENTER, C_FG, a);
	snprintf(a, sizeof(a), "%d%% dolu", S.disk_pct);
	gfx_text(FONT_REGULAR, 9, 120, TOP(142, 9), ALIGN_CENTER, C_DIM2, "Depolama");
	gfx_text(FONT_REGULAR, 12, 120, TOP(154, 12), ALIGN_CENTER, C_FG, a);
	snprintf(a, sizeof(a), "%ldsa %02ldd", S.uptime_s / 3600, S.uptime_s / 60 % 60);
	snprintf(b, sizeof(b), "%s °C", S.temp_soc[0] ? S.temp_soc : "-");
	gfx_text(FONT_REGULAR, 9, 90, TOP(180, 9), ALIGN_CENTER, C_DIM2, "Çalışma");
	gfx_text(FONT_REGULAR, 12, 90, TOP(192, 12), ALIGN_CENTER, C_FG, a);
	gfx_text(FONT_REGULAR, 9, 150, TOP(180, 9), ALIGN_CENTER, C_DIM2, "SoC");
	gfx_text(FONT_REGULAR, 12, 150, TOP(192, 12), ALIGN_CENTER, C_FG, b);
}

static void draw_log(void)
{
	int i, rows = 17, first;

	first = S.nlog - rows - scroll;
	if (first < 0)
		first = 0;
	for (i = 0; i < rows && first + i < S.nlog; i++) {
		float y = 26 + i * 11;
		/* inside the round screen: the row's half chord, less a margin */
		float dy = y + 4 - 120, half = sqrtf(fmaxf(0, 118 * 118 - dy * dy)) - 6;

		text_fit(FONT_REGULAR, 9, 120 - half, y + 8, ALIGN_LEFT, C_DIM, S.log[first + i], (int)(2 * half));
	}
}

/* settings: rows of label and value; a tap changes the value */
static const char *settings_labels[] = {"Ekran", "Kilit", "Hotspot", "Mobil veri", "Arayüz"};
#define NSET 5

static void setting_value(int i, char *v)
{
	switch (i) {
	case 0:
		if (timeout_s)
			sprintf(v, timeout_s < 60 ? "%d sn" : "%d dk", timeout_s < 60 ? timeout_s : timeout_s / 60);
		else
			strcpy(v, "Hep açık");
		break;
	case 1:
		strcpy(v, lock_mode_hold ? "Basılı tut" : "Kapalı");
		break;
	case 2:
		strcpy(v, S.wifi_on ? "Açık" : "Kapalı");
		break;
	case 3:
		strcpy(v, S.wan_up ? "Açık" : "Kapalı");
		break;
	case 4:
		strcpy(v, "Klasik");
		break;
	}
}

static void draw_settings(void)
{
	int i;
	char v[32];

	title("AYARLAR", 16, 60);
	for (i = 0; i < NSET; i++) {
		float y = 38 + i * 34;

		gfx_text(FONT_REGULAR, 12, 44, TOP(y + 6, 12), ALIGN_LEFT, C_FG, settings_labels[i]);
		setting_value(i, v);
		gfx_text(FONT_REGULAR, 11, 196, TOP(y + 7, 11), ALIGN_RIGHT, C_DIM, v);
		gfx_fill_rect(44, (int)y + 28, 152, 1, C_HAIRLINE);
	}
	gfx_text(FONT_REGULAR, 9, 120, TOP(212, 9), ALIGN_CENTER, C_DIM2, "u30ui • Linux");
}

static void settings_tap(int y)
{
	int i = (y - 38) / 34;

	if (y < 38 || i >= NSET)
		return;
	switch (i) {
	case 0:
		timeout_s = timeout_s == 30 ? 60 : timeout_s == 60 ? 300 : timeout_s == 300 ? 0 : 30;
		conf_save();
		break;
	case 1:
		lock_mode_hold = !lock_mode_hold;
		conf_save();
		break;
	case 2:
		data_run(S.wifi_on ? "uci set wireless.ap0.disabled=1; uci commit wireless; wifi reload"
				   : "uci set wireless.ap0.disabled=0; uci commit wireless; wifi reload");
		break;
	case 3:
		data_run(S.wan_up ? "ifdown wan" : "ifup wan");
		break;
	case 4:
		/* the dashboard (tools/u30ui2): mu5358-screen starts it when /etc/mu5358/ui is not "classic" */
		data_run("rm -f /etc/mu5358/ui; /etc/init.d/mu5358-screen restart");
		break;
	}
}

/* actions: the first two want a 1 s hold */
static const struct action {
	const char *label;
	uint32_t colour;
	int hold;
} actions[] = {
	{"Yeniden başlat", C_ERR, 1},
	{"Kapat", C_ERR, 1},
	{"Veri kes / aç", C_WARN, 1},
	{"Ekranı kapat", C_UP, 0},
};
static const int action_y[] = {56, 96, 136, 176};

static void draw_actions(long now)
{
	int i;

	gfx_text(FONT_REGULAR, 10, 120, TOP(20, 10), ALIGN_CENTER, C_DIM2, "AKSİYONLAR");
	for (i = 0; i < 4; i++) {
		gfx_arc(52, action_y[i] + 2, 8, 2, 0, 360, actions[i].colour);
		gfx_text(FONT_REGULAR, 14, 72, TOP(action_y[i] - 5, 14), ALIGN_LEFT, C_FG,
			 i == 2 ? (S.wan_up ? "Veriyi kes" : "Veriyi aç") : actions[i].label);
	}
	gfx_text(FONT_REGULAR, 10, 120, TOP(204, 10), ALIGN_CENTER, C_DIM2, "1 sn basılı tut");
	if (hold_row >= 0) {
		float f = (now - hold_t0) / (float)HOLD_MS;

		gfx_arc(120, 120, 114.5f, 3, 0, 360 * (f > 1 ? 1 : f), actions[hold_row].colour);
	}
}

static void action_run(int i)
{
	switch (i) {
	case 0:
		data_run("sync; reboot");
		break;
	case 1:
		data_run("sync; poweroff");
		break;
	case 2:
		data_run(S.wan_up ? "ifdown wan" : "ifup wan");
		break;
	case 3:
		display_backlight(0);
		break;
	}
}

static int action_at(int y)
{
	int i;

	for (i = 0; i < 4; i++)
		if (y >= action_y[i] - 17 && y <= action_y[i] + 21)
			return i;
	return -1;
}

static void draw(long now)
{
	gfx_clear(C_BG);
	switch (page) {
	case P_HOME: draw_home(now); break;
	case P_STATUS: draw_status(); break;
	case P_WIFI: draw_wifi(); break;
	case P_SMS: draw_sms(); break;
	case P_BATTERY: draw_battery(); break;
	case P_SETTINGS: draw_settings(); break;
	case P_CLIENTS: draw_clients(); break;
	case P_SIGNAL: draw_signal(); break;
	case P_ACTIONS: draw_actions(now); break;
	case P_SYSTEM: draw_system(); break;
	case P_LOG: draw_log(); break;
	}
	if (page != P_HOME && locked) {
		icon_padlock(120, 227, now < flash_until ? 13 : 10, now < flash_until ? C_FG : C_DIM2);
		if (now < hint_until)
			gfx_text(FONT_REGULAR, 10, 120, 214, ALIGN_CENTER, C_FG, "Basılı tut");
	}
}

/* --- touches */
static int petal_at(int x, int y)
{
	int s, dx = x - 120, dy = y - 120;

	if (dx * dx + dy * dy <= 42 * 42)
		return 99;
	for (s = 0; s < 4; s++) {
		int ex = x - (120 + slot_dx[s]), ey = y - (120 + slot_dy[s]), i = home_page * 4 + s;

		if (ex * ex + ey * ey <= 38 * 38 && i < NPETALS)
			return i;
	}
	return -1;
}

static void go_home(void)
{
	wifi_qr = 0;
	sms_open = -1;
	page = P_HOME;
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
	if (page == P_HOME) {
		switch (g->type) {
		case G_DOWN:
			pressed = petal_at(g->x, g->y);
			break;
		case G_MOVE:
			if (abs(g->dx) > 8 && abs(g->dx) >= abs(g->dy)) {
				dragging = 1;
				pressed = -1;
				drag_dx = g->dx;
				if ((drag_dx > 0 && home_page == 0) || (drag_dx < 0 && home_page == NPAGES - 1))
					drag_dx = (int)(drag_dx * 0.35f);
			}
			break;
		case G_TAP:
			if (pressed == 99)
				page = P_STATUS;
			else if (pressed >= 0 && petal_at(g->x, g->y) == pressed)
				page = petals[pressed].page;
			scroll = 0;
			pressed = -1;
			break;
		default:
			/* release: settle to a page */
			if (dragging) {
				int to = home_page;

				if (g->dx <= -24 && home_page + 1 < NPAGES)
					to = home_page + 1;
				else if (g->dx >= 24 && home_page > 0)
					to = home_page - 1;
				settle_from = drag_dx;
				settle_to_page = to;
				settle_t0 = now;
			}
			dragging = 0;
			pressed = -1;
			break;
		}
		return;
	}
	/* a detail page */
	if (page == P_SMS && sms_open >= 0 && g->type == G_SWIPE_DOWN) {
		sms_open = -1;
		return;
	}
	if (g->type == G_SWIPE_DOWN && scroll == 0) {
		go_home();
		return;
	}
	if (g->type == G_SWIPE_DOWN && scroll > 0)
		scroll--;
	if (g->type == G_SWIPE_UP)
		scroll++;
	if (page == P_LOG && g->type == G_SWIPE_DOWN)
		;
	if (page == P_SETTINGS && g->type == G_TAP)
		settings_tap(g->y);
	if (page == P_WIFI && g->type == G_TAP)
		wifi_qr = !wifi_qr;
	if (page == P_SMS && g->type == G_TAP) {
		int r = (g->y - 36) / 44 + scroll;

		if (sms_open >= 0)
			sms_open = -1;
		else if (g->y >= 36 && r < S.nsms && r < scroll + 4)
			sms_open = r;
	}
	if (page == P_ACTIONS) {
		int r = action_at(g->y);

		if (g->type == G_DOWN && r >= 0 && actions[r].hold) {
			hold_row = r;
			hold_t0 = now;
		} else if (g->type == G_TAP && r >= 0 && !actions[r].hold) {
			action_run(r);
		} else if (g->type == G_UP || g->type == G_TAP || (g->type == G_MOVE && action_at(g->y) != hold_row)) {
			hold_row = -1;
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
	gfx_init();
	if (display_init()) {
		fprintf(stderr, "u30ui: no display\n");
		return 1;
	}
	pfd.fd = input_open();
	pfd.events = POLLIN;
	data_start();
	display_backlight(1);
	last_touch = now_ms();

	for (;;) {
		struct gesture g;
		int timeout = 1000, t;
		time_t wall = time(NULL);
		struct tm tm;

		now = now_ms();
		/* the screen: the power key switches it, the timeout switches it off */
		if (display_is_on() != on) {
			on = !on;
			data_screen_on = on;
			if (on) {
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
			home_page = 0;
			redraw = 1;
		}

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

		/* test gestures: "tap X Y", "long X Y", "swipe left|right|up|down", "screen on" - one a line */
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
				} else if (!strncmp(line, "screen on", 9)) {
					display_backlight(1);
				}
			}
			if (cf)
				fclose(cf);
			redraw = 1;
		}
		/* the screenshot: drawn now, also while the backlight is off */
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
