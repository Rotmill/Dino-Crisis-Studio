// Статические меши (цепочки групп: комнаты, двери, оружие), скелетные модели и движения.
// Форматы — tools\dc_extract (EXPORT_NOTES.md, REGINA_NOTES.md) и разведка 10.08.2026 (память dc1_formats_cracked).
#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dc {

// ---------------------------------------------------------------- группы меша
std::vector<MeshGroup> room_groups(const Bytes& blk, uint32_t base) {
    std::vector<MeshGroup> g;
    if (blk.size() < 0x2C) return g;
    uint32_t p7 = le32(&blk[7 * 4]), p8 = le32(&blk[8 * 4]);
    uint16_t nt = le16(&blk[0x24]), nq = le16(&blk[0x26]);
    if (p7 != base + 0x28 || p8 != base + 0x28 + nt * 40u) return g;
    if (0x28 + nt * 40u + nq * 52u > blk.size()) return g;
    MeshGroup m0;
    m0.hdr = 0x1C; m0.tri_off = 0x28; m0.nt = nt; m0.quad_off = 0x28 + nt * 40u; m0.nq = nq;
    g.push_back(m0);
    uint32_t pos = 0x28 + nt * 40u + nq * 52u;
    while (pos + 12 <= blk.size()) {
        uint32_t a = le32(&blk[pos]), b = le32(&blk[pos + 4]);
        uint16_t t = le16(&blk[pos + 8]), q = le16(&blk[pos + 10]);
        if (a != base + pos + 12 || b != a + t * 40u) break;
        if (t > 5000 || q > 5000) break;
        uint32_t end = b - base + q * 52u;
        if (end > blk.size()) break;
        MeshGroup m;
        m.hdr = pos; m.tri_off = a - base; m.nt = t; m.quad_off = b - base; m.nq = q;
        g.push_back(m);
        pos = end;
    }
    return g;
}

std::vector<MeshGroup> chain_groups(const Bytes& blk, uint32_t base, uint32_t start) {
    std::vector<MeshGroup> g;
    uint32_t pos = start;
    while (pos + 12 <= blk.size()) {
        uint32_t a = le32(&blk[pos]), b = le32(&blk[pos + 4]);
        uint16_t t = le16(&blk[pos + 8]), q = le16(&blk[pos + 10]);
        if (a != base + pos + 12 || b != a + t * 40u || t + q == 0) break;
        uint32_t end = b - base + q * 52u;
        if (end > blk.size()) break;
        MeshGroup m;
        m.hdr = pos; m.tri_off = a - base; m.nt = t; m.quad_off = b - base; m.nq = q;
        g.push_back(m);
        pos = end;
    }
    return g;
}

uint32_t groups_end(const std::vector<MeshGroup>& g) {
    if (g.empty()) return 0;
    const MeshGroup& m = g.back();
    return m.quad_off + m.nq * 52u;
}

static MVert rv(const Bytes& b, uint32_t o) {
    MVert v;
    v.x = s16(&b[o]); v.y = s16(&b[o + 2]); v.z = s16(&b[o + 4]);
    v.u = b[o + 6]; v.v = b[o + 7];
    return v;
}

void collect_tris(const Bytes& blk, const std::vector<MeshGroup>& groups, std::vector<MTri>& out) {
    for (size_t gi = 0; gi < groups.size(); gi++) {
        const MeshGroup& g = groups[gi];
        for (uint32_t i = 0; i < g.nt; i++) {
            uint32_t o = g.tri_off + i * 40u;
            MTri t;
            for (int k = 0; k < 3; k++) t.v[k] = rv(blk, o + k * 8);
            t.tpage = le16(&blk[o + 24]); t.clut = le16(&blk[o + 26]);
            for (int k = 0; k < 3; k++) t.rgb[k] = le32(&blk[o + 28 + k * 4]) & 0xFFFFFF;
            t.code = blk[o + 31];
            t.group = (uint16_t)gi;
            t.rec = o; t.part = 0;
            out.push_back(t);
        }
        for (uint32_t i = 0; i < g.nq; i++) {
            uint32_t o = g.quad_off + i * 52u;
            MVert vs[4];
            uint32_t cs[4];
            for (int k = 0; k < 4; k++) { vs[k] = rv(blk, o + k * 8); cs[k] = le32(&blk[o + 36 + k * 4]) & 0xFFFFFF; }
            uint16_t tp = le16(&blk[o + 32]), cl = le16(&blk[o + 34]);
            uint8_t code = blk[o + 39];
            static const int order[2][3] = {{0, 1, 2}, {1, 3, 2}};
            for (int h = 0; h < 2; h++) {
                const int* ord = order[h];
                MTri t;
                for (int k = 0; k < 3; k++) { t.v[k] = vs[ord[k]]; t.rgb[k] = cs[ord[k]]; }
                t.tpage = tp; t.clut = cl; t.code = code; t.group = (uint16_t)gi;
                t.rec = o; t.part = (uint8_t)(1 + h);
                out.push_back(t);
            }
        }
    }
}

