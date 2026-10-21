#include "model_out.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace dc {

const char* model_format_name(ModelFormat f) {
    switch (f) {
    case ModelFormat::Fbx: return "FBX";
    case ModelFormat::Obj: return "OBJ";
    default: return "glTF";
    }
}

const char* model_format_ext(ModelFormat f) {
    switch (f) {
    case ModelFormat::Fbx: return ".fbx";
    case ModelFormat::Obj: return ".obj";
    default: return ".gltf";
    }
}

bool model_format_parse(const std::string& s, ModelFormat& f) {
    std::string l;
    for (char c : s) l += (char)std::tolower((unsigned char)c);
    if (!l.empty() && l[0] == '.') l.erase(0, 1);
    if (l == "gltf") { f = ModelFormat::Gltf; return true; }
    if (l == "fbx") { f = ModelFormat::Fbx; return true; }
    if (l == "obj") { f = ModelFormat::Obj; return true; }
    return false;
}

namespace {

// ================================================================ matrices (column-major, as glTF and FBX keep them)
struct M4 { double m[16]; };

M4 m4_identity() {
    M4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}

M4 m4_mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; c++)
        for (int w = 0; w < 4; w++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + w] * b.m[c * 4 + k];
            r.m[c * 4 + w] = s;
        }
    return r;
}

