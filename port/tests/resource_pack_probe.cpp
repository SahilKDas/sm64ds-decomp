#include "resource_pack.h"

#include <cstdio>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: resource_pack_probe <pack-root> [--expect-errors]\n");
        return 2;
    }
    const bool expect_errors = argc == 3 &&
                               std::string(argv[2]) == "--expect-errors";
    std::string error;
    const bool loaded = sm64ds::packs::load_all(argv[1], error);
    if (expect_errors) {
        int good = 0, bad = 0;
        for (const auto &pack : sm64ds::packs::packs())
            pack.loaded ? ++good : ++bad;
        if (loaded || good != 1 || bad != 1 ||
            sm64ds::packs::diagnostics().empty())
            return 8;
        std::puts("resource_pack_probe: independent rejection PASS");
        return 0;
    }
    if (!loaded) {
        std::fprintf(stderr, "%s", error.c_str());
        return 1;
    }
    const auto *waluigi = sm64ds::packs::character(4);
    if (!waluigi || waluigi->name != "Waluigi" ||
        waluigi->key != "64ds-dx:waluigi" || waluigi->base_character != 2 ||
        waluigi->body_model.empty() || waluigi->head_cap_model.empty() ||
        waluigi->head_no_cap_model.empty())
        return 9;
    const auto *character = sm64ds::packs::character("probe:probe");
    if (!character || character->id != 5 || character->name != "Probe" ||
        character->base_character != 2)
        return 3;
    if (character->key.empty() ||
        sm64ds::packs::character(character->key) != character)
        return 6;
    if (character->animations.size() != 2 || character->hitbox.radius != 61.0f ||
        character->hitbox.hurt_height != 117.0f)
        return 4;
    if (sm64ds::packs::textures().size() != 1 ||
        sm64ds::packs::textures()[0].target_hash != 0x0123456789abcdefULL)
        return 5;
    if (sm64ds::packs::packs().size() != 1 ||
        !sm64ds::packs::packs()[0].loaded ||
        sm64ds::packs::registry_fingerprint().size() != 16)
        return 7;
    std::puts("resource_pack_probe: PASS");
    return 0;
}