// ---------------------------------------------------------------- модели
// Сигнатура заголовка (24 Б): {vert_ptr, norm_ptr, tri_ptr, quad_ptr, u16 n_tri, n_quad, n_bones, pad}:
// vert = hdr+24+20*nb, norm-vert == tri-norm (= nvert*8), quad == tri + n_tri*16, всё внутри блока.
static bool model_sig(const Bytes& blk, uint32_t base, uint32_t h) {
    if (h + 24 > blk.size()) return false;
    uint32_t vp = le32(&blk[h]), np = le32(&blk[h + 4]), tp = le32(&blk[h + 8]), qp = le32(&blk[h + 12]);
    uint16_t nt = le16(&blk[h + 16]), nq = le16(&blk[h + 18]), nb = le16(&blk[h + 20]), pad = le16(&blk[h + 22]);
    if (nb < 1 || nb > 48 || pad != 0) return false;
    if (vp != base + h + 24 + 20u * nb) return false;
    if (np <= vp || tp <= np || np - vp != tp - np || (np - vp) % 8) return false;
    if (qp != tp + nt * 16u) return false;
    if (qp - base + nq * 20u > blk.size()) return false;
    // кости: срезы примитивов внутри общих массивов
    uint32_t st = 0, sq = 0;
    for (int i = 0; i < nb; i++) {
        uint32_t o = h + 24 + i * 20;
        uint8_t par = blk[o + 6];
        if (par != 0xFF && par >= nb) return false;
        uint32_t btp = le32(&blk[o + 8]), bqp = le32(&blk[o + 12]);
        uint16_t bnt = le16(&blk[o + 16]), bnq = le16(&blk[o + 18]);
        if (bnt && (btp < tp || btp + bnt * 16u > qp)) return false;
        if (bnq && (bqp < qp || bqp + bnq * 20u > qp + nq * 20u)) return false;
        st += bnt; sq += bnq;
    }
    return st <= nt && sq <= nq && (nt + nq) > 0;
}

std::vector<uint32_t> scan_models(const Bytes& blk, uint32_t base) {
    std::vector<uint32_t> out;
    for (uint32_t h = 0; h + 24 <= blk.size(); h += 4)
        if (model_sig(blk, base, h)) out.push_back(h);
    return out;
}

