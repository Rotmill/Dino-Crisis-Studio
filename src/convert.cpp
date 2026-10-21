#include "convert.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>

#include "gltf.h"
#include "meshsig.h"
#include "motions.h"
#include "placement.h"
#include "script.h"
#include "script_edit.h"
#include "species.h"
#include "export.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBI_ONLY_PNG
#include "../deps/stb/stb_image.h"

namespace dc {

static uint32_t align2k(uint32_t v) { return (v + 2047u) & ~2047u; }

// Блок с новым содержимым (plain — распакованный вид) на место записи entry. ⭐ Каждый блок занимает СТОЛЬКО ЖЕ секторов,
// сколько на диске: загрузчик игры другую раскладку не принимает (05.10.2026, титул: блок на 2 сектора короче — в RAM нули,
// экран чёрный; в прежних секторах — цел). LZSS короче — добиваем литералами (lzss_encode_min), длиннее — «не влезает».
bool rebuild_container(const Bytes& orig, const std::vector<DatEntry>& ents, int entry, const Bytes& plain, uint32_t limit,
                       Bytes& out, std::string& err, bool grow_last) {
    const bool may_grow = grow_last && entry == (int)ents.size() - 1;
    std::vector<Bytes> data(ents.size());
    for (size_t i = 0; i < ents.size(); i++) {
        if ((int)i != entry) data[i].assign(orig.begin() + ents[i].offset, orig.begin() + ents[i].offset + ents[i].size);
    }
    const uint32_t sectors = (ents[entry].size + 2047u) / 2048u;
    if (ents[entry].type == 7 || ents[entry].type == 8) {
        Bytes enc = lzss_encode(plain.data(), plain.size());
        const bool grown = enc.size() > sectors * 2048u;
        if (grown && !may_grow) {
            err = strfmt("the packed block is %u bytes, the game has room for %u (%u sectors)", (unsigned)enc.size(), sectors * 2048u, sectors);
            return false;
        }
        if (!grown && (enc.size() + 2047u) / 2048u < sectors) enc = lzss_encode_min(plain.data(), plain.size(), (sectors - 1) * 2048u + 1);
        Bytes check;
        if (!lzss_decode(enc.data(), enc.size(), check) || check != plain || (!grown && (enc.size() + 2047u) / 2048u != sectors)) {
            err = "internal error: packing check failed";
            return false;
        }
        data[entry] = std::move(enc);
    } else {
        if ((plain.size() + 2047u) / 2048u != sectors) {
            err = strfmt("the block is %u bytes, it must stay within %u sectors", (unsigned)plain.size(), sectors);
            return false;
        }
        data[entry] = plain;
    }
    uint32_t t = 0x800;
    for (auto& d : data) t = align2k(t + (uint32_t)d.size());
    if (t > limit && !may_grow) {
        err = strfmt("the file would be %u bytes, the game has room for %u", t, limit);
        return false;
    }
    out.assign(orig.begin(), orig.begin() + std::min<size_t>(0x800, orig.size()));
    out.resize(0x800, 0);
    for (size_t i = 0; i < ents.size(); i++) put32(&out[i * 16 + 4], (uint32_t)data[i].size());
    for (auto& d : data) {
        out.insert(out.end(), d.begin(), d.end());
        out.resize(align2k((uint32_t)out.size()), 0);
    }
    return true;
}

// 06.10.2026 (player: "can't our mod add memory from the PC side... so a hundred enemies can be added"): the block of the
// memory for mods - a container entry of its own after the last one (type 7: LZSS into RAM at 0x81000000), its data at the
// end of the file. The game's loader takes the header entries until one is not (the header sector is padded with "dummy
// header" text) and unpacks a type 7 block to its address with plain stores - Dino Crisis ME maps that memory (memory.c).
// A block of ours there already (a file built before) is replaced.
// 06.10.2026: blocks of ours at the end of a room file - LZSS (type 7) to the memory for mods (0x81000000..: enemy kinds of
// other rooms, overlays of other stages) and to the stage overlay's kind table (0x800E8000..0x80100000: 4-byte entries)
static bool ours_entry(const DatEntry& e) {
    return e.type == 7 && (e.dest >= kModRamBase || (e.dest >= kOverlayBase && e.dest < 0x80100000u));
}
static bool container_put_ours(Bytes& file, const std::vector<std::pair<uint32_t, Bytes>>& blocks, std::string& err) {
    auto ents = parse_container(file);
    if (ents.empty()) { err = "the room file has no blocks"; return false; }
    size_t n = ents.size();
    while (n > 0 && ours_entry(ents[n - 1])) n--;
    if (n == 0) { err = "the room file has only blocks of ours"; return false; }
    const uint32_t end = n < ents.size() ? ents[n].offset : align2k(ents.back().offset + ents.back().size);
    if ((n + blocks.size() + 1) * 16 > 0x800) { err = "the room file has no free entries in its header"; return false; }
    file.resize(end, 0);
    for (size_t k = 0; k < blocks.size(); k++) {
        const Bytes& plain = blocks[k].second;
        Bytes enc = lzss_encode(plain.data(), plain.size());
        Bytes check;
        if (!lzss_decode(enc.data(), enc.size(), check) || check != plain) { err = "internal error: packing check failed"; return false; }
        const size_t h = (n + k) * 16;
        put32(&file[h], 7);
        put32(&file[h + 4], (uint32_t)enc.size());
        put32(&file[h + 8], blocks[k].first);
        put32(&file[h + 12], 0);
        file.insert(file.end(), enc.begin(), enc.end());
        file.resize(align2k((uint32_t)file.size()), 0);
    }
    // the header ends where an entry does not parse: the game's own filler text after the last of ours
    static const char pad[17] = "dummy header    ";
    for (size_t h = (n + blocks.size()) * 16; h < ents.size() * 16 + 16 && h + 16 <= 0x800; h += 16) memcpy(&file[h], pad, 16);
    return true;
}

static bool load_json(const fs::path& p, json& j, std::string& err) {
    Bytes b;
    if (!read_file(p, b)) { err = "cannot read"; return false; }
    try {
        j = json::parse(b.begin(), b.end());
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

static bool from_hex(const std::string& s, Bytes& out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) out.push_back((uint8_t)strtoul(s.substr(i, 2).c_str(), nullptr, 16));
    return true;
}

static std::string uri_decode(const std::string& s) {
    std::string u;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { u += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else u += s[i];
    }
    return u;
}

// путь от папки base через «/» (так его пишут списки index.txt)
static std::string rel_to(const fs::path& p, const fs::path& base) {
    std::error_code ec;
    std::string r = path_u8(fs::relative(p, base, ec));
    if (ec || r.empty()) r = path_u8(p);
    for (auto& ch : r) if (ch == '\\') ch = '/';
    return r;
}

// секция в блоке: на месте, если влезает; иначе — в конец блока, указатель ptr[slot] переводится на новое место
static void place_section(Bytes& blk, uint32_t base, int slot, uint32_t old_off, uint32_t old_size, const Bytes& sec,
                          std::vector<std::string>& notes, const char* what) {
    if (sec.size() <= old_size) {
        memcpy(&blk[old_off], sec.data(), sec.size());
        if (old_size > sec.size()) memset(&blk[old_off + sec.size()], 0, old_size - sec.size());
        return;
    }
    while (blk.size() % 4) blk.push_back(0);
    uint32_t off = (uint32_t)blk.size();
    blk.insert(blk.end(), sec.begin(), sec.end());
    put32(&blk[slot * 4], base + off);
    notes.push_back(strfmt("%s grew %u -> %u bytes: moved to the end of the room block", what, old_size, (unsigned)sec.size()));
    section_old_place_clear(blk, base, old_off, old_size, notes, what);
}

// ---------------------------------------------------------------- свой прежний вывод
// Метка собранного файла — mods\<папка>\.dcme\<ИМЯ>.json (sha1 вывода): родной файл игрока в mods (без метки или с чужим
// sha1) — основа сборки, наш прежний — пересобирается с диска. Прежнее место метки (до 06.10) — <папка правок>\.dcme_built.json.
static fs::path built_marker(const fs::path& out) { return out.parent_path() / ".dcme" / u8path(path_u8(out.filename()) + ".json"); }

static bool marker_ours(const fs::path& out, const fs::path& old_marker) {
    if (!fs::exists(out)) return false;
    Bytes cur;
    read_file(out, cur);
    const std::string h = sha1_hex(cur.data(), cur.size());
    json mk;
    std::string e;
    const fs::path m = built_marker(out);
    if (fs::exists(m) && load_json(m, mk, e) && mk.value("output_sha1", "") == h) return true;
    return !old_marker.empty() && fs::exists(old_marker) && load_json(old_marker, mk, e) && mk.value("output_sha1", "") == h;
}

static void write_marker(const fs::path& out, const json& mk, const fs::path& old_marker) {
    std::error_code ec;
    const fs::path m = built_marker(out);
    fs::create_directories(m.parent_path(), ec);
    write_text(m, mk.dump(1));
    if (!old_marker.empty()) fs::remove(old_marker, ec);
}

static void drop_output(const fs::path& out, const fs::path& old_marker) {
    std::error_code ec;
    fs::remove(out, ec);
    fs::remove(built_marker(out), ec);
    if (!old_marker.empty()) fs::remove(old_marker, ec);
}

// основа сборки: свой родной файл игрока в mods (если он не наш прежний вывод), иначе файл с диска
static bool load_base(Disc& d, const DiscFile& f, const fs::path& out, const fs::path& old_marker, Bytes& orig, bool& base_mods,
                      ConvertReport& rep) {
    orig.clear();
    base_mods = false;
    if (fs::exists(out) && !marker_ours(out, old_marker)) {
        read_file(out, orig);
        base_mods = true;
    }
    if (orig.empty() && !d.read_file(f, orig)) { rep.errors++; rep.lines.push_back(f.name + ": cannot read the disc file"); return false; }
    return true;
}

// ---------------------------------------------------------------- движения: задания из glTF каталога
static void apply_jobs(const FileContent& fc, const std::vector<MotionJob>* jobs, std::map<int, Bytes>& mb, MotionResult& mr,
                       bool allow_relocate) {
    if (!jobs) return;
    for (const MotionJob& j : *jobs)
        apply_motion(*j.doc, j.doc_name, *j.actor, *j.anim, fc, j.set_index, j.motion, mb, mr, allow_relocate);
}

static void motion_report(const DiscFile& f, const MotionResult& mr, ConvertReport& rep, std::vector<std::string>& notes) {
    for (auto& e : mr.errors) { rep.errors++; rep.lines.push_back(f.name + ": " + e); }
    if (mr.changed) notes.push_back(strfmt("motions changed: %d", mr.changed));
    for (auto& n : mr.notes) notes.push_back("  " + n);
}

static bool convert_room_try(Disc& d, const DiscFile& f, const fs::path& pd, const fs::path& out, ConvertReport& rep, bool allow_relocate,
                             bool& relocated, const std::vector<MotionJob>* jobs, PlacementCtx& pctx);

// комната: сначала с перекладкой удлинённых движений в конец блока; не влезло в сектора — те же кадры на прежнем месте
bool convert_room(Disc& d, const DiscFile& f, const fs::path& pd, const fs::path& out, ConvertReport& rep, const std::vector<MotionJob>* jobs,
                  PlacementCtx* pctx) {
    PlacementCtx own;
    PlacementCtx& pc = pctx ? *pctx : own;
    auto merge = [&](const ConvertReport& r) {
        rep.built += r.built; rep.unchanged += r.unchanged; rep.errors += r.errors;
        rep.lines.insert(rep.lines.end(), r.lines.begin(), r.lines.end());
    };
    ConvertReport r1;
    bool reloc = false;
    if (convert_room_try(d, f, pd, out, r1, true, reloc, jobs, pc)) { merge(r1); return true; }
    if (!reloc) { merge(r1); return false; }
    ConvertReport r2;
    bool dummy = false;
    bool ok = convert_room_try(d, f, pd, out, r2, false, dummy, jobs, pc);
    merge(r2);
    return ok;
}

static bool convert_room_try(Disc& d, const DiscFile& f, const fs::path& pd, const fs::path& out, ConvertReport& rep, bool allow_relocate,
                             bool& relocated, const std::vector<MotionJob>* jobs, PlacementCtx& pctx) {
    relocated = false;
    const char* files[] = {"collision.json", "cameras.json", "footsteps.json", "triggers.json", "texts.json", "script.json",
                           "enemies.json", "items.json", "doors.json", "zones.json", "script.txt"};
    bool any = jobs && !jobs->empty();
    auto has = [&](const char* n) { return !pd.empty() && fs::exists(pd / n); };
    for (auto* n : files) if (has(n)) any = true;
    const fs::path old_marker = pd.empty() ? fs::path() : pd / ".dcme_built.json";
    if (!any) {   // правок файла нет (папка — только меш и картинки): прежний свой вывод убрать
        if (marker_ours(out, old_marker)) drop_output(out, old_marker);
        return true;
    }
    Bytes orig;
    bool base_mods = false;
    if (!load_base(d, f, out, old_marker, orig, base_mods, rep)) return false;
    FileContent fc;
    if (!analyze_raw(d, f, orig, fc) || !fc.is_room) { rep.errors++; rep.lines.push_back(f.name + ": not a room file"); return false; }
    auto ents = fc.ents;
    const int entry = fc.blocks[fc.room_block].entry;
    const uint32_t base = 0x80100000;
    std::vector<std::string> notes;
    std::string err;
    int applied = 0;
    // ---- движения (на своих местах в блоке — до правок секций); блоки вне блока комнаты — тоже в файл
    Bytes blk = fc.blocks[fc.room_block].data;
    std::map<int, Bytes> mb;
    {
        MotionResult mr;
        apply_jobs(fc, jobs, mb, mr, allow_relocate);
        motion_report(f, mr, rep, notes);
        if (!mr.errors.empty()) return false;
        if (mb.count(fc.room_block)) blk = mb[fc.room_block];
        if (mr.changed) applied++;
        relocated = mr.relocated > 0;
    }
    RoomInfo r;
    if (!parse_room(blk, base, r)) { rep.errors++; rep.lines.push_back(f.name + ": not a room block"); return false; }
    // ---- коллизия
    if (has("collision.json") && r.coll.ok) {
        json j;
        if (!load_json(pd / "collision.json", j, err)) { rep.errors++; rep.lines.push_back(f.name + ": collision.json: " + err); return false; }
        Collision c = r.coll;
        try {
            auto& rows = j.at("rows");
            if (rows.size() != 17) throw std::runtime_error("need 17 rows");
            for (int i = 0; i < 17; i++) {
                auto& row = rows[i];
                c.rows[i].pad = row.value("pad", 0);
                c.rows[i].spare = row.value("spare", 0u);
                c.floors[i].clear();
                c.walls[i].clear();
                for (auto& x : row.at("floors")) {
                    FloorRec fl{};
                    fl.type = x.value("type", 0); fl.flags = x.value("flags", 0); fl.pad = x.value("pad", 0);
                    fl.x = x.at("x"); fl.z = x.at("z"); fl.sx = x.at("sx"); fl.sz = x.at("sz");
                    for (int k = 0; k < 4; k++) fl.h[k] = x.at("h")[k];
                    c.floors[i].push_back(fl);
                }
                for (auto& x : row.at("walls")) {
                    WallRec w{};
                    w.type = x.value("type", 0); w.flag = x.value("flag", 0); w.lvl_lo = x.value("level_from", 0); w.lvl_hi = x.value("level_to", 0);
                    w.x = x.at("x"); w.z = x.at("z"); w.sx = x.at("sx"); w.sz = x.at("sz");
                    w.attr = x.value("attr", 0); w.spare = x.value("spare", 0);
                    c.walls[i].push_back(w);
                }
                if (c.floors[i].size() > 255 || c.walls[i].size() > 255) throw std::runtime_error("more than 255 records in a row");
            }
        } catch (const std::exception& e) {
            rep.errors++;
            rep.lines.push_back(f.name + ": collision.json: " + e.what());
            return false;
        }
        Bytes sec = build_collision(c);
        place_section(blk, base, 2, r.coll.off, r.coll.size, sec, notes, "collision");
        applied++;
    }
    // ---- камеры
    if (has("cameras.json") && r.cam_off) {
        json j;
        if (!load_json(pd / "cameras.json", j, err)) { rep.errors++; rep.lines.push_back(f.name + ": cameras.json: " + err); return false; }
        Bytes sec(4, 0);
        try {
            auto& cams = j.at("cameras");
            put32(&sec[0], (uint32_t)cams.size());
            for (auto& x : cams) {
                Bytes raw;
                if (!from_hex(x.value("raw", std::string()), raw) || raw.size() != 68) raw.assign(68, 0);
                raw[0] = (uint8_t)x.value("type", (int)raw[0]); raw[1] = (uint8_t)x.value("mask", (int)raw[1]);
                raw[2] = (uint8_t)x.value("camera", (int)raw[2]); raw[3] = (uint8_t)x.value("level", (int)raw[3]);
                if (x.count("zone")) for (int k = 0; k < 4; k++) { put16(&raw[4 + k * 4], (uint16_t)(int)x["zone"][k][0]); put16(&raw[6 + k * 4], (uint16_t)(int)x["zone"][k][1]); }
                if (x.count("zone2")) for (int k = 0; k < 4; k++) { put16(&raw[20 + k * 4], (uint16_t)(int)x["zone2"][k][0]); put16(&raw[22 + k * 4], (uint16_t)(int)x["zone2"][k][1]); }
                if (x.count("blend")) for (int k = 0; k < 4; k++) raw[36 + k] = (uint8_t)(int)x["blend"][k];
                if (x.count("target")) for (int k = 0; k < 3; k++) put16(&raw[44 + k * 2], (uint16_t)(int)x["target"][k]);
                if (x.count("eye")) for (int k = 0; k < 3; k++) put16(&raw[52 + k * 2], (uint16_t)(int)x["eye"][k]);
                if (x.count("extra")) put16(&raw[60], (uint16_t)(int)x["extra"]);
                sec.insert(sec.end(), raw.begin(), raw.end());
            }
        } catch (const std::exception& e) {
            rep.errors++;
            rep.lines.push_back(f.name + ": cameras.json: " + e.what());
            return false;
        }
        place_section(blk, base, 3, r.cam_off, 4 + (uint32_t)r.cameras.size() * 68u, sec, notes, "cameras");
        applied++;
    }
    // ---- зоны звука шагов (hdr[6]; footsteps.json, прежнее имя — triggers.json)
    const char* steps = has("footsteps.json") ? "footsteps.json" : has("triggers.json") ? "triggers.json" : nullptr;
    if (steps && r.trig_off) {
        json j;
        if (!load_json(pd / steps, j, err)) { rep.errors++; rep.lines.push_back(f.name + ": " + steps + ": " + err); return false; }
        Bytes sec(4, 0);
        try {
            auto& tr = j.count("zones") ? j.at("zones") : j.at("triggers");
            put32(&sec[0], (uint32_t)tr.size());
            for (size_t i = 0; i < tr.size(); i++) {
                const json& x = tr[i];
                Bytes raw(20, 0);
                if (i < r.triggers.size()) memcpy(raw.data(), r.triggers[i].raw, 20);
                if (x.count("raw")) from_hex(x["raw"].get<std::string>(), raw);
                if (raw.size() != 20) raw.assign(20, 0);
                if (x.count("zone")) for (int k = 0; k < 4; k++) { put16(&raw[k * 4], (uint16_t)(int)x["zone"][k][0]); put16(&raw[2 + k * 4], (uint16_t)(int)x["zone"][k][1]); }
                raw[16] = (uint8_t)x.value("floor", x.value("level", (int)raw[16]));
                raw[17] = (uint8_t)x.value("material", x.value("value", (int)raw[17]));
                sec.insert(sec.end(), raw.begin(), raw.end());
            }
        } catch (const std::exception& e) {
            rep.errors++;
            rep.lines.push_back(f.name + ": " + steps + ": " + e.what());
            return false;
        }
        place_section(blk, base, 6, r.trig_off, 4 + (uint32_t)r.triggers.size() * 20u, sec, notes, "footstep zones");
        applied++;
    }
    // ---- скрипт: подпрограммы того же размера (байты как есть)
    if (has("script.json") && r.script_size) {
        json j;
        if (!load_json(pd / "script.json", j, err)) { rep.errors++; rep.lines.push_back(f.name + ": script.json: " + err); return false; }
        try {
            for (auto& s : j.at("subroutines")) {
                uint32_t off = s.at("offset"), size = s.at("size");
                Bytes b;
                if (!from_hex(s.at("bytes").get<std::string>(), b)) throw std::runtime_error("bad hex");
                if (b.size() != size) throw std::runtime_error(strfmt("subroutine %d: size changed (%u -> %u), not supported yet",
                                                                      s.value("index", -1), size, (unsigned)b.size()));
                if (r.script_off + off + size > blk.size()) throw std::runtime_error("offset outside the script");
                memcpy(&blk[r.script_off + off], b.data(), b.size());
            }
        } catch (const std::exception& e) {
            rep.errors++;
            rep.lines.push_back(f.name + ": script.json: " + e.what());
            return false;
        }
        applied++;
    }
    // ---- 06.10.2026: enemy kinds of other rooms (the Studio's enemy drop-down): model, motion table and motions of the kind
    // copied to the end of the room block (species.cpp); the records get the new addresses (the placement steps take this json)
    json enemies_put;
    bool have_put = false;
    Bytes modblk;                       // 06.10.2026: the block of the memory for mods (species_put; written at the end)
    std::vector<std::pair<uint32_t, Bytes>> foreign_blocks;   // 06.10.2026: other stages' code (banks) and table entries
    // 14.10.2026: enemies the scenario adds - "from ROOM MODEL MOTIONS" + kind -> model, motions, kind in this room
    std::map<std::string, std::array<uint32_t, 3>> from_put;
    if (has("enemies.json") || has("script.txt")) {
        json j = json::object();
        if (has("enemies.json") && !load_json(pd / "enemies.json", j, err)) { rep.errors++; rep.lines.push_back(f.name + ": enemies.json: " + err); return false; }
        // 14.10.2026 (player: "the scene's raptor does not become an enemy after its scene - it flies about broken"): an enemy
        // line a scenario adds names a fighter of its kind ("from" after it, script_edit.h); its motions come in the same way
        // as a species of enemies.json - as extra records here, taken out again after the loop
        if (has("script.txt")) {
            Bytes t;
            read_file(pd / "script.txt", t);
            std::istringstream in(std::string(t.begin(), t.end()));
            std::string ln, prev;
            std::set<std::string> seen;
            if (!j.count("enemies")) j["enemies"] = json::array();
            while (std::getline(in, ln)) {
                const size_t p0 = ln.find_first_not_of(" \t");
                if (p0 == std::string::npos) continue;
                if (ln.compare(p0, 5, "from ") != 0) {
                    if (ln[p0] == '#' || ln[p0] == '@') prev = ln.substr(p0);
                    continue;
                }
                char room[16] = {};
                unsigned mo = 0, mt = 0;
                if (sscanf(ln.c_str() + p0 + 5, "%15s %x %x", room, &mo, &mt) != 3) continue;
                std::istringstream ps(prev);
                std::string lab, op, slot, kind;
                ps >> lab >> op >> slot >> kind;
                const std::string id = strfmt("%s %08X %08X %s", room, mo, mt, kind.c_str());
                if (op != "20" || seen.count(id)) continue;
                seen.insert(id);
                j["enemies"].push_back({{"kind", (int)strtol(kind.c_str(), nullptr, 16)}, {"script_from", id},
                                        {"species", {{"room", room}, {"model", strfmt("%08X", mo)}, {"motions", strfmt("%08X", mt)}, {"actor", "enemy"}}}});
            }
        }
        std::map<std::string, std::pair<uint32_t, uint32_t>> put;
        // 06.10.2026 (player: "where is adding any enemy to any room - all the dinosaurs"): a kind runs by its stage's table
        // (STn.BIN at 0x800E8000: kind -> its start function). A kind of another stage gets that stage's overlay as a bank in
        // the memory for mods (0x82E00000 + 128 KB each; the runtime runs it in place of the stage's window) and the room's
        // stage table gets the kind: at its own number when that is free here, else at a free one (the record takes it).
        auto stage_of = [](const std::string& room) { return room.size() >= 3 && room[2] >= '1' && room[2] <= '9' ? room[2] - '0' : 0; };
        const int tstage = stage_of(f.stem);
        uint32_t toff = 0, tent[32] = {};
        bool ttbl = false;
        if (tstage) {
            const DiscFile* so = d.find(strfmt("ST%d.BIN", tstage));
            Bytes ovl;
            ttbl = so && d.read_file(*so, ovl) && stage_kind_table(ovl, toff, tent);
        }
        std::map<int, int> bank_of;                   // source stage -> bank number
        std::map<int, Bytes> bank_img;                // bank number -> the overlay image (window size)
        std::map<std::pair<int, int>, int> slot_of;   // (source stage, kind) -> the kind's number in this room
        std::vector<std::pair<uint32_t, uint32_t>> tbl_patch;   // table address, value
        // 06.10.2026 (player: "the added dinosaurs have the sounds of other dinosaurs"): bank number -> (kind here, its room);
        // written as a note at the end of the bank (DCFKSND1) - Dino Crisis ME plays their sounds from those rooms' banks
        std::map<int, std::vector<std::pair<int, std::string>>> bank_note;
        auto foreign_kind = [&](json& e, const std::string& room) -> bool {
            const int sstage = stage_of(room), kind = e.value("kind", 0);
            if (!ttbl || !sstage || sstage == tstage || kind < 0 || kind > 31) return true;
            auto known = slot_of.find({sstage, kind});
            if (known != slot_of.end()) { e["kind"] = known->second; return true; }
            const DiscFile* so = d.find(strfmt("ST%d.BIN", sstage));
            Bytes ovl;
            uint32_t soff = 0, sent[32] = {};
            if (!so || !d.read_file(*so, ovl) || !stage_kind_table(ovl, soff, sent)) { err = strfmt("cannot read the table of stage %d", sstage); return false; }
            const uint32_t es = sent[kind];
            if (!es) { err = strfmt("kind %d has no start in stage %d", kind, sstage); return false; }
            uint32_t value = es;                                   // the program's own code: the same address everywhere
            const bool in_window = es >= kOverlayBase && es < 0x80100000u;
            if (in_window) {
                auto b = bank_of.find(sstage);
                if (b == bank_of.end()) {
                    if (bank_of.size() >= kForeignBanks) { err = "more than 8 stages of enemies in one room"; return false; }
                    const int nb = (int)bank_of.size();
                    b = bank_of.emplace(sstage, nb).first;
                    Bytes img = ovl;
                    img.resize(0x18000u, 0);                       // the whole window: the code's own data past the file starts at 0
                    bank_img[nb] = std::move(img);
                }
                value = kForeignBankBase + (uint32_t)b->second * kForeignBankSize + (es - kOverlayBase);
            }
            int slot = -1;
            if (tent[kind] == value || tent[kind] == 0) slot = kind;
            else
                for (int k = 3; k < 32 && slot < 0; k++) {
                    bool taken = tent[k] != 0;
                    for (auto& kv : slot_of) taken = taken || kv.second == k;
                    if (!taken) slot = k;
                }
            if (slot < 0) { err = "the stage's table of kinds is full"; return false; }
            if (tent[slot] != value) {
                tbl_patch.push_back({kOverlayBase + toff + (uint32_t)slot * 4u, value});
                tent[slot] = value;
            }
            slot_of[{sstage, kind}] = slot;
            if (in_window) bank_note[bank_of[sstage]].push_back({slot, room});
            if (slot != kind) e["kind"] = slot;
            notes.push_back(strfmt("kind %d of stage %d runs here as kind %d (%s)", kind, sstage, slot,
                                   in_window ? strfmt("its stage's code in bank %d", bank_of[sstage]).c_str() : "the program's code"));
            return true;
        };
        // 06.10.2026 (player: "the compsognathus has its texture shifted again"): a kind whose body this room has not has its
        // pictures elsewhere in video memory - and the room's video memory is full (pictures, frame, the common models). Its
        // model with its own pictures goes to mods\rooms\<ROOM>\kinds\<kind>_<ROOM>\ and kinds\index.txt: Dino Crisis ME draws
        // that body from there in this room only. Made again on every build.
        const fs::path kinds_dir = pd.empty() ? fs::path() : pd / "kinds";
        std::vector<std::string> kinds_index;
        if (!kinds_dir.empty()) { std::error_code ec2; fs::remove_all(kinds_dir, ec2); }
        if (j.count("enemies"))
            for (auto& e : j["enemies"]) {
                if (!e.count("species") || !e["species"].is_object() || e.value("removed", false)) continue;
                const json& s = e["species"];
                const std::string room = upper(s.value("room", std::string()));
                if (room.empty() || room == f.stem) continue;
                if (!foreign_kind(e, room)) {
                    rep.errors++;
                    rep.lines.push_back(f.name + ": the enemy from " + room + ": " + err);
                    return false;
                }
                uint32_t mo = 0, mt = 0;
                try {
                    mo = (uint32_t)std::stoul(s.value("model", std::string("0")), nullptr, 16);
                    mt = (uint32_t)std::stoul(s.value("motions", std::string("0")), nullptr, 16);
                } catch (...) {}
                const std::string id = strfmt("%s %08X %08X", room.c_str(), mo, mt);
                auto it = put.find(id);
                if (it == put.end()) {
                    const DiscFile* sf = d.find(room + ".DAT");
                    FileContent sc;
                    if (!sf || !analyze(d, *sf, sc)) { rep.errors++; rep.lines.push_back(f.name + ": enemies.json: cannot read " + room); return false; }
                    uint32_t nm = 0, nt = 0;
                    bool own = false;
                    std::string note;
                    if (!species_put(sc, mo, mt, &fc, blk, base, 0x5D000, modblk, kModRamBase, nm, nt, own, note, err)) {
                        rep.errors++;
                        rep.lines.push_back(f.name + ": the enemy from " + room + ": " + err);
                        return false;
                    }
                    notes.push_back("an enemy kind of " + room + " put into the room: " + note);
                    if (!own && !kinds_dir.empty()) {
                        int part = -1;
                        for (size_t ai = 0; ai < sc.actors.size() && part < 0; ai++) {
                            const ActorPart& a = sc.actors[ai];
                            if (a.block >= 0 && a.block < (int)sc.blocks.size() && sc.blocks[a.block].dest + a.model.hdr == mo) part = (int)ai;
                        }
                        const std::string kname = s.value("actor", std::string("kind")) + "_" + room;
                        uint64_t key = 0;
                        if (part >= 0) {
                            const RamBlock& b = sc.blocks[sc.actors[part].block];
                            key = actor_key_blk(b.data, b.dest, sc.actors[part].model.hdr);
                        }
                        Exporter kx(d, kinds_dir, ExportOptions(), nullptr, false);
                        if (key && kx.write_model(sc, part, kinds_dir / u8path(kname), kname)) {
                            kinds_index.push_back(strfmt("actor %016llx rooms/%s/kinds/%s/%s.gltf", (unsigned long long)key, f.stem.c_str(),
                                                         kname.c_str(), kname.c_str()));
                            notes.push_back("its pictures: rooms\\" + f.stem + "\\kinds\\" + kname + " (drawn from there in this room)");
                        } else
                            notes.push_back("its pictures could not be written: wrong colors in this room");
                    }
                    it = put.emplace(id, std::make_pair(nm, nt)).first;
                }
                e["model"] = strfmt("%08X", it->second.first);
                e["motions"] = strfmt("%08X", it->second.second);
                have_put = true;
            }
        if (j.count("enemies")) {   // 14.10.2026: the scenario's enemies out again (their records are lines of the script)
            json keep = json::array();
            for (auto& e : j["enemies"]) {
                if (!e.count("script_from")) { keep.push_back(e); continue; }
                const json& sp = e["species"];
                const std::string mo = e.value("model", sp.value("model", std::string("0"))), mt = e.value("motions", sp.value("motions", std::string("0")));
                from_put[e["script_from"].get<std::string>()] = {(uint32_t)std::stoul(mo, nullptr, 16), (uint32_t)std::stoul(mt, nullptr, 16),
                                                                  (uint32_t)e.value("kind", 0)};
            }
            j["enemies"] = std::move(keep);
        }
        if (have_put && has("enemies.json")) { enemies_put = std::move(j); applied++; }
        if (!kinds_index.empty()) {
            std::string t = "# Dino Crisis ME Studio: enemy kinds of this room drawn with their own pictures (made by the room's build)\n";
            for (auto& l : kinds_index) t += l + "\n";
            if (!write_text(kinds_dir / "index.txt", t)) { rep.errors++; rep.lines.push_back(f.name + ": cannot write kinds\\index.txt"); }
        }
        // 06.10.2026: blocks of ours for the kinds of other stages - the overlays as banks, then the table entries
        for (auto& kv : bank_img) foreign_blocks.push_back({kForeignBankBase + (uint32_t)kv.first * kForeignBankSize, std::move(kv.second)});
        for (auto& kv : bank_note) {                    // the note: "DCFKSND1", count, then {kind, 0, room[14]} per kind
            const size_t n = std::min<size_t>(kv.second.size(), 15);
            Bytes nb(16 + 16 * n, 0);
            memcpy(nb.data(), "DCFKSND1", 8);
            nb[8] = (uint8_t)n;
            for (size_t i = 0; i < n; i++) {
                uint8_t* p = nb.data() + 16 + i * 16;
                p[0] = (uint8_t)kv.second[i].first;
                memcpy(p + 2, kv.second[i].second.data(), std::min<size_t>(kv.second[i].second.size(), 13));
            }
            foreign_blocks.push_back({kForeignBankBase + (uint32_t)kv.first * kForeignBankSize + 0x1FF00u, std::move(nb)});
        }
        for (auto& p : tbl_patch) {
            Bytes w(4);
            put32(w.data(), p.second);
            foreign_blocks.push_back({p.first, std::move(w)});
        }
    }
    // ---- расстановка (на месте в байткоде; после script.json, чтобы правки полей были последними)
    const Bytes blk_pre_place = blk;   // 06.10.2026: for script.txt below - which lines the json edits changed
    for (const char* w : {"enemies", "items", "doors", "zones"}) {
        fs::path p = pd / (std::string(w) + ".json");
        if (pd.empty() || !fs::exists(p)) continue;
        json j;
        if (std::string(w) == "enemies" && have_put) j = enemies_put;
        else if (!load_json(p, j, err)) { rep.errors++; rep.lines.push_back(f.name + ": " + w + ".json: " + err); return false; }
        if (!apply_room_placement(j, w, blk, r, err)) {
            rep.errors++;
            rep.lines.push_back(f.name + ": " + w + ".json: " + err);
            return false;
        }
        applied++;
    }
    // ---- скрипт заново: script.txt (весь скрипт текстом), добавленные/убранные враги и предметы, тексты любой длины.
    // Всё, что ссылается внутрь скрипта (переходы, каталог, смещения текстов и таблиц), пересчитывается (script_edit.h).
    const bool want_script = has("script.txt") || has("texts.json") || has("enemies.json") || has("items.json");
    if (want_script && r.script_size) {
        RoomInfo r2;
        ScriptImage im;
        bool parsed = parse_room(blk, base, r2);
        // 14.10.2026: an enemy of another room has its model put at the end of the room block - right after the script when
        // that was the last section, and the script then "ran on" over it (its size is "to the next section or the end").
        // A script rebuilt in place wrote over the model (ST106: the raptor of a scenario scene crashed the game when it was
        // made; an enemies.json copy got by only because its script moved). The script is as long as it was.
        if (parsed && r2.script_off == r.script_off && r2.script_size > r.script_size) r2.script_size = r.script_size;
        if (!parsed || !script_decompose(blk, r2, im, err)) { rep.errors++; rep.lines.push_back(f.name + ": script: " + err); return false; }
        std::vector<ScriptAtom> atoms = im.atoms;
        if (has("script.txt")) {
            Bytes t;
            read_file(pd / "script.txt", t);
            if (!script_from_text(im, std::string(t.begin(), t.end()), atoms, err)) { rep.errors++; rep.lines.push_back(f.name + ": script.txt: " + err); return false; }
            for (auto& a : atoms) {   // 14.10.2026: an enemy of the scenario takes the fighter brought in above
                if (a.from.empty() || a.b.size() < 24) continue;
                auto fp = from_put.find(a.from + strfmt(" %02X", a.b[2]));
                if (fp == from_put.end()) { rep.errors++; rep.lines.push_back(f.name + ": script.txt: the fighter " + a.from + " was not brought in"); return false; }
                a.b[2] = (uint8_t)fp->second[2];
                put32(&a.b[16], fp->second[0]);
                put32(&a.b[20], fp->second[1]);
                notes.push_back("script.txt: an enemy of the scenario moves with the motions of " + a.from.substr(0, a.from.find(' ')));
            }
            // 06.10.2026 (end-to-end test, ST106): the text carries the record bytes as they were when it was written, so an
            // enemy moved in enemies.json went back to its old place once a piece was moved in script.txt. A line the text
            // left as it was takes the bytes the json edits gave it; a line changed in the text keeps the text.
            RoomInfo r0;
            ScriptImage im0;
            if (blk_pre_place != blk && parse_room(blk_pre_place, base, r0) && script_decompose(blk_pre_place, r0, im0, err)) {
                int merged = 0;
                for (auto& a : atoms) {
                    if (a.old < 0 || a.kind != ScriptAtom::OP) continue;
                    const int i0 = im0.find((uint32_t)a.old), ip = im.find((uint32_t)a.old);
                    if (i0 < 0 || ip < 0) continue;
                    if (a.b == im0.atoms[i0].b && im.atoms[ip].b != im0.atoms[i0].b) { a.b = im.atoms[ip].b; merged++; }
                }
                if (merged) notes.push_back(strfmt("script.txt: %d record(s) took the edits of the placement files", merged));
            }
            err.clear();
        }
        for (const char* w : {"enemies", "items"}) {
            const fs::path p = pd / (std::string(w) + ".json");
            if (!has((std::string(w) + ".json").c_str())) continue;
            json j;
            if (std::string(w) == "enemies" && have_put) j = enemies_put;
            else if (!load_json(p, j, err)) { rep.errors++; rep.lines.push_back(f.name + ": " + w + ".json: " + err); return false; }
            bool structural = false;
            if (j.count(w))
                for (auto& e : j[w])
                    structural |= e.count("copy_of") > 0 || e.value("removed", false) ||
                                  (std::string(w) == "items" && (e.count("rx") || e.count("ry") || e.count("rz") || item_core_upright(e)));
            if (!structural) continue;
            pctx.ensure(d);   // 06.10.2026: enemies too - a new enemy gets a free death flag
            if (!room_placement_structure(j, w, atoms, pctx, notes, err)) { rep.errors++; rep.lines.push_back(f.name + ": " + w + ".json: " + err); return false; }
        }
        if (has("texts.json") && !r2.strings.empty()) {
            json j;
            if (!load_json(pd / "texts.json", j, err)) { rep.errors++; rep.lines.push_back(f.name + ": texts.json: " + err); return false; }
            try {
                for (auto& t : j.at("texts")) {
                    size_t i = t.at("index");
                    if (i >= r2.strings.size()) throw std::runtime_error(strfmt("text %d: no such text", (int)i));
                    const GameString& gs = r2.strings[i];
                    std::vector<uint16_t> codes;
                    std::string e;
                    if (!encode_text(t.at("text").get<std::string>(), codes, e)) throw std::runtime_error(strfmt("text %d: %s", (int)i, e.c_str()));
                    if (codes == gs.codes) continue;
                    int ai = -1;
                    for (size_t k = 0; k < atoms.size(); k++)
                        if (atoms[k].kind == ScriptAtom::STRING && atoms[k].old >= 0 && (uint64_t)atoms[k].old + atoms[k].text_from == gs.off - r2.script_off) ai = (int)k;
                    if (ai < 0) {   // строка не своим атомом (рядом с кодом) — как прежде: на месте, не длиннее
                        if (codes.size() > gs.codes.size())
                            throw std::runtime_error(strfmt("text %d can not be longer here (%d codes max)", (int)i, (int)gs.codes.size()));
                        codes.pop_back();
                        for (size_t k = 0; k + 1 < gs.codes.size(); k++) put16(&blk[gs.off + k * 2], k < codes.size() ? codes[k] : (uint16_t)0x8000);
                        put16(&blk[gs.off + (gs.codes.size() - 1) * 2], 0xA000);
                        continue;
                    }
                    if (!script_set_text(atoms[ai], codes, e)) throw std::runtime_error(strfmt("text %d: %s", (int)i, e.c_str()));
                }
            } catch (const std::exception& e) {
                rep.errors++;
                rep.lines.push_back(f.name + ": texts.json: " + e.what());
                return false;
            }
        }
        // строки, изменённые на месте (вне атомов), — уже в blk; пересобранная секция берёт атомы — разобрать заново не нужно:
        // атомы строк, которых правка на месте коснулась, не трогались (у таких строк нет своего атома)
        Bytes sec;
        if (!script_assemble(im, atoms, sec, err)) { rep.errors++; rep.lines.push_back(f.name + ": script: " + err); return false; }
        const Bytes cur(blk.begin() + r2.script_off, blk.begin() + r2.script_off + r2.script_size);
        if (sec != cur) {
            if (!script_install(blk, base, r2, sec, 0x5D000, notes, err, &im)) { rep.errors++; rep.lines.push_back(f.name + ": " + err); return false; }
            applied++;
        }
    }
    // прочие блоки файла с движениями (не блок комнаты)
    bool other_changed = false;
    for (auto& kv : mb)
        if (kv.first != fc.room_block && kv.second != fc.blocks[kv.first].data) other_changed = true;
    // ничего не изменилось (папку скопировали из source как есть) — файл не собираем, прежний свой вывод убираем
    if (blk == fc.blocks[fc.room_block].data && !other_changed) {
        if (!base_mods && fs::exists(out)) drop_output(out, old_marker);
        rep.unchanged++;
        return true;
    }
    // блок комнаты живёт в 0x80100000..0x8015D000 (с 0x8015D000 — модель перехода DOOR*); самый большой на диске — 379 892 Б
    if (blk.size() > 0x5D000) {
        rep.errors++;
        rep.lines.push_back(strfmt("%s: the room block grew to %u bytes, the game has room for %u", f.name.c_str(), (unsigned)blk.size(), 0x5D000u));
        return false;
    }
    Bytes file;
    if (!rebuild_container(orig, ents, entry, blk, f.nsec * 2048u, file, err, true)) {
        rep.errors++;
        rep.lines.push_back(f.name + ": " + err);
        for (auto& n : notes) rep.lines.push_back("  " + n);   // 06.10.2026: what the edits did, so the reason is seen
        return false;
    }
    if (file.size() > f.nsec * 2048u)
        notes.push_back(strfmt("the room file grew past its place on the disc (%u -> %u sectors): Dino Crisis ME reads it from mods",
                               f.nsec, (unsigned)((file.size() + 2047u) / 2048u)));
    const uint32_t file_room = std::max<uint32_t>(f.nsec * 2048u, (uint32_t)file.size());
    for (auto& kv : mb) {
        if (kv.first == fc.room_block || kv.second == fc.blocks[kv.first].data) continue;
        auto e2 = parse_container(file);
        Bytes rebuilt;
        if (!rebuild_container(file, e2, fc.blocks[kv.first].entry, kv.second, file_room, rebuilt, err)) {
            rep.errors++;
            rep.lines.push_back(f.name + ": " + err);
            return false;
        }
        file.swap(rebuilt);
    }
    {   // 06.10.2026: models and motions of enemies from other rooms - the memory for mods. Blocks of ours left by an earlier
        // build of a mods file go first: a build without them must not keep a stale one
        std::vector<std::pair<uint32_t, Bytes>> ours;
        if (!modblk.empty()) ours.push_back({kModRamBase, modblk});
        for (auto& fb : foreign_blocks) ours.push_back(std::move(fb));
        bool had_ours = false;
        for (auto& e : parse_container(file)) had_ours = had_ours || ours_entry(e);
        if ((!ours.empty() || had_ours) && !container_put_ours(file, ours, err)) {
            rep.errors++;
            rep.lines.push_back(f.name + ": " + err);
            return false;
        }
        if (!modblk.empty())
            notes.push_back(strfmt("memory for mods: %u bytes at %08X (enemies from other rooms; the room block has its own room)",
                                   (unsigned)modblk.size(), kModRamBase));
        if (ours.size() > (modblk.empty() ? 0u : 1u))
            notes.push_back(strfmt("%d block(s) for enemy kinds of other stages", (int)(ours.size() - (modblk.empty() ? 0u : 1u))));
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    if (!write_file(out, file.data(), file.size())) { rep.errors++; rep.lines.push_back(f.name + ": cannot write " + path_u8(out)); return false; }
    json mk = {{"source", f.name}, {"base", base_mods ? "mods file" : "disc"}, {"applied", applied}, {"output_sha1", sha1_hex(file.data(), file.size())}};
    write_marker(out, mk, old_marker);
    rep.built++;
    std::string line = strfmt("%s: built from %d change(s) on top of the %s (%u bytes of %u)", f.name.c_str(), applied,
                              base_mods ? "file in mods" : "disc file", (unsigned)file.size(), f.nsec * 2048u);
    rep.lines.push_back(line);
    for (auto& n : notes) rep.lines.push_back("  " + n);
    return true;
}

// ---------------------------------------------------------------- экраны: image.png 320x240 → картинка в блоке файла
bool convert_screen(Disc& d, const DiscFile& f, const fs::path& pd, const fs::path& out, ConvertReport& rep) {
    std::vector<fs::path> pngs;
    for (int k = 1; k <= 8; k++) {
        fs::path p = pd / (k == 1 ? std::string("image.png") : strfmt("image_%d.png", k));
        if (fs::exists(p)) pngs.push_back(p);
        else if (k > 1) break;
        else pngs.push_back(fs::path());
    }
    bool any = false;
    for (auto& p : pngs) any |= !p.empty();
    const fs::path old_marker = pd / ".dcme_built.json";
    if (!any) {
        if (marker_ours(out, old_marker)) drop_output(out, old_marker);
        return true;
    }
    Bytes orig;
    bool base_mods = fs::exists(out) && !marker_ours(out, old_marker);
    if (base_mods) read_file(out, orig);
    else if (!d.read_file(f, orig)) { rep.errors++; rep.lines.push_back(f.name + ": cannot read the disc file"); return false; }
    auto ents = parse_container(orig);
    // картинки — блоки в RAM размером 320x240x16 (TITLE: + 64 байта заголовка), по порядку
    std::vector<int> img_entries;
    for (size_t i = 0; i < ents.size(); i++) {
        if (ents[i].type != 7 && ents[i].type != 0) continue;
        Bytes blk;
        if (ents[i].type == 7) { if (!lzss_decode(&orig[ents[i].offset], ents[i].size, blk)) continue; }
        else blk.assign(orig.begin() + ents[i].offset, orig.begin() + ents[i].offset + ents[i].size);
        if (blk.size() == 153600 || blk.size() == 153664) img_entries.push_back((int)i);
    }
    Bytes file = orig;
    int applied = 0;
    for (size_t k = 0; k < pngs.size(); k++) {
        if (pngs[k].empty()) continue;
        if (k >= img_entries.size()) { rep.errors++; rep.lines.push_back(f.name + ": " + path_u8(pngs[k].filename()) + ": the screen has no such picture"); return false; }
        int w = 0, h = 0, n = 0;
        Bytes png;
        read_file(pngs[k], png);
        unsigned char* px = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &n, 4);
        if (!px) { rep.errors++; rep.lines.push_back(f.name + ": " + path_u8(pngs[k].filename()) + ": not a PNG"); return false; }
        if (w != 320 || h != 240) {
            // картинка другого размера (крупнее) — не в формат игры: игра показывает её сама вместо залитой (растр, по ключу
            // пикселей картинки игры — FNV-1a 64 без бита 15, как dcvk_screen_begin)
            stbi_image_free(px);
            auto cur = parse_container(file);
            const DatEntry& e = cur[img_entries[k]];
            Bytes blk;
            if (e.type == 7) lzss_decode(&file[e.offset], e.size, blk);
            else blk.assign(file.begin() + e.offset, file.begin() + e.offset + e.size);
            const size_t o = blk.size() - 153600;
            uint64_t key = 0xCBF29CE484222325ull;
            for (int i = 0; i < 320 * 240; i++) {
                const uint16_t c = le16(&blk[o + i * 2]) & 0x7FFF;
                key ^= (uint8_t)c; key *= 0x100000001B3ull;
                key ^= (uint8_t)(c >> 8); key *= 0x100000001B3ull;
            }
            rep.screen_index.push_back(strfmt("320 240 %016llx %s/%s", (unsigned long long)key, f.stem.c_str(),
                                              path_u8(pngs[k].filename()).c_str()));
            rep.lines.push_back(strfmt("%s: %s %dx%d is shown instead of the picture", f.name.c_str(),
                                       path_u8(pngs[k].filename()).c_str(), w, h));
            continue;
        }
        auto cur = parse_container(file);
        const DatEntry& e = cur[img_entries[k]];
        Bytes blk;
        if (e.type == 7) lzss_decode(&file[e.offset], e.size, blk);
        else blk.assign(file.begin() + e.offset, file.begin() + e.offset + e.size);
        size_t o = blk.size() - 153600;
        for (int i = 0; i < 320 * 240; i++) {
            uint16_t old = le16(&blk[o + i * 2]);
            const unsigned char* q = px + i * 4;
            uint32_t was = c15_rgba(old);
            if ((was & 0xFFFFFF) == ((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16))) continue;   // как было — байты те же
            uint16_t v = (uint16_t)((q[0] >> 3) | ((q[1] >> 3) << 5) | ((q[2] >> 3) << 10));
            v |= old & 0x8000;
            if (v == 0) v = 0x8000;   // чёрный — непрозрачный (0x0000 у игры — прозрачный)
            put16(&blk[o + i * 2], v);
        }
        stbi_image_free(px);
        Bytes rebuilt;
        std::string err;
        if (!rebuild_container(file, cur, img_entries[k], blk, f.nsec * 2048u, rebuilt, err)) {
            rep.errors++;
            rep.lines.push_back(f.name + ": " + err);
            return false;
        }
        file.swap(rebuilt);
        applied++;
    }
    Bytes disc;
    d.read_file(f, disc);
    if (!applied || file == disc) {   // только крупные картинки или пиксели те же: файл игры не нужен (игра возьмёт диск)
        if (!base_mods && fs::exists(out)) drop_output(out, old_marker);
        if (applied) rep.unchanged++;
        else rep.built++;
        return true;
    }
    if (!write_file(out, file.data(), file.size())) { rep.errors++; rep.lines.push_back(f.name + ": cannot write " + path_u8(out)); return false; }
    json mk = {{"source", f.name}, {"base", base_mods ? "mods file" : "disc"}, {"applied", applied}, {"output_sha1", sha1_hex(file.data(), file.size())}};
    write_marker(out, mk, old_marker);
    rep.built++;
    rep.lines.push_back(strfmt("%s: %d picture(s) -> %u bytes of %u", f.name.c_str(), applied, (unsigned)file.size(), f.nsec * 2048u));
    return true;
}

// персонажи (CORE, P/H) и переходы: движения из glTF каталога
static bool convert_motions_file(Disc& d, const DiscFile& f, const fs::path& out, ConvertReport& rep, const std::vector<MotionJob>* jobs) {
    if (!jobs || jobs->empty()) {
        if (marker_ours(out, fs::path())) drop_output(out, fs::path());
        return true;
    }
    Bytes orig;
    bool base_mods = false;
    if (!load_base(d, f, out, fs::path(), orig, base_mods, rep)) return false;
    FileContent fc;
    if (!analyze_raw(d, f, orig, fc)) { rep.errors++; rep.lines.push_back(f.name + ": cannot read"); return false; }
    std::map<int, Bytes> mb;
    MotionResult mr;
    std::vector<std::string> notes;
    apply_jobs(fc, jobs, mb, mr, true);
    motion_report(f, mr, rep, notes);
    if (!mr.errors.empty()) return false;
    Bytes file = orig;
    int blocks = 0;
    for (auto& kv : mb) {
        if (kv.second == fc.blocks[kv.first].data) continue;
        auto ents = parse_container(file);
        Bytes rebuilt;
        std::string err;
        if (!rebuild_container(file, ents, fc.blocks[kv.first].entry, kv.second, f.nsec * 2048u, rebuilt, err)) {
            rep.errors++;
            rep.lines.push_back(f.name + ": " + err);
            return false;
        }
        file.swap(rebuilt);
        blocks++;
    }
    if (!blocks) {
        if (!base_mods && fs::exists(out)) drop_output(out, fs::path());
        rep.unchanged++;
        return true;
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    if (!write_file(out, file.data(), file.size())) { rep.errors++; rep.lines.push_back(f.name + ": cannot write " + path_u8(out)); return false; }
    json mk = {{"source", f.name}, {"base", base_mods ? "mods file" : "disc"}, {"output_sha1", sha1_hex(file.data(), file.size())}};
    write_marker(out, mk, fs::path());
    rep.built++;
    rep.lines.push_back(strfmt("%s: motions -> %u bytes of %u", f.name.c_str(), (unsigned)file.size(), f.nsec * 2048u));
    for (auto& n : notes) rep.lines.push_back("  " + n);
    return true;
}

// картинки предметов: item_NNN.png (96×64) → запись NNN файла ITEM.DAT / ITEM2.DAT (палитра — по картинке)
bool convert_items(Disc& d, const DiscFile& f, const fs::path& pd, const fs::path& out, ConvertReport& rep) {
    std::vector<std::pair<int, fs::path>> pngs;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(pd, ec)) {
        std::string n = path_u8(e.path().filename());
        int id = -1;
        if (sscanf(n.c_str(), "item_%d.png", &id) == 1 && id >= 0 && id < kItemCount) pngs.push_back({id, e.path()});
    }
    const fs::path old_marker = pd / ".dcme_built.json";
    if (pngs.empty()) {
        if (marker_ours(out, old_marker)) drop_output(out, old_marker);
        return true;
    }
    Bytes orig;
    bool base_mods = false;
    if (!load_base(d, f, out, old_marker, orig, base_mods, rep)) return false;
    if (orig.size() < (size_t)kItemCount * 5 * 2048) { rep.errors++; rep.lines.push_back(f.name + ": not an item picture file"); return false; }
    Bytes file = orig;
    int applied = 0;
    for (auto& [id, p] : pngs) {
        int w = 0, h = 0, n = 0;
        Bytes png;
        read_file(p, png);
        unsigned char* px = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &n, 4);
        if (!px) { rep.errors++; rep.lines.push_back(f.name + ": " + path_u8(p.filename()) + ": not a PNG"); return false; }
        if (w != kItemW || h != kItemH) {
            stbi_image_free(px);
            rep.errors++;
            rep.lines.push_back(strfmt("%s: %s is %dx%d, must be %dx%d", f.name.c_str(), path_u8(p.filename()).c_str(), w, h, kItemW, kItemH));
            return false;
        }
        std::vector<uint32_t> was;
        item_decode(&file[(size_t)id * 5 * 2048], was);
        if (memcmp(was.data(), px, was.size() * 4) != 0) {
            item_encode((const uint32_t*)px, &file[(size_t)id * 5 * 2048]);
            applied++;
        }
        stbi_image_free(px);
    }
    Bytes disc;
    d.read_file(f, disc);
    if (file == disc || !applied) {
        if (!base_mods && fs::exists(out)) drop_output(out, old_marker);
        rep.unchanged++;
        return true;
    }
    if (!write_file(out, file.data(), file.size())) { rep.errors++; rep.lines.push_back(f.name + ": cannot write " + path_u8(out)); return false; }
    json mk = {{"source", f.name}, {"base", base_mods ? "mods file" : "disc"}, {"output_sha1", sha1_hex(file.data(), file.size())}};
    write_marker(out, mk, old_marker);
    rep.built++;
    rep.lines.push_back(strfmt("%s: %d picture(s)", f.name.c_str(), applied));
    return true;
}

// программа игры: door_scenes.json, items.json, texts.json → mods\other\SLUS_009.22 (данные игра берёт при старте)
// оверлей этапа: roster.json → mods\other\STn.BIN
bool convert_other(Disc& d, const DiscFile& f, const fs::path& pd, const fs::path& out, ConvertReport& rep) {
    const bool is_exe = upper(f.name) == "SLUS_009.22";
    std::vector<std::string> parts = is_exe ? std::vector<std::string>{"door_scenes", "items", "texts"} : std::vector<std::string>{"roster"};
    bool any = false;
    for (auto& p : parts) any |= fs::exists(pd / (p + ".json"));
    const fs::path old_marker = pd / ".dcme_built.json";
    if (!any) {
        if (marker_ours(out, old_marker)) drop_output(out, old_marker);
        return true;
    }
    Bytes orig;
    bool base_mods = false;
    if (!load_base(d, f, out, old_marker, orig, base_mods, rep)) return false;
    Bytes disc;
    d.read_file(f, disc);
    Bytes file = orig;
    for (auto& p : parts) {
        fs::path jp = pd / (p + ".json");
        if (!fs::exists(jp)) continue;
        json j;
        std::string err;
        bool ok = load_json(jp, j, err);
        if (ok) ok = is_exe ? apply_exe(j, p.c_str(), file, err) : apply_roster(j, file, f.stem[2] - '0', err);
        if (!ok) { rep.errors++; rep.lines.push_back(f.name + ": " + p + ".json: " + err); return false; }
    }
    if (file == disc) {
        if (!base_mods && fs::exists(out)) drop_output(out, old_marker);
        rep.unchanged++;
        return true;
    }
    if (!write_file(out, file.data(), file.size())) { rep.errors++; rep.lines.push_back(f.name + ": cannot write " + path_u8(out)); return false; }
    json mk = {{"source", f.name}, {"base", base_mods ? "mods file" : "disc"}, {"output_sha1", sha1_hex(file.data(), file.size())}};
    write_marker(out, mk, old_marker);
    size_t diff = 0;
    for (size_t i = 0; i < file.size() && i < disc.size(); i++) diff += file[i] != disc[i];
    rep.built++;
    rep.lines.push_back(strfmt("%s: %u bytes changed", f.name.c_str(), (unsigned)diff));
    return true;
}

// ---------------------------------------------------------------- текстуры PNG → mods\textures\index.txt
// Картинку любого размера игра берёт вместо текстуры сама (gpu_vk_raster.cpp, TEXREP): по странице, палитре и окну UV, если
// содержимое видеопамяти в окне то же, что у игры. Картинки лежат рядом с моделями (mods\characters\regina\tp….png,
// mods\rooms\ST100\tp….png; прежнее место — mods\textures\<ФАЙЛ>\): по имени и папке находится вырезка, ИЗМЕНЁННАЯ (пиксели
// не те) — во все места игры с теми же пикселями, в любом файле. Строка: страница (hex) x палитры, y палитры, u0 v0 u1 v1,
// ключ (hex), путь картинки от mods\textures.

// ключ содержимого, как у игры (trep_key_vram): FNV-1a 64 индексов текселей окна и всех цветов палитры; 15 бит — пикселей;
// цвета без бита 15
static uint64_t tex_key(const Vram& v, uint16_t tpage, int cx, int cy, int u0, int v0, int u1, int v1) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto put = [&](uint8_t b) { h ^= b; h *= 0x100000001B3ull; };
    const int px = (tpage & 15) * 64, py = ((tpage >> 4) & 1) * 256, mode = (tpage >> 7) & 3;
    for (int y = v0; y <= v1; y++)
        for (int x = u0; x <= u1; x++) {
            const size_t row = (size_t)((py + y) & 511) * 1024;
            if (mode == 0) { uint16_t w = v.px[row + ((px + (x >> 2)) & 1023)]; put((uint8_t)((w >> ((x & 3) * 4)) & 15)); }
            else if (mode == 1) { uint16_t w = v.px[row + ((px + (x >> 1)) & 1023)]; put((uint8_t)((w >> ((x & 1) * 8)) & 0xFF)); }
            else { uint16_t w = v.px[row + ((px + x) & 1023)] & 0x7FFF; put((uint8_t)w); put((uint8_t)(w >> 8)); }
        }
    // цвета — без бита 15: игра ставит его палитрам после загрузки
    if (mode < 2) {
        const int n = mode == 0 ? 16 : 256;
        for (int i = 0; i < n; i++) {
            uint16_t c = v.px[(size_t)(cy & 511) * 1024 + ((cx + i) & 1023)] & 0x7FFF;
            put((uint8_t)c); put((uint8_t)(c >> 8));
        }
    }
    return h;
}

