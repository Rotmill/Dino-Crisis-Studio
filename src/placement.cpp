#include "placement.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "script.h"
#include "script_edit.h"

namespace dc {

// ---------------------------------------------------------------- обход скрипта (re/plc/scr.py)
std::vector<ScriptOpAt> script_walk(const Bytes& blk, uint32_t so, uint32_t ssize, const std::vector<uint32_t>& dir) {
    std::vector<ScriptOpAt> out;
    if (so >= blk.size()) return out;
    const uint32_t lim = (uint32_t)std::min<size_t>(ssize, blk.size() - so);
    const uint8_t* b = &blk[so];
    std::set<uint32_t> seen;
    for (size_t si = 0; si < dir.size(); si++) {
        std::vector<uint32_t> stack{dir[si]};
        std::vector<ScriptOpAt> got;
        while (!stack.empty()) {
            uint32_t pc = stack.back();
            stack.pop_back();
            while (pc + 4 <= lim && !seen.count(pc)) {
                int n = script_op_size(b + pc, lim - pc);
                int op = b[pc];
                if (n <= 0 || pc + (uint32_t)n > lim) break;
                seen.insert(pc);
                got.push_back(ScriptOpAt{(int)si, pc, op, n});
                if (op == 0x04 || op == 0x05 || op == 0x10 || op == 0x11) break;
                int16_t rel = (int16_t)le16(b + pc + 2);
                if (op == 0x0C) { pc = (uint32_t)((int)pc + rel); continue; }
                if (op == 0x0E) {
                    uint32_t tgt = (uint32_t)((int)pc + rel);
                    if (b[pc + 1] == 0) { pc = tgt; continue; }
                    stack.push_back(tgt);
                }
                if (op == 0x09 || op == 0x0B || op == 0x17) stack.push_back((uint32_t)((int)pc + rel));
                pc += (uint32_t)n;
            }
        }
        std::sort(got.begin(), got.end(), [](const ScriptOpAt& a, const ScriptOpAt& c) { return a.pc < c.pc; });
        out.insert(out.end(), got.begin(), got.end());
    }
    return out;
}

// ---------------------------------------------------------------- поля записей
enum FT { U8, S16, U16, U32X, XYZ, ROOM };
struct FD { const char* name; int off; FT t; };

static json get_field(const uint8_t* p, const FD& f) {
    switch (f.t) {
    case U8: return p[f.off];
    case S16: return (int)s16(p + f.off);
    case U16: return (int)le16(p + f.off);
    case U32X: return strfmt("%08X", le32(p + f.off));
    case XYZ: return json::array({(int)s16(p + f.off), (int)s16(p + f.off + 2), (int)s16(p + f.off + 4)});
    case ROOM: return room_code(le16(p + f.off));
    }
    return nullptr;
}

static int parse_room(const std::string& s) {
    // "ST10A" → 0x10A; число — как есть
    std::string u = upper(s);
    if (u.rfind("ST", 0) == 0) u = u.substr(2);
    return (int)strtol(u.c_str(), nullptr, 16);
}

static void put_field(uint8_t* p, const FD& f, const json& v) {
    switch (f.t) {
    case U8: p[f.off] = (uint8_t)v.get<int>(); break;
    case S16: put16(p + f.off, (uint16_t)(int16_t)v.get<int>()); break;
    case U16: put16(p + f.off, (uint16_t)v.get<int>()); break;
    case U32X: put32(p + f.off, v.is_string() ? (uint32_t)strtoul(v.get<std::string>().c_str(), nullptr, 16) : v.get<uint32_t>()); break;
    case XYZ: for (int k = 0; k < 3; k++) put16(p + f.off + k * 2, (uint16_t)(int16_t)v.at(k).get<int>()); break;
    case ROOM: put16(p + f.off, (uint16_t)(v.is_string() ? parse_room(v.get<std::string>()) : v.get<int>())); break;
    }
}

static void fields_out(json& o, const uint8_t* p, const FD* fd, size_t n) {
    for (size_t i = 0; i < n; i++) o[fd[i].name] = get_field(p, fd[i]);
}
static void fields_in(const json& o, uint8_t* p, const FD* fd, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (o.count(fd[i].name)) put_field(p, fd[i], o[fd[i].name]);
}

static const FD kOp20[] = {{"slot", 1, U8},  {"kind", 2, U8},  {"variant", 3, U8},     {"death_flag", 4, U8}, {"ai", 5, U8},
                           {"hp", 6, U16},   {"x", 8, S16},    {"y", 10, S16},         {"z", 12, S16},        {"rotation", 14, S16},
                           {"model", 16, U32X}, {"motions", 20, U32X}};
static const FD kOp59[] = {{"slot", 1, U8}, {"kind", 2, U8}, {"variant", 3, U8}, {"model", 4, U32X}, {"motions", 8, U32X},
                           {"places", 12, U32X}, {"roster_room", 16, U8}};
static const FD kOp5B[] = {{"slot", 1, U8},        {"kind", 2, U8},          {"variant", 3, U8},       {"x", 4, S16},
                           {"y", 6, S16},          {"z", 8, S16},            {"rotation", 10, S16},    {"model", 12, U32X},
                           {"motions", 16, U32X},  {"roster_room", 20, U8},  {"chance", 21, U8},       {"max_distance", 22, U16},
                           {"ai", 24, U8},         {"flags", 25, U8},        {"camera_shot", 26, U8},  {"scene_sub", 27, U8},
                           {"camera_target", 28, XYZ}, {"camera_eye", 36, XYZ}};
// зона (запись с pc+4)
static const FD kZone[] = {{"type", 16, U8}, {"flags", 17, U8}, {"floor", 18, U8}};
static const FD kItem[] = {{"item", 24, U16}, {"count", 26, U16}, {"taken_flag", 28, U16}, {"model_section", 30, U8},
                           {"pickup", 31, U8}, {"model_group", 32, U32X}, {"hidden", 36, U32X}};
static const FD kDoor[] = {{"to", 24, ROOM}, {"x", 26, S16}, {"y", 28, S16}, {"z", 30, S16}, {"rotation", 32, S16},
                           {"scene", 34, U8}, {"open_flag", 35, U8}, {"lock", 36, U8}, {"save", 37, U8}};
#define NF(a) (sizeof(a) / sizeof((a)[0]))

static json quad_out(const uint8_t* p) {
    json q = json::array();
    for (int k = 0; k < 4; k++) q.push_back({(int)s16(p + k * 4), (int)s16(p + k * 4 + 2)});
    return q;
}
static void quad_in(const json& q, uint8_t* p) {
    for (int k = 0; k < 4; k++) {
        put16(p + k * 4, (uint16_t)(int16_t)q.at(k).at(0).get<int>());
        put16(p + k * 4 + 2, (uint16_t)(int16_t)q.at(k).at(1).get<int>());
    }
}

std::string room_code(int v) { return strfmt("ST%X%02X", (v >> 8) & 15, v & 0xFF); }

std::string enemy_kind_name(int k) {
    switch (k) {
    case 1: return "human";
    case 2: return "raptor";
    case 3: return "trex";
    case 4: return "trex_head";
    case 5: return "small_creature";
    case 7: return "pteranodon";
    case 8: return "dino";
    default: return "";
    }
}

static std::string lock_name(int v) {
    switch (v) {
    case 0: return "none";
    case 1: return "locked from the other side";
    case 2: return "opens from this side";
    case 3: return "DDK";
    case 6: return "key card Lv.C";
    case 7: return "key card Lv.B";
    case 8: return "key card Lv.A";
    case 0xFD: return "no reason to enter";
    case 0xFE: return "lift";
    case 0xFF: return "locked";
    default: return "key item " + item_name(v);
    }
}

static std::vector<ScriptOpAt> room_ops(const FileContent& c) {
    if (!c.is_room) return {};
    const Bytes& blk = c.blocks[c.room_block].data;
    return script_walk(blk, c.room.script_off, c.room.script_size, c.room.script_dir);
}

static const char* kAbout = "Edit values in place; records keep their size. Coordinates: mm, Y down (floor L: Y = -425*L); rotation: 4096 = 360.";

json room_enemies_json(const FileContent& c) {
    json arr = json::array();
    if (!c.is_room) return arr;
    const Bytes& blk = c.blocks[c.room_block].data;
    for (auto& o : room_ops(c)) {
        if (o.op != 0x20 && o.op != 0x59 && o.op != 0x5B) continue;
        const uint8_t* p = &blk[c.room.script_off + o.pc];
        json e = {{"op", strfmt("%02X", o.op)}, {"subroutine", o.sub}, {"offset", o.pc}};
        if (o.op == 0x20) fields_out(e, p, kOp20, NF(kOp20));
        else if (o.op == 0x59) fields_out(e, p, kOp59, NF(kOp59));
        else fields_out(e, p, kOp5B, NF(kOp5B));
        e["kind_name"] = enemy_kind_name(p[2]);
        arr.push_back(e);
    }
    return {{"format", "dc-enemies-1"}, {"room", c.file->stem},
            {"about", std::string(kAbout) + " op 20: actor placed here; op 59: a dinosaur from the stage roster (other\\STn\\roster.json) "
                                            "chosen by roster_room; op 5B: a dinosaur that follows into the room. hp 0 = by kind."},
            {"enemies", arr}};
}

json room_items_json(const FileContent& c) {
    json arr = json::array();
    if (!c.is_room) return arr;
    const Bytes& blk = c.blocks[c.room_block].data;
    for (auto& o : room_ops(c)) {
        if (o.op != 0x28 || o.size < 44) continue;
        const uint8_t* p = &blk[c.room.script_off + o.pc];
        if (p[2] != 4) continue;
        const uint8_t* r = p + 4;
        json e = {{"zone_index", p[1]}, {"subroutine", o.sub}, {"offset", o.pc}, {"zone", quad_out(r)}};
        fields_out(e, r, kZone, NF(kZone));
        fields_out(e, r, kItem, NF(kItem));
        int id = le16(r + 24);
        e["item_name"] = id == 0xFF ? std::string("random") : item_name(id);
        arr.push_back(e);
    }
    return {{"format", "dc-items-1"}, {"room", c.file->stem},
            {"about", std::string(kAbout) + " item: id from other\\SLUS_009.22\\items.json (0xFF = random); taken_flag: bit of flag bank 7."},
            {"items", arr}};
}

json room_doors_json(const FileContent& c) {
    json arr = json::array();
    if (!c.is_room) return arr;
    const Bytes& blk = c.blocks[c.room_block].data;
    for (auto& o : room_ops(c)) {
        if (o.op != 0x28 || o.size < 48) continue;
        const uint8_t* p = &blk[c.room.script_off + o.pc];
        if (p[2] != 0) continue;
        const uint8_t* r = p + 4;
        json e = {{"zone_index", p[1]}, {"subroutine", o.sub}, {"offset", o.pc}, {"zone", quad_out(r)}};
        fields_out(e, r, kZone, NF(kZone));
        fields_out(e, r, kDoor, NF(kDoor));
        e["lock_name"] = lock_name(r[36]);
        arr.push_back(e);
    }
    return {{"format", "dc-doors-1"}, {"room", c.file->stem},
            {"about", std::string(kAbout) + " to: room after the door; x,y,z,rotation: Regina there; scene: door animation "
                                            "(other\\SLUS_009.22\\door_scenes.json); open_flag: bit of flag bank 9; save 1 = save point."},
            {"doors", arr}};
}

json room_zones_json(const FileContent& c) {
    json arr = json::array();
    if (!c.is_room) return arr;
    const Bytes& blk = c.blocks[c.room_block].data;
    static const char* kTypes[12] = {"door", "message", "set flag", "run subroutine", "item", "regina state", "regina action",
                                     "examine", "emergency box", "empty", "flag event", "switch"};
    for (auto& o : room_ops(c)) {
        if (o.op != 0x28) continue;
        const uint8_t* p = &blk[c.room.script_off + o.pc];
        if (p[2] == 0 || p[2] == 4 || p[2] >= 12) continue;
        const uint8_t* r = p + 4;
        json e = {{"zone_index", p[1]}, {"subroutine", o.sub}, {"offset", o.pc}, {"what", kTypes[p[2]]}, {"zone", quad_out(r)}};
        fields_out(e, r, kZone, NF(kZone));
        std::string h;
        for (int k = 20; k < o.size - 4; k++) h += strfmt("%02X", r[k]);
        e["data"] = h;
        arr.push_back(e);
    }
    return {{"format", "dc-zones-1"}, {"room", c.file->stem},
            {"about", std::string(kAbout) + " Trigger zones of the room script except doors and items (doors.json, items.json). "
                                            "flags: bit 0 test a point 400 mm ahead of Regina, bit 1 action button only. data: the rest of the record."},
            {"zones", arr}};
}

json room_footsteps_json(const FileContent& c) {
    json arr = json::array();
    for (size_t i = 0; i < c.room.triggers.size(); i++) {
        const uint8_t* r = c.room.triggers[i].raw;
        arr.push_back({{"zone", quad_out(r)}, {"floor", r[16]}, {"material", r[17]}});
    }
    return {{"format", "dc-footsteps-1"}, {"room", c.file->stem},
            {"about", "Floor zones for the sound of steps: zone (x,z corners), floor level, material."}, {"zones", arr}};
}

bool apply_room_placement(const json& j, const char* what, Bytes& blk, const RoomInfo& r, std::string& err) {
    try {
        const std::string w = what;
        const json& arr = j.at(w);
        for (auto& e : arr) {
            if (!e.count("offset") || e.value("removed", false)) continue;   // добавленные и убранные — сборкой скрипта
            uint32_t pc = e.at("offset").get<uint32_t>();
            if (r.script_off + pc + 4 > blk.size() || pc >= r.script_size) throw std::runtime_error(strfmt("offset %u is outside the script", pc));
            uint8_t* p = &blk[r.script_off + pc];
            int sz = script_op_size(p, r.script_size - pc);
            if (w == "enemies") {
                int op = (int)strtol(e.at("op").get<std::string>().c_str(), nullptr, 16);
                if (p[0] != op) throw std::runtime_error(strfmt("offset %u: not op %02X in the script", pc, op));
                if (op == 0x20) fields_in(e, p, kOp20, NF(kOp20));
                else if (op == 0x59) fields_in(e, p, kOp59, NF(kOp59));
                else if (op == 0x5B) {
                    fields_in(e, p, kOp5B, NF(kOp5B));
                    put16(p + 0x22, 0);   // рабочие поля игры (пишет прямо в байткод)
                    put16(p + 0x2A, 0);
                }
            } else {
                int want = w == "doors" ? 0 : w == "items" ? 4 : -1;
                if (p[0] != 0x28 || (want >= 0 && p[2] != want) || sz < 32)
                    throw std::runtime_error(strfmt("offset %u: not a %s zone in the script", pc, what));
                uint8_t* rec = p + 4;
                // 06.10.2026: the game puts the item model at the middle of the zone diagonal, then the script's own edits of
                // its section right after the item (SELECT 3 s; 36 place, 2A fields 3/5) may set x/z - those move with the
                // zone, or the model would stay where it was (pieces.cpp room_item_models shows the same)
                const int ox = s16(rec + 8) + (s16(rec) - s16(rec + 8)) / 2, oz = s16(rec + 10) + (s16(rec + 2) - s16(rec + 10)) / 2;
                if (e.count("zone")) quad_in(e["zone"], rec);
                if (w == "items" && rec[30] != 0xFF) {
                    const int dx = s16(rec + 8) + (s16(rec) - s16(rec + 8)) / 2 - ox, dz = s16(rec + 10) + (s16(rec + 2) - s16(rec + 10)) / 2 - oz;
                    if (dx || dz) {
                        const std::vector<ScriptOpAt> ops = script_walk(blk, r.script_off, r.script_size, r.script_dir);
                        size_t k = 0;
                        while (k < ops.size() && ops[k].pc != pc) k++;
                        bool sel = false;
                        for (k++; k < ops.size() && ops[k].sub == ops[k - 1].sub; k++) {
                            uint8_t* q = &blk[r.script_off + ops[k].pc];
                            if (q[0] == 0x22) { if (q[1] == 3 && q[2] == rec[30]) { sel = true; continue; } break; }
                            if (!sel) break;
                            if (q[0] == 0x36) { put16(q + 2, (uint16_t)(int16_t)(s16(q + 2) + dx)); put16(q + 6, (uint16_t)(int16_t)(s16(q + 6) + dz)); continue; }
                            if (q[0] == 0x37) continue;
                            if (q[0] == 0x2A && (q[2] == 3 || q[2] == 5)) { put32(q + 4, (uint32_t)((int32_t)le32(q + 4) + (q[2] == 3 ? dx : dz))); continue; }
                            if (q[0] == 0x2A) continue;
                            break;
                        }
                    }
                }
                if (e.count("flags")) rec[17] = (uint8_t)e["flags"].get<int>();
                if (e.count("floor")) rec[18] = (uint8_t)e["floor"].get<int>();
                if (w == "doors") fields_in(e, rec, kDoor, NF(kDoor));
                else if (w == "items") fields_in(e, rec, kItem, NF(kItem));
                else if (e.count("data")) {
                    std::string h = e["data"];
                    if ((int)h.size() != (sz - 24) * 2) throw std::runtime_error(strfmt("offset %u: data must be %d bytes", pc, sz - 24));
                    for (int k = 0; k < sz - 24; k++) rec[20 + k] = (uint8_t)strtoul(h.substr(k * 2, 2).c_str(), nullptr, 16);
                }
            }
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- добавить / убрать врага и предмет
void PlacementCtx::ensure(Disc& d) {
    if (ready) return;
    ready = true;
    const ExeImage& x = exe();
    if (x.ok()) {
        const uint32_t n = x.u32(0x8009CB48u + 7 * 8 + 4);
        if (n >= 64 && n <= 4096) bits7 = (int)n;
    }
    used45.insert(0);                                   // 0 - "no flag" for the ACTOR op
    for (auto& f : d.files) {
        if (f.cat != "rooms" || f.stream) continue;
        FileContent c;
        if (!analyze(d, f, c) || !c.is_room) continue;
        const Bytes& blk = c.blocks[c.room_block].data;
        for (auto& o : room_ops(c)) {
            const uint8_t* p = &blk[c.room.script_off + o.pc];
            if (o.op == 0x28 && o.size >= 44 && p[2] == 4) used7.insert(le16(p + 4 + 28));
            if ((o.op == 0x25 || o.op == 0x26 || o.op == 0x18) && p[1] == 7) used7.insert(p[2]);
            if (o.op == 0x28 && o.size >= 32 && p[2] == 2 && p[4 + 24] == 7) used7.insert(p[4 + 25]);
            if (o.op == 0x20 && o.size >= 24) used45.insert(p[4]);
            if ((o.op == 0x25 || o.op == 0x26 || o.op == 0x18) && (p[1] == 4 || p[1] == 5)) used45.insert(p[2]);
        }
    }
    // the stage rosters (op 59 takes a dinosaur from there): their death flags too
    for (auto& f : d.files) {
        if (f.stem.size() != 3 || f.stem[0] != 'S' || f.stem[1] != 'T' || f.stem[2] < '1' || f.stem[2] > '9' || f.name.find(".BIN") == std::string::npos)
            continue;
        Bytes ovl;
        if (!d.read_file(f, ovl)) continue;
        for (uint32_t off : roster_offsets(ovl, f.stem[2] - '0'))
            for (int i = 0; i < 10; i++) used45.insert(ovl[off + i * 24 + 2]);
    }
}

std::set<int> used_enemy_flags(Disc& d) {
    PlacementCtx c;
    c.ensure(d);
    return c.used45;
}

bool stage_kind_table(const Bytes& ovl, uint32_t& off, uint32_t ent[32]) {
    static const uint8_t sig[8] = {0x34, 0xD0, 0x04, 0x80, 0x50, 0x4F, 0x05, 0x80};
    for (size_t i = 0; i + 8 * 4 <= ovl.size(); i += 4) {
        if (memcmp(&ovl[i], sig, 8) != 0) continue;
        off = (uint32_t)i;
        for (int k = 0; k < 32; k++) {
            ent[k] = 0;
            if (i + (size_t)k * 4 + 4 > ovl.size()) continue;
            const uint32_t a = le32(&ovl[i + (size_t)k * 4]);
            if (a >= 0x80010000u && a < 0x80200000u) ent[k] = a;
            else if (a != 0) { for (int q = k; q < 32; q++) ent[q] = 0; break; }   // past the table
        }
        return true;
    }
    return false;
}

uint32_t stage_kinds(const Bytes& ovl) {
    // the table starts with the kinds every stage has: 0 -> 8004D034, 1 -> 80054F50 (in the EXE)
    static const uint8_t sig[8] = {0x34, 0xD0, 0x04, 0x80, 0x50, 0x4F, 0x05, 0x80};
    for (size_t i = 0; i + 8 * 4 <= ovl.size(); i += 4) {
        if (memcmp(&ovl[i], sig, 8) != 0) continue;
        uint32_t m = 0;
        for (int k = 0; k < 32 && i + (size_t)k * 4 + 4 <= ovl.size(); k++) {
            const uint32_t a = le32(&ovl[i + (size_t)k * 4]);
            if (a >= 0x80010000u && a < 0x80200000u) m |= 1u << k;
            else if (a != 0) break;                      // past the table
        }
        return m;
    }
    return 0x7u;                                         // not found: what every stage has (0, 1 people, 2 raptors)
}

// 06.10.2026: where a new enemy or item goes - the start of the room's entry code (subroutine 0) before the first command
// that tests a flag or changes the flow: every entry of the room runs it, whatever the story (a copy next to its model
// inherited the model's branch - a raptor of a one-time scene gave a compsognathus that never came)
static size_t entry_insert_at(const std::vector<ScriptAtom>& atoms) {
    static const std::set<int> stop = {0x02, 0x04, 0x05, 0x09, 0x0A, 0x0B, 0x0C, 0x0E, 0x0F, 0x17, 0x18,   // flow
                                       0x26, 0x29, 0x52, 0x5C};                                          // conditions
    size_t first = atoms.size();
    for (size_t i = 0; i < atoms.size(); i++)
        if (atoms[i].kind == ScriptAtom::OP && atoms[i].sub == 0) { first = i; break; }
    for (size_t i = first; i < atoms.size(); i++) {
        const ScriptAtom& a = atoms[i];
        if (a.kind != ScriptAtom::OP) continue;
        if (a.sub != 0 || a.b.empty() || stop.count(a.b[0])) return i;
    }
    return atoms.size();
}

// 06.10.2026 (player: "let the item be turned, it stands the wrong way"; "about all three axes"): the item's model is a section
// of the room that the zone handler puts at the zone with its own turn; the script may then select the section (22 03 s) and
// set its fields (2A field, 36 place, 37 TURN rx ry rz). The turn goes there: the item's own TURN and 2A fields 6/7/8 get the
// new angles, or (none) a TURN is put after its own edits of the section (SELECT 3 s first if it has none)
static bool item_has_turn(const json& e) { return e.count("rx") || e.count("ry") || e.count("rz"); }
// 06.10.2026 (player: "the item lies on its side... its texture is not the one, it slid"): the zone handler lays every item
// with id < 11 on its side (Z 3072, 0x8001A638) - right for a weapon's own model (ST103, ST402, ST60D, ST610), but the common
// item box of CORE (group 80180000) then lies on its side too. Such a box gets a turn 0, 0, 0 unless one is given
bool item_core_upright(const json& e) {
    if (item_has_turn(e) || e.value("item", 0) >= 11) return false;
    uint32_t grp = 0;
    try { grp = (uint32_t)std::stoul(e.value("model_group", std::string("0")), nullptr, 16); } catch (...) { return false; }
    return (grp & 0x1FFFFFu) == 0x180000u;
}
static void item_turn(std::vector<ScriptAtom>& atoms, size_t ia, int sec, const json& e) {
    const int r[3] = {e.value("rx", 0) & 4095, e.value("ry", 0) & 4095, e.value("rz", 0) & 4095};
    const int sub = atoms[ia].sub;
    size_t at = ia + 1;
    bool sel = false, turned = false;
    for (size_t k = ia + 1; k < atoms.size(); k++) {
        ScriptAtom& s = atoms[k];
        if (s.kind != ScriptAtom::OP || s.sub != sub || s.b.size() < 4) break;
        if (s.b[0] == 0x22) { if (s.b[1] == 3 && s.b[2] == sec) { sel = true; at = k + 1; continue; } break; }
        if (!sel) break;
        if (s.b[0] == 0x37 && s.b.size() >= 8) {
            for (int q = 0; q < 3; q++) put16(&s.b[2 + q * 2], (uint16_t)(int16_t)r[q]);
            turned = true; at = k + 1; continue;
        }
        if (s.b[0] == 0x2A && s.b.size() >= 8 && s.b[2] >= 6 && s.b[2] <= 8) { put32(&s.b[4], (uint32_t)(int32_t)r[s.b[2] - 6]); at = k + 1; continue; }
        if (s.b[0] == 0x36 || s.b[0] == 0x2A) { at = k + 1; continue; }
        break;
    }
    if (turned) return;
    std::vector<ScriptAtom> add;
    ScriptAtom a;
    a.kind = ScriptAtom::OP;
    a.sub = sub;
    if (!sel) { a.b = {0x22, 0x03, (uint8_t)sec, 0x00}; add.push_back(a); }
    a.b = {0x37, 0x00, 0, 0, 0, 0, 0, 0};
    for (int q = 0; q < 3; q++) put16(&a.b[2 + q * 2], (uint16_t)(int16_t)r[q]);
    add.push_back(a);
    atoms.insert(atoms.begin() + (long)at, add.begin(), add.end());
}

bool room_placement_structure(const json& j, const char* what, std::vector<ScriptAtom>& atoms, PlacementCtx& ctx,
                              std::vector<std::string>& notes, std::string& err) {
    const std::string w = what;
    if (w != "enemies" && w != "items") return true;
    auto is_enemy = [](const ScriptAtom& a) { return a.kind == ScriptAtom::OP && a.b.size() >= 20 && (a.b[0] == 0x20 || a.b[0] == 0x59 || a.b[0] == 0x5B); };
    auto is_item = [](const ScriptAtom& a) { return a.kind == ScriptAtom::OP && a.b.size() >= 44 && a.b[0] == 0x28 && a.b[2] == 4; };
    auto find_old = [&](uint32_t off) {
        for (size_t i = 0; i < atoms.size(); i++) if (atoms[i].old == (int64_t)off) return (int)i;
        return -1;
    };
    try {
        if (!j.count(w)) return true;
        const json& arr = j.at(w);
        // убрать
        for (auto& e : arr) {
            if (!e.value("removed", false) || !e.count("offset")) continue;
            const uint32_t off = e["offset"].get<uint32_t>();
            const int i = find_old(off);
            if (i < 0) { notes.push_back(strfmt("%s: offset %u is not in the script (already removed?)", what, off)); continue; }
            if (w == "enemies" ? !is_enemy(atoms[i]) : !is_item(atoms[i])) throw std::runtime_error(strfmt("offset %u: not an %s record", off, w == "enemies" ? "enemy" : "item"));
            atoms.erase(atoms.begin() + i);
        }
        // 06.10.2026: the game's own items turned in the Studio
        if (w == "items")
            for (auto& e : arr) {
                if (!e.count("offset") || e.value("removed", false) || !(item_has_turn(e) || item_core_upright(e))) continue;
                const int i = find_old(e["offset"].get<uint32_t>());
                if (i < 0 || !is_item(atoms[i]) || atoms[i].b[4 + 30] == 0xFF) continue;
                item_turn(atoms, (size_t)i, atoms[i].b[4 + 30], e);
            }
        // занятое в комнате
        std::set<int> slots, zones, sections;
        for (auto& a : atoms) {
            if (a.kind != ScriptAtom::OP || a.b.size() < 4) continue;
            if (is_enemy(a)) slots.insert(a.b[1]);
            if (a.b[0] == 0x28) zones.insert(a.b[1]);
            if (a.b[0] == 0x23) sections.insert(a.b[1]);
            if (is_item(a)) sections.insert(a.b[4 + 30]);
        }
        auto first_free = [](const std::set<int>& used, int lo, int hi) {
            for (int i = lo; i <= hi; i++) if (!used.count(i)) return i;
            return -1;
        };
        std::map<uint32_t, int> added_after;   // образец → сколько копий уже встало за ним
        int added_entry = 0;                   // 06.10.2026: сколько уже встало в начало кода входа
        for (auto& e : arr) {
            if (!e.count("copy_of") || e.count("offset")) continue;
            const uint32_t src = e["copy_of"].get<uint32_t>();
            const int i = find_old(src);
            if (i < 0 || (w == "enemies" ? !is_enemy(atoms[i]) : !is_item(atoms[i])))
                throw std::runtime_error(strfmt("copy_of %u: no such %s in the script", src, w == "enemies" ? "enemy" : "item"));
            ScriptAtom a;
            a.kind = ScriptAtom::OP;
            a.b = atoms[i].b;
            a.sub = atoms[i].sub;
            // 06.10.2026: a new enemy is a plain ACTOR record (op 20, like the two raptors at the entry of ST401) even when its
            // model came from a roster (59) or follower (5B) record: kind, model and motions of that one, place, own flag
            if (w == "enemies" && e.value("op", std::string()) == "20" && a.b[0] != 0x20) {
                a.b.assign(24, 0);
                a.b[0] = 0x20;
                a.b[5] = 1;                                  // hunting, as the game's own placed raptors
            }
            uint8_t* p = a.b.data();
            if (w == "enemies") {
                if (p[0] == 0x20) fields_in(e, p, kOp20, NF(kOp20));
                else if (p[0] == 0x59) fields_in(e, p, kOp59, NF(kOp59));
                else { fields_in(e, p, kOp5B, NF(kOp5B)); put16(p + 0x22, 0); put16(p + 0x2A, 0); }
                if (!e.count("slot")) {
                    const int s = first_free(slots, 0, 9);
                    if (s < 0) throw std::runtime_error("no free actor slot: a room has 10 (0..9)");
                    p[1] = (uint8_t)s;
                }
                slots.insert(p[1]);
                // 06.10.2026: one-time like the game's own enemies - a free death flag (banks 4/5) unless one is given
                if (p[0] == 0x20 && p[4] == 0) {
                    if (!ctx.ready) throw std::runtime_error("internal: enemy flags not collected");
                    int bit = -1;
                    for (int b = 255; b >= 1 && bit < 0; b--) if (!ctx.used45.count(b)) bit = b;   // from the top: away from the game
                    if (bit < 0) throw std::runtime_error("no free death flag for the new enemy (banks 4/5)");
                    p[4] = (uint8_t)bit;
                }
                if (p[0] == 0x20) ctx.used45.insert(p[4]);
                notes.push_back(strfmt("enemy added: copy of the record at %u, slot %d, death flag %d", src, p[1], p[0] == 0x20 ? p[4] : 0));
            } else {
                uint8_t* rec = p + 4;
                if (e.count("zone")) quad_in(e["zone"], rec);
                if (e.count("flags")) rec[17] = (uint8_t)e["flags"].get<int>();
                if (e.count("floor")) rec[18] = (uint8_t)e["floor"].get<int>();
                fields_in(e, rec, kItem, NF(kItem));
                if (!e.count("hidden")) put32(rec + 36, 0);   // 06.10.2026: a new item is seen unless it is asked to be hidden
                if (le16(rec + 24) == 0xFF) throw std::runtime_error("a random item (0xFF) cannot be copied: set \"item\" to an item id");
                if (e.count("zone_index")) p[1] = (uint8_t)e["zone_index"].get<int>();
                else {
                    const int z = first_free(zones, 0, 27);
                    if (z < 0) throw std::runtime_error("no free zone: a room has 28 (0..27)");
                    p[1] = (uint8_t)z;
                }
                zones.insert(p[1]);
                if (!e.count("taken_flag")) {
                    ctx.used7.insert(le16(atoms[i].b.data() + 4 + 28));
                    int bit = -1;
                    for (int b = ctx.bits7 - 1; b >= 0 && bit < 0; b--) if (!ctx.used7.count(b)) bit = b;   // сверху: подальше от игры
                    if (bit < 0) throw std::runtime_error("no free bit in flag bank 7 for the new item");
                    put16(rec + 28, (uint16_t)bit);
                }
                ctx.used7.insert(le16(rec + 28));
                if (!e.count("model_section") && rec[30] != 0xFF) {
                    const int s = first_free(sections, 0, 31);
                    rec[30] = (uint8_t)(s < 0 ? 0xFF : s);
                }
                if (rec[30] != 0xFF) sections.insert(rec[30]);
                notes.push_back(strfmt("item added: copy of the record at %u, zone %d, taken flag 7/%d, model section %s", src, p[1],
                                       le16(rec + 28), rec[30] == 0xFF ? "none" : std::to_string(rec[30]).c_str()));
            }
            if (w == "items" || a.b[0] == 0x20) {
                // 06.10.2026: the start of the room's entry code (after what is already added there - the order is kept)
                a.sub = 0;
                const size_t pos = entry_insert_at(atoms);
                const int sec = w == "items" ? a.b[4 + 30] : 0xFF;
                atoms.insert(atoms.begin() + (long)pos, std::move(a));
                added_entry++;
                if (sec != 0xFF && (item_has_turn(e) || item_core_upright(e))) item_turn(atoms, pos, sec, e);   // turned: its section, right after it
            } else {
                const int k = added_after[src]++;   // roster (59) and follower (5B) records stay next to their model
                atoms.insert(atoms.begin() + i + 1 + k, std::move(a));
            }
        }
        if (added_entry) notes.push_back(strfmt("%d new record(s) at the start of the room's entry code", added_entry));
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- ростер этапа (оверлей STn.BIN по 0x800E8000)
static const uint32_t kOvlBase = 0x800E8000;

std::vector<uint32_t> roster_offsets(const Bytes& ovl, int stage) {
    std::vector<uint32_t> res;
    const ExeImage& x = exe();
    uint32_t entry = x.u32(0x8009DED0u + 24u * stage + 0x10u);
    if (entry < kOvlBase || entry - kOvlBase + 4 > ovl.size()) return res;
    std::map<int, uint32_t> regs;
    for (uint32_t k = (entry - kOvlBase) / 4, n = 0; n < 160 && (k + 1) * 4 <= ovl.size(); k++, n++) {
        uint32_t w = le32(&ovl[k * 4]);
        int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
        uint32_t imm = w & 0xFFFF;
        int32_t simm = (int16_t)imm;
        if (op == 0x0F) regs[rt] = imm << 16;
        else if (op == 0x09) {
            if (rs != 0 && regs.count(rs)) {
                uint32_t v = regs[rs] + (uint32_t)simm;
                if (simm == 0xF0 && regs[rs] >= kOvlBase) res.push_back(regs[rs] - kOvlBase);
                regs[rt] = v;
            } else if (rs == 0) regs[rt] = (uint32_t)simm;
        }
        if (w == 0x03E00008) break;
    }
    std::sort(res.begin(), res.end());
    res.erase(std::unique(res.begin(), res.end()), res.end());
    res.erase(std::remove_if(res.begin(), res.end(), [&](uint32_t o) { return o + 240 > ovl.size(); }), res.end());
    return res;
}

json roster_json(const Bytes& ovl, int stage) {
    json vars = json::array();
    for (uint32_t off : roster_offsets(ovl, stage)) {
        json dinos = json::array();
        for (int i = 0; i < 10; i++) {
            const uint8_t* b = &ovl[off + i * 24];
            dinos.push_back({{"index", b[0]}, {"room", room_code((stage << 8) | b[1])}, {"death_flag", b[2]}, {"state", b[3]},
                             {"x", (int)s16(b + 4)}, {"y", (int)s16(b + 6)}, {"z", (int)s16(b + 8)}, {"hp", (int)s16(b + 10)},
                             {"rotation", (int)s16(b + 12)}, {"ai_e", (int)s16(b + 14)}, {"ai_10", (int)s16(b + 16)},
                             {"ai_12", (int)s16(b + 18)}, {"ai_14", b[20]}, {"empty", !std::any_of(b + 1, b + 0x16, [](uint8_t v) { return v != 0; })}});
        }
        vars.push_back({{"offset", off}, {"dinosaurs", dinos}});
    }
    return {{"format", "dc-roster-1"}, {"stage", stage},
            {"about", "Dinosaurs of the stage at its start (10 records; a stage may have two variants chosen by the story). "
                      "room: where it is; op 59 in a room takes a live one from there. hp 0 = by kind. Coordinates: mm, Y down; rotation 4096 = 360."},
            {"variants", vars}};
}

bool apply_roster(const json& j, Bytes& ovl, int stage, std::string& err) {
    try {
        for (auto& v : j.at("variants")) {
            uint32_t off = v.at("offset");
            auto offs = roster_offsets(ovl, stage);
            if (std::find(offs.begin(), offs.end(), off) == offs.end()) throw std::runtime_error(strfmt("no roster at offset %u", off));
            const json& d = v.at("dinosaurs");
            for (size_t i = 0; i < d.size() && i < 10; i++) {
                uint8_t* b = &ovl[off + i * 24];
                const json& e = d[i];
                if (e.value("empty", false)) continue;
                if (e.count("room")) b[1] = (uint8_t)(parse_room(e["room"].get<std::string>()) & 0xFF);
                if (e.count("death_flag")) b[2] = (uint8_t)e["death_flag"].get<int>();
                if (e.count("state")) b[3] = (uint8_t)e["state"].get<int>();
                const char* f16[] = {"x", "y", "z", "hp", "rotation", "ai_e", "ai_10", "ai_12"};
                for (int k = 0; k < 8; k++)
                    if (e.count(f16[k])) put16(b + 4 + k * 2, (uint16_t)(int16_t)e[f16[k]].get<int>());
                if (e.count("ai_14")) b[20] = (uint8_t)e["ai_14"].get<int>();
            }
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- EXE
struct Pool { const char* name; uint32_t table, pool, end; int n; };
static const Pool kPools[2] = {{"main", 0x8009E7F0, 0x8009EF1C, 0x8009EF1C + 0x684E, 459},
                               {"system", 0x800983B8, 0x8009844C, 0x8009844C + 0x9F6, 37}};

// строка пула: коды до кода конца включительно
static std::vector<uint16_t> pool_string(const Pool& p, int i, int* cap = nullptr) {
    const ExeImage& x = exe();
    std::vector<uint16_t> out;
    uint32_t a = p.pool + 2 * x.u32(p.table + 4u * i);
    uint32_t nx = i + 1 < p.n ? p.pool + 2 * x.u32(p.table + 4u * (i + 1)) : p.end;
    if (cap) *cap = nx > a ? (int)((nx - a) / 2) : 0;
    for (uint32_t q = a; q + 2 <= p.end && out.size() < 2048; q += 2) {
        const uint8_t* b = x.ptr(q);
        if (!b) break;
        uint16_t c = le16(b);
        out.push_back(c);
        if ((c & 0x8000) && (c & 0x2800)) break;   // символ с меткой конца
    }
    return out;
}

std::string item_name(int id) {
    const ExeImage& x = exe();
    if (!x.ok() || id < 0 || id >= 0x70) return "";
    const uint8_t* r = x.ptr(0x800A576Cu + 12u * id);
    if (!r) return "";
    int ti = le16(r + 4);
    if (ti >= kPools[0].n) return "";
    auto s = pool_string(kPools[0], ti);
    std::string t = decode_text(s.data(), s.size());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
    return t;
}

json exe_door_scenes_json() {
    const ExeImage& x = exe();
    json arr = json::array();
    static const std::pair<uint32_t, const char*> kHandlers[] = {
        {0x8007BB08, "push"}, {0x8007B68C, "pull"}, {0x8007B22C, "slide left"}, {0x8007B254, "slide right"}, {0x8007C520, "open"},
        {0x8007C998, "close"}, {0x8007D060, "up"}, {0x8007D088, "down"}, {0x8007D0B0, "up 2"}, {0x8007D0D8, "down 2"},
        {0x8007ABD8, "stairs up"}, {0x8007ADD8, "stairs down"}, {0x8007D8EC, "ladder up"}, {0x8007D964, "ladder down"}};
    for (int i = 0; i < 160 && x.ok(); i++) {
        const uint8_t* r = x.ptr(0x800A87D0u + 40u * i);
        if (!r) break;
        uint32_t h = le32(r + 8);
        std::string hn;
        for (auto& kv : kHandlers) if (kv.first == h) hn = kv.second;
        char nm[9] = {};
        memcpy(nm, r + 0x20, 8);
        for (auto& ch : nm) if (ch && (ch < 32 || ch > 126)) ch = '?';
        const uint8_t* fp = x.ptr(0x800AA0D0u + 2u * i);
        arr.push_back({{"index", i}, {"name", std::string(nm)}, {"model_a", strfmt("%08X", le32(r))}, {"model_b", strfmt("%08X", le32(r + 4))},
                       {"handler", strfmt("%08X", h)}, {"handler_name", hn}, {"model_b_x", (int)s16(r + 0x0C)},
                       {"camera_target", {(int)s16(r + 0x0E), (int)s16(r + 0x10), (int)s16(r + 0x12)}},
                       {"camera_eye", {(int)s16(r + 0x14), (int)s16(r + 0x16), (int)s16(r + 0x18)}},
                       {"regina", {(int)s16(r + 0x1A), (int)s16(r + 0x1C), (int)s16(r + 0x1E)}}, {"file", fp ? le16(fp) : 0}});
    }
    return {{"format", "dc-door-scenes-1"},
            {"about", "Door, stairs and lift scenes (index = scene in rooms\\STxxx\\doors.json). camera_eye looks at camera_target; "
                      "regina: her place in the scene; file: scene file number (DOORnn, WIREnnn...)."},
            {"scenes", arr}};
}

json exe_items_json() {
    const ExeImage& x = exe();
    json arr = json::array();
    for (int i = 0; i < 0x70 && x.ok(); i++) {
        const uint8_t* r = x.ptr(0x800A576Cu + 12u * i);
        if (!r) break;
        arr.push_back({{"id", i}, {"name", item_name(i)}, {"a", (int)le16(r)}, {"b", r[2]}, {"class", r[3]}, {"name_text", (int)le16(r + 4)},
                       {"short_text", (int)le16(r + 6)}, {"description_text", (int)le16(r + 8)}});
    }
    return {{"format", "dc-items-table-1"},
            {"about", "Item table: name_text, short_text, description_text are indices of the main pool in texts.json."}, {"items", arr}};
}

json exe_texts_json() {
    json pools = json::array();
    for (auto& p : kPools) {
        json arr = json::array();
        for (int i = 0; i < p.n; i++) {
            int cap = 0;
            auto s = pool_string(p, i, &cap);
            arr.push_back({{"index", i}, {"text", decode_text(s.data(), s.size())}});
        }
        pools.push_back({{"pool", p.name}, {"texts", arr}});
    }
    return {{"format", "dc-exe-texts-1"},
            {"about", "Texts of the game program: item names and descriptions, messages. \\n = new line, {#N} = game glyph N, {XXXX} = control "
                      "code. A pool is repacked when converting: texts may grow while the whole pool fits."},
            {"pools", pools}};
}

static bool exe_put(Bytes& f, uint32_t a, const void* d, size_t n) {
    const ExeImage& x = exe();
    if (!x.has(a, (uint32_t)n)) return false;
    memcpy(&f[0x800 + (a - x.addr)], d, n);
    return true;
}

bool apply_exe(const json& j, const char* what, Bytes& f, std::string& err) {
    const ExeImage& x = exe();
    if (!x.ok() || f.size() != x.data.size()) { err = "the game program is not available"; return false; }
    try {
        const std::string w = what;
        if (w == "door_scenes") {
            for (auto& e : j.at("scenes")) {
                int i = e.at("index");
                if (i < 0 || i >= 160) throw std::runtime_error(strfmt("scene %d: no such scene", i));
                uint8_t r[40];
                memcpy(r, &f[0x800 + (0x800A87D0u + 40u * i - x.addr)], 40);
                auto hx = [](const json& v) { return (uint32_t)strtoul(v.get<std::string>().c_str(), nullptr, 16); };
                if (e.count("model_a")) put32(r, hx(e["model_a"]));
                if (e.count("model_b")) put32(r + 4, hx(e["model_b"]));
                if (e.count("handler")) put32(r + 8, hx(e["handler"]));
                if (e.count("model_b_x")) put16(r + 0x0C, (uint16_t)(int16_t)e["model_b_x"].get<int>());
                auto xyz = [&](const char* k, int o) {
                    if (e.count(k)) for (int q = 0; q < 3; q++) put16(r + o + q * 2, (uint16_t)(int16_t)e[k].at(q).get<int>());
                };
                xyz("camera_target", 0x0E);
                xyz("camera_eye", 0x14);
                xyz("regina", 0x1A);
                if (e.count("name")) {
                    std::string n = e["name"];
                    memset(r + 0x20, 0, 8);
                    memcpy(r + 0x20, n.data(), std::min<size_t>(8, n.size()));
                }
                exe_put(f, 0x800A87D0u + 40u * i, r, 40);
                if (e.count("file")) {
                    uint8_t fv[2];
                    put16(fv, (uint16_t)e["file"].get<int>());
                    exe_put(f, 0x800AA0D0u + 2u * i, fv, 2);
                }
            }
        } else if (w == "items") {
            for (auto& e : j.at("items")) {
                int i = e.at("id");
                if (i < 0 || i >= 0x70) throw std::runtime_error(strfmt("item %d: no such item", i));
                uint8_t r[12];
                memcpy(r, &f[0x800 + (0x800A576Cu + 12u * i - x.addr)], 12);
                if (e.count("a")) put16(r, (uint16_t)e["a"].get<int>());
                if (e.count("b")) r[2] = (uint8_t)e["b"].get<int>();
                if (e.count("class")) r[3] = (uint8_t)e["class"].get<int>();
                if (e.count("name_text")) put16(r + 4, (uint16_t)e["name_text"].get<int>());
                if (e.count("short_text")) put16(r + 6, (uint16_t)e["short_text"].get<int>());
                if (e.count("description_text")) put16(r + 8, (uint16_t)e["description_text"].get<int>());
                exe_put(f, 0x800A576Cu + 12u * i, r, 12);
            }
        } else if (w == "texts") {
            for (auto& pj : j.at("pools")) {
                std::string pn = pj.at("pool");
                const Pool* p = nullptr;
                for (auto& q : kPools) if (pn == q.name) p = &q;
                if (!p) throw std::runtime_error("no pool " + pn);
                std::vector<std::vector<uint16_t>> all(p->n);
                for (int i = 0; i < p->n; i++) all[i] = pool_string(*p, i);
                bool changed = false;
                for (auto& t : pj.at("texts")) {
                    int i = t.at("index");
                    if (i < 0 || i >= p->n) throw std::runtime_error(strfmt("%s text %d: no such text", p->name, i));
                    std::vector<uint16_t> codes;
                    std::string e;
                    if (!encode_text(t.at("text").get<std::string>(), codes, e)) throw std::runtime_error(strfmt("%s text %d: %s", p->name, i, e.c_str()));
                    if (codes != all[i]) { all[i] = codes; changed = true; }
                }
                if (!changed) continue;   // пул как был — байты не трогаем
                // переупаковка пула: строки подряд, таблица смещений (в u16 от начала пула)
                Bytes pool, tab(4u * p->n);
                for (int i = 0; i < p->n; i++) {
                    put32(&tab[4 * i], (uint32_t)(pool.size() / 2));
                    for (uint16_t c : all[i]) { pool.push_back((uint8_t)c); pool.push_back((uint8_t)(c >> 8)); }
                }
                if (pool.size() > p->end - p->pool)
                    throw std::runtime_error(strfmt("%s texts are %u bytes, the game has room for %u", p->name, (unsigned)pool.size(), p->end - p->pool));
                // остаток пула — нули (пул — только строки; за его концом у игры другие данные)
                Bytes all_pool(p->end - p->pool, 0);
                memcpy(all_pool.data(), pool.data(), pool.size());
                exe_put(f, p->pool, all_pool.data(), all_pool.size());
                exe_put(f, p->table, tab.data(), tab.size());
            }
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

}  // namespace dc