bool parse_model(const Bytes& blk, uint32_t base, uint32_t h, Model& m, bool mirror_hand) {
    if (!model_sig(blk, base, h)) return false;
    m = Model();
    m.hdr = h;
    uint32_t vp = le32(&blk[h]) - base, np = le32(&blk[h + 4]) - base;
    m.n_tri = le16(&blk[h + 16]); m.n_quad = le16(&blk[h + 18]);
    int nb = le16(&blk[h + 20]);
    uint32_t qp = le32(&blk[h + 12]) - base;
    m.end = qp + m.n_quad * 20u;
    int nv = (int)((np - vp) / 8);
    m.vert_off = vp; m.norm_off = np; m.nv_disk = nv;
    for (int i = 0; i < nb; i++) {
        uint32_t o = h + 24 + i * 20;
        Bone b;
        b.ox = s16(&blk[o]); b.oy = s16(&blk[o + 2]); b.oz = s16(&blk[o + 4]);
        b.parent = blk[o + 6] == 0xFF ? -1 : blk[o + 6];
        b.flags = blk[o + 7];
        uint32_t btp = le32(&blk[o + 8]), bqp = le32(&blk[o + 12]);
        b.nt = le16(&blk[o + 16]); b.nq = le16(&blk[o + 18]);
        b.tri_off = b.nt ? btp - base : 0;
        b.quad_off = b.nq ? bqp - base : 0;
        m.bones.push_back(b);
    }
    for (int i = 0; i < nv; i++) {
        m.verts.push_back({s16(&blk[vp + i * 8]), s16(&blk[vp + i * 8 + 2]), s16(&blk[vp + i * 8 + 4])});
        m.norms.push_back({s16(&blk[np + i * 8]), s16(&blk[np + i * 8 + 2]), s16(&blk[np + i * 8 + 4])});
    }
    for (int bi = 0; bi < nb; bi++) {
        const Bone& b = m.bones[bi];
        for (int i = 0; i < b.nt; i++) {
            uint32_t o = b.tri_off + i * 16u;
            uint16_t w[8];
            for (int k = 0; k < 8; k++) w[k] = le16(&blk[o + k * 2]);
            // слова UV треугольника не по порядку вершин: первое — вершины 2, второе — 0, третье — 1 (REGINA_NOTES)
            APrim p{{w[0], w[1], w[2]}, {w[4], w[6], w[3]}, w[7], w[5], bi};
            p.rec = o; p.part = 1;
            m.prims.push_back(p);
        }
        for (int i = 0; i < b.nq; i++) {
            uint32_t o = b.quad_off + i * 20u;
            uint16_t w[10];
            for (int k = 0; k < 10; k++) w[k] = le16(&blk[o + k * 2]);
            uint16_t idx[4] = {w[0], w[1], w[2], w[3]}, uv[4] = {w[4], w[6], w[8], w[9]};
            static const int order[2][3] = {{0, 1, 2}, {1, 3, 2}};
            for (int h = 0; h < 2; h++) {
                const int* ord = order[h];
                APrim p{{idx[ord[0]], idx[ord[1]], idx[ord[2]]}, {uv[ord[0]], uv[ord[1]], uv[ord[2]]}, w[7], w[5], bi};
                p.rec = o; p.part = (uint8_t)(2 + h);
                m.prims.push_back(p);
            }
        }
    }
    // правая кисть Регины (кость 3) на диске пустая — зеркалим левую (6), как export_regina.py
    if (mirror_hand && nb == 15) {
        bool has3 = false;
        for (auto& p : m.prims) if (p.bone == 3) { has3 = true; break; }
        if (!has3) {
            std::vector<APrim> src;
            for (auto& p : m.prims) if (p.bone == 6) src.push_back(p);
            std::map<int, int> remap;
            for (auto& p : src) {
                uint16_t nvx[3];
                for (int k = 0; k < 3; k++) {
                    int vi = p.vi[k];
                    auto it = remap.find(vi);
                    if (it == remap.end()) {
                        int ni = (int)m.verts.size();
                        auto v = m.verts[vi];
                        auto n = m.norms[vi];
                        m.verts.push_back({(int16_t)-v[0], v[1], v[2]});
                        m.norms.push_back({(int16_t)-n[0], n[1], n[2]});
                        remap[vi] = ni;
                        nvx[k] = (uint16_t)ni;
                    } else {
                        nvx[k] = (uint16_t)it->second;
                    }
                }
                APrim q{{nvx[0], nvx[2], nvx[1]}, {p.uv[0], p.uv[2], p.uv[1]}, p.tpage, p.clut, 3};
                q.rec = 0; q.part = 0;
                m.prims.push_back(q);
            }
        }
    }
    // кость вершины — как у игры (func_80073F10): байт +7 записи кости — число ТРОЕК вершин этой кости, вершины идут подряд по
    // костям (RTPT по три, пул перезаписывается). Сумма сходится с числом вершин у всех 21 модели диска. Прежнее правило
    // «старшая кость из примитивов» сажало вершины кости без своих примитивов (морда и кончик хвоста компсогната, запястье Регины,
    // кончики крыльев) на соседнюю кость — полигоны вытягивались. Нет сходимости — старое правило.
    m.vbone.assign(m.verts.size(), 0);
    int sum = 0;
    for (auto& b : m.bones) sum += b.flags * 3;
    if (sum == nv) {
        int v = 0;
        for (int bi = 0; bi < nb; bi++)
            for (int k = 0; k < m.bones[bi].flags * 3; k++) m.vbone[v++] = bi;
        for (auto& p : m.prims)   // достроенные вершины (зеркальная кисть) — по своим примитивам
            for (int k = 0; k < 3; k++)
                if (p.vi[k] >= nv && p.vi[k] < m.vbone.size()) m.vbone[p.vi[k]] = std::max(m.vbone[p.vi[k]], p.bone);
    } else {
        for (auto& p : m.prims)
            for (int k = 0; k < 3; k++)
                if (p.vi[k] < m.vbone.size()) m.vbone[p.vi[k]] = std::max(m.vbone[p.vi[k]], p.bone);
    }
    return true;
}

