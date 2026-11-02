# Updates from inside Linux (W2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A running MU5358 Linux updates itself to the next signed release (`mu5358-update apply`, the LuCI Update page), keeps the user's files and the flash-drive image, can roll back, and the Android-side installer keeps working on the same layout.

**Architecture:** The one ext4 (region or fulldumpdb) gains mu300's layout: `system/` (the tree), `system.old/`, `data/` (bind-mounted at `/data`: the USB disk image, `prev.*` boot images, logs), `boot/` (`tree`, `tries`, `ok`, `installed`). `boot/init` mounts the filesystem at `/disk`, picks the tree, bind-mounts it as the new root and falls back to `system.old` after three boots without `ok`. `mu5358-update` is mu300-update (4810 lines, the R&D is done there) with the kernel/slot/misc/next-boot code removed and our slot-b boot-image writing (the installer's `bootimg_put`, `write_part`) put in; it unpacks the release's `mu5358-system.tar.gz` into `system.new`, carries the user's files with `mu5358-carry` (mu300's static tool, already in the base rootfs), swaps the trees, writes `boot_b`/`init_boot_b` with `prev.*` kept in `data/`. The Android installer's `update` unpacks the same tarball (no more whole-image write) and migrates a flat install to the layout. The LuCI page, adapter, rpcd methods, backup, user-packages and sysupgrade guard are already in the rebranded base tree: they are re-enabled and patched with sed guards.

**Tech Stack:** POSIX sh (busybox ash on the device, mksh/toybox under Android for the installer), awk, mu5358-carry (C, static, from the base rootfs), procd, LuCI JS, Python unittest, docker busybox for the installer's helpers, Node for the page.

**Spec:** `docs/superpowers/specs/2026-11-01-mu300-parity-usb-charging-design.md`, section "W2 Updates and the panel around them" (and "Constraints that do not move", "Decisions made here", "Left to the builder").

## Global Constraints

- Linux boots only as the one-shot trial. Nothing here writes misc (not even `--rearm`), slot a, the NV partitions or the GPT; `tests/test_static.py` enforces it on every script (new scripts get a shebang so they are found). After an update from Linux the restart goes to Android and the user starts Linux again from the page (`boot-linux`): the page/panel texts say so.
- The bench rules: full backup first (exists), never cut USB without a self-undo, never read the VPN config, no SIM data without the maintainer's go-ahead (the update check over the SIM is on by default per the spec; on the bench the Mac's NCM link is the uplink).
- The ramdisk's `init` and the kernel are unchanged except the tree-select block in `boot/init`.
- Every `sed -i` on the base tree in `openwrt/customize.sh` has a loud grep guard and an idempotence marker (`SedGuards` test). Screen strings only through `tools/u30ui2/i18n.h` + `mkfont.sh`.
- State files stay under `/etc/mu5358/` inside the tree; the carry's "user area" (etc, root, home, var...) keeps them across a Linux-side update, and the installer's `CARRY` list keeps them across an Android-side one. Both lists hold the same names (`test_carry_lists_agree`).
- Tests: `cd tests && python3 -m unittest discover -s . -p 'test_*.py'` (142 before this plan) and `node --test tests/web/*.mjs`. Hard rule tests must stay green after every task.
- Measured facts: the base rootfs has `usign`, `openssl`, `flock`, `jq`, `curl`, `tar`, `gzip`, `sha256sum`, `setsid`, no `cpio`, no `lz4`; the release's ramdisk busybox has `cpio`; the kernel wants the init_boot ramdisk LZ4-legacy (magic `02214c18`); the region on the bench holds 27 GiB, fulldumpdb 2 GiB; the tree is ~300 MB.

## Review Focus

