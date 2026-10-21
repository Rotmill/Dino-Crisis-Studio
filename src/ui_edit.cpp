// Правка комнаты прямо в приложении: тексты (любой длины), враги и предметы (поля, копия, убрать), скрипт целиком
// (script.txt). «Сохранить» кладёт файлы правок в mods\rooms\<ИМЯ>\ (texts.json, enemies.json, items.json, script.txt) и
// запускает «Сконвертировать всё»: игра берёт собранную комнату (скрипт пересобирается, script_edit.h). «Как в игре» —
// убрать файл правок. Правки, сохранённые раньше, подхватываются из mods при открытии комнаты.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include "i18n.h"
#include "pieces.h"
#include "placement.h"
#include "script_edit.h"
#include "ui.h"
#include "ui_names.h"

namespace dc {

static int str_resize(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* s = (std::string*)d->UserData;
        s->resize((size_t)d->BufTextLen);
        d->Buf = (char*)s->c_str();
    }
    return 0;
}
static bool input_str(const char* id, std::string& s, ImVec2 size, bool multiline = true) {
    const ImGuiInputTextFlags fl = ImGuiInputTextFlags_CallbackResize;
    if (multiline) return ImGui::InputTextMultiline(id, (char*)s.c_str(), s.capacity() + 1, size, fl, str_resize, &s);
    return ImGui::InputText(id, (char*)s.c_str(), s.capacity() + 1, fl, str_resize, &s);
}

fs::path App::room_mods_dir(const DiscFile& f) const { return work_root() / "rooms" / u8path(f.stem); }   // 10.10: or the package

static bool read_json_file(const fs::path& p, json& j) {
    Bytes b;
    if (!read_file(p, b)) return false;
    try {
        j = json::parse(b.begin(), b.end());
    } catch (...) {
        return false;
    }
    return true;
}

void App::ensure_edit(Loaded& L) {
    RoomEdit& E = L.ed;
    if (E.loaded || !L.c.is_room || !L.c.file) return;
    E.loaded = true;
    ensure_placement(L);
    const fs::path md = room_mods_dir(*L.c.file);
    // тексты
    E.texts.clear();
    for (auto& s : L.c.room.strings) E.texts.push_back(s.text);
    E.texts_orig = E.texts;
    json j;
    if (read_json_file(md / "texts.json", j) && j.count("texts"))
        for (auto& t : j["texts"]) {
            const size_t i = t.value("index", (size_t)-1);
            if (i < E.texts.size() && t.count("text")) E.texts[i] = t["text"].get<std::string>();
        }
    // враги и предметы: файл правок в mods (с копиями и убранными) или как в игре
    E.enemies = L.enemies;
    E.items = L.items;
    if (read_json_file(md / "enemies.json", j) && j.count("enemies")) E.enemies = j["enemies"];
    if (read_json_file(md / "items.json", j) && j.count("items")) E.items = j["items"];
    // скрипт
    E.script.clear();
    {
        Bytes b;
        if (read_file(md / "script.txt", b)) E.script.assign(b.begin(), b.end());
    }
    ScriptImage im;
    std::string err;
    if (script_decompose(L.c.blocks[L.c.room_block].data, L.c.room, im, err)) E.script_orig = script_to_text(im, L.c.file->stem);
    if (E.script.empty()) E.script = E.script_orig;
    E.pieces = room_pieces(L.c, E.script);   // 06.10.2026: placed pieces - from the script text (saved edits count)
    E.dirty_texts = E.dirty_enemies = E.dirty_items = E.dirty_script = E.dirty_pieces = false;
    E.msg.clear();
}

// сохранить правку (что: 0 тексты, 1 враги, 2 предметы, 3 скрипт) в mods и собрать
void App::save_edit(Loaded& L, int what) {
    RoomEdit& E = L.ed;
    if (set_.mods.empty() && !choose_mods()) return;   // the game's mods folder
    const DiscFile& f = *L.c.file;
    const fs::path md = room_mods_dir(f);
    std::error_code ec;
    fs::create_directories(md, ec);
    bool ok = true;
    if (what == 0) {
        json arr = json::array();
        for (size_t i = 0; i < E.texts.size(); i++) arr.push_back({{"index", i}, {"text", E.texts[i]}});
        json j = {{"format", "dc-texts-1"}, {"room", f.stem},
                  {"about", "Texts of the room. \\n = new line, {#N} = game glyph N, {XXXX} = control code. Any length: the script is rebuilt."},
                  {"texts", arr}};
        ok = write_text(md / "texts.json", j.dump(1));
        E.dirty_texts = false;
    } else if (what == 1 || what == 2) {
        const bool it = what == 2;
        json j = it ? room_items_json(L.c) : room_enemies_json(L.c);
        j[it ? "items" : "enemies"] = it ? E.items : E.enemies;
        ok = write_text(md / (it ? "items.json" : "enemies.json"), j.dump(1));
        (it ? E.dirty_items : E.dirty_enemies) = false;
    } else if (what == 3) {
        std::vector<ScriptAtom> atoms;
        ScriptImage im;
        std::string err;
        if (!script_decompose(L.c.blocks[L.c.room_block].data, L.c.room, im, err) || !script_from_text(im, E.script, atoms, err)) {
            E.msg = std::string(T("Not saved: ", "Не сохранено: ")) + err;
            return;
        }
        Bytes sec;
        if (!script_assemble(im, atoms, sec, err)) {
            E.msg = std::string(T("Not saved: ", "Не сохранено: ")) + err;
            return;
        }
        if (E.script == E.script_orig) fs::remove(md / "script.txt", ec);
        else ok = write_text(md / "script.txt", E.script);
        E.dirty_script = false;
    }
    if (!ok) { E.msg = T("Cannot write to the mods folder", "Не пишется в папку mods"); return; }
    E.msg = T("Saved to mods, building...", "Сохранено в mods, сборка...");
    start_convert();
}

void App::revert_edit(Loaded& L, int what) {
    const fs::path md = room_mods_dir(*L.c.file);
    std::error_code ec;
    static const char* files[4] = {"texts.json", "enemies.json", "items.json", "script.txt"};
    fs::remove(md / files[what], ec);
    L.ed.loaded = false;
    ensure_edit(L);
    built_ = Sel();
    L.ed.msg = T("As in the game (the change is removed from mods), building...", "Как в игре (правка убрана из mods), сборка...");
    start_convert();
}

// строка кнопок над редактором: Сохранить в mods · Как в игре · сообщение
void App::edit_bar(Loaded& L, int what, bool dirty) {
    static const char* files[4] = {"texts.json", "enemies.json", "items.json", "script.txt"};
    const bool have = fs::exists(room_mods_dir(*L.c.file) / files[what]);
    ImGui::BeginDisabled(job_ != nullptr);
    if (ImGui::Button(dirty ? T("Save to mods *", "Сохранить в mods *") : T("Save to mods", "Сохранить в mods"))) save_edit(L, what);
    ImGui::SameLine();
    ImGui::BeginDisabled(!have && !dirty);
    if (ImGui::Button(T("As in the game", "Как в игре"))) revert_edit(L, what);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!L.ed.msg.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", L.ed.msg.c_str());
    }
}