struct TexPlace { const DiscFile* f; TexCrop c; uint64_t key; };

// все вырезки файла (меши и модели) с ключами; файлы без текстурных мешей — пусто
static bool file_tex_places(Disc& d, const DiscFile& f, std::vector<TexPlace>& out) {
    FileContent c;
    if (!analyze(d, f, c)) return false;
    auto add = [&](std::vector<TexCrop>&& cr) {
        for (auto& x : cr) {
            const uint64_t k = tex_key(c.vram, x.tpage, clut_x(x.clut), clut_y(x.clut), x.u0, x.v0, x.u1, x.v1);
            out.push_back({&f, std::move(x), k});
        }
    };
    for (auto& m : c.meshes) add(crops_for_mesh(c.vram, m.tris));
    for (auto& a : c.actors) add(crops_for_model(c.vram, a.model));
    return true;
}

static bool texture_owner_file(const DiscFile& f) {
    return !f.stream && (f.cat == "rooms" || f.cat == "doors" || f.cat == "characters" || f.cat == "items");
}

// картинка та же, что у игры (поправленной нет): размер и пиксели (у прозрачных — только прозрачность)
static bool png_same_as_crop(const fs::path& p, const TexCrop& c) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path_u8(p).c_str(), &w, &h, &n, 4);
    if (!px) return false;
    bool same = w == c.w() && h == c.h() && c.rgba.size() == (size_t)w * (size_t)h;
    for (size_t i = 0; same && i < c.rgba.size(); i++) {
        const uint8_t* q = px + i * 4;
        const uint32_t a = c.rgba[i];
        const uint8_t aa = (uint8_t)(a >> 24);
        if (aa == 0 && q[3] == 0) continue;
        same = q[0] == (uint8_t)a && q[1] == (uint8_t)(a >> 8) && q[2] == (uint8_t)(a >> 16) && q[3] == aa;
    }
    stbi_image_free(px);
    return same;
}

