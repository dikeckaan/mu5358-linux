# u30ui - the U30 Pro's screen under Linux

The papatya of the U30's Android launcher (a Kotlin app of the author's), natively on DRM: one static
aarch64 binary of 1.4 MB (Roboto inside), 5 MB of RAM, no CPU while nothing changes, nothing drawn while the screen is
off. The look follows SPEC.md (numbers from the launcher's sources).

| | |
|---|---|
| Home | papatya: 4 petals a page, 3 pages (WiFi, Mesaj, Pil, Ayar / İstemci, Sinyal, Güç, Sistem / Konsol); centre disk with the clock, battery ring, signal and network |
| Status | tap the centre: operator, signal, download/upload rate, battery, clients |
| Pages | WiFi (SSID, password, clients; a tap shows the QR code to join), Mesaj (the SIM's SMS via mu300's `sms`; a tap opens one), Pil, Ayar (screen timeout, lock, hotspot, mobile data), İstemci, Sinyal (RSRP/RSRQ, wan address, traffic), Güç (restart, power off, data: 1 s hold), Sistem (CPU, RAM, storage, uptime, SoC temperature), Konsol (the system log) |
| Lock | locked at start and after 120 s without a touch; a 600 ms press opens it (Ayar: Kilit Kapalı turns it off) |
| Gestures | tap, long press, horizontal drag between home pages, swipe down = back, swipe up/down scrolls lists |
| Screen | off after the timeout (Ayar: 30 s / 1 / 5 min / never); the power key (mu5358-buttons) turns it on and off |

Settings: /etc/u30ui.conf. Started by /etc/init.d/mu5358-screen (S11), which falls back to screen-console (the log)
when u30ui is missing.

## Tests without a finger

    kill -USR1 $(pidof u30ui)        # the frame, as drawn now, into /tmp/u30ui.png (also with the screen off)
    echo "long 120 120" > /tmp/u30ui.cmd; kill -USR2 $(pidof u30ui)    # gestures: tap X Y, long X Y,
                                                                        # swipe left|right|up|down, screen on

## Build

    docker run --rm -v "$PWD/tools/u30ui":/w mu5358-mainline-build bash /w/build.sh            # u30ui
    docker run --rm -v "$PWD/tools/u30ui":/w mu5358-mainline-build bash /w/build.sh preview    # preview/*.png

font_*.h are third_party/Roboto-*.ttf (Apache-2.0) as C arrays; third_party/stb_truetype.h is public domain.

| File | |
|---|---|
| ui.c | pages, gestures, lock, screen timeout, main loop |
| gfx.c | anti-aliased shapes (distance fields) and text (stb_truetype, glyph cache) |
| icons.c | the launcher's 24x24 petal icons |
| display.c | DRM: mode, two dumb buffers, PAGE_FLIP and its event, backlight (bl_power), the panel's 180 degrees (by the panel's MADCTL through tools/spispeed/panelcmd.ko, again after an ESD recovery; else by the copy; `U30UI_PANEL_ROT=0` forces the copy) |
| input.c | the touch panel (evdev) as gestures |
| data.c | the board's state in a thread (sysfs 2 s, modem 15 s, SMS 30 s, logread -f) |
| png.c, preview.c | screenshots and the host preview |
