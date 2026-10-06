# 64DS-DX modding

64DS-DX supports asset-oriented modding through the game's existing native
resource and rendering paths. The canonical branch is `main`.

## Lua resource packs

[Lua resource packs](resource-packs/README.md) are the supported distributable
format. A sandboxed `pack.lua` declares content-hash PNG replacements and native
character metadata. It cannot access files outside its pack, the network,
processes, game memory, debug APIs, or arbitrary native code.

## Native character imports

[Native character manifests](characters/README.md) describe prepared SM64DS
BMD/BCA assets and logical IDs above the four retail characters. This is the
lower-level import path used by tooling; it does not replace Mario, Luigi,
Wario, or Yoshi.

## Deliberate non-features

- No Zig modding runtime.
- No general gameplay Lua API.
- No custom OBJ or host-triangle renderer.
- No bundled ROM, Nintendo assets, or unlicensed third-party character assets.
