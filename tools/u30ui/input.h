/* input: the touch panel as gestures. SPDX-License-Identifier: MIT */
#ifndef INPUT_H
#define INPUT_H
enum { G_DOWN, G_MOVE, G_UP, G_TAP, G_LONG, G_SWIPE_LEFT, G_SWIPE_RIGHT, G_SWIPE_UP, G_SWIPE_DOWN };
struct gesture {
	int type, x, y, dx, dy;
};
long now_ms(void);
/* the touch device's fd (for poll), or -1 */
int input_open(void);
/* 1 and *g when a gesture happened */
int input_read(struct gesture *g);
int input_poll_long(struct gesture *g);
/* ms until a long press is due, -1 if none can be */
int input_ms_to_long(void);
int input_is_down(void);
#endif
