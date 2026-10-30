/*
 * data: the board's state for the dashboard. From sysfs and procfs directly (no processes) every 2 s while the
 * screen is on - every second while a page draws the throughput graph, every 30 s while the screen is off; the
 * Wi-Fi stations (iw) every 10 s, the modem (mu300-at) every 15 s, the SIM's messages and the traffic totals every
 * 30 s, uci every 30 s. All in threads of their own: the screen never waits for the modem.
 * SPDX-License-Identifier: MIT
 */
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "data.h"

struct data D;
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t poke_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t poke_cond = PTHREAD_COND_INITIALIZER;
static int poked;
volatile int data_screen_on = 1;
volatile int data_want_fast;

#define AT "/opt/mu5358/bin/mu5358-at"
#define WAN "sipa_eth0"

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
		{"46000", "China Mobile"}, {"46001", "China Unicom"}, {"46011", "China Telecom"},
	};
	unsigned i;

	for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
		if (!strcmp(num, ops[i].num))
			return ops[i].name;
	return num;
}

/* --- the thermal zones, found once by name */
static char tz_soc[48], tz_wcn[48], tz_pa[48];
static void find_thermal(void)
{
	int i;

	for (i = 0; i < 40; i++) {
		char p[64], t[32];

		snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/type", i);
		reads(p, t, sizeof(t));
		if (!t[0])
			continue;
		snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", i);
		if (!strcmp(t, "soc-thmzone"))
			strcpy(tz_soc, p);
		else if (!strcmp(t, "wcn-thmzone"))
			strcpy(tz_wcn, p);
		else if (!strcmp(t, "pa-thmzone"))
			strcpy(tz_pa, p);
	}
}
static int temp_of(const char *p)
{
	long v = p[0] ? readl(p) : -1;

	return v < 0 ? -1000 : (int)(v / 1000);
}

/* the IPv4 address and the link of an interface, without a process */
static int if_addr(const char *ifname, char *out, int n)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq r;
	int ok = 0;

	out[0] = 0;
	if (s < 0)
		return 0;
	memset(&r, 0, sizeof(r));
	snprintf(r.ifr_name, IFNAMSIZ, "%s", ifname);
	if (ioctl(s, SIOCGIFADDR, &r) == 0) {
		inet_ntop(AF_INET, &((struct sockaddr_in *)&r.ifr_addr)->sin_addr, out, n);
		ok = 1;
	}
	close(s);
	return ok;
}

/* --- every tick: battery, traffic, system (files only) */
static unsigned long long cpu_busy0[NCPU + 1], cpu_all0[NCPU + 1];

