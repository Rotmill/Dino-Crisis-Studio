// Блок комнаты (тип 7 → 0x80100000): 9 указателей — ptr[0] свет/туман, ptr[1] навигация, ptr[2] коллизия, ptr[3] камеры,
// ptr[4] константа 40 Б, ptr[5] скрипты и тексты, ptr[6] триггер-зоны, ptr[7]/ptr[8] — треугольники/квады группы 0.
// Раскладка секций — func_80055494 игры; форматы — разведка 10.08.2026 (dc1_formats_cracked).
#include "core.h"

#include <algorithm>
#include <cstring>

namespace dc {

// ---------------------------------------------------------------- тексты
// u16 = 0x8000 | (индекс*2); 0xA000 — конец, 0xC000 — новая строка; прочие — управляющие, пишем {XXXX}.
static const char* glyph_of(int idx) {
    static char buf[2];
    if (idx == 0) return " ";
    if (idx >= 1 && idx <= 26) { buf[0] = (char)('A' + idx - 1); buf[1] = 0; return buf; }
    if (idx >= 27 && idx <= 52) { buf[0] = (char)('a' + idx - 27); buf[1] = 0; return buf; }
    if (idx >= 53 && idx <= 62) { buf[0] = (char)('0' + idx - 53); buf[1] = 0; return buf; }
    switch (idx) {
    case 64: return "!";   // 14.10.2026: 63 is the ellipsis glyph (the game drew "Studio!" as "Studio..."); "hatch{#64}" ends ST50x text
    case 65: return "?";
    case 67: return "\"";
    case 68: return ":";
    case 72: return "-";
    case 77: return ".";
    case 78: return ",";
    case 79: return "'";
    default: return nullptr;
    }
}

std::string decode_text(const uint16_t* c, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        uint16_t w = c[i];
        if (w == 0xA000) break;
        if (w == 0xC000) { s += "\n"; continue; }
        if ((w & 0x8000) && !(w & 1) && w < 0x8100) {
            int idx = (w & 0x7FFF) >> 1;
            if (const char* g = glyph_of(idx)) {
                if (*g == '{' || *g == '}') s += strfmt("{#%d}", idx);
                else s += g;
            } else {
                s += strfmt("{#%d}", idx);
            }
            continue;
        }
        s += strfmt("{%04X}", w);
    }
    return s;
}

bool encode_text(const std::string& s, std::vector<uint16_t>& out, std::string& err) {
    out.clear();
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        if (ch == '\r') continue;
        if (ch == '\n') { out.push_back(0xC000); continue; }
        if (ch == '{') {
            size_t e = s.find('}', i);
            if (e == std::string::npos) { err = "'{' without '}'"; return false; }
            std::string t = s.substr(i + 1, e - i - 1);
            if (!t.empty() && t[0] == '#') out.push_back((uint16_t)(0x8000 | (atoi(t.c_str() + 1) * 2)));
            else out.push_back((uint16_t)strtoul(t.c_str(), nullptr, 16));
            i = e;
            continue;
        }
        int found = -1;
        for (int idx = 0; idx < 128 && found < 0; idx++) {
            const char* g = glyph_of(idx);
            if (g && g[0] == ch && g[1] == 0) found = idx;
        }
        if (found < 0) { err = strfmt("character '%c' is not in the game font", ch); return false; }
        out.push_back((uint16_t)(0x8000 | (found * 2)));
    }
    out.push_back(0xA000);
    return true;
}

static bool is_text_code(uint16_t w) { return (w & 0x8000) && !(w & 1) && w < 0x8100; }
static bool is_ctrl_code(uint16_t w) { return w == 0xC000 || w == 0x0100 || w == 0x0800 || w == 0x0802 || (w & 0xFF00) == 0x1000; }

