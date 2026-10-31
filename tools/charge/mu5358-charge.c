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

/* --- the chip: REG00 (IINLIM) and REG01 (CHG_CONFIG) of the master bq2560x, over i2c-dev; a regular file named
 * in MU5358_CHARGE_I2C is a register image (the tests) */
#ifdef __linux__
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#endif

#define REG00 0
#define REG01 1
#define IINLIM_MASK 0x1f
#define EN_HIZ 0x80
#define CHG_CONFIG 0x10
#define I2C_ADDR 0x6b

struct chip {
	int fd;
	int is_file;
	const char *regs;	/* the vendor driver's registers attribute; NULL: i2c-dev */
	int regs_is_file;	/* a regular file stands in for the attribute (tests) */
};

static const char *i2c_path(void)
{
	const char *p = getenv("MU5358_CHARGE_I2C");
	return p ? p : "/dev/i2c-6";
}

/* The vendor driver's (sqc_bq2560x) attribute: a read gives "Master Reg:" then "Reg[NN] = 0xVV" lines, then the
 * SLAVE block; a write of "0 REG VAL" (hex) writes the master's register through the driver's own I2C client
 * (1 would be the slave: never written here). On the bench i2c-dev transfers to the chip answered EIO while this
 * works, and it cannot race the driver's own transfers. */
static const char *regs_path(void)
{
	const char *p = getenv("MU5358_CHARGE_REGS");
	return p ? p : "/sys/bus/i2c/devices/6-006b/registers";
}

/* the master's register REG from the attribute's text, or -1 */
static int regs_parse(const char *text, int reg)
{
	char key[16];
	const char *slave = strstr(text, "SLAVE"), *p;
	snprintf(key, sizeof key, "Reg[%02x] = 0x", reg);
	p = strstr(text, key);
	if (!p || (slave && p > slave))
		return -1;
	return (int)strtol(p + strlen(key), NULL, 16);
}

static int regs_read_text(const struct chip *c, char *buf, size_t n)
{
	FILE *f = fopen(c->regs, "r");
	size_t got;
	if (!f)
		return -1;
	got = fread(buf, 1, n - 1, f);
	fclose(f);
	buf[got] = 0;
	return 0;
}

static int regs_read(const struct chip *c, int reg)
{
	char text[2048];
	if (regs_read_text(c, text, sizeof text) < 0)
		return -1;
	return regs_parse(text, reg);
}

static int regs_write(const struct chip *c, int reg, int val)
{
	char text[2048], key[16], *p;
	FILE *f;
	if (!c->regs_is_file) {
		int fd = open(c->regs, O_WRONLY), n;
		char line[32];
		if (fd < 0)
			return -1;
		n = snprintf(line, sizeof line, "0 %x %x\n", reg, val);
		if (write(fd, line, (size_t)n) != n) {
			close(fd);
			return -1;
		}
		close(fd);
		return 0;
	}
	/* the stand-in file: the Master block's line patched in place */
	if (regs_read_text(c, text, sizeof text) < 0)
		return -1;
	snprintf(key, sizeof key, "Reg[%02x] = 0x", reg);
	p = strstr(text, key);
	if (!p || (strstr(text, "SLAVE") && p > strstr(text, "SLAVE")) || strlen(p) < strlen(key) + 2)
		return -1;
	p += strlen(key);
	p[0] = "0123456789abcdef"[(val >> 4) & 15];
	p[1] = "0123456789abcdef"[val & 15];
	f = fopen(c->regs, "w");
	if (!f)
		return -1;
	fputs(text, f);
	fclose(f);
	return 0;
}

static int chip_open(struct chip *c)
{
	struct stat st;
	const char *p;
	memset(c, 0, sizeof *c);
	c->fd = -1;
	if (stat(regs_path(), &st) == 0) {
		c->regs = regs_path();
		/* a sysfs attribute is a regular file too: the stand-in is whatever is not under /sys */
		c->regs_is_file = strncmp(c->regs, "/sys/", 5) != 0;
		return 0;
	}
	p = i2c_path();
	c->fd = open(p, O_RDWR);
	if (c->fd < 0)
		return -1;
	c->is_file = fstat(c->fd, &st) == 0 && S_ISREG(st.st_mode);
#ifdef __linux__
	if (!c->is_file && ioctl(c->fd, I2C_SLAVE_FORCE, I2C_ADDR) < 0) {	/* the vendor driver holds the address */
		close(c->fd);
		return -1;
	}
#else
	if (!c->is_file) {
		close(c->fd);
		errno = ENOTSUP;
		return -1;
	}
#endif
	return 0;
}

/* plain I2C transactions (I2C_RDWR: a write of the register address, then a read of one byte; or a write of
 * address and value), what every adapter offers; the SMBus emulation is not relied on */
