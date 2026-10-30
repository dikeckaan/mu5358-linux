/*
 * data: the board's state for the pages, from sysfs and procfs (every 2 s), the modem through mu300-at and the SIM's
 * messages through mu300's sms (every 15 and 30 s), and the system log through logread -f. All in threads of their
 * own: the screen never waits for the modem.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "data.h"

struct data D;
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t poke_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t poke_cond = PTHREAD_COND_INITIALIZER;
static int poked;
volatile int data_screen_on = 1;

#define AT "/opt/mu5358/bin/mu5358-at"

/* the first line of a command's output, or of a file */
static int run(const char *cmd, char *out, int n)
{
	FILE *p = popen(cmd, "r");
	int ok = 0;

	out[0] = 0;
	if (!p)
		return 0;
	if (fgets(out, n, p)) {
		out[strcspn(out, "\r\n")] = 0;
		ok = 1;
	}
	pclose(p);
	return ok;
}

static long readl(const char *path)
{
	FILE *f = fopen(path, "r");
	long v = -1;

	if (f) {
		if (fscanf(f, "%ld", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

static void reads(const char *path, char *out, int n)
{
	FILE *f = fopen(path, "r");

	out[0] = 0;
	if (f) {
		if (fgets(out, n, f))
			out[strcspn(out, "\r\n")] = 0;
		fclose(f);
	}
}

static const char *oper_name(const char *num)
{
	static const struct { const char *num, *name; } ops[] = {
		{"28601", "Turkcell"}, {"28602", "Vodafone"}, {"28603", "Türk Telekom"}, {"28604", "Türk Telekom"},
	};
	unsigned i;

	for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
		if (!strcmp(num, ops[i].num))
			return ops[i].name;
	return num;
}

/* --- every 2 s: battery, traffic, Wi-Fi, system */
static unsigned long long cpu_busy0, cpu_all0;

static void collect_fast(struct data *d, double dt)
{
	char b[256], mac[32];
	FILE *f;
	struct statvfs vs;
	unsigned long long rx, tx, u, n, s, i, w, q, sq;
	long v;

	d->bat_pct = readl("/sys/class/power_supply/battery/capacity");
	v = readl("/sys/class/power_supply/battery/current_now");
	d->bat_ma = v == -1 ? 0 : (int)(v / 1000);
	d->bat_mv = (int)(readl("/sys/class/power_supply/battery/voltage_now") / 1000);
	d->bat_temp10 = (int)readl("/sys/class/power_supply/battery/temp");
	d->usb_online = readl("/sys/class/power_supply/usb/online") == 1;
	d->charging = d->usb_online && d->bat_ma > 20;

	rx = readl("/sys/class/net/sipa_eth0/statistics/rx_bytes");
	tx = readl("/sys/class/net/sipa_eth0/statistics/tx_bytes");
	if (d->rx && rx >= d->rx && dt > 0) {
		d->rx_bps = (rx - d->rx) / dt;
		d->tx_bps = (tx - d->tx) / dt;
	}
	d->rx = rx;
	d->tx = tx;
	d->rx_total = rx;
	d->tx_total = tx;
	run("ip -4 -o addr show sipa_eth0 2>/dev/null | awk '{print $4}' | cut -d/ -f1", d->wan_ip, sizeof(d->wan_ip));
	run("ifstatus wan 2>/dev/null | grep -c '\"up\": true'", b, sizeof(b));
	d->wan_up = atoi(b) > 0;

	run("uci -q get wireless.ap0.ssid", d->ssid, sizeof(d->ssid));
	run("uci -q get wireless.ap0.key", d->key, sizeof(d->key));
	run("uci -q get wireless.ap0.disabled", b, sizeof(b));
	d->wifi_on = strcmp(b, "1") != 0;
	d->nclients = 0;
	f = popen("iw dev wlan0 station dump 2>/dev/null | awk '/^Station/{print $2}'", "r");
	while (f && d->nclients < MAXCLIENTS && fgets(mac, sizeof(mac), f)) {
		struct client *c = &d->clients[d->nclients++];
		FILE *l = fopen("/tmp/dhcp.leases", "r");

		mac[strcspn(mac, "\r\n")] = 0;
		snprintf(c->mac, sizeof(c->mac), "%s", mac);
		strcpy(c->ip, "-");
		strcpy(c->name, "");
		while (l && fgets(b, sizeof(b), l)) {
			char lmac[32], ip[32], name[64];

			if (sscanf(b, "%*s %31s %31s %63s", lmac, ip, name) == 3 && !strcasecmp(lmac, mac)) {
				snprintf(c->ip, sizeof(c->ip), "%s", ip);
				snprintf(c->name, sizeof(c->name), "%s", strcmp(name, "*") ? name : "");
			}
		}
		if (l)
			fclose(l);
	}
	if (f)
		pclose(f);

	f = fopen("/proc/stat", "r");
	if (f && fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &w, &q, &sq) == 7) {
		unsigned long long busy = u + n + s + q + sq, all = busy + i + w;

		if (all > cpu_all0)
			d->cpu_pct = (int)(100 * (busy - cpu_busy0) / (all - cpu_all0));
		cpu_busy0 = busy;
		cpu_all0 = all;
	}
	if (f)
		fclose(f);
	f = fopen("/proc/meminfo", "r");
	if (f) {
		long tot = 0, avail = 0;

		while (fgets(b, sizeof(b), f)) {
			sscanf(b, "MemTotal: %ld", &tot);
			sscanf(b, "MemAvailable: %ld", &avail);
		}
		fclose(f);
		d->mem_total_mb = tot / 1024;
		d->mem_free_mb = avail / 1024;
		d->mem_pct = tot ? (int)(100 - avail * 100 / tot) : 0;
	}
	if (statvfs("/", &vs) == 0 && vs.f_blocks)
		d->disk_pct = (int)(100 - vs.f_bavail * 100 / vs.f_blocks);
	reads("/proc/uptime", b, sizeof(b));
	d->uptime_s = atol(b);
	reads("/proc/loadavg", b, sizeof(b));
	d->load1 = atof(b);
	d->temp_soc[0] = 0;
	run("for z in /sys/class/thermal/thermal_zone*; do [ \"$(cat $z/type)\" = soc-thmzone ] && "
	    "expr $(cat $z/temp) / 1000 && break; done", d->temp_soc, sizeof(d->temp_soc));
}

/* --- every 15 s: the modem */
static void collect_modem(struct data *d)
{
	char b[256], num[16];
	int v[11], act = -1;

	run(AT " 'AT+CPIN?' 2>/dev/null", b, sizeof(b));
	d->sim = strstr(b, "READY") != NULL;
	run(AT " 'AT+COPS?' 2>/dev/null", b, sizeof(b));
	d->oper[0] = 0;
	if (sscanf(b, "+COPS: %*d,%*d,\"%15[^\"]\",%d", num, &act) >= 1)
		snprintf(d->oper, sizeof(d->oper), "%s", oper_name(num));
	d->act = act;
	snprintf(d->net, sizeof(d->net), "%s",
		 /* 13 is E-UTRA-NR dual connectivity: an LTE cell that can add NR - Android shows LTE for it */
		 act == 7 || act == 13 ? "LTE" : act == 11 || act == 12 ? "5G" : act == 2 || act == 4 || act == 5 || act == 6 ? "3G" :
		 act == 0 || act == 3 ? "2G" : "");
	run(AT " 'AT+CEREG?' 2>/dev/null", b, sizeof(b));
	d->reg = 0;
	sscanf(b, "+CEREG: %*d,%d", &d->reg);
	run(AT " 'AT+CESQ' 2>/dev/null", b, sizeof(b));
	d->rsrp = d->rsrq = 0;
	if (sscanf(b, "+CESQ: %d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6 && v[5] != 255) {
		d->rsrp = v[5] - 141;
		d->rsrq = v[4] != 255 ? v[4] / 2 - 19 : 0;
	}
	d->bars = !d->rsrp ? 0 : d->rsrp >= -85 ? 5 : d->rsrp >= -95 ? 4 : d->rsrp >= -105 ? 3 : d->rsrp >= -115 ? 2 : 1;
}

/* --- every 30 s: the SIM's messages (mu300's sms list: "#IDX  from SENDER  TIME", the text indented) */
static void collect_sms(struct data *d)
{
	FILE *p = popen("/opt/mu5358/bin/sms list 2>/dev/null | sed 's/\\x1b\\[[0-9;]*m//g'", "r");
	char b[512];
	int n = 0;

	if (!p)
		return;
	while (fgets(b, sizeof(b), p) && n < MAXSMS) {
		b[strcspn(b, "\r\n")] = 0;
		if (b[0] == '#') {
			struct sms *s = &d->sms[n];
			char *from = strstr(b, "  from "), *t;

			memset(s, 0, sizeof(*s));
			sscanf(b + 1, "%15s", s->index);
			if (from) {
				from += 7;
				t = strstr(from, "  ");
				if (t) {
					snprintf(s->time, sizeof(s->time), "%.*s", 19, t + 2);
					*t = 0;
				}
				snprintf(s->sender, sizeof(s->sender), "%s", from);
			}
		} else if (!strncmp(b, "    ", 4) && n < MAXSMS && d->sms[n].sender[0]) {
			snprintf(d->sms[n].text, sizeof(d->sms[n].text), "%s", b + 4);
			n++;
		}
	}
	pclose(p);
	d->nsms = n;
}

static void *collector(void *arg)
{
	struct data local;
	long last_modem = -100, last_sms = -100, t;
	struct timespec ts, prev = {0};

	(void)arg;
	memset(&local, 0, sizeof(local));
	for (;;) {
		double dt;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		dt = prev.tv_sec ? ts.tv_sec - prev.tv_sec + (ts.tv_nsec - prev.tv_nsec) / 1e9 : 0;
		prev = ts;
		t = ts.tv_sec;
		pthread_mutex_lock(&data_lock);
		local.nlog = D.nlog;
		memcpy(local.log, D.log, sizeof(local.log));
		pthread_mutex_unlock(&data_lock);
		collect_fast(&local, dt);
		/* what is there now, before the modem and the SIM, which take seconds */
		pthread_mutex_lock(&data_lock);
		local.nlog = D.nlog;
		memcpy(local.log, D.log, sizeof(local.log));
		local.logserial = D.logserial;
		local.serial = D.serial + 1;
		D = local;
		pthread_mutex_unlock(&data_lock);
		if (t - last_modem >= 15) {
			collect_modem(&local);
			last_modem = t;
		}
		if (t - last_sms >= 30) {
			collect_sms(&local);
			last_sms = t;
		}
		pthread_mutex_lock(&data_lock);
		local.nlog = D.nlog;
		memcpy(local.log, D.log, sizeof(local.log));
		local.logserial = D.logserial;
		local.serial = D.serial + 1;
		D = local;
		pthread_mutex_unlock(&data_lock);
		/* 2 s, or at once when poked */
		pthread_mutex_lock(&poke_lock);
		if (!poked) {
			struct timespec until;

			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += data_screen_on ? 2 : 30;
			pthread_cond_timedwait(&poke_cond, &poke_lock, &until);
		}
		if (poked) {
			poked = 0;
			last_modem = -100;
		}
		pthread_mutex_unlock(&poke_lock);
	}
	return NULL;
}

static void log_add(const char *line)
{
	pthread_mutex_lock(&data_lock);
	if (D.nlog == LOGLINES) {
		memmove(D.log[0], D.log[1], sizeof(D.log[0]) * (LOGLINES - 1));
		D.nlog--;
	}
	snprintf(D.log[D.nlog++], sizeof(D.log[0]), "%s", line);
	D.logserial++;
	pthread_mutex_unlock(&data_lock);
}

/* the system log, without the periodic noise screen-console also leaves out; "Fri Oct 30 00:51:07 2026 " -> "00:51:07 " */
static void *logger(void *arg)
{
	static const char *noise[] = {"charger-manager", "charger_policy", "policy: ", "sprd-fgu", "sc27xx-fgu", "SPRD_PDBG",
				      "|__", "r_speed", "SFP:", "AICWFDBG", "thmzone", "bq2560x", "zte_power_supply", "sprd_iommu",
				      "cm_batt_works", "typec-pd"};
	char b[512];

	(void)arg;
	for (;;) {
		FILE *p = popen("logread -f 2>/dev/null", "r");

		while (p && fgets(b, sizeof(b), p)) {
			unsigned i;
			int skip = 0;
			char out[128];

			b[strcspn(b, "\r\n")] = 0;
			for (i = 0; i < sizeof(noise) / sizeof(noise[0]); i++)
				if (strstr(b, noise[i]))
					skip = 1;
			if (skip)
				continue;
			if (strlen(b) > 25 && b[3] == ' ' && b[7] == ' ' && b[13] == ':')
				snprintf(out, sizeof(out), "%.8s %s", b + 11, b + 25);
			else
				snprintf(out, sizeof(out), "%s", b);
			log_add(out);
		}
		if (p)
			pclose(p);
		sleep(5);
	}
	return NULL;
}

void data_start(void)
{
	pthread_t t;

	pthread_create(&t, NULL, collector, NULL);
	pthread_create(&t, NULL, logger, NULL);
}

void data_poke(void)
{
	pthread_mutex_lock(&poke_lock);
	poked = 1;
	pthread_cond_signal(&poke_cond);
	pthread_mutex_unlock(&poke_lock);
}

static void *runner(void *arg)
{
	char *cmd = arg, b[256];
	FILE *p = popen(cmd, "r");

	snprintf(b, sizeof(b), "u30ui: %s", cmd);
	log_add(b);
	while (p && fgets(b, sizeof(b), p)) {
		b[strcspn(b, "\r\n")] = 0;
		log_add(b);
	}
	if (p)
		pclose(p);
	free(cmd);
	data_poke();
	return NULL;
}

void data_run(const char *cmd)
{
	pthread_t t;
	char *c = strdup(cmd);

	if (c && pthread_create(&t, NULL, runner, c) == 0)
		pthread_detach(t);
}
