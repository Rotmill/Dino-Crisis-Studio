#include "view3d.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gl {
#define DCGL_DEF(ret, name, args) PFN_##name name = nullptr;
DCGL_FUNCS(DCGL_DEF)
#undef DCGL_DEF
bool load() {
    bool ok = true;
#define DCGL_LOAD(ret, name, args)                                         \
    name = (PFN_##name)SDL_GL_GetProcAddress("gl" #name);                  \
    if (!name) ok = false;
    DCGL_FUNCS(DCGL_LOAD)
#undef DCGL_LOAD
    return ok;
}
}  // namespace gl

namespace dc {

// ---------------------------------------------------------------- матрицы (столбцы, как в GL)
struct M4 { float m[16]; };
static M4 m4_mul(const M4& a, const M4& b) {
    M4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
static M4 perspective(float fovy, float asp, float zn, float zf) {
    float f = 1.0f / tanf(fovy / 2);
    M4 r{};
    r.m[0] = f / asp; r.m[5] = f; r.m[10] = (zf + zn) / (zn - zf); r.m[11] = -1; r.m[14] = 2 * zf * zn / (zn - zf);
    return r;
}
static M4 look_at(const float e[3], const float t[3]) {
    float f[3] = {t[0] - e[0], t[1] - e[1], t[2] - e[2]};
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (auto& v : f) v /= fl;
    float up[3] = {0, 1, 0};
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (auto& v : s) v /= sl;
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    M4 r{};
    r.m[0] = s[0]; r.m[4] = s[1]; r.m[8] = s[2];
    r.m[1] = u[0]; r.m[5] = u[1]; r.m[9] = u[2];
    r.m[2] = -f[0]; r.m[6] = -f[1]; r.m[10] = -f[2];
    r.m[12] = -(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]);
    r.m[13] = -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]);
    r.m[14] = f[0] * e[0] + f[1] * e[1] + f[2] * e[2];
    r.m[15] = 1;
    return r;
}

// координаты игры (мм, Y вниз, Z вперёд) → вид (м, Y вверх): (x, -y, -z) / 1000
static inline void to_view(float x, float y, float z, float o[3]) { o[0] = x * 0.001f; o[1] = -y * 0.001f; o[2] = -z * 0.001f; }

static const char* kVS = R"(#version 330 core
layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUV; layout(location=2) in vec4 aCol; layout(location=3) in vec3 aNrm;
uniform mat4 uMVP; uniform int uLit;
out vec2 vUV; out vec4 vCol; out float vL;
void main(){ gl_Position = uMVP*vec4(aPos,1.0); vUV=aUV; vCol=aCol;
  vL = uLit!=0 ? 0.45+0.55*max(dot(normalize(aNrm), normalize(vec3(0.35,0.85,0.4))),0.0) : 1.0; })";
static const char* kFS = R"(#version 330 core
in vec2 vUV; in vec4 vCol; in float vL; out vec4 o;
uniform sampler2D uTex; uniform int uTexOn; uniform int uColOn; uniform float uAlpha; uniform vec4 uFlat;
void main(){ vec4 t = uTexOn!=0 ? texture(uTex, vUV) : vec4(0.72,0.72,0.72,1.0);
  if (t.a < 0.5) discard;
  vec3 c = t.rgb * (uColOn!=0 ? vCol.rgb*2.0 : vec3(1.0)) * vL;
  if (uFlat.a > 0.0) c = uFlat.rgb;
  o = vec4(c, uAlpha); })";
static const char* kLVS = R"(#version 330 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aCol; uniform mat4 uMVP; out vec3 vC;
void main(){ gl_Position = uMVP*vec4(aPos,1.0); vC=aCol; })";
static const char* kLFS = R"(#version 330 core
in vec3 vC; out vec4 o; void main(){ o = vec4(vC,1.0); })";

static unsigned compile(const char* vs, const char* fs) {
    auto sh = [](GLenum t, const char* src) {
        unsigned s = gl::CreateShader(t);
        gl::ShaderSource(s, 1, &src, nullptr);
        gl::CompileShader(s);
        GLint ok = 0;
        gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024]; gl::GetShaderInfoLog(s, 1024, nullptr, log); SDL_Log("shader: %s", log); }
        return s;
    };
    unsigned p = gl::CreateProgram();
    unsigned a = sh(GL_VERTEX_SHADER, vs), b = sh(GL_FRAGMENT_SHADER, fs);
    gl::AttachShader(p, a);
    gl::AttachShader(p, b);
    gl::LinkProgram(p);
    gl::DeleteShader(a);
    gl::DeleteShader(b);
    return p;
}

