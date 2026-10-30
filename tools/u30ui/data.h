/* data: what the pages show, collected in a thread of its own. SPDX-License-Identifier: MIT */
#ifndef DATA_H
#define DATA_H
#include <pthread.h>

#define MAXCLIENTS 16
#define MAXSMS 12
#define LOGLINES 64

struct client {
	char mac[18], ip[16], name[32];
};

struct sms {
	char index[16], sender[32], time[24], text[200];
};

struct data {
	/* battery */
	int bat_pct, bat_ma, bat_mv, bat_temp10, charging, usb_online;
	/* modem */
	int sim, rsrp, rsrq, bars, act, reg;
	char oper[32], net[8];
	/* wan and traffic */
	int wan_up;
	char wan_ip[20];
	unsigned long long rx, tx;
	double rx_bps, tx_bps;
	unsigned long long rx_total, tx_total;	/* this boot */
	/* Wi-Fi */
	int wifi_on, nclients;
	char ssid[33], key[65];
	struct client clients[MAXCLIENTS];
	/* sms */
	int nsms;
	struct sms sms[MAXSMS];
	/* system */
	int cpu_pct, mem_pct, disk_pct;
	long mem_free_mb, mem_total_mb, uptime_s;
	double load1;
	char temp_soc[8];
	/* the log, newest last */
	int nlog;
	char log[LOGLINES][96];
	unsigned serial;	/* changes whenever anything above but the log does */
	unsigned logserial;	/* changes with the log */
};

extern struct data D;
extern pthread_mutex_t data_lock;

/* the screen is on: collect every 2 s, else every 30 s (nobody looks) */
extern volatile int data_screen_on;

void data_start(void);
/* run a command in the background (an action), its output to the log */
void data_run(const char *cmd);
/* once at once: a collection without waiting for the period (after an action) */
void data_poke(void);
#endif
