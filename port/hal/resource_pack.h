#pragma once

#include <map>
#include <cstdint>
#include <string>
#include <vector>

namespace sm64ds::packs {

struct Hitbox {
    float radius = 50.0f;
    float height = 100.0f;
    float hurt_radius = 50.0f;
    float hurt_height = 100.0f;
};

struct Character {
    int id = -1;
    int legacy_id = -1;
    int base_character = 0;
    std::string pack_id;
    std::string key;
    std::string name;
    std::string author;
    std::string version;
    std::string license;
    std::string body_model;
    std::string head_cap_model;
    std::string head_no_cap_model;
    std::string preview_animation;
    std::string icon;
    float preview_yaw = 0.0f;
    float preview_pitch = 0.0f;
    float preview_distance = 300.0f;
    std::map<std::string, std::string> animations;
    Hitbox hitbox;
};

struct TextureReplacement {
    std::string pack_id;
    std::string target;
    std::uint64_t target_hash = 0;
    std::string source;
};

struct PackIssue {
    std::string pack_id;
    std::string field;
    std::string message;
};

struct PackSummary {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string license;
    std::string provenance;
    bool enabled = true;
    bool loaded = false;
    int order = 0;
    std::vector<std::string> character_keys;
    std::vector<PackIssue> warnings;
    std::vector<PackIssue> errors;
};

enum class ReloadState { Idle, Queued, Reloading, Failed };

// Loads every enabled <root>/<pack>/pack.lua. A broken pack is rejected without
// preventing the remaining packs from loading. Returns false when any pack was
// rejected and places a complete diagnostic in `error`.
bool load_all(const std::string &root, std::string &error);
bool request_reload(bool menu_safe, std::string &error);
bool apply_queued_reload(bool menu_safe, std::string &error);
bool set_pack_enabled(const std::string &id, bool enabled, std::string &error);
bool move_pack(const std::string &id, int delta, std::string &error);
bool select_character(const std::string &key, std::string &error);
bool select_retail_character(int base, std::string &error);
void clear();

const Character *character(int id);
const Character *character(const std::string &key);
const std::vector<Character> &characters();
const std::vector<TextureReplacement> &textures();
const std::vector<PackSummary> &packs();
const std::vector<PackIssue> &diagnostics();
ReloadState reload_state();
const std::string &selected_character_key();
const Character *selected_character();
int selected_base_character();
bool has_selected_character();
std::string registry_fingerprint();
std::uint64_t character_key_hash(const std::string &key);
const Character *character_by_hash(std::uint64_t hash);
const std::string &root_path();

}  // namespace sm64ds::packs