M4 m4_trs(const double t[3], const double q[4], const double s[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r = m4_identity();
    r.m[0] = (1 - 2 * (y * y + z * z)) * s[0]; r.m[1] = 2 * (x * y + z * w) * s[0];       r.m[2] = 2 * (x * z - y * w) * s[0];
    r.m[4] = 2 * (x * y - z * w) * s[1];       r.m[5] = (1 - 2 * (x * x + z * z)) * s[1]; r.m[6] = 2 * (y * z + x * w) * s[1];
    r.m[8] = 2 * (x * z + y * w) * s[2];       r.m[9] = 2 * (y * z - x * w) * s[2];       r.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r.m[12] = t[0]; r.m[13] = t[1]; r.m[14] = t[2];
    return r;
}

bool m4_inverse(const M4& a, M4& out) {
    const double* m = a.m;
    double v[16];
    v[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    v[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    v[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    v[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    v[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    v[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    v[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    v[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    v[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    v[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    v[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    v[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    v[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    v[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    v[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    v[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * v[0] + m[1] * v[4] + m[2] * v[8] + m[3] * v[12];
    if (std::fabs(det) < 1e-12) { out = m4_identity(); return false; }
    for (int i = 0; i < 16; i++) out.m[i] = v[i] / det;
    return true;
}

void m4_point(const M4& a, const double p[3], double o[3]) {
    for (int r = 0; r < 3; r++) o[r] = a.m[r] * p[0] + a.m[4 + r] * p[1] + a.m[8 + r] * p[2] + a.m[12 + r];
}

void m4_dir(const M4& a, const double p[3], double o[3]) {   // rigid parts: the upper 3x3, then unit length
    for (int r = 0; r < 3; r++) o[r] = a.m[r] * p[0] + a.m[4 + r] * p[1] + a.m[8 + r] * p[2];
    const double l = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    if (l > 0) for (int r = 0; r < 3; r++) o[r] /= l;
}

const double kDeg = 180.0 / 3.14159265358979323846;

// FBX rotation order XYZ (X first): R = Rz * Ry * Rx; degrees
void quat_to_euler_xyz(const double q[4], double e[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double r00 = 1 - 2 * (y * y + z * z), r10 = 2 * (x * y + z * w), r20 = 2 * (x * z - y * w);
    const double r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const double r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    const double sb = std::max(-1.0, std::min(1.0, -r20));
    const double b = std::asin(sb);
    double a, c;
    if (std::fabs(sb) < 0.9999999) { a = std::atan2(r21, r22); c = std::atan2(r10, r00); }
    else { c = 0; a = std::atan2(-r12, r11); }
    e[0] = a * kDeg; e[1] = b * kDeg; e[2] = c * kDeg;
}

// the same rotation closest to the previous key: the second Euler solution and whole turns (no flips in the curves)
void euler_near(double e[3], const double prev[3]) {
    double alt[3] = {e[0] + 180, 180 - e[1], e[2] + 180};
    auto wrap = [&](double* v) {
        for (int k = 0; k < 3; k++) {
            while (v[k] - prev[k] > 180) v[k] -= 360;
            while (v[k] - prev[k] < -180) v[k] += 360;
        }
    };
    wrap(e);
    wrap(alt);
    auto dist = [&](const double* v) { return std::fabs(v[0] - prev[0]) + std::fabs(v[1] - prev[1]) + std::fabs(v[2] - prev[2]); };
    if (dist(alt) + 1e-9 < dist(e)) for (int k = 0; k < 3; k++) e[k] = alt[k];
}

void m4_decompose(const M4& a, double t[3], double q[4], double s[3]) {
    t[0] = a.m[12]; t[1] = a.m[13]; t[2] = a.m[14];
    double c[3][3];
    for (int k = 0; k < 3; k++) {
        s[k] = std::sqrt(a.m[k * 4] * a.m[k * 4] + a.m[k * 4 + 1] * a.m[k * 4 + 1] + a.m[k * 4 + 2] * a.m[k * 4 + 2]);
        for (int r = 0; r < 3; r++) c[k][r] = s[k] > 0 ? a.m[k * 4 + r] / s[k] : 0;   // column k, row r
    }
    const double r00 = c[0][0], r11 = c[1][1], r22 = c[2][2];
    const double tr = r00 + r11 + r22;
    if (tr > 0) {
        const double sq = std::sqrt(tr + 1) * 2;
        q[3] = 0.25 * sq; q[0] = (c[1][2] - c[2][1]) / sq; q[1] = (c[2][0] - c[0][2]) / sq; q[2] = (c[0][1] - c[1][0]) / sq;
    } else if (r00 > r11 && r00 > r22) {
        const double sq = std::sqrt(1 + r00 - r11 - r22) * 2;
        q[3] = (c[1][2] - c[2][1]) / sq; q[0] = 0.25 * sq; q[1] = (c[1][0] + c[0][1]) / sq; q[2] = (c[2][0] + c[0][2]) / sq;
    } else if (r11 > r22) {
        const double sq = std::sqrt(1 + r11 - r00 - r22) * 2;
        q[3] = (c[2][0] - c[0][2]) / sq; q[0] = (c[1][0] + c[0][1]) / sq; q[1] = 0.25 * sq; q[2] = (c[2][1] + c[1][2]) / sq;
    } else {
        const double sq = std::sqrt(1 + r22 - r00 - r11) * 2;
        q[3] = (c[0][1] - c[1][0]) / sq; q[0] = (c[2][0] + c[0][2]) / sq; q[1] = (c[2][1] + c[1][2]) / sq; q[2] = 0.25 * sq;
    }
}

// ================================================================ the glTF, read back from memory
struct SNode {
    std::string name;
    double t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
    int mesh = -1, skin = -1, parent = -1;
    std::vector<int> kids;
    M4 world = m4_identity();
};
struct SPrim {
    std::vector<double> pos, nrm, uv, col, jnt, wgt;   // col: 4 per vertex (alpha 1 when the glTF had RGB)
    std::vector<uint32_t> idx;
    int mat = -1;
    size_t nv() const { return pos.size() / 3; }
};
struct SMesh { std::string name; std::vector<SPrim> prims; };
struct SMat { std::string name, uri; bool blend = false; };
struct SSkin { std::vector<int> joints; std::vector<M4> ibm; };
struct SChan { int node = -1, path = 0; std::vector<double> t, v; };   // path 0 translation, 1 rotation, 2 scale
struct SAnim { std::string name; std::vector<SChan> ch; double end = 0; };
struct Scene {
    std::vector<SNode> nodes;
    std::vector<int> roots;
    std::vector<SMesh> meshes;
    std::vector<SMat> mats;
    std::vector<SSkin> skins;
    std::vector<SAnim> anims;
};

int comps(const std::string& t) {
    if (t == "VEC2") return 2;
    if (t == "VEC3") return 3;
    if (t == "VEC4") return 4;
    if (t == "MAT4") return 16;
    return 1;
}

bool read_acc(const Gltf& g, int ai, std::vector<double>& out, int& nc) {
    const json& J = g.j;
    if (!J.count("accessors") || ai < 0 || ai >= (int)J["accessors"].size()) return false;
    const json& a = J["accessors"][ai];
    const int bv = a.value("bufferView", -1);
    if (!J.count("bufferViews") || bv < 0 || bv >= (int)J["bufferViews"].size()) return false;
    const json& v = J["bufferViews"][bv];
    const size_t off = v.value("byteOffset", (size_t)0) + a.value("byteOffset", (size_t)0);
    const int ct = a.value("componentType", 5126);
    nc = comps(a.value("type", std::string("SCALAR")));
    const int cs = (ct == 5126 || ct == 5125) ? 4 : (ct == 5123 || ct == 5122) ? 2 : 1;
    const size_t stride = v.value("byteStride", (size_t)(cs * nc));
    const bool norm = a.value("normalized", false);
    const size_t count = a.value("count", (size_t)0);
    out.assign(count * nc, 0.0);
    for (size_t i = 0; i < count; i++)
        for (int k = 0; k < nc; k++) {
            const size_t p = off + i * stride + (size_t)k * cs;
            if (p + cs > g.bin.size()) return false;
            const uint8_t* b = &g.bin[p];
            double x = 0;
            switch (ct) {
            case 5126: { float f; std::memcpy(&f, b, 4); x = f; break; }
            case 5125: { uint32_t u; std::memcpy(&u, b, 4); x = u; break; }
            case 5123: { uint16_t u; std::memcpy(&u, b, 2); x = norm ? u / 65535.0 : u; break; }
            case 5122: { int16_t u; std::memcpy(&u, b, 2); x = norm ? std::max(-1.0, u / 32767.0) : u; break; }
            case 5121: x = norm ? b[0] / 255.0 : b[0]; break;
            case 5120: { int8_t u = (int8_t)b[0]; x = norm ? std::max(-1.0, u / 127.0) : u; break; }
            default: return false;
            }
            out[i * nc + k] = x;
        }
    return true;
}

std::string uri_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

void world_of(Scene& sc, int n, const M4& parent) {
    SNode& nd = sc.nodes[n];
    nd.world = m4_mul(parent, m4_trs(nd.t, nd.q, nd.s));
    for (int k : nd.kids) world_of(sc, k, nd.world);
}

bool load_scene(const Gltf& g, Scene& sc) {
    const json& J = g.j;
    if (J.count("nodes"))
        for (size_t i = 0; i < J["nodes"].size(); i++) {
            const json& n = J["nodes"][i];
            SNode s;
            s.name = n.value("name", strfmt("node_%d", (int)i));
            if (n.count("matrix") && n["matrix"].size() == 16) {
                M4 m;
                for (int k = 0; k < 16; k++) m.m[k] = n["matrix"][k].get<double>();
                m4_decompose(m, s.t, s.q, s.s);
            }
            if (n.count("translation")) for (int k = 0; k < 3; k++) s.t[k] = n["translation"][k].get<double>();
            if (n.count("rotation")) for (int k = 0; k < 4; k++) s.q[k] = n["rotation"][k].get<double>();
            if (n.count("scale")) for (int k = 0; k < 3; k++) s.s[k] = n["scale"][k].get<double>();
            s.mesh = n.value("mesh", -1);
            s.skin = n.value("skin", -1);
            if (n.count("children")) for (auto& c : n["children"]) s.kids.push_back(c.get<int>());
            sc.nodes.push_back(s);
        }
    for (size_t i = 0; i < sc.nodes.size(); i++)
        for (int k : sc.nodes[i].kids)
            if (k >= 0 && k < (int)sc.nodes.size()) sc.nodes[k].parent = (int)i;
    const int si = J.value("scene", 0);
    if (J.count("scenes") && si >= 0 && si < (int)J["scenes"].size() && J["scenes"][si].count("nodes"))
        for (auto& r : J["scenes"][si]["nodes"]) sc.roots.push_back(r.get<int>());
    for (size_t i = 0; i < sc.nodes.size(); i++)
        if (sc.nodes[i].parent < 0 && std::find(sc.roots.begin(), sc.roots.end(), (int)i) == sc.roots.end()) sc.roots.push_back((int)i);
    for (int r : sc.roots) world_of(sc, r, m4_identity());
    if (J.count("materials"))
        for (auto& m : J["materials"]) {
            SMat s;
            s.name = m.value("name", strfmt("material_%d", (int)sc.mats.size()));
            s.blend = m.value("alphaMode", std::string("OPAQUE")) == "BLEND";
            if (m.count("pbrMetallicRoughness") && m["pbrMetallicRoughness"].count("baseColorTexture")) {
                const int ti = m["pbrMetallicRoughness"]["baseColorTexture"].value("index", -1);
                if (J.count("textures") && ti >= 0 && ti < (int)J["textures"].size()) {
                    const int ii = J["textures"][ti].value("source", -1);
                    if (J.count("images") && ii >= 0 && ii < (int)J["images"].size())
                        s.uri = uri_decode(J["images"][ii].value("uri", std::string()));
                }
            }
            sc.mats.push_back(s);
        }
    if (J.count("meshes"))
        for (auto& m : J["meshes"]) {
            SMesh sm;
            sm.name = m.value("name", strfmt("mesh_%d", (int)sc.meshes.size()));
            if (m.count("primitives"))
                for (auto& p : m["primitives"]) {
                    if (p.value("mode", 4) != 4 || !p.count("attributes")) continue;
                    const json& at = p["attributes"];
                    SPrim sp;
                    int nc = 0;
                    if (!at.count("POSITION") || !read_acc(g, at["POSITION"].get<int>(), sp.pos, nc) || nc != 3) continue;
                    if (at.count("NORMAL") && (!read_acc(g, at["NORMAL"].get<int>(), sp.nrm, nc) || nc != 3)) sp.nrm.clear();
                    if (at.count("TEXCOORD_0") && (!read_acc(g, at["TEXCOORD_0"].get<int>(), sp.uv, nc) || nc != 2)) sp.uv.clear();
                    if (at.count("COLOR_0")) {
                        std::vector<double> c;
                        if (read_acc(g, at["COLOR_0"].get<int>(), c, nc) && (nc == 3 || nc == 4)) {
                            const size_t n = c.size() / nc;
                            sp.col.resize(n * 4);
                            for (size_t i = 0; i < n; i++)
                                for (int k = 0; k < 4; k++) sp.col[i * 4 + k] = k < nc ? c[i * nc + k] : 1.0;
                        }
                    }
                    if (at.count("JOINTS_0") && at.count("WEIGHTS_0")) {
                        int nj = 0, nw = 0;
                        if (!read_acc(g, at["JOINTS_0"].get<int>(), sp.jnt, nj) || !read_acc(g, at["WEIGHTS_0"].get<int>(), sp.wgt, nw) ||
                            nj != 4 || nw != 4) { sp.jnt.clear(); sp.wgt.clear(); }
                    }
                    if (p.count("indices")) {
                        std::vector<double> ix;
                        if (!read_acc(g, p["indices"].get<int>(), ix, nc)) continue;
                        for (double d : ix) sp.idx.push_back((uint32_t)d);
                    } else {
                        for (uint32_t i = 0; i < (uint32_t)sp.nv(); i++) sp.idx.push_back(i);
                    }
                    sp.idx.resize(sp.idx.size() / 3 * 3);
                    sp.mat = p.value("material", -1);
                    sm.prims.push_back(std::move(sp));
                }
            sc.meshes.push_back(std::move(sm));
        }
    if (J.count("skins"))
        for (auto& s : J["skins"]) {
            SSkin sk;
            if (s.count("joints")) for (auto& jn : s["joints"]) sk.joints.push_back(jn.get<int>());
            sk.ibm.assign(sk.joints.size(), m4_identity());
            if (s.count("inverseBindMatrices")) {
                std::vector<double> v;
                int nc = 0;
                if (read_acc(g, s["inverseBindMatrices"].get<int>(), v, nc) && nc == 16)
                    for (size_t i = 0; i < sk.joints.size() && (i + 1) * 16 <= v.size(); i++)
                        for (int k = 0; k < 16; k++) sk.ibm[i].m[k] = v[i * 16 + k];
            } else {
                for (size_t i = 0; i < sk.joints.size(); i++)
                    if (sk.joints[i] >= 0 && sk.joints[i] < (int)sc.nodes.size()) m4_inverse(sc.nodes[sk.joints[i]].world, sk.ibm[i]);
            }
            sc.skins.push_back(std::move(sk));
        }
    if (J.count("animations"))
        for (auto& a : J["animations"]) {
            SAnim an;
            an.name = a.value("name", strfmt("animation_%d", (int)sc.anims.size()));
            if (!a.count("channels") || !a.count("samplers")) continue;
            for (auto& c : a["channels"]) {
                if (!c.count("target")) continue;
                const std::string path = c["target"].value("path", std::string());
                SChan ch;
                ch.node = c["target"].value("node", -1);
                ch.path = path == "translation" ? 0 : path == "rotation" ? 1 : path == "scale" ? 2 : -1;
                const int si2 = c.value("sampler", -1);
                if (ch.path < 0 || ch.node < 0 || ch.node >= (int)sc.nodes.size() || si2 < 0 || si2 >= (int)a["samplers"].size()) continue;
                const json& smp = a["samplers"][si2];
                int nt = 0, nv = 0;
                std::vector<double> v;
                if (!read_acc(g, smp.value("input", -1), ch.t, nt) || !read_acc(g, smp.value("output", -1), v, nv)) continue;
                const int want = ch.path == 1 ? 4 : 3;
                if (nv != want) continue;
                const bool cubic = smp.value("interpolation", std::string("LINEAR")) == "CUBICSPLINE";
                for (size_t k = 0; k < ch.t.size(); k++) {
                    const size_t at = cubic ? (k * 3 + 1) * want : k * want;   // cubic: tangent, value, tangent
                    if (at + want > v.size()) break;
                    ch.v.insert(ch.v.end(), v.begin() + at, v.begin() + at + want);
                }
                if (ch.v.size() != ch.t.size() * want) continue;
                if (!ch.t.empty()) an.end = std::max(an.end, ch.t.back());
                an.ch.push_back(std::move(ch));
            }
            sc.anims.push_back(std::move(an));
        }
    return true;
}

// ================================================================ FBX 7.4, binary
// A node record: end offset, property count, property bytes, name, properties, nested records, and a record of 13 zero
// bytes after a nested list (also after an empty node and after AnimationStack / AnimationLayer, as the FBX SDK writes).
struct FNode {
    std::string name;
    Bytes props;
    uint32_t nprops = 0;
    std::vector<FNode> kids;
    explicit FNode(const char* n) : name(n) {}
    template <class T> void raw(T v) { props.insert(props.end(), (const uint8_t*)&v, (const uint8_t*)&v + sizeof v); }
    FNode& I(int32_t v) { props.push_back('I'); raw(v); nprops++; return *this; }
    FNode& L(int64_t v) { props.push_back('L'); raw(v); nprops++; return *this; }
    FNode& D(double v) { props.push_back('D'); raw(v); nprops++; return *this; }
    FNode& C(bool v) { props.push_back('C'); props.push_back(v ? 1 : 0); nprops++; return *this; }
    FNode& S(const std::string& s) {
        props.push_back('S');
        raw((uint32_t)s.size());
        props.insert(props.end(), s.begin(), s.end());
        nprops++;
        return *this;
    }
    FNode& R(const void* p, size_t n) {
        props.push_back('R');
        raw((uint32_t)n);
        props.insert(props.end(), (const uint8_t*)p, (const uint8_t*)p + n);
        nprops++;
        return *this;
    }
    template <class T> FNode& arr(char type, const std::vector<T>& a) {
        props.push_back((uint8_t)type);
        raw((uint32_t)a.size());
        raw((uint32_t)0);                                   // not compressed
        raw((uint32_t)(a.size() * sizeof(T)));
        if (!a.empty()) props.insert(props.end(), (const uint8_t*)a.data(), (const uint8_t*)a.data() + a.size() * sizeof(T));
        nprops++;
        return *this;
    }
    FNode& Ad(const std::vector<double>& a) { return arr('d', a); }
    FNode& Af(const std::vector<float>& a) { return arr('f', a); }
    FNode& Ai(const std::vector<int32_t>& a) { return arr('i', a); }
    FNode& Al(const std::vector<int64_t>& a) { return arr('l', a); }
    FNode& add(const char* n) { kids.emplace_back(n); return kids.back(); }
    FNode& add(FNode n) { kids.push_back(std::move(n)); return kids.back(); }
};

void fbx_emit(Bytes& out, const FNode& n) {
    const size_t at = out.size();
    auto put32 = [&](uint32_t v) { out.insert(out.end(), (const uint8_t*)&v, (const uint8_t*)&v + 4); };
    put32(0);
    put32(n.nprops);
    put32((uint32_t)n.props.size());
    out.push_back((uint8_t)n.name.size());
    out.insert(out.end(), n.name.begin(), n.name.end());
    out.insert(out.end(), n.props.begin(), n.props.end());
    for (const FNode& k : n.kids) fbx_emit(out, k);
    if (!n.kids.empty() || n.nprops == 0 || n.name == "AnimationStack" || n.name == "AnimationLayer") out.insert(out.end(), 13, 0);
    const uint32_t end = (uint32_t)out.size();
    std::memcpy(&out[at], &end, 4);
}

FNode P(const char* name, const char* type, const char* label, const char* flags) {
    FNode p("P");
    p.S(name).S(type).S(label).S(flags);
    return p;
}

std::string oname(const std::string& n, const char* cls) {
    std::string s = n;
    s.push_back('\0');
    s.push_back('\x01');
    s += cls;
    return s;
}

std::vector<double> m4_vec(const M4& m) { return std::vector<double>(m.m, m.m + 16); }

const int64_t kTicks = 46186158000LL;   // FBX time units per second

}  // namespace

bool write_fbx(const Gltf& g, const fs::path& path) {
    Scene sc;
    if (!load_scene(g, sc)) return false;
    const fs::path dir = path.parent_path();
    int64_t next = 1000000000LL;
    auto nid = [&] { return next++; };

    FNode objects("Objects");
    FNode conns("Connections");
    auto oo = [&](int64_t child, int64_t parent) { conns.add("C").S("OO").L(child).L(parent); };
    auto op = [&](int64_t child, int64_t parent, const char* prop) { conns.add("C").S("OP").L(child).L(parent).S(prop); };
    std::map<std::string, int> counts;

    // ---- nodes: Mesh, LimbNode (joints of a skin), Null
    std::set<int> joints, holding;               // joints of skins; joints some vertex hangs on
    for (auto& sk : sc.skins) for (int j : sk.joints) joints.insert(j);
    for (auto& n : sc.nodes) {
        if (n.mesh < 0 || n.mesh >= (int)sc.meshes.size() || n.skin < 0 || n.skin >= (int)sc.skins.size()) continue;
        const SSkin& sk = sc.skins[n.skin];
        for (auto& p : sc.meshes[n.mesh].prims)
            for (size_t k = 0; k < p.jnt.size(); k++)
                if (p.wgt[k] > 0 && p.jnt[k] >= 0 && p.jnt[k] < (int)sk.joints.size()) holding.insert(sk.joints[(int)p.jnt[k]]);
    }
    std::vector<int64_t> model_id(sc.nodes.size(), 0);
    for (size_t i = 0; i < sc.nodes.size(); i++) model_id[i] = nid();
    for (size_t i = 0; i < sc.nodes.size(); i++) {
        const SNode& n = sc.nodes[i];
        // the skin's top joint that holds no vertex (the export's "root": the ground motion) goes out as a Null: Blender's
        // importer would join the first bone to it (that bone's head lies at its tail) and then drop the bone's own motion
        const bool top_empty = joints.count((int)i) && !holding.count((int)i) && (n.parent < 0 || !joints.count(n.parent));
        const char* kind = n.mesh >= 0 ? "Mesh" : (joints.count((int)i) && !top_empty) ? "LimbNode" : "Null";
        FNode& m = objects.add("Model");
        m.L(model_id[i]).S(oname(n.name, "Model")).S(kind);
        m.add("Version").I(232);
        FNode& p70 = m.add("Properties70");
        double e[3];
        quat_to_euler_xyz(n.q, e);
        p70.add(P("Lcl Translation", "Lcl Translation", "", "A")).D(n.t[0]).D(n.t[1]).D(n.t[2]);
        p70.add(P("Lcl Rotation", "Lcl Rotation", "", "A")).D(e[0]).D(e[1]).D(e[2]);
        p70.add(P("Lcl Scaling", "Lcl Scaling", "", "A")).D(n.s[0]).D(n.s[1]).D(n.s[2]);
        if (n.mesh >= 0) p70.add(P("DefaultAttributeIndex", "int", "Integer", "")).I(0);
        p70.add(P("InheritType", "enum", "", "")).I(1);
        m.add("Shading").C(true);
        m.add("Culling").S("CullingOff");
        counts["Model"]++;
        oo(model_id[i], n.parent >= 0 ? model_id[n.parent] : 0);
        if (std::string(kind) == "LimbNode") {
            const int64_t a = nid();
            FNode& at = objects.add("NodeAttribute");
            at.L(a).S(oname(n.name, "NodeAttribute")).S("LimbNode");
            at.add("Properties70").add(P("Size", "double", "Number", "")).D(1.0);
            at.add("TypeFlags").S("Skeleton");
            counts["NodeAttribute"]++;
            oo(a, model_id[i]);
        }
    }

    // ---- materials and their pictures
    std::vector<int64_t> mat_id(sc.mats.size(), 0);
    std::map<std::string, int64_t> tex_of;
    auto material = [&](int mi) -> int64_t {
        if (mi < 0 || mi >= (int)sc.mats.size()) return 0;
        if (mat_id[mi]) return mat_id[mi];
        const SMat& sm = sc.mats[mi];
        const int64_t id = mat_id[mi] = nid();
        FNode& m = objects.add("Material");
        m.L(id).S(oname(sm.name, "Material")).S("");
        m.add("Version").I(102);
        m.add("ShadingModel").S("lambert");
        m.add("MultiLayer").I(0);
        FNode& p70 = m.add("Properties70");
        p70.add(P("DiffuseColor", "Color", "", "A")).D(1).D(1).D(1);
        p70.add(P("DiffuseFactor", "Number", "", "A")).D(1);
        p70.add(P("AmbientColor", "Color", "", "A")).D(0).D(0).D(0);
        counts["Material"]++;
        if (!sm.uri.empty()) {
            auto it = tex_of.find(sm.uri);
            if (it == tex_of.end()) {
                const int64_t t = nid(), v = nid();
                const std::string abs = path_u8((dir / u8path(sm.uri)).lexically_normal());
                std::string rel = sm.uri;
                std::replace(rel.begin(), rel.end(), '/', '\\');
                const std::string base = path_u8(u8path(sm.uri).stem());
                FNode& vd = objects.add("Video");
                vd.L(v).S(oname(base, "Video")).S("Clip");
                vd.add("Type").S("Clip");
                FNode& vp = vd.add("Properties70");
                vp.add(P("Path", "KString", "XRefUrl", "")).S(abs);
                vp.add(P("RelPath", "KString", "XRefUrl", "")).S(rel);
                vd.add("UseMipMap").I(0);
                vd.add("Filename").S(abs);
                vd.add("RelativeFilename").S(rel);
                FNode& tx = objects.add("Texture");
                tx.L(t).S(oname(base, "Texture")).S("");
                tx.add("Type").S("TextureVideoClip");
                tx.add("Version").I(202);
                tx.add("TextureName").S(oname(base, "Texture"));
                FNode& tp = tx.add("Properties70");
                tp.add(P("UseMaterial", "bool", "", "")).I(1);
                tx.add("Media").S(oname(base, "Video"));
                tx.add("FileName").S(abs);
                tx.add("RelativeFilename").S(rel);
                tx.add("ModelUVTranslation").D(0).D(0);
                tx.add("ModelUVScaling").D(1).D(1);
                tx.add("Texture_Alpha_Source").S("None");
                tx.add("Cropping").I(0).I(0).I(0).I(0);
                counts["Video"]++;
                counts["Texture"]++;
                oo(v, t);
                it = tex_of.emplace(sm.uri, t).first;
            }
            op(it->second, id, "DiffuseColor");
        }
        return id;
    };

    // ---- meshes (one geometry per node: its primitives, a material per polygon), skins, the bind pose
    std::vector<std::pair<int64_t, M4>> pose;
    std::set<int> posed;
    for (size_t i = 0; i < sc.nodes.size(); i++) {
        const SNode& n = sc.nodes[i];
        if (n.mesh < 0 || n.mesh >= (int)sc.meshes.size()) continue;
        const SMesh& sm = sc.meshes[n.mesh];
        std::vector<double> verts, nrm, uv, col;
        std::vector<int32_t> pvi, uvi, coli, mats;
        std::vector<int> used;                       // glTF materials in the order connected to the model
        bool any_n = false, any_uv = false, any_col = false;
        for (auto& p : sm.prims) { any_n |= !p.nrm.empty(); any_uv |= !p.uv.empty(); any_col |= !p.col.empty(); }
        struct Infl { std::vector<int32_t> idx; std::vector<double> w; };
        std::map<int, Infl> infl;                    // joint slot -> control points
        int base = 0;
        for (auto& p : sm.prims) {
            const int nv = (int)p.nv();
            for (int v = 0; v < nv; v++) {
                verts.insert(verts.end(), {p.pos[v * 3], p.pos[v * 3 + 1], p.pos[v * 3 + 2]});
                if (any_uv) { uv.push_back(p.uv.empty() ? 0 : p.uv[v * 2]); uv.push_back(p.uv.empty() ? 0 : 1.0 - p.uv[v * 2 + 1]); }
                if (any_col) for (int k = 0; k < 4; k++) col.push_back(p.col.empty() ? 1.0 : p.col[v * 4 + k]);
                if (n.skin >= 0 && !p.jnt.empty())
                    for (int k = 0; k < 4; k++) {
                        const double w = p.wgt[v * 4 + k];
                        if (w <= 0) continue;
                        Infl& f = infl[(int)p.jnt[v * 4 + k]];
                        f.idx.push_back(base + v);
                        f.w.push_back(w);
                    }
            }
            int slot = 0;
            auto it = std::find(used.begin(), used.end(), p.mat);
            if (it == used.end()) { used.push_back(p.mat); slot = (int)used.size() - 1; }
            else slot = (int)(it - used.begin());
            for (size_t t = 0; t + 2 < p.idx.size(); t += 3) {
                for (int k = 0; k < 3; k++) {
                    const int vi = base + (int)p.idx[t + k];
                    pvi.push_back(k == 2 ? -vi - 1 : vi);
                    if (any_uv) uvi.push_back(vi);
                    if (any_col) coli.push_back(vi);
                    if (any_n) {
                        const uint32_t li = p.idx[t + k];
                        if (!p.nrm.empty()) nrm.insert(nrm.end(), {p.nrm[li * 3], p.nrm[li * 3 + 1], p.nrm[li * 3 + 2]});
                        else nrm.insert(nrm.end(), {0.0, 1.0, 0.0});
                    }
                }
                mats.push_back(slot);
            }
            base += nv;
        }
        const int64_t gid = nid();
        FNode& ge = objects.add("Geometry");
        ge.L(gid).S(oname(sm.name, "Geometry")).S("Mesh");
        ge.add("GeometryVersion").I(124);
        ge.add("Vertices").Ad(verts);
        ge.add("PolygonVertexIndex").Ai(pvi);
        FNode layer("Layer");
        layer.I(0);
        layer.add("Version").I(100);
        auto le = [&](const char* type) { FNode& e = layer.add("LayerElement"); e.add("Type").S(type); e.add("TypedIndex").I(0); };
        if (any_n) {
            FNode& e = ge.add("LayerElementNormal");
            e.I(0);
            e.add("Version").I(102);
            e.add("Name").S("");
            e.add("MappingInformationType").S("ByPolygonVertex");
            e.add("ReferenceInformationType").S("Direct");
            e.add("Normals").Ad(nrm);
            le("LayerElementNormal");
        }
        if (any_uv) {
            FNode& e = ge.add("LayerElementUV");
            e.I(0);
            e.add("Version").I(101);
            e.add("Name").S("UVMap");
            e.add("MappingInformationType").S("ByPolygonVertex");
            e.add("ReferenceInformationType").S("IndexToDirect");
            e.add("UV").Ad(uv);
            e.add("UVIndex").Ai(uvi);
            le("LayerElementUV");
        }
        if (any_col) {
            FNode& e = ge.add("LayerElementColor");
            e.I(0);
            e.add("Version").I(101);
            e.add("Name").S("Col");
            e.add("MappingInformationType").S("ByPolygonVertex");
            e.add("ReferenceInformationType").S("IndexToDirect");
            e.add("Colors").Ad(col);
            e.add("ColorIndex").Ai(coli);
            le("LayerElementColor");
        }
        {
            FNode& e = ge.add("LayerElementMaterial");
            e.I(0);
            e.add("Version").I(101);
            e.add("Name").S("");
            e.add("MappingInformationType").S("ByPolygon");
            e.add("ReferenceInformationType").S("IndexToDirect");
            e.add("Materials").Ai(mats);
            le("LayerElementMaterial");
        }
        ge.add(std::move(layer));
        counts["Geometry"]++;
        oo(gid, model_id[i]);
        for (int mi : used) {
            const int64_t m = material(mi);
            if (m) oo(m, model_id[i]);
        }
        if (n.skin >= 0 && n.skin < (int)sc.skins.size() && !infl.empty()) {
            const SSkin& sk = sc.skins[n.skin];
            const int64_t skid = nid();
            FNode& s = objects.add("Deformer");
            s.L(skid).S(oname(sm.name + "_skin", "Deformer")).S("Skin");
            s.add("Version").I(101);
            s.add("Link_DeformAcuracy").D(50.0);
            counts["Deformer"]++;
            oo(skid, gid);
            for (auto& kv : infl) {
                const int slot = kv.first;
                if (slot < 0 || slot >= (int)sk.joints.size()) continue;
                const int jn = sk.joints[slot];
                if (jn < 0 || jn >= (int)sc.nodes.size()) continue;
                // Transform: the geometry in the bone's space at bind time (the glTF inverse bind matrix, the mesh node
                // taken into account); TransformLink: the bone in the world at bind time
                M4 link;
                m4_inverse(sk.ibm[slot], link);
                link = m4_mul(n.world, link);
                M4 xf;
                M4 inv_link;
                m4_inverse(link, inv_link);
                xf = m4_mul(inv_link, n.world);
                const int64_t cid = nid();
                FNode& c = objects.add("Deformer");
                c.L(cid).S(oname(sc.nodes[jn].name, "SubDeformer")).S("Cluster");
                c.add("Version").I(100);
                c.add("UserData").S("").S("");
                c.add("Indexes").Ai(kv.second.idx);
                c.add("Weights").Ad(kv.second.w);
                c.add("Transform").Ad(m4_vec(xf));
                c.add("TransformLink").Ad(m4_vec(link));
                counts["Deformer"]++;
                oo(cid, skid);
                oo(model_id[jn], cid);
                if (!posed.count(jn)) { posed.insert(jn); pose.push_back({model_id[jn], link}); }
            }
            if (!posed.count((int)i)) { posed.insert((int)i); pose.push_back({model_id[i], n.world}); }
        }
    }
    if (!pose.empty()) {
        // joints without vertices of their own (the root) - their place in the skeleton
        for (auto& sk : sc.skins)
            for (int jn : sk.joints)
                if (jn >= 0 && jn < (int)sc.nodes.size() && !posed.count(jn)) { posed.insert(jn); pose.push_back({model_id[jn], sc.nodes[jn].world}); }
        const int64_t pid = nid();
        FNode& p = objects.add("Pose");
        p.L(pid).S(oname("BindPose", "Pose")).S("BindPose");
        p.add("Type").S("BindPose");
        p.add("Version").I(100);
        p.add("NbPoseNodes").I((int32_t)pose.size());
        for (auto& pn : pose) {
            FNode& n = p.add("PoseNode");
            n.add("Node").L(pn.first);
            n.add("Matrix").Ad(m4_vec(pn.second));
        }
        counts["Pose"]++;
    }

    // ---- animations: a stack with one layer each; rotations as XYZ Euler degrees, keys linear
    int64_t span = 0;
    std::string first_stack;
    FNode takes("Takes");
    takes.add("Current").S("");
    for (auto& an : sc.anims) {
        if (an.ch.empty()) continue;
        const int64_t stop = (int64_t)std::llround(an.end * (double)kTicks);
        span = std::max(span, stop);
        if (first_stack.empty()) first_stack = an.name;
        const int64_t sid = nid(), lid = nid();
        FNode& st = objects.add("AnimationStack");
        st.L(sid).S(oname(an.name, "AnimStack")).S("");
        FNode& sp = st.add("Properties70");
        sp.add(P("LocalStart", "KTime", "Time", "")).L(0);
        sp.add(P("LocalStop", "KTime", "Time", "")).L(stop);
        sp.add(P("ReferenceStart", "KTime", "Time", "")).L(0);
        sp.add(P("ReferenceStop", "KTime", "Time", "")).L(stop);
        FNode& ly = objects.add("AnimationLayer");
        ly.L(lid).S(oname("BaseLayer", "AnimLayer")).S("");
        counts["AnimationStack"]++;
        counts["AnimationLayer"]++;
        oo(lid, sid);
        for (auto& ch : an.ch) {
            const size_t nk = ch.t.size();
            if (!nk) continue;
            std::vector<int64_t> kt(nk);
            for (size_t k = 0; k < nk; k++) kt[k] = (int64_t)std::llround(ch.t[k] * (double)kTicks);
            std::vector<double> xyz(nk * 3);
            if (ch.path == 1) {
                double prev[3] = {0, 0, 0};
                for (size_t k = 0; k < nk; k++) {
                    double e[3];
                    quat_to_euler_xyz(&ch.v[k * 4], e);
                    if (k) euler_near(e, prev);
                    for (int c = 0; c < 3; c++) prev[c] = xyz[k * 3 + c] = e[c];
                }
            } else {
                for (size_t k = 0; k < nk * 3; k++) xyz[k] = ch.v[k];
            }
            static const char* cn_name[3] = {"T", "R", "S"};
            static const char* prop[3] = {"Lcl Translation", "Lcl Rotation", "Lcl Scaling"};
            const int64_t cnid = nid();
            FNode& cn = objects.add("AnimationCurveNode");
            cn.L(cnid).S(oname(cn_name[ch.path], "AnimCurveNode")).S("");
            FNode& cp = cn.add("Properties70");
            static const char* dn[3] = {"d|X", "d|Y", "d|Z"};
            for (int c = 0; c < 3; c++) cp.add(P(dn[c], "Number", "", "A")).D(xyz[c]);
            counts["AnimationCurveNode"]++;
            oo(cnid, lid);
            op(cnid, model_id[ch.node], prop[ch.path]);
            for (int c = 0; c < 3; c++) {
                std::vector<float> kv(nk);
                for (size_t k = 0; k < nk; k++) kv[k] = (float)xyz[k * 3 + c];
                const int64_t cid = nid();
                FNode& cu = objects.add("AnimationCurve");
                cu.L(cid).S(oname("", "AnimCurve")).S("");
                cu.add("Default").D(xyz[c]);
                cu.add("KeyVer").I(4009);
                cu.add("KeyTime").Al(kt);
                cu.add("KeyValueFloat").Af(kv);
                cu.add("KeyAttrFlags").Ai(std::vector<int32_t>{24836});        // linear, as Blender writes it
                cu.add("KeyAttrDataFloat").Af(std::vector<float>{0.0f, 0.0f, 9.419963346924634e-30f, 0.0f});
                cu.add("KeyAttrRefCount").Ai(std::vector<int32_t>{(int32_t)nk});
                counts["AnimationCurve"]++;
                op(cid, cnid, dn[c]);
            }
        }
        FNode& tk = takes.add("Take");
        tk.S(an.name);
        tk.add("FileName").S(an.name + ".tak");
        tk.add("LocalTime").L(0).L(stop);
        tk.add("ReferenceTime").L(0).L(stop);
    }

    // ---- the file
    Bytes out;
    static const char head[] = "Kaydara FBX Binary  ";
    out.insert(out.end(), head, head + 20);
    out.push_back(0); out.push_back(0x1A); out.push_back(0);
    const uint32_t version = 7400;
    out.insert(out.end(), (const uint8_t*)&version, (const uint8_t*)&version + 4);

    FNode hx("FBXHeaderExtension");
    hx.add("FBXHeaderVersion").I(1003);
    hx.add("FBXVersion").I(7400);
    hx.add("EncryptionType").I(0);
    {
        FNode& ts = hx.add("CreationTimeStamp");
        ts.add("Version").I(1000);
        ts.add("Year").I(1970); ts.add("Month").I(1); ts.add("Day").I(1);
        ts.add("Hour").I(10); ts.add("Minute").I(0); ts.add("Second").I(0); ts.add("Millisecond").I(0);
    }
    hx.add("Creator").S("Dino Crisis ME Studio");
    {
        FNode& si = hx.add("SceneInfo");
        si.S(oname("GlobalInfo", "SceneInfo")).S("UserData");
        si.add("Type").S("UserData");
        si.add("Version").I(100);
        FNode& md = si.add("MetaData");
        md.add("Version").I(100);
        for (const char* k : {"Title", "Subject", "Author", "Keywords", "Revision", "Comment"}) md.add(k).S("");
        FNode& p70 = si.add("Properties70");
        const std::string doc = path_u8(path);
        p70.add(P("DocumentUrl", "KString", "Url", "")).S(doc);
        p70.add(P("SrcDocumentUrl", "KString", "Url", "")).S(doc);
        p70.add(P("Original", "Compound", "", ""));
        p70.add(P("Original|ApplicationVendor", "KString", "", "")).S("Dino Crisis ME");
        p70.add(P("Original|ApplicationName", "KString", "", "")).S("Dino Crisis ME Studio");
        p70.add(P("Original|ApplicationVersion", "KString", "", "")).S("1");
        p70.add(P("Original|DateTime_GMT", "DateTime", "", "")).S("01/01/1970 00:00:00.000");
        p70.add(P("Original|FileName", "KString", "", "")).S(doc);
        p70.add(P("LastSaved", "Compound", "", ""));
        p70.add(P("LastSaved|ApplicationVendor", "KString", "", "")).S("Dino Crisis ME");
        p70.add(P("LastSaved|ApplicationName", "KString", "", "")).S("Dino Crisis ME Studio");
        p70.add(P("LastSaved|ApplicationVersion", "KString", "", "")).S("1");
        p70.add(P("LastSaved|DateTime_GMT", "DateTime", "", "")).S("01/01/1970 00:00:00.000");
    }
    fbx_emit(out, hx);
    // the FBX SDK accepts a file whose id and creation time go with the footer below (the values every exporter outside
    // Autodesk writes)
    static const uint8_t file_id[16] = {0x28, 0xb3, 0x2a, 0xeb, 0xb6, 0x24, 0xcc, 0xc2, 0xbf, 0xc8, 0xb0, 0x2a, 0xa9, 0x2b, 0xfc, 0xf1};
    { FNode f("FileId"); f.R(file_id, 16); fbx_emit(out, f); }
    { FNode f("CreationTime"); f.S("1970-01-01 10:00:00:000"); fbx_emit(out, f); }
    { FNode f("Creator"); f.S("Dino Crisis ME Studio"); fbx_emit(out, f); }
    {
        FNode gs("GlobalSettings");
        gs.add("Version").I(1000);
        FNode& p = gs.add("Properties70");
        p.add(P("UpAxis", "int", "Integer", "")).I(1);
        p.add(P("UpAxisSign", "int", "Integer", "")).I(1);
        p.add(P("FrontAxis", "int", "Integer", "")).I(2);
        p.add(P("FrontAxisSign", "int", "Integer", "")).I(1);
        p.add(P("CoordAxis", "int", "Integer", "")).I(0);
        p.add(P("CoordAxisSign", "int", "Integer", "")).I(1);
        p.add(P("OriginalUpAxis", "int", "Integer", "")).I(1);
        p.add(P("OriginalUpAxisSign", "int", "Integer", "")).I(1);
        p.add(P("UnitScaleFactor", "double", "Number", "")).D(100.0);          // 1 unit = 1 m, as the glTF
        p.add(P("OriginalUnitScaleFactor", "double", "Number", "")).D(100.0);
        p.add(P("AmbientColor", "ColorRGB", "Color", "")).D(0).D(0).D(0);
        p.add(P("DefaultCamera", "KString", "", "")).S("Producer Perspective");
        p.add(P("TimeMode", "enum", "", "")).I(6);                             // 30 frames per second, as the game
        p.add(P("TimeProtocol", "enum", "", "")).I(2);
        p.add(P("SnapOnFrameMode", "enum", "", "")).I(0);
        p.add(P("TimeSpanStart", "KTime", "Time", "")).L(0);
        p.add(P("TimeSpanStop", "KTime", "Time", "")).L(span ? span : kTicks);
        p.add(P("CustomFrameRate", "double", "Number", "")).D(-1);
        p.add(P("TimeMarker", "Compound", "", ""));
        p.add(P("CurrentTimeMarker", "int", "Integer", "")).I(-1);
        fbx_emit(out, gs);
    }
    {
        FNode d("Documents");
        d.add("Count").I(1);
        FNode& doc = d.add("Document");
        doc.L(nid()).S("Scene").S("Scene");
        FNode& p = doc.add("Properties70");
        p.add(P("SourceObject", "object", "", ""));
        p.add(P("ActiveAnimStackName", "KString", "", "")).S(first_stack);
        doc.add("RootNode").L(0);
        fbx_emit(out, d);
    }
    { FNode r("References"); fbx_emit(out, r); }
    {
        FNode d("Definitions");
        d.add("Version").I(100);
        int total = 1;
        for (auto& kv : counts) total += kv.second;
        d.add("Count").I(total);
        d.add("ObjectType").S("GlobalSettings").add("Count").I(1);
        static const char* order[] = {"Model", "NodeAttribute", "Geometry", "Material", "Texture", "Video", "Deformer", "Pose",
                                      "AnimationStack", "AnimationLayer", "AnimationCurveNode", "AnimationCurve"};
        for (const char* t : order) {
            auto it = counts.find(t);
            if (it == counts.end() || !it->second) continue;
            d.add("ObjectType").S(t).add("Count").I(it->second);
        }
        fbx_emit(out, d);
    }
    fbx_emit(out, objects);
    fbx_emit(out, conns);
    fbx_emit(out, takes);
    out.insert(out.end(), 13, 0);                     // the end of the top-level list
    static const uint8_t foot_id[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
    out.insert(out.end(), foot_id, foot_id + 16);
    out.insert(out.end(), 4, 0);
    size_t pad = ((out.size() + 15) & ~(size_t)15) - out.size();
    if (pad == 0) pad = 16;
    out.insert(out.end(), pad, 0);
    out.insert(out.end(), (const uint8_t*)&version, (const uint8_t*)&version + 4);
    out.insert(out.end(), 120, 0);
    static const uint8_t foot_magic[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
    out.insert(out.end(), foot_magic, foot_magic + 16);
    return write_file(path, out.data(), out.size());
}

// ================================================================ OBJ + MTL
bool write_obj(const Gltf& g, const fs::path& path) {
    Scene sc;
    if (!load_scene(g, sc)) return false;
    fs::path mtlp = path;
    mtlp.replace_extension(".mtl");
    std::string obj = "# Dino Crisis ME Studio: metres, Y up (as the glTF); the meshes in the bind pose - OBJ keeps no skeleton "
                      "and no animation\n";
    obj += "mtllib " + path_u8(mtlp.filename()) + "\n";
    std::string mtl = "# Dino Crisis ME Studio\n";
    std::set<int> written;
    int nv = 0, nt = 0, nn = 0;
    std::string body;
    for (size_t i = 0; i < sc.nodes.size(); i++) {
        const SNode& n = sc.nodes[i];
        if (n.mesh < 0 || n.mesh >= (int)sc.meshes.size()) continue;
        const SMesh& sm = sc.meshes[n.mesh];
        // a skinned mesh is already in the bind pose (its node sits at the origin); others take their node's place
        const M4& w = n.world;
        body += "o " + sm.name + "\n";
        for (auto& p : sm.prims) {
            const int v0 = nv, t0 = nt, n0 = nn;
            const int cnt = (int)p.nv();
            for (int v = 0; v < cnt; v++) {
                double q[3];
                m4_point(w, &p.pos[v * 3], q);
                if (!p.col.empty())
                    body += strfmt("v %.6f %.6f %.6f %.4f %.4f %.4f\n", q[0], q[1], q[2], p.col[v * 4], p.col[v * 4 + 1], p.col[v * 4 + 2]);
                else
                    body += strfmt("v %.6f %.6f %.6f\n", q[0], q[1], q[2]);
            }
            nv += cnt;
            if (!p.uv.empty()) {
                for (int v = 0; v < cnt; v++) body += strfmt("vt %.6f %.6f\n", p.uv[v * 2], 1.0 - p.uv[v * 2 + 1]);
                nt += cnt;
            }
            if (!p.nrm.empty()) {
                for (int v = 0; v < cnt; v++) {
                    double d[3];
                    m4_dir(w, &p.nrm[v * 3], d);
                    body += strfmt("vn %.5f %.5f %.5f\n", d[0], d[1], d[2]);
                }
                nn += cnt;
            }
            if (p.mat >= 0 && p.mat < (int)sc.mats.size()) {
                body += "usemtl " + sc.mats[p.mat].name + "\n";
                if (!written.count(p.mat)) {
                    written.insert(p.mat);
                    const SMat& m = sc.mats[p.mat];
                    mtl += "newmtl " + m.name + "\nKd 1 1 1\nd 1\nillum 1\n";
                    if (!m.uri.empty()) mtl += "map_Kd " + m.uri + "\nmap_d " + m.uri + "\n";
                    mtl += "\n";
                }
            }
            for (size_t t = 0; t + 2 < p.idx.size(); t += 3) {
                body += "f";
                for (int k = 0; k < 3; k++) {
                    const int li = (int)p.idx[t + k];
                    const int a = v0 + li + 1;
                    if (!p.uv.empty() && !p.nrm.empty()) body += strfmt(" %d/%d/%d", a, t0 + li + 1, n0 + li + 1);
                    else if (!p.uv.empty()) body += strfmt(" %d/%d", a, t0 + li + 1);
                    else if (!p.nrm.empty()) body += strfmt(" %d//%d", a, n0 + li + 1);
                    else body += strfmt(" %d", a);
                }
                body += "\n";
            }
        }
    }
    obj += body;
    return write_text(path, obj) && write_text(mtlp, mtl);
}

}  // namespace dc
