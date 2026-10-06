#include "resource_pack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <set>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace sm64ds::packs {
namespace fs = std::filesystem;

namespace {
std::vector<Character> g_characters;
std::vector<TextureReplacement> g_textures;
std::vector<PackSummary> g_packs;
std::vector<PackIssue> g_diagnostics;
std::string g_root;
std::string g_selected_key;
int g_selected_base = 0;
bool g_selected_configured = false;
ReloadState g_reload_state = ReloadState::Idle;

struct Preference {
    bool enabled = true;
    int order = 0;
};
std::map<std::string, Preference> g_preferences;

struct LoadContext {
    fs::path directory;
    std::string id;
    PackSummary summary;
    std::vector<Character> characters;
    std::vector<TextureReplacement> textures;
};

bool valid_id(const std::string &value)
{
    if (value.empty() || value.size() > 64) return false;
    for (unsigned char ch : value)
        if (!(std::isalnum(ch) || ch == '-' || ch == '_')) return false;
    return true;
}

std::string optional_string(lua_State *L, int table, const char *field,
                            const std::string &fallback = {})
{
    lua_getfield(L, table, field);
    std::string result = fallback;
    if (!lua_isnil(L, -1)) {
        size_t length = 0;
        const char *value = lua_tolstring(L, -1, &length);
        if (!value) luaL_error(L, "%s must be a string", field);
        result.assign(value, length);
    }
    lua_pop(L, 1);
    return result;
}

std::string state_path()
{
    fs::path root(g_root);
    return (root.parent_path().parent_path() / "resource-packs.state").string();
}

void read_state()
{
    g_preferences.clear();
    g_selected_key.clear();
    g_selected_configured = false;
    std::ifstream input(state_path());
    std::string kind;
    while (input >> kind) {
        if (kind == "selected") {
            if (input >> std::quoted(g_selected_key) >> g_selected_base)
                g_selected_configured = true;
        }
        else if (kind == "pack") {
            std::string id; int enabled = 1, order = 0;
            if (input >> std::quoted(id) >> enabled >> order)
                g_preferences[id] = {enabled != 0, order};
        }
        input.ignore(4096, '\n');
    }
}

bool write_state(std::string &error)
{
    const fs::path path(state_path());
    const fs::path temporary = path.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) { error = "cannot write " + temporary.string(); return false; }
    output << "selected " << std::quoted(g_selected_key) << ' '
           << g_selected_base << '\n';
    for (const PackSummary &pack : g_packs)
        output << "pack " << std::quoted(pack.id) << ' ' << (pack.enabled ? 1 : 0)
               << ' ' << pack.order << '\n';
    output.close();
    if (!output) { error = "cannot finish " + temporary.string(); return false; }
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(path, ec); ec.clear(); fs::rename(temporary, path, ec);
    }
    if (ec) { error = "cannot publish pack state: " + ec.message(); return false; }
    return true;
}

struct LuaMemory {
    size_t used = 0;
    size_t limit = 16 * 1024 * 1024;
};

void *limited_alloc(void *user, void *ptr, size_t old_size, size_t new_size)
{
    LuaMemory *memory = static_cast<LuaMemory *>(user);
    if (!new_size) {
        std::free(ptr);
        memory->used = old_size > memory->used ? 0 : memory->used - old_size;
        return nullptr;
    }
    if (new_size > old_size && new_size - old_size > memory->limit - memory->used)
        return nullptr;
    void *next = std::realloc(ptr, new_size);
    if (!next) return nullptr;
    memory->used = new_size >= old_size ? memory->used + new_size - old_size
                                        : memory->used - (old_size - new_size);
    return next;
}

LoadContext *context(lua_State *L)
{
    return static_cast<LoadContext *>(lua_touserdata(L, lua_upvalueindex(1)));
}

int fail(lua_State *L, const char *message) { return luaL_error(L, "%s", message); }