// ---------------------------------------------------------------- движения
int motion_frame_size(int nb) {
    int bytes = 12 + (nb * 3 * 12 + 7) / 8;
    return (bytes + 3) & ~3;
}

static bool motion_hdr(const Bytes& blk, uint32_t base, uint32_t pos, Motion& m) {
    if (pos + 8 > blk.size()) return false;
    uint32_t dptr = le32(&blk[pos]), cnt = le32(&blk[pos + 4]);
    if (dptr < base || dptr >= base + blk.size() || cnt > 500) return false;
    uint32_t nk = cnt + 1;
    if (dptr - base != pos + 8 + nk * 4) return false;
    m = Motion();
    m.hdr = pos;
    m.data = dptr - base;
    int mx = 0;
    for (uint32_t i = 0; i < nk; i++) {
        const uint8_t* e = &blk[pos + 8 + i * 4];
        MotionKey k{e[0], e[1], e[2], e[3]};
        if (k.hold == 0 || k.hold > 16) return false;
        mx = std::max<int>(mx, k.frame);
        m.keys.push_back(k);
        m.ticks += k.hold;
    }
    m.nframes = mx + 1;
    return true;
}

std::vector<Motion> walk_motions(const Bytes& blk, uint32_t base, uint32_t start, int nbones) {
    std::vector<Motion> out;
    uint32_t pos = start;
    while (true) {
        Motion m;
        if (!motion_hdr(blk, base, pos, m)) {
            // выравнивание между движениями (до 16 байт)
            bool found = false;
            for (uint32_t a = 4; a < 16 && !found; a += 4) {
                if (motion_hdr(blk, base, pos + a, m)) { pos += a; found = true; }
            }
            if (!found) break;
        }
        m.nbones = nbones;
        m.fsize = motion_frame_size(nbones);
        uint32_t end = m.data + (uint32_t)m.nframes * m.fsize;
        if (end > blk.size()) break;
        out.push_back(m);
        pos = end;
    }
    return out;
}

std::vector<Motion> scan_motions(const Bytes& blk, uint32_t base) {
    std::vector<Motion> cand;
    for (uint32_t pos = 0; pos + 8 <= blk.size(); pos += 4) {
        Motion m;
        if (motion_hdr(blk, base, pos, m)) cand.push_back(m);
    }
    // число костей: место до следующего заголовка (или до конца блока) / число кадров
    static const int known[] = {15, 21, 20, 22, 18, 10, 7, 6, 1, 23};
    std::vector<Motion> out;
    for (size_t i = 0; i < cand.size(); i++) {
        Motion m = cand[i];
        uint32_t lim = i + 1 < cand.size() ? cand[i + 1].hdr : (uint32_t)blk.size();
        if (lim <= m.data) continue;
        uint32_t gap = lim - m.data;
        int best = 0;
        uint32_t best_rem = 0xFFFFFFFF;
        for (int nb : known) {
            uint32_t need = (uint32_t)m.nframes * motion_frame_size(nb);
            if (need > gap) continue;
            uint32_t rem = gap - need;
            if (rem < best_rem) { best_rem = rem; best = nb; }
        }
        // последний в блоке: хвост может быть чем угодно — берём 15, если влезает, иначе ближайший
        if (!best) continue;
        if (best_rem >= 16 && i + 1 < cand.size()) continue;   // между заголовками не сходится — не движение
        m.nbones = best;
        m.fsize = motion_frame_size(best);
        out.push_back(m);
    }
    return out;
}