// ---------------------------------------------------------------- тексты
void App::view_texts(const FileContent& c) {
    Loaded* L = load(sel_.f);
    if (!L) return;
    ensure_edit(*L);
    RoomEdit& E = L->ed;
    edit_bar(*L, 0, E.dirty_texts);
    if (!ImGui::BeginTable("texts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn(T("Text (\\n is a new line; any length)", "Текст (\\n - новая строка; любой длины)"));
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < E.texts.size() && i < c.room.strings.size(); i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool changed = E.texts[i] != E.texts_orig[i];
        if (changed) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%d*", (int)i);
        else ImGui::TextDisabled("%d", (int)i);
        ImGui::TableNextColumn();
        ImGui::PushID((int)i);
        int lines = 1;
        for (char ch : E.texts[i]) lines += ch == '\n';
        ImGui::SetNextItemWidth(-1);
        if (input_str("##t", E.texts[i], ImVec2(-1, ImGui::GetTextLineHeight() * (lines + 0.8f)))) E.dirty_texts = true;
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- скрипт
void App::view_script(const FileContent& c) {
    (void)c;
    Loaded* L = load(sel_.f);
    if (!L) return;
    ensure_edit(*L);
    RoomEdit& E = L->ed;
    edit_bar(*L, 3, E.dirty_script);
    ImGui::TextDisabled("%s", T("Line = command (hex). Keep @XXXX. No @: a new command.",
                                "Строка = команда (hex). @XXXX не трогать. Без @: новая команда."));
    if (input_str("##script", E.script, ImVec2(-1, -1))) E.dirty_script = true;
}

// ---------------------------------------------------------------- враги и предметы (панель рядом с 3D)
static int jint(const json& e, const char* k, int def = 0) { return e.count(k) && e[k].is_number() ? e[k].get<int>() : def; }

// 06.10.2026: a new enemy is a plain ACTOR record (op 20) whatever its model came from (a roster or follower record): its
// kind, model and motions, a place of its own, hunting (ai 1), its own death flag (make_new) - the build writes it so
static void enemy_as_actor(json& c) {
    if (c.value("op", std::string("20")) == "20") return;
    json n;
    for (const char* k : {"kind", "kind_name", "variant", "model", "motions", "species", "copy_of"})
        if (c.count(k)) n[k] = c[k];
    n["op"] = "20";
    n["ai"] = 1;
    n["hp"] = 0;
    n["death_flag"] = 0;
    n["x"] = jint(c, "x"); n["y"] = jint(c, "y"); n["z"] = jint(c, "z"); n["rotation"] = jint(c, "rotation");
    c = n;
}

// 06.10.2026 (player: "let the item be turned - about all three axes"): an item's turn (x, y up, z; 4096 = 360) as the game
// shows it now - its section after the script's edits and the Studio's own (pieces.cpp), 0 without a model; a turn changed
// in the Studio stores all three (the build writes a TURN of the section with them)
static void item_rot(const Loaded& L, const json& items, const json& e, int r[3]) {
    r[0] = r[1] = r[2] = 0;
    int idx = -1;
    for (size_t k = 0; k < items.size(); k++) if (&items[k] == &e) idx = (int)k;
    for (auto& m : room_item_models(L.c, L.ed.script, items))
        if (m.value("index", -2) == idx) { r[0] = m.value("rx", 0) & 4095; r[1] = m.value("ry", 0) & 4095; r[2] = m.value("rz", 0) & 4095; return; }
    if (e.count("rx")) r[0] = e["rx"].get<int>() & 4095;
    if (e.count("ry")) r[1] = e["ry"].get<int>() & 4095;
    if (e.count("rz")) r[2] = e["rz"].get<int>() & 4095;
}
static void item_rot_set(json& e, const int r[3]) {
    e["rx"] = r[0] & 4095; e["ry"] = r[1] & 4095; e["rz"] = r[2] & 4095;
}

// 06.10.2026: человек в списке врагов — по имени персонажа (модель команды появления → ключ → каталог: Гейл, Рик…)
static std::string person_of(const Library& lib, const FileContent& c, const json& e) {
    if (!lib.ready.load() || e.value("kind_name", std::string()) != "human") return std::string();
    uint32_t addr = 0;
    try { addr = (uint32_t)std::stoul(e.value("model", std::string("0")), nullptr, 16); } catch (...) { return std::string(); }
    for (const ActorPart& a : c.actors) {
        if (a.block < 0 || a.block >= (int)c.blocks.size()) continue;
        const RamBlock& b = c.blocks[a.block];
        if (b.dest + a.model.hdr != addr) continue;
        const uint64_t k = actor_key_blk(b.data, b.dest, a.model.hdr);
        for (const LibActor& la : lib.actors)
            if (la.key == k && la.chr >= 0 && la.chr < (int)lib.chars.size() && !lib.chars[la.chr].en.empty()) {
                std::string n = T(lib.chars[la.chr].en.c_str(), lib.chars[la.chr].ru.c_str());
                if (!la.var_en.empty()) n += std::string(", ") + (g_lang == LANG_RU ? la.var_ru : la.var_en);
                return n;
            }
    }
    return std::string();
}

// 06.10.2026 (player: "an added enemy comes once; killed - never again; like items"; "hidden, quest... make them a choice"):
// a new copy gets its own flag at once (kept in its json - the same flag on every later build), an item is seen
int App::new_flag(bool enemy) {
    if (!flags_ready_.load()) return -1;
    std::set<int> used = enemy ? flags45_ : flags7_;
    const char* key = enemy ? "death_flag" : "taken_flag";
    const char* arrk = enemy ? "enemies" : "items";
    std::error_code ec;
    const fs::path rooms = work_root() / "rooms";   // 10.10: mods or the package the studio works in
    if (!set_.mods.empty() && fs::is_directory(rooms, ec))       // new things of every room saved in mods
        for (auto& d : fs::directory_iterator(rooms, ec)) {
            Bytes b;
            if (!d.is_directory() || !read_file(d.path() / (std::string(arrk) + ".json"), b)) continue;
            try {
                json j = json::parse(b.begin(), b.end());
                for (auto& e : j.at(arrk))
                    if (e.count("copy_of") && e.count(key) && e[key].is_number()) used.insert(e[key].get<int>());
            } catch (...) {
            }
        }
    // 14.10.2026: enemies a scenario adds (new 0x20 lines "#N  20 ss kk vv DF ..." of script.txt; the unsaved ones below)
    if (enemy && !set_.mods.empty() && fs::is_directory(rooms, ec))
        for (auto& d : fs::directory_iterator(rooms, ec)) {
            Bytes b;
            if (!d.is_directory() || !read_file(d.path() / "script.txt", b)) continue;
            std::string line;
            std::istringstream in(std::string(b.begin(), b.end()));
            while (std::getline(in, line)) {
                if (line.empty() || line[0] != '#') continue;
                std::istringstream ls(line);
                std::string lab, h[5];
                ls >> lab >> h[0] >> h[1] >> h[2] >> h[3] >> h[4];
                if (h[0] == "20" && h[4].size() == 2) used.insert((int)strtol(h[4].c_str(), nullptr, 16));
            }
        }
    for (auto& kv : cache_) {
        if (!kv.second || !enemy) continue;
        for (auto& a : kv.second->ed.scn_atoms)
            if (a.old < 0 && a.kind == ScriptAtom::OP && a.b.size() >= 5 && a.b[0] == 0x20) used.insert(a.b[4]);
    }
    for (auto& kv : cache_) {                                       // and the unsaved ones of this session
        if (!kv.second) continue;
        const json& arr = enemy ? kv.second->ed.enemies : kv.second->ed.items;
        if (!arr.is_array()) continue;
        for (auto& e : arr)
            if (e.count("copy_of") && e.count(key) && e[key].is_number()) used.insert(e[key].get<int>());
    }
    for (int b = enemy ? 255 : flags_bits7_ - 1; b >= 1; b--)       // from the top: away from the game's own
        if (!used.count(b)) return b;
    return -1;
}

void App::make_new(json& c, bool items) {
    if (items) {
        c["hidden"] = "00000000";
        if (jint(c, "item", 0) == 0xFF || jint(c, "item", 0) == 0) c["item"] = 1;   // a hiding place copies as a real item
        const int f = new_flag(false);
        if (f >= 0) c["taken_flag"] = f; else c.erase("taken_flag");               // the build picks one
    } else if (c.value("op", std::string()) == "20") {
        const int f = new_flag(true);
        if (f >= 0) c["death_flag"] = f; else c["death_flag"] = 0;                 // 0: the build picks one
    }
}

// ---------------------------------------------------------------- 06.10.2026: враг = вид (внешность), роль, характер
// Player: "sort the enemies by what they are: a scene one, or a mob you fight and can kill; two raptors that even look
// different - that is all the kinds; the variants into one plain choice". Measured: every raptor of the game is one model
// (21 bones, 300 triangles); the texture makes the two looks (stages 1-3 olive-green, stage 5 grey). A raptor of a scene
// (ST10D: 3 motions) given the hunting brain asks for motions its table has not - the model twists and flies; a fighter
// carries the full set (59-70). The variant of a raptor is a row of the attack table (0x8009AE48, rows 0..8): 0-2 never
// leap and wait longer, 3-5 leap now and then, 6-8 leap often; 9 and 10 are past the table (scene raptors).

// motions in a record's table (the table ends where the first motion begins). 06.10.2026: an entry that is a link
// {u32 another motion, u32 -1} does not count - every raptor table has 76 entries, a scene raptor's (ST10D) links all but
// 3 of them to those 3; counted as the library counts (library.cpp, LibSpecies::nmot)
static int table_motions(const FileContent& c, uint32_t tab) {
    auto rd32 = [&](uint32_t addr, uint32_t& v) {
        for (auto& b : c.blocks)
            if (addr >= b.dest && addr + 4 <= b.dest + b.data.size()) { v = le32(&b.data[addr - b.dest]); return true; }
        return false;
    };
    uint32_t first = 0;
    int n = 0;
    for (int i = 0; i < 300; i++) {
        uint32_t v = 0, w = 0;
        if (!rd32(tab + (uint32_t)i * 4u, v) || v < 0x80000000u || v >= 0x80200000u) break;
        if (i == 0) first = v;
        if (first && tab + (uint32_t)i * 4u >= first) break;
        if (rd32(v + 4u, w) && w == 0xFFFFFFFFu) continue;
        n++;
    }
    return n;
}

static std::string species_name(const Library& lib, const LibSpecies& s) {
    for (const LibActor& la : lib.actors)
        if (la.key == s.key && la.chr >= 0 && la.chr < (int)lib.chars.size() && !lib.chars[la.chr].en.empty()) {
            std::string n = T(lib.chars[la.chr].en.c_str(), lib.chars[la.chr].ru.c_str());
            if (!la.var_en.empty()) n += std::string(", ") + (g_lang == LANG_RU ? la.var_ru : la.var_en);
            return n;
        }
    // 06.10.2026 (player: "one is called Model, another Dinosaur - fix the names"): the catalog's name of the body (compsognathus,
    // dino = Therizinosaurus), not the kind's (small_creature had no label)
    if (!s.actor.empty()) {
        const std::string n = actor_label(s.actor);
        if (n != actor_label(std::string())) return n;   // a known body; people without a name of their own - by kind below
    }
    return s.kind_name.empty() ? strfmt("kind %d", s.kind) : actor_label(s.kind_name);
}

static const char* color_label(const std::string& c) {
    if (c == "green") return T("green", "зелёный");
    if (c == "grey") return T("grey", "серый");
    if (c == "brown") return T("brown", "бурый");
    if (c == "blue") return T("blue", "синий");
    if (c == "purple") return T("purple", "фиолетовый");
    if (c == "dark") return T("dark", "тёмный");
    return "";
}

// a record's look: its model in this room (or the kind it was given from another room) and the motions its table has
bool App::enemy_record_look(Loaded& L, const json& o, uint64_t& key, uint64_t& look, int& nmot) {
    key = look = 0;
    nmot = 0;
    uint32_t model = 0, tab = 0;
    try {
        model = (uint32_t)std::stoul(o.value("model", std::string("0")), nullptr, 16);
        tab = (uint32_t)std::stoul(o.value("motions", std::string("0")), nullptr, 16);
    } catch (...) { return false; }
    if (o.count("species") && o["species"].is_object()) {
        const std::string room = o["species"].value("room", std::string());
        for (auto& s : lib_.species)
            if (s.f && s.f->stem == room && strfmt("%08X", s.model) == o["species"].value("model", std::string()) &&
                strfmt("%08X", s.motions) == o["species"].value("motions", std::string())) {
                key = s.key; look = s.look; nmot = s.nmot;
                return true;
            }
        return false;
    }
    for (const ActorPart& a : L.c.actors) {
        if (a.block < 0 || a.block >= (int)L.c.blocks.size() || L.c.blocks[a.block].dest + a.model.hdr != model) continue;
        key = actor_key_blk(L.c.blocks[a.block].data, L.c.blocks[a.block].dest, a.model.hdr);
        auto& lk = look_cache_[std::make_pair(L.c.file, model)];
        if (!lk.first) { std::string col; model_look(L.c.vram, a.model, lk.second, col); lk.first = true; }
        look = lk.second;
        nmot = table_motions(L.c, tab);
        return true;
    }
    return false;
}

void App::enemy_look_role(Loaded& L, json& e, bool& changed) {
    struct Look { int kind; uint64_t key, look; std::string name; const LibSpecies* full; };
    const int stage = L.c.file && L.c.file->stem.size() >= 3 ? L.c.file->stem[2] - '0' : 0;
    // 06.10.2026 (player: "where is adding any enemy to any room - only the raptor is there; where are the dinosaurs"): every
    // kind of the game; a kind of another stage brings its stage's code on save (convert.cpp: a bank + the stage's table)
    std::vector<Look> looks;
    if (lib_.ready.load())
        for (auto& s : lib_.species) {
            if (s.actor.empty() || s.kind < 0 || s.kind > 31) continue;
            Look* have = nullptr;
            for (auto& l : looks) if (l.kind == s.kind && l.key == s.key && l.look == s.look) have = &l;
            if (have) { if (s.nmot > have->full->nmot) have->full = &s; continue; }
            looks.push_back({s.kind, s.key, s.look, std::string(), &s});
        }
    for (auto& l : looks) {
        int same = 0, maxmot = 0;
        for (auto& o : looks) {
            same += o.kind == l.kind && o.key == l.key;
            if (o.kind == l.kind) maxmot = std::max(maxmot, o.full->nmot);   // the fullest set of the kind = a fighter's
        }
        l.name = species_name(lib_, *l.full);
        if (same > 1 && *color_label(l.full->color)) l.name += std::string(" (") + color_label(l.full->color) + ")";
        // 06.10.2026: a version with a scene's few motions (the T-rex of ST201 waits for its room's script) - marked
        if (l.kind != 1 && l.full->nmot * 2 < maxmot) l.name += T(" (scene)", " (сцена)");
    }
    uint64_t rkey = 0, rlook = 0;
    int rmot = 0;
    const bool known = lib_.ready.load() && enemy_record_look(L, e, rkey, rlook, rmot);
    int cur = -1;
    for (size_t k = 0; k < looks.size(); k++)
        if (known && looks[k].kind == jint(e, "kind") && looks[k].key == rkey && looks[k].look == rlook) cur = (int)k;
    const bool person = jint(e, "kind") == 1;
    // the role: as set in the Studio; else the game's: the script moves it (AI bit 0x80) or its table is a scene's
    std::string role = e.value("role", std::string());
    if (role != "enemy" && role != "scene") {
        if (person || (jint(e, "ai") & 0x80)) role = "scene";
        else if (e.count("copy_of")) role = "enemy";                                     // an added one is there to fight
        else role = (cur >= 0 && rmot * 2 < looks[cur].full->nmot) ? "scene" : "enemy";
    }
    auto fierce_of = [](int v) { return v <= 2 ? 0 : v <= 5 ? 1 : 2; };
    static const int kFierce[3] = {1, 4, 7};
    // take a look with a role: the room's own model and motions when they serve (a fighter needs a full set), else the
    // look's full set from the room that has it (the build puts it into the memory for mods)
    auto apply = [&](const Look& l, bool enemy, int variant) {
        e["kind"] = l.kind;
        e["kind_name"] = l.full->kind_name;
        const json* own = nullptr;
        for (auto& o : L.enemies) {
            if (o.value("op", std::string()) != "20") continue;
            uint64_t k2, lk2;
            int n2;
            if (enemy_record_look(L, o, k2, lk2, n2) && k2 == l.key && lk2 == l.look && (!enemy || n2 * 2 >= l.full->nmot)) { own = &o; break; }
        }
        if (own) {
            e["model"] = (*own)["model"];
            e["motions"] = (*own)["motions"];
            e.erase("species");
        } else {
            e["model"] = strfmt("%08X", l.full->model);
            e["motions"] = strfmt("%08X", l.full->motions);
            e["species"] = {{"room", l.full->f->stem}, {"model", strfmt("%08X", l.full->model)},
                            {"motions", strfmt("%08X", l.full->motions)}, {"actor", l.full->actor}};
        }
        if (l.kind == 1) {                                       // people: as the game has them (no fighting)
            e["ai"] = l.full->ai >= 0 ? l.full->ai : 0;
            e["variant"] = l.full->variant;
            e.erase("role");
        } else if (l.kind == 2) {
            e["ai"] = enemy ? 1 : 0x80;                          // hunting / the script moves it
            e["variant"] = variant;
            e["role"] = enemy ? "enemy" : "scene";
        } else {
            // 06.10.2026: the other dinosaurs keep the behaviour number and variant of the room they come from; the script
            // takes over with bit 0x80, as with the raptor
            const int sai = l.full->ai >= 0 ? l.full->ai : 1;
            e["ai"] = enemy ? (sai & 0x7F) : (sai | 0x80);
            e["variant"] = l.full->variant;
            e["role"] = enemy ? "enemy" : "scene";
        }
        changed = true;
    };
    const int fv = jint(e, "variant");
    const int vdef = (jint(e, "kind") == 2 && fv > 8) ? 4 : fv;   // a scene raptor's 9/10: past the attack table
    // an added fighter still with a scene's motions (the Studio before 06.10 evening): the full set now
    if (role == "enemy" && !person && cur >= 0 && e.count("copy_of") && rmot * 2 < looks[cur].full->nmot) {
        apply(looks[cur], true, vdef);
        rmot = looks[cur].full->nmot;
    }
    ImGui::SetNextItemWidth(240);
    const std::string shown = cur >= 0 ? looks[cur].name : (person_of(lib_, L.c, e).empty() ? actor_label(e.value("kind_name", std::string()))
                                                                                         : person_of(lib_, L.c, e));
    if (ImGui::BeginCombo(T("enemy", "враг"), shown.c_str())) {
        for (size_t k = 0; k < looks.size(); k++) {
            ImGui::PushID((int)k);
            if (ImGui::Selectable(looks[k].name.c_str(), (int)k == cur) && (int)k != cur)
                apply(looks[k], looks[k].kind != 1 && role == "enemy", looks[k].kind == 2 ? kFierce[fierce_of(vdef)] : looks[k].full->variant);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Every enemy of the game, by how it looks. One of another room or stage comes in on save.",
                                  "Все враги игры, по внешности. Враг другой комнаты или этапа копируется при сохранении."));
    if (!person && cur >= 0) {
        const bool enemy = role == "enemy";
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo(T("role", "роль"), enemy ? T("enemy", "враг") : T("scene", "сцена"))) {
            if (ImGui::Selectable(T("enemy", "враг"), enemy) && !enemy) apply(looks[cur], true, jint(e, "kind") == 2 ? kFierce[fierce_of(vdef)] : vdef);
            if (ImGui::Selectable(T("scene", "сцена"), !enemy) && enemy) apply(looks[cur], false, fv);
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Enemy: hunts, fights, can be killed. Scene: stands, only the room's script moves it.",
                                      "Враг: охотится, дерётся, его можно убить. Сцена: стоит, двигает его только скрипт комнаты."));
        if (enemy && jint(e, "kind") == 2) {
            static const char* fn[3][2] = {{"calm", "спокойный"}, {"normal", "обычный"}, {"fierce", "яростный"}};
            const int f = fierce_of(vdef);
            ImGui::SetNextItemWidth(120);
            if (ImGui::BeginCombo(T("temper", "характер"), T(fn[f][0], fn[f][1]))) {
                for (int k = 0; k < 3; k++)
                    if (ImGui::Selectable(T(fn[k][0], fn[k][1]), k == f) && k != f) { e["variant"] = kFierce[k]; changed = true; }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Calm: never leaps. Normal: leaps now and then. Fierce: leaps often.",
                                          "Спокойный: не прыгает. Обычный: иногда прыгает. Яростный: прыгает часто."));
        }
        if (e.count("species") && e["species"].is_object()) {
            // 06.10.2026: the build takes this room's own model of the same body (its pictures are here, species.cpp)
            bool body_here = false;
            for (const ActorPart& a : L.c.actors)
                if (a.block >= 0 && a.block < (int)L.c.blocks.size() &&
                    actor_key_blk(L.c.blocks[a.block].data, L.c.blocks[a.block].dest, a.model.hdr) == looks[cur].key) body_here = true;
            const std::string room = e["species"].value("room", std::string());
            if (room.size() >= 3 && room[2] - '0' != stage)
                ImGui::TextDisabled("%s", strfmt(T("Its behaviour: the code of stage %c (memory for mods)",
                                                   "Поведение: код этапа %c (память модов)"), room[2]).c_str());
            if (body_here)
                ImGui::TextDisabled("%s", strfmt(T("Motions from %s (memory for mods), the model of this room",
                                                   "Движения из %s (память модов), модель этой комнаты"), room.c_str()).c_str());
            else   // 06.10.2026: its pictures go with its model to mods\rooms\<ROOM>\kinds (the game draws it from there)
                ImGui::TextDisabled("%s", strfmt(T("Model and motions from %s (memory for mods), its pictures in rooms\\%s\\kinds",
                                                   "Модель и движения из %s (память модов), картинки в rooms\\%s\\kinds"),
                                                 room.c_str(), L.c.file ? L.c.file->stem.c_str() : "").c_str());
        }
    }
}

