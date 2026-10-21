#include "content.h"
#include "library.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace dc {

std::string TexCrop::file_name() const {
    return strfmt("tp%04x_cx%d_cy%d_u%d-%d_v%d-%d.png", tpage, clut_x(clut), clut_y(clut), u0, u1, v0, v1);
}

std::string material_key(uint16_t tpage, uint16_t clut) { return strfmt("tp%04x_cl%04x", tpage, clut); }

static void finish_crop(const Vram& v, TexCrop& c) {
    v.decode_rect(c.tpage, clut_x(c.clut), clut_y(c.clut), c.u0, c.v0, c.w(), c.h(), c.rgba);
    Bytes key(4 + c.rgba.size() * 4);
    put16(&key[0], (uint16_t)c.w());
    put16(&key[2], (uint16_t)c.h());
    memcpy(&key[4], c.rgba.data(), c.rgba.size() * 4);
    c.hash = sha1_hex(key.data(), key.size());
}

std::vector<TexCrop> crops_for_mesh(const Vram& v, const std::vector<MTri>& tris) {
    std::map<std::pair<uint16_t, uint16_t>, TexCrop> m;
    for (auto& t : tris) {
        auto k = std::make_pair(t.tpage, t.clut);
        auto it = m.find(k);
        if (it == m.end()) {
            TexCrop c;
            c.tpage = t.tpage; c.clut = t.clut;
            c.u0 = c.v0 = 255; c.u1 = c.v1 = 0;
            it = m.emplace(k, c).first;
        }
        for (int i = 0; i < 3; i++) {
            it->second.u0 = std::min<int>(it->second.u0, t.v[i].u);
            it->second.v0 = std::min<int>(it->second.v0, t.v[i].v);
            it->second.u1 = std::max<int>(it->second.u1, t.v[i].u);
            it->second.v1 = std::max<int>(it->second.v1, t.v[i].v);
        }
    }
    std::vector<TexCrop> out;
    for (auto& kv : m) { finish_crop(v, kv.second); out.push_back(std::move(kv.second)); }
    return out;
}

std::vector<TexCrop> crops_for_model(const Vram& v, const Model& md) {
    std::map<std::pair<uint16_t, uint16_t>, TexCrop> m;
    for (auto& p : md.prims) {
        auto k = std::make_pair(p.tpage, p.clut);
        auto it = m.find(k);
        if (it == m.end()) {
            TexCrop c;
            c.tpage = p.tpage; c.clut = p.clut;
            c.u0 = c.v0 = 255; c.u1 = c.v1 = 0;
            it = m.emplace(k, c).first;
        }
        for (int i = 0; i < 3; i++) {
            it->second.u0 = std::min<int>(it->second.u0, p.uv[i] & 0xFF);
            it->second.v0 = std::min<int>(it->second.v0, p.uv[i] >> 8);
            it->second.u1 = std::max<int>(it->second.u1, p.uv[i] & 0xFF);
            it->second.v1 = std::max<int>(it->second.v1, p.uv[i] >> 8);
        }
    }
    std::vector<TexCrop> out;
    for (auto& kv : m) { finish_crop(v, kv.second); out.push_back(std::move(kv.second)); }
    return out;
}

