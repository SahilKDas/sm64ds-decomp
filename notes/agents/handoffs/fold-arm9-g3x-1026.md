# fold/arm9-g3x-1026 — arm9/G3X

Folded G3X (the 3D engine's fog and clear-color register writer set) into
`src/engine/gx/G3X.cpp`, claiming `0x02055574..0x02055624`.

- 4 functions: SetClearColor, func_020555a4, SetFogTable, SetFog.
  4/4 MATCH, objisolate clean, reloc-destinations clean, ROM-ascending
  emission (source written descending for deferred codegen).
- func_020555a4 is a file-local helper (signature `(void* dst)`, snapshots
  the register block at 0x4000380 through MultiCopyHalf): folded under its
  own func_ name in `extern "C"` so it stays unmangled — the same absorbed-
  shard disposition as dBgPi's func_ members, minus the rename its signature
  does not support.
- New `include/G3X.h` declares the three all-static members; no lifecycle,
  no virtuals, no RTTI — the TU emits nothing else, so the manifest has no
  compiler_only_output rows.
- SetClearColor keeps `*(int *)&e` on the bool parameter — the byte-proven
  body from the shard (bool-widening codegen form).
- delinks: four shard rows replaced by one complete TU claim.
- attribution.json: four path#symbol overrides carry each shard's original
  credit (SetFogTable stays ruspecial's).
- converted-baseline: three member identities re-keyed to
  src/engine/gx/G3X.cpp#symbol (func_020555a4 was a plain C function, not a
  converted member).
- decl-agreement baseline: two re-keys (Copy32Bytes, MultiCopyHalf — the
  TU's local externs moved paths) and eight newly-banked rows for callers
  whose stale `int` spellings the now-visible member definitions expose
  (SetClearColor param #5 and SetFog param #1 are `bool` per the mangled
  names; the caller externs predate the header). The whole-tree --update was
  avoided because it also drops 316 rows owned by other folds; the baseline
  edit is surgical.
- Boundaries: func_020554bc below and func_02055624 above are free-function
  neighbours, not members. symbols.txt lists exactly three _ZN3G3X symbols.
