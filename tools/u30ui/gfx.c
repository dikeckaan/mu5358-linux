/*
 * gfx: shapes by their distance fields (coverage = 0.5 - distance, clamped: one pixel of anti-aliasing) and text by
 * stb_truetype with a glyph cache.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "font_regular.h"
#include "font_medium.h"

uint32_t gfx_buf[W * H];

static stbtt_fontinfo fonts[2];

void gfx_init(void)
{
	stbtt_InitFont(&fonts[FONT_REGULAR], roboto_regular_ttf, 0);
	stbtt_InitFont(&fonts[FONT_MEDIUM], roboto_medium_ttf, 0);
}

int gfx_visible(int x, int y)
{
	float dx = x + 0.5f - W / 2.0f, dy = y + 0.5f - H / 2.0f;

	return dx * dx + dy * dy <= (W / 2.0f) * (W / 2.0f);
}

void gfx_clear(uint32_t c)
{
	int i;

	for (i = 0; i < W * H; i++)
		gfx_buf[i] = c;
}

int gfx_ox;

void gfx_blend(int x, int y, uint32_t c, int a)
{
	uint32_t d, r, g, b;

	x += gfx_ox;
	if (x < 0 || y < 0 || x >= W || y >= H || a <= 0)
		return;
	if (a >= 255) {
		gfx_buf[y * W + x] = c;
		return;
	}
	d = gfx_buf[y * W + x];
	r = (((c >> 16) & 0xff) * a + ((d >> 16) & 0xff) * (255 - a)) / 255;
	g = (((c >> 8) & 0xff) * a + ((d >> 8) & 0xff) * (255 - a)) / 255;
	b = ((c & 0xff) * a + (d & 0xff) * (255 - a)) / 255;
	gfx_buf[y * W + x] = r << 16 | g << 8 | b;
}

static int cov(float d)
{
	float a = 0.5f - d;

	if (a <= 0)
		return 0;
	if (a >= 1)
		return 255;
	return (int)(a * 255);
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t c)
{
	int i, j;

	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			gfx_blend(i, j, c, 255);
}

void gfx_round_rect(float x, float y, float w, float h, float r, uint32_t c)
{
	float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - r, hy = h / 2 - r;
	int i, j;

	for (j = (int)y - 1; j <= (int)(y + h) + 1; j++)
		for (i = (int)x - 1; i <= (int)(x + w) + 1; i++) {
			float qx = fabsf(i + 0.5f - cx) - hx, qy = fabsf(j + 0.5f - cy) - hy;
			float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
			float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;

			gfx_blend(i, j, c, cov(d));
		}
}

/* the outline of a rounded rectangle, stroke w centred on the edge */
void gfx_round_rect_stroke(float x, float y, float w, float h, float r, float sw, uint32_t c)
{
	float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - r, hy = h / 2 - r;
	int i, j;

	for (j = (int)(y - sw) - 1; j <= (int)(y + h + sw) + 1; j++)
		for (i = (int)(x - sw) - 1; i <= (int)(x + w + sw) + 1; i++) {
			float qx = fabsf(i + 0.5f - cx) - hx, qy = fabsf(j + 0.5f - cy) - hy;
			float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
			float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;

			gfx_blend(i, j, c, cov(fabsf(d) - sw / 2));
		}
}

void gfx_circle(float cx, float cy, float r, uint32_t c)
{
	int i, j;

	for (j = (int)(cy - r) - 1; j <= (int)(cy + r) + 1; j++)
		for (i = (int)(cx - r) - 1; i <= (int)(cx + r) + 1; i++) {
			float dx = i + 0.5f - cx, dy = j + 0.5f - cy;

			gfx_blend(i, j, c, cov(sqrtf(dx * dx + dy * dy) - r));
		}
}

/* angles: degrees, 0 at 12 o'clock, clockwise */
static float ang_of(float dx, float dy)
{
	float a = atan2f(dx, -dy) * 180.0f / (float)M_PI;

	return a < 0 ? a + 360 : a;
}

void gfx_arc(float cx, float cy, float r, float w, float a0, float sweep, uint32_t c)
{
	float ro = r + w / 2 + 1, e0x, e0y, e1x, e1y, a1, hw = w / 2;
	int i, j, full = sweep >= 360;

	if (sweep <= 0)
		return;
	a0 = fmodf(a0, 360);
	if (a0 < 0)
		a0 += 360;
	a1 = a0 + sweep;
	e0x = cx + r * sinf(a0 * (float)M_PI / 180);
	e0y = cy - r * cosf(a0 * (float)M_PI / 180);
	e1x = cx + r * sinf(a1 * (float)M_PI / 180);
	e1y = cy - r * cosf(a1 * (float)M_PI / 180);
	for (j = (int)(cy - ro); j <= (int)(cy + ro); j++)
		for (i = (int)(cx - ro); i <= (int)(cx + ro); i++) {
			float dx = i + 0.5f - cx, dy = j + 0.5f - cy, d, a, rel;

			d = fabsf(sqrtf(dx * dx + dy * dy) - r) - hw;
			if (!full) {
				a = ang_of(dx, dy);
				rel = a - a0;
				if (rel < 0)
					rel += 360;
				if (rel > sweep) {
					/* outside the sweep: only the round caps */
					float d0x = i + 0.5f - e0x, d0y = j + 0.5f - e0y, d1x = i + 0.5f - e1x, d1y = j + 0.5f - e1y;

					d = fminf(sqrtf(d0x * d0x + d0y * d0y), sqrtf(d1x * d1x + d1y * d1y)) - hw;
				}
			}
			gfx_blend(i, j, c, cov(d));
		}
}

