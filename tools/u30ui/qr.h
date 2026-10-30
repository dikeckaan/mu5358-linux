/* qr: QR codes for the Wi-Fi page. SPDX-License-Identifier: MIT */
#ifndef QR_H
#define QR_H
#define QR_MAX 57	/* version 10 */
/* the modules (1 dark) of text into out[size*size]; returns size, 0 when it does not fit */
int qr_encode(const char *text, unsigned char *out);
void qr_wifi_payload(char *out, int n, const char *ssid, const char *psk);
#endif
