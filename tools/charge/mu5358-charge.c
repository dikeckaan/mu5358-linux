/* mu5358-charge: the U30 Pro's charger input limit, set from what the source offers.
 *
 * Without Android's charging policy the vendor stack leaves the master bq2560x at the USB default of 500 mA
 * whatever is plugged in (2026-11-01: the Mac mini offers 3 A over Type-C, Android takes 1.5 A, Linux took
 * 450 mA). Every power-supply sysfs knob accepts a write and changes nothing, so this program talks to the chip
 * itself over /dev/i2c-6 (address 0x6b; kernel/mu5358-linux.fragment's i2c-dev).
 *
 * What it writes, and nothing else:
 *   REG00  bit 7 EN_HIZ (kept 0), bits 4:0 IINLIM = 100 mA + N * 100 mA  (500 -> 0x04, 1500 -> 0x0e)
 *   REG01  bit 4 CHG_CONFIG (charging on/off)
 * Never the slave charger (i2c 4-006b), never VREG (REG04) or ICHG (REG02), never the watchdog (REG05, off).
 *
 * The decision (decide): SDP 500 mA, CDP 1500, DCP 1500 (the hard cap this round; the spec's 2 A waits for a
 * measured DCP charger), Type-C with current_max >= 1.5 A or a PD contract 1500, unknown 500. Guards: battery
 * temperature >= 45.0 C or <= 0 C stops charging (back on under 40.0 C and above 0 C); a charge limit N in
 * 50..99 (/etc/mu5358/charge-limit) stops at N and resumes at N-3; a temperature that cannot be read keeps
 * charging at 500 mA.
 *
 *   mu5358-charge            the daemon: every 5 s decide, apply, write /run/mu5358/charge
 *   mu5358-charge once       one round, the state line on stdout
 *   mu5358-charge decide     the decision only, no I2C (tests)
 *   mu5358-charge status     the state file
 *   mu5358-charge limit [N|off]
 * Environment, for the tests and the service: MU5358_ROOT (a prefix for every path), MU5358_PREV ("on|off:reason"
 * seeds the previous decision for `decide`), MU5358_CHARGE_I2C (the device; a regular file is used as a register
 * image), MU5358_STATE (the state file), MU5358_CHARGE_PERIOD (seconds).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CAP_MA 1500
#define MIN_MA 500
#define HOT 450
#define HOT_RESUME 400
#define COLD 0
#define LIMIT_GAP 3

#define P_USB "/sys/class/power_supply/usb/online"
#define P_TCPM "/sys/class/power_supply/sprd-tcpm-source-psy-sc27xx-pd"
#define P_AC "/sys/class/power_supply/ac"
#define P_BAT "/sys/class/power_supply/battery"
#define P_LIMIT "/etc/mu5358/charge-limit"

struct inputs {
	int usb_online, tcpm_online, tcpm_current_ua;
	char tcpm_type[16];
	int ac_online;
	char ac_type[16];
	int temp, capacity, limit;
};

struct decision {
	int input_ma;
	int charge;
	char source[8];
	char reason[32];
};

static const char *root(void)
{
	const char *r = getenv("MU5358_ROOT");
	return r ? r : "";
}

/* the first line of ROOT/REL, without its newline; "" when it cannot be read */
static int read_line(const char *rel, char *buf, size_t n)
{
	char path[512];
	FILE *f;
	snprintf(path, sizeof path, "%s%s", root(), rel);
	buf[0] = 0;
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, (int)n, f))
		buf[0] = 0;
	fclose(f);
	buf[strcspn(buf, "\r\n")] = 0;
	return 0;
}

/* an integer, or -1 when the file is missing or does not hold one */
static int read_int(const char *rel)
{
	char b[64], *end;
	long v;
	if (read_line(rel, b, sizeof b) < 0 || !b[0])
		return -1;
	v = strtol(b, &end, 10);
	if (end == b || (*end && *end != ' '))
		return -1;
	if (v > 1000000000L || v < -1000000000L)
		return -1;
	return (int)v;
}

/* the word in brackets of a power-supply enum ("[C] PD PD_PPS" -> "C"); "" when there is none */
static void bracketed(const char *s, char *out, size_t n)
{
	const char *a = strchr(s, '['), *b;
	out[0] = 0;
	if (!a || !(b = strchr(a, ']')) || (size_t)(b - a - 1) >= n)
		return;
	memcpy(out, a + 1, (size_t)(b - a - 1));
	out[b - a - 1] = 0;
}

