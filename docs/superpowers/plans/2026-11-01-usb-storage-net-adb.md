# USB flash-drive mode, network mode, adb and host-mode CLI (W5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the U30 Pro shows up to a computer as a USB flash drive when asked (from the screen, LuCI or the shell), takes a USB network protocol choice for the next boot, lets adb into Linux with keys, and has a host-mode switch ready for the bench test.

**Architecture:** one board-specific POSIX script `mu5358-usb` (openwrt/usb/, installed over the rootfs's rebranded mu300 one, which stays beside it for its `adb` subcommands) owns the state files and the gadget changes at run time; `boot/init` composes the gadget from those state files at boot, just before switch_root, so a mode never costs a rebind at boot; a small rpcd object `mu5358usb` and a LuCI card module give the web panel the toggle; u30ui2's settings page gets one row. Nothing new in the kernel: `USB_CONFIGFS_MASS_STORAGE`, `F_FS`, `NCM`, `ECM`, `RNDIS` are all `y` in the running kernel (checked on the bench, 2026-11-01).

**Tech Stack:** POSIX sh (busybox on the device; `sh` on the Mac and dash in CI for the tests), configfs USB gadget, Python `unittest` (tests/), LuCI JS (`'require baseclass'`, `rpc.declare`), C (u30ui2), `openwrt/customize.sh` guarded seds.

**Spec:** `docs/superpowers/specs/2026-11-01-mu300-parity-usb-charging-design.md`, section "W5 USB" (and "Hardware facts", "Constraints that do not move").

## Global Constraints

- Linux stays a one-shot trial; nothing here touches misc, slot a, NV or the GPT. No `telnetd`. (Spec constraints 1, 2, 6.)
- "No test cuts the USB link to the bench Mac without an on-board self-undo (a timer that puts the port back)." Every bench step that rebinds the gadget or swaps the role arms the undo first; the board's hotspot is the second way in.
- The kernel is not changed in this plan (constraint 7); the fragment stays as it is.
- Every `sed -i` in `openwrt/customize.sh` is anchored by a loud grep (tests/test_static.py `SedGuards`); every u30ui2 string goes through `i18n.h` with three languages and `mkfont.sh` (test `ScreenStrings`); no `mu300` in code under `tools/u30ui2`, `boot/init`, `installer/` (test `Rebranded`).
- State files live in `/etc/mu5358/` and are carried by `installer/mu5358-install.sh`'s `CARRY` list; the backing image lives at `/data/usb-disk.img` (the flat layout today; W2's layout bind-mounts `<fs>/data` there, so the path does not change).
- Tests: `cd tests && python3 -m unittest discover -s . -p 'test_*.py'` must stay green (84 tests before this plan); the new ones need no docker.
- Measured facts the code relies on (bench, 2026-11-01): the gadget is `/sys/kernel/config/usb_gadget/linux` once configfs is mounted (the running system does **not** mount configfs: `mu5358-usb` mounts it); the UDC is `musb-hdrc.1.auto`; the role switch `64a00000.usb-role-switch` has **no `role` attribute** (the driver does not allow userspace control), while `/sys/class/typec/port0/data_role` reads `host [device]`; `/opt/mu5358/bin/busybox` has `mkdosfs`, `losetup`, `truncate`, `fallocate`; the root filesystem has 25 GiB free on the bench (the region), 1.7 GiB on a fulldumpdb install.

## Review Focus

1. `storage on` when the image is missing and the filesystem's free space minus 2 GiB would be under 1 GiB: a smaller image, `min(2048, free_mib - 1024)` MiB, and under 256 MiB available the feature is refused with a message; the state file is left as it was. (Task 1 test `test_size_follows_the_free_space`, `test_refused_under_256_mib`.)
2. `storage size N` while the drive is exported: refused ("turn it off first"), the image untouched; `N` that is not a number, below 64 or above the free space: refused. (Task 1 `test_size_only_while_off`.)
3. Toggling while the gadget is not bound (no UDC, or the port in the host role): the state file is written, no error from the rebind, the next boot applies it. (Task 1 `test_toggle_without_a_bound_gadget_saves_the_state`.)
4. `boot/init` with `usb-storage` saying `on` but no image at `/newroot/data/usb-disk.img`: the function is still added, without a medium; the system's `mu5358-usb boot` makes the image and inserts it. (Task 2 `test_storage_on_without_an_image_still_adds_the_function`.)
5. The rpcd `set` with a value outside its allow-list, or a `size` with a sign or letters: `{"ok":0,...}` and `mu5358-usb` is never run. (Task 4 `test_refuses_what_is_not_allowed`.)

---

## File map

| file | responsibility |
|---|---|
| `openwrt/usb/mu5358-usb` (new) | the board's USB tool: `status`, `storage on\|off\|status\|size N\|mount\|umount`, `net ncm\|ecm\|rndis [once]\|default`, `adb …` (delegated), `host\|device`, `boot` |
| `openwrt/usb/rpcd-mu5358usb` (new) | rpcd object `mu5358usb`: `get`, `set` |
| `openwrt/usb/acl-mu5358usb.json` (new) | the ACL for it, group `luci-app-mu5358` |
| `openwrt/usb/usb-storage.js` (new) | LuCI module `mu5358/usb-storage`: the "USB flash drive" card on System > Device |
| `openwrt/usb/mu5358-usb-boot.init` (new) | `/etc/init.d/mu5358-usb-boot`, START=12: `mu5358-usb boot` |
| `boot/init` (modify) | `usb_apply ROOT` just before switch_root: the composite from `ROOT/etc/mu5358/usb-net` and `usb-storage` |
| `openwrt/customize.sh` (modify) | installs the five files, keeps the rootfs's script as `/opt/mu5358/lib/usb-adb`, turns adb on with keys, patches `mu5358-adbd`, `device-usb` and `device.js` |
| `openwrt/panel/po/{tr,zh_Hans}/mu5358.po` (modify) | the card's strings |
| `tools/u30ui2/{i18n.h,ui.c,data.c,data.h,font_cjk.h}` (modify) | the settings row |
| `installer/mu5358-install.sh` (modify) | `CARRY` += `etc/mu5358/usb-storage etc/mu5358/adb_auth etc/mu5358/adb_keys` |
| `tests/test_usb.py` (new) | the script, the init block, the rpcd object, with fake trees |
| `tests/test_static.py` (modify) | class `Usb`: wiring in customize.sh, CARRY, the po strings of the JS, the settings row |
| `docs/GKI-SYSTEM.md`, `docs/MU300-PARITY.md`, `README.md` (modify) | status, the role-switch finding, parity rows A9/A10 |