1. A power cut between the tree swap and the boot-image write: the next boot runs the new tree on the old kernel/ramdisk (both are the same release family; init is self-contained) and `mu5358-update status` says `boot.pending` — pinned by `test_apply_power_cut_between_swap_and_boot`.
2. A flat install (v2026.10.30.x…v2026.11.01.5) updated from Android: `data/` created, `usb-disk.img` moved into it, the old tree becomes `system/` first so an interrupted migration still boots — `MigrateFlat` tests.
3. `boot/init` on a filesystem with `system/` missing but `system.old/` present (a failed swap): boots `system.old`, logs `stage=tree-fallback` — `TreeSelect.test_missing_tree_falls_back`.
4. A SHA256SUMS whose `mu5358-release` line names another tag (a replayed old release served by a mirror): refused before any download — `test_sums_for_another_tag_refused`.
5. The Android installer's `boot-linux` after a Linux-side update: `boot_b` differs from `/data/adb/mu5358/installed` — `installed()` reads the region's `boot/installed` first — `test_boot_linux_reads_the_regions_record`.

---

## File map

| file | responsibility |
|---|---|
| `openwrt/build-rootfs.sh` (modify) | stage `system/ data/ boot/tree` into the image; write `etc/mu5358/image-manifest.gz` after customize; emit `mu5358-system.tar.gz` |
| `tools/make-release.sh` (modify) | the tarball, `mu5358-update`, `mu5358-release` (tag + newline) in the release and in SHA256SUMS |
| `tools/ci-release.sh`, `tools/make-release-mirror.sh` (modify) | asset count 8 → 11; mirror FILES |
| `tools/lz4/build.sh` (new) | static aarch64 `lz4` (pinned source tarball) → `/opt/mu5358/bin/lz4` |
| `boot/init` (modify) | `# --- tree-select begin/end`: mount at `/disk`, pick the tree, tries/fallback, bind mounts, publish `/run/mu5358/ramdisk/` |
| `openwrt/update/mu5358-update` (new) | the updater (mu300-update, trimmed, plus boot images) |
| `openwrt/update/mu5358-boot-ok.init`, `mu5358-update-notice.init` (new) | `boot/ok` writer (S99); the notice timer (S99) |
| `openwrt/update/update-page.sed` (new, applied by customize.sh with guards) | update.js: no kernel choice, no "Start Android", trial restart text |
| `openwrt/customize.sh` (modify) | install the above, patch the adapter's `stage_of`, re-add the Update menu entry, `lz4` |
| `installer/mu5358-install.sh` (modify) | layout-aware `system()`/`vendor_files()`/`carry_*`; `update` by tarball + migration; `installed()` from the region; `status` gains `layout` |
| `site/assets/web-installer/{github,device,flow,app,strings}.js` (modify) | the new assets; update pushes the tarball; steps `migrate`, `carry` |
| `tests/test_update.py`, `tests/test_tree_select.py`, `tests/test_installer.py` (+MigrateFlat), `tests/test_static.py` (+Update), `tests/web/flow.test.mjs` | tests |
| `docs/GKI-SYSTEM.md`, `README.md`, `docs/MU300-PARITY.md` | docs |

### Layout (shared by every task)

```
<fs>/system/            the tree (sbin/init, etc/mu5358/version, etc/mu5358/image-manifest.gz)
<fs>/system.old/        the previous tree (rollback); system.new/ only during an update
<fs>/data/              bind-mounted at /data in the tree: usb-disk.img, prev/boot_b.img, prev/init_boot_b.img, prev/installed
<fs>/boot/tree          "system" (default when absent); a name with '.' or '/' is ignored
<fs>/boot/tries         boots of the named tree without boot/ok (init adds 1; boot-ok writes 0)
<fs>/boot/ok            the version that reached boot-ok last
<fs>/boot/installed     key=value: version date target boot_b boot_b_head init_boot_b ramdisk (sha of mu5358-ramdisk.tar.gz)
<fs>/.mu5358-update/    the updater's stage (700)
<fs>/lost+found
```
The whole filesystem is visible in the tree at `/mnt/mu5358-disk` (bind). `boot/init` publishes `/run/mu5358/ramdisk/{busybox,misc-bc-android.bin,modules/}` (copies of its own `/mu`) before switch_root, so the updater can rebuild the ramdisk.

