/*
 * gpt-userdata: show or move the end of the GPT partition "userdata", the last one on the U30 Pro's eMMC, to make
 * room behind it for Linux (or give the room back). mu300-linux's tools/resize-last-partition.py, on the device.
 *
 *   gpt-userdata DISK                  userdata START END, the last usable LBA, the disk's sectors
 *   gpt-userdata DISK END [--dry-run]  userdata ends at LBA END (inclusive), in both GPT copies
 *
 * Only the last partition moves, so no other entry changes and every other partition keeps its place. Everything
 * is checked before anything is written: both signatures and header CRCs, both entry arrays and their CRCs, that
 * the two copies describe the same layout, that userdata is the partition that ends last and that END is inside
 * the usable area. The backup copy is written first, then the primary; both are read back. The kernel keeps the
 * old size until the next boot (no BLKRRPART: userdata is in use).
 * Build: aarch64-linux-android30-clang -static -O2 -o gpt-userdata gpt-userdata.c
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECTOR 512
#define MAXENT (128 * 128)

struct hdr {
	char sig[8];
	uint32_t rev, hsize, hcrc, reserved;
	uint64_t cur, bak, first, last;
	uint8_t guid[16];
	uint64_t entries;
	uint32_t nents, esize, ecrc;
} __attribute__((packed));

static uint32_t crc32(const void *p, size_t n)
{
	static uint32_t t[256];
	const uint8_t *b = p;
	uint32_t c = 0xffffffff;

	if (!t[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t x = i;
			for (int k = 0; k < 8; k++)
				x = x & 1 ? 0xedb88320 ^ (x >> 1) : x >> 1;
			t[i] = x;
		}
	while (n--)
		c = t[(c ^ *b++) & 0xff] ^ (c >> 8);
	return ~c;
}

static int fd;

static int rd(uint64_t lba, void *buf, size_t n)
{
	return pread(fd, buf, n, (off_t)(lba * SECTOR)) == (ssize_t)n ? 0 : -1;
}

static int wr(uint64_t lba, const void *buf, size_t n)
{
	return pwrite(fd, buf, n, (off_t)(lba * SECTOR)) == (ssize_t)n && fsync(fd) == 0 ? 0 : -1;
}

static int die(const char *m)
{
	fprintf(stderr, "gpt-userdata: %s\n", m);
	return 1;
}

/* the header at LBA and its entry array; 0 when both CRCs hold */
static int load(uint64_t lba, struct hdr *h, uint8_t *sec, uint8_t *ent)
{
	struct hdr x;

	if (rd(lba, sec, SECTOR))
		return -1;
	memcpy(h, sec, sizeof(*h));
	if (memcmp(h->sig, "EFI PART", 8) || h->hsize < 92 || h->hsize > SECTOR || h->esize != 128 ||
	    !h->nents || h->nents * h->esize > MAXENT)
		return -1;
	memcpy(&x, sec, sizeof(x));
	x.hcrc = 0;
	{
		uint8_t tmp[SECTOR];
		memcpy(tmp, sec, SECTOR);
		memcpy(tmp, &x, sizeof(x));
		if (crc32(tmp, h->hsize) != h->hcrc)
			return -1;
	}
	if (rd(h->entries, ent, h->nents * h->esize) || crc32(ent, h->nents * h->esize) != h->ecrc)
		return -1;
	return 0;
}

static void reseal(uint8_t *sec, struct hdr *h, const uint8_t *ent)
{
	h->ecrc = crc32(ent, h->nents * h->esize);
	h->hcrc = 0;
	memcpy(sec, h, sizeof(*h));
	h->hcrc = crc32(sec, h->hsize);
	memcpy(sec, h, sizeof(*h));
}

