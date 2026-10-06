#!/usr/bin/env python3
"""Stage a CoopDX character source tree for conversion to native SM64DS BMD.

This importer intentionally does not emit a host mesh. It copies the selected
actor's geometry declarations and textures into the ignored local source area,
records hashes/provenance, and leaves native BMD generation as the next pipeline
stage. No third-party bytes are written to a tracked character directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess


REQUIRED = ("model.inc.c", "geo.inc.c")


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("character", nargs="?", default="waluigi")
    parser.add_argument("--coopdx", type=pathlib.Path,
                        default=pathlib.Path("reference/sm64coopdx"))
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parents[2]
    source_root = args.coopdx.resolve()
    actor = source_root / "actors" / args.character
    missing = [name for name in REQUIRED if not (actor / name).is_file()]
    textures = sorted(actor.glob("*.png"))
    if missing or not textures:
        detail = ", ".join(missing) if missing else "PNG textures"
        parser.error(f"incomplete actor directory {actor}: missing {detail}")

    destination = (root / "port" / "mods" / ".local-src" /
                   args.character).resolve()
    local_root = (root / "port" / "mods" / ".local-src").resolve()
    if local_root not in destination.parents:
        parser.error("destination escaped the local character source root")
    destination.mkdir(parents=True, exist_ok=True)

    copied = []
    for source in [*(actor / name for name in REQUIRED), *textures]:
        target = destination / source.name
        shutil.copy2(source, target)
        copied.append({"file": source.name, "sha256": sha256(target),
                       "bytes": target.stat().st_size})

    try:
        revision = subprocess.check_output(
            ["git", "-C", str(source_root), "rev-parse", "HEAD"],
            text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        revision = "unknown"

    provenance = {
        "schemaVersion": 1,
        "project": "sm64coopdx",
        "revision": revision,
        "character": args.character,
        "redistribution": "local-import-only",
        "files": copied,
    }
    (destination / "source.json").write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    print(f"staged {len(copied)} files in {destination}")
    print("next output must be native BMD under the ignored character assets directory")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
