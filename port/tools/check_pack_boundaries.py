#!/usr/bin/env python3
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
manifest = json.loads((root / "port/compat/tango-symbols.json").read_text(encoding="utf-8"))
for name, relative in manifest["paths"].items():
    if not (root / relative).is_file():
        raise SystemExit(f"compat path {name} is stale: {relative}")
if manifest["reserved_character_ids"] != [0, 1, 2, 3]:
    raise SystemExit("retail character IDs 0..3 must remain reserved")
loader = (root / "port/hal/resource_pack.cpp").read_text(encoding="utf-8")
if "item.id < 4" not in loader and "next = 4" not in loader:
    raise SystemExit("pack loader no longer visibly reserves retail IDs")
print("pack boundaries: PASS")
