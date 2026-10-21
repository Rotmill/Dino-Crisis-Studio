// 3D-вид: меши комнат/дверей/оружия и скелетные модели; рисует в свою текстуру (показ — ImGui::Image).
#pragma once
#include <cmath>
#include <map>
#include <vector>

#include "core.h"
#include "gfx.h"

namespace dc {

class View3D {
public:
    ~View3D();
    bool init();
    void clear();
    // 06.10.2026: the decoded pages go (the video memory under a page/palette changed: a kind of another room put in its place)
    void forget_pages();
    void set_mesh(const std::vector<MTri>& tris, const Vram& v, bool keep_view = false);   // keep_view: camera and textures stay
    void set_actor(const Model& m, const Vram& v);
    void pose_actor(const std::vector<BoneXf>& bones);     // скелет в текущей позе (родные координаты)
    void set_lines(const std::vector<float>& xyz_rgb);      // накладка: пары точек (x,y,z, r,g,b) в координатах игры
    void render(int w, int h);
    unsigned texture() const { return color_; }
    void fit();
    // 06.10.2026: fit to a box in game coordinates (mm, Y down) - a room by its walkable floor, not by the sky around it
    void fit_box(const float mn[3], const float mx[3]);
    void orbit(float dx, float dy) { yaw_ -= dx * 0.01f; pitch_ += dy * 0.01f; if (pitch_ > 1.5f) pitch_ = 1.5f; if (pitch_ < -1.5f) pitch_ = -1.5f; }
    void pan(float dx, float dy);
    void zoom(float steps) { dist_ *= powf(0.88f, steps); if (dist_ < 0.05f) dist_ = 0.05f; }
    // лицевая сторона полигонов игры в нашем виде (x, -y, -z) — по часовой (проверено: изнутри комнаты видно стены)
    bool textured = true, vcolors = true, wire = false, lit = true, grid = true, skeleton = false, cull = false, front_cw = true;
    int tris() const { return ntris_; }
    // 06.10.2026: правка расстановки мышью — точка игры (мм, Y вниз) → пиксель вида последнего кадра (от левого верхнего угла);
    // пиксель → точка на плоскости игры y = gy (луч камеры). false — за камерой / мимо плоскости
    bool project(float gx, float gy, float gz, float& sx, float& sy) const;
    bool ground(float sx, float sy, float gy, float& gx, float& gz) const;
    float size_m() const { return radius_ * 2; }

private:
    struct Vtx { float p[3]; float uv[2]; uint8_t c[4]; float n[3]; };
    struct Batch { unsigned tex; int first, count; bool semi; int mode; };
    unsigned page(const Vram& v, uint16_t tpage, uint16_t clut);
    void upload();
    void build_grid();
    bool ok_ = false;
    unsigned prog_ = 0, lprog_ = 0, vao_ = 0, vbo_ = 0, lvao_ = 0, lvbo_ = 0;
    unsigned fbo_ = 0, color_ = 0, depth_ = 0;
    int fw_ = 0, fh_ = 0;
    std::vector<Vtx> verts_;
    std::vector<Batch> batches_;
    std::map<uint32_t, unsigned> pages_;
    std::vector<float> lines_, grid_;
    int ntris_ = 0;
    // скелетная модель: исходные данные вершин
    struct SkinV { int vi; int bone; };
    std::vector<SkinV> skin_;
    std::vector<std::array<int16_t, 3>> sv_, sn_;
    std::vector<std::pair<int, int>> bone_links_;
    std::vector<float> bone_pts_;
    float center_[3] = {0, 0, 0}, radius_ = 1;
    float yaw_ = 0.6f, pitch_ = 0.35f, dist_ = 5, target_[3] = {0, 0, 0};
    float mvp_[16] = {};                 // матрица последнего кадра (project / ground)
    int vw_ = 0, vh_ = 0;
    bool dirty_ = false;
};

}  // namespace dc