std::vector<GameString> scan_strings(const Bytes& blk, uint32_t from, uint32_t to) {
    std::vector<GameString> out;
    to = std::min<uint32_t>(to, (uint32_t)blk.size());
    uint32_t p = (from + 1) & ~1u;
    while (p + 2 <= to) {
        uint16_t w = le16(&blk[p]);
        if (!is_text_code(w)) { p += 2; continue; }
        // строка: коды текста и управляющие, конец 0xA000, хотя бы 2 кода текста
        uint32_t q = p;
        int ntext = 0;
        bool ended = false;
        std::vector<uint16_t> codes;
        while (q + 2 <= to) {
            uint16_t c = le16(&blk[q]);
            if (c == 0xA000) { ended = true; codes.push_back(c); q += 2; break; }
            if (is_text_code(c)) ntext++;
            else if (!is_ctrl_code(c)) break;
            codes.push_back(c);
            q += 2;
        }
        if (ended && ntext >= 2) {
            GameString gs;
            gs.off = p;
            gs.codes = codes;
            gs.text = decode_text(codes.data(), codes.size());
            out.push_back(gs);
            p = q;
        } else {
            p += 2;
        }
    }
    return out;
}

// ---------------------------------------------------------------- коллизия
bool parse_collision(const Bytes& blk, uint32_t base, uint32_t off, uint32_t limit, Collision& c) {
    (void)base;
    c = Collision();
    if (off + 272 > blk.size() || off + 272 > limit) return false;
    c.off = off;
    c.floors.resize(17);
    c.walls.resize(17);
    uint32_t end = off + 272;
    for (int r = 0; r < 17; r++) {
        const uint8_t* p = &blk[off + r * 16];
        CollRow& row = c.rows[r];
        row.na = p[0]; row.nb = p[1]; row.pad = le16(p + 2);
        row.offa = le32(p + 4); row.offb = le32(p + 8); row.spare = le32(p + 12);
        for (int i = 0; i < row.na; i++) {
            uint32_t o = off + 272 + row.offa + i * 20u;
            if (o + 20 > limit) return false;
            FloorRec f;
            f.type = blk[o]; f.flags = blk[o + 1]; f.pad = le16(&blk[o + 2]);
            f.x = s16(&blk[o + 4]); f.z = s16(&blk[o + 6]); f.sx = s16(&blk[o + 8]); f.sz = s16(&blk[o + 10]);
            for (int k = 0; k < 4; k++) f.h[k] = s16(&blk[o + 12 + k * 2]);
            c.floors[r].push_back(f);
            end = std::max(end, o + 20);
        }
        for (int i = 0; i < row.nb; i++) {
            uint32_t o = off + 272 + row.offb + i * 16u;
            if (o + 16 > limit) return false;
            WallRec w;
            w.type = blk[o]; w.flag = blk[o + 1]; w.lvl_lo = blk[o + 2]; w.lvl_hi = blk[o + 3];
            w.x = s16(&blk[o + 4]); w.z = s16(&blk[o + 6]); w.sx = s16(&blk[o + 8]); w.sz = s16(&blk[o + 10]);
            w.attr = le16(&blk[o + 12]); w.spare = s16(&blk[o + 14]);
            c.walls[r].push_back(w);
            end = std::max(end, o + 16);
        }
    }
    c.size = end - off;
    c.ok = true;
    return true;
}

// Раскладка как у игры: сначала все полы строк 0..16 подряд, затем все стены строк 0..16 подряд.
Bytes build_collision(const Collision& c) {
    uint32_t nf = 0, nw = 0;
    for (int r = 0; r < 17; r++) { nf += (uint32_t)c.floors[r].size(); nw += (uint32_t)c.walls[r].size(); }
    Bytes out(272 + nf * 20 + nw * 16, 0);
    uint32_t fa = 0, wb = nf * 20;
    for (int r = 0; r < 17; r++) {
        uint8_t* p = &out[r * 16];
        p[0] = (uint8_t)c.floors[r].size();
        p[1] = (uint8_t)c.walls[r].size();
        put16(p + 2, c.rows[r].pad);
        put32(p + 4, fa);
        put32(p + 8, wb);
        put32(p + 12, c.rows[r].spare);
        for (auto& f : c.floors[r]) {
            uint8_t* q = &out[272 + fa];
            q[0] = f.type; q[1] = f.flags; put16(q + 2, f.pad);
            put16(q + 4, (uint16_t)f.x); put16(q + 6, (uint16_t)f.z); put16(q + 8, (uint16_t)f.sx); put16(q + 10, (uint16_t)f.sz);
            for (int k = 0; k < 4; k++) put16(q + 12 + k * 2, (uint16_t)f.h[k]);
            fa += 20;
        }
        for (auto& w : c.walls[r]) {
            uint8_t* q = &out[272 + wb];
            q[0] = w.type; q[1] = w.flag; q[2] = w.lvl_lo; q[3] = w.lvl_hi;
            put16(q + 4, (uint16_t)w.x); put16(q + 6, (uint16_t)w.z); put16(q + 8, (uint16_t)w.sx); put16(q + 10, (uint16_t)w.sz);
            put16(q + 12, w.attr); put16(q + 14, (uint16_t)w.spare);
            wb += 16;
        }
    }
    return out;
}

