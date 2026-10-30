/* icons: the launcher's petal icons. SPDX-License-Identifier: MIT */
#ifndef ICONS_H
#define ICONS_H
#include <stdint.h>
enum {
	IC_WIFI, IC_SMS, IC_BATTERY, IC_SETTINGS, IC_CLIENTS, IC_SIGNAL, IC_POWER, IC_STATUS, IC_SUMMARY, IC_SYSTEM,
	IC_SCREEN, IC_HISTORY, IC_QR, IC_VPN, IC_CALL, IC_APPS, IC_CLOCK, IC_LOCK, IC_PLUS
};
void icon_draw(int kind, float cx, float cy, float size, uint32_t colour);
void icon_padlock(float cx, float cy, float s, uint32_t c);
#endif
