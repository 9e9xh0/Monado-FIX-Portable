# Function-local guest state in the lifted code

Ideas from TotK Recomp (`recomp/flags.py`, `regs.py`, `vlocals.py`, MIT), measured here one at a time.

## NZCV flags in a local (done)

`tools/lifter/flags.py` (ported from TotK Recomp) and the `Flags` helpers in `runtime/cpu.h`.

Before, every flag-setting instruction packed N, Z, C and V into `c->nzcv` (`subs64()` and friends) and every reader
unpacked them (`COND_xx(c)`). `Cpu *restrict c` already let clang keep `c->nzcv` in a register inside a function, but
not drop the packing or the store before each call and return. Now a generated function keeps its flags in a local
`Flags F`:

- A compare stores its operands; a reader whose definitions are all of one kind computes its condition from them, so
  `cmp x1, x2; b.lt` is one host compare.
- Definitions only known packed (CCMP, FCMP, MSR NZCV, ADCS) write `F.nz`, as do several kinds reaching one reader.
- `c->nzcv` is written only where the function leaves by a tail branch, a `br` or falling into the next function.
  Calls and returns publish nothing: AAPCS64 code neither passes flags to a callee nor reads a callee's.
- Functions re-entered through the all-instruction dispatch (unresolved computed `br`), patch and randomizer sites
  keep flags in `c->nzcv`, as before.

In the game code, about 95% of flag accesses now use `F` (main: 97,076 local, 2,820 still in the `Cpu`; sdk: 72,540
and 6,307). The executable shrank from 82.0 MB to 72.8 MB.

**Checked** (2026-11-18, deterministic mode, frame hashes): identical on every frame of the new-game route (59,969
frames: boot, title, opening cutscene) and of a save loaded into the field (44,993 frames).

**Measured** (RX 6900 XT, deterministic mode, -O2 without a profile, mean CPU time per frame, 2 runs each):

| | Boot | Title and menus | Field | Whole run |
|---|---|---|---|---|
| -O2 | 2.87 / 2.79 ms | 3.20 / 3.14 ms | 12.09 / 11.86 ms | 5.17 / 5.06 ms |
| -O2 + local flags | 2.84 / 2.79 ms | 3.18 / 3.15 ms | 11.47 / 11.38 ms | 4.99 / 4.94 ms |

Field -4.6%, whole run -2.5%.

The change alters the generated C and `cpu.h`, so it changes `GAMELIB_ID` (players rebuild their game code once) and
makes the published PGO profile stale: retrain it (docs/PGO.md) before release; the release workflow's `pgo` job
fails on a stale profile.

## General registers in locals (not done)

TotK keeps each function's 6 most used X registers and SP in locals and, after a call, reloads only X0-X18 and X30,
trusting AAPCS64's callee-saved X19-X29 and SP. Our earlier attempt with every register in locals (T165) cost 5.6%
more instructions. With `Cpu *restrict c` most of the benefit TotK describes (registers kept across guest stores)
is already there; what remains is mostly the callee-saved trust, which needs proof that nothing changes X19-X29 or SP
of a running function behind its back (longjmp, C++ exceptions, fibers, the runtime writing a running thread's Cpu).
