#include "pieces.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace dc {

void piece_rot(int rx, int ry, int rz, float M[9]) {
    const float K = 6.28318530718f / 4096.f;
    const float cx = cosf(rx * K), sx = sinf(rx * K), cy = cosf(ry * K), sy = sinf(ry * K), cz = cosf(rz * K), sz = sinf(rz * K);
    M[0] = cy * cz;                M[1] = -cy * sz;               M[2] = sy;
    M[3] = cx * sz + sx * sy * cz; M[4] = cx * cz - sx * sy * sz; M[5] = -sx * cy;
    M[6] = sx * sz - cx * sy * cz; M[7] = sx * cz + cx * sy * sz; M[8] = cx * cy;
}

static const MeshPart* room_part(const FileContent& c) {
    for (auto& m : c.meshes)
        if (m.kind == "room" && m.block >= 0) return &m;
    return nullptr;
}

// one command line of script.txt: "@XXXX b0 b1 ...   ; comment" -> offset and bytes (references ->@ $@ are not bytes)
static bool parse_cmd(const std::string& line, uint32_t& at, std::vector<uint8_t>& b) {
    size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos || line[p] != '@') return false;
    const size_t semi = line.find(';', p);
    std::istringstream in(line.substr(p + 1, semi == std::string::npos ? std::string::npos : semi - p - 1));
    std::string tok;
    if (!(in >> tok)) return false;
    char* e = nullptr;
    at = (uint32_t)strtoul(tok.c_str(), &e, 16);
    if (*e) return false;
    b.clear();
    while (in >> tok) {
        if (tok.size() != 2 || !isxdigit((unsigned char)tok[0]) || !isxdigit((unsigned char)tok[1])) return false;   // reference
        b.push_back((uint8_t)strtoul(tok.c_str(), nullptr, 16));
    }
    return true;
}

json room_pieces(const FileContent& c, const std::string& text) {
    json out = json::array();
    const MeshPart* mp = room_part(c);
    std::istringstream in(text);
    std::string line;
    int sub = -1;
    std::set<uint32_t> seen;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        if (line.compare(p, 4, "sub ") == 0) { sub = atoi(line.c_str() + p + 4); continue; }
        uint32_t at = 0;
        std::vector<uint8_t> b;
        if (!parse_cmd(line, at, b) || b.size() < 24 || b[0] != 0x23 || !seen.insert(at).second) continue;
        auto s16v = [&](int o) { return (int)(int16_t)(b[o] | (b[o + 1] << 8)); };
        const uint32_t grp = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
        int gi = -1;
        if (mp)
            for (size_t k = 0; k < mp->hdrs.size(); k++)
                if (((mp->block_dest + mp->hdrs[k]) & 0x1FFFFFu) == (grp & 0x1FFFFFu)) { gi = (int)k; break; }
        json e = {{"at", at}, {"sub", sub}, {"slot", b[1]}, {"anim", b[2]}, {"group", strfmt("%08X", grp)}, {"gi", gi},
                  {"x", s16v(12)}, {"y", s16v(14)}, {"z", s16v(16)}, {"rx", s16v(18)}, {"ry", s16v(20)}, {"rz", s16v(22)}};
        e["placed"] = s16v(12) || s16v(14) || s16v(16) || s16v(18) || s16v(20) || s16v(22);
        out.push_back(e);
    }
    // the same piece at the same place from several subroutines (one per camera angle) - one entry to edit: "lead" is the
    // first such command, an edit of the lead goes to all of them
    for (size_t i = 0; i < out.size(); i++) {
        int lead = (int)i;
        for (size_t j = 0; j < i; j++) {
            const json &a = out[i], &b = out[j];
            if (a["gi"] == b["gi"] && a["group"] == b["group"] && a["anim"] == b["anim"] && a["x"] == b["x"] && a["y"] == b["y"] &&
                a["z"] == b["z"] && a["rx"] == b["rx"] && a["ry"] == b["ry"] && a["rz"] == b["rz"]) { lead = (int)j; break; }
        }
        out[i]["lead"] = lead;
    }
    return out;
}

// every "@XXXX ..." line of the script text in order: subroutine, offset, bytes (a reference token stops the bytes;
// "ref" tells such lines apart, they are never section edits)
struct SLine { int sub; uint32_t at; std::vector<uint8_t> b; bool ref; };
static std::vector<SLine> script_lines(const std::string& text) {
    std::vector<SLine> out;
    std::istringstream in(text);
    std::string line;
    int sub = -1;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        if (line.compare(p, 4, "sub ") == 0) { sub = atoi(line.c_str() + p + 4); continue; }
        if (line[p] != '@') continue;
        const size_t semi = line.find(';', p);
        std::istringstream ls(line.substr(p + 1, semi == std::string::npos ? std::string::npos : semi - p - 1));
        std::string tok;
        if (!(ls >> tok)) continue;
        char* e = nullptr;
        SLine s{sub, (uint32_t)strtoul(tok.c_str(), &e, 16), {}, false};
        if (*e) continue;
        while (ls >> tok) {
            if (tok.size() != 2 || !isxdigit((unsigned char)tok[0]) || !isxdigit((unsigned char)tok[1])) { s.ref = true; break; }
            s.b.push_back((uint8_t)strtoul(tok.c_str(), nullptr, 16));
        }
        out.push_back(std::move(s));
    }
    return out;
}

