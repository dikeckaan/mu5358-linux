"""Checks over every source without running it: syntax under the shell that runs it, and the rules the project keeps
(README "Safety rules this project keeps", docs/MU300-PARITY.md D2): slot a, the NV partitions and the GPT are never
written; misc only by the installer's bc_write and boot/init's restore; no telnet; a root password at install; the
rebranded tree carries no mu300 name in code; every sed -i of the customisation is anchored.
Modelled on mu300-linux's tests/test_static.py."""
import os
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

FAKEBIN = Path(__file__).resolve().parent / 'fixtures/fakebin'
from helpers import TOP, c_sources, python_scripts, rel, shell_scripts, walk

INSTALLER = TOP / 'installer' / 'mu5358-install.sh'
INIT = TOP / 'boot' / 'init'
CUSTOMIZE = TOP / 'openwrt' / 'customize.sh'
REBRAND = TOP / 'openwrt' / 'rebrand.sh'
I18N = TOP / 'tools' / 'u30ui2' / 'i18n.h'

# the bench unit's GPT (backup/324761041479-2026-10-29/partitions.txt, not in git): a write by partition number is
# judged by the name it has there
PART_BY_NUMBER = {
    'p1': 'prodnv', 'p2': 'calinv', 'p3': 'miscdata', 'p4': 'misc', 'p5': 'trustos_a', 'p6': 'trustos_b', 'p7': 'sml_a',
    'p8': 'sml_b', 'p9': 'uboot_a', 'p10': 'uboot_b', 'p11': 'uboot_log', 'p12': 'esp_a', 'p13': 'esp_b',
    'p14': 'logo_a', 'p15': 'logo_b', 'p16': 'fbootlogo_a', 'p17': 'fbootlogo_b', 'p18': 'nr_factorynv',
    'p19': 'nr_runtimenv1', 'p20': 'nr_runtimenv2', 'p21': 'nr_downloadnv_a', 'p22': 'nr_downloadnv_b', 'p23': 'persist',
    'p24': 'ztecfg', 'p25': 'ztepersist', 'p26': 'nr_modem_a', 'p27': 'nr_modem_b', 'p28': 'nr_deltanv_a',
    'p29': 'nr_deltanv_b', 'p30': 'nr_phy_a', 'p31': 'nr_phy_b', 'p32': 'nr_phy_npi_a', 'p33': 'nr_phy_npi_b',
    'p34': 'l_agdsp_a', 'p35': 'l_agdsp_b', 'p36': 'pm_sys_a', 'p37': 'pm_sys_b', 'p38': 'ch_sys_a', 'p39': 'ch_sys_b',
    'p40': 'teecfg_a', 'p41': 'teecfg_b', 'p42': 'hypervsior_a', 'p43': 'hypervsior_b', 'p44': 'boot_a', 'p45': 'boot_b',
    'p46': 'vendor_boot_a', 'p47': 'vendor_boot_b', 'p48': 'init_boot_a', 'p49': 'init_boot_b', 'p50': 'dtb_a',
    'p51': 'dtb_b', 'p52': 'dtbo_a', 'p53': 'dtbo_b', 'p54': 'super', 'p55': 'cache', 'p56': 'blackbox', 'p57': 'sd_klog',
    'p58': 'vbmeta_a', 'p59': 'vbmeta_b', 'p60': 'metadata', 'p61': 'sysdumpdb', 'p62': 'fulldumpdb',
    'p63': 'vbmeta_system_a', 'p64': 'vbmeta_system_b', 'p65': 'vbmeta_vendor_a', 'p66': 'vbmeta_vendor_b',
    'p67': 'vbmeta_system_ext_a', 'p68': 'vbmeta_system_ext_b', 'p69': 'vbmeta_product_a', 'p70': 'vbmeta_product_b',
    'p71': 'vbmeta_odm_a', 'p72': 'vbmeta_odm_b', 'p73': 'avbmeta_rs_a', 'p74': 'avbmeta_rs_b', 'p75': 'common_rs1_a',
    'p76': 'common_rs1_b', 'p77': 'common_rs2_a', 'p78': 'common_rs2_b', 'p79': 'common_rs3_a', 'p80': 'common_rs3_b',
    'p81': 'reserve1', 'p82': 'reserve2', 'p83': 'userdata', 'boot0': 'spl_a', 'boot1': 'spl_b',
}
# the names that are never written (README; installer/mu5358-install.sh's header), besides every *_a
NEVER_WRITTEN = {'prodnv', 'calinv', 'miscdata', 'persist', 'ztepersist', 'userdata', 'super', 'uboot', 'spl',
                 'uboot_a', 'uboot_b', 'spl_a', 'spl_b', 'ztecfg', 'nr_factorynv', 'nr_runtimenv1', 'nr_runtimenv2'}
