# arm9/G3i fold — handoff

Branch `fold/arm9-g3i-1024`. One class: `G3i`, the geometry engine's
immediate-mode matrix writers — `LookAt_` (camera basis -> position-matrix
port) and `PerspectiveW_` (division-unit perspective -> projection port).
Both matched byte-identical on first compile; the fold needed one policy row.

## Shape

- `src/engine/gx/G3i.cpp`, promoted TU claiming `0x02055a64..0x02055dec`.
- `LookAt_` was a `namespace G3i` dodge in its shard; it is now a real
  `G3i::LookAt_` static-member definition under the new `include/G3i.h`
  (`struct G3i`, all-static — the `G2x.h`/`OAM.h` shape).
- `PerspectiveW_` keeps its literal-mangled `extern "C"` name and parses
  under `#pragma cplusplus off`: six by-value `Fix12<int>` parameters (the
  OAM.h ABI wall) plus a C-front-end ROM body. `G3i.h` deliberately does not
  declare it; callers keep their `extern "C"` decls.
- Written ROM-descending; deferred codegen emits ascending.
- `_ZN7Vector3D1Ev` emits from LookAt_'s stack Vector3s and is licensed
  `deadstrip-duplicate` — the cartridge body lives at 0x020072c0, owned by
  its own shard.

## Gates

- tubuild verify: 2/2 MATCH, objisolate clean, reloc destinations clean.
- decl-agreement: two callee-extern baselines re-keyed to the promoted path;
  eight `LookAt_` caller rows banked (the def was invisible to the checker
  while it lived in a `namespace` block; the disagreements are pre-existing
  caller spellings, not new ones).
- No `decl_common.h`, port-manifest, or converted-tier references to repoint.
