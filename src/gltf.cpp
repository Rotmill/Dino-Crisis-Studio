#include "gltf.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace dc {

Gltf::Gltf() {
    j["asset"] = {{"version", "2.0"}, {"generator", "Dino Crisis ME Studio"}};
    j["scene"] = 0;
    j["scenes"] = json::array({json{{"nodes", json::array()}}});
    j["nodes"] = json::array();
    j["meshes"] = json::array();
    j["accessors"] = json::array();
    j["bufferViews"] = json::array();
}

int Gltf::view(const void* data, size_t n, int target) {
    while (bin.size() % 4) bin.push_back(0);
    size_t off = bin.size();
    bin.insert(bin.end(), (const uint8_t*)data, (const uint8_t*)data + n);
    json v = {{"buffer", 0}, {"byteOffset", off}, {"byteLength", n}};
    if (target) v["target"] = target;
    j["bufferViews"].push_back(v);
    return (int)j["bufferViews"].size() - 1;
}

static const char* type_of(int n) {
    switch (n) {
    case 1: return "SCALAR";
    case 2: return "VEC2";
    case 3: return "VEC3";
    case 4: return "VEC4";
    case 16: return "MAT4";
    default: return "SCALAR";
    }
}

int Gltf::acc_f(const float* d, size_t count, int nc, bool minmax, int target) {
    int v = view(d, count * nc * 4, target);
    json a = {{"bufferView", v}, {"componentType", 5126}, {"count", count}, {"type", type_of(nc)}};
    if (minmax && count) {
        std::vector<float> mn(nc, FLT_MAX), mx(nc, -FLT_MAX);
        for (size_t i = 0; i < count; i++)
            for (int k = 0; k < nc; k++) { mn[k] = std::min(mn[k], d[i * nc + k]); mx[k] = std::max(mx[k], d[i * nc + k]); }
        a["min"] = mn;
        a["max"] = mx;
    }
    j["accessors"].push_back(a);
    return (int)j["accessors"].size() - 1;
}

int Gltf::acc_u16(const uint16_t* d, size_t count, int nc, int target) {
    int v = view(d, count * nc * 2, target);
    j["accessors"].push_back({{"bufferView", v}, {"componentType", 5123}, {"count", count}, {"type", type_of(nc)}});
    return (int)j["accessors"].size() - 1;
}

int Gltf::acc_u32(const uint32_t* d, size_t count, int target) {
    int v = view(d, count * 4, target);
    j["accessors"].push_back({{"bufferView", v}, {"componentType", 5125}, {"count", count}, {"type", "SCALAR"}});
    return (int)j["accessors"].size() - 1;
}

int Gltf::acc_u8n(const uint8_t* d, size_t count, int nc, int target) {
    // атрибуты вершин: шаг выровнен на 4 байта
    std::vector<uint8_t> pad;
    const int stride = (nc + 3) & ~3;
    pad.assign(count * stride, 0);
    for (size_t i = 0; i < count; i++) memcpy(&pad[i * stride], d + i * nc, nc);
    while (bin.size() % 4) bin.push_back(0);
    size_t off = bin.size();
    bin.insert(bin.end(), pad.begin(), pad.end());
    json vw = {{"buffer", 0}, {"byteOffset", off}, {"byteLength", pad.size()}, {"byteStride", stride}};
    if (target) vw["target"] = target;
    j["bufferViews"].push_back(vw);
    int v = (int)j["bufferViews"].size() - 1;
    j["accessors"].push_back({{"bufferView", v}, {"componentType", 5121}, {"normalized", true}, {"count", count}, {"type", type_of(nc)}});
    return (int)j["accessors"].size() - 1;
}

int Gltf::acc_u8(const uint8_t* d, size_t count, int nc, int target) {
    int v = view(d, count * nc, target);
    j["accessors"].push_back({{"bufferView", v}, {"componentType", 5121}, {"count", count}, {"type", type_of(nc)}});
    return (int)j["accessors"].size() - 1;
}

int Gltf::image(const std::string& uri) {
    if (!j.count("images")) j["images"] = json::array();
    for (size_t i = 0; i < j["images"].size(); i++)
        if (j["images"][i]["uri"] == uri) return (int)i;
    j["images"].push_back({{"uri", uri}});
    return (int)j["images"].size() - 1;
}

int Gltf::texture(int img) {
    if (!j.count("textures")) {
        j["textures"] = json::array();
        j["samplers"] = json::array({json{{"magFilter", 9728}, {"minFilter", 9728}, {"wrapS", 33071}, {"wrapT", 33071}}});
    }
    for (size_t i = 0; i < j["textures"].size(); i++)
        if (j["textures"][i]["source"] == img) return (int)i;
    j["textures"].push_back({{"source", img}, {"sampler", 0}});
    return (int)j["textures"].size() - 1;
}

int Gltf::material(const std::string& name, int tex, bool blend, bool ds) {
    if (!j.count("materials")) j["materials"] = json::array();
    json m = {{"name", name}, {"doubleSided", ds}};
    json pbr = {{"metallicFactor", 0.0}, {"roughnessFactor", 0.9}};
    if (tex >= 0) pbr["baseColorTexture"] = {{"index", tex}};
    m["pbrMetallicRoughness"] = pbr;
    if (blend) m["alphaMode"] = "BLEND";
    else { m["alphaMode"] = "MASK"; m["alphaCutoff"] = 0.5; }
    j["materials"].push_back(m);
    return (int)j["materials"].size() - 1;
}

int Gltf::node(const json& n) {
    j["nodes"].push_back(n);
    return (int)j["nodes"].size() - 1;
}

bool Gltf::save(const fs::path& p) {
    fs::path binp = p;
    binp.replace_extension(".bin");
    j["buffers"] = json::array({json{{"uri", path_u8(binp.filename())}, {"byteLength", bin.size()}}});
    if (j["meshes"].empty()) j.erase("meshes");
    std::string s = j.dump(1);
    return write_file(binp, bin.data(), bin.size()) && write_text(p, s);
}

std::string rel_uri(const fs::path& from_dir, const fs::path& target) {
    std::error_code ec;
    fs::path r = fs::relative(target, from_dir, ec);
    if (ec || r.empty()) r = target;
    std::string s = r.generic_u8string();
    std::string out;
    for (char c : s) {
        if (c == ' ') out += "%20";
        else out += c;
    }
    return out;
}

}  // namespace dc