std::string required_string(lua_State *L, int table, const char *field)
{
    lua_getfield(L, table, field);
    size_t length = 0;
    const char *value = lua_tolstring(L, -1, &length);
    if (!value || !length) luaL_error(L, "%s must be a non-empty string", field);
    std::string result(value, length);
    lua_pop(L, 1);
    return result;
}

int optional_int(lua_State *L, int table, const char *field, int fallback)
{
    lua_getfield(L, table, field);
    int result = fallback;
    if (!lua_isnil(L, -1)) {
        int exact = 0;
        const lua_Integer value = lua_tointegerx(L, -1, &exact);
        if (!exact) luaL_error(L, "%s must be an integer", field);
        result = static_cast<int>(value);
    }
    lua_pop(L, 1);
    return result;
}

float optional_number(lua_State *L, int table, const char *field, float fallback)
{
    lua_getfield(L, table, field);
    float result = fallback;
    if (!lua_isnil(L, -1)) {
        int numeric = 0;
        result = static_cast<float>(lua_tonumberx(L, -1, &numeric));
        if (!numeric || result <= 0.0f || result > 10000.0f)
            luaL_error(L, "%s must be a number in (0, 10000]", field);
    }
    lua_pop(L, 1);
    return result;
}

float optional_finite(lua_State *L, int table, const char *field, float fallback,
                      float minimum, float maximum)
{
    lua_getfield(L, table, field);
    float result = fallback;
    if (!lua_isnil(L, -1)) {
        int numeric = 0;
        result = static_cast<float>(lua_tonumberx(L, -1, &numeric));
        if (!numeric || result < minimum || result > maximum)
            luaL_error(L, "%s must be a number in [%g, %g]", field,
                       minimum, maximum);
    }
    lua_pop(L, 1);
    return result;
}

std::string asset_path(lua_State *L, LoadContext *ctx, const std::string &relative,
                       const char *field, const char *extension)
{
    fs::path rel(relative);
    if (rel.empty() || rel.is_absolute() || relative.find(':') != std::string::npos)
        luaL_error(L, "%s must be a relative path", field);
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(ctx->directory, ec);
    if (ec) luaL_error(L, "cannot resolve pack directory: %s", ec.message().c_str());
    const fs::path normalized = fs::weakly_canonical(ctx->directory / rel, ec);
    if (ec) luaL_error(L, "%s cannot be resolved: %s", field, ec.message().c_str());
    auto mismatch = std::mismatch(base.begin(), base.end(), normalized.begin(),
                                  normalized.end());
    if (mismatch.first != base.end()) luaL_error(L, "%s escapes its pack", field);
    if (extension && normalized.extension().string() != extension)
        luaL_error(L, "%s must reference a %s file", field, extension);
    if (!fs::is_regular_file(normalized, ec) || ec)
        luaL_error(L, "%s does not exist: %s", field, relative.c_str());
    return normalized.string();
}