static int read_inputs(struct inputs *in)
{
	char b[128];
	memset(in, 0, sizeof *in);
	in->usb_online = read_int(P_USB);
	in->tcpm_online = read_int(P_TCPM "/online");
	in->tcpm_current_ua = read_int(P_TCPM "/current_max");
	if (read_line(P_TCPM "/usb_type", b, sizeof b) == 0)
		bracketed(b, in->tcpm_type, sizeof in->tcpm_type);
	in->ac_online = read_int(P_AC "/online");
	if (read_line(P_AC "/type", b, sizeof b) == 0)
		snprintf(in->ac_type, sizeof in->ac_type, "%.15s", b);
	in->temp = read_int(P_BAT "/temp");
	in->capacity = read_int(P_BAT "/capacity");
	in->limit = read_int(P_LIMIT);
	return 0;
}

static void set(struct decision *d, int ma, int charge, const char *source, const char *reason)
{
	d->input_ma = ma;
	d->charge = charge;
	if (source)
		snprintf(d->source, sizeof d->source, "%s", source);
	snprintf(d->reason, sizeof d->reason, "%s", reason);
}

static void decide(const struct inputs *in, const struct decision *prev, struct decision *out)
{
	int prev_off = prev && !prev->charge;
	memset(out, 0, sizeof *out);
	/* the source */
	if (in->usb_online != 1 && in->tcpm_online != 1)
		set(out, MIN_MA, 1, "none", "source");
	else if (in->tcpm_online == 1 &&
		 (!strcmp(in->tcpm_type, "PD") || !strcmp(in->tcpm_type, "PD_PPS") ||
		  (!strcmp(in->tcpm_type, "C") && in->tcpm_current_ua >= 1500000)))
		set(out, CAP_MA, 1, "typec", "source");
	else if (in->ac_online == 1 && !strcmp(in->ac_type, "USB_DCP"))
		set(out, CAP_MA, 1, "dcp", "source");
	else if (in->ac_online == 1 && !strcmp(in->ac_type, "USB_CDP"))
		set(out, CAP_MA, 1, "cdp", "source");
	else
		set(out, MIN_MA, 1, "sdp", "source");
	/* the temperature guard */
	if (in->temp == -1) {
		set(out, MIN_MA, 1, NULL, "notemp");
		return;
	}
	if (in->temp >= HOT) {
		set(out, MIN_MA, 0, NULL, "hot");
		return;
	}
	if (in->temp <= COLD) {
		set(out, MIN_MA, 0, NULL, "cold");
		return;
	}
	if (prev_off && !strcmp(prev->reason, "hot") && in->temp > HOT_RESUME) {
		set(out, MIN_MA, 0, NULL, "hot");
		return;
	}
	/* the charge limit */
	if (in->limit >= 50 && in->limit <= 99 && in->capacity >= 0) {
		if (in->capacity >= in->limit ||
		    (prev_off && !strcmp(prev->reason, "limit") && in->capacity > in->limit - LIMIT_GAP))
			set(out, MIN_MA, 0, NULL, "limit");
	}
}

static void prev_from_env(struct decision *prev, int *have)
{
	const char *p = getenv("MU5358_PREV"), *c;
	*have = 0;
	if (!p)
		return;
	memset(prev, 0, sizeof *prev);
	c = strchr(p, ':');
	prev->charge = strncmp(p, "off", 3) != 0;
	snprintf(prev->reason, sizeof prev->reason, "%s", c ? c + 1 : "source");
	*have = 1;
}

static void print_decision(const struct decision *d)
{
	printf("source=%s input_ma=%d charge=%s reason=%s\n", d->source, d->input_ma, d->charge ? "on" : "off", d->reason);
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	if (!strcmp(cmd, "decide")) {
		struct inputs in;
		struct decision prev, d;
		int have;
		read_inputs(&in);
		prev_from_env(&prev, &have);
		decide(&in, have ? &prev : NULL, &d);
		print_decision(&d);
		return 0;
	}
	fprintf(stderr, "usage: mu5358-charge [once|decide|status|limit [N|off]]\n");
	return 2;
}