// группы где угодно в блоке (модели переходов DOOR*): {u32 a = свой адрес+12, u32 b = a+nt*40, u16 nt, u16 nq}
static std::vector<MeshGroup> scan_groups(const Bytes& blk, uint32_t base) {
    std::vector<MeshGroup> out;
    uint32_t pos = 0;
    while (pos + 12 <= blk.size()) {
        uint32_t a = le32(&blk[pos]), b = le32(&blk[pos + 4]);
        uint16_t t = le16(&blk[pos + 8]), q = le16(&blk[pos + 10]);
        bool ok = a == base + pos + 12 && b == a + t * 40u && t + q > 0 && pos + 12 + t * 40u + q * 52u <= blk.size();
        if (ok) {
            MeshGroup g;
            g.hdr = pos; g.tri_off = pos + 12; g.nt = t; g.quad_off = pos + 12 + t * 40u; g.nq = q;
            // страницы текстур в разумных пределах
            bool sane = true;
            for (uint32_t i = 0; i < t && sane; i++) sane = le16(&blk[g.tri_off + i * 40 + 24]) < 0x200;
            for (uint32_t i = 0; i < q && sane; i++) sane = le16(&blk[g.quad_off + i * 52 + 32]) < 0x200;
            if (sane) {
                out.push_back(g);
                pos = g.quad_off + q * 52u;
                continue;
            }
        }
        pos += 4;
    }
    return out;
}

static void add_banks(FileContent& c) {
    for (size_t i = 0; i < c.ents.size(); i++) {
        const DatEntry& h = c.ents[i];
        if (h.type == 3) {
            for (size_t k = i + 1; k < c.ents.size(); k++) {
                const DatEntry& b = c.ents[k];
                if (b.type == 4 && b.dest == h.dest) {
                    SoundBank sb;
                    if (parse_vab(&c.raw[h.offset], h.size, &c.raw[b.offset], b.size, b.dest, sb.vab)) {
                        sb.name = strfmt("bank%d", (int)c.banks.size());
                        sb.hdr_entry = (int)i;
                        sb.body_entry = (int)k;
                        c.banks.push_back(std::move(sb));
                    }
                    break;
                }
            }
        } else if (h.type == 5) {
            SeqData s;
            s.name = strfmt("song%d", (int)c.seqs.size());
            s.data.assign(c.raw.begin() + h.offset, c.raw.begin() + h.offset + h.size);
            s.param = h.param;
            s.entry = (int)i;
            c.seqs.push_back(std::move(s));
        }
    }
}

static void add_motion_sets(FileContent& c, int bi, const std::vector<Motion>& all) {
    std::map<int, std::vector<Motion>> by;
    for (auto& m : all) by[m.nbones].push_back(m);
    for (auto& kv : by) {
        MotionSet ms;
        ms.name = strfmt("motions_%d", kv.first);
        ms.block = bi;
        ms.block_dest = c.blocks[bi].dest;
        ms.motions = kv.second;
        ms.nbones = kv.first;
        c.motion_sets.push_back(std::move(ms));
    }
}

static void link_actor_motions(FileContent& c) {
    for (auto& a : c.actors) {
        a.motion_sets.clear();
        for (size_t i = 0; i < c.motion_sets.size(); i++)
            if (c.motion_sets[i].nbones == (int)a.model.bones.size()) a.motion_sets.push_back((int)i);
    }
}

// По числу костей (перепись 10.08.2026 + просмотр 05.10): 21 — раптор, 20 — тираннозавр, 10 — голова тираннозавра (ST202),
// 18 — птеранодон, 7 — компсогнат, 15 — люди (в комнатах) и Регина (CORE, P/H), 22 — тёмный динозавр этапов 6/9 (вид не уточнён).
std::string actor_kind(int nb, bool regina_file) {
    switch (nb) {
    case 15: return regina_file ? "regina" : "human";
    case 21: return "raptor";
    case 20: return "trex";
    case 10: return "trex_head";
    case 18: return "pteranodon";
    case 7: return "compsognathus";
    case 22: return "dino";
    case 1: return "prop";
    default: return "model";
    }
}

static void name_parts(FileContent& c) {
    std::map<std::string, int> count, seen;
    for (auto& a : c.actors) count[a.kind]++;
    for (auto& a : c.actors) {
        int k = ++seen[a.kind];
        a.name = count[a.kind] > 1 ? strfmt("%s_%d", a.kind.c_str(), k) : a.kind;
    }
    // наборы движений — по модели с тем же числом костей
    for (auto& ms : c.motion_sets) {
        if (!ms.name.empty() && ms.name.rfind("motions_", 0) != 0) continue;
        for (auto& a : c.actors)
            if ((int)a.model.bones.size() == ms.nbones) { ms.name = a.kind; break; }
    }
}