int api_character(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    LoadContext *ctx = context(L);
    Character item;
    item.pack_id = ctx->id;
    item.legacy_id = optional_int(L, 1, "id", -1);
    item.id = item.legacy_id;
    item.base_character = optional_int(L, 1, "base", 0);
    item.name = required_string(L, 1, "name");
    std::string local_key = optional_string(L, 1, "key");
    if (local_key.empty()) {
        local_key = item.name;
        std::transform(local_key.begin(), local_key.end(), local_key.begin(),
                       [](unsigned char ch) { return ch == ' ' ? '-' : std::tolower(ch); });
    }
    if (!valid_id(local_key)) return fail(L, "key must contain only letters, digits, - or _");
    item.key = ctx->id + ":" + local_key;
    if (item.id != -1 && (item.id < 4 || item.id > 255))
        return fail(L, "legacy character id must be 4..255");
    if (item.base_character < 0 || item.base_character > 3)
        return fail(L, "base must be 0..3 (Mario, Luigi, Wario, Yoshi)");
    const auto duplicate = std::find_if(ctx->characters.begin(), ctx->characters.end(),
        [&](const Character &c) { return c.key == item.key; });
    if (duplicate != ctx->characters.end()) return fail(L, "duplicate character key in pack");

    item.author = optional_string(L, 1, "author", ctx->summary.author);
    item.version = optional_string(L, 1, "version", ctx->summary.version);
    item.license = optional_string(L, 1, "license", ctx->summary.license);

    item.body_model = asset_path(L, ctx, required_string(L, 1, "body"), "body", ".bmd");
    item.head_cap_model = asset_path(L, ctx, required_string(L, 1, "head_cap"),
                                     "head_cap", ".bmd");
    item.head_no_cap_model = asset_path(L, ctx, required_string(L, 1, "head_no_cap"),
                                        "head_no_cap", ".bmd");

    lua_getfield(L, 1, "hitbox");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        item.hitbox.radius = optional_number(L, -1, "radius", item.hitbox.radius);
        item.hitbox.height = optional_number(L, -1, "height", item.hitbox.height);
        item.hitbox.hurt_radius = optional_number(L, -1, "hurt_radius", item.hitbox.radius);
        item.hitbox.hurt_height = optional_number(L, -1, "hurt_height", item.hitbox.height);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "preview");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        item.preview_animation = optional_string(L, -1, "animation");
        const std::string icon = optional_string(L, -1, "icon");
        if (!icon.empty()) item.icon = asset_path(L, ctx, icon, "preview.icon", ".png");
        item.preview_yaw = optional_finite(L, -1, "yaw", 0.0f, -360.0f, 360.0f);
        item.preview_pitch = optional_finite(L, -1, "pitch", 0.0f, -89.0f, 89.0f);
        item.preview_distance = optional_number(L, -1, "distance", 300.0f);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "animations");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING)
                return fail(L, "animations must map names to .bca paths");
            const std::string key = lua_tostring(L, -2);
            const std::string value = lua_tostring(L, -1);
            item.animations[key] = asset_path(L, ctx, value, "animation", ".bca");
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    ctx->characters.push_back(std::move(item));
    return 0;
}

int api_pack(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    LoadContext *ctx = context(L);
    const std::string id = optional_string(L, 1, "id", ctx->id);
    if (id != ctx->id)
        return fail(L, "pack id must match its directory name");
    if (!valid_id(id)) return fail(L, "pack id must contain only letters, digits, - or _");
    ctx->summary.name = optional_string(L, 1, "name", id);
    ctx->summary.author = optional_string(L, 1, "author");
    ctx->summary.version = optional_string(L, 1, "version");
    ctx->summary.license = optional_string(L, 1, "license");
    ctx->summary.provenance = optional_string(L, 1, "provenance");
    return 0;
}

int api_texture(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    LoadContext *ctx = context(L);
    TextureReplacement item;
    item.pack_id = ctx->id;
    item.target = required_string(L, 1, "target");
    if (item.target.size() != 16 ||
        item.target.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return fail(L, "texture target must be a 16-digit content hash");
    item.target_hash = std::strtoull(item.target.c_str(), nullptr, 16);
    item.source = asset_path(L, ctx, required_string(L, 1, "source"), "source", ".png");
    ctx->textures.push_back(std::move(item));
    return 0;
}

int api_log(lua_State *L)
{
    size_t length = 0;
    const char *message = luaL_checklstring(L, 1, &length);
    std::fprintf(stderr, "[resource-pack:%s] %.*s\n", context(L)->id.c_str(),
                 static_cast<int>(length), message);
    return 0;
}

void instruction_limit(lua_State *L, lua_Debug *)
{
    luaL_error(L, "pack.lua exceeded its instruction budget");
}

void open_sandbox(lua_State *L)
{
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L, 1);
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "load");
}

