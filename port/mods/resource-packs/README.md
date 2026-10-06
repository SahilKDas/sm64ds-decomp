# Lua resource packs

Resource packs use the sandboxed declarative Lua API. They cannot open files,
use the network, start processes, inspect game memory, or run general gameplay
scripts. Models and animations must use the port's native BMD/BCA renderer;
texture replacements are PNG files consumed by the existing HD-texture path.

## API v2

Each pack lives in `mods/resource-packs/<pack-id>/pack.lua`. The directory name
is its stable ID. IDs and character-local keys contain only letters, digits,
`-`, and `_`.

```lua
sm64ds.pack {
  id = "example-pack",
  name = "Example Pack",
  author = "Example Author",
  version = "1.0.0",
  license = "CC-BY-4.0",
  provenance = "https://example.invalid/revision/abc123"
}

sm64ds.character {
  key = "waluigi",              -- stable network/save key is example-pack:waluigi
  name = "Waluigi",
  base = 2,                      -- retail gameplay profile; 0..3 only
  body = "models/body.bmd",
  head_cap = "models/head_cap.bmd",
  head_no_cap = "models/head_no_cap.bmd",
  hitbox = { radius = 48, height = 116, hurt_radius = 48, hurt_height = 116 },
  animations = { idle = "anims/idle.bca", run = "anims/run.bca" },
  preview = { animation = "idle", icon = "icon.png", yaw = 15, distance = 320 }
}
```

Numeric `id` remains accepted for v1 packs, but stable keys are authoritative.
Runtime ID 4 and the fifth character-select position are reserved for Waluigi.
When no licensed Waluigi asset pack is installed, that entry safely uses the
Wario gameplay/model fallback; a `*:waluigi` pack supplies its native BMD/BCA
assets and metadata. The loader assigns other runtime IDs 5..255 in enabled
pack order and permanently reserves 0..3 for Mario, Luigi, Wario, and Yoshi. Duplicate keys, texture
hashes, invalid paths, missing assets, oversized scripts, and runaway scripts
reject only the offending pack.

The in-game F5 menu's **mods** rows browse packs, save enable state, and reload.
Reload requests made in a level are queued until a safe menu. State is written
atomically to `resource-packs.state` beside `settings.json` and the executable.

## CoopDX intake

`python port/tools/import_coopdx_characters.py <checkout> --dry-run --report report.json`
inventories Waluigi first and then the remaining roster deterministically. Add
`--output <local-pack-root>` to emit packs. Assets are emitted only when their
manifest has an explicitly permitted SPDX license and already supplies native
BMD/BCA files plus the required animation set. Everything else is reported
with a local-import recipe; the importer never invents a renderer or silently
degrades a rig.

This is the supported high-level mod format in 64DS-DX. It is asset-oriented,
not a general gameplay scripting API, and it contains no Zig component.

Each enabled subdirectory contains a `pack.lua`. Prefix the directory with
`off_` to disable it. Lua is deliberately declarative: it registers native
SM64DS resources and metadata, but cannot access the filesystem, network,
process, debug library, game memory, or arbitrary native code.
Each manifest is also capped at 1 MiB, 1,000,000 VM instructions, and 16 MiB
of Lua-managed memory, so a broken pack cannot hang startup or consume memory
without limit.

```lua
sm64ds.character {
  id = 4,
  name = "Example character",
  base = 2, -- 0 Mario, 1 Luigi, 2 Wario, 3 Yoshi
  body = "assets/body.bmd",
  head_cap = "assets/head-cap.bmd",
  head_no_cap = "assets/head-no-cap.bmd",
  hitbox = {
    radius = 50,
    height = 100,
    hurt_radius = 45,
    hurt_height = 95,
  },
  animations = {
    idle = "assets/idle.bca",
    run = "assets/run.bca",
  },
}

sm64ds.texture {
  -- Content hash printed by SM64DS_HD_TEXTURES_DUMP/INDEX.
  target = "0123456789abcdef",
  source = "assets/body.png",
}
```

Models must be native `.bmd`, animations native `.bca`, and replacement
textures PNG. Texture targets are the existing v0.4 content hashes, so Lua
packs travel through the established HD-texture renderer. Paths cannot leave
the pack directory. Character IDs 0 through 3 remain reserved for Mario,
Luigi, Wario, and Yoshi.

Texture declarations are consumed by the renderer today. Character declarations
are validated and registered for the native character-loading bridge; packs must
not assume an ID is selectable until that bridge reports it as available.

The default pack root is `mods/resource-packs` beside the game process. Set
`SM64DS_RESOURCE_PACKS` to use another exact directory. Each enabled immediate
subdirectory needs a `pack.lua`; prefix its directory name with `off_` to disable
it without deleting it.

Third-party assets are not bundled merely because a pack references them. Pack
authors are responsible for having permission to distribute every asset.
