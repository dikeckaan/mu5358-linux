/*
 * png: the back buffer as an RGB PNG (deflate as stored blocks, no zlib); outside the round screen grey.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "png.h"

static unsigned long crc_table[256];
static unsigned long crc(const unsigned char *b, int n, unsigned long c)
{
	int i, k;

	if (!crc_table[1])
		for (i = 0; i < 256; i++) {
			unsigned long v = i;

			for (k = 0; k < 8; k++)
				v = v & 1 ? 0xedb88320UL ^ (v >> 1) : v >> 1;
			crc_table[i] = v;
		}
	for (i = 0; i < n; i++)
		c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
	return c;
}

static void be32(unsigned char *p, unsigned long v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void chunk(FILE *f, const char *type, const unsigned char *d, unsigned long n)
{
	unsigned char h[8];
	unsigned long c;

	be32(h, n);
	memcpy(h + 4, type, 4);
	fwrite(h, 1, 8, f);
	fwrite(d, 1, n, f);
	c = crc((const unsigned char *)type, 4, 0xffffffffUL);
	c = crc(d, (int)n, c) ^ 0xffffffffUL;
	be32(h, c);
	fwrite(h, 1, 4, f);
}

/* RGB, deflate as stored blocks (one per row) */
void png_write(const char *name)
{
	static unsigned char raw[H * (1 + W * 3)], z[H * (1 + W * 3) + H * 5 + 16];
	unsigned char ihdr[13] = {0};
	unsigned long a = 1, b = 0, n = 0, i;
	int x, y, row = 1 + W * 3;
	char tmp[256];
	FILE *f;

	/* written beside it and renamed: a reader never sees half a file */
	snprintf(tmp, sizeof(tmp), "%s.tmp", name);
	f = fopen(tmp, "wb");
	if (!f)
		return;

	for (y = 0; y < H; y++) {
		raw[y * row] = 0;
		for (x = 0; x < W; x++) {
			uint32_t c = gfx_visible(x, y) ? gfx_buf[y * W + x] : 0x404040;

			raw[y * row + 1 + x * 3] = c >> 16;
			raw[y * row + 2 + x * 3] = c >> 8;
			raw[y * row + 3 + x * 3] = c;
		}
	}
	z[n++] = 0x78;
	z[n++] = 0x01;
	for (y = 0; y < H; y++) {
		z[n++] = y == H - 1;
		z[n++] = row & 0xff;
		z[n++] = row >> 8;
		z[n++] = ~row & 0xff;
		z[n++] = (~row >> 8) & 0xff;
		memcpy(z + n, raw + y * row, row);
		n += row;
	}
	for (i = 0; i < sizeof(raw); i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	be32(z + n, b << 16 | a);
	n += 4;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	be32(ihdr, W);
	be32(ihdr + 4, H);
	ihdr[8] = 8;
	ihdr[9] = 2;
	chunk(f, "IHDR", ihdr, 13);
	chunk(f, "IDAT", z, n);
	chunk(f, "IEND", (const unsigned char *)"", 0);
	fclose(f);
	rename(tmp, name);
}

