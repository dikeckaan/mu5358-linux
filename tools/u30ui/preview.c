/*
 * preview: every u30ui page with sample data as PNG files (host build, no device): build.sh preview.
 * The round screen's outside is drawn grey so the corners that do not show stand out.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "data.h"
#include "png.h"

/* stubs for what the device would do */
struct data D;
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int data_screen_on = 1;
void data_start(void) {}
void data_run(const char *c) { (void)c; }
void data_poke(void) {}
int display_init(void) { return 0; }
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
	page = p;
	draw(now_ms());
	png_write(name);
}

int main(void)
{
	static const char *lines[] = {
		"00:51:07 mu5358-usb: gadget configured (carrier 0)", "00:51:09 netifd: Interface 'wan' is now up",
		"00:51:12 kernel: [drm] sprd_swdispc_enable()", "00:52:00 dnsmasq-dhcp: DHCPACK(br-lan) 192.168.58.229",
	};
	int i;

	setenv("TZ", "<+03>-3", 1);
	tzset();
	gfx_init();
	memset(&S, 0, sizeof(S));
	S.bat_pct = 87; S.bat_ma = -6; S.bat_mv = 4372; S.bat_temp10 = 295; S.usb_online = 1;
	S.sim = 1; S.rsrp = -101; S.rsrq = -10; S.bars = 3; S.act = 7; S.reg = 1;
	strcpy(S.oper, "Türk Telekom"); strcpy(S.net, "LTE");
	S.wan_up = 1; strcpy(S.wan_ip, "10.33.126.40");
	S.rx_bps = 2.4e6 / 8 * 1.0; S.tx_bps = 310e3 / 8; S.rx_total = 21ULL << 20; S.tx_total = 3ULL << 20;
	S.wifi_on = 1; strcpy(S.ssid, "U30Pro-A1B2"); strcpy(S.key, "example-pass1");
	S.nclients = 2;
	strcpy(S.clients[0].ip, "192.168.58.229"); strcpy(S.clients[0].name, "iPhone");
	strcpy(S.clients[0].mac, "02:00:00:00:00:01");
	strcpy(S.clients[1].ip, "192.168.58.113"); strcpy(S.clients[1].name, "MacBook");
	strcpy(S.clients[1].mac, "02:35:58:00:00:02");
	S.nsms = 2;
	strcpy(S.sms[0].sender, "TURK TELEKOM"); strcpy(S.sms[0].time, "25/10/30 00:12:01");
	strcpy(S.sms[0].text, "Sayın abonemiz, kalan internet paketiniz 12,4 GB'tır. İyi günler dileriz.");
	strcpy(S.sms[1].sender, "+905551112233"); strcpy(S.sms[1].time, "25/10/29 21:40:55");
	strcpy(S.sms[1].text, "Akşam geliyor musun?");
	S.cpu_pct = 7; S.mem_pct = 22; S.mem_free_mb = 1120; S.mem_total_mb = 1433; S.disk_pct = 1;
	S.uptime_s = 3725; strcpy(S.temp_soc, "31");
	for (i = 0; i < 40; i++)
		snprintf(S.log[S.nlog++], sizeof(S.log[0]), "%s", lines[i % 4]);

	locked = 1;
	shot(P_HOME, "home-locked.png");
	locked = 0;
	shot(P_HOME, "home.png");
	home_page = 1;
	shot(P_HOME, "home-2.png");
	home_page = 0;
	shot(P_STATUS, "status.png");
	shot(P_WIFI, "wifi.png");
	wifi_qr = 1;
	shot(P_WIFI, "wifi-qr.png");
	wifi_qr = 0;
	shot(P_SMS, "sms.png");
	sms_open = 0;
	shot(P_SMS, "sms-open.png");
	sms_open = -1;
	shot(P_BATTERY, "battery.png");
	shot(P_SETTINGS, "settings.png");
	shot(P_CLIENTS, "clients.png");
	shot(P_SIGNAL, "signal.png");
	shot(P_ACTIONS, "actions.png");
	shot(P_SYSTEM, "system.png");
	shot(P_LOG, "log.png");
	return 0;
}
