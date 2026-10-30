# Tests

```sh
python3 -m unittest discover -s tests -v          # everything this machine can run
python3 -m unittest discover -s tests -p test_static.py -v   # one module
node --test tests/web/                            # the web installer's JavaScript alone (Node 20+)
MU5358_TEST_DOCKER=0 python3 -m unittest discover -s tests   # without docker (the installer's helpers are skipped)
```

Standard library only (Python 3.9+). The JavaScript tests need Node 20 or newer; the installer's helper tests need
docker and the `busybox:stable` image (pulled on first use; `MU5358_TEST_BUSYBOX` names another). CI runs all of it on
Ubuntu (`.github/workflows/tests.yml`: python3, Node 20, docker), and a `site` job stages the website as the Pages
workflow does and checks the three languages are there.

| file | what |
|---|---|
| `helpers.py` | not a test: the repository's sources (every shell script by shebang or `.sh`, Python, C), Node's version, whether docker can run busybox |
| `test_static.py` | rules over every source without running it. **Syntax**: `sh -n` of every POSIX script (`bash -n` of the bash ones), `py_compile` of every `.py`. **Partitions**: no write (`dd of=`, a redirection, `blkdiscard`, `write_part`, `cp`/`mv`) to `by-name/*_a`, `nr_*nv*`, `prodnv`, `calinv`, `miscdata`, `persist`, `ztepersist`, `userdata`, `super`, `uboot`, `spl` (reads are fine); a write by partition number (`mmcblk0pN`) is judged by the bench unit's table; `misc` is written only on the allow-listed lines (the installer's `bc_write`, which reads the block back; `boot/init`'s restore; `mainline/trial.sh`'s one-shot arm), and every allow-listed line must still exist; the boot chain copied to slot b carries no NV name; the GPT is only touched through `gpt-userdata`. **No telnet**: no `telnetd` in `boot/init` or anywhere under `openwrt/`; init's only shell is on the USB serial console. **The password**: `check` dies with `no password set`, `password` takes only a `$6$` hash (0600), `take` moves the pushed files to root's directory and verifies `SHA256SUMS` and every file before any is used. **Never the default boot**: the trial block is one-shot (`0x2f`), `rebrand.sh` removes `mu300-post`, `mu300-next-boot` and the early recorder, `customize.sh` links none of them back. **sed guards**: every `sed -i` in `customize.sh`/`rebrand.sh` is anchored by a grep that fails loudly, an `if`/`while` on a grep, `grep -q && sed -i`, or `grep -q \|\| continue` followed by a loud check. **Screen strings**: every `X()` row of `tools/u30ui2/i18n.h` has three non-empty strings with the same printf conversions in the same order. **Rebranded**: no `mu300` in the code of `tools/u30ui2`, `modem/`, `installer/`, `boot/init` (comments, and the names `mu300-linux`, `mu300-linux-vpn`, `mu300project`, `mu300-linux-mirror` are allowed) |
| `test_installer.py` | `installer/mu5358-install.sh`'s pure helpers through its `_test FUNCTION ARGS` entry, under busybox `sh` in docker, the repository mounted read-only at `/w`: `bc_trial` of the known Android block gives the known trial block (b: priority 15, 2 tries, not successful; a keeps its successful bit); `bc_state` on android, trial, other and invalid blocks (CRC checked); `zero` fails closed on a short, empty, missing or non-zero file; `password` keeps a valid `$6$` hash 0600 in `/data/adb/mu5358/in` and refuses everything else (eleven shapes); `bootimg_put` refuses what is not a v4 boot image, a payload too large, a ramdisk into an image that holds a kernel, and - with the bench unit's fixtures - makes `work/unit2/linux-b/boot.img` byte for byte from `work/unit2/img/boot_a.img` and `work/kernel/Image`; not root is refused with exit 2; the usage; unknown commands and files |
| `test_web.py` | runs `tests/web/*.test.mjs` under Node (`node --test`) and `node --check`s every module; `strings.js` parsed in Python (same keys, `{n}` fillers and links in en/tr/zh; every sentence `t("...")` asks for exists; no mu300 name); the six pages (`site/`, `site/tr`, `site/zh`, and their `web-installer/`) have their `lang` and all three `hreflang`s, load the same modules and nothing from another origin; `site/tools/stage.sh` into a temporary directory carries the three languages and not `tools/` |
| `web/strings.test.mjs` | `strings.js` imported: key-set equality, fillers, links, nothing empty |
| `web/sha512crypt.test.mjs` | `sha512crypt.js` against Drepper's vector (`$6$saltstring` / "Hello world!") and the file's own; refused salts; `makeSalt`; `isHash` against the installer's regex |
| `web/flow.test.mjs` | `flow.js`: `validPassword` and `passwordProblem`; `parseSums` on the newest `work/release/*/SHA256SUMS` when one is here, else an inline fixture, and on odd input; `parseStatus` on a status line of the bench unit and on hostile values; `classifyLine`; the constants against the script |
| `web/signature.test.mjs` | the release signature (`SHA256SUMS.sig`): written with the release-signing work, run by the same `node --test` |

## What is skipped where

| | skipped when | reason given |
|---|---|---|
| `test_installer.py` (whole module) | no docker on PATH, docker not running, the busybox image cannot be pulled, or `MU5358_TEST_DOCKER=0` | the message names which |
| `test_installer.py` bootimg fixture tests | `work/unit2/img/boot_a.img`, `work/kernel/Image` or `work/unit2/linux-b/boot.img` is not here (they are the bench unit's, never in git) | always skipped in CI |
| `test_web.py` `JavaScript` | Node older than 20 or absent | `Node.js 20 or newer is needed` |
| `test_static.py` bash syntax | no bash | per script |

The partition-number table in `test_static.py` is the bench unit's (`backup/.../partitions.txt`, not in git): a
write to `/dev/block/mmcblk0pN` is judged by the name `pN` has there.