### Gadget identity (shared by Tasks 1, 2, 7)

| composite | idProduct | bcdDevice | config string |
|---|---|---|---|
| NCM/ECM + ACM (today) | `0xa4a1` | `0x0301` | `ncm + ACM console` / `ecm + ACM console` |
| RNDIS + ACM | `0xa4a1` | `0x0302` | `RNDIS + ACM console` (+ MS OS descriptors as mu300's init: `os_desc/use 1`, `b_vendor_code 0xcd`, `qw_sign MSFT100`, `functions/rndis.rn0/os_desc/interface.rndis/compatible_id RNDIS`, `sub_compatible_id 5162001`, link `os_desc/c.1`) |
| any of the above + mass storage | `0xa4a2` | as above | the same string with ` + USB disk` |

A different idProduct when the disk is in the composite, rather than a bcdDevice bit: `mu5358-adbd` (mu300's, kept) writes bcdDevice absolutely (`0x0301..0x0304`) and would clobber a bit; Windows caches composite layouts by VID/PID. Functions: `f1` network, `f2` `acm.GS0`, `f3` `mass_storage.0` (`lun.0/removable 1`, `lun.0/nofua 1`, `lun.0/cdrom 0`, `lun.0/ro 0`, `lun.0/inquiry_string` = `MU5358  Linux disk      0001`), `f4` is adb's (`ffs.adb`, made by `mu5358-adbd` as `f3` today: Task 7 patches its `L=` line to `f4`). A medium is "inserted" by writing the image path to `lun.0/file` and ejected by writing an empty line: no rebind for either. Adding or removing `f3` rebinds once (`echo '' > UDC`, then the UDC name again).

### State files

| file | content | written by |
|---|---|---|
| `/etc/mu5358/usb-storage` | `on SIZE_MIB` or `off SIZE_MIB` (one line; missing = off, 2048) | `mu5358-usb storage` |
| `/data/usb-disk.img` | the sparse FAT32 image, `SIZE_MIB` MiB | `mu5358-usb storage` (first `on`, `size`, `boot` when missing) |
| `/etc/mu5358/usb-net`, `usb-net.once` | mu300's, through `device-usb` (UCI `unisoc_modem.usb`) | `device-usb set-net` |
| `/etc/mu5358/usb-net-applied` | what `boot/init` composed this boot: `ncm\|ecm\|rndis` | `boot/init`; copied to `/run/mu5358/usb-net-applied` by `mu5358-usb boot` for `device-usb boot` and the rootfs script |
| `/etc/mu5358/adb`, `adb_auth`, `adb_keys` | mu300's | the delegated `adb` subcommands |
| `/run/mu5358/usb-host-keep` | its presence cancels the host-mode self-undo | the tester |

FAT32 (busybox `mkdosfs -F 32`) rather than exFAT: no `mkfs.exfat` in the rootfs, a 2 GiB disk cannot hold a file over 4 GiB anyway, and FAT32 needs no driver on any host. The spec's "exFAT" is amended by this plan (author's call; `size N` up to the free space, FAT32 volumes go to 2 TiB).

---

### Task 1: `mu5358-usb` and flash-drive mode

**Files:**
- Create: `openwrt/usb/mu5358-usb`
- Test: `tests/test_usb.py`

**Interfaces:**
- Produces: the script (`#!/bin/sh`, `set -u`, `PATH` extended with `/opt/mu5358/bin:/usr/sbin:/sbin`), reading these environment overrides (tests): `MU5358_GADGET` (default `/sys/kernel/config/usb_gadget/linux`), `MU5358_CONFIGFS` (`/sys/kernel/config`; when `MU5358_GADGET` is set no mount is attempted), `MU5358_UDC_CLASS` (`/sys/class/udc`), `MU5358_STORAGE_CONF` (`/etc/mu5358/usb-storage`), `MU5358_USB_DISK` (`/data/usb-disk.img`), `MU5358_MKFS` (a command line; default `mkfs.vfat` when on PATH, else `/opt/mu5358/bin/busybox mkdosfs`), `MU5358_FREE_KB` (overrides `df -k` on the image's directory), `MU5358_RUN` (`/run/mu5358`), `MU5358_TYPEC` (`/sys/class/typec/port0`), `MU5358_DEVICE_USB` (`/usr/libexec/unisoc-modem/device-usb`), `MU5358_USB_ADB` (`/opt/mu5358/lib/usb-adb`), `MU5358_HOST_UNDO` (seconds, default 60; `0` = no timer), `MU5358_APPLIED` (`/etc/mu5358/usb-net-applied`).
- Produces: shell functions inside it that Task 3 extends: `gadget_bound` (UDC file non-empty), `rebind` (unbind, bind the UDC that was bound or the first in `MU5358_UDC_CLASS`; returns 1 when there is none, prints nothing), `storage_state` (sets `ST_ON` 0/1 and `ST_MIB`), `storage_write STATE MIB` (atomic `.new.$$` + `mv`), `image_make MIB` (sparse by `dd if=/dev/zero of=… bs=1M count=0 seek=MIB`, then `$MU5358_MKFS -F 32 -n MU5358 FILE`), `disk_in` (f3 link exists), `disk_add`/`disk_remove` (the function dir, the attributes of the table above, the link, idProduct and the configuration string; no rebind), `medium_insert`/`medium_eject`.
- Exit codes: 0 ok, 1 refused (message on stderr starting `mu5358-usb: `), 2 usage.

- [ ] **Step 1: Write the failing tests** (`tests/test_usb.py`, class `Storage`; a helper `Fake` in the same file builds under `mkdtemp` a gadget tree `functions/{ncm.usb0,acm.GS0}`, `configs/c.1/{f1,f2}` symlinks, `configs/c.1/strings/0x409/configuration` = `ncm + ACM console`, `UDC` = `musb-hdrc.1.auto`, `idProduct` = `0xa4a1`, `udc/musb-hdrc.1.auto/` dir, a `data/` dir, an `etc/` dir, a `bin/` dir first on PATH with a `mkfs.vfat` stub that appends its argv to `$STUBLOG` and a `mount`/`umount` stub pair; `run(*args, env)` runs `sh openwrt/usb/mu5358-usb` with the overrides set). Also: a fake configfs needs `mkdir functions/mass_storage.0` to produce `lun.0/`: the stub's `Fake.gadget_mkdir_hook` is not possible from outside the script, so `disk_add` must `mkdir -p "$G/functions/mass_storage.0/lun.0"` (harmless on real configfs: the directory already exists after the first mkdir) — pin that in `test_on_adds_the_function`.)

```python
def test_on_adds_the_function_and_rebinds(self):
    r = self.run('storage', 'on')
    self.assertEqual(r.returncode, 0, r.stderr)
    self.assertEqual(os.readlink(self.g / 'configs/c.1/f3'), str(self.g / 'functions/mass_storage.0'))
    lun = self.g / 'functions/mass_storage.0/lun.0'
    self.assertEqual((lun / 'removable').read_text().strip(), '1')
    self.assertEqual((lun / 'nofua').read_text().strip(), '1')
    self.assertEqual((lun / 'file').read_text().strip(), str(self.data / 'usb-disk.img'))
    self.assertEqual((self.g / 'idProduct').read_text().strip(), '0xa4a2')
    self.assertEqual((self.g / 'configs/c.1/strings/0x409/configuration').read_text().strip(), 'ncm + ACM console + USB disk')
    self.assertEqual((self.g / 'UDC').read_text().strip(), 'musb-hdrc.1.auto')
    self.assertTrue((self.run_dir / 'usb-rebound').exists())        # rebind leaves this marker (the uptime); the no-rebind tests assert its absence
    self.assertEqual((self.etc / 'usb-storage').read_text(), 'on 2048\n')
    self.assertEqual(os.stat(self.data / 'usb-disk.img').st_size, 2048 << 20)
    self.assertIn('-F 32', self.stublog())

def test_on_twice_is_quiet_and_does_not_rebind(self): ...   # second run: rc 0, mkfs not run again, UDC unchanged
def test_off_ejects_removes_and_restores_identity(self):   # f3 gone, idProduct 0xa4a1, configuration string back, state 'off 2048', the image kept
def test_size_follows_the_free_space(self):                 # MU5358_FREE_KB = 2.5 GiB → the image is 1536 MiB and the state says 1536
def test_refused_under_256_mib(self):                       # MU5358_FREE_KB = 1.2 GiB → rc 1, stderr mentions '256 MiB', no image, no state file
def test_size_only_while_off(self):                         # on → 'size 512' rc 1 'turn it off first'; off → 'size 512' makes a 512 MiB image, state 'off 512'; 'size 12abc' and 'size 32' rc 2/1
def test_toggle_without_a_bound_gadget_saves_the_state(self):  # UDC file empty and udc dir empty → 'on' rc 0, state 'on 2048', f3 present, no error text
def test_status_lines(self):                                # 'storage status' prints 'storage: on', 'size: 2048 MiB', 'exported: yes|no', 'image: /data/usb-disk.img (present|missing)', 'free: N MiB'
def test_mount_ejects_first_and_umount_reinserts(self):     # 'storage mount' while on: lun file emptied, mount stub called with '-o loop' the image on /mnt/usb-disk; 'storage umount': umount stub, lun file back
def test_usage(self):                                       # 'storage' alone and 'storage bogus' → rc 2, usage on stderr
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cd tests && python3 -m unittest test_usb.Storage -v 2>&1 | tail -15`
Expected: every test FAILS or ERRORS with `No such file or directory: 'openwrt/usb/mu5358-usb'` (the subprocess) or rc 127.

- [ ] **Step 3: Write `openwrt/usb/mu5358-usb`** with the header comment in the project's style (what it is, the subcommands, the files), the env table from Interfaces, the functions from Interfaces, and the `storage` dispatcher: `on` (state → if not `ST_ON`: free-space rule → `image_make` when missing → `disk_add` → `rebind` when `gadget_bound` → `medium_insert` → `storage_write on MIB`; prints `USB flash drive: on (MIB MiB); the USB connection to the computer comes back in a moment`), `off` (`medium_eject`, `disk_remove`, `rebind` if bound, `storage_write off MIB`), `size N` (`ST_ON` → refuse; N numeric ≥ 64 ≤ free-1024 → remove the image, `image_make`, `storage_write off N`), `status`, `mount` (`medium_eject`; `mkdir -p /mnt/usb-disk; mount -o loop,uid=0 IMG /mnt/usb-disk`), `umount` (`umount /mnt/usb-disk; medium_insert` when on). The free-space rule: `free_kb=${MU5358_FREE_KB:-$(df -k "$(dirname "$IMG")" | awk 'NR == 2 { print $4 }')}`; `avail_mib=$((free_kb / 1024 - 1024))`; `[ $avail_mib -ge 256 ] || die "not enough room for a USB disk: NN MiB free, 256 MiB needed beside 1 GiB kept for the system"`; `mib = avail_mib < 2048 ? avail_mib : 2048`. Only `storage`, `status` (prints the storage status and `adb: ` via the delegate when present) and the usage exist after this task; `net`, `adb`, `host`, `device`, `boot` print `not yet` and exit 2 (Task 3 fills them).

- [ ] **Step 4: Run the tests**

Run: `cd tests && python3 -m unittest test_usb.Storage -v 2>&1 | tail -15`
Expected: `OK` (10 tests).

- [ ] **Step 5: The whole suite, and the syntax rule**

Run: `cd tests && python3 -m unittest discover -s . -p 'test_*.py' 2>&1 | tail -3`
Expected: `OK`, 94 tests (the `Syntax` class picks the new script up by its shebang).

- [ ] **Step 6: Commit**

```bash
git add openwrt/usb/mu5358-usb tests/test_usb.py
git commit -m "mu5358-usb: the U30 Pro as a USB flash drive (a FAT32 image in /data, mass_storage in the gadget)"
```

---

### Task 2: `boot/init` composes the gadget from the state files

**Files:**
- Modify: `boot/init` (the gadget block at the `# USB:` comment, and a new call just before `kill $timer $console`)
- Test: `tests/test_usb.py` class `InitApply`

**Interfaces:**
- Produces: in `boot/init`, between the marker comments `# --- usb-apply begin` and `# --- usb-apply end`, the function `usb_apply ROOT` (and its helpers `usb_want ROOT` → sets `WANT_NET` ncm|ecm|rndis from `ROOT/etc/mu5358/usb-net` (anything else: ncm), `WANT_DISK` 0/1 from the first word of `ROOT/etc/mu5358/usb-storage`; `usb_now` → `NOW_NET` from the `f1` link target, `NOW_DISK` from `f3`; `usb_compose NET DISK` → the function set of the table, `f1` link swapped (the old function dir removed with `rmdir` after unlinking), `f3` added/removed, idProduct, bcdDevice, configuration string, MS OS descriptors for rndis, the netdev MACs `02:35:58:00:00:01/02` for ncm/ecm and `03/04` for rndis). `usb_apply` rebinds only when `WANT_* != NOW_*`, then writes `ROOT/etc/mu5358/usb-net-applied` = `WANT_NET` in every case, and logs `stage=usb-apply net=… disk=… rebind=0|1`. Variables it uses: `g`, `UDC_CLASS` (`${UDC_CLASS:-/sys/class/udc}`), the `log` function.
- The existing block keeps composing NCM + ACM early (the initramfs console and DHCP need it before the root is found); `usb_apply /newroot` runs after the `/mu5358-stay` check and before `kill $timer $console`.

- [ ] **Step 1: Write the failing tests** (class `InitApply`: `block()` cuts the marker block out of `boot/init` with a regex; `harness(root_files, now)` writes a script: `log() { echo "LOG $*"; }`, `g=$TMP/g`, `UDC_CLASS=$TMP/udc`, a fake tree as Task 1's with `f1 → functions/ncm.usb0`, `mkdir` overridden by a function that also creates `lun.0/` under `mass_storage.0` and `os_desc/interface.rndis/` under `rndis.rn0`, `rmdir() { rm -rf "$@"; }`; runs `usb_apply $TMP/root` under `sh`)

```python
def test_ncm_and_no_disk_is_what_is_bound_no_rebind(self):   # no state files → f1 ncm, no f3, applied 'ncm', 'rebind=0' in the log, UDC file unchanged
def test_rndis_rebuilds_f1_with_the_ms_os_descriptors(self):  # usb-net 'rndis' → f1 → rndis.rn0, ncm.usb0 gone, bcdDevice 0x0302, os_desc/use 1, qw_sign MSFT100, compatible_id RNDIS, link os_desc/c.1, host_addr 02:35:58:00:00:04, 'rebind=1'
def test_ecm_rebuilds_f1(self):                                # 'ecm' → f1 → ecm.usb0, configuration 'ecm + ACM console'
def test_storage_on_adds_f3_without_a_medium(self):            # usb-storage 'on 2048' → f3 → mass_storage.0, lun.0/removable 1, nofua 1, file empty, idProduct 0xa4a2, configuration '... + USB disk'
def test_storage_on_without_an_image_still_adds_the_function(self):  # same as above with no data/ at all → f3 there; (the fourth Review Focus line)
def test_garbage_in_the_files_means_the_defaults(self):        # usb-net 'wat', usb-storage 'maybe' → ncm, no f3, applied 'ncm'
def test_once_is_not_the_init_s_business(self):               # usb-net.once present → nothing different (the system's boot step consumes it)
def test_place_in_init(self):                                  # 'usb_apply /newroot' occurs once in boot/init, after '/mu5358-stay' and before 'kill $timer $console'
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tests && python3 -m unittest test_usb.InitApply -v 2>&1 | tail -12`
Expected: FAIL — `block()` finds no `# --- usb-apply begin` marker (assert it with a clear message first in `block()`).

- [ ] **Step 3: Implement the block and the call in `boot/init`**, with the comment explaining why it sits there (the state files are on the root, which is found after the early gadget; a rebind here costs nothing: the console shell and the initramfs DHCP are about to be stopped and the system's preinit `06_mu5358_early_usb` serves `usb0` or `rndis0`).

- [ ] **Step 4: Run the tests**

Run: `cd tests && python3 -m unittest test_usb.InitApply -v 2>&1 | tail -12`
Expected: `OK` (8 tests).

- [ ] **Step 5: The whole suite** (the `Partitions`, `NoTelnet`, `Rebranded` and `Syntax` rules read `boot/init`)

Run: `cd tests && python3 -m unittest discover -s . -p 'test_*.py' 2>&1 | tail -3`
Expected: `OK`, 102 tests.

- [ ] **Step 6: Commit**

```bash
git add boot/init tests/test_usb.py
git commit -m "boot/init: the USB composite from /etc/mu5358/usb-net and usb-storage, before switch_root (no rebind at boot)"
```

---

### Task 3: `boot`, `net`, `adb`, `host|device`, `status`

**Files:**
- Modify: `openwrt/usb/mu5358-usb`
- Test: `tests/test_usb.py` classes `Boot`, `Net`, `Host`

**Interfaces:**
- Consumes: Task 1's functions and env; `MU5358_DEVICE_USB`, `MU5358_USB_ADB`, `MU5358_TYPEC`, `MU5358_HOST_UNDO`, `MU5358_APPLIED`, `MU5358_RUN`.
- Produces:
  - `boot`: mounts configfs at `MU5358_CONFIGFS` when `MU5358_GADGET` is unset and `/proc/mounts` lacks it; `mkdir -p $MU5358_RUN`; copies `MU5358_APPLIED` to `$MU5358_RUN/usb-net-applied` when present; storage: when `ST_ON`: image missing → `image_make ST_MIB` (the free-space rule; a refusal is logged, the state left); `disk_in` → `medium_insert`; not `disk_in` (an init_boot from before Task 2) → logs `usb disk: on, but this boot's init did not add it (update from the page once); not rebinding a live link` and does nothing else. **Never a rebind from `boot`.**
  - `net ncm|ecm|rndis [once]` → `exec "$MU5358_DEVICE_USB" set-net MODE once|permanent 1`; `net default` → `set-net ncm permanent 0`; `net` alone → prints the saved choice from `/etc/mu5358/usb-net` (+ `(once)`) and this boot's from `MU5358_APPLIED`.
  - `adb …` → `exec "$MU5358_USB_ADB" adb "$@"` (the rootfs's mu300 script kept by Task 7); missing delegate → `die "adb is not on this image"`.
  - `host` → `[ -e $MU5358_TYPEC/data_role ] || die "no USB-C port controller to ask"`; writes `host` to `data_role`; unless `MU5358_HOST_UNDO` is 0, starts `( sleep N; [ -e $MU5358_RUN/usb-host-keep ] || { echo device > data_role; logger -t mu5358-usb "host mode undone after N s (no $MU5358_RUN/usb-host-keep)"; } ) &` detached with `setsid`; prints what it did and how to keep it. `device` → writes `device`, removes the keep file. The refusal text when the write fails: `the port controller refused the swap (needs a USB-PD partner for a data role swap; the role switch of this kernel takes no userspace writes)`.
  - `status` (and no argument): `storage` lines, `net: …`, `adb: …` from the delegate's `adb status` when there, `role: data_role's bracketed value`.

- [ ] **Step 1: Write the failing tests**

```python
class Boot(Base):
    def test_inserts_the_medium_when_on_and_the_function_is_there(self):  # f3 present, lun file empty, state 'on 2048', image present → lun file = image, no rebind marker
    def test_makes_the_image_when_missing(self):                          # state 'on 2048', no image → mkfs stub ran, image 2048 MiB, inserted
    def test_without_the_function_it_says_so_and_does_not_rebind(self):   # state on, no f3 → stdout/stderr contains 'update from the page once', no f3 added, no rebind marker
    def test_copies_the_applied_mode_to_run(self):                        # MU5358_APPLIED file 'rndis' → $RUN/usb-net-applied == 'rndis'
    def test_off_does_nothing(self):                                       # state off → no image, no lun write
class Net(Base):
    def test_delegates_to_device_usb(self):   # a recorder stub as MU5358_DEVICE_USB: 'net rndis once' → ['set-net','rndis','once','1']; 'net ecm' → ['set-net','ecm','permanent','1']; 'net default' → ['set-net','ncm','permanent','0']
    def test_shows_the_choice(self):          # usb-net 'ecm' + usb-net.once, applied 'ncm' → stdout has 'ecm (once)' and 'this boot: ncm'
    def test_adb_delegates(self):             # recorder as MU5358_USB_ADB: 'adb key -' → ['adb','key','-']
class Host(Base):
    def test_host_writes_the_data_role_and_undoes_itself(self):  # MU5358_HOST_UNDO=1: data_role file 'host' right after; after 2 s 'device'
    def test_keep_file_cancels_the_undo(self):                   # touch $RUN/usb-host-keep before: still 'host' after 2 s
    def test_device_puts_it_back(self):                          # 'device' → file 'device', keep file removed
    def test_no_typec_is_refused(self):                          # MU5358_TYPEC to a missing dir → rc 1, 'no USB-C port controller'
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tests && python3 -m unittest test_usb.Boot test_usb.Net test_usb.Host -v 2>&1 | tail -15`
Expected: FAIL/ERROR: the stubs print `not yet`, rc 2.

- [ ] **Step 3: Implement the four subcommands and `status`** in `openwrt/usb/mu5358-usb` as the Interfaces say.

- [ ] **Step 4: Run the tests**

Run: `cd tests && python3 -m unittest test_usb -v 2>&1 | tail -5`
Expected: `OK` (30 tests in the module).

- [ ] **Step 5: Commit**

```bash
git add openwrt/usb/mu5358-usb tests/test_usb.py
git commit -m "mu5358-usb: boot (the medium, no rebind), net via device-usb, adb delegated, host/device over the Type-C port with a self-undo"
```

---

### Task 4: rpcd object `mu5358usb`

**Files:**
- Create: `openwrt/usb/rpcd-mu5358usb`, `openwrt/usb/acl-mu5358usb.json`
- Test: `tests/test_usb.py` class `Rpcd`

**Interfaces:**
- Consumes: `mu5358-usb storage status` lines (Task 1's exact `key: value` format) and `mu5358-usb storage on|off|size N`, `mu5358-usb adb on|off` (Task 3); env `MU5358_USB_BIN` (default `/opt/mu5358/bin/mu5358-usb`).
- Produces: rpcd's plugin protocol (`list` → `{"get":{},"set":{"kind":"str","value":"str"}}`; `call get` → `{"ok":1,"storage":"on|off","size_mib":N,"exported":0|1,"image":0|1,"free_mib":N,"adb":"off|open|keys|unavailable","role":"device|host|unavailable"}`; `call set` with stdin `{"kind":"storage|storage_size|adb","value":"…"}` → `{"ok":1,"message":"<the tool's first stdout line>"}` or `{"ok":0,"error":"…"}`). Values: `storage` on|off; `storage_size` `[0-9]{2,6}`; `adb` on|off. Stdin parsed with `sed -n 's/.*"kind"[[:space:]]*:[[:space:]]*"\([A-Za-z_]*\)".*/\1/p'` (and the same for `value`); `jsonfilter` is not required (the tests run on the Mac). ACL: `{"luci-app-mu5358": {"description": "USB flash drive and adb (mu5358usb)", "read": {"ubus": {"mu5358usb": ["get"]}}, "write": {"ubus": {"mu5358usb": ["set"]}}}}`.

- [ ] **Step 1: Write the failing tests** (`Rpcd`: a recorder stub `mu5358-usb` in the fake `bin/` printing a canned `storage status` and `adb status`; `rpc(method, stdin)` runs `sh openwrt/usb/rpcd-mu5358usb call METHOD` with `MU5358_USB_BIN` pointing at the stub)

```python
def test_list(self):                          # json.loads(stdout) == {'get': {}, 'set': {'kind': 'str', 'value': 'str'}}
def test_get(self):                           # the canned lines → ok 1, storage 'on', size_mib 2048, exported 1, image 1, free_mib 1234, adb 'keys', role 'device'
def test_set_storage_runs_the_tool(self):     # {'kind':'storage','value':'on'} → argv ['storage','on'], ok 1
def test_set_size(self):                      # {'kind':'storage_size','value':'512'} → ['storage','size','512']
def test_refuses_what_is_not_allowed(self):   # value 'on; reboot', kind 'reboot', size '-5', size '1e3' → ok 0, no argv recorded
def test_acl_names_the_methods(self):         # json.load of the ACL: read ubus mu5358usb == ['get'], write == ['set']
```

- [ ] **Step 2: Run to see them fail** — `cd tests && python3 -m unittest test_usb.Rpcd -v 2>&1 | tail -8` — Expected: ERROR, no such file.

- [ ] **Step 3: Write the two files.**

- [ ] **Step 4: Run** — same command — Expected: `OK` (6 tests).

- [ ] **Step 5: Commit**

```bash
git add openwrt/usb/rpcd-mu5358usb openwrt/usb/acl-mu5358usb.json tests/test_usb.py
git commit -m "rpcd mu5358usb: get/set for the USB flash drive and adb (the panel's toggle)"
```

---

### Task 5: the LuCI card and the Device page

**Files:**
- Create: `openwrt/usb/usb-storage.js`
- Modify: `openwrt/panel/po/tr/mu5358.po`, `openwrt/panel/po/zh_Hans/mu5358.po`, `openwrt/customize.sh` (the `device.js` patch block, lines ~565-575)
- Test: `tests/test_static.py` class `Usb` (methods `test_card_strings_are_translated`, `test_device_js_patch_keeps_the_net_card`), `tests/test_web.py` (the `node --check` list gains the file)

**Interfaces:**
- Consumes: rpc `mu5358usb.get/set` (Task 4's JSON).
- Produces: a LuCI module at `/www/luci-static/resources/mu5358/usb-storage.js`: `'use strict'; 'require baseclass'; 'require rpc'; 'require ui';` … `return baseclass.extend({ render: function(root, before) { … } })` that inserts `<section class="mud-card" id="mud-usb-disk-card">` before `before` with: title `_('USB flash drive')`, rows `_('State')` (`_('On')`/`_('Off')`), `_('Size')` (`N MiB`), `_('Exported to the computer')` (`_('Yes')`/`_('No')`), a size input (MiB, min 64), buttons `_('Turn on')`/`_('Turn off')`/`_('Resize (while off)')`, and the note `_('While the drive is on, the computer sees this device as a removable disk as well as the network link; turning it on or off replugs the USB link for a moment. The device itself does not touch the disk while it is exported.')`. Errors from `set` go to `ui.addNotification`. Polls `get` 3 s after a change.
- customize.sh: the existing guarded sed on `this.root = root;` is replaced by one that (1) hides only the role card (`root.querySelector(".mud-device-grid > section")`) and the adapters card, (2) keeps the network-mode card (boot/init reads the choice now), (3) `L.require("mu5358.usb-storage").then(function(m) { m.render(root, root.querySelector(".mud-device-grid")); });`. The old note string is removed from the two po files; the new strings added (tr and zh_Hans entries for every `_('…')` above).

- [ ] **Step 1: Write the failing tests** (`Usb.test_card_strings_are_translated`: every `_('…')` literal in `openwrt/usb/usb-storage.js` has a `msgid` with a non-empty `msgstr` in both po files; `Usb.test_device_js_patch_keeps_the_net_card`: customize.sh's device.js sed contains `mu5358.usb-storage` and not `mud-device-grid").style.display = "none"`; test_web's check list includes `openwrt/usb/usb-storage.js`)

- [ ] **Step 2: Run** — `cd tests && python3 -m unittest test_static.Usb test_web -v 2>&1 | tail -8` — Expected: FAIL (no such file / strings missing).

- [ ] **Step 3: Write the module, the po entries, the customize.sh patch** (keep the sed guard pattern: `grep -q '^[[:space:]]*this.root = root;$' "$f" || { echo …; exit 1; }` and the `! grep -q 'mu5358: USB'` idempotence marker renamed to `mu5358: USB disk`).

- [ ] **Step 4: Run** — same — Expected: `OK`.

- [ ] **Step 5: Commit**

```bash
git add openwrt/usb/usb-storage.js openwrt/panel/po openwrt/customize.sh tests/test_static.py tests/test_web.py
git commit -m "Control panel: the USB flash drive card on System > Device; the network-mode card back (boot/init reads it)"
```

---

### Task 6: the settings row on the screen

**Files:**
- Modify: `tools/u30ui2/i18n.h` (row `X(S_USB_DRIVE, "USB drive", "USB bellek", "U 盘")`), `tools/u30ui2/data.h` (`int usb_storage;` in `struct data`, under `/* system */`), `tools/u30ui2/data.c` (the collector reads the first word of `/etc/mu5358/usb-storage` each pass: `on` → 1), `tools/u30ui2/ui.c` (`SET_USB` after `SET_UI` in the enum, `S_USB_DRIVE` in `set_label`, `setting_value` → `T(S.usb_storage ? S_ON : S_OFF)`, `settings_tap` → `data_run("/opt/mu5358/bin/mu5358-usb storage off")` when on, else `… on`), `tools/u30ui2/font_cjk.h` (by `mkfont.sh`)
- Test: `tests/test_static.py` class `Usb` (`test_screen_row`)

**Interfaces:**
- Consumes: `mu5358-usb storage on|off` (Task 1), the state file format.

- [ ] **Step 1: Write the failing test** (`Usb.test_screen_row`: `i18n.h` has `S_USB_DRIVE`; `ui.c` has `SET_USB` in the enum and the two command strings `mu5358-usb storage on` / `off`; `data.c` reads `/etc/mu5358/usb-storage`)

- [ ] **Step 2: Run** — `cd tests && python3 -m unittest test_static.Usb.test_screen_row -v 2>&1 | tail -5` — Expected: FAIL.

- [ ] **Step 3: Make the four edits**; six rows fit (y = 76 + 5·27 = 211 on the 240 px screen; `settings_tap` already maps y up to 240).

- [ ] **Step 4: The font subset** (the character 盘 is new):

Run: `tools/u30ui2/mkfont.sh 2>&1 | tail -3`
Expected: `NNN code points` (one more than `git stash; tools/u30ui2/mkfont.sh` would print — compare with `git diff --stat tools/u30ui2/font_cjk.h`, which must show a change).

- [ ] **Step 5: Build and look**

Run: `docker run --rm -v "$PWD/tools":/t mu5358-mainline-build bash /t/u30ui2/build.sh 2>&1 | tail -2 && docker run --rm -v "$PWD/tools":/t mu5358-mainline-build bash /t/u30ui2/build.sh preview 2>&1 | tail -1`
Expected: `u30ui2` rebuilt; the preview count printed. Open `tools/u30ui2/preview/settings-zh.png` (or the file the preview names for the settings page in zh) with the Read tool: the sixth row reads `U 盘` with `关` / `开`.

- [ ] **Step 6: Run the suite** — `cd tests && python3 -m unittest discover -s . -p 'test_*.py' 2>&1 | tail -3` — Expected: `OK` (`ScreenStrings` passes on the new row).

- [ ] **Step 7: Commit**

```bash
git add tools/u30ui2/i18n.h tools/u30ui2/ui.c tools/u30ui2/data.c tools/u30ui2/data.h tools/u30ui2/font_cjk.h tools/u30ui2/preview tests/test_static.py
git commit -m "u30ui2: the USB flash drive switch on the settings page (en/tr/zh)"
```

---

### Task 7: wiring in the image, adb with keys, carry, docs

**Files:**
- Modify: `openwrt/customize.sh`, `installer/mu5358-install.sh` (CARRY), `docs/GKI-SYSTEM.md`, `docs/MU300-PARITY.md`, `README.md`
- Create: `openwrt/usb/mu5358-usb-boot.init`
- Test: `tests/test_static.py` class `Usb` (`test_in_image`, `test_adb_on_with_keys`, `test_carry`)

**Interfaces:**
- Consumes: everything above.
- Produces, in `openwrt/customize.sh` (a block after the charge block, same style):
  1. `mv "$R/opt/mu5358/bin/mu5358-usb" "$R/opt/mu5358/lib/usb-adb"` (guarded: the file must exist and contain `mu5358-adbd`), then `cp openwrt/usb/mu5358-usb "$R/opt/mu5358/bin/mu5358-usb"`, `chmod 755`.
  2. `cp openwrt/usb/rpcd-mu5358usb "$R/usr/libexec/rpcd/mu5358usb"` (755), `cp openwrt/usb/acl-mu5358usb.json "$R/usr/share/rpcd/acl.d/luci-app-mu5358-usb.json"`, `cp openwrt/usb/usb-storage.js "$R/www/luci-static/resources/mu5358/usb-storage.js"`.
  3. `cp openwrt/usb/mu5358-usb-boot.init "$R/etc/init.d/mu5358-usb-boot"` (755) + `ln -sf ../init.d/mu5358-usb-boot "$R/etc/rc.d/S12mu5358-usb-boot"`; the init file: `#!/bin/sh /etc/rc.common`, `START=12`, `start() { /opt/mu5358/bin/mu5358-usb boot 2>&1 | logger -t mu5358-usb; }`.
  4. adb: replace the line `echo off > "$R/etc/mu5358/adb"` (and its comment) by `echo on > adb`, `echo keys > adb_auth`, `touch adb_keys; chmod 600 adb_keys`, `ln -sf ../init.d/mu5358-adbd "$R/etc/rc.d/S19mu5358-adbd"`; guarded seds in `$R/opt/mu5358/bin/mu5358-adbd`: `ROLE=…25100000.dwc3-role-switch/role` → `64a00000.usb-role-switch/role`; `L=$G/configs/c.1/f3` → `f4`; the `usable()` FunctionFS test gains `|| zcat /proc/config.gz 2>/dev/null | grep -q '^CONFIG_USB_CONFIGFS_F_FS=y'` before the `||` that prints the refusal (anchor: the line starting `grep -qw functionfs "$FILESYSTEMS"`); in `$R/usr/libexec/unisoc-modem/device-usb`: `ROLE_FILE=…25100000.dwc3-role-switch/role` → `64a00000.usb-role-switch/role`.
  5. The comment at the top of customize.sh listing what is not ported loses `adbd`.
- `installer/mu5358-install.sh`: `CARRY` += `etc/mu5358/usb-storage etc/mu5358/adb_auth etc/mu5358/adb_keys` (`etc/mu5358/adb` and `usb-net` are already there).
- Docs: README's feature table gains "USB flash drive mode (screen, panel, `mu5358-usb storage`), USB network protocol, adb with keys"; `docs/GKI-SYSTEM.md` gets the finding "the role switch exposes no `role` attribute (no userspace control in the vendor driver); host mode goes through the Type-C data-role swap, untested until the OTG bench test"; `docs/MU300-PARITY.md` rows A9 (network mode) and A10 (adb) → done, with the storage row added.

- [ ] **Step 1: Write the failing tests** (`Usb.test_in_image`: customize.sh contains `opt/mu5358/lib/usb-adb`, `usr/libexec/rpcd/mu5358usb`, `acl.d/luci-app-mu5358-usb.json`, `resources/mu5358/usb-storage.js`, `S12mu5358-usb-boot`; `Usb.test_adb_on_with_keys`: `echo on > "$R/etc/mu5358/adb"`, `echo keys > "$R/etc/mu5358/adb_auth"`, `S19mu5358-adbd`, `64a00000.usb-role-switch`, `configs/c.1/f4`, `CONFIG_USB_CONFIGFS_F_FS=y`; `Usb.test_carry`: the three names in CARRY)

- [ ] **Step 2: Run** — `cd tests && python3 -m unittest test_static.Usb -v 2>&1 | tail -8` — Expected: the three FAIL.

- [ ] **Step 3: Make the edits** (the SedGuards rule: each `sed -i` anchored by a `grep -q … || { echo …; exit 1; }` and an idempotence `! grep -q` marker).

- [ ] **Step 4: Run the suite** — `cd tests && python3 -m unittest discover -s . -p 'test_*.py' 2>&1 | tail -3` — Expected: `OK`.

- [ ] **Step 5: Build the image once** to prove customize.sh runs through (the rootfs tar and the kernel build volume are on this Mac):

Run: `tools/make-release.sh local-usb-test 2>&1 | tail -5`
Expected: the `ls -la` of `work/release/local-usb-test` with the eight files and no `no … line to patch` error. (Then `rm -rf work/release/local-usb-test`.)

- [ ] **Step 6: Commit**

```bash
git add openwrt/customize.sh openwrt/usb/mu5358-usb-boot.init installer/mu5358-install.sh docs/GKI-SYSTEM.md docs/MU300-PARITY.md README.md tests/test_static.py
git commit -m "Image: mu5358-usb, the panel's USB object and card, adb on with keys (mu300's adbd on this kernel's FunctionFS), the USB state carried"
```

---

### Task 8: the bench, with the self-undo

**Files:** none in the repository but the docs (`docs/GKI-SYSTEM.md` bench table).

Device: `ssh root@192.168.58.1` (Linux is running, trial of v2026.11.01.1-era system); copies with `ssh root@192.168.58.1 'cat > /tmp/x' < file`. The board's hotspot is the second way in: before the first rebind, confirm the Mac can reach `192.168.1.1` (or the LAN address `mu5358-lan-ip` prints) over the hotspot, and note the SSID.

- [ ] **Step 1: Install the pieces on the running system** (`mu5358-usb` → `/opt/mu5358/bin/` after `mv` of the old one to `/opt/mu5358/lib/usb-adb`; rpcd + ACL + JS; the init file; the customize.sh seds applied with the same `sed` commands by hand; `u30ui2` binary; `/etc/mu5358/adb` on, `adb_auth` keys, `adb_keys` with the Mac's `~/.android/adbkey.pub` through `mu5358-usb adb key -`). `rpcd` restart: `/etc/init.d/rpcd restart`.
- [ ] **Step 2: The storage toggle, armed:** `ssh … 'setsid sh -c "sleep 120; [ -e /run/mu5358/usb-keep ] || /opt/mu5358/bin/mu5358-usb storage off" </dev/null >/dev/null 2>&1 &'`, then `mu5358-usb storage on`. Expected on the Mac within 15 s: `system_profiler SPUSBDataType` shows a "Mass Storage" interface under the `MU5358 Linux USB Ethernet` device with idProduct `0xa4a2`, `diskutil list` shows an external 2.1 GB disk `MU5358`, and `ssh` still answers (the NCM link came back). Then `touch /run/mu5358/usb-keep`. Copy a 100 MB file onto the volume from the Mac, eject it, `mu5358-usb storage mount` on the device, `ls -la /mnt/usb-disk` shows the file, `storage umount`. If the link did not come back after 120 s: the undo rebinds back; if ssh is still gone, go in over the hotspot, note it in GKI-SYSTEM.md as the 2026-10-30 class of failure, and the toggles ship **with a warning in the card and on the screen** rather than being pulled.
- [ ] **Step 3: adb, armed:** `setsid sh -c "sleep 120; [ -e /run/mu5358/usb-keep2 ] || { /opt/mu5358/bin/mu5358-usb adb off; }"`, then `/etc/init.d/mu5358-adbd start`. Expected: `adb devices` on the Mac lists `MU5358LINUX-324761041479` as `device` (the key authorised) and `adb shell id` says root; with the key removed (`adb kill-server; mv ~/.android/adbkey ~/.android/adbkey.bak` — put it back after) the device shows `unauthorized`. `touch /run/mu5358/usb-keep2`. If the rebind costs the link as in step 2, the default stays **off** in customize.sh (change the `echo on` to `off`, keep the keys mode) and the plan's final message says so.
- [ ] **Step 4: The panel and the screen:** LuCI System > Device shows the "USB flash drive" card (state, size, buttons) and the network-mode card; a tap on the screen's settings row flips the state (the log line `u30ui: /opt/mu5358/bin/mu5358-usb storage …`).
- [ ] **Step 5: Record** the measurements (times, what the Mac saw, the host mode line "not tested: needs the OTG adapter") in `docs/GKI-SYSTEM.md`; commit; push; `gh workflow run release.yml --repo dikeckaan/mu5358-linux --ref main`; after the release, the bench update (`mu5358-install.sh take/run update/follow`, `boot-linux`) proves Task 2's init: `cat /etc/mu5358/usb-net-applied` says `ncm`, and with `usb-storage` `on` from step 2 carried, the disk is there right after boot without a rebind (`logread | grep usb-apply` shows `rebind=0`, the Mac's `diskutil list` shows the volume within 30 s of the boot).

The host-mode bench test (`mu5358-usb host` with an OTG adapter and a USB stick, the board on the hotspot, the 60 s self-undo) is the maintainer's to start: it needs the adapter plugged in place of the Mac's cable. It is not part of this plan's completion; its outcome decides whether the LuCI role card is unhidden (a one-line change in the Task 5 sed) in a later plan.