# misc: the only lines that may write it. Each a (file, text the line contains) pair; every one must exist (so the
# list cannot go stale), and the installer's must be inside bc_write.
MISC_WRITERS = (
    ('installer/mu5358-install.sh', 'dd if=$HOME_DIR/bc.bin of=$BYNAME/misc bs=1 seek=2048 count=32 conv=notrunc,fsync'),
    ('boot/init', 'dd if=/mu/misc-bc-android.bin of="$misc" bs=1 seek=2048 count=32 conv=notrunc,fsync'),
    # the paused mainline port's own one-shot trial (mainline/trial.sh: the same 32 bytes at 0x800, read back, and the
    # Android block put back at the end), by partition number from the host over adb
    ('mainline/trial.sh', 'dd if=/data/local/tmp/bc.bin of=/dev/block/mmcblk0p4 bs=1 seek=2048 count=32 conv=notrunc,fsync'),
)


def forbidden_name(name):
    """Why NAME may never be written, or ''. NAME may still hold a shell variable (${_c}_b): judged by what is literal."""
    base = name.strip('"\'')
    if base.endswith('_a'):
        return 'slot a'
    if re.match(r'nr_.*nv', base):
        return 'an NV partition'
    if base in NEVER_WRITTEN:
        return 'never written'
    return ''