### Release (shared)

Assets: the 8 of today + `mu5358-system.tar.gz` (the tree, numeric owners, `--sort=name --mtime=@0`), `mu5358-update`, `mu5358-release`. SHA256SUMS gains their lines; `# version TAG` stays. The manifest inside the tree is `etc/mu5358/image-manifest.gz` (`mu5358-carry manifest ROOT openwrt-luci TAG`, the image name kept as `openwrt-luci` so the base tree's carry classes apply unchanged).

---

### Task 1: The release carries the tree, the updater and the tag-binding line

**Files:** Modify `openwrt/build-rootfs.sh`, `tools/make-release.sh`, `tools/ci-release.sh`, `tools/make-release-mirror.sh`, `site/assets/web-installer/github.js`; Test `tests/test_static.py` (class `Update`), `tests/web/mirror.test.mjs`.

**Interfaces:** Produces `mu5358-system.tar.gz` (paths `./sbin/init`…), `mu5358-release` (= `TAG\n`), `mu5358-update` as assets; SHA256SUMS lists all three. `github.js` `REQUIRED_FILES` + `OPTIONAL_FILES` include them.

- [ ] Step 1: tests in `tests/test_static.py` class `Update`: `test_release_lists_the_tree_the_updater_and_the_tag_line` (make-release.sh's sha256sum line names `mu5358-system.tar.gz mu5358-update mu5358-release`; ci-release.sh's count is 11; make-release-mirror.sh FILES has the three), `test_the_page_knows_the_new_assets` (github.js). Run: expect FAIL.
- [ ] Step 2: build-rootfs.sh: after customize and the catalogs, `mu5358-carry manifest "$R" openwrt-luci "$VERSION" | gzip -9n > "$R/etc/mu5358/image-manifest.gz"` (the tree's own `/opt/mu5358/bin/mu5358-carry` runs in the container: aarch64 on CI's arm runner and on the Mac's docker; guard with `[ -x ]`, else fail loudly); `tar --numeric-owner --owner=0 --group=0 --sort=name --mtime=@0 -C "$R" -czf /w/work/openwrt/mu5358-system.tar.gz .`; then stage `S=/build/stage; mkdir -p $S/system $S/data $S/boot; mv $R/* $R/.[!.]* $S/system/; echo system > $S/boot/tree; mke2fs -d $S`. make-release.sh: move the tarball to `$OUT`, `cp openwrt/update/mu5358-update "$OUT/"`, `printf '%s\n' "$TAG" > "$OUT/mu5358-release"`, extend the sha256sum line. ci-release.sh: 11. make-release-mirror.sh FILES. github.js lists.
- [ ] Step 3: run the two tests and the whole suite: OK. Commit `Release: the system tree, the updater and the tag-binding line as assets`.

### Task 2: The image has the layout and the Android installer installs into it

**Files:** Modify `installer/mu5358-install.sh` (`system()`, `vendor_files()`, `carry_save/restore`, `status`); Test `tests/test_installer.py` class `Layout`.

**Interfaces:** `tree_dir MNT` prints `$MNT/system` when `$MNT/system/sbin/init` exists, else `$MNT` (flat); `layout MNT` prints `tree|flat`. `status` JSON gains `"layout":"tree|flat|none"`.

- [ ] Step 1: tests (busybox docker, `_test` entry): `test_tree_dir_prefers_system`, `test_layout_flat_when_init_is_at_the_top`, `test_status_json_has_layout` (static: the printf). Expect FAIL.
- [ ] Step 2: implement; every `$MNT/etc|lib|opt|root` write in vendor_files/carry_restore/system() goes through `$(tree_dir $MNT)`; the `openwrt_release` check too. `update()` keeps its whole-image path for now (Task 7 replaces it).
- [ ] Step 3: suite OK; commit `Installer: the system tree lives in system/ (layout-aware paths)`.

### Task 3: boot/init picks the tree

**Files:** Modify `boot/init` (block `# --- tree-select begin` … `end` between `stage=root-mounted` and the init choice; the `/newroot/...` reads/writes after it use `$NEWROOT_TREE`); Test `tests/test_tree_select.py` (new; extracts the block like `test_usb.py` `InitApply`, runs it under sh with `DISK=<tmpdir>` and a `mount` stub that records bind calls).

**Interfaces:** Block input: `DISK` (the mounted filesystem), `NEWROOT=/newroot`, `log()`. Output: `TREE` (the directory booted), bind mounts `mount --bind $DISK/$TREE /newroot`, `mount --bind $DISK /newroot/mnt/mu5358-disk`, `mount --bind $DISK/data /newroot/data` (mkdir -p both), `boot/tries` incremented (+`sync`), `/run/mu5358/ramdisk/` published from `/mu`. Rules: name from `boot/tree` (default `system`; `.` or `/` in it → ignored, log `stage=tree-name-ignored`); a tree needs `sbin/init` and `etc/mu5358/version`; `tries` ≥ 3 and `boot/ok` ≠ the tree's version → `system.old` if usable (log `stage=tree-fallback tries=N`); the named tree missing/unusable → `system.old` (log `stage=tree-fallback missing`); neither but `$DISK/sbin/init` → flat (log `stage=tree-flat`); nothing → `stage=root-has-no-init`.

- [ ] Step 1: tests: `test_default_tree_is_system`, `test_tries_count_up_and_three_without_ok_fall_back`, `test_missing_tree_falls_back`, `test_bad_name_ignored`, `test_flat_install_still_boots`, `test_data_and_disk_are_bind_mounted`, `test_ramdisk_files_published`. Expect FAIL (no block).
- [ ] Step 2: implement the block; mount the filesystem at `/disk` (not `/newroot`) and bind as above; `/newroot/mu5358-stay`, `usb_apply`, fsck.log copy and `persist-log` write use the tree.
- [ ] Step 3: suite OK (test_usb's InitApply still finds its block); commit `boot/init: picks system/ (or system.old after three boots without ok), binds data/`.

### Task 4: boot-ok and the notice timer

**Files:** Create `openwrt/update/mu5358-boot-ok.init` (procd, START=99, `sleep 30` after start; writes `boot/ok` = `/etc/mu5358/version`, `boot/tries` = 0, `sync`), `openwrt/update/mu5358-update-notice.init` (START=99; `sleep 300`, then loop: `mu5358-update notice`, 21600 s after success / 3600 s after failure; exits at once when `/etc/mu5358/update-check` says `off`); Modify `openwrt/customize.sh` (install both, S99 links, `/etc/mu5358/update-check` default `on`, CARRY in the installer gets `etc/mu5358/update-check`); Test `tests/test_update.py` class `BootOk` (runs the init's `boot_ok` function with `MU5358_DISK=<tmp>`), `tests/test_static.py` Usb-style check that CARRY has `update-check`.

- [ ] Step 1: tests `test_boot_ok_writes_ok_and_zeroes_tries`, `test_notice_loop_respects_off` (the loop body as a function with `sleep` stubbed). FAIL.
- [ ] Step 2: implement; the inits source nothing from mu300-post (deleted by rebrand). Commit `boot-ok and the update notice timer`.

### Task 5: mu5358-update (the port)

**Files:** Create `openwrt/update/mu5358-update` (from `/Volumes/TasinabilirSSD/projects/mu300-linux/rootfs/overlay/opt/mu300/bin/mu300-update`, `s/mu300/mu5358/g; s/MU300/MU5358/g`); Modify `openwrt/customize.sh` (copy over the rebrand stub at `/opt/mu5358/bin/mu5358-update`, 755); Test `tests/test_update.py` (new; `MU5358_LIB=1 . mu5358-update` under `sh`, `MU5358_DISK=<tmp>/disk`, fake release dir served by `MU5358_RELEASE_URL=file://…`? — mu300's tests use a stub `curl` on PATH that copies from a directory; port that stub).

**Interfaces (kept from mu300):** `check`, `version`, `apply [--no-rollback] [--from FILE|DIR [--sha256=H] [--unsigned]] [--downgrade]`, `report`, `rollback`, `rollback-boot`, `clean`, `notice`, `source [URL|off]`, `status [--sizes]`, `releases`, `inspect`, internal `carry OLD NEW OS copy|link`. Env: `MU5358_REPO=dikeckaan/mu5358-linux`, `MU5358_DISK=/mnt/mu5358-disk`, `MU5358_RELEASE`, `MU5358_RELEASE_URL`, `MU5358_DOWNLOAD_URL`, `MU5358_API_URL`, `MU5358_LOCK=/run/mu5358-update`, `MU5358_SYSROOT`, `MU5358_BIN`, `MU5358_NOTICE=/run/mu5358/update-available`, `MU5358_PROGRESS`, `MU5358_ALLOW_UNSIGNED`, `MU5358_ALLOW_DOWNGRADE`, `MU5358_SELF_UPDATED`. Systems: exactly one, `system` (`SYSTEMS=system`, asset `mu5358-system.tar.gz`, image name `openwrt-luci` in the manifest header). Exit: 1 die, 2 usage, fetch 2/3, carry room 3.

**Removed:** `kernel` command and bundles, `unlock_for_trial`/`mu5358-next-boot`, `linux_slot`, the storage block (`part_dev` by misc), `write_boot`/`boot_update_run` (AVB v4 writer), `rollback_boot` (replaced), LK/misc reads in `status_json` (`slot`, `boot_default`, `boot_attempts`, `boot_locked`), the Ubuntu/apt-only functions where they are separate functions (`accounts` stays: it is used by carry), `extras`/VPN module fetch (W9 later; keep the code path but with an empty list).

**Added (our boot step, `# --- boot-images begin/end`):** `boot_update TAG`: needs `/proc/cmdline` with `androidboot.slot_suffix=_b` (else die "Linux is not running from slot b"); `prev/` copies of `boot_b` and `init_boot_b` into `$DISK/data/prev/` (sha256 read-back) before any write; kernel: `gzip -dc mu5358-kernel.Image.gz`, arm64 magic at 56, `bootimg_put $(dd of boot_a) Image kernel`; ramdisk: untar `mu5358-ramdisk.tar.gz`, add `/run/mu5358/ramdisk/misc-bc-android.bin` and `modules/*.ko`, `mknod dev/console`, `find init mu dev | /run/mu5358/ramdisk/busybox cpio -o -H newc`, `lz4 -l -9` (magic check `02214c18`, `lz4 -d` round trip), `bootimg_put init_boot_a rd.lz4 ramdisk`; `write_part` both (dd conv=fsync, drop_caches, sha256 read-back); write `$DISK/boot/installed`. `rollback_boot`: `data/prev/*` back with read-back, `prev/installed` → `boot/installed`. `bootimg_put`, `write_part`, `u32/u64be/le32/be64/put/fsize/sha` copied from the installer verbatim (test: both files hold the same text between `# ---- bootimg begin/end` markers, `test_bootimg_helpers_are_the_installers`). `reboot_goes_to_android` returns true always (the trial): the "Reboot to start it" texts become "Restart: the phone starts Android; start Linux again from the web installer (Restart into Linux once)".

**Tree swap (`apply_one`)**: `system.new` ← tar; sanity `sbin/init`; `carry_run`; `.mu5358-complete`; `mv system system.old` (after `rm -rf system.old`); `mv system.new system`; `sync`; `boot/tries`=0, `boot/ok` cleared. `rollback`: the reverse. `status_json` gains `"layout":"tree|flat"`, `"tree":"system"`, `"boot":{"installed":TAG,"pending":bool}` (pending = `boot/installed` version ≠ `system/etc/mu5358/version`).

- [ ] Step 1: tests (port from mu300's test_update.py the ones that still apply, renamed): `test_sums_for_another_tag_refused`, `test_tag_line_must_be_present_when_signed`, `test_pinned_sums_change_stops_the_run`, `test_lock_is_held_once`, `test_apply_swaps_the_trees_and_keeps_old`, `test_apply_refuses_a_flat_install`, `test_apply_refuses_downgrade_without_flag`, `test_rollback_swaps_back`, `test_apply_power_cut_between_swap_and_boot` (boot step made to fail: `status` says pending, `system/` is the new tree), `test_boot_update_refuses_slot_a_cmdline`, `test_boot_update_keeps_prev_and_reads_back` (part files via `MU5358_BYNAME=<tmp>`), `test_rollback_boot_restores_prev`, `test_notice_writes_the_marker`, `test_status_json_is_valid` (python json), `test_no_misc_no_slot_a_no_nv` (the static suite covers it: assert the script is found by `helpers.shell_scripts()`), `test_bootimg_helpers_are_the_installers`. FAIL (no script).
- [ ] Step 2: port (copy, sed, delete the listed blocks, add ours). Run under `sh` and busybox (docker) like the installer tests.
- [ ] Step 3: suite OK; commit `mu5358-update: updates from inside Linux (mu300-update without the slot and misc code)`.

### Task 6: The static lz4 and the carry list agreement

**Files:** Create `tools/lz4/build.sh` (pinned lz4 release tarball + sha256, `make -C programs lz4 LDFLAGS=-static` in the Alpine image ci-release already uses for the static tools; output `tools/lz4/lz4`); Modify `tools/ci-release.sh`, `tools/make-release.sh` (build when missing; `[ -x tools/lz4/lz4 ]` required with `MU5358_REQUIRE_SIGNATURE`), `openwrt/customize.sh` (`/opt/mu5358/bin/lz4` if present); Test `tests/test_static.py` `Update.test_lz4_is_pinned`, `test_carry_lists_agree` (every CARRY path of the installer is under mu5358-update's user-area classes: etc, root; the test parses both).

- [ ] Steps: tests FAIL → implement → OK → commit `A static lz4 for the ramdisk; the carry lists agree`.

### Task 7: The Android installer updates by tarball and migrates a flat install

**Files:** Modify `installer/mu5358-install.sh` (`update()`: `check; migrate; unpack; carry; swap; boot_images`), `F_SYSTEM=mu5358-system.tar.gz` taken by `take` (optional for install, required for update), `installed()` reads `boot/installed` from the mounted filesystem when present (copies it to `$HOME_DIR/installed`), `space` for update = 2 × tree + 64 MiB; Test `tests/test_installer.py` class `MigrateFlat` (a tmp dir as `$MNT`: flat → `system/`, `data/` with `usb-disk.img` moved, `boot/tree`; idempotent on a tree layout), `Installed.test_boot_linux_reads_the_regions_record`.

**Interfaces:** `migrate MNT`: flat → `mkdir system; mv` everything but `lost+found` into it; `mkdir data boot; mv system/data/usb-disk.img data/ 2>/dev/null`; `echo system > boot/tree`; `sync`. Steps emitted: `check carry migrate system vendor boot done` (`MU5358-STEP migrate` is new; the page's `INSTALL_STEPS` learns `migrate` and `carry` in Task 8).

- [ ] Steps: tests FAIL → implement (tar through Magisk's busybox: `tar -xzf` into `system.new`, then the same swap as Task 5; vendor_files into the new tree; CARRY restore; `.mu5358-complete`) → suite OK → commit `Installer: update unpacks the tree into system.new, keeps data/, migrates a flat install`.

### Task 8: The web installer pushes the tree for an update

**Files:** Modify `site/assets/web-installer/device.js` (`PUSHED_IN` by kind: update pushes sums, kernel, ramdisk, system tarball, tools; install pushes the image too), `app.js` (`startInstall` passes the kind to pushRelease), `flow.js` (`INSTALL_STEPS` + `migrate`, `carry`; `parseStatus` `layout`), `strings.js` (step labels en/tr/zh; the restart note after a Linux-side update is the panel's, not the page's); Test `tests/web/flow.test.mjs`.

- [ ] Steps: tests FAIL → implement → `node --test tests/web/*.mjs` OK → commit `Web installer: an update pushes the system tree, shows migrate and carry`.

### Task 9: The panel's Update page

**Files:** Modify `openwrt/customize.sh` (re-add `admin/system/mu5358-update` to the menu — the removal line goes; `/usr/libexec/unisoc-modem/update` `stage_of`: add the "carrying your files over" text → `keep`; `update.js` via `openwrt/update/update-page.sed`: delete the kernel chooser rows, delete the "Start Android at the next restart" button, the restart confirm text → the trial note; all with guards), the ACL stays; Test `tests/test_static.py` `Update.test_menu_keeps_update_and_backup`, `SedGuards` covers the new seds automatically.

- [ ] Steps: tests FAIL → implement → OK → commit `Panel: the Update page is back (no kernel choice, no default-boot switch; restart means Android)`.

### Task 10: Dashboard badges and the login note

**Files:** The base tree's `dashboard-info` reads `/run/mu5358/update-available` and `home.js` shows it (rebranded, unchanged: verify, no code); `etc/profile.d/mu5358-update.sh` likewise. u30ui2: `tools/u30ui2/i18n.h` row `X(S_UPDATE_OUT, "Update %s is out", "Güncelleme %s çıktı", "有更新 %s")`, `data.c` reads the notice file's tag, `ui.c` shows it on the dashboard; `tools/u30ui2/mkfont.sh` regenerates `font_cjk.h`; Test `tests/test_static.py` `ScreenStrings` (automatic), `test_update.py` `test_notice_file_has_the_tag_first`.

- [ ] Steps: test FAIL → implement → OK → commit `u30ui2: the update badge`.

### Task 11: Docs

**Files:** `docs/GKI-SYSTEM.md` (services table: boot-ok, update-notice, the panel row; "Known behaviour": the layout, the fallback, the restart-goes-to-Android note; logs: `stage=tree-*`), `README.md` (an "Updating" section: from the page, from Linux, rollback; the restart note), `docs/MU300-PARITY.md` rows for B1/B10/#285/#120.

- [ ] Commit `Docs: updating from Linux`.

### Task 12: Bench (the maintainer's device, trial boots only)

1. CI release R1 (this branch merged to main): `gh workflow run release.yml`.
2. Device in Android: the page or `deploy` script → `update` to R1: watch `MU5358-STEP migrate`, then `boot-linux`. In Linux: `mount | grep mu5358-disk`, `/data/usb-disk.img` present (16 GiB), `cat /mnt/mu5358-disk/boot/tree tries ok`, `mu5358-update status` (`layout:tree`, `boot.pending:false`), the Update page in LuCI shows R1 installed.
3. A second CI release R2 (a docs-only commit). On the device: `mu5358-update check` → R2 offered; `mu5358-update apply` (over the Mac's NCM uplink; the SIM stays untouched); watch the stages; `status` → pending:false, `system.old` present, `data/prev/*` present. Reboot → Android → page: status shows R2 installed (`installed()` from the region) → `boot-linux` → Linux R2, `boot/ok` = R2 after 30 s, USB disk still there, adb keys kept.
4. `mu5358-update rollback` → reboot → Android → `boot-linux` → R1 (`boot/installed` says R1: rollback-boot ran too).
5. Fallback: in Linux R1, `echo 3 > /mnt/mu5358-disk/boot/tries; rm boot/ok` → reboot → `boot-linux` → the persistent log shows `stage=tree-fallback`, the system booted is `system.old` (R2 after the rollback) — then `mu5358-update rollback` again to tidy.
6. Findings → docs; no USB cut is involved (no self-undo needed beyond the trial's own 5-minute reboot timer).
