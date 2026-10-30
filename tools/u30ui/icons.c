/*
 * icons: the launcher's petal icons (PetalArt.kt:233-370), in its 24x24 box scaled to the size asked for, strokes
 * 2.4 px with round ends. Arcs are given in Android's angles (0 = 3 o'clock, clockwise), as in the original.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include "gfx.h"
#include "icons.h"

static float k, ox, oy;	/* box scale and top-left */
static uint32_t ink;
#define SW 2.4f
#define X(v) (ox + (v) * k)
#define Y(v) (oy + (v) * k)

static void L(float x0, float y0, float x1, float y1)
{
	gfx_line(X(x0), Y(y0), X(x1), Y(y1), SW, ink);
}

/* Android angles to gfx's (0 = 12 o'clock) */
static void A(float cx, float cy, float r, float start, float sweep)
{
	gfx_arc(X(cx), Y(cy), r * k, SW, start + 90, sweep, ink);
}

static void ring(float cx, float cy, float r)
{
	gfx_arc(X(cx), Y(cy), r * k, SW, 0, 360, ink);
}

static void dot(float cx, float cy, float r)
{
	gfx_circle(X(cx), Y(cy), r * k, ink);
}

static void RR(float x0, float y0, float x1, float y1, float r)
{
	gfx_round_rect_stroke(X(x0), Y(y0), (x1 - x0) * k, (y1 - y0) * k, r * k, SW, ink);
}

/* curves as short lines */
static float px, py;
static void M(float x, float y)
{
	px = x;
	py = y;
}

static void LT(float x, float y)
{
	L(px, py, x, y);
	px = x;
	py = y;
}

static void C(float x1, float y1, float x2, float y2, float x, float y)
{
	int i;
	float lx = px, ly = py;

	for (i = 1; i <= 12; i++) {
		float t = i / 12.0f, u = 1 - t;
		float nx = u * u * u * px + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x;
		float ny = u * u * u * py + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y;

		L(lx, ly, nx, ny);
		lx = nx;
		ly = ny;
	}
	px = x;
	py = y;
}

static void Q(float x1, float y1, float x, float y)
{
	C(px + 2.0f / 3 * (x1 - px), py + 2.0f / 3 * (y1 - py), x + 2.0f / 3 * (x1 - x), y + 2.0f / 3 * (y1 - y), x, y);
}

void icon_draw(int kind, float cx, float cy, float size, uint32_t colour)
{
	k = size / 24;
	ox = cx - size / 2;
	oy = cy - size / 2;
	ink = colour;
	switch (kind) {
	case IC_WIFI:
		A(12, 20.2f, 15, -131.8f, 83.6f);
		A(12, 20.1f, 10, -130.5f, 81);
		A(12, 20, 5, -126.9f, 73.8f);
		dot(12, 19.5f, 1.2f);
		break;
	case IC_SMS:
		RR(3, 5, 21, 19, 3);
		M(3.5f, 6.5f);
		LT(12, 13.5f);
		LT(20.5f, 6.5f);
		break;
	case IC_BATTERY:
		RR(3, 7, 19, 17, 2.5f);
		L(21, 10.5f, 21, 13.5f);
		break;
	case IC_SETTINGS:
		L(4, 7, 13.8f, 7);
		L(18.2f, 7, 20, 7);
		L(4, 17, 5.8f, 17);
		L(10.2f, 17, 20, 17);
		ring(16, 7, 2.2f);
		ring(8, 17, 2.2f);
		break;
	case IC_CLIENTS:
		ring(9, 8, 3);
		M(3, 19);
		C(3, 16, 6, 14, 9, 14);
		C(12, 14, 15, 16, 15, 19);
		M(18, 14);
		C(20, 14.6f, 21, 16.3f, 21, 19);
		A(16, 8, 3, -90, 180);
		break;
	case IC_SIGNAL:
		L(5, 16, 5, 20);
		L(10, 12, 10, 20);
		L(15, 8, 15, 20);
		L(20, 4, 20, 20);
		break;
	case IC_POWER:
		L(12, 4, 12, 12);
		A(12, 11.9f, 7, -44.4f, 268.8f);
		break;
	case IC_STATUS:
		ring(12, 12, 9);
		L(12, 11, 12, 16.5f);
		dot(12, 7.8f, 1.2f);
		break;
	case IC_SUMMARY:
		L(5, 7, 19, 7);
		L(5, 12, 19, 12);
		L(5, 17, 14, 17);
		break;
	case IC_SYSTEM:
		RR(7, 7, 17, 17, 2);
		L(10, 3, 10, 7);
		L(14, 3, 14, 7);
		L(10, 17, 10, 21);
		L(14, 17, 14, 21);
		L(3, 10, 7, 10);
		L(3, 14, 7, 14);
		L(17, 10, 21, 10);
		L(17, 14, 21, 14);
		break;
	case IC_SCREEN:
		L(12, 3, 12, 6);
		L(12, 18, 12, 21);
		L(3, 12, 6, 12);
		L(18, 12, 21, 12);
		ring(12, 12, 4);
		break;
	case IC_HISTORY:
		A(12, 12, 9, 0, 270);
		M(12, 7);
		LT(12, 12);
		LT(15, 14);
		break;
	case IC_QR:
		RR(4, 4, 20, 20, 3);
		RR(8, 8, 11, 11, 0.5f);
		RR(13, 13, 16, 16, 0.5f);
		break;
	case IC_VPN:
		M(12, 3);
		LT(19, 6);
		LT(19, 11);
		C(19, 15.5f, 16, 19, 12, 21);
		C(8, 19, 5, 15.5f, 5, 11);
		LT(5, 6);
		LT(12, 3);
		break;
	case IC_CALL:
		M(5, 4);
		LT(9, 4);
		LT(11, 9);
		LT(8, 11);
		Q(9.6f, 14.4f, 13, 16);
		LT(15, 13);
		LT(20, 15);
		LT(20, 19);
		Q(20, 21, 18, 21);
		C(9.5f, 20.5f, 3.5f, 14.5f, 3, 6);
		Q(3, 4, 5, 4);
		break;
	case IC_APPS:
		RR(4, 4, 11, 11, 2);
		RR(13, 4, 20, 11, 2);
		RR(4, 13, 11, 20, 2);
		RR(13, 13, 20, 20, 2);
		break;
	case IC_CLOCK:
		ring(12, 12, 9);
		M(12, 6.5f);
		LT(12, 12);
		LT(16, 12);
		break;
	case IC_LOCK:
		RR(5, 10, 19, 20, 2);
		M(8, 10);
		LT(8, 7);
		A(12, 7, 4, 180, 180);
		M(16, 7);
		LT(16, 10);
		break;
	case IC_PLUS:
		L(12, 5, 12, 19);
		L(5, 12, 19, 12);
		break;
	}
}

/* the launcher's padlock (Geom.padlock): shackle the top half of an ellipse, body a round-rect, stroke 1.5 */
void icon_padlock(float cx, float cy, float s, uint32_t c)
{
	float x = cx - s / 2, y = cy - s / 2;

	gfx_arc(x + 0.5f * s, y + 0.45f * s, 0.28f * s, 1.5f, 270, 180, c);
	gfx_round_rect_stroke(x, y + 0.45f * s, s, 0.55f * s, 1.5f, 1.5f, c);
}
