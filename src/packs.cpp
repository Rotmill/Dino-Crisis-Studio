// Mod packages (10.10.2026) - see packs.h. The order rules are the game's (dc_mods.c vpack_order).
#include "packs.h"

#include <windows.h>

#include <algorithm>
#include <set>

#include "gltf.h"   // json
#include "zip.h"

namespace dc {

static std::wstring lower_w(std::wstring s) {
    CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

static bool is_meta(const std::string& rel_top) {   // files of a package that are about it, not for the game
    const std::string u = upper(rel_top);
    return u == "MOD.JSON" || u == "PREVIEW.PNG" || u == "README.TXT";
}

// a file the game merges instead of taking whole (not a conflict)
static bool merged_file(const std::string& rel) {
    const std::string u = upper(rel);
    if (u == "NAMES.TXT" || u == "OTHER\\SLUS_009.22") return true;
    return u.size() >= 9 && u.compare(u.size() - 9, 9, "INDEX.TXT") == 0;
}

static void files_of(const fs::path& dir, bool root, const std::set<std::wstring>& packs_lower,
                     std::vector<std::string>& out) {
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(dir, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path rel = fs::relative(it->path(), dir, ec);
        const std::wstring top = rel.begin() != rel.end() ? (*rel.begin()).wstring() : std::wstring();
        if (it->is_directory(ec)) {
            if (root && it.depth() == 0 && packs_lower.count(lower_w(top))) it.disable_recursion_pending();
            if (!top.empty() && top[0] == L'.') it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string r = path_u8(rel);
        if (it.depth() == 0) {
            if (root && (upper(r) == "LOAD_ORDER.TXT" || upper(r) == "NAMES.TXT")) continue;
            if (!root && is_meta(r)) continue;
        }
        if (!r.empty() && r[0] == '.') continue;
        out.push_back(r);
    }
}

int PackSet::find(const std::string& dir) const {
    for (size_t i = 0; i < packs.size(); i++)
        if (lower_w(u8path(packs[i].dir).wstring()) == lower_w(u8path(dir).wstring())) return (int)i;
    return -1;
}

bool PackSet::scan(const fs::path& mods_dir) {
    mods = mods_dir;
    packs.clear();
    own_files = 0;
    std::error_code ec;
    if (!fs::is_directory(mods, ec)) return false;
    std::vector<ModPack> found;
    for (auto& e : fs::directory_iterator(mods, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string d = path_u8(e.path().filename());
        if (d.empty() || d[0] == '.') continue;
        if (!fs::is_regular_file(e.path() / "mod.json", ec)) continue;
        ModPack p;
        p.dir = d;
        p.name = d;
        Bytes b;
        if (read_file(e.path() / "mod.json", b)) {
            try {
                const json j = json::parse(b.begin(), b.end());
                p.name = j.value("name", d);
                p.author = j.value("author", "");
                p.version = j.value("version", "");
                p.description = j.value("description", "");
            } catch (...) {
            }
        }
        found.push_back(p);
    }
    // load_order.txt: listed first ("-" - off), the rest on, by name (as the game)
    std::vector<bool> used(found.size(), false);
    Bytes lo;
    if (read_file(mods / "load_order.txt", lo)) {
        std::string t(lo.begin(), lo.end());
        size_t a = 0;
        while (a < t.size()) {
            size_t b2 = t.find('\n', a);
            if (b2 == std::string::npos) b2 = t.size();
            std::string l = t.substr(a, b2 - a);
            a = b2 + 1;
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
            size_t s = 0;
            while (s < l.size() && (l[s] == ' ' || l[s] == '\t' || (unsigned char)l[s] == 0xEF || (unsigned char)l[s] == 0xBB ||
                                    (unsigned char)l[s] == 0xBF)) s++;
            l = l.substr(s);
            if (l.empty() || l[0] == '#') continue;
            const bool off = l[0] == '-';
            if (off) l = l.substr(1);
            for (size_t i = 0; i < found.size(); i++)
                if (!used[i] && lower_w(u8path(found[i].dir).wstring()) == lower_w(u8path(l).wstring())) {
                    used[i] = true;
                    found[i].on = !off;
                    packs.push_back(found[i]);
                    break;
                }
        }
    }
    std::vector<size_t> rest;
    for (size_t i = 0; i < found.size(); i++) if (!used[i]) rest.push_back(i);
    std::sort(rest.begin(), rest.end(), [&](size_t x, size_t y) {
        return lower_w(u8path(found[x].dir).wstring()) < lower_w(u8path(found[y].dir).wstring());
    });
    for (size_t i : rest) packs.push_back(found[i]);
    // files and conflicts (the packages that are on, in order; the own files of mods first)
    std::set<std::wstring> pl;
    for (auto& p : packs) pl.insert(lower_w(u8path(p.dir).wstring()));
    std::vector<std::string> own;
    files_of(mods, true, pl, own);
    own_files = (int)own.size();
    std::map<std::wstring, std::vector<int>> who;   // file -> layers that have it (-1 - mods itself)
    for (auto& r : own) if (!merged_file(r)) who[lower_w(u8path(r).wstring())].push_back(-1);
    std::map<std::wstring, std::string> shown;
    for (size_t i = 0; i < packs.size(); i++) {
        std::vector<std::string> fl;
        files_of(mods / u8path(packs[i].dir), false, pl, fl);
        packs[i].files = (int)fl.size();
        if (!packs[i].on) continue;
        for (auto& r : fl) {
            if (merged_file(r)) continue;
            const std::wstring k = lower_w(u8path(r).wstring());
            who[k].push_back((int)i);
            shown[k] = r;
        }
    }
    for (auto& kv : who) {
        if (kv.second.size() < 2) continue;
        const std::string& r = shown.count(kv.first) ? shown[kv.first] : path_u8(fs::path(kv.first));
        const int last = kv.second.back();
        for (size_t k = 0; k + 1 < kv.second.size(); k++) {
            const int l = kv.second[k];
            if (l >= 0) packs[(size_t)l].loses.push_back(r + "  <  " + packs[(size_t)last].name);
        }
        if (last >= 0) {
            const int prev = kv.second[kv.second.size() - 2];
            packs[(size_t)last].wins.push_back(r + "  >  " + (prev < 0 ? std::string("mods") : packs[(size_t)prev].name));
        }
    }
    return true;
}

bool PackSet::save_order(std::string& err) const {
    std::string t = "# Dino Crisis ME: mod packages in load order (the studio writes it). A later one wins. \"-\" in front: off.\n";
    for (auto& p : packs) t += (p.on ? "" : "-") + p.dir + "\n";
    if (!write_text(mods / "load_order.txt", t)) { err = "cannot write " + path_u8(mods / "load_order.txt"); return false; }
    return true;
}

std::string pack_dir_name(const fs::path& mods, const std::string& name) {
    std::wstring w = u8path(name).wstring(), o;
    for (wchar_t c : w) o += (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    while (!o.empty() && (o.back() == L' ' || o.back() == L'.')) o.pop_back();
    while (!o.empty() && o[0] == L' ') o.erase(0, 1);
    if (o.empty()) o = L"My mod";
    static const wchar_t* reserved[] = {L"textures", L"screens", L"rooms", L"doors", L"characters", L"items", L"weapons", L"movie",
                                        L"videos", L"sound", L"sounds", L"other", L"collision", L"source", L"con", L"prn", L"aux",
                                        L"nul", L"com1", L"lpt1"};
    for (const wchar_t* r : reserved) if (!_wcsicmp(o.c_str(), r)) { o += L" mod"; break; }
    std::error_code ec;
    std::wstring t = o;
    for (int k = 2; fs::exists(mods / t, ec); k++) t = o + L" " + std::to_wstring(k);
    return path_u8(fs::path(t));
}

bool pack_save_info(const fs::path& mods, const ModPack& p, std::string& err) {
    json j = {{"name", p.name}, {"author", p.author}, {"version", p.version}, {"description", p.description},
              {"game", "Dino Crisis ME"}};
    if (!write_text(mods / u8path(p.dir) / "mod.json", j.dump(2) + "\n")) {
        err = "cannot write " + path_u8(mods / u8path(p.dir) / "mod.json");
        return false;
    }
    return true;
}

bool pack_create(const fs::path& mods, const std::string& name, std::string& dir_out, std::string& err) {
    std::error_code ec;
    fs::create_directories(mods, ec);
    ModPack p;
    p.dir = pack_dir_name(mods, name);
    p.name = name.empty() ? p.dir : name;
    p.version = "1.0";
    fs::create_directories(mods / u8path(p.dir), ec);
    if (ec) { err = "cannot make " + path_u8(mods / u8path(p.dir)); return false; }
    if (!pack_save_info(mods, p, err)) return false;
    dir_out = p.dir;
    return true;
}

static bool is_category(const std::string& top) {
    static const char* c[] = {"textures", "screens", "rooms", "doors", "characters", "items", "weapons", "movie", "videos",
                              "sound", "sounds", "other", "collision"};
    for (const char* x : c) if (!_stricmp(top.c_str(), x)) return true;
    return false;
}

bool pack_install_zip(const fs::path& zip, const fs::path& mods, bool replace, std::string& dir_out, bool& exists, std::string& err) {
    exists = false;
    Bytes z;
    if (!read_file(zip, z)) { err = "cannot read " + path_u8(zip); return false; }
    std::vector<ZipEntry> es;
    if (!zip_list(z, es, err)) return false;
    std::vector<const ZipEntry*> files;
    for (auto& e : es) {
        if (e.name.empty() || e.name.back() == '/') continue;
        if (!zip_name_safe(e.name)) { err = "the zip has a bad path: " + e.name; return false; }
        files.push_back(&e);
    }
    if (files.empty()) { err = "the zip is empty"; return false; }
    // where the package is: mod.json at the root, or one folder with everything (its name - the package's)
    std::string root, dir;
    bool has_json = false;
    for (auto* e : files) if (!_stricmp(e->name.c_str(), "mod.json")) has_json = true;
    if (!has_json) {
        std::string top;
        bool one = true;
        for (auto* e : files) {
            const size_t s = e->name.find('/');
            const std::string t = s == std::string::npos ? std::string() : e->name.substr(0, s);
            if (t.empty() || (!top.empty() && _stricmp(t.c_str(), top.c_str()) != 0)) { one = false; break; }
            top = t;
        }
        if (one && !top.empty() && !is_category(top)) { root = top + "/"; dir = top; }
    }
    std::string stem = path_u8(zip.stem());
    if (dir.empty()) dir = stem;
    std::string json_name;
    for (auto* e : files)
        if (!_stricmp(e->name.c_str(), (root + "mod.json").c_str())) {
            Bytes b;
            if (zip_read(z, *e, b, err)) {
                try { json_name = json::parse(b.begin(), b.end()).value("name", ""); } catch (...) {}
            }
        }
    {   // the folder name: as in the zip, without characters Windows does not take
        std::wstring w = u8path(dir).wstring(), o;
        for (wchar_t c : w) o += (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
        while (!o.empty() && (o.back() == L' ' || o.back() == L'.')) o.pop_back();
        if (o.empty()) o = L"Mod";
        if (is_category(path_u8(fs::path(o)))) o += L" mod";
        dir = path_u8(fs::path(o));
    }
    std::error_code ec;
    const fs::path target = mods / u8path(dir);
    if (fs::exists(target, ec)) {
        if (!replace) { exists = true; dir_out = dir; err = "a package named " + dir + " is there already"; return false; }
        fs::remove_all(target, ec);
        if (ec) { err = "cannot remove the old " + path_u8(target) + ": " + ec.message(); return false; }
    }
    int n = 0;
    bool wrote_json = false;
    for (auto* e : files) {
        if (e->name.size() <= root.size() || _strnicmp(e->name.c_str(), root.c_str(), root.size()) != 0) continue;
        const std::string rel = e->name.substr(root.size());
        Bytes b;
        if (!zip_read(z, *e, b, err)) return false;
        const fs::path out = target / u8path(rel);
        fs::create_directories(out.parent_path(), ec);
        if (!write_file(out, b.data(), b.size())) { err = "cannot write " + path_u8(out); return false; }
        if (!_stricmp(rel.c_str(), "mod.json")) wrote_json = true;
        n++;
    }
    if (!n) { err = "nothing in the zip for the game"; return false; }
    if (!wrote_json) {
        ModPack p;
        p.dir = dir;
        p.name = json_name.empty() ? dir : json_name;
        if (!pack_save_info(mods, p, err)) return false;
    }
    dir_out = dir;
    return true;
}

bool pack_build_zip(const fs::path& mods, const std::string& dir, const fs::path& zip, std::string& err, int* nfiles) {
    return zip_write_dir(mods / u8path(dir), zip, dir, err, nfiles);
}

}  // namespace dc
