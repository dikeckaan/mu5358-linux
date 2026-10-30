/*
 * preview: every u30ui2 page with sample data, in the three languages, as PNG files (host build, no device):
 * build.sh preview. The round screen's outside is drawn grey so the corners that do not show stand out.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "data.h"
#include "../u30ui/png.h"

/* stubs for what the device would do */
struct data D;
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int data_screen_on = 1;
volatile int data_want_fast;
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
	char f[64];

	page = p;
	draw(now_ms());
	snprintf(f, sizeof(f), "%s-%s.png", name, lang == L_TR ? "tr" : lang == L_ZH ? "zh" : "en");
	png_write(f);
}

int main(void)
{
	int i, l;

	gfx_init();
	setenv("TZ", "Europe/Istanbul", 1);
	memset(&S, 0, sizeof(S));
	S.bat_pct = 87; S.bat_ma = -412; S.bat_mv = 3924; S.bat_temp10 = 272; S.usb_online = 0;
	strcpy(S.bat_health, "Good");
	S.sim = 1; S.rsrp = -101; S.rsrq = -11; S.bars = 3; S.act = 7; S.reg = 1;
	S.band = 3; S.earfcn = 1279; S.pci = 302;
	strcpy(S.oper, "Türk Telekom"); strcpy(S.net, "LTE"); strcpy(S.cell, "05405B2B"); strcpy(S.tac, "523E");
	strcpy(S.apn, "internet");
	S.wan_up = 1; S.data_on = 1; strcpy(S.wan_ip, "10.31.182.143");
	S.rx_bps = 2.4e6; S.tx_bps = 310e3;
	S.rx_total = 1234567890ULL; S.tx_total = 123456789ULL;
	S.today_rx = 1288490188ULL; S.today_tx = 150000000ULL; S.month_rx = 12884901888ULL; S.month_tx = 1500000000ULL;
	for (i = 0; i < HIST; i++) {
		S.rx_hist[i] = 1.2e6f + 1.1e6f * (float)(0.5 + 0.5 * __builtin_sin(i / 4.0)) + (i % 7) * 1.5e5f;
		S.tx_hist[i] = 2e5f + 1.5e5f * (float)(0.5 + 0.5 * __builtin_cos(i / 6.0));
	}
	for (i = 0; i < SIGHIST; i++)
		S.sig_hist[i] = (signed char)(-101 + (int)(6 * __builtin_sin(i / 5.0)));
	S.wifi_on = 1; S.channel = 36; S.width_mhz = 80; S.band_ghz = 5;
	strcpy(S.ssid, "U30Pro-5AB8"); strcpy(S.key, "example-pass1"); strcpy(S.wifi_mac, "02:00:00:00:00:01");
	S.nclients = 2;
	strcpy(S.clients[0].mac, "02:00:00:00:00:11"); strcpy(S.clients[0].ip, "192.168.58.200");
	strcpy(S.clients[0].name, "MacBook"); S.clients[0].rx = 50000000; S.clients[0].tx = 820000000; S.clients[0].signal = -52;
	strcpy(S.clients[1].mac, "02:00:00:00:00:12"); strcpy(S.clients[1].ip, "192.168.58.201");
	S.clients[1].rx = 2000000; S.clients[1].tx = 30000000; S.clients[1].signal = -71;
	S.nsms = 3;
	strcpy(S.sms[0].sender, "Türk Telekom"); strcpy(S.sms[0].time, "2026-10-30 09:12:03");
	strcpy(S.sms[0].text, "Internet paketinizin %80'ini kullandiniz. Kalan: 4 GB.");
	strcpy(S.sms[1].sender, "+905551112233"); strcpy(S.sms[1].time, "2026-10-29 21:40:11");
	strcpy(S.sms[1].text, "Selam, bu aksam musait misin?");
	strcpy(S.sms[2].sender, "4444"); strcpy(S.sms[2].time, "2026-10-28 08:00:00");
	strcpy(S.sms[2].text, "Faturaniz hazirlandi.");
	S.cpu_pct = 12; S.ncpu = 8;
	for (i = 0; i < 8; i++) { S.core_pct[i] = (i * 13 + 5) % 60; S.freq_mhz[i] = i < 6 ? 884 : 2041; }
	S.mem_pct = 41; S.mem_total_mb = 1870; S.mem_free_mb = 1103; S.disk_pct = 1; S.disk_free_mb = 25719;
	S.uptime_s = 2 * 3600 + 14 * 60; S.load1 = 0.31; S.load5 = 0.28;
	S.temp_soc = 36; S.temp_wcn = 59; S.temp_pa = 26; S.temp_bat = 27;
	strcpy(S.kernel, "6.6.98-android15-8-mu5358-4k"); strcpy(S.version, "v2026.10.30.4");
	S.nlog = 6;
	strcpy(S.log[0], "09:11:58 netifd: wan (1234): udhcpc: lease of 10.31.182.143 obtained");
	strcpy(S.log[1], "09:12:01 hostapd: wlan0: AP-STA-CONNECTED 02:00:00:00:00:11");
	strcpy(S.log[2], "09:12:03 dnsmasq-dhcp[2011]: DHCPACK(br-lan) 192.168.58.200 MacBook");
	strcpy(S.log[3], "09:12:40 mu5358-usb: gadget configured on musb-hdrc.1.auto");
	strcpy(S.log[4], "09:13:02 mobile-data: bearer up, 286-03 LTE");
	strcpy(S.log[5], "09:14:10 u30ui2: started");
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