/* a filled triangle: the distance to the nearest edge, negative inside */
void gfx_triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
	float px[3] = {x0, x1, x2}, py[3] = {y0, y1, y2};
	int i, j, e;

	for (j = (int)fminf(y0, fminf(y1, y2)) - 1; j <= (int)fmaxf(y0, fmaxf(y1, y2)) + 1; j++)
		for (i = (int)fminf(x0, fminf(x1, x2)) - 1; i <= (int)fmaxf(x0, fmaxf(x1, x2)) + 1; i++) {
			float d = -1e9f, qx = i + 0.5f, qy = j + 0.5f;

			for (e = 0; e < 3; e++) {
				float ax = px[e], ay = py[e], bx = px[(e + 1) % 3], by = py[(e + 1) % 3];
				float ox3 = px[(e + 2) % 3], oy3 = py[(e + 2) % 3];
				float nx = by - ay, ny = ax - bx, l = sqrtf(nx * nx + ny * ny), de;

				/* the opposite corner is on the inside */
				if ((ox3 - ax) * nx + (oy3 - ay) * ny < 0) {
					nx = -nx;
					ny = -ny;
				}
				de = l > 0 ? ((qx - ax) * nx + (qy - ay) * ny) / l : 0;
				d = fmaxf(d, -de);
			}
			gfx_blend(i, j, c, cov(d));
		}
}

void gfx_line(float x0, float y0, float x1, float y1, float w, uint32_t c)
{
	float minx = fminf(x0, x1) - w, maxx = fmaxf(x0, x1) + w, miny = fminf(y0, y1) - w, maxy = fmaxf(y0, y1) + w;
	float vx = x1 - x0, vy = y1 - y0, ll = vx * vx + vy * vy;
	int i, j;

	for (j = (int)miny; j <= (int)maxy; j++)
		for (i = (int)minx; i <= (int)maxx; i++) {
			float px = i + 0.5f - x0, py = j + 0.5f - y0;
			float t = ll > 0 ? (px * vx + py * vy) / ll : 0, dx, dy;

			t = t < 0 ? 0 : t > 1 ? 1 : t;
			dx = px - t * vx;
			dy = py - t * vy;
			gfx_blend(i, j, c, cov(sqrtf(dx * dx + dy * dy) - w / 2));
		}
}

/* --- text: UTF-8, a cache of rendered glyphs */
struct glyph {
	int font, cp, size10;	/* size in tenths of a pixel */
	int w, h, xoff, yoff, adv;
	unsigned char *bm;
};
#define NCACHE 512
static struct glyph cache[NCACHE];

static const char *utf8(const char *s, int *cp)
{
	unsigned char c = *s;

	if (c < 0x80) {
		*cp = c;
		return s + 1;
	}
	if ((c & 0xe0) == 0xc0 && s[1]) {
		*cp = (c & 0x1f) << 6 | (s[1] & 0x3f);
		return s + 2;
	}
	if ((c & 0xf0) == 0xe0 && s[1] && s[2]) {
		*cp = (c & 0x0f) << 12 | (s[1] & 0x3f) << 6 | (s[2] & 0x3f);
		return s + 3;
	}
	if ((c & 0xf8) == 0xf0 && s[1] && s[2] && s[3]) {
		*cp = (c & 0x07) << 18 | (s[1] & 0x3f) << 12 | (s[2] & 0x3f) << 6 | (s[3] & 0x3f);
		return s + 4;
	}
	*cp = '?';
	return s + 1;
}

static struct glyph *glyph_get(int font, float px, int cp)
{
	int size10 = (int)(px * 10 + 0.5f);
	unsigned h = ((unsigned)cp * 31 + size10 * 7 + font) % NCACHE;
	struct glyph *g = &cache[h];
	stbtt_fontinfo *f = &fonts[font];
	float sc;
	int adv, lsb;

	if (g->bm && g->font == font && g->cp == cp && g->size10 == size10)
		return g;
	free(g->bm);
	sc = stbtt_ScaleForPixelHeight(f, px);
	g->bm = stbtt_GetCodepointBitmap(f, 0, sc, cp, &g->w, &g->h, &g->xoff, &g->yoff);
	if (!g->bm)
		g->bm = calloc(1, 1);
	stbtt_GetCodepointHMetrics(f, cp, &adv, &lsb);
	g->adv = (int)(adv * sc + 0.5f);
	g->font = font;
	g->cp = cp;
	g->size10 = size10;
	return g;
}

int gfx_text_width(int font, float px, const char *s)
{
	int w = 0, cp;

	while (*s) {
		s = utf8(s, &cp);
		w += glyph_get(font, px, cp)->adv;
	}
	return w;
}

int gfx_text(int font, float px, float x, float y, int align, uint32_t c, const char *s)
{
	int tw = gfx_text_width(font, px, s), cp, i, j;
	float pen = x;

	if (align == ALIGN_CENTER)
		pen = x - tw / 2.0f;
	else if (align == ALIGN_RIGHT)
		pen = x - tw;
	while (*s) {
		struct glyph *g;

		s = utf8(s, &cp);
		g = glyph_get(font, px, cp);
		for (j = 0; j < g->h; j++)
			for (i = 0; i < g->w; i++)
				gfx_blend((int)pen + g->xoff + i, (int)y + g->yoff + j, c, g->bm[j * g->w + i]);
		pen += g->adv;
	}
	return tw;
}
