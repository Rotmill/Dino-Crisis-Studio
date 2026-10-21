// Mod packages (10.10.2026, player: "each mod in its own folder, a manager in the studio: on, off, order, conflicts").
// A package is a folder of mods with mod.json and the layout of mods inside (textures, characters, rooms, sound, other...).
// The game reads them itself (dc_mods.c): the own files of mods first, then the packages that are on, in the order of
// mods\load_order.txt (a line - a folder, "-" in front - off; the ones not listed - on, after them, by name); a file with the
// same path is taken from the later one, the lists (index.txt, names.txt) and the game's program data are merged.
#pragma once
#include "core.h"

namespace dc {

struct ModPack {
    std::string dir;                      // folder under mods (UTF-8)
    std::string name, author, version, description;
    bool on = true;
    int files = 0;                        // files it carries (mod.json, preview.png, readme.txt not counted)
    std::vector<std::string> wins;        // its files that a layer before it has too (it wins)
    std::vector<std::string> loses;       // its files that a later layer takes over
};

struct PackSet {
    fs::path mods;
    int own_files = 0;                    // files of mods outside the packages (names.txt, load_order.txt not counted)
    std::vector<ModPack> packs;           // in the game's order
    bool scan(const fs::path& mods_dir);  // false: no mods folder
    bool save_order(std::string& err) const;
    int find(const std::string& dir) const;
};

std::string pack_dir_name(const fs::path& mods, const std::string& name);   // a free folder name for a mod name
bool pack_create(const fs::path& mods, const std::string& name, std::string& dir_out, std::string& err);
bool pack_save_info(const fs::path& mods, const ModPack& p, std::string& err);   // mod.json
// a downloaded zip into mods: the package in it (its folder, or the root of the zip) -> mods\<folder>. The folder is there
// already and replace is false: returns false with exists = true (ask, then call again with replace)
bool pack_install_zip(const fs::path& zip, const fs::path& mods, bool replace, std::string& dir_out, bool& exists, std::string& err);
bool pack_build_zip(const fs::path& mods, const std::string& dir, const fs::path& zip, std::string& err, int* nfiles = nullptr);

}  // namespace dc
