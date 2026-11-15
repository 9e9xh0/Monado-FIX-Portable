# Contract: patch format

Each supported game version names its own patch file in `games/<id>/profile.json` (`patches`): `games/<id>/patches.py`
for 2.0.0 and `games/<id>/patches-1.1.2.py` for 1.1.2. A patch file defines `PATCHES = { address: Patch }`, each
`Patch` with:
- `expected`: the expected original bytes (u32). If they don't match, the generator aborts with the address and the
  bytes found.
- `emit(gen, addr) -> str`: the replacement C.
- `req`: the requirement or task that justifies it (mandatory).
- `note`: why.