static const char* weapon_kind(uint32_t dest) {
    switch (dest) {
    case 0x8017D000: return "shotgun";
    case 0x8017E000: return "handgun";
    case 0x8017F000: return "grenade_gun";
    default: return "weapon";
    }
}

static bool load_container(Disc& d, const char* name, Bytes& raw, std::vector<DatEntry>& ents) {
    const DiscFile* f = d.find(name);
    if (!f || !d.read_file(*f, raw)) return false;
    ents = parse_container(raw);
    return !ents.empty();
}

// Регина из CORE.DAT — для движений, у которых в файле нет своей модели (наборы оружия P/H, движения Регины в комнатах)
static bool add_core_regina(Disc& disc, FileContent& c) {
    Bytes cr;
    std::vector<DatEntry> ce;
    if (!load_container(disc, "CORE.DAT", cr, ce)) return false;
    for (auto& rb : ram_blocks(cr, ce)) {
        if (rb.dest != 0x80180000) continue;
        for (uint32_t h : scan_models(rb.data, rb.dest)) {
            ActorPart a;
            if (!parse_model(rb.data, rb.dest, h, a.model, true) || a.model.bones.size() != 15) continue;
            a.kind = "regina";
            a.block = -1;
            a.block_dest = rb.dest;
            c.actors.push_back(std::move(a));
            // текстуры Регины — страница CORE (костюм 0) в видеопамять файла, если файл её не заливает
            Vram tmp = c.vram;
            c.vram.apply(cr, ce);
            Bytes c0;
            std::vector<DatEntry> e0;
            if (load_container(disc, "CORE00.DAT", c0, e0)) c.vram.apply(c0, e0);
            c.vram.apply(c.raw, c.ents);   // свои заливки файла — сверху, как в игре
            (void)tmp;
            return true;
        }
    }
    return false;
}

bool analyze(Disc& disc, const DiscFile& f, FileContent& c) {
    c = FileContent();
    c.file = &f;
    if (f.stream) {
        c.xa = xa_scan(disc, f);
        return true;
    }
    if (!disc.read_file(f, c.raw)) return false;
    return analyze_raw(disc, f, std::move(c.raw), c);
}

// ---------------------------------------------------------------- картинки предметов
static size_t item_px(int x, int y) {   // смещение байта пикселя в 4 секторах записи
    int wx = x >> 1, tx = wx >> 5, ty = y >> 5;
    return (size_t)(ty * 2 + tx) * 2048u + (size_t)(((y & 31) * 32 + (wx & 31)) * 2 + (x & 1));
}

void item_decode(const uint8_t* rec, std::vector<uint32_t>& rgba) {
    rgba.assign((size_t)kItemW * kItemH, 0);
    const uint8_t* pal = rec + 4 * 2048;
    for (int y = 0; y < kItemH; y++)
        for (int x = 0; x < kItemW; x++) {
            uint16_t c = le16(pal + rec[item_px(x, y)] * 2);
            rgba[(size_t)y * kItemW + x] = c ? (c15_rgba(c) | 0xFF000000u) : 0u;
        }
}

