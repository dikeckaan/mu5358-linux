/*
 * qr: ISO/IEC 18004 QR codes, byte mode, level M, versions 1..10 - the launcher's QrEncoder.kt in C (the same tables,
 * the same choices: the Wi-Fi payload is all it encodes, M is the level that reads best at 240 px).
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "qr.h"

#define MAXV 10
static const int TOTAL_CW[MAXV] = {26, 44, 70, 100, 134, 172, 196, 242, 292, 346};
static const int ECC_PER_BLOCK[MAXV] = {10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
static const int BLOCKS[MAXV] = {1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
static const int ALIGN[MAXV][4] = {
	{0}, {2, 6, 18}, {2, 6, 22}, {2, 6, 26}, {2, 6, 30}, {2, 6, 34}, {3, 6, 22, 38}, {3, 6, 24, 42},
	{3, 6, 26, 46}, {3, 6, 28, 50},
};	/* [0] the count */

static int gf_exp[512], gf_log[256];

static void gf_init(void)
{
	int i, x = 1;

	if (gf_exp[0])
		return;
	for (i = 0; i < 255; i++) {
		gf_exp[i] = x;
		gf_log[x] = i;
		x <<= 1;
		if (x >= 256)
			x ^= 0x11d;
	}
	for (i = 255; i < 512; i++)
		gf_exp[i] = gf_exp[i - 255];
}

static int gf_mul(int a, int b)
{
	return a && b ? gf_exp[gf_log[a] + gf_log[b]] : 0;
}

static int data_cw(int v)
{
	return TOTAL_CW[v - 1] - ECC_PER_BLOCK[v - 1] * BLOCKS[v - 1];
}

static int count_bits(int v)
{
	return v <= 9 ? 8 : 16;
}

static int byte_capacity(int v)
{
	return (data_cw(v) * 8 - 4 - count_bits(v)) / 8;
}

struct bitbuf {
	unsigned char cw[400];
	int len;
};

static void put(struct bitbuf *b, int value, int count)
{
	int i;

	for (i = count - 1; i >= 0; i--) {
		if ((value >> i) & 1)
			b->cw[b->len >> 3] |= 0x80 >> (b->len & 7);
		b->len++;
	}
}

static void build(const unsigned char *d, int n, int v, unsigned char *out)
{
	static struct bitbuf b;
	int total = TOTAL_CW[v - 1], ecl = ECC_PER_BLOCK[v - 1], blocks = BLOCKS[v - 1], dcw = data_cw(v);
	int cap = dcw * 8, i, j, k = 0, pad = 1, starts[MAXV + 2] = {0}, gen[32] = {1}, glen = 1;
	int shortlen = dcw / blocks, longb = dcw % blocks;
	unsigned char ecc[8][32];

	memset(&b, 0, sizeof(b));
	put(&b, 4, 4);
	put(&b, n, count_bits(v));
	for (i = 0; i < n; i++)
		put(&b, d[i], 8);
	put(&b, 0, cap - b.len < 4 ? cap - b.len : 4);
	while (b.len % 8)
		put(&b, 0, 1);
	while (b.len < cap) {
		put(&b, pad ? 0xec : 0x11, 8);
		pad = !pad;
	}
	for (i = 0; i < blocks; i++)
		starts[i + 1] = starts[i] + shortlen + (i >= blocks - longb ? 1 : 0);
	/* the generator polynomial */
	for (i = 0; i < ecl; i++) {
		int next[32] = {0};

		for (j = 0; j < glen; j++) {
			next[j] ^= gen[j];
			next[j + 1] ^= gf_mul(gen[j], gf_exp[i]);
		}
		glen++;
		memcpy(gen, next, sizeof(gen));
	}
	for (i = 0; i < blocks; i++) {
		int rem[32] = {0}, len = starts[i + 1] - starts[i], x, y;

		for (x = 0; x < len; x++) {
			int f = b.cw[starts[i] + x] ^ rem[0];

			for (y = 0; y < ecl - 1; y++)
				rem[y] = rem[y + 1];
			rem[ecl - 1] = 0;
			for (y = 0; y < ecl; y++)
				rem[y] ^= gf_mul(gen[y + 1], f);
		}
		for (x = 0; x < ecl; x++)
			ecc[i][x] = rem[x];
	}
	for (j = 0; j <= shortlen; j++)
		for (i = 0; i < blocks; i++)
			if (j < starts[i + 1] - starts[i])
				out[k++] = b.cw[starts[i] + j];
	for (j = 0; j < ecl; j++)
		for (i = 0; i < blocks; i++)
			out[k++] = ecc[i][j];
	(void)total;
}