View3D::~View3D() {
    if (!ok_) return;
    clear();
    gl::DeleteBuffers(1, &vbo_);
    gl::DeleteVertexArrays(1, &vao_);
    gl::DeleteBuffers(1, &lvbo_);
    gl::DeleteVertexArrays(1, &lvao_);
    if (fbo_) { gl::DeleteFramebuffers(1, &fbo_); glDeleteTextures(1, &color_); gl::DeleteRenderbuffers(1, &depth_); }
}

bool View3D::init() {
    if (ok_) return true;
    prog_ = compile(kVS, kFS);
    lprog_ = compile(kLVS, kLFS);
    gl::GenVertexArrays(1, &vao_);
    gl::GenBuffers(1, &vbo_);
    gl::BindVertexArray(vao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, p));
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, uv));
    gl::VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)offsetof(Vtx, c));
    gl::VertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, n));
    for (int i = 0; i < 4; i++) gl::EnableVertexAttribArray(i);
    gl::GenVertexArrays(1, &lvao_);
    gl::GenBuffers(1, &lvbo_);
    gl::BindVertexArray(lvao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, lvbo_);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
    gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)12);
    gl::EnableVertexAttribArray(0);
    gl::EnableVertexAttribArray(1);
    gl::BindVertexArray(0);
    ok_ = true;
    return true;
}

void View3D::forget_pages() {
    for (auto& kv : pages_) glDeleteTextures(1, &kv.second);
    pages_.clear();
    dirty_ = true;
}

void View3D::clear() {
    for (auto& kv : pages_) glDeleteTextures(1, &kv.second);
    pages_.clear();
    verts_.clear();
    batches_.clear();
    lines_.clear();
    skin_.clear();
    sv_.clear();
    sn_.clear();
    bone_links_.clear();
    bone_pts_.clear();
    ntris_ = 0;
    dirty_ = true;
}

unsigned View3D::page(const Vram& v, uint16_t tpage, uint16_t clut) {
    uint32_t key = ((uint32_t)tpage << 16) | clut;
    auto it = pages_.find(key);
    if (it != pages_.end()) return it->second;
    std::vector<uint32_t> px;
    if (tpage == 0xFFFF) px.assign(256 * 256, 0xFFFFFFFFu);   // 06.10.2026: no picture (a shape in its vertex colour)
    else v.decode_rect(tpage, clut_x(clut), clut_y(clut), 0, 0, 256, 256, px);
    unsigned t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    pages_[key] = t;
    return t;
}

void View3D::set_mesh(const std::vector<MTri>& tris, const Vram& v, bool keep_view) {
    if (keep_view) { verts_.clear(); batches_.clear(); skin_.clear(); sv_.clear(); sn_.clear(); }
    else clear();
    // порядок: непрозрачные, затем полупрозрачные; внутри — по странице/палитре
    std::vector<const MTri*> order;
    for (auto& t : tris) order.push_back(&t);
    std::stable_sort(order.begin(), order.end(), [](const MTri* a, const MTri* b) {
        bool sa = a->code & 2, sb = b->code & 2;
        if (sa != sb) return !sa;
        if (a->tpage != b->tpage) return a->tpage < b->tpage;
        return a->clut < b->clut;
    });
    for (const MTri* t : order) {
        bool semi = (t->code & 2) != 0;
        unsigned tex = page(v, t->tpage, t->clut);
        int mode = (t->tpage >> 5) & 3;
        if (batches_.empty() || batches_.back().tex != tex || batches_.back().semi != semi || batches_.back().mode != mode)
            batches_.push_back(Batch{tex, (int)verts_.size(), 0, semi, mode});
        float p[3][3];
        for (int k = 0; k < 3; k++) to_view(t->v[k].x, t->v[k].y, t->v[k].z, p[k]);
        float e1[3], e2[3], n[3];
        for (int i = 0; i < 3; i++) { e1[i] = p[1][i] - p[0][i]; e2[i] = p[2][i] - p[0][i]; }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        for (int k = 0; k < 3; k++) {
            Vtx x;
            memcpy(x.p, p[k], sizeof x.p);
            x.uv[0] = (t->v[k].u + 0.5f) / 256.0f;
            x.uv[1] = (t->v[k].v + 0.5f) / 256.0f;
            x.c[0] = t->rgb[k] & 0xFF; x.c[1] = (t->rgb[k] >> 8) & 0xFF; x.c[2] = (t->rgb[k] >> 16) & 0xFF; x.c[3] = 255;
            memcpy(x.n, n, sizeof n);
            verts_.push_back(x);
        }
        batches_.back().count += 3;
    }
    ntris_ = (int)tris.size();
    lit = false;
    if (!keep_view) fit();
    dirty_ = true;
}