void App::placement_panel(Loaded& L, bool items) {
    RoomEdit& E = L.ed;
    json& arr = items ? E.items : E.enemies;
    edit_bar(L, items ? 2 : 1, items ? E.dirty_items : E.dirty_enemies);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", T("Click: pick. Drag: move. Q/E: turn. Delete: remove. Right click: add.",
                               "Щелчок: выбрать. Тянуть: двигать. Q/E: повернуть. Delete: убрать. Правая кнопка: добавить."));
    ImGui::PopStyleColor();
    bool changed = false;
    int copy_from = -1;
    ImGui::BeginChild("list");
    for (size_t i = 0; i < arr.size(); i++) {
        json& e = arr[i];
        ImGui::PushID((int)i);
        const bool removed = e.value("removed", false);
        const bool added = e.count("copy_of") > 0;
        std::string title;
        if (items) {
            const int id = jint(e, "item", 0);
            title = (id == 0xFF ? std::string(T("random", "случайный")) : item_name(id)) + strfmt(" x%d", jint(e, "count", 1));
        } else {
            const std::string kn = e.value("kind_name", std::string());
            const std::string who = person_of(lib_, L.c, e);
            title = (!who.empty() ? who : kn.empty() ? strfmt("kind %d", jint(e, "kind")) : actor_label(kn)) +
                    (e.count("slot") ? strfmt(" (%s %d)", T("slot", "слот"), jint(e, "slot")) : std::string());
            const std::string op = e.value("op", std::string());
            if (op == "59") title += T("  roster", "  ростер");
            if (op == "5B") title += T("  follows", "  следом");
        }
        if (added) title += T("  NEW", "  НОВЫЙ");
        if (removed) title += T("  REMOVED", "  УБРАН");
        const bool is_sel = edit_file_ == L.c.file && edit_items_ == items && edit_sel_ == (int)i;
        if (is_sel && edit_scroll_) { ImGui::SetNextItemOpen(true); }
        if (removed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1));
        if (is_sel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.75f, 0.42f, 0.18f, 1));
        const bool open = ImGui::CollapsingHeader((title + "###h").c_str(), added ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        if (ImGui::IsItemClicked()) {                        // выбор из списка — та же метка во вьюпорте
            edit_file_ = L.c.file; edit_items_ = items; edit_sel_ = (int)i;
            placement_refresh(L, items);
        }
        if (is_sel) { ImGui::PopStyleColor(); if (edit_scroll_) { ImGui::SetScrollHereY(0.2f); edit_scroll_ = false; } }
        if (removed) ImGui::PopStyleColor();
        if (open) {
            ImGui::Indent(8);
            auto num = [&](const char* label, const char* key, int lo, int hi) {
                if (!e.count(key)) return;
                int v = jint(e, key);
                ImGui::SetNextItemWidth(90);
                if (ImGui::InputInt(label, &v, 0, 0)) { e[key] = std::clamp(v, lo, hi); changed = true; }
            };
            if (items) {
                // предмет: что и сколько; место — середина зоны (сдвиг всех углов) и размер
                int id = jint(e, "item");
                ImGui::SetNextItemWidth(220);
                if (ImGui::BeginCombo(T("item", "предмет"), id == 0xFF ? T("random", "случайный") : item_name(id).c_str())) {
                    for (int k = 1; k < 0x70; k++) {
                        const std::string n = item_name(k);
                        if (n.empty() || n == "?") continue;
                        if (ImGui::Selectable(strfmt("%3d  %s", k, n.c_str()).c_str(), k == id)) { e["item"] = k; changed = true; }
                    }
                    ImGui::EndCombo();
                }
                num(T("count", "количество"), "count", 0, 999);
                if (e.count("zone") && e["zone"].size() == 4) {
                    json& q = e["zone"];
                    float cx = 0, cz = 0, x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
                    for (int k = 0; k < 4; k++) {
                        const float x = q[k][0].get<int>(), z = q[k][1].get<int>();
                        cx += x / 4; cz += z / 4;
                        x0 = std::min(x0, x); x1 = std::max(x1, x); z0 = std::min(z0, z); z1 = std::max(z1, z);
                    }
                    // 06.10.2026 (игрок: «нормальные величины, а не нативные из игры»): метры (в игре — миллиметры)
                    float pos[2] = {cx / 1000.0f, cz / 1000.0f};
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::InputFloat2(T("place X, Z (m)", "место X, Z (м)"), pos, "%.2f")) {
                        const int dx = (int)lroundf(pos[0] * 1000.0f) - (int)lroundf(cx), dz = (int)lroundf(pos[1] * 1000.0f) - (int)lroundf(cz);
                        for (int k = 0; k < 4; k++) { q[k][0] = q[k][0].get<int>() + dx; q[k][1] = q[k][1].get<int>() + dz; }
                        changed = true;
                    }
                    float sz[2] = {(x1 - x0) / 1000.0f, (z1 - z0) / 1000.0f};
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::InputFloat2(T("zone size (m)", "размер зоны (м)"), sz, "%.2f")) {
                        const float sx = (x1 > x0) ? std::max(200.0f, sz[0] * 1000.0f) / (x1 - x0) : 1,
                                    szz = (z1 > z0) ? std::max(200.0f, sz[1] * 1000.0f) / (z1 - z0) : 1;
                        for (int k = 0; k < 4; k++) {
                            q[k][0] = (int)lroundf(cx + (q[k][0].get<int>() - cx) * sx);
                            q[k][1] = (int)lroundf(cz + (q[k][1].get<int>() - cz) * szz);
                        }
                        changed = true;
                    }
                }
                num(T("floor", "этаж"), "floor", 0, 7);
                {   // 06.10.2026: seen, or hidden (found only by searching the spot)
                    const bool hid = e.value("hidden", std::string("00000000")) != "00000000";
                    ImGui::SetNextItemWidth(120);
                    if (ImGui::BeginCombo(T("shown", "видно"), hid ? T("hidden", "спрятан") : T("visible", "виден"))) {
                        if (ImGui::Selectable(T("visible", "виден"), !hid) && hid) { e["hidden"] = "00000000"; changed = true; }
                        if (ImGui::Selectable(T("hidden", "спрятан"), hid) && !hid) { e["hidden"] = "00000001"; changed = true; }
                        ImGui::EndCombo();
                    }
                }
                {   // 06.10.2026 (player: "let the item be turned - about all three axes"): written into the room's script
                    int r[3];
                    item_rot(L, arr, e, r);
                    float deg[3];
                    for (int k = 0; k < 3; k++) { deg[k] = r[k] * 360.0f / 4096.0f; if (deg[k] > 180.0f) deg[k] -= 360.0f; }
                    ImGui::SetNextItemWidth(240);
                    if (ImGui::InputFloat3(T("turn X, Y, Z (deg)", "поворот X, Y, Z (°)"), deg, "%.1f")) {
                        for (int k = 0; k < 3; k++) r[k] = (int)lroundf(deg[k] * 4096.0f / 360.0f) & 4095;
                        item_rot_set(e, r);
                        changed = true;
                    }
                }
            } else {
                // 06.10.2026: метры (высота — вверх), градусы; в игре — миллиметры (Y вниз) и 4096 на круг
                if (e.count("x") && e.count("z")) {
                    float p[3] = {jint(e, "x") / 1000.0f, -jint(e, "y") / 1000.0f, jint(e, "z") / 1000.0f};
                    ImGui::SetNextItemWidth(240);
                    if (ImGui::InputFloat3(T("X, height, Z (m)", "X, высота, Z (м)"), p, "%.2f")) {
                        e["x"] = std::clamp((int)lroundf(p[0] * 1000.0f), -32768, 32767);
                        if (e.count("y")) e["y"] = std::clamp((int)lroundf(-p[1] * 1000.0f), -32768, 32767);
                        e["z"] = std::clamp((int)lroundf(p[2] * 1000.0f), -32768, 32767);
                        changed = true;
                    }
                } else ImGui::TextDisabled("%s", T("Place: from the stage roster", "Место: из ростера этапа"));
                if (e.count("rotation")) {
                    float deg = (jint(e, "rotation") & 4095) * 360.0f / 4096.0f;
                    ImGui::SetNextItemWidth(90);
                    if (ImGui::InputFloat(T("turn (deg)", "поворот (°)"), &deg, 0, 0, "%.1f")) {
                        e["rotation"] = (int)lroundf(deg * 4096.0f / 360.0f) & 4095;
                        changed = true;
                    }
                }
                // 06.10.2026 (player: "sort the enemies by what they are: a scene one, or a mob you fight and can kill; there are
                // two raptors that even look different - that is all the kinds; the variants into one plain choice, like hidden
                // for items"): the enemy is a LOOK (kind + model + the picture it wears) with a ROLE - enemy (hunts, fights,
                // dies: the full set of the kind's motions) or scene (stands, only the room's script moves it) - and, for a
                // raptor, how FIERCE it is (the variant picks a row of the game's attack table: three tiers). A look this room
                // does not have, or an enemy whose motions here are a scene's (3 of 65), takes the full set from the room
                // that has it - the build puts it into the memory for mods (no room memory needed).
                if (e.value("op", std::string("20")) == "20") enemy_look_role(L, e, changed);
                else {
                    ImGui::TextDisabled("%s", e.value("op", std::string()) == "59"
                                                  ? T("A dinosaur of the stage roster", "Динозавр из ростера этапа")
                                                  : T("A dinosaur that follows into the room", "Динозавр, который идёт следом"));
                    num(T("roster room", "комната ростера"), "roster_room", 0, 255);
                }
                num(T("health (0: default)", "здоровье (0: обычное)"), "hp", 0, 65535);
            }
            if (ImGui::Button(T("Copy", "Копия"))) copy_from = (int)i;
            ImGui::SameLine();
            if (added) {
                if (ImGui::Button(T("Delete", "Удалить"))) { arr.erase(arr.begin() + (long)i); changed = true; ImGui::Unindent(8); ImGui::PopID(); break; }
            } else if (ImGui::Button(removed ? T("Restore", "Вернуть") : T("Remove", "Убрать"))) {
                if (removed) e.erase("removed");
                else e["removed"] = true;
                changed = true;
            }
            ImGui::Unindent(8);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (copy_from >= 0) {
        json c = arr[copy_from];
        const int src = c.count("copy_of") ? c["copy_of"].get<int>() : c.value("offset", -1);
        if (src >= 0) {
            for (const char* k : {"offset", "removed", "subroutine", "slot", "zone_index", "taken_flag", "model_section"}) c.erase(k);
            c["copy_of"] = src;
            if (!items) enemy_as_actor(c);
            // рядом с образцом: на 600 мм по x
            if (items && c.count("zone")) for (auto& v : c["zone"]) v[0] = v[0].get<int>() + 600;
            if (!items && c.count("x")) c["x"] = c["x"].get<int>() + 600;
            make_new(c, items);
            arr.push_back(c);
            changed = true;
        }
    }
    if (changed) {
        (items ? E.dirty_items : E.dirty_enemies) = true;
        placement_refresh(L, items);
    }
}

