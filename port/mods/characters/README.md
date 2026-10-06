# Native character packs

For distributable, declarative packs, see the
[Lua resource-pack format](../resource-packs/README.md). This directory holds the
lower-level native import manifests and tooling used to prepare compatible assets.

Character packs extend the port without replacing the four cartridge character
slots. A pack owns a logical ID above the retail range and supplies ordinary
SM64DS `BMD_File` assets. The game renderer, material loader, and animation path
consume those files exactly as they consume cartridge BMDs.

There is deliberately no OBJ, host-triangle, or texture-interception fallback.
If a required BMD is missing or invalid, the extra character is unavailable;
Mario, Luigi, Wario, and Yoshi remain untouched.

Third-party sources and generated assets live below ignored directories. Only
the manifest and conversion/import tooling belong in Git until the asset owner
grants redistribution permission.