void View3D::set_actor(const Model& m, const Vram& v) {
    clear();
    std::vector<const APrim*> order;
    for (auto& p : m.prims) order.push_back(&p);
    std::stable_sort(order.begin(), order.end(), [](const APrim* a, const APrim* b) {
        if (a->tpage != b->tpage) return a->tpage < b->tpage;
        return a->clut < b->clut;
    });
    for (const APrim* p : order) {
        unsigned tex = page(v, p->tpage, p->clut);
        if (batches_.empty() || batches_.back().tex != tex) batches_.push_back(Batch{tex, (int)verts_.size(), 0, false, 0});
        for (int k = 0; k < 3; k++) {
            int vi = p->vi[k] < m.verts.size() ? p->vi[k] : 0;
            Vtx x{};
            x.uv[0] = ((p->uv[k] & 0xFF) + 0.5f) / 256.0f;
            x.uv[1] = ((p->uv[k] >> 8) + 0.5f) / 256.0f;
            x.c[0] = x.c[1] = x.c[2] = 128; x.c[3] = 255;
            verts_.push_back(x);
            skin_.push_back(SkinV{vi, m.vbone[vi]});
        }
        batches_.back().count += 3;
    }
    sv_ = m.verts;
    sn_ = m.norms;
    for (size_t i = 0; i < m.bones.size(); i++)
        if (m.bones[i].parent >= 0) bone_links_.push_back({m.bones[i].parent, (int)i});
    ntris_ = (int)m.prims.size();
    lit = true;
    std::vector<BoneXf> rest;
    rest_bones(m, rest);
    pose_actor(rest);
    fit();
}

void View3D::pose_actor(const std::vector<BoneXf>& b) {
    for (size_t i = 0; i < skin_.size(); i++) {
        const SkinV& s = skin_[i];
        if (s.bone >= (int)b.size()) continue;
        const BoneXf& x = b[s.bone];
        float v[3] = {(float)sv_[s.vi][0], (float)sv_[s.vi][1], (float)sv_[s.vi][2]}, w[3];
        mulv(x.r, v, w);
        to_view(w[0] + x.t[0], w[1] + x.t[1], w[2] + x.t[2], verts_[i].p);
        float n[3] = {(float)sn_[s.vi][0], (float)sn_[s.vi][1], (float)sn_[s.vi][2]}, nn[3];
        mulv(x.r, n, nn);
        verts_[i].n[0] = nn[0]; verts_[i].n[1] = -nn[1]; verts_[i].n[2] = -nn[2];
    }
    bone_pts_.clear();
    for (auto& l : bone_links_) {
        float a[3], c[3];
        to_view(b[l.first].t[0], b[l.first].t[1], b[l.first].t[2], a);
        to_view(b[l.second].t[0], b[l.second].t[1], b[l.second].t[2], c);
        bone_pts_.insert(bone_pts_.end(), {a[0], a[1], a[2], 1.0f, 0.8f, 0.1f, c[0], c[1], c[2], 1.0f, 0.3f, 0.1f});
    }
    dirty_ = true;
}

void View3D::set_lines(const std::vector<float>& l) {
    lines_.clear();
    for (size_t i = 0; i + 5 < l.size(); i += 6) {
        float p[3];
        to_view(l[i], l[i + 1], l[i + 2], p);
        lines_.insert(lines_.end(), {p[0], p[1], p[2], l[i + 3], l[i + 4], l[i + 5]});
    }
}

void View3D::fit() {
    if (verts_.empty()) return;
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    for (auto& v : verts_)
        for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], v.p[k]); mx[k] = std::max(mx[k], v.p[k]); }
    float r = 0;
    for (int k = 0; k < 3; k++) { center_[k] = (mn[k] + mx[k]) / 2; target_[k] = center_[k]; r = std::max(r, (mx[k] - mn[k]) / 2); }
    radius_ = std::max(r, 0.05f);
    dist_ = radius_ * 2.6f;
    build_grid();
}