static int S;
static unsigned char *M, *F;

static void setfn(int x, int y, int dark)
{
	if (x < 0 || y < 0 || x >= S || y >= S)
		return;
	M[y * S + x] = dark;
	F[y * S + x] = 1;
}

static void finder(int cx, int cy)
{
	int dx, dy;

	for (dy = -4; dy <= 4; dy++)
		for (dx = -4; dx <= 4; dx++) {
			int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);

			setfn(cx + dx, cy + dy, d != 2 && d != 4);
		}
}

static void alignment(int cx, int cy)
{
	int dx, dy;

	for (dy = -2; dy <= 2; dy++)
		for (dx = -2; dx <= 2; dx++) {
			int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);

			setfn(cx + dx, cy + dy, d != 1);
		}
}

static int bit(int v, int i)
{
	return (v >> i) & 1;
}

static void format_bits(int mask)
{
	int data = mask, rem = data, bits, i;

	for (i = 0; i < 10; i++)
		rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	bits = ((data << 10) | rem) ^ 0x5412;
	for (i = 0; i < 6; i++)
		setfn(8, i, bit(bits, i));
	setfn(8, 7, bit(bits, 6));
	setfn(8, 8, bit(bits, 7));
	setfn(7, 8, bit(bits, 8));
	for (i = 9; i < 15; i++)
		setfn(14 - i, 8, bit(bits, i));
	for (i = 0; i < 8; i++)
		setfn(S - 1 - i, 8, bit(bits, i));
	for (i = 8; i < 15; i++)
		setfn(8, S - 15 + i, bit(bits, i));
	setfn(8, S - 8, 1);
}

static void version_bits(int v)
{
	int rem = v, bits, i;

	if (v < 7)
		return;
	for (i = 0; i < 12; i++)
		rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
	bits = (v << 12) | rem;
	for (i = 0; i < 18; i++) {
		setfn(S - 11 + i % 3, i / 3, bit(bits, i));
		setfn(i / 3, S - 11 + i % 3, bit(bits, i));
	}
}

static void apply_mask(int mask)
{
	int x, y;

	for (y = 0; y < S; y++)
		for (x = 0; x < S; x++) {
			int inv;

			if (F[y * S + x])
				continue;
			switch (mask) {
			case 0: inv = (x + y) % 2 == 0; break;
			case 1: inv = y % 2 == 0; break;
			case 2: inv = x % 3 == 0; break;
			case 3: inv = (x + y) % 3 == 0; break;
			case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
			case 5: inv = x * y % 2 + x * y % 3 == 0; break;
			case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
			default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
			}
			if (inv)
				M[y * S + x] ^= 1;
		}
}

static void push_run(int *h, int len)
{
	int i;

	if (!h[0])
		len += S;
	for (i = 6; i >= 1; i--)
		h[i] = h[i - 1];
	h[0] = len;
}

static int finder_like(const int *h)
{
	int n = h[1], core = n > 0 && h[2] == n && h[3] == n * 3 && h[4] == n && h[5] == n, c = 0;

	if (core && h[0] >= n * 4 && h[6] >= n)
		c++;
	if (core && h[6] >= n * 4 && h[0] >= n)
		c++;
	return c;
}

static int finish_runs(int *h, int colour, int len)
{
	if (colour) {
		push_run(h, len);
		len = 0;
	}
	push_run(h, len + S);
	return finder_like(h);
}