void register_api(lua_State *L, LoadContext *ctx)
{
    lua_createtable(L, 0, 5);
    lua_pushlightuserdata(L, ctx); lua_pushcclosure(L, api_pack, 1);
    lua_setfield(L, -2, "pack");
    lua_pushlightuserdata(L, ctx); lua_pushcclosure(L, api_character, 1);
    lua_setfield(L, -2, "character");
    lua_pushlightuserdata(L, ctx); lua_pushcclosure(L, api_texture, 1);
    lua_setfield(L, -2, "texture");
    lua_pushlightuserdata(L, ctx); lua_pushcclosure(L, api_log, 1);
    lua_setfield(L, -2, "log");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "api_version");
    lua_setglobal(L, "sm64ds");
}

bool load_pack(const fs::path &directory, PackSummary &summary, std::string &error)
{
    const fs::path script = directory / "pack.lua";
    if (!fs::is_regular_file(script)) {
        error = summary.id + ": missing pack.lua";
        return false;
    }
    if (fs::file_size(script) > 1024 * 1024) {
        error = script.string() + ": pack.lua exceeds 1 MiB";
        return false;
    }
    LoadContext ctx;
    ctx.directory = directory;
    ctx.id = directory.filename().string();
    if (!valid_id(ctx.id)) {
        error = ctx.id + ": directory name is not a valid pack id";
        return false;
    }
    ctx.summary = summary;
    LuaMemory memory;
    lua_State *L = lua_newstate(limited_alloc, &memory);
    if (!L) { error = ctx.id + ": cannot create Lua state"; return false; }
    open_sandbox(L);
    register_api(L, &ctx);
    lua_sethook(L, instruction_limit, LUA_MASKCOUNT, 1000000);
    const int loaded = luaL_loadfilex(L, script.string().c_str(), "t");
    const int called = loaded == LUA_OK ? lua_pcall(L, 0, 0, 0) : loaded;
    if (called != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        error = ctx.id + ": " + (message ? message : "unknown Lua error");
        lua_close(L);
        return false;
    }
    lua_close(L);
    for (const Character &candidate : ctx.characters) {
        if (character(candidate.key)) {
            error = ctx.id + ": duplicate character key " + candidate.key;
            return false;
        }
    }
    for (const TextureReplacement &candidate : ctx.textures) {
        const auto duplicate = std::find_if(g_textures.begin(), g_textures.end(),
            [&](const TextureReplacement &item) {
                return item.target_hash == candidate.target_hash;
            });
        if (duplicate != g_textures.end()) {
            error = ctx.id + ": texture hash " + candidate.target +
                    " conflicts with pack " + duplicate->pack_id;
            return false;
        }
    }
    summary = ctx.summary;
    summary.loaded = true;
    for (const Character &candidate : ctx.characters)
        summary.character_keys.push_back(candidate.key);
    g_characters.insert(g_characters.end(), ctx.characters.begin(), ctx.characters.end());
    g_textures.insert(g_textures.end(), ctx.textures.begin(), ctx.textures.end());
    std::fprintf(stderr, "[resource-pack] loaded %s (%zu characters, %zu textures)\n",
                 ctx.id.c_str(), ctx.characters.size(), ctx.textures.size());
    return true;
}

void reserve_waluigi_slot()
{
    auto found = std::find_if(g_characters.begin(), g_characters.end(),
        [](const Character &item) {
            const std::string suffix = ":waluigi";
            return item.key == "64ds-dx:waluigi" ||
                (item.key.size() > suffix.size() &&
                 item.key.compare(item.key.size() - suffix.size(),
                                  suffix.size(), suffix) == 0);
        });
    if (found == g_characters.end()) {
        Character item;
        item.pack_id = "64ds-dx";
        item.key = "64ds-dx:waluigi";
        item.name = "Waluigi";
        item.author = "64DS-DX";
        item.version = "1";
        item.license = "local assets required";
        item.base_character = 2; // Wario abilities until a pack supplies assets.
        item.body_model = "port/mods/characters/waluigi/assets/waluigi_model.bmd";
        item.head_cap_model = "port/mods/characters/waluigi/assets/waluigi_head_cap.bmd";
        item.head_no_cap_model = "port/mods/characters/waluigi/assets/waluigi_head_nocap.bmd";
        item.icon = "port/mods/characters/waluigi/assets/waluigi_icon.png";
        item.hitbox.radius = 48.0f;
        item.hitbox.height = 116.0f;
        item.hitbox.hurt_radius = 48.0f;
        item.hitbox.hurt_height = 116.0f;
        g_characters.insert(g_characters.begin(), std::move(item));
    } else if (found != g_characters.begin()) {
        std::rotate(g_characters.begin(), found, found + 1);
    }
    // Slot 4 is the fifth character. Pack order remains deterministic from 5 on.
    g_characters.front().legacy_id = 4;
    g_characters.front().id = 4;
}