void View3D::fit_box(const float a[3], const float b[3]) {
    float p[3], q[3];
    to_view(a[0], a[1], a[2], p);
    to_view(b[0], b[1], b[2], q);
    float r = 0;
    for (int k = 0; k < 3; k++) {
        center_[k] = (p[k] + q[k]) / 2;
        target_[k] = center_[k];
        r = std::max(r, fabsf(q[k] - p[k]) / 2);
    }
    radius_ = std::max(r, 0.5f);
    dist_ = radius_ * 2.4f;
    build_grid();
}

void View3D::build_grid() {
    grid_.clear();
    float step = radius_ > 6 ? 1.0f : radius_ > 1.5f ? 0.5f : 0.1f;
    int n = (int)(radius_ * 1.3f / step) + 1;
    float y = 0;   // пол игры — y = 0
    for (int i = -n; i <= n; i++) {
        float c = i == 0 ? 0.45f : 0.28f;
        grid_.insert(grid_.end(), {center_[0] + i * step, y, center_[2] - n * step, c, c, c, center_[0] + i * step, y, center_[2] + n * step, c, c, c});
        grid_.insert(grid_.end(), {center_[0] - n * step, y, center_[2] + i * step, c, c, c, center_[0] + n * step, y, center_[2] + i * step, c, c, c});
    }
}

// ---------------------------------------------------------------- точка ↔ экран (правка расстановки мышью)
static bool m4_inverse(const float m[16], float o[16]) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) o[i] = inv[i] * det;
    return true;
}

bool View3D::project(float gx, float gy, float gz, float& sx, float& sy) const {
    if (vw_ <= 0 || vh_ <= 0) return false;
    float p[3];
    to_view(gx, gy, gz, p);
    float c[4];
    for (int r = 0; r < 4; r++) c[r] = mvp_[r] * p[0] + mvp_[4 + r] * p[1] + mvp_[8 + r] * p[2] + mvp_[12 + r];
    if (c[3] <= 1e-6f) return false;
    sx = (c[0] / c[3] * 0.5f + 0.5f) * vw_;
    sy = (0.5f - c[1] / c[3] * 0.5f) * vh_;
    return true;
}

bool View3D::ground(float sx, float sy, float gy, float& gx, float& gz) const {
    if (vw_ <= 0 || vh_ <= 0) return false;
    float inv[16];
    if (!m4_inverse(mvp_, inv)) return false;
    const float nx = sx / vw_ * 2 - 1, ny = 1 - sy / vh_ * 2;
    float a[3], b[3];
    for (int k = 0; k < 2; k++) {
        const float nz = k == 0 ? -1.0f : 1.0f;
        float w[4];
        for (int r = 0; r < 4; r++) w[r] = inv[r] * nx + inv[4 + r] * ny + inv[8 + r] * nz + inv[12 + r];
        if (fabsf(w[3]) < 1e-9f) return false;
        float* o = k == 0 ? a : b;
        for (int r = 0; r < 3; r++) o[r] = w[r] / w[3];
    }
    const float py = -gy * 0.001f;                    // плоскость в виде (Y вверх, м)
    const float dy = b[1] - a[1];
    if (fabsf(dy) < 1e-9f) return false;
    const float t = (py - a[1]) / dy;
    if (t < 0) return false;
    const float vx = a[0] + (b[0] - a[0]) * t, vz = a[2] + (b[2] - a[2]) * t;
    gx = vx * 1000.0f;
    gz = -vz * 1000.0f;
    return true;
}

void View3D::pan(float dx, float dy) {
    float s = dist_ * 0.0015f;
    float cy = cosf(yaw_), sy = sinf(yaw_);
    target_[0] -= dx * s * cy;
    target_[2] += dx * s * sy;
    target_[1] += dy * s;
}

void View3D::upload() {
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(Vtx)), verts_.data(), GL_DYNAMIC_DRAW);
    dirty_ = false;
}

