// Небольшой писатель glTF 2.0 (.gltf + .bin), изображения — внешними PNG (относительные пути).
#pragma once
#include "core.h"
#include "../deps/json/json.hpp"

namespace dc {

using json = nlohmann::json;

struct Gltf {
    json j = json::object();
    Bytes bin;
    Gltf();
    int view(const void* data, size_t n, int target = 0);
    int acc_f(const float* d, size_t count, int ncomp, bool minmax = false, int target = 0);
    int acc_u16(const uint16_t* d, size_t count, int ncomp, int target = 0);
    int acc_u32(const uint32_t* d, size_t count, int target = 0);
    int acc_u8n(const uint8_t* d, size_t count, int ncomp, int target = 0);   // нормированные байты (цвета)
    int acc_u8(const uint8_t* d, size_t count, int ncomp, int target = 0);    // индексы костей
    int image(const std::string& uri);
    int texture(int image);
    int material(const std::string& name, int tex, bool blend, bool double_sided = true);
    int node(const json& n);
    bool save(const fs::path& gltf_path);   // .bin рядом с тем же именем
};

// пути для URI: относительный, прямые слэши
std::string rel_uri(const fs::path& from_dir, const fs::path& target);

}  // namespace dc
