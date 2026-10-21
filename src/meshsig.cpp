#include "meshsig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace dc {

namespace {

struct M4 { float m[16]; };   // по столбцам, как в glTF
M4 m4id() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f; return r; }
M4 m4mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0.f;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
M4 m4trs(const float t[3], const float q[4], const float s[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r = m4id();
    r.m[0] = (1 - 2 * (y * y + z * z)) * s[0]; r.m[1] = (2 * (x * y + z * w)) * s[0]; r.m[2] = (2 * (x * z - y * w)) * s[0];
    r.m[4] = (2 * (x * y - z * w)) * s[1]; r.m[5] = (1 - 2 * (x * x + z * z)) * s[1]; r.m[6] = (2 * (y * z + x * w)) * s[1];
    r.m[8] = (2 * (x * z + y * w)) * s[2]; r.m[9] = (2 * (y * z - x * w)) * s[2]; r.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r.m[12] = t[0]; r.m[13] = t[1]; r.m[14] = t[2];
    return r;
}
void m4pt(const M4& a, const float* p, float* o) {
    for (int r = 0; r < 3; r++) o[r] = a.m[r] * p[0] + a.m[4 + r] * p[1] + a.m[8 + r] * p[2] + a.m[12 + r];
}

// мировые матрицы узлов в позе покоя (как dc_meshrep.cpp: node_worlds)
void worlds(const json& j, std::vector<M4>& W, std::vector<int>& parent) {
    const size_t n = j.count("nodes") ? j["nodes"].size() : 0;
    std::vector<M4> L(n, m4id());
    parent.assign(n, -1);
    for (size_t i = 0; i < n; i++) {
        const json& nd = j["nodes"][i];
        if (nd.count("matrix") && nd["matrix"].size() == 16) {
            for (int k = 0; k < 16; k++) L[i].m[k] = nd["matrix"][k].get<float>();
        } else {
            float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
            if (nd.count("translation") && nd["translation"].size() == 3) for (int k = 0; k < 3; k++) t[k] = nd["translation"][k].get<float>();
            if (nd.count("rotation") && nd["rotation"].size() == 4) for (int k = 0; k < 4; k++) q[k] = nd["rotation"][k].get<float>();
            if (nd.count("scale") && nd["scale"].size() == 3) for (int k = 0; k < 3; k++) s[k] = nd["scale"][k].get<float>();
            L[i] = m4trs(t, q, s);
        }
        if (nd.count("children"))
            for (auto& c : nd["children"]) {
                const int ci = c.get<int>();
                if (ci >= 0 && (size_t)ci < n) parent[(size_t)ci] = (int)i;
            }
    }
    W.assign(n, m4id());
    std::vector<char> done(n, 0);
    for (size_t i = 0; i < n; i++) {
        std::vector<int> chain;
        for (int k = (int)i; k >= 0 && !done[(size_t)k] && chain.size() < 256; k = parent[(size_t)k]) chain.push_back(k);
        for (size_t c = chain.size(); c-- > 0;) {
            const int k = chain[c];
            W[(size_t)k] = parent[(size_t)k] >= 0 ? m4mul(W[(size_t)parent[(size_t)k]], L[(size_t)k]) : L[(size_t)k];
            done[(size_t)k] = 1;
        }
    }
}

std::string uri_dec(const std::string& s) {
    std::string u;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { u += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else u += s[i];
    }
    return u;
}

// имя картинки: без папки и расширения, нижний регистр (Blender при пересохранении кладёт картинки в свою папку)
std::string low_base(std::string s) {
    const size_t sl = s.find_last_of("/\\");
    if (sl != std::string::npos) s = s.substr(sl + 1);
    const size_t dot = s.rfind('.');
    if (dot != std::string::npos) s.resize(dot);
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string material_image(const json& j, int mi) {
    if (mi < 0 || !j.count("materials") || mi >= (int)j["materials"].size()) return "";
    const json& m = j["materials"][mi];
    if (!m.count("pbrMetallicRoughness") || !m["pbrMetallicRoughness"].count("baseColorTexture")) return "";
    const int ti = m["pbrMetallicRoughness"]["baseColorTexture"].value("index", -1);
    if (ti < 0 || !j.count("textures") || ti >= (int)j["textures"].size()) return "";
    const int si = j["textures"][ti].value("source", -1);
    if (si < 0 || !j.count("images") || si >= (int)j["images"].size()) return "";
    const json& im = j["images"][si];
    const std::string uri = im.value("uri", "");
    if (!uri.empty() && uri.rfind("data:", 0) != 0) return low_base(uri_dec(uri));
    return low_base(im.value("name", ""));   // картинка внутри файла (.glb) — по имени
}

const char* kReginaBones[15] = {"spine", "arm_r_upper", "arm_r_lower", "hand_r", "arm_l_upper", "arm_l_lower", "hand_l",
                                "head", "pelvis", "leg_r_upper", "leg_r_lower", "foot_r", "leg_l_upper", "leg_l_lower", "foot_l"};

}  // namespace

int bone_of_node_name(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    const size_t dot = s.find('.');
    if (dot != std::string::npos) s.resize(dot);
    for (int i = 0; i < 15; i++)
        if (s == kReginaBones[i]) return i;
    if (s.compare(0, 4, "bone") == 0) {
        size_t k = 4;
        while (k < s.size() && (s[k] == '_' || s[k] == ' ')) k++;
        if (k < s.size() && isdigit((unsigned char)s[k])) return atoi(s.c_str() + k);
    }
    return -1;
}

void sig_static(const MeshPart& m, const Vram& v, int group, SigMesh& out) {
    auto crops = crops_for_mesh(v, m.tris);   // вырезки — по всему мешу (как выгрузка), не по группе
    std::map<std::pair<uint16_t, uint16_t>, const TexCrop*> by;
    for (auto& c : crops) by[{c.tpage, c.clut}] = &c;
    out.has_col = true;
    for (auto& t : m.tris) {
        if (group >= 0 && t.group != group) continue;
        auto it = by.find({t.tpage, t.clut});
        const TexCrop* c = it == by.end() ? nullptr : it->second;
        SigTri s{};
        for (int k = 0; k < 3; k++) {
            const MVert& vv = t.v[k];
            s.p[k][0] = vv.x; s.p[k][1] = vv.y; s.p[k][2] = vv.z;
            s.uv[k][0] = c ? (vv.u - c->u0 + 0.5f) / c->w() : 0.f;
            s.uv[k][1] = c ? (vv.v - c->v0 + 0.5f) / c->h() : 0.f;
            for (int ch = 0; ch < 3; ch++) s.col[k][ch] = std::min(255u, ((t.rgb[k] >> (8 * ch)) & 0xFF) * 2u) / 255.f;
            s.bone[k] = -1;
        }
        s.img = c ? low_base(c->file_name()) : "";
        out.tris.push_back(std::move(s));
    }
}

void sig_actor(const ActorPart& a, const Vram& v, SigMesh& out) {
    const Model& md = a.model;
    auto crops = crops_for_model(v, md);
    std::map<std::pair<uint16_t, uint16_t>, const TexCrop*> by;
    for (auto& c : crops) by[{c.tpage, c.clut}] = &c;
    std::vector<BoneXf> rest;
    rest_bones(md, rest);
    out.has_bone = true;
    if (md.verts.empty()) return;
    for (auto& p : md.prims) {
        auto it = by.find({p.tpage, p.clut});
        const TexCrop* c = it == by.end() ? nullptr : it->second;
        SigTri s{};
        for (int k = 0; k < 3; k++) {
            int vi = p.vi[k];
            if (vi >= (int)md.verts.size()) vi = 0;
            const int b = vi < (int)md.vbone.size() ? md.vbone[vi] : 0;
            const auto& vv = md.verts[vi];
            for (int q = 0; q < 3; q++) s.p[k][q] = vv[q] + (b >= 0 && b < (int)rest.size() ? rest[b].t[q] : 0.f);
            s.uv[k][0] = c ? ((p.uv[k] & 0xFF) - c->u0 + 0.5f) / c->w() : 0.f;
            s.uv[k][1] = c ? ((p.uv[k] >> 8) - c->v0 + 0.5f) / c->h() : 0.f;
            s.col[k][0] = s.col[k][1] = s.col[k][2] = 1.f;
            s.bone[k] = b;
        }
        s.img = c ? low_base(c->file_name()) : "";
        out.tris.push_back(std::move(s));
    }
}

bool sig_gltf(const GDoc& d, const std::string& node, bool actor, SigMesh& out, std::string& err) {
    try {
        const json& j = d.j;
        out.has_bone = actor;
        if (!j.count("nodes")) return node.empty() || (err = "no node " + node, false);
        std::vector<M4> W;
        std::vector<int> parent;
        worlds(j, W, parent);
        const json& nodes = j["nodes"];
        const size_t nn = nodes.size();
        auto bone_up = [&](int k) {
            for (int g = 0; k >= 0 && (size_t)k < nn && g < 256; k = parent[(size_t)k], g++) {
                const int b = bone_of_node_name(nodes[(size_t)k].value("name", ""));
                if (b >= 0) return b;
            }
            return -1;
        };
        bool found = node.empty(), all_col = true, any = false;
        for (size_t i = 0; i < nn; i++) {
            const json& nd = nodes[i];
            if (!nd.count("mesh")) continue;
            if (!node.empty()) {
                bool under = false;
                for (int k = (int)i, g = 0; k >= 0 && g < 256 && !under; k = parent[(size_t)k], g++) under = nodes[(size_t)k].value("name", "") == node;
                if (!under) continue;
            }
            found = true;
            const int mi = nd["mesh"].get<int>();
            if (!j.count("meshes") || mi < 0 || mi >= (int)j["meshes"].size()) continue;
            const json& mesh = j["meshes"][mi];
            const json* skin = nullptr;
            if (actor && nd.count("skin") && j.count("skins")) {
                const int si = nd["skin"].get<int>();
                if (si >= 0 && si < (int)j["skins"].size()) skin = &j["skins"][si];
            }
            std::vector<int> jbone;
            std::vector<M4> jm;
            if (skin && skin->count("joints")) {
                std::vector<float> ibm;
                int nc = 0;
                const bool have_ibm = skin->count("inverseBindMatrices") && d.floats((*skin)["inverseBindMatrices"].get<int>(), ibm, nc) && nc == 16;
                const json& joints = (*skin)["joints"];
                for (size_t k = 0; k < joints.size(); k++) {
                    const int jn = joints[k].get<int>();
                    jbone.push_back(jn >= 0 && (size_t)jn < nn ? bone_up(jn) : -1);
                    M4 ib = m4id();
                    if (have_ibm && (k + 1) * 16 <= ibm.size()) memcpy(ib.m, &ibm[k * 16], sizeof ib.m);
                    jm.push_back(jn >= 0 && (size_t)jn < nn ? m4mul(W[(size_t)jn], ib) : m4id());
                }
            }
            const int rigid = actor ? bone_up((int)i) : -1;
            if (!mesh.count("primitives")) continue;
            for (auto& pr : mesh["primitives"]) {
                if (pr.value("mode", 4) != 4 || !pr.count("attributes")) continue;
                const json& at = pr["attributes"];
                std::vector<float> P, T, C, J, Wt, I;
                int ncP = 0, ncT = 0, ncC = 0, ncJ = 0, ncW = 0, ncI = 0;
                if (!at.count("POSITION") || !d.floats(at["POSITION"].get<int>(), P, ncP) || ncP != 3) continue;
                if (at.count("TEXCOORD_0")) d.floats(at["TEXCOORD_0"].get<int>(), T, ncT);
                const bool hc = at.count("COLOR_0") && d.floats(at["COLOR_0"].get<int>(), C, ncC) && ncC >= 3;
                if (!hc) all_col = false;
                const bool skinned = !jm.empty() && at.count("JOINTS_0") && at.count("WEIGHTS_0") &&
                                     d.floats(at["JOINTS_0"].get<int>(), J, ncJ) && ncJ == 4 &&
                                     d.floats(at["WEIGHTS_0"].get<int>(), Wt, ncW) && ncW == 4;
                const size_t nv = P.size() / 3;
                std::vector<float> gp(nv * 3);
                std::vector<int> gb(nv, rigid);
                for (size_t v = 0; v < nv; v++) {
                    const float* p = &P[v * 3];
                    float o[3] = {0, 0, 0};
                    if (skinned) {
                        float ws = 0, best = -1;
                        for (int k = 0; k < 4; k++) {
                            const float w = Wt[v * 4 + k];
                            const int ji = (int)J[v * 4 + k];
                            if (w <= 0 || ji < 0 || ji >= (int)jm.size()) continue;
                            float tp[3];
                            m4pt(jm[(size_t)ji], p, tp);
                            for (int c = 0; c < 3; c++) o[c] += w * tp[c];
                            ws += w;
                            if (w > best) { best = w; gb[v] = jbone[(size_t)ji]; }
                        }
                        if (ws > 0) for (int c = 0; c < 3; c++) o[c] /= ws;
                    } else {
                        m4pt(W[i], p, o);
                    }
                    gp[v * 3] = o[0] * 1000.f; gp[v * 3 + 1] = -o[1] * 1000.f; gp[v * 3 + 2] = -o[2] * 1000.f;
                }
                const std::string img = material_image(j, pr.value("material", -1));
                std::vector<uint32_t> idx;
                if (pr.count("indices") && d.floats(pr["indices"].get<int>(), I, ncI) && ncI == 1)
                    for (float f : I) idx.push_back((uint32_t)f);
                else
                    for (uint32_t k = 0; k < (uint32_t)nv; k++) idx.push_back(k);
                for (size_t k = 0; k + 2 < idx.size(); k += 3) {
                    SigTri s{};
                    for (int c = 0; c < 3; c++) {
                        size_t vi = idx[k + c];
                        if (vi >= nv) vi = 0;
                        for (int q = 0; q < 3; q++) s.p[c][q] = gp[vi * 3 + q];
                        s.uv[c][0] = ncT == 2 && T.size() >= vi * 2 + 2 ? T[vi * 2] : 0.f;
                        s.uv[c][1] = ncT == 2 && T.size() >= vi * 2 + 2 ? T[vi * 2 + 1] : 0.f;
                        for (int q = 0; q < 3; q++) s.col[c][q] = hc && C.size() >= vi * ncC + 3 ? C[vi * ncC + q] : 1.f;
                        s.bone[c] = gb[vi];
                    }
                    s.img = img;
                    out.tris.push_back(std::move(s));
                }
                any = true;
            }
        }
        if (!found) { err = "no node " + node; return false; }
        out.has_col = any && all_col;
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

static bool tri_match(const SigTri& a, const SigTri& b, bool col, bool bone) {
    if (a.img != b.img) return false;
    static const int perm[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {2, 1, 0}, {1, 0, 2}};
    for (auto& pm : perm) {
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) {
            const int q = pm[k];
            for (int c = 0; c < 3 && ok; c++) ok = fabsf(a.p[k][c] - b.p[q][c]) <= 0.6f;
            for (int c = 0; c < 2 && ok; c++) ok = fabsf(a.uv[k][c] - b.uv[q][c]) <= 1e-3f;
            if (col) for (int c = 0; c < 3 && ok; c++) ok = fabsf(a.col[k][c] - b.col[q][c]) <= 6.f / 255.f;
            if (bone && ok) ok = a.bone[k] == b.bone[q];
        }
        if (ok) return true;
    }
    return false;
}

bool sig_same(const SigMesh& a, const SigMesh& b) {
    if (a.tris.size() != b.tris.size()) return false;
    const bool col = a.has_col && b.has_col, bone = a.has_bone && b.has_bone;
    // корзины по центру треугольника (8 мм), поиск — в соседних тоже (центр у границы корзины)
    auto cell = [](const SigTri& t, int axis) {
        return (int)floorf((t.p[0][axis] + t.p[1][axis] + t.p[2][axis]) / 3.f / 8.f);
    };
    auto pack = [](int x, int y, int z) {
        return ((uint64_t)(uint32_t)(x & 0x1FFFFF) << 42) | ((uint64_t)(uint32_t)(y & 0x1FFFFF) << 21) | (uint64_t)(uint32_t)(z & 0x1FFFFF);
    };
    std::unordered_map<uint64_t, std::vector<int>> grid;
    for (size_t i = 0; i < b.tris.size(); i++) grid[pack(cell(b.tris[i], 0), cell(b.tris[i], 1), cell(b.tris[i], 2))].push_back((int)i);
    std::vector<char> used(b.tris.size(), 0);
    for (auto& t : a.tris) {
        const int cx = cell(t, 0), cy = cell(t, 1), cz = cell(t, 2);
        bool hit = false;
        for (int dx = -1; dx <= 1 && !hit; dx++)
            for (int dy = -1; dy <= 1 && !hit; dy++)
                for (int dz = -1; dz <= 1 && !hit; dz++) {
                    auto it = grid.find(pack(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (int j : it->second)
                        if (!used[(size_t)j] && tri_match(t, b.tris[(size_t)j], col, bone)) { used[(size_t)j] = 1; hit = true; break; }
                }
        if (!hit) return false;
    }
    return true;
}

std::vector<std::string> nodes_outside(const GDoc& d, const std::vector<std::string>& names) {
    std::vector<std::string> out;
    try {
        const json& j = d.j;
        if (!j.count("nodes")) return out;
        std::vector<M4> W;
        std::vector<int> parent;
        worlds(j, W, parent);
        const json& nodes = j["nodes"];
        for (size_t i = 0; i < nodes.size(); i++) {
            if (!nodes[i].count("mesh")) continue;
            bool under = false;
            for (int k = (int)i, g = 0; k >= 0 && g < 256 && !under; k = parent[(size_t)k], g++)
                under = std::find(names.begin(), names.end(), nodes[(size_t)k].value("name", "")) != names.end();
            if (!under) out.push_back(nodes[i].value("name", strfmt("node %d", (int)i)));
        }
    } catch (...) {
    }
    return out;
}

}  // namespace dc
