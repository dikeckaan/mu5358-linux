#!/usr/bin/env python3
"""The stock boot image (boot_a, v4 with an AVB footer) with its kernel replaced: for the mainline probes and
kernels, written to boot_b only (one-shot trials, as for the GKI kernel).

usage: mkboot.py STOCK_BOOT KERNEL OUT
"""
import importlib.util, os, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('build_initboot', os.path.join(here, '..', 'boot', 'build-initboot.py'))
bi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bi)

stock, kernel, out = sys.argv[1:4]
k = open(kernel, 'rb').read()
if k[0x38:0x3c] != b'ARM\x64':
    sys.exit(f'{kernel}: not an arm64 Image')
img = bi.replace_payload(open(stock, 'rb').read(), k, 'kernel')
open(out, 'wb').write(img)
print(f'{out}: kernel {len(k)} bytes, image {len(img)} bytes')