static int chip_read(struct chip *c, int reg)
{
	unsigned char b, r = (unsigned char)reg;
	if (c->regs)
		return regs_read(c, reg);
	if (c->is_file)
		return pread(c->fd, &b, 1, reg) == 1 ? b : -1;
#ifdef __linux__
	{
		struct i2c_msg m[2] = {
			{ I2C_ADDR, 0, 1, &r },
			{ I2C_ADDR, I2C_M_RD, 1, &b },
		};
		struct i2c_rdwr_ioctl_data d = { m, 2 };
		if (ioctl(c->fd, I2C_RDWR, &d) < 0)
			return -1;
		return b;
	}
#else
	return -1;
#endif
}

static int chip_write(struct chip *c, int reg, int val)
{
	unsigned char b[2] = { (unsigned char)reg, (unsigned char)val };
	if (c->regs)
		return regs_write(c, reg, val);
	if (c->is_file)
		return pwrite(c->fd, &b[1], 1, reg) == 1 ? 0 : -1;
#ifdef __linux__
	{
		struct i2c_msg m = { I2C_ADDR, 0, 2, b };
		struct i2c_rdwr_ioctl_data d = { &m, 1 };
		return ioctl(c->fd, I2C_RDWR, &d) < 0 ? -1 : 0;
	}
#else
	return -1;
#endif
}

struct applied {
	int reg00_before, reg00_after, reg01_before, reg01_after;
	int rewritten;	/* 1: REG00 was not what we last wrote; -1: a write did not read back */
};

static int last_reg00 = -1;

/* IINLIM = 100 mA + N * 100 mA */
static int iinlim_bits(int ma)
{
	int n = (ma - 100) / 100;
	return n < 0 ? 0 : n > IINLIM_MASK ? IINLIM_MASK : n;
}

static int apply(struct chip *c, const struct decision *d, struct applied *a)
{
	int r0, r1, w0, w1;
	memset(a, 0, sizeof *a);
	r0 = chip_read(c, REG00);
	r1 = chip_read(c, REG01);
	if (r0 < 0 || r1 < 0)
		return -1;
	a->reg00_before = r0;
	a->reg01_before = r1;
	if (last_reg00 >= 0 && r0 != last_reg00)
		a->rewritten = 1;
	w0 = (r0 & ~(IINLIM_MASK | EN_HIZ)) | iinlim_bits(d->input_ma);
	w1 = d->charge ? (r1 | CHG_CONFIG) : (r1 & ~CHG_CONFIG);
	if (w0 != r0 && chip_write(c, REG00, w0) < 0)
		return -1;
	if (w1 != r1 && chip_write(c, REG01, w1) < 0)
		return -1;
	a->reg00_after = chip_read(c, REG00);
	a->reg01_after = chip_read(c, REG01);
	if (a->reg00_after != w0 || a->reg01_after != w1)
		a->rewritten = -1;
	last_reg00 = w0;
	return 0;
}

/* back to what the vendor driver left: 500 mA, charging on */
static void restore(struct chip *c)
{
	int r0 = chip_read(c, REG00), r1 = chip_read(c, REG01);
	if (r0 >= 0)
		chip_write(c, REG00, (r0 & ~(IINLIM_MASK | EN_HIZ)) | iinlim_bits(MIN_MA));
	if (r1 >= 0)
		chip_write(c, REG01, r1 | CHG_CONFIG);
}

/* --- the state file and the CLI */
static void state_path(char *buf, size_t n)
{
	const char *p = getenv("MU5358_STATE");
	if (p)
		snprintf(buf, n, "%s", p);
	else
		snprintf(buf, n, "%s/run/mu5358/charge", root());
}

static void state_line(char *buf, size_t n, const struct inputs *in, const struct decision *d, const struct applied *a, int rewrites)
{
	snprintf(buf, n, "source=%s input_ma=%d charge=%s reason=%s temp=%d capacity=%d limit=%d reg00=0x%02x reg01=0x%02x rewrites=%d at=%ld",
		 d->source, d->input_ma, d->charge ? "on" : "off", d->reason, in->temp, in->capacity,
		 in->limit > 0 ? in->limit : 100, a->reg00_after & 0xff, a->reg01_after & 0xff, rewrites, (long)time(NULL));
}

/* written whole, then renamed into place */
static void state_write(const char *line)
{
	char path[512], tmp[520];
	FILE *f;
	state_path(path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "%s\n", line);
	fclose(f);
	chmod(tmp, 0644);
	rename(tmp, path);
}

static void state_remove(void)
{
	char path[512];
	state_path(path, sizeof path);
	unlink(path);
}

static volatile sig_atomic_t stop;
static void on_signal(int s)
{
	(void)s;
	stop = 1;
}

