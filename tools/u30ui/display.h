/* display: the panel through DRM. SPDX-License-Identifier: MIT */
#ifndef DISPLAY_H
#define DISPLAY_H
int display_init(void);
/* the back buffer (gfx_buf) onto the panel */
void display_present(void);
void display_backlight(int on);
int display_is_on(void);
#endif