int main(int argc, char **argv)
{
	static uint8_t pe[MAXENT], be[MAXENT], chk[MAXENT];
	uint8_t ps[SECTOR], bs[SECTOR], cs[SECTOR];
	struct hdr ph, bh;
	uint64_t disk, end = 0, last_end = 0;
	int dry = argc > 3 && !strcmp(argv[3], "--dry-run"), ud = -1;
	struct stat st;

	if (argc < 2 || argc > 4)
		return die("usage: gpt-userdata DISK [END [--dry-run]]");
	if (argc > 2) {
		char *e;
		end = strtoull(argv[2], &e, 10);
		if (*e || !end)
			return die("bad END");
	}
	fd = open(argv[1], argc > 2 && !dry ? O_RDWR : O_RDONLY);
	if (fd < 0 || fstat(fd, &st))
		return die(strerror(errno));
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &disk))
			return die("BLKGETSIZE64");
	} else
		disk = st.st_size;
	disk /= SECTOR;

	if (load(1, &ph, ps, pe))
		return die("primary GPT: bad signature or CRC");
	if (ph.cur != 1 || ph.bak != disk - 1)
		return die("primary GPT: its backup is not at the disk's last sector");
	if (load(ph.bak, &bh, bs, be))
		return die("backup GPT: bad signature or CRC");
	if (bh.cur != ph.bak || bh.bak != 1 || bh.first != ph.first || bh.last != ph.last || bh.nents != ph.nents ||
	    memcmp(bh.guid, ph.guid, 16) || memcmp(pe, be, ph.nents * ph.esize))
		return die("the two GPT copies differ");

	for (uint32_t i = 0; i < ph.nents; i++) {
		uint8_t *e = pe + i * ph.esize;
		uint64_t l;
		char name[37];
		int k;

		if (!memcmp(e, (uint8_t[16]){ 0 }, 16))
			continue;
		memcpy(&l, e + 40, 8);
		for (k = 0; k < 36 && e[56 + 2 * k]; k++)
			name[k] = e[56 + 2 * k];
		name[k] = 0;
		if (!strcmp(name, "userdata")) {
			if (ud >= 0)
				return die("two partitions named userdata");
			ud = i;
		}
		if (l > last_end)
			last_end = l;
	}
	if (ud < 0)
		return die("no partition named userdata");
	{
		uint8_t *e = pe + ud * ph.esize;
		uint64_t first, cur;

		memcpy(&first, e + 32, 8);
		memcpy(&cur, e + 40, 8);
		if (cur != last_end)
			return die("userdata is not the partition that ends last");
		if (argc == 2) {
			printf("userdata %llu %llu last_usable %llu disk %llu\n", (unsigned long long)first,
			       (unsigned long long)cur, (unsigned long long)ph.last, (unsigned long long)disk);
			return 0;
		}
		if (end > ph.last || end < first + 8388608)
			return die("END outside the usable area (or userdata under 4 GiB)");
		printf("userdata %llu..%llu -> %llu..%llu%s\n", (unsigned long long)first, (unsigned long long)cur,
		       (unsigned long long)first, (unsigned long long)end, dry ? " (dry run)" : "");
		if (dry || end == cur)
			return 0;
		memcpy(e + 40, &end, 8);
		memcpy(be + ud * ph.esize + 40, &end, 8);
	}
	reseal(bs, &bh, be);
	reseal(ps, &ph, pe);
	if (wr(bh.entries, be, bh.nents * bh.esize) || wr(bh.cur, bs, SECTOR))
		return die("writing the backup GPT failed (the primary is unchanged)");
	if (wr(ph.entries, pe, ph.nents * ph.esize) || wr(1, ps, SECTOR))
		return die("writing the primary GPT failed (the backup has the new end)");
	if (rd(1, cs, SECTOR) || memcmp(cs, ps, SECTOR) || rd(ph.entries, chk, ph.nents * ph.esize) ||
	    memcmp(chk, pe, ph.nents * ph.esize) || rd(bh.cur, cs, SECTOR) || memcmp(cs, bs, SECTOR))
		return die("read back differs");
	printf("written\n");
	return 0;
}
