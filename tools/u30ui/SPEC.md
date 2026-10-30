# u30ui: the launcher's look, natively (from the U30's Android launcher sources, read 2026-10-30)

Numbers from the Kotlin sources (file:line there); screen 240x240, centre (120,120), round. Text: Roboto; "top y" means
the text's top, baseline = top + 0.93 * size.

## Papatya (paged home) - core/PapatyaLayout.kt, ui/PagedHome.kt, ui/PetalArt.kt
- 4 slots a page, centres (120,120) + (0,-80) top, (+80,0) right, (0,+80) bottom, (-80,0) left; PETAL_R 32, CENTER_R 42;
  hit: centre <= 42, slot <= 38.
- Petal: filled circle r32 in its colour (pressed: + 0x55000000), white ink (SCREEN: black). Icon box 22.4 at
  (cx, cy-6.4); label Medium 10.5 px, baseline cy+17.6, max 52.8 wide ("..." when longer).
- Colours: WIFI 0A84FF WiFi, SMS FF9F0A Mesaj, BATTERY 34C759 Pil, SETTINGS 8E8E93 Ayar, CLIENTS AF52DE Istemci,
  CALL 32ADE6, SCREEN FFD60A, HISTORY FF2D92, SIGNAL 30B0C7 Sinyal, QR 5E5CE6, VPN 64D2FF, APPS FF6482,
  POWER 8B0000 Guc, STATUS 0A84FF, SUMMARY 7D7AFF, SYSTEM A2845E Sistem, LOCK_WARN FF3B30.
- Icons: a 24x24 box scaled to the icon size, strokes 2.4 px round caps/joins; Android arc angles (0 = 3 o'clock,
  clockwise). Primitives per kind: see icons.c (copied 1:1 from PetalArt.kt:233-370).
- SMS badge at (cx+23.04, cy-23.04): BG round-rect, FF3B30 pill (half width max(9, textW/2+5), +-9), Medium 10 white.
- Page pills at (190.7,190.7) rotated -45 deg: 5 tall, active 12 wide FG, others 5 wide DIM2, gap 4.

## Centre disk - PetalArt.kt:388-445
- r42 MENU_BG 1C1C1E; battery ring stroke 5 at r39.5: TRACK full, fill from 12 o'clock clockwise 3.6*pct
  (>30 DOWN, >15 WARN, else ERR); clock "HH:MM" Medium 24, top y 100; signal bars (5 bars, 4 wide gap 3, heights
  4+3i, radius 1.5, scaled 0.7) at top 129 with network type 10 px DIM; "SIM yok" 10 px DIM at 129.

## Lock - Pager.kt, PageView.kt, LauncherActivity.kt
- Locked at start; a 600 ms press anywhere (no movement) unlocks; 120 s without touch relocks (and back to page 0).
- Locked: no petal or page acts; a touch flashes the padlock (500 ms) and shows "Basili tut" (1000 ms). Padlock at
  (190.7,190.7) in place of the pills (10 DIM2; 13 FG while flashing); other pages at (120,227), hint y 211.

## Status (Stacked) - ui/theme/StackedTheme.kt
- Clock 13 DIM2 top 26; operator 17 FG top 48; signal bars + "LTE  *  -95 dBm" 11 DIM at 72/74; download 34 px
  DOWN at 100 (down triangle), upload at 144 in UP; battery row at 186 (22x11 icon, pct 12 FG, estimate 10 DIM);
  bottom row at 206: clients 10 DIM.

## Pages - WifiPage, SmsPage, RingPage, SettingsPage, SystemPage, ActionsPage
- Title 10 DIM2 at y 14..22, hairline (1A1A1C) under it at y 30..40.
- Settings/lists: rows 36 px from y 34 (label 12 FG at x 44, value 11 DIM right at 196).
- Actions: rows at 56/96/136/176 (ring bullet r8 stroke 2 at x 52, label 14 FG at x 72); dangerous ones need a
  1 s hold, shown by an arc of stroke 3 at r114.5 from 12 o'clock; lifting or leaving cancels. Hint "1 sn basili tut".
- Battery ring: stroke 10 at r115; value 40 px at 46; status 12 DIM at 92.

## Gestures - Pager.kt
- Horizontal drag moves the pages with the finger; release past 24 px changes page (180 ms ease-out), ends damped
  0.35; no wrap. A swipe down (>= 24 px) is "back" on detail pages; the centre opens Status.
