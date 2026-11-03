/* A file-backed /dev/i2c-4 for tests/test_nfc.py: the NFC tool compiled against this file sees NFC_TEST_IMAGE as the
 * tag's memory (16-bit addresses: 0x00-0x0B UID and lock, the capability container at 0x0C, the data area from
 * 0x10, the SAK byte at 0x3BF). Register writes (addresses >= 0x400, no EEPROM behind them) are appended to
 * NFC_TEST_IMAGE.log as "reg ADDR VALUE"; NFC_TEST_FAIL_AT=ADDR makes every write at ADDR fail with EIO.
 * After the fork's tests/fixtures/u30pro_nfc_io.c (kanoqwq/mu300-linux u30pro-bringup, MIT). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int test_open(const char *path, int flags, ...)
{
	if (strcmp(path, "/dev/i2c-4")) {
		errno = ENODEV;
		return -1;
	}
	return open(getenv("NFC_TEST_IMAGE"), flags);
}

static int test_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	struct i2c_rdwr_ioctl_data *d;
	struct i2c_msg *m;
	unsigned a;
	const char *fail;

	va_start(ap, request);
	d = va_arg(ap, struct i2c_rdwr_ioctl_data *);
	va_end(ap);
	if (request != I2C_RDWR || !d || !d->nmsgs)
		return -1;
	m = d->msgs;
	if (m[0].addr != 0x57 || m[0].len < 2)
		return -1;
	a = (m[0].buf[0] << 8) | m[0].buf[1];
	if (d->nmsgs == 2 && (m[1].flags & I2C_M_RD))
		return pread(fd, m[1].buf, m[1].len, a) == m[1].len ? 2 : -1;
	if (d->nmsgs != 1 || m[0].len < 3)
		return -1;
	fail = getenv("NFC_TEST_FAIL_AT");
	if (fail && a == strtoul(fail, NULL, 0)) {
		errno = EIO;
		return -1;
	}
	if (a >= 0x400) {
		char path[4096];
		FILE *log;

		snprintf(path, sizeof(path), "%s.log", getenv("NFC_TEST_IMAGE"));
		log = fopen(path, "a");
		if (!log)
			return -1;
		fprintf(log, "reg %x %02x\n", a, m[0].buf[2]);
		fclose(log);
		return 1;
	}
	return pwrite(fd, m[0].buf + 2, m[0].len - 2, a) == m[0].len - 2 ? 1 : -1;
}

#define open test_open
#define ioctl test_ioctl
#include "mu5358-nfc.c"
