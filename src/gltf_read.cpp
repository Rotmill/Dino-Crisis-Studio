#include "gltf_read.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dc {

static bool b64(const std::string& s, size_t from, Bytes& out) {
    static int8_t tab[256];
    static bool init = false;
    if (!init) {
        memset(tab, -1, sizeof tab);
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) tab[(uint8_t)a[i]] = (int8_t)i;
        init = true;
    }
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = from; i < s.size(); i++) {
        int v = tab[(uint8_t)s[i]];
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return true;
}

bool GDoc::load(const fs::path& p, std::string& err) {
    Bytes f;
    if (!read_file(p, f)) { err = "cannot read"; return false; }
    Bytes glb_bin;
    try {
        if (f.size() >= 20 && memcmp(f.data(), "glTF", 4) == 0) {
            uint32_t jl = le32(&f[12]);
            if (20 + (size_t)jl > f.size() || memcmp(&f[16], "JSON", 4) != 0) { err = "bad GLB"; return false; }
            j = json::parse(f.begin() + 20, f.begin() + 20 + jl);
            size_t o = 20 + jl;
            if (o + 8 <= f.size() && memcmp(&f[o + 4], "BIN\0", 4) == 0) {
                uint32_t bl = le32(&f[o]);
                glb_bin.assign(f.begin() + o + 8, f.begin() + std::min(f.size(), o + 8 + bl));
            }
        } else {
            j = json::parse(f.begin(), f.end());
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    buffers.clear();
    if (j.count("buffers")) {
        for (auto& b : j["buffers"]) {
            Bytes data;
            if (!b.count("uri")) data = glb_bin;
            else {
                std::string uri = b["uri"];
                if (uri.rfind("data:", 0) == 0) {
                    size_t c = uri.find(',');
                    if (c != std::string::npos) b64(uri, c + 1, data);
                } else {
                    std::string u;   // %20 и т.п.
                    for (size_t i = 0; i < uri.size(); i++) {
                        if (uri[i] == '%' && i + 2 < uri.size()) { u += (char)strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
                        else u += uri[i];
                    }
                    if (!read_file(p.parent_path() / u8path(u), data)) { err = "cannot read buffer " + u; return false; }
                }
            }
            buffers.push_back(std::move(data));
        }
    }
    // анимации
    anims.clear();
    if (j.count("animations")) {
        for (auto& a : j["animations"]) {
            GAnim ga;
            ga.name = a.value("name", "");
            auto& smp = a["samplers"];
            for (auto& ch : a["channels"]) {
                if (!ch.count("target") || !ch["target"].count("node")) continue;
                int node = ch["target"]["node"];
                std::string path = ch["target"].value("path", "");
                if (path != "translation" && path != "rotation") continue;
                const json& s = smp.at((size_t)ch.at("sampler").get<int>());
                GTrack t;
                int nc_in = 0;
                if (!floats(s.at("input"), t.t, nc_in) || !floats(s.at("output"), t.v, t.nc)) continue;
                std::string ip = s.value("interpolation", "LINEAR");
                t.interp = ip == "STEP" ? 1 : ip == "CUBICSPLINE" ? 2 : 0;
                if (!t.t.empty()) ga.duration = std::max(ga.duration, t.t.back());
                (path == "translation" ? ga.tr : ga.rot)[node] = std::move(t);
            }
            anims.push_back(std::move(ga));
        }
    }
    return true;
}

bool GDoc::floats(int ai, std::vector<float>& out, int& nc) const {
    out.clear();
    if (!j.count("accessors") || ai < 0 || ai >= (int)j["accessors"].size()) return false;
    const json& a = j["accessors"][ai];
    std::string type = a.value("type", "SCALAR");
    nc = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
    int ct = a.value("componentType", 5126);
    size_t count = a.value("count", 0);
    bool norm = a.value("normalized", false);
    int cs = (ct == 5126 || ct == 5125) ? 4 : (ct == 5122 || ct == 5123) ? 2 : 1;
    out.assign(count * nc, 0.0f);
    if (!a.count("bufferView")) return nc > 0;   // одни нули (sparse не поддерживаем)
    const json& bv = j["bufferViews"][a["bufferView"].get<int>()];
    int bi = bv.value("buffer", 0);
    if (bi < 0 || bi >= (int)buffers.size()) return false;
    const Bytes& b = buffers[bi];
    size_t off = (size_t)bv.value("byteOffset", 0) + (size_t)a.value("byteOffset", 0);
    size_t stride = bv.value("byteStride", 0);
    if (!stride) stride = (size_t)cs * nc;
    for (size_t i = 0; i < count; i++) {
        for (int c = 0; c < nc; c++) {
            size_t o = off + i * stride + (size_t)c * cs;
            if (o + cs > b.size()) return false;
            const uint8_t* q = &b[o];
            float v = 0;
            switch (ct) {
            case 5126: memcpy(&v, q, 4); break;
            case 5120: v = (int8_t)q[0]; if (norm) v = std::max(v / 127.0f, -1.0f); break;
            case 5121: v = q[0]; if (norm) v /= 255.0f; break;
            case 5122: v = (int16_t)le16(q); if (norm) v = std::max(v / 32767.0f, -1.0f); break;
            case 5123: v = le16(q); if (norm) v /= 65535.0f; break;
            case 5125: v = (float)le32(q); break;
            }
            out[i * nc + c] = v;
        }
    }
    return true;
}

int GDoc::node_by_name(const std::string& name) const {
    if (!j.count("nodes")) return -1;
    for (size_t i = 0; i < j["nodes"].size(); i++)
        if (j["nodes"][i].value("name", "") == name) return (int)i;
    return -1;
}

void GDoc::node_rest(int n, float t[3], float q[4]) const {
    t[0] = t[1] = t[2] = 0;
    q[0] = q[1] = q[2] = 0; q[3] = 1;
    if (n < 0 || !j.count("nodes") || n >= (int)j["nodes"].size()) return;
    const json& nd = j["nodes"][n];
    if (nd.count("translation")) for (int k = 0; k < 3; k++) t[k] = nd["translation"][k];
    if (nd.count("rotation")) for (int k = 0; k < 4; k++) q[k] = nd["rotation"][k];
}

void GTrack::eval(float time, float* out, bool quat) const {
    const size_t n = t.size();
    auto val = [&](size_t k) -> const float* { return &v[(interp == 2 ? k * 3 + 1 : k) * nc]; };
    if (n == 0) return;
    if (time <= t[0] || n == 1) { memcpy(out, val(0), nc * sizeof(float)); return; }
    if (time >= t[n - 1]) { memcpy(out, val(n - 1), nc * sizeof(float)); return; }
    size_t k = std::upper_bound(t.begin(), t.end(), time) - t.begin() - 1;
    float dt = t[k + 1] - t[k];
    float f = dt > 0 ? (time - t[k]) / dt : 0;
    const float *a = val(k), *b = val(k + 1);
    if (interp == 1) { memcpy(out, a, nc * sizeof(float)); return; }
    if (interp == 2) {
        const float* ao = &v[(k * 3 + 2) * nc];   // выходная касательная a
        const float* bi = &v[((k + 1) * 3) * nc];   // входная касательная b
        float f2 = f * f, f3 = f2 * f;
        float h00 = 2 * f3 - 3 * f2 + 1, h10 = f3 - 2 * f2 + f, h01 = -2 * f3 + 3 * f2, h11 = f3 - f2;
        for (int c = 0; c < nc; c++) out[c] = h00 * a[c] + h10 * dt * ao[c] + h01 * b[c] + h11 * dt * bi[c];
    } else if (quat) {
        float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        float s = d < 0 ? -1.0f : 1.0f;
        d = fabsf(d);
        float w0, w1;
        if (d > 0.9995f) { w0 = 1 - f; w1 = f; }
        else {
            float th = acosf(d), sn = sinf(th);
            w0 = sinf((1 - f) * th) / sn;
            w1 = sinf(f * th) / sn;
        }
        for (int c = 0; c < 4; c++) out[c] = w0 * a[c] + w1 * s * b[c];
    } else {
        for (int c = 0; c < nc; c++) out[c] = a[c] + (b[c] - a[c]) * f;
    }
    if (quat) {
        float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
        if (l > 0) for (int c = 0; c < 4; c++) out[c] /= l;
    }
}

}  // namespace dc
