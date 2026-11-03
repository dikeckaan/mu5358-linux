/* sample data for the preview (preview.c) and the simulator (sim.c): a board on LTE with two clients and three
 * messages. SPDX-License-Identifier: MIT */
#ifndef SAMPLE_H
#define SAMPLE_H
#include <string.h>
#include "data.h"

static void sample_data(struct data *s)
{
	int i;

	memset(s, 0, sizeof(*s));
	s->bat_pct = 87; s->bat_ma = -412; s->bat_mv = 3924; s->bat_temp10 = 272; s->usb_online = 0;
	strcpy(s->bat_health, "Good");
	s->sim = 1; s->rsrp = -101; s->rsrq = -11; s->bars = 3; s->act = 7; s->reg = 1;
	s->band = 3; s->earfcn = 1279; s->pci = 302;
	strcpy(s->oper, "Türk Telekom"); strcpy(s->net, "LTE"); strcpy(s->cell, "05405B2B"); strcpy(s->tac, "523E");
	strcpy(s->apn, "internet");
	s->wan_up = 1; s->data_on = 1; strcpy(s->wan_ip, "10.31.182.143");
	s->rx_bps = 2.4e6; s->tx_bps = 310e3;
	s->rx_total = 1234567890ULL; s->tx_total = 123456789ULL;
	s->today_rx = 1288490188ULL; s->today_tx = 150000000ULL; s->month_rx = 12884901888ULL; s->month_tx = 1500000000ULL;
	for (i = 0; i < HIST; i++) {
		s->rx_hist[i] = 1.2e6f + 1.1e6f * (float)(0.5 + 0.5 * __builtin_sin(i / 4.0)) + (i % 7) * 1.5e5f;
		s->tx_hist[i] = 2e5f + 1.5e5f * (float)(0.5 + 0.5 * __builtin_cos(i / 6.0));
	}
	for (i = 0; i < SIGHIST; i++)
		s->sig_hist[i] = (signed char)(-101 + (int)(6 * __builtin_sin(i / 5.0)));
	s->wifi_on = 1; s->channel = 36; s->width_mhz = 80; s->band_ghz = 5;
	strcpy(s->ssid, "U30Pro-5AB8"); strcpy(s->key, "example-pass1"); strcpy(s->wifi_mac, "02:00:00:00:00:01");
	s->nclients = 2; s->nfc = 1;
	strcpy(s->clients[0].mac, "02:00:00:00:00:11"); strcpy(s->clients[0].ip, "192.168.58.200");
	strcpy(s->clients[0].name, "MacBook"); s->clients[0].rx = 50000000; s->clients[0].tx = 820000000; s->clients[0].signal = -52;
	strcpy(s->clients[1].mac, "02:00:00:00:00:12"); strcpy(s->clients[1].ip, "192.168.58.201");
	s->clients[1].rx = 2000000; s->clients[1].tx = 30000000; s->clients[1].signal = -71;
	s->nsms = 3;
	strcpy(s->sms[0].sender, "Türk Telekom"); strcpy(s->sms[0].time, "2026-10-30 09:12:03");
	strcpy(s->sms[0].text, "Internet paketinizin %80'ini kullandiniz. Kalan: 4 GB.");
	strcpy(s->sms[1].sender, "+905551112233"); strcpy(s->sms[1].time, "2026-10-29 21:40:11");
	strcpy(s->sms[1].text, "Selam, bu aksam musait misin?");
	strcpy(s->sms[2].sender, "4444"); strcpy(s->sms[2].time, "2026-10-28 08:00:00");
	strcpy(s->sms[2].text, "Faturaniz hazirlandi.");
	s->cpu_pct = 12; s->ncpu = 8;
	for (i = 0; i < 8; i++) { s->core_pct[i] = (i * 13 + 5) % 60; s->freq_mhz[i] = i < 6 ? 884 : 2041; }
	s->mem_pct = 41; s->mem_total_mb = 1870; s->mem_free_mb = 1103; s->disk_pct = 1; s->disk_free_mb = 25719;
	s->uptime_s = 2 * 3600 + 14 * 60; s->load1 = 0.31; s->load5 = 0.28;
	s->temp_soc = 36; s->temp_wcn = 59; s->temp_pa = 26; s->temp_bat = 27;
	strcpy(s->kernel, "6.6.98-android15-8-mu5358-4k"); strcpy(s->version, "v2026.10.30.4");
	s->nlog = 6;
	strcpy(s->log[0], "09:11:58 netifd: wan (1234): udhcpc: lease of 10.31.182.143 obtained");
	strcpy(s->log[1], "09:12:01 hostapd: wlan0: AP-STA-CONNECTED 02:00:00:00:00:11");
	strcpy(s->log[2], "09:12:03 dnsmasq-dhcp[2011]: DHCPACK(br-lan) 192.168.58.200 MacBook");
	strcpy(s->log[3], "09:12:40 mu5358-usb: gadget configured on musb-hdrc.1.auto");
	strcpy(s->log[4], "09:13:02 mobile-data: bearer up, 286-03 LTE");
	strcpy(s->log[5], "09:14:10 u30ui2: started");
}
#endif
