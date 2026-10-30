# u30ui2: the dashboard

The U30 Pro's 240x240 round screen as a router's control panel, in C on DRM, in English, Turkish and Simplified
Chinese. The default screen of MU5358 Linux; the launcher-look `u30ui` (`tools/u30ui`) stays as the "classic"
interface, and either one's settings page switches to the other (`/etc/mu5358/ui`).

| | |
|---|---|
| Binary | one static aarch64 executable, 1.6 MB (Roboto and a 270-glyph subset of Noto Sans SC inside) |
| Memory | ~6 MB resident, three threads |
| CPU | 0.5 % with the screen on and the home page's live graph (measured 2026-10-30: 31 ticks in 60 s); nothing is drawn while the screen is off, and the data threads slow to one pass in 30 s |
| Processes | none for the common readings (sysfs/procfs, ioctl); `iw` every 10 s, `mu5358-at` every 15 s, `uci`/`sms`/`mu5358-traffic` every 30 s |

## Pages (swipe sideways; a swipe down goes home or closes what is open)

1. **Home** - operator, type/band/RSRP, the last minute's throughput as a graph, the two rates, clients and today's data
2. **Network** - the signal's last 10 minutes, operator, band + EARFCN, cell + PCI, RSRP/RSRQ, IP, APN, the mobile data switch
3. **Wi-Fi** - SSID, band/channel/width, clients, the password (tap to show), the hotspot switch, the QR code to join
4. **Clients** - every station: name or IP, MAC, signal, bytes down/up
5. **Traffic** - the graph, today, this billing cycle, since boot (mu300-traffic's counters)
6. **Messages** - the SIM's SMS; a tap opens one
7. **Battery** - the ring, state, current/voltage/temperature, a rough estimate
8. **System** - a bar per core with the clusters' clocks, memory, storage, four temperatures, load and uptime, versions
9. **Settings** - language, screen timeout, brightness, lock, interface
10. **Power** - restart (to Android), power off, restart the modem, restart Wi-Fi, screen off; the first four want a 1 s hold
11. **Log** - the system log

Locked at start (hold to unlock, as the launcher does); 2 minutes without a touch relock it; the power key switches
the screen, and the timeout switches it off.

## Files

| | |
|---|---|
| `ui.c` | the pages, gestures and the main loop |
| `data.c`, `data.h` | the readings, in threads of their own, with the throughput and signal histories |
| `i18n.h`, `i18n.c` | every string in the three languages; `/etc/mu5358/lang` |
| `mkfont.sh`, `font_cjk.h` | the Chinese glyphs: Noto Sans SC (OFL, `third_party/`) cut down to the characters `i18n.h` uses |
| `preview.c`, `preview/` | every page in every language as PNG files, with sample data (`build.sh preview`) |
| `build.sh` | the static binary (`gfx`, `icons`, `display`, `input`, `png`, `qr` are `tools/u30ui`'s) |

Testing without a finger, as `u30ui`: `kill -USR1` writes the frame to `/tmp/u30ui.png`; `kill -USR2` runs the
gestures in `/tmp/u30ui.cmd` (`tap X Y`, `long X Y`, `swipe left|right|up|down`, `lang en|tr|zh`, `screen on`).
