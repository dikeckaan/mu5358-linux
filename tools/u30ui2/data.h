/*
 * data: what the dashboard shows, collected in threads of its own (the screen never waits for the modem), with
 * short histories for the graphs. SPDX-License-Identifier: MIT
 */
#ifndef DATA_H
#define DATA_H
#include <pthread.h>

#define MAXCLIENTS 16
#define MAXSMS 16
#define LOGLINES 64
#define NCPU 8
#define HIST 60		/* throughput: one sample a second, the last minute */
#define SIGHIST 60	/* signal: one sample every 10 s, the last 10 minutes */

struct client {
	char mac[18], ip[16], name[32];
	unsigned long long rx, tx;	/* bytes the station sent us / got (iw station dump) */
	int signal;			/* dBm, 0 unknown */
};

struct sms {
	char index[16], sender[32], time[24], text[200];
};

struct data {
	/* battery (power_supply: battery, usb) */
	int bat_pct, bat_ma, bat_mv, bat_temp10, charging, usb_online, bat_full;
	char bat_health[16];
	/* modem */
	int sim, rsrp, rsrq, sinr, bars, act, reg, roaming;
	int band, earfcn, pci;
	char oper[32], net[8], cell[12], tac[8], apn[40];
	/* wan and traffic */
	int wan_up, data_on;
	char wan_ip[20];
	unsigned long long rx, tx;		/* interface counters, bytes */
	double rx_bps, tx_bps;			/* bits a second, the last sample */
	unsigned long long rx_total, tx_total;	/* this boot */
	unsigned long long today_rx, today_tx, month_rx, month_tx;	/* mu300-traffic, when it has them */
	float rx_hist[HIST], tx_hist[HIST];	/* bits a second, newest at [HIST-1] */
	signed char sig_hist[SIGHIST];		/* rsrp, 0 = none, newest at [SIGHIST-1] */
	/* Wi-Fi */
	int wifi_on, nclients, channel, width_mhz, band_ghz;
	char ssid[33], key[65], wifi_mac[18];
	struct client clients[MAXCLIENTS];
	/* sms */
	int nsms;
	struct sms sms[MAXSMS];
	/* system */
	int cpu_pct, core_pct[NCPU], ncpu, mem_pct, disk_pct;
	int freq_mhz[NCPU];
	long mem_free_mb, mem_total_mb, disk_free_mb, uptime_s;
	double load1, load5;
	int temp_soc, temp_wcn, temp_pa, temp_bat;	/* degrees C, -1000 unknown */
	char kernel[40], version[24];
	int usb_storage;	/* /etc/mu5358/usb-storage says on: the device is a USB flash drive for the computer */
	/* the log, newest last */
	int nlog;
	char log[LOGLINES][96];
	unsigned serial;	/* changes whenever anything above but the log does */
	unsigned logserial;	/* changes with the log */
};

extern struct data D;
extern pthread_mutex_t data_lock;

/* the screen is on: collect every 2 s (the graphs: every second), else every 30 s */
extern volatile int data_screen_on;
/* a page that draws the throughput graph is up: sample every second */
extern volatile int data_want_fast;

void data_start(void);
/* run a command in the background (an action), its output to the log */
void data_run(const char *cmd);
/* once at once: a collection without waiting for the period (after an action) */
void data_poke(void);
#endif