FramePose read_frame(const Bytes& blk, uint32_t off, int nb) {
    FramePose f{};
    if (off + 12 > blk.size()) return f;
    f.tx = s16(&blk[off]); f.ty = s16(&blk[off + 2]); f.tz = s16(&blk[off + 4]);
    f.rx = s16(&blk[off + 6]); f.ry = s16(&blk[off + 8]); f.rz = s16(&blk[off + 10]);
    f.ang.resize((size_t)nb * 3);
    const uint8_t* bits = &blk[off + 12];
    size_t avail = blk.size() - off - 12;
    for (int i = 0; i < nb * 3; i++) {
        int bo = i * 12, by = bo >> 3, sh = bo & 7;
        uint32_t w = 0;
        for (int k = 0; k < 3; k++)
            if ((size_t)(by + k) < avail) w |= (uint32_t)bits[by + k] << (8 * k);
        f.ang[i] = (uint16_t)((w >> sh) & 0xFFF);
    }
    return f;
}

// ---------------------------------------------------------------- математика
static const float kAng = 6.28318530718f / 4096.0f;

M3 mul(const M3& a, const M3& b) {
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return r;
}

void mulv(const M3& a, const float v[3], float o[3]) {
    for (int i = 0; i < 3; i++) o[i] = a.m[i * 3] * v[0] + a.m[i * 3 + 1] * v[1] + a.m[i * 3 + 2] * v[2];
}

M3 euler_xyz(int ax, int ay, int az) {
    float cx = cosf(ax * kAng), sx = sinf(ax * kAng), cy = cosf(ay * kAng), sy = sinf(ay * kAng);
    float cz = cosf(az * kAng), sz = sinf(az * kAng);
    M3 rx{{1, 0, 0, 0, cx, -sx, 0, sx, cx}}, ry{{cy, 0, sy, 0, 1, 0, -sy, 0, cy}}, rz{{cz, -sz, 0, sz, cz, 0, 0, 0, 1}};
    return mul(mul(rx, ry), rz);
}

void pose_bones(const Model& m, const FramePose& f, std::vector<BoneXf>& out, bool root_motion) {
    out.resize(m.bones.size());
    for (size_t i = 0; i < m.bones.size(); i++) {
        const Bone& b = m.bones[i];
        int a0 = i * 3 < f.ang.size() ? f.ang[i * 3] : 0, a1 = i * 3 + 1 < f.ang.size() ? f.ang[i * 3 + 1] : 0;
        int a2 = i * 3 + 2 < f.ang.size() ? f.ang[i * 3 + 2] : 0;
        M3 r = euler_xyz(a0, a1, a2);
        if (b.parent < 0) {
            M3 pr = euler_xyz(0, f.ry, 0);
            out[i].r = mul(pr, r);
            if (root_motion) { out[i].t[0] = f.tx; out[i].t[1] = f.ty; out[i].t[2] = f.tz; }
            else { out[i].t[0] = 0; out[i].t[1] = f.ty; out[i].t[2] = 0; }
        } else {
            const BoneXf& p = out[b.parent];
            float off[3] = {(float)b.ox, (float)b.oy, (float)b.oz}, d[3];
            mulv(p.r, off, d);
            out[i].r = mul(p.r, r);
            for (int k = 0; k < 3; k++) out[i].t[k] = p.t[k] + d[k];
        }
    }
}

void rest_bones(const Model& m, std::vector<BoneXf>& out) {
    out.resize(m.bones.size());
    M3 id{{1, 0, 0, 0, 1, 0, 0, 0, 1}};
    for (size_t i = 0; i < m.bones.size(); i++) {
        const Bone& b = m.bones[i];
        out[i].r = id;
        if (b.parent < 0) { out[i].t[0] = b.ox; out[i].t[1] = b.oy; out[i].t[2] = b.oz; }
        else for (int k = 0; k < 3; k++) out[i].t[k] = out[b.parent].t[k] + (k == 0 ? b.ox : k == 1 ? b.oy : b.oz);
    }
}

}  // namespace dc