// ---------------------------------------------------------------- комната
bool parse_room(const Bytes& blk, uint32_t base, RoomInfo& r) {
    r = RoomInfo();
    r.groups = room_groups(blk, base);
    if (r.groups.empty()) return false;
    for (int i = 0; i < 9; i++) r.ptr[i] = le32(&blk[i * 4]);
    r.mesh_end = groups_end(r.groups);
    auto in = [&](uint32_t p) { return p >= base && p < base + blk.size(); };
    // границы секций — по отсортированным указателям
    std::vector<uint32_t> starts;
    for (int i = 0; i < 7; i++) if (in(r.ptr[i])) starts.push_back(r.ptr[i] - base);
    // движения, переложенные конвертером в конец блока (за последней секцией), — тоже граница: скрипт на них не заходит
    {
        uint32_t last = 0;
        for (uint32_t s : starts) last = std::max(last, s);
        for (auto& m : scan_motions(blk, base)) if (m.hdr > last) starts.push_back(m.hdr);
    }
    starts.push_back((uint32_t)blk.size());
    std::sort(starts.begin(), starts.end());
    auto limit_of = [&](uint32_t off) {
        for (uint32_t s : starts) if (s > off) return s;
        return (uint32_t)blk.size();
    };
    if (in(r.ptr[2])) {
        uint32_t o = r.ptr[2] - base;
        parse_collision(blk, base, o, limit_of(o), r.coll);
    }
    if (in(r.ptr[3])) {
        uint32_t o = r.ptr[3] - base;
        uint32_t n = le32(&blk[o]);
        if (n < 512 && o + 4 + n * 68u <= blk.size()) {
            r.cam_off = o;
            for (uint32_t i = 0; i < n; i++) { CameraRec c; memcpy(c.raw, &blk[o + 4 + i * 68], 68); r.cameras.push_back(c); }
        }
    }
    if (in(r.ptr[6])) {
        uint32_t o = r.ptr[6] - base;
        uint32_t n = le32(&blk[o]);
        if (n < 512 && o + 4 + n * 20u <= blk.size()) {
            r.trig_off = o;
            for (uint32_t i = 0; i < n; i++) { TriggerRec t; memcpy(t.raw, &blk[o + 4 + i * 20], 20); r.triggers.push_back(t); }
        }
    }
    if (in(r.ptr[5])) {
        uint32_t o = r.ptr[5] - base;
        r.script_off = o;
        r.script_size = limit_of(o) - o;
        uint32_t d0 = le32(&blk[o]);
        if (d0 >= 4 && d0 % 4 == 0 && d0 / 4 < 512 && d0 <= r.script_size) {
            for (uint32_t i = 0; i < d0 / 4; i++) r.script_dir.push_back(le32(&blk[o + i * 4]));
        }
        r.strings = scan_strings(blk, o, o + r.script_size);
        for (auto& s : r.strings) {
            uint32_t rel = s.off - o;
            int sub = -1;
            for (size_t i = 0; i < r.script_dir.size(); i++)
                if (r.script_dir[i] <= rel) sub = (int)i;
            s.sub = (uint32_t)(sub < 0 ? 0 : sub);
        }
    }
    r.ok = true;
    return true;
}

}  // namespace dc