static void convert_textures(Disc& d, const Library& lib, const fs::path& mods, ConvertReport& rep, ExportLog* log) {
    std::error_code ec;
    const fs::path root = mods / "textures";
    const fs::path idx = root / "index.txt";
    struct Pic { fs::path path; std::string rel, shown; const DiscFile* owner = nullptr; unsigned tp = 0; int cx = 0, cy = 0, u0 = 0, u1 = 0, v0 = 0, v1 = 0; };
    std::vector<Pic> pics;
    // папка → файл игры, чьи вырезки в ней лежат
    auto owner_of = [&](const std::string& cat, const std::string& folder) -> const DiscFile* {
        if (cat == "characters") { if (const LibActor* a = lib.actor(folder)) return a->at.f; }
        else if (cat == "weapons" || cat == "items" || cat == "doors") { if (const LibMesh* m = lib.mesh(folder)) return m->at.f; }
        return d.find(upper(folder) + ".DAT");
    };
    auto scan = [&](const std::string& cat, const fs::path& dir, const std::string& folder) {
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            if (upper(path_u8(e.path().extension())) != ".PNG") continue;
            const std::string nm = path_u8(e.path().filename());
            Pic p;
            char tail = 0;
            if (sscanf(nm.c_str(), "tp%x_cx%d_cy%d_u%d-%d_v%d-%d.pn%c", &p.tp, &p.cx, &p.cy, &p.u0, &p.u1, &p.v0, &p.v1, &tail) != 8) continue;
            p.path = e.path();
            p.rel = rel_to(e.path(), root);   // от mods\textures: «../characters/regina/tp….png»
            p.shown = cat + "\\" + folder + "\\" + nm;
            p.owner = owner_of(cat, folder);
            pics.push_back(p);
        }
    };
    for (const char* cat : {"textures", "characters", "weapons", "items", "doors", "rooms"}) {
        const fs::path cd = mods / cat;
        if (!fs::is_directory(cd, ec)) continue;
        for (auto& e : fs::directory_iterator(cd, ec))
            if (e.is_directory() && path_u8(e.path().filename())[0] != '.') scan(cat, e.path(), path_u8(e.path().filename()));
    }
    // пиксели каждой картинки — по вырезке файла, в папке которого она лежит
    std::map<std::string, std::vector<TexPlace>> places;   // имя файла диска → его вырезки
    auto places_of = [&](const DiscFile& f) -> std::vector<TexPlace>& {
        auto it = places.find(f.name);
        if (it != places.end()) return it->second;
        auto& v = places[f.name];
        file_tex_places(d, f, v);
        return v;
    };
    std::map<std::string, std::string> want;   // хеш пикселей игры → путь картинки
    int same = 0;
    for (auto& p : pics) {
        if (!p.owner || !texture_owner_file(*p.owner)) {
            rep.lines.push_back(p.shown + ": no game file with models for this folder, skipped");
            continue;
        }
        const TexCrop* hit = nullptr;
        for (auto& tp : places_of(*p.owner)) {
            const TexCrop& c = tp.c;
            if (c.tpage == p.tp && clut_x(c.clut) == p.cx && clut_y(c.clut) == p.cy && c.u0 == p.u0 && c.u1 == p.u1 &&
                c.v0 == p.v0 && c.v1 == p.v1) { hit = &c; break; }
        }
        if (!hit) {
            rep.lines.push_back(p.shown + ": no such texture in " + p.owner->name + ", skipped");
            continue;
        }
        if (png_same_as_crop(p.path, *hit)) { same++; continue; }   // не правили — подмена не нужна
        int w = 0, h = 0, n = 0;
        if (!stbi_info(path_u8(p.path).c_str(), &w, &h, &n)) {
            rep.lines.push_back(p.shown + ": not a readable PNG, skipped");
            rep.errors++;
            continue;
        }
        if (!want.count(hit->hash)) want[hit->hash] = p.rel;
        else if (want[hit->hash] != p.rel) rep.lines.push_back(p.shown + ": the same texture is already taken from " + want[hit->hash]);
    }
    if (want.empty()) {
        if (fs::exists(idx, ec)) { fs::remove(idx, ec); rep.lines.push_back("textures: no changed pictures, index.txt removed"); }
        return;
    }
    // все места тех же пикселей во всей игре
    std::vector<const DiscFile*> all;
    for (auto& f : d.files) if (texture_owner_file(f)) all.push_back(&f);
    if (log) { std::lock_guard<std::mutex> lk(log->mu); log->current = "textures"; }
    std::set<std::string> seen;
    std::string out = "# Dino Crisis ME Studio: textures from PNG (page, palette x y, window u0 v0 u1 v1, content key, picture)\n";
    int lines = 0;
    std::set<std::string> used;
    for (const DiscFile* f : all) {
        if (log && log->cancel) return;
        for (auto& tp : places_of(*f)) {
            auto w = want.find(tp.c.hash);
            if (w == want.end()) continue;
            const unsigned tpm = tp.c.tpage & 0x19F;
            std::string ln = strfmt("%03x %d %d %d %d %d %d %016llx ", tpm, clut_x(tp.c.clut), clut_y(tp.c.clut), tp.c.u0, tp.c.v0,
                                    tp.c.u1, tp.c.v1, (unsigned long long)tp.key);
            if (!seen.insert(ln).second) continue;
            out += ln + w->second + "\n";
            used.insert(w->second);
            lines++;
        }
        places.erase(f->name);   // память: вырезки с пикселями больше не нужны
    }
    fs::create_directories(root, ec);
    if (!write_text(idx, out)) { rep.lines.push_back("textures: cannot write " + path_u8(idx)); rep.errors++; return; }
    rep.lines.push_back(strfmt("textures: %d changed pictures, %d places in the game (%d pictures unchanged)", (int)used.size(), lines, same));
    rep.built++;
}