void App::placement_refresh(Loaded& L, int what) {
    // 06.10.2026: an item's model follows its zone in the view (room_item_models)
    if ((what == 1 || what == 0) && L.c.is_room) v3d_.set_mesh(room_view_tris(L), room_vram(L), true);
    std::vector<float> lines;
    const int sel = (edit_file_ == L.c.file && edit_items_ == what) ? edit_sel_ : -1;
    if (what == 2) {                                   // pieces: a cross and a post at the place (the picked one white)
        for (int i = 0; i < (int)L.ed.pieces.size(); i++) {
            const json& p = L.ed.pieces[i];
            if (!p.value("placed", false) || p.value("lead", i) != i) continue;
            const float x = (float)p.value("x", 0), y = (float)p.value("y", 0), z = (float)p.value("z", 0);
            const float c0 = i == sel ? 1.0f : 0.35f, c1 = i == sel ? 1.0f : 1.0f, c2 = i == sel ? 1.0f : 0.55f;
            const float s = 250;
            const float m[36] = {x - s, y, z, c0, c1, c2, x + s, y, z, c0, c1, c2, x, y, z - s, c0, c1, c2, x, y, z + s, c0, c1, c2,
                                 x, y, z, c0, c1, c2, x, y - 900, z, c0, c1, c2};
            lines.insert(lines.end(), m, m + 36);
        }
        v3d_.set_lines(lines);
        return;
    }
    placement_overlay_sel(what == 1 ? L.ed.items : L.ed.enemies, what, sel, lines);
    v3d_.set_lines(lines);
}

