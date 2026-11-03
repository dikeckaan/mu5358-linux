/*
 * preview: every u30ui2 page with sample data, in the three languages, as PNG files (host build, no device):
 * build.sh preview. The round screen's outside is drawn grey so the corners that do not show stand out.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "data.h"
#include "sample.h"
#include "../u30ui/png.h"

/* stubs for what the device would do */
struct data D;
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int data_screen_on = 1;
volatile int data_want_fast;
void data_start(void) {}
void data_run(const char *c) { (void)c; }
void data_poke(void) {}
int display_init(void (*first)(void)) { if (first) first(); return 0; }
void display_present(void) {}
void display_backlight(int on) { (void)on; }
int display_is_on(void) { return 1; }
long now_ms(void) { return 1000000; }
int input_open(void) { return -1; }
int input_ms_to_long(void) { return -1; }
int input_is_down(void) { return 0; }
struct gesture;
int input_read(struct gesture *g) { (void)g; return 0; }
int input_poll_long(struct gesture *g) { (void)g; return 0; }

#define main u30ui_main
#include "ui.c"
#undef main

static void shot(int p, const char *name)
{
	char f[64];

	page = p;
	draw(now_ms());
	snprintf(f, sizeof(f), "%s-%s.png", name, lang == L_TR ? "tr" : lang == L_ZH ? "zh" : "en");
	png_write(f);
}

int main(void)
{
	int l;

	gfx_init();
	setenv("TZ", "Europe/Istanbul", 1);
	sample_data(&S);
	locked = 0;
	for (l = 0; l < NLANG; l++) {
		lang = l;
		shot(P_HOME, "home");
		shot(P_NET, "network");
		shot(P_WIFI, "wifi");
		wifi_qr = 1; shot(P_WIFI, "wifi-qr"); wifi_qr = 0;
		shot(P_CLIENTS, "clients");
		shot(P_TRAFFIC, "traffic");
		shot(P_SMS, "sms");
		sms_open = 0; shot(P_SMS, "sms-open"); sms_open = -1;
		shot(P_BATTERY, "battery");
		shot(P_SYSTEM, "system");
		shot(P_SETTINGS, "settings");
		shot(P_POWER, "power");
		shot(P_LOG, "log");
	}
	lang = L_EN;
	locked = 1;
	hint_until = now_ms() + 1;
	shot(P_HOME, "home-locked");
	return 0;
}
