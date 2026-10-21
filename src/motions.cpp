// glTF → движения игры. Обратное к Exporter::write_actor:
//   узел root: перенос (tx/1000, 0, −tz/1000), поворот — рывок ry вокруг вертикали;
//   кость-корень: перенос (0, −ty/1000, 0), поворот — её углы; прочие кости — углы;
//   поворот кости в игре M = Rx·Ry·Rz (12 бит на угол, 4096 = 360°), в glTF — S·M·S, S = diag(1,−1,−1).
// Из двух равнозначных троек углов берётся ближайшая к прежней (игра интерполирует углы покомпонентно — прыжок
// представления дал бы кувырок между ключами); при неизменённой анимации кадры выходят байт в байт.
#include "motions.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "placement.h"

namespace dc {

static const float kPi = 3.14159265358979323846f;
static const char* kReginaBones[15] = {"spine", "arm_r_upper", "arm_r_lower", "hand_r", "arm_l_upper", "arm_l_lower", "hand_l",
                                       "head", "pelvis", "leg_r_upper", "leg_r_lower", "foot_r", "leg_l_upper", "leg_l_lower", "foot_l"};

std::string bone_node_name(const ActorPart& a, int i) {
    if (a.kind == "regina" && a.model.bones.size() == 15 && i >= 0 && i < 15) return kReginaBones[i];
    return strfmt("bone_%02d", i);
}

std::string actor_export_name(const FileContent& c, const ActorPart& a) {
    if (a.kind == "regina" && a.block < 0 && c.file) return "regina_" + c.file->stem;
    return a.name;
}

std::string motion_anim_name(const MotionSet& ms, int mi) {
    std::string an = strfmt("%s_%02d", ms.name.c_str(), mi);
    for (auto& ch : an) if (ch == ' ' || ch == ',' || ch == '(' || ch == ')') ch = '_';
    return an;
}

// ---------------------------------------------------------------- математика
static M3 quat_m3(const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    M3 r{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
          2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
          2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    return r;
}
static M3 to_game(const M3& m) {   // S·M·S
    static const float s[3] = {1, -1, -1};
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i * 3 + j] = m.m[i * 3 + j] * s[i] * s[j];
    return r;
}
static int a12(float rad) { return (int)lroundf(rad / (2 * kPi) * 4096.0f) & 4095; }
static int cdist(int a, int b) { int d = std::abs((a - b) & 4095); return std::min(d, 4096 - d); }

// M = Rx(a)·Ry(b)·Rz(c) → (a,b,c) в 12 битах, ближайшее к ref представление
static void decompose(const M3& M, const int ref[3], int out[3]) {
    float sb = std::clamp(M.m[2], -1.0f, 1.0f);
    float b = asinf(sb), cb = cosf(b);
    int t1[3];
    if (cb > 1e-4f) {
        t1[0] = a12(atan2f(-M.m[5], M.m[8]));
        t1[1] = a12(b);
        t1[2] = a12(atan2f(-M.m[1], M.m[0]));
    } else {
        // складывание осей: известна только сумма (b = +90°) или разность (b = −90°) a и c — c берём прежним
        int s = a12(atan2f(M.m[3], M.m[4]));
        t1[1] = sb > 0 ? 1024 : 3072;
        t1[2] = ref[2];
        t1[0] = sb > 0 ? (s - ref[2]) & 4095 : (ref[2] - s) & 4095;
    }
    int t2[3] = {(t1[0] + 2048) & 4095, (2048 - t1[1]) & 4095, (t1[2] + 2048) & 4095};
    int d1 = cdist(t1[0], ref[0]) + cdist(t1[1], ref[1]) + cdist(t1[2], ref[2]);
    int d2 = cdist(t2[0], ref[0]) + cdist(t2[1], ref[1]) + cdist(t2[2], ref[2]);
    const int* t = d2 < d1 ? t2 : t1;
    for (int k = 0; k < 3; k++) out[k] = t[k];
}

// ---------------------------------------------------------------- кадр
struct Pose { int tx = 0, ty = 0, tz = 0, ry = 0; std::vector<int> ang; };

static void write_frame(uint8_t* dst, int nb, const Pose& p, int rx, int rz) {
    put16(dst + 0, (uint16_t)(int16_t)p.tx);
    put16(dst + 2, (uint16_t)(int16_t)p.ty);
    put16(dst + 4, (uint16_t)(int16_t)p.tz);
    put16(dst + 6, (uint16_t)(int16_t)rx);
    put16(dst + 8, (uint16_t)(int16_t)p.ry);
    put16(dst + 10, (uint16_t)(int16_t)rz);
    uint8_t* bits = dst + 12;
    for (int i = 0; i < nb * 3; i++) {
        int v = p.ang[i] & 0xFFF;
        for (int b = 0; b < 12; b++) {
            int bit = i * 12 + b;
            uint8_t m = (uint8_t)(1u << (bit & 7));
            if ((v >> b) & 1) bits[bit >> 3] |= m;
            else bits[bit >> 3] &= (uint8_t)~m;
        }
    }
}

struct Rig {
    int root_node = -1;
    std::vector<int> bone_node;
    int root_bone = 0;
};

static void sample(const GDoc& d, const GAnim& an, int node, float t, float T[3], float Q[4]) {
    d.node_rest(node, T, Q);
    auto it = an.tr.find(node);
    if (it != an.tr.end() && !it->second.empty() && it->second.nc == 3) it->second.eval(t, T, false);
    auto ir = an.rot.find(node);
    if (ir != an.rot.end() && !ir->second.empty() && ir->second.nc == 4) ir->second.eval(t, Q, true);
}

static Pose eval_pose(const GDoc& d, const GAnim& an, const Rig& rig, const Model& m, float t, const FramePose& ref) {
    Pose p;
    const int nb = (int)m.bones.size();
    p.ang.resize((size_t)nb * 3);
    float T[3], Q[4];
    sample(d, an, rig.root_node, t, T, Q);
    p.tx = (int)lroundf(T[0] * 1000.0f);
    p.tz = (int)lroundf(-T[2] * 1000.0f);
    M3 rroot = to_game(quat_m3(Q));
    p.ry = a12(atan2f(rroot.m[2], rroot.m[0]));
    // ближе к прежнему значению (ry — полный угол, не 12 бит: оставляем знак как был)
    int ry_ref = ref.ry & 4095;
    if (cdist(p.ry, ry_ref) == 0) p.ry = ref.ry;
    else {
        int d = ((p.ry - ry_ref + 2048) & 4095) - 2048;
        p.ry = ref.ry + d;
    }
    for (int i = 0; i < nb; i++) {
        sample(d, an, rig.bone_node[i], t, T, Q);
        M3 r = to_game(quat_m3(Q));
        if (i == rig.root_bone) {
            p.ty = (int)lroundf(-T[1] * 1000.0f);
            // в игре корень = Ry(ry)·E: E = Ry(−ry)·(Rroot·Rbone)
            M3 tot = mul(rroot, r);
            r = mul(euler_xyz(0, (-p.ry) & 4095, 0), tot);
        }
        int rf[3] = {i * 3 < (int)ref.ang.size() ? ref.ang[i * 3] : 0, i * 3 + 1 < (int)ref.ang.size() ? ref.ang[i * 3 + 1] : 0,
                     i * 3 + 2 < (int)ref.ang.size() ? ref.ang[i * 3 + 2] : 0};
        int o[3];
        decompose(r, rf, o);
        for (int k = 0; k < 3; k++) p.ang[i * 3 + k] = o[k];
    }
    return p;
}

static FramePose pose_as_frame(const Pose& p) {
    FramePose f{};
    f.tx = (int16_t)p.tx; f.ty = (int16_t)p.ty; f.tz = (int16_t)p.tz; f.ry = (int16_t)p.ry;
    f.ang.assign(p.ang.begin(), p.ang.end());
    return f;
}

// ---------------------------------------------------------------- перекладка движения в конец блока комнаты
// Разбор 05.10 (re/motions.md): на заголовок движения врага ссылаются только ячейки таблиц движений в том же блоке,
// скрипт op 0x24 (+4) и op 0x23 (+0x12, при b2 = 3 или 6); у 9 комнат — ещё программа этапа (тогда не перекладываем).
// Блок комнаты — не дальше 0x8015D000 (там блок перехода).
struct RelocCtx {
    bool room = false;
    bool allow_relocate = true;
    std::string room_name;
    const RoomInfo* r = nullptr;
    std::vector<uint32_t> keep;   // смещения u32, которые не ссылки (указатели костей моделей)
};

static bool reloc_room_motion(const RelocCtx& rc, Bytes& blk, uint32_t base, uint32_t old_hdr, const Bytes& motion, uint32_t& new_hdr,
                              int& nrefs, std::string& err) {
    static const char* kStageRefs[] = {"ST201", "ST202", "ST511", "ST605", "ST60C", "ST611", "ST613", "ST900", "ST905"};
    for (auto* s : kStageRefs)
        if (rc.room_name == s) { err = "the stage program refers to motions of this room: only the same length (or shorter) fits"; return false; }
    while (blk.size() % 4) blk.push_back(0);
    if (blk.size() + motion.size() > 0x5D000) { err = strfmt("no room at the end of the room block (%u bytes free)", 0x5D000u - (unsigned)blk.size()); return false; }
    const uint32_t old_addr = base + old_hdr;
    new_hdr = (uint32_t)blk.size();
    const uint32_t new_addr = base + new_hdr;
    blk.insert(blk.end(), motion.begin(), motion.end());
    put32(&blk[new_hdr], le32(&blk[new_hdr]) - (base + old_hdr) + new_addr);   // data_ptr — за новыми ключами
    nrefs = 0;
    std::set<uint32_t> keep(rc.keep.begin(), rc.keep.end());
    // ячейки таблиц (выровненные u32 до конца прежнего блока)
    for (uint32_t o = 0; o + 4 <= new_hdr; o += 4) {
        if (le32(&blk[o]) != old_addr || keep.count(o)) continue;
        if (o + 8 <= new_hdr && le32(&blk[o + 4]) == 0xFFFFFFFFu) continue;   // заглушка пустого слота
        put32(&blk[o], new_addr);
        nrefs++;
    }
    // скрипт: op 0x24 +4, op 0x23 +0x12
    if (rc.r && rc.r->script_size) {
        for (auto& op : script_walk(blk, rc.r->script_off, rc.r->script_size, rc.r->script_dir)) {
            uint8_t* p = &blk[rc.r->script_off + op.pc];
            if (op.op == 0x24 && op.size >= 8 && le32(p + 4) == old_addr) { put32(p + 4, new_addr); nrefs++; }
            if (op.op == 0x23 && op.size >= 24 && (p[2] == 3 || p[2] == 6) && le32(p + 0x12) == old_addr) { put32(p + 0x12, new_addr); nrefs++; }
        }
    }
    return true;
}

// ---------------------------------------------------------------- движение целиком
static bool convert_motion(const GDoc& d, const GAnim& an, const Rig& rig, const Model& m, const Motion& mo, Bytes& blk, uint32_t base,
                           const Bytes& orig, const RelocCtx& rc, std::string& note, std::string& err) {
    const int nb = (int)m.bones.size();
    const int fs = mo.fsize;
    const float t_orig = mo.ticks / 30.0f;
    std::vector<float> kt(mo.keys.size());
    float acc = 0;
    for (size_t k = 0; k < mo.keys.size(); k++) { kt[k] = acc; acc += mo.keys[k].hold / 30.0f; }
    auto orig_frame = [&](int fr) { return read_frame(orig, mo.data + (uint32_t)fr * fs, nb); };
    // ---- та же длина: кадры на своих местах
    if (fabsf(an.duration - t_orig) < 0.5f / 30.0f) {
        int changed = 0;
        for (int fr = 0; fr < mo.nframes; fr++) {
            int key = -1;
            for (size_t k = 0; k < mo.keys.size() && key < 0; k++) if (mo.keys[k].frame == fr) key = (int)k;
            if (key < 0) continue;
            FramePose of = orig_frame(fr);
            Pose p = eval_pose(d, an, rig, m, kt[key], of);
            std::vector<uint8_t> fb(orig.begin() + mo.data + (size_t)fr * fs, orig.begin() + mo.data + (size_t)(fr + 1) * fs);
            write_frame(fb.data(), nb, p, of.rx, of.rz);
            if (memcmp(fb.data(), &orig[mo.data + (size_t)fr * fs], fs) != 0) {
                memcpy(&blk[mo.data + (size_t)fr * fs], fb.data(), fs);
                changed++;
            }
        }
        if (changed) note = strfmt("%d of %d frames", changed, mo.nframes);
        return true;
    }
    // ---- другая длина: ключи заново (шаг — как у большинства ключей игры), кадр на ключ
    int h1 = 0, h2 = 0;
    for (auto& k : mo.keys) (k.hold >= 2 ? h2 : h1)++;
    const int step = h2 > h1 ? 2 : 1;
    const int T = std::max(2, (int)lroundf(an.duration * 30.0f));   // длина в тиках (1/30 с)
    int n = std::max(2, (T + step / 2) / step);   // ключей
    int nf = n;                                    // кадров (ключ k → кадр fk[k])
    const uint32_t old_region = 8 + (uint32_t)mo.keys.size() * 4 + (uint32_t)mo.nframes * fs;
    bool sparse = false;
    if (8 + (uint32_t)n * (4 + fs) > old_region && (!rc.room || !rc.allow_relocate)) {
        // на прежнем месте не помещается, переложить нельзя: ключей — сколько нужно (до 15 тиков на ключ), кадров — сколько
        // влезет; соседние ключи делят кадр (поза держится)
        n = std::max(2, (T + 14) / 15);
        nf = (int)std::min<int64_t>(n, ((int64_t)old_region - 8 - 4 * n) / fs);
        sparse = true;
        if (nf < 2) { err = strfmt("%.2f s instead of %.2f s does not fit here", an.duration, t_orig); return false; }
    }
    if (n > 256 || nf > 256) { err = strfmt("too long: %.2f s", an.duration); return false; }
    const uint32_t new_region = 8 + (uint32_t)n * 4 + (uint32_t)nf * fs;
    const bool relocate = new_region > old_region;
    std::vector<MotionKey> keys(n);
    std::vector<float> nt(n);   // начало ключа, с
    for (int k = 0; k < n; k++) {
        int a = (int)((int64_t)k * T / n), b = (int)((int64_t)(k + 1) * T / n);
        keys[k] = MotionKey{(uint8_t)std::clamp(b - a, 1, 15), (uint8_t)((int64_t)k * nf / n), 0, 0};
        nt[k] = a / 30.0f;
    }
    for (size_t k = 0; k < mo.keys.size(); k++) {
        if (!mo.keys[k].ev_flags) continue;
        float te = kt[k] / std::max(1e-3f, t_orig) * an.duration;
        int nk = 0;
        for (int q = 0; q < n; q++) if (nt[q] <= te + 1e-4f) nk = q;
        keys[nk].ev_flags = mo.keys[k].ev_flags;
        keys[nk].ev_id = mo.keys[k].ev_id;
    }
    // новое движение: заголовок, ключи, кадры подряд (base + mo.hdr — адрес, на который пишем; при перекладке — временно)
    Bytes mv(new_region, 0);
    uint8_t* h = mv.data();
    put32(h, base + mo.hdr + 8 + (uint32_t)n * 4);
    put32(h + 4, (uint32_t)(n - 1));
    for (int k = 0; k < n; k++) {
        h[8 + k * 4] = keys[k].hold; h[9 + k * 4] = keys[k].frame; h[10 + k * 4] = keys[k].ev_flags; h[11 + k * 4] = keys[k].ev_id;
    }
    FramePose ref = orig_frame(0);
    for (int f = 0; f < nf; f++) {
        int k = 0;   // первый ключ этого кадра
        while (k + 1 < n && keys[k].frame < f) k++;
        float t = nt[k];
        int ofr = mo.keys[std::min<size_t>(mo.keys.size() - 1, (size_t)std::max(0, (int)lroundf(t / std::max(1e-3f, an.duration) * (mo.keys.size() - 1))))].frame;
        FramePose of = orig_frame(ofr);
        Pose p = eval_pose(d, an, rig, m, t, ref);
        write_frame(&mv[8 + (size_t)n * 4 + (size_t)f * fs], nb, p, of.rx, of.rz);
        ref = pose_as_frame(p);
    }
    if (!relocate) {
        // на прежнее место (ключи сразу за заголовком — так требует игра), хвост прежнего места — нули
        memset(&blk[mo.hdr], 0, old_region);
        memcpy(&blk[mo.hdr], mv.data(), mv.size());
        note = strfmt("%.2f s -> %.2f s (%d keys%s)", t_orig, an.duration, n, sparse ? ", same frames: no room for more" : "");
        return true;
    }
    uint32_t nh = 0;
    int nrefs = 0;
    if (!reloc_room_motion(rc, blk, base, mo.hdr, mv, nh, nrefs, err)) return false;
    if (nrefs == 0) { err = "no references to this motion were found: it is not used by the game"; return false; }
    memset(&blk[mo.hdr], 0, old_region);   // прежнее место ничьё — нули (блок сжимается в свои сектора)
    note = strfmt("%.2f s -> %.2f s (%d keys), moved to the end of the room block (%d references)", t_orig, an.duration, n, nrefs);
    return true;
}

void apply_motion(const GDoc& d, const std::string& doc_name, const ActorPart& a, const GAnim& an, const FileContent& c, int set_index,
                  int motion, std::map<int, Bytes>& blocks, MotionResult& res, bool allow_relocate) {
    if (set_index < 0 || set_index >= (int)c.motion_sets.size()) return;
    const MotionSet& ms = c.motion_sets[set_index];
    if (ms.ext || ms.block < 0 || motion < 0 || motion >= (int)ms.motions.size()) return;
    Rig rig;
    rig.root_node = d.node_by_name("root");
    const int nb = (int)a.model.bones.size();
    for (int i = 0; i < nb; i++)
        if (a.model.bones[i].parent < 0) { rig.root_bone = i; break; }
    std::string missing;
    for (int i = 0; i < nb; i++) {
        int n = d.node_by_name(bone_node_name(a, i));
        if (n < 0) missing += (missing.empty() ? "" : ", ") + bone_node_name(a, i);
        rig.bone_node.push_back(n);
    }
    if (rig.root_node < 0) missing = "root" + (missing.empty() ? std::string() : ", " + missing);
    if (!missing.empty()) { res.errors.push_back(doc_name + ": no bones " + missing); return; }
    if ((int)ms.nbones != nb) { res.errors.push_back(strfmt("%s: %s: the motion has %d bones, the model %d", doc_name.c_str(), an.name.c_str(), ms.nbones, nb)); return; }
    const RamBlock& rb = c.blocks[ms.block];
    auto it = blocks.find(ms.block);
    if (it == blocks.end()) it = blocks.emplace(ms.block, rb.data).first;
    RelocCtx rc;
    rc.room = c.is_room && ms.block == c.room_block;
    rc.allow_relocate = allow_relocate;
    if (rc.room) {
        rc.room_name = c.file ? c.file->stem : "";
        rc.r = &c.room;
        for (const ActorPart& o : c.actors)   // указатели костей моделей («конец модели») — не ссылки
            if (o.block == ms.block)
                for (size_t bi = 0; bi < o.model.bones.size(); bi++) {
                    rc.keep.push_back(o.model.hdr + 24 + (uint32_t)bi * 20 + 8);
                    rc.keep.push_back(o.model.hdr + 24 + (uint32_t)bi * 20 + 12);
                }
    }
    std::string note, e;
    if (!convert_motion(d, an, rig, a.model, ms.motions[motion], it->second, rb.dest, rb.data, rc, note, e)) {
        res.errors.push_back(strfmt("%s: %s: %s", doc_name.c_str(), an.name.c_str(), e.c_str()));
        return;
    }
    if (note.empty()) res.same++;
    else {
        res.changed++;
        if (note.find("moved to the end") != std::string::npos) res.relocated++;
        res.notes.push_back(strfmt("%s: %s", an.name.c_str(), note.c_str()));
    }
}

}  // namespace dc
