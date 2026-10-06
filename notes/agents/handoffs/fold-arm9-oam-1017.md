# fold/arm9-oam-1017 — arm9 OAM shard fold

## What

- Class: `OAM` (`include/OAM.h`) — the sprite/OAM manager. Static methods, no
  vtable/RTTI/ctors, so no key-function partition applies.
- Range: `0x02020884..0x02021a04` (arm9), contiguous and exhaustive: 12 `OAM::`
  members, bounded below by `func_02020820` and above by
  `Particle::SysTracker::Contents::Unlink`.
- Staged under `src_tu/`, promoted to `src/engine/oam/OAM.cpp`
  via `tools/tu_promote.py` (12 attribution overrides, 6 CONVERTED identities).
- 12 legacy shards `git rm`'d.

## Verification

- `tools/tubuild.py verify arm9/OAM` — **12/12 MATCH**, objisolate clean,
  reloc destinations clean, emission order ROM-ascending. TEXT-VERIFIED.

## The two things this fold needed that the shards hid

### 1. `#pragma defer_codegen off` + ROM-ascending source order

mwccarm's default is deferred codegen: functions emit in reverse source order
and read optimization pragmas **last-wins at end of file**. Under the naive
merge, `LoadAffineParams`' trailing `opt_strength_reduction off` /
`opt_common_subs off` leaked backward onto `_ZN3OAM6RenderEbP7OamAttriiii
5Fix12IiES3_ii` (which needs `common_subs`/`propagation`/`loop_invariants`
off) — frame came out 8 bytes short, 0x688 vs 0x690.

With `defer_codegen off` (the issue-2411/dCapIcon pattern), pragmas apply
positionally and emission is source order, so the file lists members
ROM-ascending — LoadAffineParams first with its own bracketed offs +
`on` restores, the 10-arg Render mid-file with its three offs + `cplusplus
off/on` island, everything else at defaults.

### 2. `#pragma cplusplus off` for the 10-arg Render

`_ZN3OAM6RenderEbP7OamAttriiii5Fix12IiES3_ii` was a `.c` shard — C front end.
Compiled as C++ (even `extern "C"`) the scalar-parameter homing produces a
different frame (verified: identical instruction stream, every `[sp,#imm]`
−8). The ov062/`daJango_c.cpp` precedent applies: C mode can't spell member
calls, so the island re-declares the mangled callees
(`_ZN3OAM11GetObjWidthEii` etc.) exactly as the shard did.

## Deliberate survivals (byte-driven)

- Both `Fix12`-by-value Render overloads keep literal-mangled definitions
  (`extern "C"` for the C++ one, plain C for the `.c` one). By-value
  `Fix12<int>` params are the documented wall — `include/OAM.h`.
- Four local views of the 8-byte attribute entry: `OamAttrEntry`,
  `OamAttrTpl`, `OamAttrRaw`/`OamEntryRaw`, `OAMEntry`. Shared `OamAttr`
  stays the param type on member signatures; bodies bind their view in one
  cast.
- `data_020755a0`/`ac` stay `unsigned char[]` — the byte-indexed read
  through a 4-stride table is load-bearing.

## One header touched

`include/OAM.h` gained `static void Reset();` — the only member the header
lacked. The shard dodged it with `namespace OAM` (same mangled name), which
cannot coexist with `struct OAM` in one TU. Declaration-only addition; no
consumer's codegen moves.

`include/OamAttr.h`: retired the stale "NONMATCHING … not enrolled" claim on
the 10-arg Render — it matches and is enrolled.

## Bookkeeping

- `config/arm9/delinks.txt`: 12 shard blocks → one `complete` claim
  `src/engine/oam/OAM.cpp` `.text 0x02020884..0x02021a04`.
- `config/tu_manifest.d/arm9/OAM.json`: `promoted`, 12/12, notes record the
  defer_codegen + cplusplus architecture.
- `port/slice_gate6.txt`: six shard paths → `src/engine/oam/OAM.cpp`
  (port_refcheck 403 clean).
- `config/converted-baseline.json`: six legacy shard rows re-keyed to
  `OAM.cpp#symbol`.
- `attribution.json`: twelve `OAM.cpp#symbol` overrides preserve each shard's
  author (lunavyqo / andrewboudreau / tangosdev).