// палитра по картинке: до 255 цветов как есть, больше — разбиение «медианой» по самой длинной оси; 0 — прозрачный
void item_encode(const uint32_t* rgba, uint8_t* rec) {
    std::vector<uint16_t> px((size_t)kItemW * kItemH);
    for (size_t i = 0; i < px.size(); i++) {
        uint32_t c = rgba[i];
        if ((c >> 24) < 128) { px[i] = 0; continue; }
        uint16_t v = (uint16_t)(((c & 0xFF) >> 3) | (((c >> 8) & 0xFF) >> 3) << 5 | (((c >> 16) & 0xFF) >> 3) << 10);
        px[i] = v ? v : 0x8000;   // непрозрачный чёрный — с битом STP (0x0000 у игры прозрачен)
    }
    std::vector<uint16_t> uniq;
    for (uint16_t v : px) if (v) uniq.push_back(v);
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    std::vector<uint16_t> pal;
    if (uniq.size() <= 255) pal = uniq;
    else {
        struct Box { std::vector<uint16_t> c; };
        auto ch = [](uint16_t v, int k) { return (v >> (k * 5)) & 31; };
        std::vector<Box> boxes(1);
        for (uint16_t v : px) if (v) boxes[0].c.push_back(v & 0x7FFF);
        while (boxes.size() < 255) {
            int bi = -1, axis = 0, best = -1;
            for (size_t i = 0; i < boxes.size(); i++) {
                if (boxes[i].c.size() < 2) continue;
                for (int k = 0; k < 3; k++) {
                    int lo = 31, hi = 0;
                    for (uint16_t v : boxes[i].c) { lo = std::min(lo, ch(v, k)); hi = std::max(hi, ch(v, k)); }
                    if (hi - lo > best) { best = hi - lo; bi = (int)i; axis = k; }
                }
            }
            if (bi < 0 || best <= 0) break;
            auto& cv = boxes[bi].c;
            std::sort(cv.begin(), cv.end(), [&](uint16_t a, uint16_t b) { return ch(a, axis) < ch(b, axis); });
            Box nb;
            nb.c.assign(cv.begin() + cv.size() / 2, cv.end());
            cv.resize(cv.size() / 2);
            boxes.push_back(std::move(nb));
        }
        for (auto& b : boxes) {
            if (b.c.empty()) continue;
            int s[3] = {0, 0, 0};
            for (uint16_t v : b.c) for (int k = 0; k < 3; k++) s[k] += ch(v, k);
            uint16_t m = 0;
            for (int k = 0; k < 3; k++) m |= (uint16_t)((s[k] + (int)b.c.size() / 2) / (int)b.c.size()) << (k * 5);
            pal.push_back(m ? m : 0x8000);
        }
    }
    uint8_t* palb = rec + 4 * 2048;
    memset(rec, 0, 5 * 2048);
    for (size_t i = 0; i < pal.size(); i++) put16(palb + (i + 1) * 2, pal[i]);
    for (int y = 0; y < kItemH; y++)
        for (int x = 0; x < kItemW; x++) {
            uint16_t v = px[(size_t)y * kItemW + x];
            int idx = 0;
            if (v) {
                int best = 1 << 30;
                for (size_t i = 0; i < pal.size(); i++) {
                    if (pal[i] == v) { idx = (int)i + 1; break; }
                    int d = 0;
                    for (int k = 0; k < 3; k++) { int a = (v >> (k * 5)) & 31, b = (pal[i] >> (k * 5)) & 31; d += (a - b) * (a - b); }
                    if (d < best) { best = d; idx = (int)i + 1; }
                }
            }
            rec[item_px(x, y)] = (uint8_t)idx;
        }
}