// ---------------------------------------------------------------- меши из glTF → mods\<папка>\index.txt
// Строка: actor|group, ключ (hex), путь от mods [\t узел]. Ключ — по содержимому (library.cpp, как dc_meshrep.cpp): одна
// строка — модель подменяется во всех файлах и комнатах, где она есть. В список — только изменённое (meshsig.h).
static const char* kMeshIndexHead = "# Dino Crisis ME Studio: meshes from glTF (actor|group, key, path from mods, tab node)";

// модель в папке: <id>.gltf / <id>.glb, иначе единственный glTF папки
static fs::path find_model(const fs::path& dir, const std::string& id, std::string& note) {
    for (const char* ext : {".gltf", ".glb"}) {
        fs::path p = dir / u8path(id + ext);
        if (fs::exists(p)) return p;
    }
    std::error_code ec;
    std::vector<fs::path> all;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        const std::string x = upper(path_u8(e.path().extension()));
        if (e.is_regular_file() && (x == ".GLTF" || x == ".GLB")) all.push_back(e.path());
    }
    if (all.size() == 1) return all[0];
    if (all.size() > 1) note = strfmt("%d models and none is named %s.gltf: skipped", (int)all.size(), id.c_str());
    return fs::path();
}

static std::vector<std::string> missing_images(const GDoc& d, const fs::path& gp) {
    std::vector<std::string> out;
    if (!d.j.count("images")) return out;
    for (auto& im : d.j["images"]) {
        const std::string uri = im.value("uri", "");
        if (uri.empty() || uri.rfind("data:", 0) == 0) continue;
        if (!fs::exists(gp.parent_path() / u8path(uri_decode(uri)))) out.push_back(uri);
    }
    return out;
}