// an edit of a piece goes to every command that puts the same piece at the same place (pieces.cpp "lead")
static void pieces_sync(json& pcs, int lead) {
    if (lead < 0 || lead >= (int)pcs.size()) return;
    for (auto& p : pcs)
        if (p.value("lead", -1) == lead)
            for (const char* k : {"x", "y", "z", "rx", "ry", "rz"}) p[k] = pcs[lead][k];
}

const FileContent* App::side_content(const std::string& name) {
    auto it = side_c_.find(name);
    if (it != side_c_.end()) return it->second.get();
    const DiscFile* f = disc_.find(name);
    auto c = std::make_unique<FileContent>();
    if (!f || !analyze(disc_, *f, *c)) c.reset();
    return (side_c_[name] = std::move(c)).get();
}

const Vram& App::room_vram(Loaded& L) {
    if (!L.view_vram_ok) {
        L.view_vram_ok = true;
        L.view_vram = L.c.vram;
        if (const FileContent* core = side_content("CORE.DAT"))
            for (const Upload& u : core->uploads)
                for (int y = std::max(0, u.y); y < u.y + u.h && y < 512; y++)
                    for (int x = std::max(0, u.x); x < u.x + u.w && x < 1024; x++)
                        L.view_vram.px[(size_t)y * 1024 + x] = core->vram.px[(size_t)y * 1024 + x];
    }
    return L.view_vram;
}

// a model in its rest pose at a place (turn about the vertical, as the game turns an actor); flat - no picture (a kind
// from another room whose pictures could not be put into the view's video memory), a grey shape; remap - page and palette
// (tpage << 16 | clut) -> where foreign_view_tex put them
using TexRemap = std::map<uint32_t, std::pair<uint16_t, uint16_t>>;
static void actor_place(const Model& m, int x, int y, int z, int ry, bool flat, std::vector<MTri>& out, const TexRemap* remap = nullptr) {
    std::vector<std::array<float, 3>> rest(m.bones.size());
    for (size_t b = 0; b < m.bones.size(); b++) {
        const Bone& bo = m.bones[b];
        const bool par = bo.parent >= 0 && bo.parent < (int)b;
        rest[b] = {(par ? rest[bo.parent][0] : 0.f) + bo.ox, (par ? rest[bo.parent][1] : 0.f) + bo.oy, (par ? rest[bo.parent][2] : 0.f) + bo.oz};
    }
    float R[9];
    piece_rot(0, ry, 0, R);
    auto c16 = [](float f) { return (int16_t)std::max(-32768.0f, std::min(32767.0f, roundf(f))); };
    for (const APrim& p : m.prims) {
        MTri t;
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) {
            const int vi = p.vi[k];
            if (vi < 0 || vi >= (int)m.verts.size()) { ok = false; break; }
            const int b = vi < (int)m.vbone.size() && m.vbone[vi] < (int)rest.size() ? m.vbone[vi] : 0;
            const float vx = m.verts[vi][0] + (rest.empty() ? 0.f : rest[b][0]), vy = m.verts[vi][1] + (rest.empty() ? 0.f : rest[b][1]),
                        vz = m.verts[vi][2] + (rest.empty() ? 0.f : rest[b][2]);
            t.v[k].x = c16(x + R[0] * vx + R[1] * vy + R[2] * vz);
            t.v[k].y = c16(y + R[3] * vx + R[4] * vy + R[5] * vz);
            t.v[k].z = c16(z + R[6] * vx + R[7] * vy + R[8] * vz);
            t.v[k].u = (uint8_t)(p.uv[k] & 0xFF);
            t.v[k].v = (uint8_t)(p.uv[k] >> 8);
        }
        if (!ok) continue;
        bool grey = flat;
        t.tpage = p.tpage;
        t.clut = p.clut;
        if (remap) {
            auto it = remap->find(((uint32_t)p.tpage << 16) | p.clut);
            if (it != remap->end()) { t.tpage = it->second.first; t.clut = it->second.second; grey = false; }
            else grey = true;
        }
        if (grey) { t.tpage = 0xFFFF; t.clut = 0; }
        t.code = 0x34;
        t.group = 0xFFFF;
        if (grey) t.rgb[0] = t.rgb[1] = t.rgb[2] = 0x8C8C8C;
        out.push_back(t);
    }
}

// 06.10.2026 (player: "in the Studio their textures are not shown - they are just white"): a kind from another room has its
// pictures in that room's video memory. For the view (the game draws it from mods\rooms\<ROOM>\kinds): its texture pages and
// palettes are copied from there into places of the view's video memory no upload of the room or CORE takes (the frame
// buffer columns are free here); `used` marks what is taken, the triangles get the new page and palette (UV the same).
static void vram_mark(std::vector<uint8_t>& used, int x, int y, int w, int h) {
    for (int yy = std::max(0, y); yy < std::min(512, y + h); yy++)
        for (int xx = std::max(0, x); xx < std::min(1024, x + w); xx++) used[(size_t)yy * 1024 + xx] = 1;
}
static bool vram_free(const std::vector<uint8_t>& used, int x, int y, int w, int h) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 1024 || y + h > 512) return false;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (used[(size_t)yy * 1024 + xx]) return false;
    return true;
}
static void foreign_view_tex(const Model& m, const Vram& src, Vram& dst, std::vector<uint8_t>& used, TexRemap& remap) {
    struct R { int u0 = 255, v0 = 255, u1 = 0, v1 = 0; };
    std::map<uint16_t, R> pages;
    std::map<uint16_t, int> cluts;                      // palette -> its width (16 or 256)
    for (const APrim& p : m.prims) {
        R& r = pages[p.tpage];
        for (int k = 0; k < 3; k++) {
            const int u = p.uv[k] & 0xFF, v = p.uv[k] >> 8;
            r.u0 = std::min(r.u0, u); r.u1 = std::max(r.u1, u); r.v0 = std::min(r.v0, v); r.v1 = std::max(r.v1, v);
        }
        const int depth = (p.tpage >> 7) & 3;
        if (depth < 2) cluts[p.clut] = std::max(cluts[p.clut], depth == 1 ? 256 : 16);
    }
    std::map<uint16_t, uint16_t> new_page, new_clut;
    for (auto& kv : pages) {
        const uint16_t tp = kv.first;
        const int depth = (tp >> 7) & 3, div = depth == 0 ? 4 : depth == 1 ? 2 : 1;
        const int bx = (tp & 0xF) * 64, by = ((tp >> 4) & 1) * 256;
        const int rx = kv.second.u0 / div, ry = kv.second.v0, w = kv.second.u1 / div - rx + 1, h = kv.second.v1 - ry + 1;
        bool done = false;
        for (int py = 0; py < 2 && !done; py++)
            for (int px = 0; px < 16 && !done; px++) {
                const int dx = px * 64 + rx, dy = py * 256 + ry;
                if (!vram_free(used, dx, dy, w, h)) continue;
                for (int yy = 0; yy < h; yy++)
                    for (int xx = 0; xx < w; xx++)
                        if (by + ry + yy < 512 && bx + rx + xx < 1024)
                            dst.px[(size_t)(dy + yy) * 1024 + dx + xx] = src.px[(size_t)(by + ry + yy) * 1024 + bx + rx + xx];
                vram_mark(used, dx, dy, w, h);
                new_page[tp] = (uint16_t)((tp & ~0x1Fu) | (uint16_t)px | (uint16_t)(py << 4));
                done = true;
            }
    }
    for (auto& kv : cluts) {
        const int cx = clut_x(kv.first), cy = clut_y(kv.first), w = kv.second;
        bool done = false;
        for (int y = 511; y >= 0 && !done; y--)
            for (int x = 0; x + w <= 1024 && !done; x += 16) {
                if (!vram_free(used, x, y, w, 1)) continue;
                for (int xx = 0; xx < w && cx + xx < 1024; xx++) dst.px[(size_t)y * 1024 + x + xx] = src.px[(size_t)cy * 1024 + cx + xx];
                vram_mark(used, x, y, w, 1);
                new_clut[kv.first] = (uint16_t)((x / 16) | (y << 6));
                done = true;
            }
    }
    for (const APrim& p : m.prims) {
        auto pg = new_page.find(p.tpage);
        if (pg == new_page.end()) continue;
        uint16_t cl = p.clut;
        if (((p.tpage >> 7) & 3) < 2) {
            auto c = new_clut.find(p.clut);
            if (c == new_clut.end()) continue;
            cl = c->second;
        }
        remap[((uint32_t)p.tpage << 16) | p.clut] = {pg->second, cl};
    }
}

