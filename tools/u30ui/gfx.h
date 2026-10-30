/*
 * gfx: drawing into the 240x240 back buffer (0x00RRGGBB), anti-aliased, and Roboto text (stb_truetype).
 * SPDX-License-Identifier: MIT
 */
#ifndef GFX_H
#define GFX_H
#include <stdint.h>

#define W 240
#define H 240

extern uint32_t gfx_buf[W * H];
/* added to every x drawn: a page dragged sideways */
extern int gfx_ox;

/* the launcher's colours (Palette.kt, dark set) */
#define C_BG 0x000000
#define C_FG 0xffffff
#define C_DIM 0x8a8a8e
#define C_DIM2 0x5a5a5f
#define C_DOWN 0x34c759
#define C_UP 0x0a84ff
#define C_WARN 0xff9f0a
#define C_ERR 0xff453a
#define C_TRACK 0x26262a
#define C_HAIRLINE 0x1a1a1c
#define C_MENU_BG 0x1c1c1e

enum { FONT_REGULAR, FONT_MEDIUM };
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

void gfx_init(void);
void gfx_clear(uint32_t c);
/* alpha 0..255 over what is there */
void gfx_blend(int x, int y, uint32_t c, int a);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t c);
void gfx_round_rect(float x, float y, float w, float h, float r, uint32_t c);
void gfx_round_rect_stroke(float x, float y, float w, float h, float r, float sw, uint32_t c);
void gfx_circle(float cx, float cy, float r, uint32_t c);
/* a ring of width w at radius r (the stroke's centre), from a0 over sweep degrees (0 = 12 o'clock, clockwise) */
void gfx_arc(float cx, float cy, float r, float w, float a0, float sweep, uint32_t c);
void gfx_line(float x0, float y0, float x1, float y1, float w, uint32_t c);
void gfx_triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c);
/* text at baseline y; returns its width */
int gfx_text(int font, float px, float x, float y, int align, uint32_t c, const char *s);
int gfx_text_width(int font, float px, const char *s);
/* the screen is round: 1 inside the circle that shows */
int gfx_visible(int x, int y);
#endif
