#!/usr/bin/env python3
"""Build the unicorn library the Windows first-start builder ships, with tools/unicorn/*.patch applied.

    python tools/build_unicorn.py [--out build/unicorn] [--install] [--check] [--skip-build]

GitHub #104: unicorn 2.1.4 (the newest release) reserves its 1 GiB code buffer on Windows and commits it on demand
from a vectored exception handler. On some PCs that first access violation was never resolved and Windows stopped
monado-gamebuild.exe (0xC0000409) at its first uc_mem_map. The patch commits a 64 MiB buffer up front instead, which
is what upstream made the default after 2.1.4 (unicorn-engine/unicorn#2364).

Only the AArch64 target is built (all the lifter emulates), which keeps the build to a minute or two. The library
must match the unicorn Python package (same version): --install replaces the package's own library in place, so
PyInstaller's --collect-all unicorn picks it up. --check runs a short emulation in a fresh interpreter with
faulthandler on: with the patch there is no first-touch access violation, so any reported fault fails the check.

Windows needs CMake and Visual Studio 2022 (the generator the official wheels use); other hosts use Ninja and the
system compiler (useful only to test this script: their unicorn has no such handler).
"""
import argparse
import hashlib
import importlib.metadata
import importlib.util
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = '2.1.4'
URL = f'https://github.com/unicorn-engine/unicorn/archive/refs/tags/{VERSION}.tar.gz'
SHA256 = 'ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4'
PATCHES = sorted((ROOT / 'tools' / 'unicorn').glob('*.patch'))
WINDOWS = os.name == 'nt'
LIBRARY = 'unicorn.dll' if WINDOWS else 'libunicorn.so.2'

CHECK = r'''
import faulthandler, io, os, sys, tempfile
log = tempfile.TemporaryFile(mode='w+')
faulthandler.enable(file=log, all_threads=True)
import unicorn as uc
from unicorn import arm64_const as A
from unicorn.unicorn_py3 import unicorn as binding
mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
mu.mem_map(0x7000000000, 0x200000)               # (the lifter's first call: unicorn initialises here)
mu.mem_write(0x7000000000, bytes.fromhex('400580d2'))   # mov x0, #42
mu.emu_start(0x7000000000, 0, count=1)
x0 = mu.reg_read(A.UC_ARM64_REG_X0)
size = mu.ctl_get_tcg_buffer_size()
faulthandler.disable()
log.seek(0)
faults = log.read()
print(f'unicorn {uc.__version__}: x0={x0}, code buffer {size >> 20} MiB, library {binding.uclib._name}')
if faults:
    print(faults)
patched = os.name != 'nt' or size == 64 << 20   # (the patch's Windows default: proves this library is the one loaded)
if not patched:
    print('the loaded unicorn is not the patched build (code buffer is not 64 MiB)')
sys.exit(0 if x0 == 42 and not faults and patched else 1)
'''


def run(cmd, **kw):
    print('+', ' '.join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), check=True, **kw)


def fetch_source(work):
    request = urllib.request.Request(URL, headers={'User-Agent': 'Monado-build'})
    with urllib.request.urlopen(request, timeout=300) as response:
        data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SHA256:
        raise SystemExit(f'{URL}: SHA-256 {digest}, expected {SHA256}')
    archive = work / f'unicorn-{VERSION}.tar.gz'
    archive.write_bytes(data)
    with tarfile.open(archive) as tar:
        tar.extractall(work, filter='data')
    return work / f'unicorn-{VERSION}'


def apply_patches(src):
    # git apply works outside a repository (the work folder is a temporary one, never inside this checkout). The
    # patches are applied with LF line endings whatever the checkout did to them (core.autocrlf on Windows runners).
    for patch in PATCHES:
        normalized = src.parent / patch.name
        normalized.write_bytes(patch.read_bytes().replace(b'\r\n', b'\n'))
        run(['git', 'apply', '--verbose', str(normalized)], cwd=src)


def build(out):
    with tempfile.TemporaryDirectory(prefix='monado-unicorn-') as tmp:
        work = Path(tmp)
        src = fetch_source(work)
        apply_patches(src)
        build_dir = work / 'build'
        args = ['cmake', '-S', src, '-B', build_dir, '-DCMAKE_BUILD_TYPE=Release', '-DUNICORN_ARCH=aarch64',
                '-DUNICORN_BUILD_TESTS=OFF', '-DUNICORN_INSTALL=OFF', '-DBUILD_SHARED_LIBS=ON',
                '-DUNICORN_LEGACY_STATIC_ARCHIVE=OFF']
        args += ['-G', 'Visual Studio 17 2022', '-A', 'x64'] if WINDOWS else ['-G', 'Ninja']
        run(args)
        run(['cmake', '--build', build_dir, '--config', 'Release', '--parallel'])
        found = [p for p in build_dir.rglob(LIBRARY) if p.is_file()]
        if not found:
            raise SystemExit(f'unicorn: the build produced no {LIBRARY}')
        out.mkdir(parents=True, exist_ok=True)
        shutil.copy2(found[0], out / LIBRARY)
    (out / 'BUILD.txt').write_text(f'unicorn {VERSION} (aarch64 only) with: '
                                   + ', '.join(p.name for p in PATCHES) + '\n', encoding='utf-8')
    print(f'unicorn {VERSION}: {out / LIBRARY}')


def package_lib_dir():
    """The installed unicorn package's lib/ folder, found without importing it (that would load and lock the DLL)."""
    installed = importlib.metadata.version('unicorn')
    if installed != VERSION:
        raise SystemExit(f'unicorn: the Python package is {installed}, the patched library is {VERSION}')
    spec = importlib.util.find_spec('unicorn')
    if spec is None or not spec.submodule_search_locations:
        raise SystemExit('unicorn: the Python package is not installed')
    lib = Path(list(spec.submodule_search_locations)[0]) / 'lib'
    if not (lib / LIBRARY).is_file():
        raise SystemExit(f'unicorn: {lib / LIBRARY} not found (unexpected package layout)')
    return lib


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'unicorn')
    ap.add_argument('--skip-build', action='store_true', help='use the library already in --out (a CI cache hit)')
    ap.add_argument('--install', action='store_true', help="replace the Python package's library with it")
    ap.add_argument('--check', action='store_true', help='emulate one instruction with faulthandler on')
    a = ap.parse_args(argv)
    out = a.out.resolve()
    if not a.skip_build:
        build(out)
    elif not (out / LIBRARY).is_file():
        raise SystemExit(f'unicorn: --skip-build but {out / LIBRARY} is missing')
    if a.install:
        lib = package_lib_dir()
        shutil.copy2(out / LIBRARY, lib / LIBRARY)
        shutil.copy2(out / 'BUILD.txt', lib / 'MONADO_BUILD.txt')
        print(f'unicorn: installed {out / LIBRARY} -> {lib / LIBRARY}')
    if a.check:
        print('+ check: one ARM64 instruction with faulthandler on', flush=True)
        subprocess.run([sys.executable, '-c', CHECK], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
