// Чтение glTF 2.0 (.gltf с .bin или data:, .glb): узлы, аксессоры, анимации — для конвертера движений.
#pragma once
#include "gltf.h"

namespace dc {

struct GTrack {
    std::vector<float> t, v;   // времена и значения (для CUBICSPLINE — по три на ключ: вход, значение, выход)
    int nc = 0;                // компонент в значении: 3 перенос, 4 поворот
    int interp = 0;            // 0 LINEAR, 1 STEP, 2 CUBICSPLINE
    bool empty() const { return t.empty(); }
    void eval(float time, float* out, bool quat) const;
};

struct GAnim {
    std::string name;
    std::map<int, GTrack> tr, rot;   // по узлу: перенос, поворот
    float duration = 0;
};

struct GDoc {
    json j;
    std::vector<Bytes> buffers;
    std::vector<GAnim> anims;
    bool load(const fs::path& p, std::string& err);
    bool floats(int accessor, std::vector<float>& out, int& ncomp) const;
    int node_by_name(const std::string& name) const;   // −1 — нет
    void node_rest(int node, float t[3], float q[4]) const;
};

}  // namespace dc
