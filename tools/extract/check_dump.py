"""Validate a user-supplied dump against a game profile (FR-001..004, FR-006).

Accepts a folder holding exefs/ and romfs/ (or --exefs/--romfs separately). Never reads encrypted containers
(.nsp/.xci/.nca/.nsz/.xcz) and never opens key files (Constitution art. II).

Exit codes: 0 ok, 1 usage, 2 container, 3 exefs, 4 version, 5 romfs.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from manifest import build_manifest  # noqa: E402

ERR_OK, ERR_USAGE, ERR_CONTAINER, ERR_EXEFS, ERR_VERSION, ERR_ROMFS = 0, 1, 2, 3, 4, 5
CONTAINER_EXT = ('.nsp', '.xci', '.nca', '.nsz', '.xcz')


class Result:
    def __init__(self):
        self.ok = True
        self.code = ERR_OK
        self.messages = []
        self.version = None       # the supported version the dump matched
        self.version_entry = None  # its entry in the game profile

    def fail(self, code, *msgs):
        self.ok = False
        self.code = code
        self.messages.extend(msgs)
        return self


def versions(profile):
    """The supported versions: the top-level profile first, then its `versions` list.

    A profile without `versions` is one version (the old layout); a profile with it keeps
    the top-level fields as the default version, so readers that predate the list still work.
    """
    out = [profile]
    out.extend(profile.get('versions', []))
    return out


def version_entry(profile, name=None):
    """Select one version entry (default: the profile's `default_version` or top level)."""
    if name is None:
        name = profile.get('default_version') or profile.get('version') or versions(profile)[0]['version']
    for entry in versions(profile):
        if entry['version'] == name:
            return entry
    raise KeyError(f'version {name!r} is not in the game profile')


def _matches(entry, ids):
    return all(ids.get(m['name']) == m['build_id'][:16] for m in entry['modules'])


def detect_version(profile, exefs):
    """The supported version entry whose module build-ids match `exefs`, or None."""
    ids = {m['name']: _read_build_id(os.path.join(exefs, m['name'])) for m in profile['modules']}
    for entry in versions(profile):
        if _matches(entry, ids):
            return entry
    return None


def _expected_versions(profile):
    parts = []
    for entry in versions(profile):
        main = next((m for m in entry['modules'] if m['name'] == 'main'), None)
        parts.append(f'{entry["version"]} (main build-id {main["build_id"][:16]})' if main
                     else entry['version'])
    return ', '.join(parts)


def _containers_in(path):
    try:
        return [n for n in os.listdir(path) if n.lower().endswith(CONTAINER_EXT)]
    except OSError:
        return []


def _read_build_id(path):
    """First 8 bytes of the NSO build id (offset 0x40), or None if the file is not an NSO."""
    try:
        with open(path, 'rb') as f:
            head = f.read(0x48)
    except OSError:
        return None
    if len(head) < 0x48 or head[:4] != b'NSO0':
        return None
    return head[0x40:0x48].hex()


def check_dump(path, profile, exefs=None, romfs=None):
    r = Result()
    if path is not None:
        low = path.lower()
        if os.path.isfile(path):
            if low.endswith(CONTAINER_EXT):
                return r.fail(ERR_CONTAINER,
                              f'"{os.path.basename(path)}" is a container ({os.path.splitext(path)[1]}): '
                              'the extracted content is needed (exefs and romfs folders). The file was not read.')
            return r.fail(ERR_USAGE, 'Expected a folder with exefs/ and romfs/.')
        if not os.path.isdir(path):
            return r.fail(ERR_USAGE, f'No such folder: {path}')
        exefs = exefs or (os.path.join(path, 'exefs') if os.path.isdir(os.path.join(path, 'exefs')) else None)
        romfs = romfs or (os.path.join(path, 'romfs') if os.path.isdir(os.path.join(path, 'romfs')) else None)
        if exefs is None and os.path.isfile(os.path.join(path, 'main')):
            exefs = path  # the folder itself is the exefs
        if exefs is None and romfs is None and _containers_in(path):
            return r.fail(ERR_CONTAINER,
                          'The folder only holds encrypted containers (' + ', '.join(_containers_in(path)) + '). '
                          'The extracted content is needed (exefs and romfs).')
    if not exefs or not os.path.isdir(exefs):
        return r.fail(ERR_EXEFS, 'The exefs folder is missing.')

    # exefs: required files, NSO magic
    missing = [n for n in profile['exefs_required'] if not os.path.isfile(os.path.join(exefs, n))]
    if missing:
        return r.fail(ERR_EXEFS, 'Files missing in exefs: ' + ', '.join(missing))
    ids = {}
    for m in profile['modules']:
        bid = _read_build_id(os.path.join(exefs, m['name']))
        if bid is None:
            return r.fail(ERR_EXEFS, f'exefs/{m["name"]} is not a valid NSO.')
        ids[m['name']] = bid

    # version by build id (each supported version names all modules it applies to)
    matches = [entry for entry in versions(profile) if _matches(entry, ids)]
    if not matches:
        lines = [f'Unsupported version: supported are {_expected_versions(profile)}.']
        ref = versions(profile)[0]
        lines += [f'  {m["name"]}: build-id found {ids.get(m["name"])}, expected {m["build_id"][:16]}'
                  for m in ref['modules'] if ids.get(m['name']) != m['build_id'][:16]]
        return r.fail(ERR_VERSION, *lines)
    entry = matches[0]

    # romfs: aggregate manifest (the build ids already picked the version; with several exefs
    # matches the romfs decides between them)
    if not romfs or not os.path.isdir(romfs):
        return r.fail(ERR_ROMFS, 'The romfs folder is missing.')
    got = build_manifest(romfs)
    if len(matches) > 1:
        for candidate in matches:
            if got['sha256'] == candidate.get('romfs_manifest', profile['romfs_manifest'])['sha256']:
                entry = candidate
                break
    r.version = entry['version']
    r.version_entry = entry
    exp = entry.get('romfs_manifest', profile['romfs_manifest'])
    if got['sha256'] == exp['sha256']:
        r.messages.append(f'OK: version {entry["version"]}, {got["files"]} files in romfs.')
        return r
    msgs = [f'The romfs does not match version {entry["version"]}.']
    for name, (n, b) in exp['dirs'].items():
        g = got['dirs'].get(name)
        if g is None:
            msgs.append(f"  folder '{name}' is missing ({n} files)")
        elif g[0] != n:
            msgs.append(f"  '{name}': {g[0]} files, {n} expected ({n - g[0]} missing)" if g[0] < n
                        else f"  '{name}': {g[0]} files, {n} expected ({g[0] - n} extra)")
        elif g[1] != b:
            msgs.append(f"  '{name}': the same number of files but {g[1]} bytes instead of {b} (truncated or modified?)")
    for name in got['dirs']:
        if name not in exp['dirs']:
            msgs.append(f"  unexpected folder '{name}'")
    if len(msgs) == 1:
        msgs.append('  the same counts and sizes per folder, but different file names.')
    return r.fail(ERR_ROMFS, *msgs)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dump', nargs='?')
    ap.add_argument('--exefs')
    ap.add_argument('--romfs')
    ap.add_argument('--profile', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                                                       'games', 'xcde', 'profile.json'))
    a = ap.parse_args(argv)
    with open(a.profile, encoding='utf-8') as f:
        profile = json.load(f)
    res = check_dump(a.dump, profile, exefs=a.exefs, romfs=a.romfs)
    for m in res.messages:
        print(m)
    return res.code


if __name__ == '__main__':
    sys.exit(main())