struct CatalogOut {
    std::map<std::string, std::vector<std::string>> index;   // папка mods → строки
    MotionJobs jobs;
};

// модель изменена → строка; картинки модели должны лежать рядом (игра их читает сама)
static bool accept_mesh(const GDoc& doc, const fs::path& gp, const std::string& rel, ConvertReport& rep) {
    auto miss = missing_images(doc, gp);
    if (miss.empty()) return true;
    rep.errors++;
    rep.lines.push_back(rel + ": no picture " + miss[0] + " next to the model (the game reads it from there)");
    return false;
}

static void convert_catalog(Disc& d, const Library& lib, const fs::path& mods, CatalogOut& out, ConvertReport& rep, ExportLog* log) {
    std::error_code ec;
    // 06.10.2026 (игрок: «разные костюмы Регины — разные модели… остальные костюмы не должны быть тронуты»): заменённые
    // костюмы Регины — правка «оружия в руке» (в нём кисть костюма; у двух костюмов меш бывает один и тот же: H20 = H22)
    // действует только при них (поле actor= строки; игра сверяет модель персонажа); ни один не заменён — при любом
    std::vector<uint64_t> regina_replaced;
    // ---- персонажи: меш (actor) и движения (<набор>_<NN> во все файлы набора)
    for (const LibActor& la : lib.actors) {
        if (log && log->cancel) return;
        const fs::path dir = mods / "characters" / u8path(la.id);
        if (!fs::is_directory(dir, ec)) continue;
        if (log) { std::lock_guard<std::mutex> lk(log->mu); log->current = "characters\\" + la.id; }
        std::string note;
        const fs::path gp = find_model(dir, la.id, note);
        if (!note.empty()) rep.lines.push_back("characters\\" + la.id + ": " + note);
        if (gp.empty()) continue;
        const std::string rel = rel_to(gp, mods);
        auto doc = std::make_shared<GDoc>();
        std::string err;
        if (!doc->load(gp, err)) { rep.errors++; rep.lines.push_back(rel + ": " + err); continue; }
        FileContent c;
        if (!analyze(d, *la.at.f, c) || la.at.part < 0 || la.at.part >= (int)c.actors.size()) {
            rep.errors++;
            rep.lines.push_back(rel + ": cannot read " + la.at.f->name);
            continue;
        }
        auto ap = std::make_shared<ActorPart>(c.actors[la.at.part]);
        SigMesh so, sm;
        sig_actor(*ap, c.vram, so);
        if (!sig_gltf(*doc, "", true, sm, err)) { rep.errors++; rep.lines.push_back(rel + ": " + err); }
        else if (!sig_same(so, sm) && accept_mesh(*doc, gp, rel, rep)) {
            out.index["characters"].push_back(strfmt("actor %016llx %s", (unsigned long long)la.key, rel.c_str()));
            if (la.kind == "regina") regina_replaced.push_back(la.key);
            rep.lines.push_back(strfmt("%s: the model is replaced everywhere (%d file(s))", rel.c_str(), 1 + (int)la.also.size()));
            rep.built++;
        }
        // движения: анимация <набор>_<NN> — движение каталога, во все его места (тот же раптор в 15 комнатах — одна правка)
        int unknown = 0;
        for (auto& an : doc->anims) {
            const int m = lib.motion(an.name);
            if (m < 0) { unknown++; continue; }
            const LibMotion& lm = lib.motions[m];
            if (lm.nbones != (int)ap->model.bones.size()) continue;
            out.jobs[lm.at.f].push_back({doc, rel, ap, &an, lm.at.set, lm.at.motion});
            for (auto& pl : lm.also) out.jobs[pl.f].push_back({doc, rel, ap, &an, pl.set, pl.motion});
        }
        if (unknown) rep.lines.push_back(strfmt("%s: %d animation(s) with names not from the game (<set>_<NN>): not used", rel.c_str(), unknown));
    }
    // ---- движения сцен комнат (06.10.2026): rooms\<КОМНАТА>\scene\*.gltf — анимации <набор>_<NN> на скелете Регины,
    // в каждое место движения (как у персонажа)
    if (const LibActor* rg = lib.actor("regina")) {
        FileContent rc;
        std::shared_ptr<ActorPart> rap;
        for (auto& kv : lib.scene_motions) {
            if (log && log->cancel) return;
            const fs::path dir = mods / "rooms" / u8path(kv.first->stem) / "scene";
            if (!fs::is_directory(dir, ec)) continue;
            for (auto& e : fs::directory_iterator(dir, ec)) {
                const std::string x = upper(path_u8(e.path().extension()));
                if (!e.is_regular_file() || (x != ".GLTF" && x != ".GLB")) continue;
                if (!rap) {
                    if (!analyze(d, *rg->at.f, rc) || rg->at.part < 0 || rg->at.part >= (int)rc.actors.size()) break;
                    rap = std::make_shared<ActorPart>(rc.actors[rg->at.part]);
                }
                const std::string rel = rel_to(e.path(), mods);
                auto doc = std::make_shared<GDoc>();
                std::string err;
                if (!doc->load(e.path(), err)) { rep.errors++; rep.lines.push_back(rel + ": " + err); continue; }
                int used = 0, unknown = 0;
                for (auto& an : doc->anims) {
                    const int m = lib.motion(an.name);
                    if (m < 0) { unknown++; continue; }
                    const LibMotion& lm = lib.motions[m];
                    if (lm.nbones != (int)rap->model.bones.size()) continue;
                    out.jobs[lm.at.f].push_back({doc, rel, rap, &an, lm.at.set, lm.at.motion});
                    for (auto& pl : lm.also) out.jobs[pl.f].push_back({doc, rel, rap, &an, pl.set, pl.motion});
                    used++;
                }
                if (unknown) rep.lines.push_back(strfmt("%s: %d animation(s) with names not from the game (<set>_<NN>): not used", rel.c_str(), unknown));
                (void)used;
            }
        }
    }
    // ---- неподвижные меши: оружие, оружие в руке, предметы, переходы
    for (const LibMesh& lm : lib.meshes) {
        if (log && log->cancel) return;
        const fs::path dir = mods / u8path(lm.cat) / u8path(lm.id);
        if (!fs::is_directory(dir, ec)) continue;
        if (log) { std::lock_guard<std::mutex> lk(log->mu); log->current = lm.cat + "\\" + lm.id; }
        std::string note;
        const fs::path gp = find_model(dir, lm.id, note);
        if (!note.empty()) rep.lines.push_back(lm.cat + "\\" + lm.id + ": " + note);
        if (gp.empty()) continue;
        const std::string rel = rel_to(gp, mods);
        GDoc doc;
        std::string err;
        if (!doc.load(gp, err)) { rep.errors++; rep.lines.push_back(rel + ": " + err); continue; }
        FileContent c;
        if (!analyze(d, *lm.at.f, c) || lm.at.part < 0 || lm.at.part >= (int)c.meshes.size()) {
            rep.errors++;
            rep.lines.push_back(rel + ": cannot read " + lm.at.f->name);
            continue;
        }
        const MeshPart& mp = c.meshes[lm.at.part];
        const bool by_group = mp.hdrs.size() > 1;
        int changed = 0;
        bool bad = false;
        std::vector<std::string> names;
        for (size_t gi = 0; gi < mp.hdrs.size() && gi < lm.keys.size(); gi++) {
            const std::string node = by_group ? strfmt("group_%02d", (int)gi) : std::string();
            names.push_back(node);
            SigMesh so, sm;
            sig_static(mp, c.vram, by_group ? (int)gi : -1, so);
            if (!sig_gltf(doc, node, false, sm, err)) {
                if (by_group) rep.lines.push_back(rel + ": " + err + " (the group is left as in the game)");
                else { rep.errors++; rep.lines.push_back(rel + ": " + err); bad = true; }
                continue;
            }
            if (sig_same(so, sm)) continue;
            if (!changed && !accept_mesh(doc, gp, rel, rep)) { bad = true; break; }
            std::string line = strfmt("group %016llx %s", (unsigned long long)lm.keys[gi], rel.c_str()) + (by_group ? "\t" + node : "");
            if (lm.kind == "in_hand" && !regina_replaced.empty()) {
                line += (by_group ? "" : "\t") + std::string("\tactor=");
                for (size_t k = 0; k < regina_replaced.size(); k++) line += strfmt(k ? ",%016llx" : "%016llx", (unsigned long long)regina_replaced[k]);
            }
            out.index[lm.cat].push_back(line);
            changed++;
        }
        if (bad) continue;
        if (by_group)
            for (auto& n : nodes_outside(doc, names)) rep.lines.push_back(rel + ": node " + n + " is outside group_NN: the game does not see it");
        if (changed) {
            if (lm.kind == "in_hand" && !regina_replaced.empty())
                rep.lines.push_back(strfmt("%s: %d group(s) replaced, only with the replaced costume(s) of Regina", rel.c_str(), changed));
            else
                rep.lines.push_back(strfmt("%s: %d group(s) replaced everywhere (%d file(s))", rel.c_str(), changed, 1 + (int)lm.also.size()));
            rep.built++;
        }
    }
    // ---- тела комнат: группа (group_NN) — по ключу содержимого (та же группа в комнате-двойнике меняется тоже)
    for (auto& f : d.files) {
        if (log && log->cancel) return;
        if (f.cat != "rooms" || f.stream) continue;
        const fs::path dir = mods / "rooms" / u8path(f.stem);
        if (!fs::is_directory(dir, ec)) continue;
        std::string note;
        const fs::path gp = find_model(dir, f.stem, note);
        if (!note.empty()) rep.lines.push_back("rooms\\" + f.stem + ": " + note);
        if (gp.empty()) continue;
        if (log) { std::lock_guard<std::mutex> lk(log->mu); log->current = "rooms\\" + f.stem; }
        const std::string rel = rel_to(gp, mods);
        GDoc doc;
        std::string err;
        if (!doc.load(gp, err)) { rep.errors++; rep.lines.push_back(rel + ": " + err); continue; }
        FileContent c;
        if (!analyze(d, f, c)) { rep.errors++; rep.lines.push_back(rel + ": cannot read " + f.name); continue; }
        const MeshPart* mp = nullptr;
        for (auto& m : c.meshes) if (m.kind == "room" && m.block >= 0) { mp = &m; break; }
        if (!mp) continue;
        const RamBlock& b = c.blocks[mp->block];
        const bool by_group = mp->hdrs.size() > 1;
        int changed = 0;
        bool bad = false;
        std::vector<std::string> names;
        for (size_t gi = 0; gi < mp->hdrs.size(); gi++) {
            const std::string node = by_group ? strfmt("group_%02d", (int)gi) : std::string();
            names.push_back(node);
            SigMesh so, sm;
            sig_static(*mp, c.vram, by_group ? (int)gi : -1, so);
            if (!sig_gltf(doc, node, false, sm, err)) {
                if (by_group) rep.lines.push_back(rel + ": " + err + " (the group is left as in the game)");
                else { rep.errors++; rep.lines.push_back(rel + ": " + err); bad = true; }
                continue;
            }
            if (sig_same(so, sm)) continue;
            if (!changed && !accept_mesh(doc, gp, rel, rep)) { bad = true; break; }
            const uint64_t key = group_key_blk(b.data, b.dest, mp->hdrs[gi], false);
            out.index["rooms"].push_back(strfmt("group %016llx %s", (unsigned long long)key, rel.c_str()) + (by_group ? "\t" + node : ""));
            changed++;
        }
        if (bad) continue;
        if (by_group)
            for (auto& n : nodes_outside(doc, names)) rep.lines.push_back(rel + ": node " + n + " is outside group_NN: the game does not see it");
        if (changed) {
            rep.lines.push_back(strfmt("%s: %d of %d group(s) replaced", rel.c_str(), changed, (int)mp->hdrs.size()));
            rep.built++;
        }
    }
}

