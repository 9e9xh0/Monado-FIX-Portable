"""Generate C for every module the game profile marks `lift` (T011/T013). Output: generated/<module>/*.c

Usage: gen_all.py [--profile games/xcde/profile.json] [--exefs targets/exefs] [--out generated] [--version X]
Prints a sha256 over the generated tree (names + contents) so a run can be compared with another (NFR-006).
The version is detected from the exefs build ids unless --version names one; its modules, heap and patch file
come from the matching entry of the game profile (games/xcde/profile.json).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'extract'))
import check_dump  # noqa: E402


def tree_hash(path):
    h = hashlib.sha256()
    for dirpath, dirs, files in os.walk(path):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(dirpath, f)
            h.update(os.path.relpath(p, path).replace(os.sep, '/').encode())
            with open(p, 'rb') as fh:
                h.update(hashlib.sha256(fh.read()).digest())
    return h.hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--profile', default=os.path.join(ROOT, 'games', 'xcde', 'profile.json'))
    ap.add_argument('--exefs', default=os.path.join(ROOT, 'targets', 'exefs'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'generated'))
    ap.add_argument('--patches')  # default: the patch file named by the detected version's profile entry
    ap.add_argument('--hle', default=os.path.join(ROOT, 'games', 'xcde', 'hle_symbols.txt'))
    ap.add_argument('--version', help='force a supported version (default: detect from --exefs)')
    ap.add_argument('--simd', default='c', choices=('c', 'asm'))
    a = ap.parse_args(argv)
    prof = json.load(open(a.profile, encoding='utf-8'))
    if a.version:
        try:
            entry = check_dump.version_entry(prof, a.version)
        except KeyError as error:
            print(error, file=sys.stderr)
            return 2
    else:
        entry = check_dump.detect_version(prof, a.exefs)
        if entry is None:
            print(f'gen_all: the exefs in {a.exefs} is not a supported game version '
                  f'({", ".join(v["version"] for v in check_dump.versions(prof))})', file=sys.stderr)
            return 2
    patches = a.patches or os.path.join(ROOT, 'games', 'xcde', entry.get('patches', 'patches.py'))
    print(f'game version {entry["version"]}: modules ' +
          ', '.join(m['name'] for m in entry['modules'] if m['lift']) + f'; patches {patches}')
    gen = os.path.join(ROOT, 'tools', 'lifter', 'gen.py')
    procs = []
    t0 = time.time()
    for m in entry['modules']:
        if not m['lift']:
            continue
        out = os.path.join(a.out, m['name'])
        cmd = [sys.executable, '-E', '-s', gen, m['name'], os.path.join(a.exefs, m['name']), m['base'], out,
               '--simd', a.simd, '--patches', patches, '--hle', a.hle,
               '--game-version', entry['version']]
        log = open(os.path.join(a.out, m['name'] + '.log'), 'w') if os.path.isdir(a.out) else None
        if log is None:
            os.makedirs(a.out, exist_ok=True)
            log = open(os.path.join(a.out, m['name'] + '.log'), 'w')
        procs.append((m['name'], subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT), log))
    rc = 0
    for name, p, log in procs:
        r = p.wait()
        log.close()
        print(f'{name}: exit {r}')
        rc = rc or r
    print(f'generated in {time.time() - t0:.0f}s')
    if rc == 0:
        for m in entry['modules']:
            if m['lift']:
                print(f'{m["name"]}: sha256 {tree_hash(os.path.join(a.out, m["name"]))}')
    return rc


if __name__ == '__main__':
    sys.exit(main())