std::vector<MTri> App::room_view_tris(Loaded& L) {
    ensure_edit(L);
    // 06.10.2026: item models where the game puts them (pieces.cpp room_item_models), with the pieces of the script
    json all = L.ed.pieces;
    json items = room_item_models(L.c, L.ed.script, L.ed.items);
    for (auto& m : items) all.push_back(m);
    std::vector<MTri> out = room_tris_placed(L.c, all);
    // 06.10.2026 (player: "what I add is not seen in the view - make it seen"): the common item box (CORE) at each item that
    // has it, and every enemy of the room - the game's and the added ones - in its rest pose at its place
    if (const FileContent* core = side_content("CORE.DAT")) {
        std::vector<MTri> box;
        for (const MeshPart& mp : core->meshes)
            for (size_t k = 0; k < mp.hdrs.size(); k++)
                if (((mp.block_dest + mp.hdrs[k]) & 0x1FFFFFu) == 0x180000u)
                    for (const MTri& t : mp.tris) if (t.group == (uint16_t)k) box.push_back(t);
        for (auto& m : items) {
            if (!m.value("core_box", false)) continue;
            float R[9];
            piece_rot(m.value("rx", 0), m.value("ry", 0), m.value("rz", 0), R);
            const float px = (float)m.value("x", 0), py = (float)m.value("y", 0), pz = (float)m.value("z", 0);
            for (MTri t : box) {
                for (int k = 0; k < 3; k++) {
                    const float x = t.v[k].x, y = t.v[k].y, z = t.v[k].z;
                    t.v[k].x = (int16_t)lroundf(px + R[0] * x + R[1] * y + R[2] * z);
                    t.v[k].y = (int16_t)lroundf(py + R[3] * x + R[4] * y + R[5] * z);
                    t.v[k].z = (int16_t)lroundf(pz + R[6] * x + R[7] * y + R[8] * z);
                }
                t.group = 0xFFFF;
                out.push_back(t);
            }
        }
    }
    // 06.10.2026: kinds of other rooms - their pictures copied into free places of the view's video memory (foreign_view_tex)
    std::vector<uint8_t> used;
    std::map<std::pair<const FileContent*, uint32_t>, TexRemap> remaps;
    std::string fk_sig;
    auto view_used = [&]() -> std::vector<uint8_t>& {
        if (used.empty()) {
            used.assign((size_t)1024 * 512, 0);
            room_vram(L);                                         // the base: the room's and CORE's pictures
            for (const Upload& u : L.c.uploads) vram_mark(used, u.x, u.y, u.w, u.h);
            if (const FileContent* core = side_content("CORE.DAT"))
                for (const Upload& u : core->uploads) vram_mark(used, u.x, u.y, u.w, u.h);
        }
        return used;
    };
    for (const json& e : L.ed.enemies) {
        if (e.value("removed", false) || !e.count("x") || !e.count("z")) continue;
        uint32_t addr = 0;
        try { addr = (uint32_t)std::stoul(e.value("model", std::string("0")), nullptr, 16); } catch (...) { continue; }
        const FileContent* src = &L.c;
        bool flat = false;
        if (e.count("species") && e["species"].is_object()) {      // a kind from another room: its model there, no picture here
            const std::string room = e["species"].value("room", std::string());
            if (!room.empty() && room != L.c.file->stem) {
                src = side_content(room + ".DAT");
                try { addr = (uint32_t)std::stoul(e["species"].value("model", std::string("0")), nullptr, 16); } catch (...) { continue; }
                flat = true;
                // 06.10.2026: the build takes this room's model of the same body (species.cpp) - drawn with its pictures
                uint64_t k = 0;
                if (src)
                    for (const ActorPart& a : src->actors)
                        if (a.block >= 0 && a.block < (int)src->blocks.size() && ((src->blocks[a.block].dest + a.model.hdr) & 0x1FFFFFu) == (addr & 0x1FFFFFu))
                            k = actor_key_blk(src->blocks[a.block].data, src->blocks[a.block].dest, a.model.hdr);
                for (const ActorPart& a : L.c.actors)
                    if (k && a.block >= 0 && a.block < (int)L.c.blocks.size() &&
                        actor_key_blk(L.c.blocks[a.block].data, L.c.blocks[a.block].dest, a.model.hdr) == k) {
                        src = &L.c;
                        addr = L.c.blocks[a.block].dest + a.model.hdr;
                        flat = false;
                        break;
                    }
            }
        }
        if (!src) continue;
        for (const ActorPart& a : src->actors)
            if (((a.block_dest + a.model.hdr) & 0x1FFFFFu) == (addr & 0x1FFFFFu)) {
                const TexRemap* rm = nullptr;
                if (flat) {
                    auto key = std::make_pair(src, addr & 0x1FFFFFu);
                    auto it = remaps.find(key);
                    if (it == remaps.end()) {
                        it = remaps.emplace(key, TexRemap()).first;
                        foreign_view_tex(a.model, src->vram, L.view_vram, view_used(), it->second);
                        fk_sig += strfmt("%s:%06X;", src->file ? src->file->stem.c_str() : "?", addr & 0x1FFFFFu);
                    }
                    rm = &it->second;
                }
                actor_place(a.model, e.value("x", 0), e.value("y", 0), e.value("z", 0), e.value("rotation", 0), flat, out, rm);
                break;
            }
    }
    // the view keeps decoded pages by page and palette: when what stands at the copied places changed, they go
    if (fk_sig != view_fk_sig_) { view_fk_sig_ = fk_sig; v3d_.forget_pages(); }
    return out;
}