// список мешей папки: свой (с нашей шапкой) — переписать или убрать; чужой (игрок написал руками) — не трогать
static void write_mesh_index(const fs::path& mods, const std::string& folder, const std::vector<std::string>& lines, ConvertReport& rep) {
    std::error_code ec;
    const fs::path p = mods / u8path(folder) / "index.txt";
    if (fs::exists(p, ec)) {
        Bytes b;
        read_file(p, b);
        const std::string head(kMeshIndexHead);
        const bool ours = b.size() >= 24 && std::string(b.begin(), b.begin() + std::min(b.size(), head.size())) == head;
        if (!ours) {
            if (!lines.empty()) rep.lines.push_back(folder + "\\index.txt is not written by this program: left as is");
            return;
        }
        if (lines.empty()) { fs::remove(p, ec); return; }
    }
    if (lines.empty()) return;
    std::string t = std::string(kMeshIndexHead) + "\n";
    for (auto& l : lines) t += l + "\n";
    fs::create_directories(p.parent_path(), ec);
    if (!write_text(p, t)) { rep.errors++; rep.lines.push_back("cannot write " + path_u8(p)); }
}

void convert_all(Disc& d, const fs::path& mods, ConvertReport& rep, ExportLog* log, const Library* shared_lib) {
    std::error_code ec;
    // каталог без повторов (из приложения — готовый, иначе строим: ~2 с)
    Library own;
    const Library* lib = shared_lib && shared_lib->ready ? shared_lib : nullptr;
    if (!lib) {
        if (log) { std::lock_guard<std::mutex> lk(log->mu); log->current = "catalog"; }
        own.build(d);
        lib = &own;
    }
    // ---- каталог: меши и движения
    CatalogOut cat;
    convert_catalog(d, *lib, mods, cat, rep, log);
    // ---- файлы игры: папки правок (комнаты, экраны, картинки предметов, программа) и файлы с движениями
    struct Job { const DiscFile* f; fs::path dir; };
    std::map<const DiscFile*, Job> todo;
    for (const char* c : {"rooms", "screens", "items", "other", "characters", "doors", "weapons"}) {
        if (!fs::exists(mods / c, ec)) continue;
        for (auto& e : fs::directory_iterator(mods / c, ec)) {
            if (!e.is_directory()) continue;
            const std::string folder = path_u8(e.path().filename());
            if (folder.empty() || folder[0] == '.') continue;
            const std::string stem = upper(folder);
            const std::string cs = c;
            if (cs == "characters" && lib->actor(folder)) continue;                      // каталог — выше
            if ((cs == "weapons" || cs == "items" || cs == "doors") && lib->mesh(folder)) continue;
            const DiscFile* f = nullptr;
            if (cs == "other") {
                f = stem == "SLUS_009" ? d.find("SLUS_009.22") : d.find(stem + ".BIN");
                if (!f) continue;   // DEMO*.TRG и прочее — без правок
            } else {
                f = d.find(stem + ".DAT");
            }
            if (cs == "characters" || cs == "doors" || cs == "weapons") {
                bool models = false;
                for (auto& x : fs::directory_iterator(e.path(), ec)) {
                    const std::string ext = upper(path_u8(x.path().extension()));
                    models |= ext == ".GLTF" || ext == ".GLB";
                }
                if (models)
                    rep.lines.push_back(cs + "\\" + folder + (f ? ": the old layout (models by game file): export again, models now live in "
                                                                  "characters\\<name> (regina, raptor...), weapons\\, items\\, doors\\"
                                                                : ": no such model in the catalog, skipped"));
                continue;
            }
            if (!f || f->cat != cs) { rep.lines.push_back(cs + "\\" + folder + ": no such file on the disc, skipped"); continue; }
            todo[f] = {f, e.path()};
        }
    }
    for (auto& kv : cat.jobs)
        if (!todo.count(kv.first)) todo[kv.first] = {kv.first, fs::path()};
    if (log) { log->total = (int)todo.size() + 2; log->done = 0; }
    PlacementCtx pctx;   // флаги «взят» во всей игре — один раз на конвертацию (только если добавляют предметы)
    for (auto& kv : todo) {
        if (log && log->cancel) break;
        const Job& t = kv.second;
        if (log) { std::lock_guard<std::mutex> lk(log->mu); log->current = t.f->name; }
        const fs::path out = mods / u8path(t.f->cat) / u8path(t.f->name);
        auto jt = cat.jobs.find(t.f);
        const std::vector<MotionJob>* jobs = jt == cat.jobs.end() ? nullptr : &jt->second;
        if (t.f->cat == "screens") convert_screen(d, *t.f, t.dir, out, rep);
        else if (t.f->cat == "rooms") convert_room(d, *t.f, t.dir, out, rep, jobs, &pctx);
        else if (t.f->cat == "other") convert_other(d, *t.f, t.dir, out, rep);
        else if (t.f->cat == "items") convert_items(d, *t.f, t.dir, out, rep);
        else convert_motions_file(d, *t.f, out, rep, jobs);
        if (log) log->done++;
    }
    // наш прежний вывод файлов, у которых правок больше нет (движения убрали из glTF) — убрать
    for (const char* c : {"characters", "doors", "rooms"}) {
        const fs::path md = mods / c / ".dcme";
        if (!fs::is_directory(md, ec)) continue;
        for (auto& e : fs::directory_iterator(md, ec)) {
            std::string n = path_u8(e.path().filename());
            if (n.size() < 6 || upper(n.substr(n.size() - 5)) != ".JSON") continue;
            n.resize(n.size() - 5);
            const DiscFile* f = d.find(upper(n));
            if (!f || todo.count(f)) continue;
            const fs::path out = mods / c / u8path(n);
            if (marker_ours(out, fs::path())) {
                drop_output(out, fs::path());
                rep.lines.push_back(std::string(c) + "\\" + n + ": no changes left, removed");
            }
        }
    }
    // ---- списки для игры
    for (const char* c : {"characters", "weapons", "items", "doors", "rooms"}) write_mesh_index(mods, c, cat.index[c], rep);
    convert_textures(d, *lib, mods, rep, log);
    if (log) log->done++;
    {   // экраны PNG крупнее 320x240 — список для игры
        const fs::path si = mods / "screens" / "index.txt";
        if (!rep.screen_index.empty()) {
            std::string t = "# Dino Crisis ME Studio: screens from PNG (width height key picture)\n";
            for (auto& l : rep.screen_index) t += l + "\n";
            if (!write_text(si, t)) { rep.errors++; rep.lines.push_back("screens: cannot write " + path_u8(si)); }
        } else if (fs::exists(si, ec)) fs::remove(si, ec);
    }
    if (log) log->done++;
    if (rep.built == 0 && rep.errors == 0) rep.lines.push_back("nothing to convert");
}

}  // namespace dc