static void collect_fast(struct data *d, double dt)
{
	char b[256], ifn[16], st[16];
	FILE *f;
	struct statvfs vs;
	unsigned long long rx, tx, u, n, s, i, w, q, sq;
	long v;
	int k;

	d->bat_pct = (int)readl("/sys/class/power_supply/battery/capacity");
	v = readl("/sys/class/power_supply/battery/current_now");
	d->bat_ma = v == -1 ? 0 : (int)(v / 1000);
	d->bat_mv = (int)(readl("/sys/class/power_supply/battery/voltage_now") / 1000);
	d->bat_temp10 = (int)readl("/sys/class/power_supply/battery/temp");
	d->usb_online = readl("/sys/class/power_supply/usb/online") == 1;
	reads("/sys/class/power_supply/battery/status", st, sizeof(st));
	d->bat_full = !strcmp(st, "Full");
	d->charging = !strcmp(st, "Charging") || (d->usb_online && d->bat_ma > 20);
	reads("/sys/class/power_supply/battery/health", d->bat_health, sizeof(d->bat_health));

	rx = readl("/sys/class/net/" WAN "/statistics/rx_bytes");
	tx = readl("/sys/class/net/" WAN "/statistics/tx_bytes");
	if (d->rx && rx >= d->rx && dt > 0.2) {
		d->rx_bps = (rx - d->rx) * 8 / dt;
		d->tx_bps = (tx - d->tx) * 8 / dt;
	}
	d->rx = rx;
	d->tx = tx;
	d->rx_total = rx;
	d->tx_total = tx;
	/* the history: one slot a second; a slower tick fills the slots it skipped with the same rate */
	for (k = 0; k < (dt > 0.2 && dt < HIST ? (int)(dt + 0.5) : 1); k++) {
		memmove(d->rx_hist, d->rx_hist + 1, sizeof(d->rx_hist) - sizeof(d->rx_hist[0]));
		memmove(d->tx_hist, d->tx_hist + 1, sizeof(d->tx_hist) - sizeof(d->tx_hist[0]));
		d->rx_hist[HIST - 1] = (float)d->rx_bps;
		d->tx_hist[HIST - 1] = (float)d->tx_bps;
	}
	d->wan_up = if_addr(WAN, d->wan_ip, sizeof(d->wan_ip));
	reads("/sys/class/net/" WAN "/operstate", st, sizeof(st));
	if (strcmp(st, "up") && strcmp(st, "unknown"))
		d->wan_up = 0;

	f = fopen("/proc/stat", "r");
	k = 0;
	while (f && fgets(b, sizeof(b), f)) {
		int c;

		if (sscanf(b, "%15s %llu %llu %llu %llu %llu %llu %llu", ifn, &u, &n, &s, &i, &w, &q, &sq) != 8)
			continue;
		if (!strcmp(ifn, "cpu"))
			c = NCPU;
		else if (sscanf(ifn, "cpu%d", &c) != 1 || c < 0 || c >= NCPU)
			continue;
		{
			unsigned long long busy = u + n + s + q + sq, all = busy + i + w;
			int pct = all > cpu_all0[c] ? (int)(100 * (busy - cpu_busy0[c]) / (all - cpu_all0[c])) : 0;

			if (c == NCPU)
				d->cpu_pct = pct;
			else {
				d->core_pct[c] = pct;
				if (c + 1 > k)
					k = c + 1;
			}
			cpu_busy0[c] = busy;
			cpu_all0[c] = all;
		}
	}
	if (f)
		fclose(f);
	d->ncpu = k;
	for (k = 0; k < d->ncpu; k++) {
		snprintf(b, sizeof(b), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", k);
		v = readl(b);
		d->freq_mhz[k] = v > 0 ? (int)(v / 1000) : 0;
	}
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
	if (statvfs("/", &vs) == 0 && vs.f_blocks) {
		d->disk_pct = (int)(100 - vs.f_bavail * 100 / vs.f_blocks);
		d->disk_free_mb = (long)(vs.f_bavail * (unsigned long long)vs.f_frsize / 1048576);
	}
	reads("/proc/uptime", b, sizeof(b));
	d->uptime_s = atol(b);
	reads("/proc/loadavg", b, sizeof(b));
	sscanf(b, "%lf %lf", &d->load1, &d->load5);
	d->temp_soc = temp_of(tz_soc);
	d->temp_wcn = temp_of(tz_wcn);
	d->temp_pa = temp_of(tz_pa);
	d->temp_bat = d->bat_temp10 == -1 ? -1000 : d->bat_temp10 / 10;
}

/* --- every 10 s: the Wi-Fi stations (iw) */
static void collect_stations(struct data *d)
{
	FILE *f = popen("iw dev wlan0 station dump 2>/dev/null", "r");
	char b[256];
	struct client *c = NULL;

	d->nclients = 0;
	while (f && fgets(b, sizeof(b), f)) {
		char mac[32];
		long long v;

		if (sscanf(b, "Station %31s", mac) == 1) {
			if (d->nclients == MAXCLIENTS)
				break;
			c = &d->clients[d->nclients++];
			memset(c, 0, sizeof(*c));
			snprintf(c->mac, sizeof(c->mac), "%s", mac);
			strcpy(c->ip, "-");
		} else if (c && sscanf(b, " rx bytes: %lld", &v) == 1) {
			c->rx = v;
		} else if (c && sscanf(b, " tx bytes: %lld", &v) == 1) {
			c->tx = v;
		} else if (c && sscanf(b, " signal: %lld", &v) == 1) {
			c->signal = (int)v;
		}
	}
	if (f)
		pclose(f);
	if (d->nclients) {
		FILE *l = fopen("/tmp/dhcp.leases", "r");
		int i;

		while (l && fgets(b, sizeof(b), l)) {
			char lmac[32], ip[32], name[64];

			if (sscanf(b, "%*s %31s %31s %63s", lmac, ip, name) != 3)
				continue;
			for (i = 0; i < d->nclients; i++)
				if (!strcasecmp(lmac, d->clients[i].mac)) {
					snprintf(d->clients[i].ip, sizeof(d->clients[i].ip), "%s", ip);
					snprintf(d->clients[i].name, sizeof(d->clients[i].name), "%s", strcmp(name, "*") ? name : "");
				}
		}
		if (l)
			fclose(l);
	}
}

/* --- every 30 s: the configuration (uci), the radio (iw), the traffic totals */
static void collect_slow(struct data *d)
{
	char b[256];
	FILE *p;

	run("uci -q get wireless.ap0.ssid", d->ssid, sizeof(d->ssid));
	run("uci -q get wireless.ap0.key", d->key, sizeof(d->key));
	run("uci -q get wireless.ap0.disabled", b, sizeof(b));
	d->wifi_on = strcmp(b, "1") != 0;
	run("uci -q get network.wan.auto", b, sizeof(b));
	d->data_on = strcmp(b, "0") != 0;
	run("uci -q get network.wan.apn", d->apn, sizeof(d->apn));
	reads("/sys/class/net/wlan0/address", d->wifi_mac, sizeof(d->wifi_mac));
	d->channel = d->width_mhz = d->band_ghz = 0;
	p = popen("iw dev wlan0 info 2>/dev/null", "r");
	while (p && fgets(b, sizeof(b), p)) {
		int ch, mhz, wd;

		if (sscanf(b, " channel %d (%d MHz), width: %d MHz", &ch, &mhz, &wd) == 3) {
			d->channel = ch;
			d->width_mhz = wd;
			d->band_ghz = mhz > 4000 ? 5 : 2;
		}
	}
	if (p)
		pclose(p);
	/* mu5358-traffic json: {"today":{"date":..,"rx":N,"tx":N},...,"cycle":{..,"rx":N,"tx":N,..}} on one line */
	d->today_rx = d->today_tx = d->month_rx = d->month_tx = 0;
	p = popen("/opt/mu5358/bin/mu5358-traffic json 2>/dev/null", "r");
	if (p) {
		static char j[4096];
		char *s;
		long long rx, tx;

		if (fgets(j, sizeof(j), p)) {
			if ((s = strstr(j, "\"today\":{")) && (s = strstr(s, "\"rx\":")) && sscanf(s, "\"rx\":%lld,\"tx\":%lld", &rx, &tx) == 2) {
				d->today_rx = rx;
				d->today_tx = tx;
			}
			if ((s = strstr(j, "\"cycle\":{")) && (s = strstr(s, "\"rx\":")) && sscanf(s, "\"rx\":%lld,\"tx\":%lld", &rx, &tx) == 2) {
				d->month_rx = rx;
				d->month_tx = tx;
			}
		}
		pclose(p);
	}
}

/* --- every 15 s: the modem */
static int spengmd_field(const char *s, int idx)
{
	/* "3-1279-302--10046--1054-...": fields split by '-', a negative one as an empty field then its digits */
	int i = 0, neg = 0;

	while (*s && i < idx) {
		if (*s == '-') {
			if (s[1] == '-')
				s++;	/* the empty field before a negative */
			i++;
		}
		s++;
	}
	if (*s == '-') {
		neg = 1;
		s++;
	}
	return (neg ? -1 : 1) * atoi(s);
}

static void collect_modem(struct data *d)
{
	char b[512], num[16], tac[16], ci[16];
	int v[11], act = -1, stat = 0;

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
	d->tac[0] = d->cell[0] = 0;
	if (sscanf(b, "+CEREG: %*d,%d,\"%15[^\"]\",\"%15[^\"]\"", &stat, tac, ci) >= 1) {
		d->reg = stat;
		snprintf(d->tac, sizeof(d->tac), "%s", tac);
		snprintf(d->cell, sizeof(d->cell), "%s", ci);
	}
	d->roaming = stat == 5;
	run(AT " 'AT+CESQ' 2>/dev/null", b, sizeof(b));
	d->rsrp = d->rsrq = 0;
	if (sscanf(b, "+CESQ: %d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6 && v[5] != 255) {
		d->rsrp = v[5] - 141;
		d->rsrq = v[4] != 255 ? v[4] / 2 - 19 : 0;
	}
	/* the LTE serving cell (Unisoc engineering mode): band, EARFCN, PCI, RSRP and RSRQ in hundredths */
	d->band = d->earfcn = d->pci = d->sinr = 0;
	run(AT " 'AT+SPENGMD=0,6,0' 2>/dev/null", b, sizeof(b));
	if (b[0] >= '0' && b[0] <= '9') {
		d->band = spengmd_field(b, 0);
		d->earfcn = spengmd_field(b, 1);
		d->pci = spengmd_field(b, 2);
		if (!d->rsrp && spengmd_field(b, 3) < 0)
			d->rsrp = spengmd_field(b, 3) / 100;
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

static void publish(struct data *local)
{
	pthread_mutex_lock(&data_lock);
	local->nlog = D.nlog;
	memcpy(local->log, D.log, sizeof(local->log));
	local->logserial = D.logserial;
	local->serial = D.serial + 1;
	D = *local;
	pthread_mutex_unlock(&data_lock);
}

static void *collector(void *arg)
{
	struct data local;
	long last_modem = -100, last_sms = -100, last_slow = -100, last_sta = -100, last_sig = 0, t;
	struct timespec ts, prev = {0};
	struct utsname un;

	(void)arg;
	memset(&local, 0, sizeof(local));
	find_thermal();
	if (uname(&un) == 0)
		snprintf(local.kernel, sizeof(local.kernel), "%s", un.release);
	reads("/etc/mu5358/version", local.version, sizeof(local.version));
	for (;;) {
		double dt;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		dt = prev.tv_sec ? ts.tv_sec - prev.tv_sec + (ts.tv_nsec - prev.tv_nsec) / 1e9 : 0;
		prev = ts;
		t = ts.tv_sec;
		collect_fast(&local, dt);
		if (t - last_sta >= (data_screen_on ? 10 : 30)) {
			collect_stations(&local);
			last_sta = t;
		}
		if (t - last_slow >= 30) {
			collect_slow(&local);
			last_slow = t;
		}
		/* what is there now, before the modem and the SIM, which take seconds */
		publish(&local);
		if (t - last_modem >= 15) {
			collect_modem(&local);
			last_modem = t;
			if (t - last_sig >= 10) {
				memmove(local.sig_hist, local.sig_hist + 1, SIGHIST - 1);
				local.sig_hist[SIGHIST - 1] = (signed char)(local.rsrp < -127 ? -127 : local.rsrp);
				last_sig = t;
			}
		}
		if (t - last_sms >= 30) {
			collect_sms(&local);
			last_sms = t;
		}
		publish(&local);
		/* the period, or at once when poked */
		pthread_mutex_lock(&poke_lock);
		if (!poked) {
			struct timespec until;

			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += !data_screen_on ? 30 : data_want_fast ? 1 : 2;
			pthread_cond_timedwait(&poke_cond, &poke_lock, &until);
		}
		if (poked) {
			poked = 0;
			last_modem = last_slow = -100;
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

/* the system log, without the periodic noise; "Fri Oct 30 00:51:07 2026 " -> "00:51:07 " */
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