/* one round: decide, apply, the state line into LINE; the exit code of `once` */
static int round_once(struct chip *c, struct decision *prev, int have_prev, int *rewrites, char *line, size_t n)
{
	struct inputs in;
	struct decision d;
	struct applied a;
	read_inputs(&in);
	decide(&in, have_prev ? prev : NULL, &d);
	if (apply(c, &d, &a) < 0) {
		snprintf(line, n, "source=%s input_ma=%d charge=%s reason=i2c-error temp=%d capacity=%d error=%s", d.source,
			 d.input_ma, d.charge ? "on" : "off", in.temp, in.capacity, strerror(errno));
		return 4;
	}
	if (a.rewritten == 1)
		(*rewrites)++;
	state_line(line, n, &in, &d, &a, *rewrites);
	*prev = d;
	return a.rewritten < 0 ? 3 : 0;
}

static int cmd_limit(int argc, char **argv)
{
	char path[512], b[32];
	snprintf(path, sizeof path, "%s" P_LIMIT, root());
	if (argc < 3) {
		int v = read_int(P_LIMIT);
		printf("%d\n", v >= 50 && v <= 100 ? v : 100);
		return 0;
	}
	if (!strcmp(argv[2], "off"))
		snprintf(b, sizeof b, "100");
	else {
		char *end;
		long v = strtol(argv[2], &end, 10);
		if (end == argv[2] || *end || v < 50 || v > 100) {
			fprintf(stderr, "mu5358-charge limit: 50..100 or off\n");
			return 2;
		}
		snprintf(b, sizeof b, "%ld", v);
	}
	{
		char dir[512], *s;
		FILE *f;
		snprintf(dir, sizeof dir, "%s", path);
		for (s = dir + 1; (s = strchr(s, '/')); s++) {	/* mkdir -p of the directories */
			*s = 0;
			mkdir(dir, 0755);
			*s = '/';
		}
		f = fopen(path, "w");
		if (!f) {
			perror(path);
			return 1;
		}
		fprintf(f, "%s\n", b);
		fclose(f);
	}
	return 0;
}

static int cmd_status(void)
{
	char path[512], b[512];
	FILE *f;
	state_path(path, sizeof path);
	f = fopen(path, "r");
	if (!f) {
		printf("not running\n");
		return 1;
	}
	if (fgets(b, sizeof b, f))
		fputs(b, stdout);
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	struct decision prev;
	int have_prev, rewrites = 0, rc;
	char line[512];
	struct chip c;
	if (!strcmp(cmd, "decide")) {
		struct inputs in;
		struct decision d;
		read_inputs(&in);
		prev_from_env(&prev, &have_prev);
		decide(&in, have_prev ? &prev : NULL, &d);
		print_decision(&d);
		return 0;
	}
	if (!strcmp(cmd, "status"))
		return cmd_status();
	if (!strcmp(cmd, "limit"))
		return cmd_limit(argc, argv);
	if (cmd[0] && strcmp(cmd, "once")) {
		fprintf(stderr, "usage: mu5358-charge [once|decide|status|limit [N|off]]\n");
		return 2;
	}
	if (chip_open(&c) < 0) {
		fprintf(stderr, "mu5358-charge: %s: %s\n", i2c_path(), strerror(errno));
		return 4;
	}
	prev_from_env(&prev, &have_prev);
	/* a previous run's state (a respawn, `once` after `once`): what we last wrote, the rewrite count, and the
	 * decision whose hysteresis still holds */
	{
		char path[512], b[512];
		FILE *f;
		state_path(path, sizeof path);
		f = fopen(path, "r");
		if (f) {
			if (fgets(b, sizeof b, f)) {
				char *p;
				if ((p = strstr(b, " reg00=0x")))
					last_reg00 = (int)strtol(p + 9, NULL, 16);
				if ((p = strstr(b, " rewrites=")))
					rewrites = atoi(p + 10);
				if (!have_prev && (p = strstr(b, " charge="))) {
					char *r = strstr(b, " reason=");
					memset(&prev, 0, sizeof prev);
					prev.charge = strncmp(p + 8, "off", 3) != 0;
					if (r)
						sscanf(r + 8, "%31s", prev.reason);
					have_prev = 1;
				}
			}
			fclose(f);
		}
	}
	if (!strcmp(cmd, "once")) {
		rc = round_once(&c, &prev, have_prev, &rewrites, line, sizeof line);
		state_write(line);
		printf("%s\n", line);
		return rc;
	}
	{
		const char *p = getenv("MU5358_CHARGE_PERIOD");
		int period = p ? atoi(p) : 5;
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_signal;
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGINT, &sa, NULL);
		if (period < 1)
			period = 1;
		while (!stop) {
			rc = round_once(&c, &prev, have_prev, &rewrites, line, sizeof line);
			have_prev = 1;
			state_write(line);
			if (rc == 4)
				fprintf(stderr, "mu5358-charge: %s\n", line);
			for (int i = 0; i < period * 10 && !stop; i++)
				usleep(100000);
		}
		restore(&c);
		state_remove();
		return 0;
	}
}
