#!/usr/bin/env python3
"""Inventory a CoopDX checkout and emit licensed SM64DS Lua v2 packs.

This tool intentionally does not convert arbitrary meshes. A character is importable
only when its manifest points at native BMD/BCA files and carries an explicit SPDX
license that permits redistribution. Rejected entries remain useful: the report gives
their exact blocker and a local-import recipe without copying their assets.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path

LICENSE_ALLOWLIST = {
    "0BSD", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "CC0-1.0",
    "CC-BY-3.0", "CC-BY-4.0", "ISC", "MIT", "Zlib",
}
REQUIRED_ANIMATIONS = {"idle", "walk", "run", "jump", "damage"}
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


def revision(root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"], text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def manifests(root: Path) -> list[Path]:
    found = list(root.rglob("character.json"))
    return sorted(found, key=lambda p: (p.stem.lower() != "waluigi", "waluigi" not in str(p).lower(), str(p).lower()))


def asset(manifest: Path, value: object, suffix: str, field: str, errors: list[str]) -> Path | None:
    if not isinstance(value, str) or not value:
        errors.append(f"{field}: missing path")
        return None
    candidate = (manifest.parent / value).resolve()
    try:
        candidate.relative_to(manifest.parent.resolve())
    except ValueError:
        errors.append(f"{field}: path escapes character directory")
        return None
    if candidate.suffix.lower() != suffix or not candidate.is_file():
        errors.append(f"{field}: requires an existing {suffix} asset")
        return None
    return candidate


def lua_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def inspect_one(path: Path, source_root: Path, source_rev: str) -> dict:
    result = {"manifest": str(path.relative_to(source_root)), "status": "rejected", "errors": []}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        result["errors"].append(f"manifest: {exc}")
        return result
    char_id = str(data.get("id", "")).strip().lower()
    if not ID_RE.fullmatch(char_id):
        result["errors"].append("id: expected 1-64 letters, digits, '-' or '_'")
    license_id = str(data.get("license", "")).strip()
    if license_id not in LICENSE_ALLOWLIST:
        result["errors"].append("license: explicit compatible SPDX identifier required")
    author = str(data.get("author", "")).strip()
    if not author:
        result["errors"].append("author: required for provenance")
    base = data.get("base", data.get("baseCharacter", 0))
    if base not in (0, 1, 2, 3):
        result["errors"].append("base: must be 0..3; retail slots are never replaced")
    models = data.get("models", {})
    resolved = {
        "body": asset(path, models.get("body"), ".bmd", "models.body", result["errors"]),
        "head_cap": asset(path, models.get("head_cap"), ".bmd", "models.head_cap", result["errors"]),
        "head_no_cap": asset(path, models.get("head_no_cap"), ".bmd", "models.head_no_cap", result["errors"]),
    }
    animations = data.get("animations", {})
    if not isinstance(animations, dict):
        result["errors"].append("animations: expected name/path object")
        animations = {}
    missing = sorted(REQUIRED_ANIMATIONS - animations.keys())
    if missing:
        result["errors"].append("animations: missing " + ", ".join(missing))
    resolved_anims = {
        name: asset(path, value, ".bca", f"animations.{name}", result["errors"])
        for name, value in sorted(animations.items())
    }
    result.update({"id": char_id, "name": data.get("name", char_id), "author": author,
                   "license": license_id, "source_revision": source_rev, "base": base})
    if result["errors"]:
        result["recipe"] = f"Fix {result['manifest']}, then rerun this command with --output <local-pack-root>."
        return result
    result["status"] = "compatible"
    result["resolved"] = {k: str(v) for k, v in resolved.items()}
    result["resolved_animations"] = {k: str(v) for k, v in resolved_anims.items()}
    result["data"] = data
    return result


def emit(entry: dict, output: Path) -> None:
    pack_id = "coopdx-" + entry["id"]
    target = output / pack_id
    assets = target / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    copied = {}
    for field, source in entry["resolved"].items():
        dst = assets / (field + Path(source).suffix.lower())
        shutil.copy2(source, dst); copied[field] = dst.relative_to(target).as_posix()
    copied_anims = {}
    for name, source in entry["resolved_animations"].items():
        dst = assets / ("anim_" + name + Path(source).suffix.lower())
        shutil.copy2(source, dst); copied_anims[name] = dst.relative_to(target).as_posix()
    data = entry["data"]
    hitbox = data.get("hitbox", {})
    lines = [
        "-- Generated by import_coopdx_characters.py; edit the source manifest, not this file.",
        "sm64ds.pack {",
        f"  id = {lua_string(pack_id)}, name = {lua_string(str(entry['name']))},",
        f"  author = {lua_string(entry['author'])}, version = {lua_string(str(data.get('version', '1')))},",
        f"  license = {lua_string(entry['license'])},",
        f"  provenance = {lua_string('CoopDX ' + entry['source_revision'])}",
        "}", "", "sm64ds.character {",
        f"  key = {lua_string(entry['id'])}, name = {lua_string(str(entry['name']))}, base = {entry['base']},",
        f"  body = {lua_string(copied['body'])}, head_cap = {lua_string(copied['head_cap'])},",
        f"  head_no_cap = {lua_string(copied['head_no_cap'])},",
        "  hitbox = { " + ", ".join(f"{k} = {float(v):g}" for k, v in sorted(hitbox.items())) + " },",
        "  animations = {",
    ]
    lines += [f"    {name} = {lua_string(value)}," for name, value in copied_anims.items()]
    lines += ["  },", "}", ""]
    (target / "pack.lua").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("checkout", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    root = args.checkout.resolve()
    rev = revision(root)
    entries = [inspect_one(path, root, rev) for path in manifests(root)]
    report = {"source": str(root), "revision": rev, "characters": entries}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not args.dry_run:
        if args.output is None: parser.error("--output is required unless --dry-run is used")
        for entry in entries:
            if entry["status"] == "compatible": emit(entry, args.output)
    print(f"CoopDX inventory: {sum(e['status'] == 'compatible' for e in entries)} compatible, "
          f"{sum(e['status'] != 'compatible' for e in entries)} rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