json room_item_models(const FileContent& c, const std::string& text, const json& items) {
    json out = json::array();
    const MeshPart* mp = room_part(c);
    if (!mp || !items.is_array()) return out;
    const std::vector<SLine> L = script_lines(text);
    std::map<uint32_t, size_t> by_at;
    for (size_t i = 0; i < L.size(); i++) by_at.emplace(L[i].at, i);
    auto gi_of = [&](uint32_t grp) {
        for (size_t k = 0; k < mp->hdrs.size(); k++)
            if (((mp->block_dest + mp->hdrs[k]) & 0x1FFFFFu) == (grp & 0x1FFFFFu)) return (int)k;
        return -1;
    };
    auto s16b = [](const std::vector<uint8_t>& b, size_t o) { return (int)(int16_t)(b[o] | (b[o + 1] << 8)); };
    auto mid = [](int c0, int c2) { return c2 + (c0 - c2) / 2; };   // the game: c2 + (c0 - c2) / 2, toward zero
    std::map<int, int> sec_of_offset;                                 // original records: offset -> model section
    for (auto& e : items)
        if (e.count("offset")) sec_of_offset[e["offset"].get<int>()] = e.value("model_section", 255);
    int index = -1;
    for (auto& e : items) {
        index++;
        if (e.value("removed", false) || !e.count("zone") || e["zone"].size() != 4) continue;
        int sec = e.value("model_section", -1);
        if (sec < 0 && e.count("copy_of")) {                         // a copy gets a section when saved if its model has one
            auto s = sec_of_offset.find(e["copy_of"].get<int>());
            sec = s == sec_of_offset.end() ? 255 : (s->second == 255 ? 255 : 1000);
        }
        if (sec < 0 || sec == 255) continue;
        uint32_t grp = 0, hidden = 0;
        try {
            grp = (uint32_t)std::stoul(e.value("model_group", std::string("0")), nullptr, 16);
            hidden = (uint32_t)std::stoul(e.value("hidden", std::string("0")), nullptr, 16);
        } catch (...) { continue; }
        if (hidden) continue;                                         // a hiding place: found by searching, no model
        const int gi = gi_of(grp);
        // 06.10.2026: the common item box of CORE (80180000) is not in the room file - the caller draws it (core_box)
        const bool core_box = gi < 0 && (grp & 0x1FFFFFu) == 0x180000u;
        if (gi < 0 && !core_box) continue;
        const json& q = e["zone"];
        const int x = mid(q[0][0].get<int>(), q[2][0].get<int>()), z = mid(q[0][1].get<int>(), q[2][1].get<int>());
        const int id = e.value("item", 0);
        // the game lays a weapon (id < 11) on its side: its own model in its room lies so. The common box stands - the
        // Studio writes it a turn (placement.cpp item_core_upright; player: "it lies on its side, its texture slid")
        int v[6] = {x, 0, z, 0, 0, id < 11 && !core_box ? 3072 : 0};
        // the script's own edits of this section right after the item (original records only: a copy has a new section)
        if (e.count("offset")) {
            auto it = by_at.find(e["offset"].get<uint32_t>());
            if (it != by_at.end() && L[it->second].b.size() >= 16) {
                const SLine& il = L[it->second];
                const int ox = mid(s16b(il.b, 4), s16b(il.b, 12)), oz = mid(s16b(il.b, 6), s16b(il.b, 14));
                const int dx = x - ox, dz = z - oz;                   // the zone moved here and not saved yet: as the save will do
                bool sel = false;
                for (size_t k = it->second + 1; k < L.size() && L[k].sub == il.sub; k++) {
                    const SLine& s = L[k];
                    if (s.ref || s.b.empty()) break;
                    if (s.b[0] == 0x22 && s.b.size() >= 4) { if (s.b[1] == 3 && s.b[2] == sec) { sel = true; continue; } break; }
                    if (!sel) break;
                    if (s.b[0] == 0x36 && s.b.size() >= 8) { v[0] = s16b(s.b, 2) + dx; v[1] = s16b(s.b, 4); v[2] = s16b(s.b, 6) + dz; continue; }
                    if (s.b[0] == 0x37 && s.b.size() >= 8) { v[3] = s16b(s.b, 2); v[4] = s16b(s.b, 4); v[5] = s16b(s.b, 6); continue; }
                    if (s.b[0] == 0x2A && s.b.size() >= 8) {
                        const int f = s.b[2];
                        const int val = (int)(int32_t)(s.b[4] | (s.b[5] << 8) | (s.b[6] << 16) | ((uint32_t)s.b[7] << 24));
                        if (f >= 3 && f <= 8) v[f - 3] = (int)(int16_t)(val + (f == 3 ? dx : f == 5 ? dz : 0));
                        continue;
                    }
                    break;
                }
            }
        }
        // 06.10.2026 (player: "let the item be turned - about all three axes"): its turn, written into the script on save
        if (e.count("rx") && e["rx"].is_number()) v[3] = e["rx"].get<int>() & 4095;
        if (e.count("ry") && e["ry"].is_number()) v[4] = e["ry"].get<int>() & 4095;
        if (e.count("rz") && e["rz"].is_number()) v[5] = e["rz"].get<int>() & 4095;
        out.push_back({{"item", true}, {"gi", gi}, {"group", strfmt("%08X", grp)}, {"slot", sec}, {"anim", 0}, {"placed", true},
                       {"x", v[0]}, {"y", v[1]}, {"z", v[2]}, {"rx", v[3]}, {"ry", v[4]}, {"rz", v[5]}, {"core_box", core_box},
                       {"index", index}});
    }
    return out;
}