def code_lines(path):
    """[(n, code)] of a shell or Python file, comment lines and trailing comments removed (a # after whitespace with
    an even number of quotes before it)."""
    out = []
    for n, line in enumerate(path.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        m = re.search(r'\s#', line)
        if m and line[:m.start()].count('"') % 2 == 0 and line[:m.start()].count("'") % 2 == 0:
            line = line[:m.start()]
        out.append((n, line))
    return out


def c_without_comments(text):
    """The C text with /* */ and // comments blanked (newlines kept, so line numbers hold)."""
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    text = re.sub(r'/\*.*?\*/', blank, text, flags=re.S)
    return re.sub(r'//[^\n]*', blank, text)


class Syntax(unittest.TestCase):
    def test_every_shell_script_parses(self):
        scripts = shell_scripts()
        self.assertGreater(len(scripts), 15, [rel(p) for p, _ in scripts])
        for p, kind in scripts:
            with self.subTest(script=rel(p), shell=kind):
                if kind == 'bash':
                    if not shutil.which('bash'):
                        self.skipTest('no bash')
                    r = subprocess.run(['bash', '-n', str(p)], capture_output=True, text=True, timeout=60)
                else:
                    r = subprocess.run(['sh', '-n', str(p)], capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stderr)

    def test_the_installer_and_init_are_among_the_scripts(self):
        names = {rel(p) for p, _ in shell_scripts()}
        for f in ('installer/mu5358-install.sh', 'boot/init', 'openwrt/customize.sh', 'openwrt/rebrand.sh',
                  'openwrt/build-rootfs.sh', 'modem/modem-start', 'site/tools/stage.sh'):
            self.assertIn(f, names)

    def test_every_python_file_compiles(self):
        files = python_scripts()
        self.assertGreater(len(files), 4)
        for p in files:
            with self.subTest(file=rel(p)):
                r = subprocess.run([sys.executable, '-m', 'py_compile', str(p)], capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stderr)


class Partitions(unittest.TestCase):
    """Slot a, the NV partitions, the GPT and userdata are never written; misc only where it is allowed."""

    # a write's target: dd's of=, a redirection, blkdiscard, the installer's write_part, cp/mv's last argument
    WRITE = re.compile(r'''(?:\bof=|>{1,2}\s*|\bblkdiscard\s+(?:-\S+\s+)*|\bwrite_part\s+\S+\s+|\b(?:cp|mv)\s+(?:-\S+\s+)*\S+\s+)(["']?)(\S+)\1''')

    @classmethod
    def targets(cls, path):
        """[(n, line, partition name)] of every write to a partition in the shell file at PATH."""
        text = path.read_text(encoding='utf-8', errors='replace')
        # VAR=$BYNAME/name, VAR=/dev/block/by-name/name: a later of=$VAR is a write to name
        alias = {}
        for m in re.finditer(r'(?m)^\s*(\w+)=(?:\$\{?BYNAME\}?|/dev/block/by-name)/(\S+)\s*$', text):
            alias[m.group(1)] = m.group(2)
        alias['misc'] = 'misc'    # boot/init: misc=/dev/mmcblk0pN found by its GPT name
        out = []
        for n, line in code_lines(path):
            for m in cls.WRITE.finditer(line):
                t = m.group(2).rstrip(';)&|')
                name = None
                mm = re.match(r'(?:\$\{?BYNAME\}?|/dev/block/by-name)/(.+)$', t)
                if mm:
                    name = mm.group(1)
                mm = re.match(r'/dev/(?:block/)?mmcblk0(p\d+|boot[01])$', t)
                if mm:
                    name = PART_BY_NUMBER.get(mm.group(1), 'mmcblk0' + mm.group(1))
                mm = re.match(r'\$\{?(\w+)\}?$', t)
                if mm and mm.group(1) in alias:
                    name = alias[mm.group(1)]
                if name:
                    out.append((n, line.strip(), name))
        return out

    def test_no_write_to_slot_a_the_nv_partitions_or_the_gpt(self):
        seen = 0
        for p, _ in shell_scripts():
            for n, line, name in self.targets(p):
                seen += 1
                why = forbidden_name(name)
                self.assertFalse(why, f'{rel(p)}:{n}: writes {name} ({why}): {line}')
        self.assertGreater(seen, 10, 'the write detector found almost nothing: is it broken?')

    def test_c_sources_name_no_forbidden_partition(self):
        # the C tools (gpt-userdata, f2fs-shrink) take the device as an argument: no by-name path of a forbidden
        # partition is built into any of them
        for p in c_sources():
            text = c_without_comments(p.read_text(encoding='utf-8', errors='replace'))
            for m in re.finditer(r'/dev/block/by-name/([\w-]+)|/dev/(?:block/)?mmcblk0(p\d+|boot[01])', text):
                name = m.group(1) or PART_BY_NUMBER.get(m.group(2), m.group(2))
                self.assertFalse(forbidden_name(name), f'{rel(p)}: names {name}')
                self.assertNotEqual(name, 'misc', f'{rel(p)}: names misc')

    def test_misc_is_written_only_where_allowed(self):
        allowed = {}
        for f, needle in MISC_WRITERS:
            lines = [n for n, line in code_lines(TOP / f) if needle in line]
            self.assertTrue(lines, f'{f}: the allowed misc write is gone: {needle}')
            allowed[f] = lines
        for p, _ in shell_scripts():
            for n, line, name in self.targets(p):
                if name == 'misc':
                    self.assertIn(n, allowed.get(rel(p), []), f'{rel(p)}:{n}: writes misc outside the allowed lines: {line}')
        # the installer's is inside bc_write, which reads the block back; boot-linux and uninstall go through it
        s = INSTALLER.read_text()
        body = re.search(r'(?ms)^bc_write\(\) \{.*?^\}', s)
        self.assertIsNotNone(body, 'no bc_write() in the installer')
        self.assertIn(MISC_WRITERS[0][1], body.group(0))
        self.assertIn('[ "$(bc_live)" = "$1" ]', body.group(0))
        self.assertEqual(s.count('of=$BYNAME/misc'), 1)
        self.assertEqual(s.count('bc_write "'), 3, 'bc_write calls: boot-linux (and its put-back), uninstall')
        # boot/init: the block it was built with, read back and compared
        init = INIT.read_text()
        self.assertEqual(init.count('of="$misc"'), 1)
        self.assertIn('md5sum < /mu/misc-bc-android.bin', init)
        self.assertIn('stage=misc-restore-FAILED', init)

    def test_the_boot_chain_copied_to_slot_b_has_no_nv_partition(self):
        s = INSTALLER.read_text()
        m = re.search(r'(?s)^CHAIN="([^"]*)"', s, re.M)
        self.assertIsNotNone(m)
        chain = m.group(1).split()
        self.assertGreater(len(chain), 10)
        for c in chain:
            self.assertFalse(re.match(r'nr_.*nv', c), f'CHAIN carries the NV partition {c}')
            self.assertNotIn(c, ('prodnv', 'calinv', 'miscdata', 'persist', 'ztepersist', 'userdata', 'super', 'spl', 'misc'), c)
        # only slot a -> slot b, after a size check, read back
        self.assertIn('dd if=$BYNAME/${_c}_a of=$BYNAME/${_c}_b', s)
        self.assertNotIn('of=$BYNAME/${_c}_a', s)
        self.assertIn('cmp -s $BYNAME/${_c}_a $BYNAME/${_c}_b || die "${_c}_b: read back differs"', s)

    def test_the_gpt_is_only_touched_through_gpt_userdata(self):
        # the GPT: only userdata's end moves, and only gpt-userdata (installer/gpt-userdata.c) writes it
        s = INSTALLER.read_text()
        self.assertNotRegex(s, r'of=\$DISK bs=512 (seek=0 )?count=3[34]\b', 'a raw write over the GPT header')
        self.assertNotRegex(s, r'of=\$DISK\b(?!.*seek=)', 'a write at the start of the disk')
        for n, line in code_lines(INSTALLER):
            if 'of=$DISK' in line:
                self.assertRegex(line, r'seek=\$\(\(R0 / 2048\)\)', f'{n}: a write to the disk outside the region: {line}')
        c = (TOP / 'installer' / 'gpt-userdata.c').read_text()
        self.assertIn('userdata', c)
        self.assertNotIn('by-name', c_without_comments(c))


class NoTelnetAndThePassword(unittest.TestCase):
    def test_no_telnetd_in_init_or_the_openwrt_tree(self):
        files = [INIT] + [p for p in walk(TOP / 'openwrt') if p.is_file()]
        self.assertGreater(len(files), 3)
        for p in files:
            try:
                text = p.read_text(encoding='utf-8')
            except (UnicodeDecodeError, OSError):
                continue
            for n, line in enumerate(text.splitlines(), 1):
                self.assertNotRegex(line, r'\btelnetd\b', f'{rel(p)}:{n}: {line.strip()}')

    def test_init_has_no_shell_on_the_network(self):
        init = INIT.read_text()
        self.assertNotIn('telnetd', init)
        self.assertNotRegex(init, r'\bnc\b.*-[a-z]*l|busybox (nc|httpd)\b')
        # the only shell is on the USB serial console
        self.assertIn("exec sh </dev/ttyGS0 >/dev/ttyGS0", init)

    def test_check_requires_the_password(self):
        s = INSTALLER.read_text()
        body = re.search(r'(?ms)^check\(\) \{.*?^\}', s).group(0)
        self.assertIn('die "no password set"', body)
        self.assertRegex(body, r'\[ -s \$IN/shadow-hash \] \|\| \[ "\$\{MU5358_NO_PASSWORD:-\}" = 1 \] \|\| die "no password set"')
        # the hash is validated as a sha512-crypt and stored 0600; the password itself never reaches the device
        pw = re.search(r'(?ms)^password\(\) \{.*?^\}', s).group(0)
        self.assertIn("grep -qE '^\\$6\\$[./A-Za-z0-9]{1,16}\\$[./A-Za-z0-9]{86}$' || die \"not a sha512-crypt hash\"", pw)
        self.assertIn('umask 077', pw)
        # the install runs check before anything is written
        inst = re.search(r'(?ms)^install\(\) \{.*?^\}', s).group(0)
        self.assertRegex(inst, r'check; backup; space; chain; system; boot_images')
        # and the hash goes into the system's shadow, root's line replaced
        self.assertIn("$1 == \"root\" { $2 = h }", s)
        self.assertIn("grep -qF 'root:$6$' $_r/etc/shadow.new", s)

    def test_take_verifies_sha256sums_before_any_file_is_used(self):
        s = INSTALLER.read_text()
        body = re.search(r'(?ms)^take\(\) \{.*?^\}', s).group(0)
        moved = body.index('mv "$PUSHED/$_f" "$IN/$_f"')
        sums = body.index('[ "$(sha $IN/$F_SUMS)" = "$_want" ] || die "SHA256SUMS is not the release\'s"')
        files = body.index('die "$_f: checksum differs"')
        first_use = min(i for i in (body.find('tar -x'), body.find('cp $IN/'), body.find('sed -n'), body.find('chmod 700 $IN/gpt-userdata')) if i >= 0)
        self.assertLess(moved, sums, 'the files are checked where the adb user can still change them')
        self.assertLess(sums, files)
        self.assertLess(files, first_use, 'a release file is used before its checksum is checked')
        self.assertIn('[ ! -L "$PUSHED/$_f" ]', body)
        self.assertIn('rm -f $IN/*', body)
        # the page's own SHA256SUMS argument is required
        self.assertIn('_want=${1:?take SUMS_SHA256}', body)

    def test_linux_is_never_made_the_default_boot(self):
        # the trial block: b with 2 tries and not successful (0x2f), a's successful bit kept; boot/init puts
        # Android's block back; the tools that re-arm a slot are gone from the OpenWrt tree
        s = INSTALLER.read_text()
        trial = re.search(r'(?ms)^bc_trial\(\) \{.*?^\}', s).group(0)
        self.assertIn('2f', trial)
        self.assertIn('(0x$_a & 0xf0) | 14', trial)
        self.assertNotIn('successful', trial.replace('# ', ''))
        self.assertIn('[ "$_bc" = "$(cat $HOME_DIR/bc-android.hex)" ] || die "misc changed since the installation', s)
        r = REBRAND.read_text()
        self.assertRegex(r, r'for s in [^\n]*\bmu300-post\b')
        self.assertRegex(r, r'for t in [^\n]*\bmu300-next-boot\b')
        self.assertIn('05_mu300_early_recorder', r)
        c = CUSTOMIZE.read_text()
        for name in ('mu5358-post', 'mu5358-next-boot', 'early-recorder'):
            self.assertNotRegex(c, r'ln -sf [^\n]*' + re.escape(name), f'customize.sh links {name}')
        self.assertNotIn('mu5358-post', c.split('for s in', 1)[1].split('; do', 1)[0])


class SedGuards(unittest.TestCase):
    """Every sed -i that patches a file of the OpenWrt tree is anchored: a grep for the line it changes fails loudly
    (grep -q ... || { echo ...; exit 1; }), or the sed is behind an if/while whose condition is a grep, or a
    grep -q ... && sed -i on one line, or a grep -q ... || continue (an entry that is not there is skipped) followed
    by a loud grep that the change took."""

    LOUD = re.compile(r'\bgrep\b.*\|\|\s*\{[^}]*exit 1')

    def unguarded(self, path):
        lines = path.read_text().splitlines()
        out = []
        for i, line in enumerate(lines):
            if 'sed -i' not in line or line.strip().startswith('#'):
                continue
            before = line[:line.index('sed -i')]
            if re.search(r'\bgrep -q\b.*&&\s*$', before):
                continue
            guarded = False
            for j in range(i - 1, max(-1, i - 7), -1):
                prev = lines[j].strip()
                if prev.startswith('#') or not prev:
                    continue
                if 'grep' in prev and (self.LOUD.search(prev) or re.match(r'(if|elif|while)\b', prev) or '| while read' in prev):
                    guarded = True
                    break
                if re.match(r'(if|elif|while)\b.*\bgrep\b', prev):
                    guarded = True
                    break
                if re.search(r'\bgrep -q\b.*\|\|\s*continue$', prev) and j == i - 1:
                    after = [l.strip() for l in lines[i + 1:i + 3]]
                    if any(self.LOUD.search(a) for a in after):
                        guarded = True
                        break
            if not guarded:
                out.append(f'{rel(path)}:{i + 1}: {line.strip()[:100]}')
        return out

    def test_customize_and_rebrand_anchor_every_sed(self):
        for p in (CUSTOMIZE, REBRAND):
            with self.subTest(file=rel(p)):
                n = p.read_text().count('sed -i')
                self.assertGreater(n, 2)
                self.assertEqual(self.unguarded(p), [])

    def test_every_loud_guard_names_its_file(self):
        # a failed anchor says which file and what it looked for
        for p in (CUSTOMIZE, REBRAND):
            for n, line in enumerate(p.read_text().splitlines(), 1):
                if self.LOUD.search(line):
                    self.assertIn('>&2', line, f'{rel(p)}:{n}: the error goes to stdout')
                    self.assertRegex(line, r'echo "\$f: ', f'{rel(p)}:{n}: the error does not name the file')


class ScreenStrings(unittest.TestCase):
    ROW = re.compile(r'X\((S_\w+),\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\)')
    CONV = re.compile(r'%[-+ 0#]*\d*(?:\.\d+)?(?:ll|l|h|z)?[dsuxXfcp%]')

    def rows(self):
        text = I18N.read_text(encoding='utf-8')
        macro = text[text.index('#define STRINGS(X)'):text.index('enum {', text.index('#define STRINGS(X)'))]
        return macro, self.ROW.findall(macro)

    def test_every_row_has_three_strings_with_the_same_conversions(self):
        macro, rows = self.rows()
        self.assertEqual(len(rows), macro.count('X('), 'a row the parser does not read (not three quoted strings?)')
        self.assertGreater(len(rows), 50)
        ids = [r[0] for r in rows]
        self.assertEqual(len(ids), len(set(ids)), 'a duplicate id')
        for id_, en, tr, zh in rows:
            with self.subTest(id=id_):
                for lang, s in (('en', en), ('tr', tr), ('zh', zh)):
                    self.assertTrue(s.strip(), f'{lang} is empty')
                    self.assertNotIn('mu300', s.lower())
                self.assertEqual(self.CONV.findall(tr), self.CONV.findall(en), f'tr: {tr!r} vs {en!r}')
                self.assertEqual(self.CONV.findall(zh), self.CONV.findall(en), f'zh: {zh!r} vs {en!r}')
                self.assertEqual(en.count('%'), len(self.CONV.findall(en)), f'a stray % in {en!r}')

    def test_three_languages_in_order(self):
        text = I18N.read_text(encoding='utf-8')
        self.assertIn('enum { L_EN, L_TR, L_ZH, NLANG };', text)
        self.assertIn('#define X(id, en, tr, zh) id,', text)


class Rebranded(unittest.TestCase):
    """The tree is mu300-linux's renamed (openwrt/rebrand.sh): this project's own sources carry no mu300 name in
    code. Comments may say where something came from; the sibling repository, its VPN module and its Telegram
    channel keep their names."""
    DIRS = ('tools/u30ui2', 'modem', 'installer')
    FILES = ('boot/init',)
    ALLOWED = ('mu300-linux-vpn', 'mu300-linux-mirror', 'mu300-linux', 'mu300project')
    TEXT = ('.c', '.h', '.sh', '.py', '.txt', '')

    def sources(self):
        out = [TOP / f for f in self.FILES]
        for d in self.DIRS:
            out += [p for p in walk(TOP / d) if p.suffix in self.TEXT and p.suffix != '.md']
        return [p for p in out if p.is_file() and not p.name.endswith('.md')]

    def findings(self):
        found = []
        for p in self.sources():
            try:
                text = p.read_text(encoding='utf-8')
            except UnicodeDecodeError:
                continue    # a binary (the built tools)
            if p.suffix in ('.c', '.h'):
                lines = list(enumerate(c_without_comments(text).splitlines(), 1))
            else:
                lines = code_lines(p)
            for n, line in lines:
                code = line
                for a in self.ALLOWED:
                    code = code.replace(a, '')
                if re.search(r'mu300', code, re.I):
                    found.append(f'{rel(p)}:{n}: {line.strip()[:120]}')
        return found

    def test_no_mu300_name_in_code(self):
        self.assertGreater(len(self.sources()), 8)
        self.assertEqual(self.findings(), [])

    def test_the_rebrand_puts_the_external_names_back(self):
        r = REBRAND.read_text()
        for a in ('mu300-linux-vpn', 'mu300project', 'mu300-linux-mirror'):
            self.assertIn(f's/{a.replace("mu300", "mu5358")}/{a}/g', r)
        self.assertIn("s/mu300/mu5358/g; s/MU300/MU5358/g; s/Mu300/Mu5358/g", r)
        self.assertIn('grep -rIl', r)   # text files only: never a binary


if __name__ == '__main__':
    unittest.main()


class Telegram(unittest.TestCase):
    """tools/telegram-announce.sh (mu300's): the token never on a command line, a dry run that shows the message,
    bad tags refused, and the Release workflow announcing its release (a release made with the workflow's token
    fires no release event, so telegram.yml alone would never hear of it)."""
    S = TOP / 'tools/telegram-announce.sh'

    def test_token_never_on_argv(self):
        t = self.S.read_text(encoding='utf-8')
        self.assertIn('curl -sS -K', t)
        self.assertNotRegex(t, r'curl[^\n]*TELEGRAM_BOT_TOKEN')
        self.assertNotRegex(t, r'api\.telegram\.org/bot\$')

    def test_dry_run_message(self):
        if not shutil.which('jq'):
            self.skipTest('no jq')
        env = dict(os.environ, MU5358_TG_DRY='1', TELEGRAM_BOT_TOKEN='x', TELEGRAM_CHAT_ID='1',
                   PATH=f'{FAKEBIN}:{os.environ["PATH"]}')
        out = subprocess.run(['sh', str(self.S), 'release', 'v2026.11.01.1', 'published'], env=env,
                             capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn('MU5358 Linux v2026.11.01.1', out.stdout)
        self.assertIn('web-installer', out.stdout)
        self.assertIn('Pre-release', out.stdout)

    def test_bad_tag_refused(self):
        for tag in ('main', 'v2026.11.01.1;x', '../x'):
            self.assertEqual(subprocess.run(['sh', str(self.S), 'release', tag], capture_output=True).returncode, 2, tag)

    def test_release_workflow_announces(self):
        y = (TOP / '.github/workflows/release.yml').read_text(encoding='utf-8')
        self.assertIn('tools/telegram-announce.sh release "$TAG" published', y)
        self.assertLess(y.index('tools/ci-release.sh'), y.index('telegram-announce.sh'))
        self.assertIn('continue-on-error: true', y[y.index('telegram-announce.sh') - 400:y.index('telegram-announce.sh')])
        self.assertTrue((TOP / '.github/workflows/telegram.yml').is_file())


class KernelFragment(unittest.TestCase):
    """kernel/mu5358-linux.fragment round 1 (design 2026-11-01): userspace I2C for mu5358-charge, spidev, DisplayLink
    DL-1x5, RNDIS; modules or code only. i2c-dev is the one the image loads at boot (openwrt/customize.sh)."""

    def test_round1_modules(self):
        f = (TOP / 'kernel/mu5358-linux.fragment').read_text(encoding='utf-8')
        for line in ('CONFIG_I2C_CHARDEV=m', 'CONFIG_SPI_SPIDEV=m', 'CONFIG_DRM_UDL=m', 'CONFIG_USB_CONFIGFS_RNDIS=y',
                     'CONFIG_USB_F_RNDIS=y'):
            self.assertIn(line + '\n', f)

    def test_i2c_dev_in_image(self):
        c = (TOP / 'openwrt/customize.sh').read_text(encoding='utf-8')
        self.assertIn('gki-modules-disabled/i2c-dev.ko', c)
        self.assertLess(c.index('echo rfkill >>'), c.index('echo i2c-dev >>'))


class Charge(unittest.TestCase):
    """mu5358-charge in the image (a procd service that respawns, after the i2c-dev module), in the release's required
    files, built by the Release workflow, its charge limit carried across updates."""

    def test_in_image_and_release(self):
        c = (TOP / 'openwrt/customize.sh').read_text(encoding='utf-8')
        self.assertIn('opt/mu5358/bin/mu5358-charge', c)
        self.assertIn('init.d/mu5358-charge', c)
        self.assertIn('respawn 3600 5 0', c[c.index('init.d/mu5358-charge'):][:900])
        self.assertIn('tools/charge/mu5358-charge', (TOP / 'tools/make-release.sh').read_text(encoding='utf-8'))
        self.assertIn('/t/charge/build.sh', (TOP / 'tools/ci-release.sh').read_text(encoding='utf-8'))
        self.assertIn('etc/mu5358/charge-limit', (TOP / 'installer/mu5358-install.sh').read_text(encoding='utf-8'))


class ScreenRadioLabel(unittest.TestCase):
    """+COPS AcT 13 is E-UTRAN-NR dual connectivity (5G NSA, what this network gives): the dashboard labels it 5G as
    Android does, not LTE (2026-11-01: 'Android says 5G, Linux said 4G')."""

    def test_act_13_is_5g(self):
        d = (TOP / 'tools/u30ui2/data.c').read_text(encoding='utf-8')
        self.assertNotRegex(d, r'act == 7 \|\| act == 13 \? "LTE"')
        self.assertRegex(d, r'act == 11 \|\| act == 12 \|\| act == 13 \? "5G"')


class ReleaseWorkflow(unittest.TestCase):
    """The Release workflow and tools/ci-release.sh: main only, the key required, the kernel pinned to the commit
    kernel/build.sh names, the release string the vendor modules were loaded into, and the mirror after the release."""

    def setUp(self):
        self.wf = (TOP / '.github/workflows/release.yml').read_text(encoding='utf-8')
        self.ci = (TOP / 'tools/ci-release.sh').read_text(encoding='utf-8')

    def test_kmi_reference_is_the_committed_one(self):
        """kernel/build.sh copies the build's Module.symvers over kernel/Module.symvers (the record, on the Mac): the
        CI gate must compare against the committed file, taken before the build, not against what the build wrote."""
        self.assertRegex(self.ci, r'git show "?\$\{?GITHUB_SHA[^\n]*:kernel/Module\.symvers[^\n]*> "\$W/ci/Module\.symvers\.ref"')
        self.assertLess(self.ci.index('Module.symvers.ref"'), self.ci.index('bash /work/build.sh'))
        self.assertIn('"$KOUT/Module.symvers" "$W/ci/Module.symvers.ref"', self.ci)

    def test_fragment_gate(self):
        """An option olddefconfig dropped from kernel/mu5358-linux.fragment fails the release, after the KMI check."""
        self.assertIn('fragment-rejected.txt', self.ci)
        self.assertLess(self.ci.index('KMI:'), self.ci.index('fragment-rejected.txt'))
        self.assertRegex(self.ci, r"grep -q \. kernel/fragment-rejected\.txt && \{[^\n]*exit 1")

    def test_workflow_runs_on_arm64_from_main_with_the_key_and_contents_write(self):
        self.assertIn('runs-on: ubuntu-24.04-arm', self.wf)
        self.assertIn('workflow_dispatch:', self.wf)
        self.assertNotIn('\n  push:', self.wf, 'a release is only ever made when asked')
        self.assertIn('refs/heads/main', self.wf)
        self.assertIn('contents: write', self.wf)
        self.assertIn('secrets.MU5358_RELEASE_KEY', self.wf)
        self.assertIn('tools/ci-release.sh', self.wf)
        # inputs reach the shell through the environment, never inline in a run: line
        for m in re.finditer(r'run: (.*)', self.wf):
            self.assertNotIn('inputs.tag', m.group(1))

    def test_ci_release_pins_the_kernel_commit_of_build_sh(self):
        short = re.search(r'android15-6\.6 at ([0-9a-f]{12})', (TOP / 'kernel/build.sh').read_text(encoding='utf-8')).group(1)
        full = re.search(r'^ACK_COMMIT=([0-9a-f]{40})', self.ci, re.M).group(1)
        self.assertTrue(full.startswith(short), f'{full} is not {short}')
        self.assertIn('KREL_WANT=6.6.98-android15-8-mu5358-4k', self.ci)
        self.assertIn('kernel/Module.symvers', self.ci)
        self.assertIn('MU5358_REQUIRE_SIGNATURE=1', self.ci)
        self.assertLess(self.ci.rindex('gh release create'), self.ci.rindex('tools/publish-mirror.sh'))