bool analyze_raw(Disc& disc, const DiscFile& f, Bytes raw, FileContent& c) {
    c = FileContent();
    c.file = &f;
    c.raw = std::move(raw);
    if (f.cat == "items" && c.raw.size() >= (size_t)kItemCount * 5 * 2048) {
        // ITEM2 — картинки костюма «Ancient»: свои только у оружия и деталей (1–15)
        const bool two = f.stem == "ITEM2";
        for (int id = 0; id < kItemCount; id++) {
            if (two && (id < 1 || id > 15)) continue;
            ImagePart im;
            im.id = id;
            im.name = strfmt("item_%03d", id);
            im.w = kItemW; im.h = kItemH;
            item_decode(&c.raw[(size_t)id * 5 * 2048], im.rgba);
            c.images.push_back(std::move(im));
        }
        return true;
    }
    c.ents = parse_container(c.raw);
    c.container = !c.ents.empty();
    if (!c.container) return true;

    // видеопамять: костюмы и оружие живут поверх CORE.DAT (шрифт, предметы, эффекты), набор оружия P/Hwk — ещё и костюма k
    // (CORE0k: кисть и текстуры оружия этого костюма; с CORE00 у костюмов 1–3 выходили «не те» текстуры)
    if (f.cat == "characters" && f.stem != "CORE") {
        Bytes cr; std::vector<DatEntry> ce;
        if (load_container(disc, "CORE.DAT", cr, ce)) c.vram.apply(cr, ce);
        if (f.stem[0] == 'P' || f.stem[0] == 'H') {
            const char k = f.stem.size() >= 3 && f.stem[2] >= '0' && f.stem[2] <= '3' ? f.stem[2] : '0';
            Bytes c0; std::vector<DatEntry> e0;
            if (load_container(disc, (std::string("CORE0") + k + ".DAT").c_str(), c0, e0)) c.vram.apply(c0, e0);
        }
    }
    c.vram.apply(c.raw, c.ents, &c.uploads);
    c.blocks = ram_blocks(c.raw, c.ents);
    add_banks(c);

    for (size_t bi = 0; bi < c.blocks.size(); bi++) {
        RamBlock& b = c.blocks[bi];
        if (f.cat == "rooms" && b.dest == 0x80100000 && !c.is_room) {
            RoomInfo r;
            if (parse_room(b.data, b.dest, r)) {
                c.is_room = true;
                c.room = r;
                c.room_block = (int)bi;
                MeshPart mp;
                mp.name = f.stem;
                mp.kind = "room";
                mp.ngroups = (int)r.groups.size();
                mp.block_dest = b.dest;
                mp.block = (int)bi;
                for (auto& g : r.groups) mp.hdrs.push_back(g.hdr);
                collect_tris(b.data, r.groups, mp.tris);
                c.meshes.push_back(std::move(mp));
                for (uint32_t h : scan_models(b.data, b.dest)) {
                    ActorPart a;
                    if (!parse_model(b.data, b.dest, h, a.model)) continue;
                    a.block = (int)bi;
                    a.block_dest = b.dest;
                    a.kind = actor_kind((int)a.model.bones.size(), false);
                    c.actors.push_back(std::move(a));
                }
                add_motion_sets(c, (int)bi, scan_motions(b.data, b.dest));
            }
            continue;
        }
        if (f.cat == "doors" && b.dest == 0x8015D000) {
            auto g = scan_groups(b.data, b.dest);
            if (!g.empty()) {
                MeshPart mp;
                mp.name = f.stem;
                mp.kind = "door";
                mp.ngroups = (int)g.size();
                mp.block_dest = b.dest;
                mp.block = (int)bi;
                for (auto& x : g) mp.hdrs.push_back(x.hdr);
                collect_tris(b.data, g, mp.tris);
                c.meshes.push_back(std::move(mp));
            }
            for (uint32_t h : scan_models(b.data, b.dest)) {
                ActorPart a;
                if (!parse_model(b.data, b.dest, h, a.model)) continue;
                a.block = (int)bi;
                a.block_dest = b.dest;
                a.kind = actor_kind((int)a.model.bones.size(), false);
                c.actors.push_back(std::move(a));
            }
            add_motion_sets(c, (int)bi, scan_motions(b.data, b.dest));
            continue;
        }
        if (f.cat == "characters") {
            if (b.dest == 0x80180000) {
                // две группы в начале блока: ящик предмета на полу (0; модель зон предметов 0x80180000) и дротик (1) — отдельными мешами
                auto g = chain_groups(b.data, b.dest, 0);
                for (size_t gi = 0; gi < g.size(); gi++) {
                    MeshPart mp;
                    mp.kind = gi == 0 ? "item_box" : gi == 1 ? "dart" : "prop";
                    mp.name = gi < 2 ? mp.kind : strfmt("prop_%d", (int)gi);
                    mp.ngroups = 1;
                    mp.block_dest = b.dest;
                    mp.block = (int)bi;
                    mp.hdrs.push_back(g[gi].hdr);
                    collect_tris(b.data, std::vector<MeshGroup>{g[gi]}, mp.tris);
                    c.meshes.push_back(std::move(mp));
                }
                for (uint32_t h : scan_models(b.data, b.dest)) {
                    ActorPart a;
                    if (!parse_model(b.data, b.dest, h, a.model, true)) continue;
                    a.block = (int)bi;
                    a.block_dest = b.dest;
                    a.kind = actor_kind((int)a.model.bones.size(), true);
                    c.actors.push_back(std::move(a));
                }
                uint32_t start = 0;
                for (auto& a : c.actors) start = std::max(start, a.model.end);
                // движения — сразу за моделью (после забивки 0xCD)
                auto mots = scan_motions(b.data, b.dest);
                std::vector<Motion> after;
                for (auto& m : mots) if (m.hdr >= start) after.push_back(m);
                add_motion_sets(c, (int)bi, after);
            } else if (b.dest == 0x8017D000 || b.dest == 0x8017E000 || b.dest == 0x8017F000) {
                auto g = chain_groups(b.data, b.dest, 0);
                if (!g.empty()) {
                    MeshPart mp;
                    mp.kind = weapon_kind(b.dest);
                    mp.name = mp.kind;
                    mp.ngroups = (int)g.size();
                    mp.block_dest = b.dest;
                    mp.block = (int)bi;
                    for (auto& x : g) mp.hdrs.push_back(x.hdr);
                    collect_tris(b.data, g, mp.tris);
                    c.meshes.push_back(std::move(mp));
                }
            } else if (b.dest == 0x80160000) {
                // оружие в руке: вложение кости кисти (кисть и оружие одним мешем, записи с нормалями)
                for (auto& g : scan_lit_groups(b.data, b.dest)) {
                    MeshPart mp;
                    mp.kind = "in_hand";
                    mp.name = "in_hand";
                    mp.ngroups = 1;
                    mp.block_dest = b.dest;
                    mp.block = (int)bi;
                    mp.lit = true;
                    mp.hdrs.push_back(g.hdr);
                    collect_lit_tris(b.data, g, mp.tris);
                    c.meshes.push_back(std::move(mp));
                }
                auto mots = walk_motions(b.data, b.dest, 0, 15);
                if (!mots.empty()) {
                    MotionSet ms;
                    ms.name = f.stem;
                    ms.block = (int)bi;
                    ms.block_dest = b.dest;
                    ms.motions = mots;
                    ms.nbones = 15;
                    c.motion_sets.push_back(std::move(ms));
                }
            }
            continue;
        }
        if (f.cat == "screens") {
            if (b.data.size() == 153600 || b.data.size() == 153664) {
                ImagePart im;
                im.name = c.images.empty() ? "image" : strfmt("image_%d", (int)c.images.size() + 1);
                im.w = 320; im.h = 240;
                im.rgba.resize(320 * 240);
                size_t o = b.data.size() - 153600;
                for (int i = 0; i < 320 * 240; i++) im.rgba[i] = c15_rgba(le16(&b.data[o + i * 2])) | 0xFF000000u;
                c.images.push_back(std::move(im));
            }
        }
    }
    // движения на 15 костей без своей модели (наборы оружия P/H; движения Регины в комнатах) — на Регине из CORE.DAT
    bool has15 = false, mot15 = false;
    for (auto& a : c.actors) has15 |= a.model.bones.size() == 15;
    for (auto& s : c.motion_sets) mot15 |= s.nbones == 15;
    if (mot15 && !has15) add_core_regina(disc, c);
    name_parts(c);
    link_actor_motions(c);
    return true;
}

}  // namespace dc
