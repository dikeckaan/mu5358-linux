"""The web installer (site/web-installer, site/assets/web-installer) and the website: the JavaScript tests of
tests/web under Node 20+ (skipped without it), what can be checked about the pages and strings.js without a
browser, and the staged site (site/tools/stage.sh) carrying the three languages."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from helpers import NODE, TOP, node_major, rel

WI = TOP / 'site/assets/web-installer'
PAGES = {'en': TOP / 'site/web-installer/index.html', 'tr': TOP / 'site/tr/web-installer/index.html',
         'zh': TOP / 'site/zh/web-installer/index.html'}
HOME = {'en': TOP / 'site/index.html', 'tr': TOP / 'site/tr/index.html', 'zh': TOP / 'site/zh/index.html'}
LANG_ATTR = {'en': 'en', 'tr': 'tr', 'zh': 'zh-Hans'}


def strings():
    """{lang: {key: text}} out of strings.js (three object literals of "key": "text" pairs)"""
    src = (WI / 'strings.js').read_text(encoding='utf-8')
    out = {}
    for lang in ('en', 'tr', 'zh'):
        body = src[src.index(f'const {lang} = {{'):]
        body = body[:body.index('\n};')]
        out[lang] = dict(re.findall(r'"([a-z0-9._-]+)":\s*"((?:[^"\\]|\\.)*)"', body))
    return out


@unittest.skipIf(node_major() < 20, 'Node.js 20 or newer is needed')
class JavaScript(unittest.TestCase):
    def test_node_tests(self):
        files = sorted(str(p) for p in (TOP / 'tests/web').glob('*.test.mjs'))
        self.assertGreaterEqual(len(files), 3)
        r = subprocess.run([NODE, '--test', '--test-timeout=120000'] + files, capture_output=True, text=True, timeout=600, cwd=TOP)
        # (the reporter depends on whether Node writes to a terminal: "not ok" lines of TAP, or the ✖ of spec)
        failed = '\n'.join(l for l in r.stdout.splitlines() if l.startswith(('not ok', '✖')) or 'Error' in l)[-6000:]
        self.assertEqual(r.returncode, 0, failed + r.stderr[-2000:])
        self.assertRegex(r.stdout, r'(?m)^(ℹ|#) fail 0$')
        self.assertRegex(r.stdout, r'(?m)^(ℹ|#) pass [1-9]\d*$')

    def test_every_module_parses(self):
        files = sorted(WI.rglob('*.js'))
        self.assertGreater(len(files), 8)
        for f in files:
            r = subprocess.run([NODE, '--check', str(f)], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, f'{rel(f)}: {r.stderr}')


class Strings(unittest.TestCase):
    def setUp(self):
        self.s = strings()

    def test_three_languages_same_keys_and_placeholders(self):
        en = self.s['en']
        self.assertGreater(len(en), 100)
        for lang in ('tr', 'zh'):
            self.assertEqual(sorted(set(en) - set(self.s[lang])), [], f'missing in {lang}')
            self.assertEqual(sorted(set(self.s[lang]) - set(en)), [], f'only in {lang}')
            for k, v in en.items():
                self.assertEqual(sorted(set(re.findall(r'\{\d\}', v))), sorted(set(re.findall(r'\{\d\}', self.s[lang][k]))), f'{lang} {k}')
                self.assertEqual(v.count('['), self.s[lang][k].count('['), f'{lang} {k}: the link brackets')
                if v:
                    self.assertTrue(self.s[lang][k].strip(), f'{lang} {k} is empty')

    def test_every_sentence_the_code_asks_for_exists(self):
        en = self.s['en']
        asked = set()
        for name in ('app.js', 'device.js', 'demo.js'):
            src = (WI / name).read_text(encoding='utf-8')
            asked |= set(re.findall(r'(?<![A-Za-z.])t\("([a-z0-9-]+\.[a-z0-9._-]+)"', src))
        self.assertGreater(len(asked), 20)
        self.assertEqual(sorted(k for k in asked if not k.endswith('.') and k not in en), [])

    def test_no_mu300_name_in_the_strings(self):
        for lang, table in self.s.items():
            for k, v in table.items():
                self.assertNotRegex(v.replace('mu300-linux', ''), r'mu300', f'{lang} {k}')


class Pages(unittest.TestCase):
    def test_three_pages_one_per_language(self):
        for lang, p in list(PAGES.items()) + list(HOME.items()):
            with self.subTest(page=rel(p)):
                self.assertTrue(p.is_file(), f'{rel(p)} is missing')
                html = p.read_text(encoding='utf-8')
                self.assertIn(f'<html lang="{LANG_ATTR[lang]}">', html)
                for other in LANG_ATTR.values():
                    self.assertIn(f'hreflang="{other}"', html)

    def test_the_installer_pages_load_the_same_modules_and_nothing_from_elsewhere(self):
        mods = {}
        for lang, p in PAGES.items():
            html = p.read_text(encoding='utf-8')
            srcs = re.findall(r'<script[^>]*\ssrc="([^"]+)"', html) + re.findall(r'<link[^>]*\shref="([^"]+\.css)"', html)
            self.assertTrue(srcs, rel(p))
            for s in srcs:
                self.assertFalse(re.match(r'(https?:)?//', s), f'{rel(p)} loads {s} from elsewhere')
            mods[lang] = sorted(Path(s).name for s in srcs)
            self.assertNotIn('<iframe', html)
        self.assertEqual(mods['tr'], mods['en'])
        self.assertEqual(mods['zh'], mods['en'])

    def test_the_staged_site_has_the_three_languages(self):
        # the Pages workflow publishes what site/tools/stage.sh stages (site/ without its tools)
        tmp = Path(tempfile.mkdtemp(prefix='mu5358-site-'))
        try:
            r = subprocess.run(['sh', str(TOP / 'site/tools/stage.sh'), str(tmp)], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stderr)
            for f, lang in (('index.html', 'en'), ('tr/index.html', 'tr'), ('zh/index.html', 'zh-Hans'),
                            ('web-installer/index.html', 'en'), ('tr/web-installer/index.html', 'tr'),
                            ('zh/web-installer/index.html', 'zh-Hans')):
                self.assertTrue((tmp / f).is_file(), f'{f} is not staged')
                self.assertIn(f'<html lang="{lang}">', (tmp / f).read_text(encoding='utf-8'))
            self.assertFalse((tmp / 'tools').exists(), 'site/tools is staged')
            self.assertTrue((tmp / 'assets/web-installer/strings.js').is_file())
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
