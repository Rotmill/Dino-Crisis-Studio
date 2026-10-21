#include "species.h"

#include "library.h"
#include "placement.h"

#include <functional>
#include <map>
#include <vector>

namespace dc {

static const RamBlock* block_of(const FileContent& c, uint32_t addr) {
    for (auto& b : c.blocks)
        if (addr >= b.dest && addr < b.dest + b.data.size()) return &b;
    return nullptr;
}

// 06.10.2026 (player: "can't our mod add memory from the PC... extend it a lot, so a hundred enemies can be added"): the model
// goes to the end of the room block while it fits (the runtime's own code reads models by the console's 2 MB); the table
// and the motions - the bulk, a raptor's 65 are ~200 KB - into the block for the memory for mods (mod, at mod_base): the
// game's loader unpacks it there, Dino Crisis ME maps that memory (memory.c mod_at)
bool species_put(const FileContent& src, uint32_t model, uint32_t motions, const FileContent* dst, Bytes& blk_room, uint32_t base_room,
                 uint32_t limit, Bytes& mod, uint32_t mod_base, uint32_t& new_model, uint32_t& new_motions, bool& own_model,
                 std::string& note, std::string& err) {
    // ---- the model: header 24 B, bones 20 B each, vertices, normals, triangles, quads - one piece [hdr, end)
    const RamBlock* mb = block_of(src, model);
    if (!mb) { err = strfmt("the model %08X is not in the blocks of %s", model, src.file ? src.file->name.c_str() : "?"); return false; }
    Model m;
    if (!parse_model(mb->data, mb->dest, model - mb->dest, m)) { err = strfmt("no model at %08X", model); return false; }
    const int nb = (int)m.bones.size();
    // ---- the same body in this room: its own model (its pictures are here)
    uint32_t own = 0;
    if (dst) {
        const uint64_t k = actor_key_blk(mb->data, mb->dest, model - mb->dest);
        for (const ActorPart& a : dst->actors) {
            if (!k || a.block < 0 || a.block >= (int)dst->blocks.size() || (int)a.model.bones.size() != nb) continue;
            const RamBlock& b = dst->blocks[a.block];
            if (actor_key_blk(b.data, b.dest, a.model.hdr) == k) { own = b.dest + a.model.hdr; break; }
        }
    }
    own_model = own != 0;
    // ---- the motion table: pointers to motion headers, up to where the motions begin
    const RamBlock* tb = block_of(src, motions);
    if (!tb) { err = strfmt("the motion table %08X is not in the blocks", motions); return false; }
    std::vector<uint32_t> tab;
    uint32_t first = 0;
    for (int i = 0; i < 300; i++) {
        const uint32_t a = motions + (uint32_t)i * 4u;
        if (a + 4 > tb->dest + tb->data.size()) break;
        const uint32_t v = le32(&tb->data[a - tb->dest]);
        if (v < 0x80000000u || v >= 0x80200000u) break;
        if (i == 0) first = v;
        if (first && a >= first) break;
        tab.push_back(v);
    }
    if (tab.empty()) { err = strfmt("no motion table at %08X", motions); return false; }
    // ---- the model: at the end of the room block while it fits, else in the memory for mods; its pointers moved
    const bool in_room = ((blk_room.size() + 3u) & ~(size_t)3u) + (m.end - m.hdr) <= limit;
    const size_t start_room = blk_room.size(), start_mod = mod.size();
    uint32_t nm = own;
    Bytes md;
    if (!own) {
        Bytes& mblk = in_room ? blk_room : mod;
        const uint32_t mbase = in_room ? base_room : mod_base;
        while (mblk.size() % 4) mblk.push_back(0);
        const uint32_t at_model = (uint32_t)mblk.size();
        const uint32_t old_lo = mb->dest + m.hdr, old_hi = mb->dest + m.end;
        nm = mbase + at_model;
        md.assign(mb->data.begin() + m.hdr, mb->data.begin() + m.end);
        auto mv = [&](uint32_t off) {
            if (off + 4 > md.size()) return;
            const uint32_t v = le32(&md[off]);
            if (v >= old_lo && v <= old_hi) put32(&md[off], v - old_lo + nm);
        };
        for (uint32_t k = 0; k < 4; k++) mv(k * 4u);
        for (int i = 0; i < nb; i++) { mv(24u + (uint32_t)i * 20u + 8u); mv(24u + (uint32_t)i * 20u + 12u); }
        mblk.insert(mblk.end(), md.begin(), md.end());
    }
    // ---- the table, then every motion once (header, keys, frames), its pointer to the frames moved: memory for mods
    Bytes& blk = mod;
    const uint32_t base = mod_base;
    while (blk.size() % 4) blk.push_back(0);
    const uint32_t at_tab = (uint32_t)blk.size();
    blk.resize(blk.size() + tab.size() * 4u, 0);
    std::map<uint32_t, uint32_t> moved;
    uint32_t mbytes = 0;
    // one motion to the end of the block (once); a record {u32 address of another motion, u32 -1} is a link to that motion
    // (ST100 raptor: entries 57 and the last ones) - the motion it links to goes too, the link gets its new address
    std::function<bool(uint32_t, int, uint32_t&)> put_motion = [&](uint32_t addr, int depth, uint32_t& na) -> bool {
        auto it = moved.find(addr);
        if (it != moved.end()) { na = it->second; return true; }
        const RamBlock* b = block_of(src, addr);
        if (!b || addr + 8 > b->dest + b->data.size()) { err = strfmt("the motion %08X is not in the blocks", addr); return false; }
        const uint32_t o = addr - b->dest;
        if (le32(&b->data[o + 4]) == 0xFFFFFFFFu) {
            if (depth > 4) { err = strfmt("the motion link %08X goes round", addr); return false; }
            uint32_t tgt = 0;
            if (!put_motion(le32(&b->data[o]), depth + 1, tgt)) return false;
            while (blk.size() % 4) blk.push_back(0);
            const uint32_t at = (uint32_t)blk.size();
            blk.resize(blk.size() + 8u, 0);
            put32(&blk[at], tgt);
            put32(&blk[at + 4], 0xFFFFFFFFu);
            na = base + at;
            moved.emplace(addr, na);
            return true;
        }
        std::vector<Motion> ms = walk_motions(b->data, b->dest, o, nb);
        if (ms.empty() || ms[0].hdr != o) { err = strfmt("the motion %08X is not a motion of %d bones", addr, nb); return false; }
        const Motion& mo = ms[0];
        const uint32_t len = mo.data + (uint32_t)mo.nframes * (uint32_t)mo.fsize - mo.hdr;
        while (blk.size() % 4) blk.push_back(0);
        const uint32_t at = (uint32_t)blk.size();
        blk.insert(blk.end(), b->data.begin() + mo.hdr, b->data.begin() + mo.hdr + len);
        put32(&blk[at], base + at + (mo.data - mo.hdr));
        na = base + at;
        moved.emplace(addr, na);
        mbytes += len;
        return true;
    };
    for (size_t i = 0; i < tab.size(); i++) {
        uint32_t na = 0;
        if (!put_motion(tab[i], 0, na)) {
            blk_room.resize(start_room);
            mod.resize(start_mod);
            err = strfmt("motion %d of the table: %s", (int)i, err.c_str());
            return false;
        }
        put32(&blk[at_tab + (uint32_t)i * 4u], na);
    }
    const uint32_t kModMax = 32u << 20;                   // the runtime maps 32 MB (memory.c, DC_MOD_RAM_MB)
    if (mod.size() > kModMax) {
        blk_room.resize(start_room);
        mod.resize(start_mod);
        err = strfmt("the memory for mods is full (%u MB)", kModMax >> 20);
        return false;
    }
    new_model = nm;
    new_motions = base + at_tab;
    if (own)
        note = strfmt("the room's own model of this body at %08X (its pictures), %d motion(s) %u bytes, table at %08X (memory for mods)",
                      own, (int)moved.size(), mbytes, new_motions);
    else
        note = strfmt("model %u bytes (%d bones, %s; this room has no such body - its pictures are of %s, here other pictures lie "
                      "at those places), %d motion(s) %u bytes, table at %08X (memory for mods)", (unsigned)md.size(), nb,
                      in_room ? "room block" : "memory for mods", src.file ? src.file->stem.c_str() : "the other room",
                      (int)moved.size(), mbytes, new_motions);
    return true;
}

int species_motions(const FileContent& c, uint32_t tab) {
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

bool species_fighter(Disc& d, const FileContent& room, uint32_t model, int kind, SpeciesSrc& out) {
    auto key_of = [](const FileContent& c, uint32_t m) -> uint64_t {
        for (const ActorPart& a : c.actors)
            if (a.block >= 0 && a.block < (int)c.blocks.size() && c.blocks[a.block].dest + a.model.hdr == m)
                return actor_key_blk(c.blocks[a.block].data, c.blocks[a.block].dest, a.model.hdr);
        return 0;
    };
    const uint64_t key = model ? key_of(room, model) : 0;   // 0: the room has none - any body of the kind
    if ((model && !key) || !room.file) return false;
    const char st = room.file->stem.size() >= 3 ? room.file->stem[2] : 0;
    out = SpeciesSrc();
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (auto& f : d.files) {
            if (f.cat != "rooms" || f.stream || f.stem.size() < 3 || (pass == 0) != (f.stem[2] == st)) continue;
            FileContent c;
            if (!analyze(d, f, c) || !c.is_room) continue;
            const json en = room_enemies_json(c);
            if (!en.is_object() || !en.count("enemies")) continue;
            for (auto& e : en["enemies"]) {
                if (e.value("kind", -1) != kind || e.value("op", std::string("20")) != "20") continue;
                uint32_t mo = 0, mt = 0;
                try {
                    mo = (uint32_t)std::stoul(e.value("model", std::string("0")), nullptr, 16);
                    mt = (uint32_t)std::stoul(e.value("motions", std::string("0")), nullptr, 16);
                } catch (...) { continue; }
                if (key && key_of(c, mo) != key) continue;
                const int n = species_motions(c, mt);
                if (n <= best) continue;
                best = n;
                out.room = f.stem;
                out.model = mo;
                out.motions = mt;
                out.nmot = n;
                out.variant = e.value("variant", 0);
                out.hp = e.value("hp", 0);
            }
        }
    return best > 0;
}

}  // namespace dc