static int penalty(void)
{
	int r = 0, x, y, pass, dark = 0, total = S * S;

	for (pass = 0; pass < 2; pass++)
		for (y = 0; y < S; y++) {
			int h[7] = {0}, colour = 0, len = 0;

			for (x = 0; x < S; x++) {
				int m = pass ? M[x * S + y] : M[y * S + x];

				if (m == colour) {
					len++;
					if (len == 5)
						r += 3;
					else if (len > 5)
						r++;
				} else {
					push_run(h, len);
					if (!colour)
						r += finder_like(h) * 40;
					colour = m;
					len = 1;
				}
			}
			r += finish_runs(h, colour, len) * 40;
		}
	for (y = 0; y < S - 1; y++)
		for (x = 0; x < S - 1; x++) {
			int c = M[y * S + x];

			if (c == M[y * S + x + 1] && c == M[(y + 1) * S + x] && c == M[(y + 1) * S + x + 1])
				r += 3;
		}
	for (x = 0; x < total; x++)
		dark += M[x];
	r += ((abs(dark * 20 - total * 10) + total - 1) / total - 1) * 10;
	return r;
}

int qr_encode(const char *text, unsigned char *out)
{
	const unsigned char *d = (const unsigned char *)text;
	int n = strlen(text), v, i, x, right, vert, j, best = 0, bestp = 1 << 30, mask, totalbits;
	static unsigned char cw[400], fixed[QR_MAX * QR_MAX];

	gf_init();
	for (v = 1; v <= MAXV && n > byte_capacity(v); v++)
		;
	if (v > MAXV)
		return 0;
	S = 17 + 4 * v;
	M = out;
	F = fixed;
	memset(M, 0, S * S);
	memset(F, 0, S * S);
	for (i = 0; i < S; i++) {
		setfn(6, i, i % 2 == 0);
		setfn(i, 6, i % 2 == 0);
	}
	finder(3, 3);
	finder(S - 4, 3);
	finder(3, S - 4);
	for (i = 1; i <= ALIGN[v - 1][0]; i++)
		for (j = 1; j <= ALIGN[v - 1][0]; j++) {
			int last = ALIGN[v - 1][0];

			if ((i == 1 && j == 1) || (i == 1 && j == last) || (i == last && j == 1))
				continue;
			alignment(ALIGN[v - 1][i], ALIGN[v - 1][j]);
		}
	format_bits(0);
	version_bits(v);
	build(d, n, v, cw);
	totalbits = TOTAL_CW[v - 1] * 8;
	i = 0;
	for (right = S - 1; right >= 1; right -= 2) {
		if (right == 6)
			right = 5;
		for (vert = 0; vert < S; vert++)
			for (j = 0; j < 2; j++) {
				int up = ((right + 1) & 2) == 0, y;

				x = right - j;
				y = up ? S - 1 - vert : vert;
				if (!F[y * S + x] && i < totalbits) {
					M[y * S + x] = bit(cw[i >> 3], 7 - (i & 7));
					i++;
				}
			}
	}
	for (mask = 0; mask < 8; mask++) {
		int p;

		apply_mask(mask);
		format_bits(mask);
		p = penalty();
		if (p < bestp) {
			bestp = p;
			best = mask;
		}
		apply_mask(mask);
	}
	apply_mask(best);
	format_bits(best);
	return S;
}

/* WIFI:T:WPA;S:...;P:...;; with \ ; , : " escaped */
void qr_wifi_payload(char *out, int n, const char *ssid, const char *psk)
{
	const char *parts[2] = {ssid, psk}, *pre[2] = {"WIFI:T:WPA;S:", ";P:"};
	int k = 0, p;

	for (p = 0; p < 2; p++) {
		const char *s;

		for (s = pre[p]; *s && k < n - 1; s++)
			out[k++] = *s;
		for (s = parts[p]; *s && k < n - 2; s++) {
			if (strchr("\\;,:\"", *s))
				out[k++] = '\\';
			out[k++] = *s;
		}
	}
	if (k < n - 2) {
		out[k++] = ';';
		out[k++] = ';';
	}
	out[k] = 0;
}
