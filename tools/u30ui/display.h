/* display: the panel through DRM. SPDX-License-Identifier: MIT */
#ifndef DISPLAY_H
#define DISPLAY_H
/* FIRST, when given, draws the first frame (into gfx_buf) before the mode-set: the panel never shows a black frame
 * between the bootloader's logo and the UI. The panel's MADCTL turn (panelcmd) runs before the mode-set as well. */
int display_init(void (*first)(void));
/* the back buffer (gfx_buf) onto the panel */
void display_present(void);
void display_backlight(int on);
int display_is_on(void);
#endif