// ---------------------------------------------------------------- placed pieces (CREATE with its own place / turn)
void App::pieces_panel(Loaded& L) {
    RoomEdit& E = L.ed;
    ImGui::BeginDisabled(job_ != nullptr);
    if (ImGui::Button(E.dirty_pieces ? "Save to mods *" : "Save to mods")) {
        std::string err;
        if (!pieces_to_script(E.pieces, E.script, err)) E.msg = "Not saved: " + err;
        else { E.dirty_pieces = false; save_edit(L, 3); }
    }
    ImGui::SameLine();
    const bool have = fs::exists(room_mods_dir(*L.c.file) / "script.txt");
    ImGui::BeginDisabled(!have && !E.dirty_pieces);
    if (ImGui::Button("As in the game")) { revert_edit(L, 3); v3d_.set_mesh(room_view_tris(L), room_vram(L), true); placement_refresh(L, 2); }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!E.msg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", E.msg.c_str()); }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("Click: pick. Arrow: move along it. Square: move on the floor. Ring: turn.");
    ImGui::PopStyleColor();
    bool changed = false;
    ImGui::BeginChild("list");
    for (int i = 0; i < (int)E.pieces.size(); i++) {
        json& p = E.pieces[i];
        if (!p.value("placed", false) || p.value("lead", i) != i) continue;
        ImGui::PushID(i);
        std::string subs;
        for (auto& q : E.pieces)
            if (q.value("lead", -1) == i) subs += (subs.empty() ? "" : ", ") + std::to_string(q.value("sub", -1));
        std::string title = strfmt("Group %d  (sub %s; slot %d)", p.value("gi", -1), subs.c_str(), p.value("slot", 0));
        if (p.value("anim", 0)) title += "  scene animation";
        const bool is_sel = edit_file_ == L.c.file && edit_items_ == 2 && edit_sel_ == i;
        if (is_sel && edit_scroll_) ImGui::SetNextItemOpen(true);
        if (is_sel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.75f, 0.42f, 0.18f, 1));
        const bool open = ImGui::CollapsingHeader((title + "###h").c_str());
        if (ImGui::IsItemClicked()) { edit_file_ = L.c.file; edit_items_ = 2; edit_sel_ = i; placement_refresh(L, 2); }
        if (is_sel) { ImGui::PopStyleColor(); if (edit_scroll_) { ImGui::SetScrollHereY(0.2f); edit_scroll_ = false; } }
        if (open) {
            ImGui::Indent(8);
            float pos[3] = {p.value("x", 0) / 1000.0f, -p.value("y", 0) / 1000.0f, p.value("z", 0) / 1000.0f};
            ImGui::SetNextItemWidth(240);
            if (ImGui::InputFloat3("X, height, Z (m)", pos, "%.2f")) {
                p["x"] = std::clamp((int)lroundf(pos[0] * 1000.0f), -32768, 32767);
                p["y"] = std::clamp((int)lroundf(-pos[1] * 1000.0f), -32768, 32767);
                p["z"] = std::clamp((int)lroundf(pos[2] * 1000.0f), -32768, 32767);
                pieces_sync(E.pieces, i);
                changed = true;
            }
            auto deg = [](int v) { float d = fmodf(v * 360.0f / 4096.0f, 360.0f); return d < 0 ? d + 360.0f : d; };
            float rot[3] = {deg(p.value("rx", 0)), deg(p.value("ry", 0)), deg(p.value("rz", 0))};
            ImGui::SetNextItemWidth(240);
            if (ImGui::InputFloat3("turn X, Y, Z (deg)", rot, "%.1f")) {
                p["rx"] = (int)lroundf(rot[0] * 4096.0f / 360.0f) & 4095;
                p["ry"] = (int)lroundf(rot[1] * 4096.0f / 360.0f) & 4095;
                p["rz"] = (int)lroundf(rot[2] * 4096.0f / 360.0f) & 4095;
                pieces_sync(E.pieces, i);
                changed = true;
            }
            ImGui::TextDisabled("command @%04X, mesh group %s", p.value("at", 0), p.value("group", std::string()).c_str());
            ImGui::Unindent(8);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (changed) {
        E.dirty_pieces = true;
        v3d_.set_mesh(room_view_tris(L), room_vram(L), true);
        placement_refresh(L, 2);
    }
}

// ---------------------------------------------------------------- правка мышью во вьюпорте
// новая запись по образцу src (копия команды игры: свободный слот / зона / флаг «взят» — converter, placement.cpp) в точке x, z
static bool add_copy(json& arr, int src, bool items, float x, float z) {
    if (src < 0 || src >= (int)arr.size()) return false;
    json c = arr[src];
    const int from = c.count("copy_of") ? c["copy_of"].get<int>() : c.value("offset", -1);
    if (from < 0) return false;
    for (const char* k : {"offset", "removed", "subroutine", "slot", "zone_index", "taken_flag", "model_section"}) c.erase(k);
    c["copy_of"] = from;
    if (!items) enemy_as_actor(c);
    float px, py, pz;
    if (!placement_point(c, items, px, py, pz)) return false;
    const int dx = (int)lroundf(x - px), dz = (int)lroundf(z - pz);
    if (items) for (auto& v : c["zone"]) { v[0] = v[0].get<int>() + dx; v[1] = v[1].get<int>() + dz; }
    else { c["x"] = c["x"].get<int>() + dx; c["z"] = c["z"].get<int>() + dz; }
    arr.push_back(c);
    return true;
}

// место записи → (x, y, z) с привязкой к сетке 10 мм; у предмета высота — этаж (по 425 мм), зона сдвигается целиком
static bool move_entry(json& e, bool items, float x, float y, float z) {
    float ox, oy, oz;
    if (!placement_point(e, items, ox, oy, oz)) return false;
    const int dx = (int)lroundf(x / 10.0f) * 10 - (int)lroundf(ox), dz = (int)lroundf(z / 10.0f) * 10 - (int)lroundf(oz);
    bool ch = false;
    if (items) {
        if (dx || dz) { for (auto& v : e["zone"]) { v[0] = v[0].get<int>() + dx; v[1] = v[1].get<int>() + dz; } ch = true; }
        const int fl = std::clamp((int)lroundf(-y / 425.0f), 0, 7);
        if (e.count("floor") && fl != e.value("floor", 0)) { e["floor"] = fl; ch = true; }
        return ch;
    }
    if (dx || dz) {
        e["x"] = std::clamp(e["x"].get<int>() + dx, -32768, 32767);
        e["z"] = std::clamp(e["z"].get<int>() + dz, -32768, 32767);
        ch = true;
    }
    const int ny = std::clamp((int)lroundf(y / 10.0f) * 10, -32768, 32767);
    if (e.count("y") && ny != e.value("y", 0)) { e["y"] = ny; ch = true; }
    return ch;
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / l2 : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = ax + vx * t - px, dy = ay + vy * t - py;
    return sqrtf(dx * dx + dy * dy);
}

// 06.10.2026 (игрок: «в самом вьюпорте перетаскивать предметы и спавнеры врагов, трансформить, добавлять»; «где оси координат
// у трансформа предмета»): у выбранной записи — манипулятор: оси X (красная), вверх (зелёная), Z (синяя) — тянуть вдоль оси;
// квадрат в центре или сама метка — по полу; кольцо (враги) — поворот, Shift — шагами по 11.25°. Щелчок по метке — выбрать;
// Ctrl+колесо или Q/E — поворот; Delete — убрать; правая кнопка по полу — добавить сюда.
bool App::viewport_edit(bool hovered, float img_x, float img_y) {
    Loaded* L = load(sel_.f);
    if (!L) return false;
    ensure_edit(*L);
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();   // картинка вида (последний элемент)
    const int what = sel_.k == K_ITEMS ? 1 : sel_.k == K_PIECES ? 2 : 0;
    const bool items = what == 1;
    RoomEdit& E = L->ed;
    json& arr = what == 1 ? E.items : what == 2 ? E.pieces : E.enemies;
    const char* rk = what == 2 ? "ry" : "rotation";            // turn about the vertical axis (4096 = 360)
    if (edit_file_ != L->c.file || edit_items_ != what) {   // another room or list - the pick starts over
        edit_file_ = L->c.file; edit_items_ = what; edit_sel_ = edit_drag_ = -1; gizmo_ = 0;
    }
    if (edit_sel_ >= (int)arr.size()) { edit_sel_ = -1; gizmo_ = 0; }
    ImGuiIO& io = ImGui::GetIO();
    const float mx = io.MousePos.x - img_x, my = io.MousePos.y - img_y;
    bool changed = false, took = false;
    // проверка без человека (--edit-probe): на 6-м кадре метка 0 → экран → пол (ошибка в мм), затем перетаскивание той же
    // формулой, что у мыши, в экранную точку метки +1000 мм по x; отчёт — <снимок>.edit.txt
    if (edit_probe_) {
        static int frame = 0;
        if (++frame == 6) {
            std::string rep;
            for (int i = 0; i < (int)arr.size(); i++) {
                float x, y, z, sx, sy, gx, gz, tx, ty;
                if (arr[i].value("removed", false) || (what == 2 && (!arr[i].value("placed", false) || arr[i].value("lead", i) != i)) ||
                    !placement_point(arr[i], items, x, y, z)) continue;
                if (!v3d_.project(x, y, z, sx, sy) || !v3d_.ground(sx, sy, y, gx, gz)) { rep += strfmt("entry %d: not on screen\n", i); continue; }
                rep += strfmt("entry %d at %.0f %.0f %.0f -> screen %.1f %.1f -> floor %.1f %.1f (error %.2f mm)\n", i, x, y, z, sx, sy, gx, gz,
                              sqrtf((gx - x) * (gx - x) + (gz - z) * (gz - z)));
                if (v3d_.project(x + 1000, y, z, tx, ty) && v3d_.ground(tx, ty, y, gx, gz)) {
                    const int dx = (int)lroundf(gx / 10.0f) * 10 - (int)lroundf(x), dz = (int)lroundf(gz / 10.0f) * 10 - (int)lroundf(z);
                    if (items) for (auto& v : arr[i]["zone"]) { v[0] = v[0].get<int>() + dx; v[1] = v[1].get<int>() + dz; }
                    else { arr[i]["x"] = arr[i]["x"].get<int>() + dx; arr[i]["z"] = arr[i]["z"].get<int>() + dz; }
                    float nx, ny, nz;
                    placement_point(arr[i], items, nx, ny, nz);
                    rep += strfmt("dragged to screen %.1f %.1f -> now at %.0f %.0f %.0f (moved %d %d)\n", tx, ty, nx, ny, nz, dx, dz);
                    edit_sel_ = i; edit_scroll_ = true; changed = true;
                }
                break;
            }
            write_text(u8path(shot_path + ".edit.txt"), rep);
        }
    }
    // ---- манипулятор выбранной: точка, оси на экране (длина оси ~95 пикселей), кольцо поворота
    float ox = 0, oy = 0, oz = 0, so[2] = {0, 0}, tip[3][2] = {}, len_mm = 1000;
    bool gz_ok = false;
    std::vector<ImVec2> ring;
    // 06.10.2026: items turn too - about the vertical axis here (the panel has all three); it starts from what the game shows
    const bool can_turn = edit_sel_ >= 0 && (items || arr[edit_sel_].count(rk));
    auto cur_rot = [&]() {
        if (!items) return arr[edit_sel_][rk].get<int>() & 4095;
        int r[3];
        item_rot(*L, arr, arr[edit_sel_], r);
        return r[1];
    };
    auto set_rot = [&](json& e, int v) {
        if (!items) { e[rk] = v & 4095; return; }
        int r[3];
        item_rot(*L, arr, e, r);
        r[1] = v & 4095;
        item_rot_set(e, r);
    };
    auto gizmo_geometry = [&]() {
        gz_ok = false;
        ring.clear();
        if (edit_sel_ < 0 || arr[edit_sel_].value("removed", false) || (what == 2 && !arr[edit_sel_].value("placed", false)) ||
            !placement_point(arr[edit_sel_], items, ox, oy, oz)) return;
        float tx, ty;
        if (!v3d_.project(ox, oy, oz, so[0], so[1]) || !v3d_.project(ox + 1000, oy, oz, tx, ty)) return;
        const float px = std::max(4.0f, sqrtf((tx - so[0]) * (tx - so[0]) + (ty - so[1]) * (ty - so[1])));
        len_mm = std::clamp(1000.0f * 95.0f / px, 50.0f, 50000.0f);
        const float ax[3][3] = {{len_mm, 0, 0}, {0, -len_mm, 0}, {0, 0, len_mm}};
        for (int k = 0; k < 3; k++)
            if (!v3d_.project(ox + ax[k][0], oy + ax[k][1], oz + ax[k][2], tip[k][0], tip[k][1])) return;
        if (can_turn)
            for (int k = 0; k <= 48; k++) {
                const float a = k * 6.2831853f / 48, rr = 0.8f * len_mm;
                float rx, ry;
                if (v3d_.project(ox + rr * sinf(a), oy, oz + rr * cosf(a), rx, ry)) ring.push_back(ImVec2(rx, ry));
            }
        gz_ok = true;
    };
    gizmo_geometry();
    // что под мышью: 1..3 ось, 4 квадрат, 5 кольцо
    int hot = 0;
    if (gz_ok && hovered && !gizmo_) {
        if (fabsf(mx - so[0]) <= 8 && fabsf(my - so[1]) <= 8) hot = 4;
        else {
            float best = 8.0f;
            for (int k = 0; k < 3; k++) {
                const float d = seg_dist(mx, my, so[0], so[1], tip[k][0], tip[k][1]);
                if (d < best) { best = d; hot = k + 1; }
            }
            if (!hot && can_turn)
                for (size_t k = 0; k + 1 < ring.size(); k++)
                    if (seg_dist(mx, my, ring[k].x, ring[k].y, ring[k + 1].x, ring[k + 1].y) < 7) { hot = 5; break; }
        }
    }
    auto begin_drag = [&](int what) {
        gizmo_ = what;
        gz_mouse_[0] = mx; gz_mouse_[1] = my;
        gz_start_[0] = ox; gz_start_[1] = oy; gz_start_[2] = oz;
        gz_start_rot_ = can_turn ? cur_rot() : 0;
        float gx, gzz;
        drag_off_[0] = drag_off_[1] = 0;
        if (v3d_.ground(mx, my, oy, gx, gzz)) {
            drag_off_[0] = ox - gx; drag_off_[1] = oz - gzz;
            gz_start_ang_ = atan2f(gx - ox, gzz - oz);
        }
    };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hot) { begin_drag(hot); took = true; }
        else {
            // ближайшая метка под мышью (до 18 пикселей): ноги, середина и верх стойки — выбрать и тянуть по полу
            int best = -1;
            float bd = 18.0f * 18.0f;
            for (int i = 0; i < (int)arr.size(); i++) {
                const json& e = arr[i];
                if (e.value("removed", false) || (what == 2 && (!e.value("placed", false) || e.value("lead", i) != i))) continue;
                float x, y, z;
                if (!placement_point(e, items, x, y, z)) continue;
                for (float up : {0.0f, items ? 300.0f : 900.0f, items ? 600.0f : what == 2 ? 900.0f : 1800.0f}) {
                    float sx, sy;
                    if (!v3d_.project(x, y - up, z, sx, sy)) continue;
                    const float d = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
                    if (d < bd) { bd = d; best = i; }
                }
            }
            if (best >= 0) {
                edit_sel_ = best;
                edit_scroll_ = true;
                gizmo_geometry();
                begin_drag(4);
                placement_refresh(*L, what);
                took = true;
            }
        }
    }
    if (gizmo_ && edit_sel_ >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            took = true;
            json& e = arr[edit_sel_];
            float nx = ox, ny = oy, nz = oz;
            if (gizmo_ >= 1 && gizmo_ <= 3 && gz_ok) {
                const int k = gizmo_ - 1;
                const float ux = tip[k][0] - so[0], uy = tip[k][1] - so[1], l2 = ux * ux + uy * uy;
                if (l2 > 4) {
                    const float d = ((mx - gz_mouse_[0]) * ux + (my - gz_mouse_[1]) * uy) / l2 * len_mm;   // мм вдоль оси
                    nx = gz_start_[0] + (k == 0 ? d : 0);
                    ny = gz_start_[1] - (k == 1 ? d : 0);
                    nz = gz_start_[2] + (k == 2 ? d : 0);
                }
            } else if (gizmo_ == 4) {
                float gx, gzz;
                if (v3d_.ground(mx, my, gz_start_[1], gx, gzz)) { nx = gx + drag_off_[0]; nz = gzz + drag_off_[1]; }
            } else if (gizmo_ == 5 && can_turn) {
                float gx, gzz;
                if (v3d_.ground(mx, my, gz_start_[1], gx, gzz)) {
                    const float a = atan2f(gx - gz_start_[0], gzz - gz_start_[2]);
                    int r = (gz_start_rot_ + (int)lroundf((a - gz_start_ang_) * 4096.0f / 6.2831853f)) & 4095;
                    if (io.KeyShift) r = ((r + 64) / 128 * 128) & 4095;
                    if (r != cur_rot()) { set_rot(e, r); changed = true; }
                }
            }
            if (gizmo_ <= 4 && move_entry(e, items, nx, ny, nz)) changed = true;
        } else gizmo_ = 0;
    }
    // поворот врага и предмета: Ctrl+колесо (1/64 круга) или Q/E (1/16)
    if (can_turn) {
        int step = 0;
        if (hovered && io.KeyCtrl && io.MouseWheel != 0) step = io.MouseWheel > 0 ? 64 : -64;
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_Q)) step = -256;
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_E)) step = 256;
        if (step) { set_rot(arr[edit_sel_], cur_rot() + step); changed = true; }
    }
    // Delete — убрать (новую — удалить совсем)
    if (hovered && what != 2 && edit_sel_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (arr[edit_sel_].count("copy_of")) arr.erase(arr.begin() + edit_sel_);
        else arr[edit_sel_]["removed"] = true;
        edit_sel_ = edit_drag_ = -1;
        gizmo_ = 0;
        changed = true;
    }
    // правая кнопка без перетаскивания — меню «добавить сюда»
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x +
                                                                         ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y < 16) {
        float y = 0;
        if (edit_sel_ >= 0) { float x, z; placement_point(arr[edit_sel_], items, x, y, z); }
        float gx, gz;
        if (v3d_.ground(mx, my, y, gx, gz)) { add_at_[0] = gx; add_at_[1] = y; add_at_[2] = gz; ImGui::OpenPopup("addhere"); }
    }
    if (ImGui::BeginPopup("addhere")) {
        // образцы: выбранная запись, затем по одной записи каждого вида (у врагов — вид и вариант, у предметов — предмет)
        std::vector<int> tpl;
        std::vector<std::string> seen;
        auto label = [&](const json& e) {
            if (items) { const int id = e.value("item", 0); return id == 0xFF ? std::string(T("random", "случайный")) : item_name(id); }
            const std::string kn = e.value("kind_name", std::string());
            const std::string who = person_of(lib_, L->c, e);
            if (!who.empty()) return who;
            return (kn.empty() ? strfmt("kind %d", e.value("kind", 0)) : actor_label(kn)) + (e.value("variant", 0) ? strfmt(" %d", e.value("variant", 0)) : std::string());
        };
        if (edit_sel_ >= 0) { tpl.push_back(edit_sel_); seen.push_back(label(arr[edit_sel_])); }
        for (int i = 0; i < (int)arr.size(); i++) {
            float x, y, z;
            // 06.10.2026: an enemy without a place of its own (roster) is a model to copy too - the copy gets the click's place
            if (!placement_point(arr[i], items, x, y, z) && (items || !arr[i].count("model") || arr[i].value("removed", false))) continue;
            const std::string l = label(arr[i]);
            if (std::find(seen.begin(), seen.end(), l) != seen.end()) continue;
            seen.push_back(l);
            tpl.push_back(i);
        }
        if (tpl.empty()) ImGui::TextDisabled("%s", items ? T("No item in this room to copy", "В комнате нет предмета-образца")
                                                         : T("No enemy in this room to copy", "В комнате нет врага-образца"));
        for (size_t k = 0; k < tpl.size(); k++)
            if (ImGui::MenuItem(strfmt(g_lang == LANG_RU ? "Добавить сюда: %s" : "Add here: %s", seen[k].c_str()).c_str())) {
                if (add_copy(arr, tpl[k], items, add_at_[0], add_at_[2])) {
                    make_new(arr.back(), items);
                    edit_sel_ = (int)arr.size() - 1; edit_scroll_ = true; changed = true;
                }
            }
        ImGui::EndPopup();
    }
    if (changed) {
        (what == 2 ? E.dirty_pieces : items ? E.dirty_items : E.dirty_enemies) = true;
        if (what == 2) {
            pieces_sync(E.pieces, edit_sel_);
            v3d_.set_mesh(room_view_tris(*L), room_vram(*L), true);   // the piece moves with its geometry
        }
        placement_refresh(*L, what);
        gizmo_geometry();
    }
    // ---- рисунок манипулятора поверх вида
    if (gz_ok) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(r0, r1, true);
        const ImVec2 o(img_x + so[0], img_y + so[1]);
        const ImU32 col[3] = {IM_COL32(235, 70, 70, 255), IM_COL32(95, 215, 95, 255), IM_COL32(80, 140, 255, 255)};
        const ImU32 hotc = IM_COL32(255, 220, 70, 255);
        const int act = gizmo_ ? gizmo_ : hot;
        if (can_turn && ring.size() > 1) {
            std::vector<ImVec2> pts;
            for (auto& p : ring) pts.push_back(ImVec2(img_x + p.x, img_y + p.y));
            dl->AddPolyline(pts.data(), (int)pts.size(), act == 5 ? hotc : IM_COL32(225, 225, 225, 210), 0, act == 5 ? 3.0f : 2.0f);
        }
        static const char* names[3] = {"X", "Y", "Z"};
        for (int k = 0; k < 3; k++) {
            const ImVec2 t(img_x + tip[k][0], img_y + tip[k][1]);
            const ImU32 c = act == k + 1 ? hotc : col[k];
            dl->AddLine(o, t, c, act == k + 1 ? 4.0f : 3.0f);
            float dx = t.x - o.x, dy = t.y - o.y;
            const float l = sqrtf(dx * dx + dy * dy);
            if (l > 1) {
                dx /= l; dy /= l;
                const ImVec2 b(t.x - dx * 12, t.y - dy * 12);
                dl->AddTriangleFilled(ImVec2(t.x + dx * 4, t.y + dy * 4), ImVec2(b.x - dy * 6, b.y + dx * 6), ImVec2(b.x + dy * 6, b.y - dx * 6), c);
                dl->AddText(ImVec2(t.x + dx * 10 - 4, t.y + dy * 10 - 7), c, names[k]);
            }
        }
        dl->AddRectFilled(ImVec2(o.x - 6, o.y - 6), ImVec2(o.x + 6, o.y + 6), act == 4 ? hotc : IM_COL32(240, 240, 240, 235));
        dl->PopClipRect();
    }
    return took;
}

}  // namespace dc