void View3D::render(int w, int h) {
    if (!ok_ || w < 8 || h < 8) return;
    if (w != fw_ || h != fh_) {
        if (fbo_) { gl::DeleteFramebuffers(1, &fbo_); glDeleteTextures(1, &color_); gl::DeleteRenderbuffers(1, &depth_); }
        gl::GenFramebuffers(1, &fbo_);
        gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glGenTextures(1, &color_);
        glBindTexture(GL_TEXTURE_2D, color_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
        gl::GenRenderbuffers(1, &depth_);
        gl::BindRenderbuffer(GL_RENDERBUFFER, depth_);
        gl::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
        fw_ = w; fh_ = h;
    }
    if (dirty_) upload();
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);
    glClearColor(0.11f, 0.12f, 0.14f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glFrontFace(front_cw ? GL_CW : GL_CCW);
    if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    float eye[3] = {target_[0] + dist_ * cosf(pitch_) * sinf(yaw_), target_[1] + dist_ * sinf(pitch_), target_[2] + dist_ * cosf(pitch_) * cosf(yaw_)};
    M4 mvp = m4_mul(perspective(0.8f, (float)w / h, std::max(0.01f, dist_ * 0.01f), dist_ * 50 + radius_ * 10), look_at(eye, target_));
    memcpy(mvp_, mvp.m, sizeof mvp_);
    vw_ = w; vh_ = h;
    // линии: сетка пола, накладка
    auto draw_lines = [&](const std::vector<float>& L, bool depth) {
        if (L.empty()) return;
        if (!depth) glDisable(GL_DEPTH_TEST);
        gl::UseProgram(lprog_);
        gl::UniformMatrix4fv(gl::GetUniformLocation(lprog_, "uMVP"), 1, GL_FALSE, mvp.m);
        gl::BindVertexArray(lvao_);
        gl::BindBuffer(GL_ARRAY_BUFFER, lvbo_);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(L.size() * 4), L.data(), GL_DYNAMIC_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)(L.size() / 6));
        if (!depth) glEnable(GL_DEPTH_TEST);
    };
    if (grid) draw_lines(grid_, true);
    gl::UseProgram(prog_);
    gl::UniformMatrix4fv(gl::GetUniformLocation(prog_, "uMVP"), 1, GL_FALSE, mvp.m);
    gl::Uniform1i(gl::GetUniformLocation(prog_, "uLit"), lit ? 1 : 0);
    gl::Uniform1i(gl::GetUniformLocation(prog_, "uTexOn"), textured ? 1 : 0);
    gl::Uniform1i(gl::GetUniformLocation(prog_, "uColOn"), vcolors && !lit ? 1 : 0);
    gl::Uniform1i(gl::GetUniformLocation(prog_, "uTex"), 0);
    GLint aloc = gl::GetUniformLocation(prog_, "uAlpha"), floc = gl::GetUniformLocation(prog_, "uFlat");
    gl::Uniform4f(floc, 0, 0, 0, 0);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindVertexArray(vao_);
    for (int pass = 0; pass < 2; pass++) {
        for (auto& b : batches_) {
            if (b.semi != (pass == 1)) continue;
            glBindTexture(GL_TEXTURE_2D, b.tex);
            if (b.semi) {
                glEnable(GL_BLEND);
                glDepthMask(GL_FALSE);
                float a = 0.5f;
                if (b.mode == 1) { glBlendFunc(GL_ONE, GL_ONE); gl::BlendEquation(GL_FUNC_ADD); a = 1; }
                else if (b.mode == 2) { glBlendFunc(GL_ONE, GL_ONE); gl::BlendEquation(GL_FUNC_REVERSE_SUBTRACT); a = 1; }
                else if (b.mode == 3) { glBlendFunc(GL_SRC_ALPHA, GL_ONE); gl::BlendEquation(GL_FUNC_ADD); a = 0.25f; }
                else { glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); gl::BlendEquation(GL_FUNC_ADD); }
                gl::Uniform1f(aloc, a);
            } else {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
                gl::Uniform1f(aloc, 1);
            }
            glDrawArrays(GL_TRIANGLES, b.first, b.count);
        }
    }
    gl::BlendEquation(GL_FUNC_ADD);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    if (wire) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        gl::Uniform1i(gl::GetUniformLocation(prog_, "uTexOn"), 0);
        gl::Uniform4f(floc, 0.95f, 0.85f, 0.3f, 1);
        gl::Uniform1f(aloc, 1);
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1, -1);
        for (auto& b : batches_) glDrawArrays(GL_TRIANGLES, b.first, b.count);
        glDisable(GL_POLYGON_OFFSET_LINE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    draw_lines(lines_, false);
    if (skeleton) draw_lines(bone_pts_, false);
    gl::BindVertexArray(0);
    gl::UseProgram(0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

}  // namespace dc