void assign_runtime_ids(std::ostringstream &errors, bool &ok)
{
    std::set<int> used;
    for (Character &item : g_characters) {
        if (item.legacy_id == -1) { item.id = -1; continue; }
        if (!used.insert(item.legacy_id).second) {
            errors << item.pack_id << ": warning: legacy character id "
                   << item.legacy_id << " conflicts; assigned by stable pack order\n";
            g_diagnostics.push_back({item.pack_id, "id",
                "legacy numeric ID conflicts; stable key and pack order were used"});
            item.id = -1;
        }
    }
    int next = 4;
    for (Character &item : g_characters) {
        if (item.id >= 4) continue;
        while (next <= 255 && used.count(next)) ++next;
        if (next > 255) {
            ok = false;
            errors << item.pack_id << ": no free runtime character IDs (4..255)\n";
            continue;
        }
        item.id = next;
        used.insert(next++);
    }
}
}  // namespace

void clear()
{
    g_characters.clear(); g_textures.clear(); g_packs.clear();
    g_diagnostics.clear();
}

bool load_all(const std::string &root, std::string &error)
{
    g_root = root;
    read_state();
    clear();
    error.clear();
    const fs::path base(root);
    if (!fs::exists(base)) return true;
    if (!fs::is_directory(base)) { error = root + " is not a directory"; return false; }
    bool ok = true;
    std::ostringstream errors;
    std::error_code ec;
    std::vector<fs::path> directories;
    for (const auto &entry : fs::directory_iterator(base, ec))
        if (entry.is_directory()) directories.push_back(entry.path());
    std::sort(directories.begin(), directories.end(), [](const fs::path &a,
                                                          const fs::path &b) {
        const auto ai = g_preferences.find(a.filename().string());
        const auto bi = g_preferences.find(b.filename().string());
        const int ao = ai == g_preferences.end() ? 0x3fffffff : ai->second.order;
        const int bo = bi == g_preferences.end() ? 0x3fffffff : bi->second.order;
        return ao == bo ? a.filename().string() < b.filename().string() : ao < bo;
    });
    int order = 0;
    for (const fs::path &directory : directories) {
        const std::string name = directory.filename().string();
        if (name.empty() || name[0] == '.') continue;
        PackSummary summary;
        summary.id = summary.name = name;
        summary.order = order++;
        const auto preference = g_preferences.find(name);
        summary.enabled = name.rfind("off_", 0) != 0 &&
            (preference == g_preferences.end() || preference->second.enabled);
        if (!summary.enabled) { g_packs.push_back(std::move(summary)); continue; }
        std::string one;
        if (!load_pack(directory, summary, one)) {
            ok = false;
            PackIssue issue{name, "pack.lua", one};
            summary.errors.push_back(issue);
            g_diagnostics.push_back(issue);
            errors << one << '\n';
        }
        g_packs.push_back(std::move(summary));
    }
    if (ec) { ok = false; errors << root << ": " << ec.message() << '\n'; }
    reserve_waluigi_slot();
    assign_runtime_ids(errors, ok);
    error = errors.str();
    g_reload_state = ok ? ReloadState::Idle : ReloadState::Failed;
    return ok;
}

bool request_reload(bool menu_safe, std::string &error)
{
    if (!menu_safe) { g_reload_state = ReloadState::Queued; error.clear(); return true; }
    g_reload_state = ReloadState::Reloading;
    return load_all(g_root, error);
}