std::vector<MTri> room_tris_placed(const FileContent& c, const json& pieces) {
    const MeshPart* mp = room_part(c);
    if (!mp) return c.meshes.empty() ? std::vector<MTri>() : c.meshes[0].tris;
    std::set<int> named, body;
    std::map<int, std::vector<std::array<int, 6>>> at;   // group -> distinct places (x, y, z, rx, ry, rz)
    std::set<int> item_gi;                               // an item model: the item puts its group, a bare CREATE of it does not
    for (auto& p : pieces)
        if (p.value("item", false)) item_gi.insert(p.value("gi", -1));
    for (auto& p : pieces) {
        const int gi = p.value("gi", -1);
        if (gi < 0) continue;
        named.insert(gi);
        if (!p.value("item", false) && item_gi.count(gi) && !p.value("placed", false)) continue;
        if (p.value("anim", 0) != 0) continue;            // animated (cutscene) - the animation moves it, not shown at rest
        if (!p.value("placed", false)) { body.insert(gi); continue; }
        const std::array<int, 6> v = {p.value("x", 0), p.value("y", 0), p.value("z", 0), p.value("rx", 0), p.value("ry", 0), p.value("rz", 0)};
        auto& l = at[gi];
        if (std::find(l.begin(), l.end(), v) == l.end()) l.push_back(v);
    }
    std::map<int, std::vector<std::array<float, 12>>> xf;   // group -> M (9) + place (3)
    for (auto& kv : at)
        for (auto& v : kv.second) {
            std::array<float, 12> m{};
            piece_rot(v[3], v[4], v[5], m.data());
            m[9] = (float)v[0]; m[10] = (float)v[1]; m[11] = (float)v[2];
            xf[kv.first].push_back(m);
        }
    auto clamp16 = [](float f) { return (int16_t)std::max(-32768.0f, std::min(32767.0f, roundf(f))); };
    std::vector<MTri> out;
    out.reserve(mp->tris.size());
    for (const MTri& t : mp->tris) {
        const int g = t.group;
        if (!named.count(g) || body.count(g)) out.push_back(t);
        auto it = xf.find(g);
        if (it == xf.end()) continue;
        for (auto& m : it->second) {
            MTri u = t;
            for (int k = 0; k < 3; k++) {
                const float x = t.v[k].x, y = t.v[k].y, z = t.v[k].z;
                u.v[k].x = clamp16(m[9] + m[0] * x + m[1] * y + m[2] * z);
                u.v[k].y = clamp16(m[10] + m[3] * x + m[4] * y + m[5] * z);
                u.v[k].z = clamp16(m[11] + m[6] * x + m[7] * y + m[8] * z);
            }
            out.push_back(u);
        }
    }
    return out;
}

bool pieces_to_script(const json& pieces, std::string& text, std::string& err) {
    std::map<uint32_t, const json*> by;
    for (auto& p : pieces) by[(uint32_t)p.value("at", 0)] = &p;
    std::istringstream in(text);
    std::string line, out;
    std::set<uint32_t> done;
    bool crlf = text.find("\r\n") != std::string::npos;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        uint32_t at = 0;
        std::vector<uint8_t> b;
        auto it = by.end();
        if (parse_cmd(line, at, b) && b.size() >= 24 && b[0] == 0x23 && (it = by.find(at)) != by.end()) {
            const json& p = *it->second;
            const int v[6] = {p.value("x", 0), p.value("y", 0), p.value("z", 0), p.value("rx", 0), p.value("ry", 0), p.value("rz", 0)};
            for (int k = 0; k < 6; k++) {
                const int16_t s = (int16_t)std::max(-32768, std::min(32767, v[k]));
                b[12 + k * 2] = (uint8_t)(s & 0xFF);
                b[13 + k * 2] = (uint8_t)((uint16_t)s >> 8);
            }
            const size_t semi = line.find(';');
            const std::string comment = semi == std::string::npos ? std::string() : "   " + line.substr(semi);
            std::string nl = strfmt("@%04X ", at);
            for (uint8_t x : b) nl += strfmt(" %02X", x);
            line = nl + comment;
            done.insert(at);
        }
        out += line + (crlf ? "\r\n" : "\n");
    }
    for (auto& kv : by)
        if (!done.count(kv.first)) { err = strfmt("the command @%04X is not in the script any more", kv.first); return false; }
    text = out;
    return true;
}

}  // namespace dc
