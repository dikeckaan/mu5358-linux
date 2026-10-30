"""Shared helpers for the tests: the repository's sources, the tools this machine has (node, docker, bash).

Standard library only. Everything that needs a tool that may be missing (Node 20+, docker with a busybox image,
bash) says so in a skip reason instead of failing.
"""
import os
import re
import shutil
import subprocess
from pathlib import Path

TOP = Path(__file__).resolve().parents[1]

# directories that are never sources: device dumps and builds (work/, backup/), third-party code, test files, the
# staged site and the browser's own files
SKIP_DIRS = {'.git', 'work', 'backup', 'tests', 'third_party', 'node_modules', '_site', '.playwright-mcp', '.claude',
             '__pycache__', 'out', 'preview', 'img'}
# extensionless scripts the project runs (no .sh): named here so that a shebang-less one is still checked
EXTRA_SCRIPTS = ('boot/init', 'modem/modem-start', 'modem/mu5358-cell', 'modem/mu5358-modem', 'gpu/gpu-run',
                 'mainline/initramfs/mlinit', 'installer/mu5358-install.sh')


def shebang(p):
    try:
        with open(p, 'rb') as f:
            first = f.readline()
    except OSError:
        return ''
    return first.decode(errors='replace').strip() if first.startswith(b'#!') else ''


def walk(root=TOP):
    """Every file under ROOT that may be a source (the skipped directories left out), sorted."""
    out = []
    for d, dirs, files in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS)
        for f in files:
            out.append(Path(d) / f)
    return sorted(out)


def shell_scripts():
    """[(path, 'sh'|'bash')] of every shell script in the repository: *.sh, and anything with a shell shebang."""
    out = []
    for p in walk():
        sb = shebang(p)
        kind = None
        if 'bash' in sb:
            kind = 'bash'
        elif re.search(r'(^|/)(busybox )?(a|d)?sh\b', sb) or sb.endswith('/sh') or 'rc.common' in sb:
            kind = 'sh'
        elif p.suffix == '.sh':
            kind = 'sh'
        if kind:
            out.append((p, kind))
    for rel in EXTRA_SCRIPTS:
        p = TOP / rel
        if p.is_file() and p not in [x for x, _ in out]:
            out.append((p, 'sh'))
    return sorted(out)


def python_scripts():
    return [p for p in walk() if p.suffix == '.py']


def c_sources():
    return [p for p in walk() if p.suffix in ('.c', '.h')]


def rel(p):
    return str(Path(p).resolve().relative_to(TOP))


# ---- node --------------------------------------------------------------------------------------------------------
NODE = shutil.which('node')


def node_major():
    if not NODE:
        return 0
    try:
        v = subprocess.run([NODE, '--version'], capture_output=True, text=True, timeout=20).stdout.strip()
        return int(v.lstrip('v').split('.')[0])
    except (ValueError, OSError, subprocess.TimeoutExpired):
        return 0


# ---- docker ------------------------------------------------------------------------------------------------------
DOCKER = shutil.which('docker') if os.environ.get('MU5358_TEST_DOCKER', '1') != '0' else None
BUSYBOX_IMAGE = os.environ.get('MU5358_TEST_BUSYBOX', 'busybox:stable')
_docker_state = {}


def docker_busybox():
    """'' when docker can run the busybox image here, else why not (the skip reason)."""
    if 'why' in _docker_state:
        return _docker_state['why']
    why = ''
    if not DOCKER:
        why = 'docker is not on PATH (or MU5358_TEST_DOCKER=0)'
    else:
        try:
            r = subprocess.run([DOCKER, 'info'], capture_output=True, text=True, timeout=30)
            if r.returncode != 0:
                why = 'docker is not running: ' + (r.stderr.strip().splitlines() or ['?'])[-1][:200]
            else:
                r = subprocess.run([DOCKER, 'image', 'inspect', BUSYBOX_IMAGE], capture_output=True, text=True, timeout=30)
                if r.returncode != 0:
                    r = subprocess.run([DOCKER, 'pull', '-q', BUSYBOX_IMAGE], capture_output=True, text=True, timeout=300)
                    if r.returncode != 0:
                        why = f'cannot pull {BUSYBOX_IMAGE}: ' + r.stderr.strip()[-200:]
        except (OSError, subprocess.TimeoutExpired) as e:
            why = f'docker: {e}'
    _docker_state['why'] = why
    return why