bool apply_queued_reload(bool menu_safe, std::string &error)
{
    if (g_reload_state != ReloadState::Queued) return true;
    return menu_safe ? request_reload(true, error) : true;
}

bool set_pack_enabled(const std::string &id, bool enabled, std::string &error)
{
    auto found = std::find_if(g_packs.begin(), g_packs.end(),
        [&](const PackSummary &pack) { return pack.id == id; });
    if (found == g_packs.end()) { error = "unknown pack: " + id; return false; }
    found->enabled = enabled;
    return write_state(error);
}

bool move_pack(const std::string &id, int delta, std::string &error)
{
    auto found = std::find_if(g_packs.begin(), g_packs.end(),
        [&](const PackSummary &pack) { return pack.id == id; });
    if (found == g_packs.end()) { error = "unknown pack: " + id; return false; }
    const int from = static_cast<int>(found - g_packs.begin());
    const int to = std::max(0, std::min(static_cast<int>(g_packs.size()) - 1,
                                       from + delta));
    if (from != to) std::iter_swap(g_packs.begin() + from, g_packs.begin() + to);
    for (size_t i = 0; i < g_packs.size(); ++i) g_packs[i].order = static_cast<int>(i);
    return write_state(error);
}

bool select_character(const std::string &key, std::string &error)
{
    if (!key.empty() && !character(key)) {
        error = "character is unavailable: " + key;
        return false;
    }
    g_selected_key = key;
    if (const Character *item = character(key))
        g_selected_base = item->base_character;
    g_selected_configured = true;
    return write_state(error);
}

bool select_retail_character(int base, std::string &error)
{
    if (base < 0 || base > 3) {
        error = "retail character must be Mario, Luigi, Wario, or Yoshi";
        return false;
    }
    g_selected_key.clear();
    g_selected_base = base;
    g_selected_configured = true;
    return write_state(error);
}

const Character *character(int id)
{
    const auto found = std::find_if(g_characters.begin(), g_characters.end(),
        [id](const Character &item) { return item.id == id; });
    return found == g_characters.end() ? nullptr : &*found;
}
const Character *character(const std::string &key)
{
    const auto found = std::find_if(g_characters.begin(), g_characters.end(),
        [&](const Character &item) { return item.key == key; });
    return found == g_characters.end() ? nullptr : &*found;
}
const std::vector<Character> &characters() { return g_characters; }
const std::vector<TextureReplacement> &textures() { return g_textures; }
const std::vector<PackSummary> &packs() { return g_packs; }
const std::vector<PackIssue> &diagnostics() { return g_diagnostics; }
ReloadState reload_state() { return g_reload_state; }
const std::string &selected_character_key() { return g_selected_key; }
const Character *selected_character() { return character(g_selected_key); }
int selected_base_character()
{
    const Character *item = selected_character();
    return item ? item->base_character : std::max(0, std::min(3, g_selected_base));
}
bool has_selected_character() { return g_selected_configured; }
const std::string &root_path() { return g_root; }

std::string registry_fingerprint()
{
    std::uint64_t hash = 1469598103934665603ULL;
    std::vector<std::string> keys;
    for (const Character &item : g_characters) keys.push_back(item.key);
    std::sort(keys.begin(), keys.end());
    for (const std::string &key : keys) {
        for (unsigned char ch : key) { hash ^= ch; hash *= 1099511628211ULL; }
        hash ^= 0xff; hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::uint64_t character_key_hash(const std::string &key)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : key) { hash ^= ch; hash *= 1099511628211ULL; }
    return key.empty() ? 0 : hash;
}

const Character *character_by_hash(std::uint64_t hash)
{
    if (!hash) return nullptr;
    const auto found = std::find_if(g_characters.begin(), g_characters.end(),
        [hash](const Character &item) {
            return character_key_hash(item.key) == hash;
        });
    return found == g_characters.end() ? nullptr : &*found;
}

}  // namespace sm64ds::packs
