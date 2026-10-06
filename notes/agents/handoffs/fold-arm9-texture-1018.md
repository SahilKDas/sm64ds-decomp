# fold/arm9-texturetransformer-1018 — arm9 TextureTransformer shard fold

## What

- Class: `TextureTransformer` (`include/TextureTransformer.h`) — the Animation
  child that plays a `BTA_File` against `ModelComponents`. Cartridge RTTI
  spells it `dExtAnmTexSRT_c` (base `dExtFrameCtrl_c`), so the TU compiles
  with `#pragma RTTI off` — the `MaterialChanger`/`dExtAnmMaterial_c`
  precedent, same table row.
- Range: `0x0201587c..0x0201597c` (arm9), contiguous and exhaustive: 6
  members, bounded below by `MaterialChanger::C1` and above by
  `TextureSequence::Prepare`.
- Staged under `src_tu/`, promoted to `src/engine/model/TextureTransformer.cpp`
  via `tools/tu_promote.py` (6 attribution overrides, 5 CONVERTED identities).
- 6 legacy shards `git rm`'d.

## Verification

- `tools/tubuild.py verify arm9/TextureTransformer` — **6/6 MATCH**,
  objisolate clean, reloc destinations clean, emission order ROM-ascending.
  TEXT-VERIFIED.

## Shape

The whole class folds, lifecycle variants included. The ROM's D0,D1,C1 run is
the order one `~TextureTransformer()` plus one `TextureTransformer()` emits
under deferred codegen; the key-function TU emits D2/C2 and the vtable, all
deadstripped unenrolled. Written back-to-front (C1, D1, SetFile, Update,
Prepare) — default deferred codegen needs no `defer_codegen off` here because
no member carries unscoped `opt_*` pragmas.

Deliberate survivals:

- `SetFile` stays a mangled `extern "C"` definition: `Fix12<int>` by value is
  wall 6az. Its `SetAnimation` call keeps the mangled spelling for the same
  reason; `SetFlags` takes a plain int and is the real member call.
- The `.c` shard compiled identically under the C++ front end — no
  `cplusplus` island needed (measured, same as MaterialChanger).

## One header touched

`include/TextureTransformer.h`: retired the stale "shard owns C1" and
"D0 stays a C file" notes — the TU now owns all six.

## Bookkeeping

- `config/arm9/delinks.txt`: 6 shard blocks → one `complete` claim
  `.text 0x0201587c..0x0201597c`.
- `config/tu_manifest.d/arm9/TextureTransformer.json`: `promoted`, 6/6,
  `compiler_only_output` records D2/C2 deadstrip + `_ZTV18TextureTransformer`
  deadstrip-data at `0x0208e7c4`.
- `config/decl-agreement-baseline.json`: two banked callee disagreements
  (`func_020469e8`, `func_02046b64`) re-keyed from the shard paths to the TU;
  the `SetAnimation` shard row dropped — the merged decl agrees.
- `attribution.json`: six `TextureTransformer.cpp#symbol` overrides preserve
  each shard's author.
