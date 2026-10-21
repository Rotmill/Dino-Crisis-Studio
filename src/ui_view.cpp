// Вьюпорт по типу выбранного: 3D (меш, модель с движением, коллизия, камеры, триггеры), картинка, звук, видео, таблицы.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "i18n.h"
#include "placement.h"
#include "script.h"
#include "ui.h"
#include "ui_names.h"

namespace dc {

// ---------------------------------------------------------------- накладка комнаты
void room_overlay(const RoomInfo& r, int mask, std::vector<float>& L) {
    auto seg = [&](float x0, float y0, float z0, float x1, float y1, float z1, float cr, float cg, float cb) {
        L.insert(L.end(), {x0, y0, z0, cr, cg, cb, x1, y1, z1, cr, cg, cb});
    };
    if (mask & 1) {
        for (int row = 0; row < 17; row++) {
            for (auto& f : r.coll.floors[row]) {
                if (!f.sx || !f.sz) continue;
                float xs[4] = {(float)f.x, (float)f.x + f.sx, (float)f.x + f.sx, (float)f.x};
                float zs[4] = {(float)f.z, (float)f.z, (float)f.z + f.sz, (float)f.z + f.sz};
                float hs[4] = {(float)f.h[0], (float)f.h[1], (float)f.h[3], (float)f.h[2]};
                for (int k = 0; k < 4; k++) seg(xs[k], hs[k] - 5, zs[k], xs[(k + 1) % 4], hs[(k + 1) % 4] - 5, zs[(k + 1) % 4], 0.25f, 0.95f, 0.35f);
            }
            for (auto& w : r.coll.walls[row]) {
                float xs[4] = {(float)w.x, (float)w.x + w.sx, (float)w.x + w.sx, (float)w.x};
                float zs[4] = {(float)w.z, (float)w.z, (float)w.z + w.sz, (float)w.z + w.sz};
                for (int k = 0; k < 4; k++) {
                    seg(xs[k], -5, zs[k], xs[(k + 1) % 4], -5, zs[(k + 1) % 4], 1.0f, 0.3f, 0.25f);
                    seg(xs[k], -900, zs[k], xs[(k + 1) % 4], -900, zs[(k + 1) % 4], 1.0f, 0.3f, 0.25f);
                    seg(xs[k], -5, zs[k], xs[k], -900, zs[k], 1.0f, 0.3f, 0.25f);
                }
            }
        }
    }
    if (mask & 2) {
        for (auto& cam : r.cameras) {
            const uint8_t* p = cam.raw;
            for (int k = 0; k < 4; k++) {
                int a = k, b = (k + 1) % 4;
                seg(s16(p + 4 + a * 4), -12, s16(p + 6 + a * 4), s16(p + 4 + b * 4), -12, s16(p + 6 + b * 4), 0.35f, 0.65f, 1.0f);
            }
            seg(s16(p + 52), s16(p + 54), s16(p + 56), s16(p + 44), s16(p + 46), s16(p + 48), 0.55f, 0.85f, 1.0f);
        }
    }
    if (mask & 4) {
        for (auto& t : r.triggers) {
            const uint8_t* p = t.raw;
            for (int k = 0; k < 4; k++) {
                int a = k, b = (k + 1) % 4;
                seg(s16(p + a * 4), -18, s16(p + 2 + a * 4), s16(p + b * 4), -18, s16(p + 2 + b * 4), 1.0f, 0.85f, 0.2f);
                seg(s16(p + a * 4), -18, s16(p + 2 + a * 4), s16(p + a * 4), -700, s16(p + 2 + a * 4), 1.0f, 0.85f, 0.2f);
            }
        }
    }
}

void placement_overlay(const json& arr, int what, std::vector<float>& L) {
    auto seg = [&](float x0, float y0, float z0, float x1, float y1, float z1, float cr, float cg, float cb) {
        L.insert(L.end(), {x0, y0, z0, cr, cg, cb, x1, y1, z1, cr, cg, cb});
    };
    static const float col[4][3] = {{1.0f, 0.25f, 0.85f}, {1.0f, 0.85f, 0.2f}, {0.35f, 0.8f, 1.0f}, {0.75f, 0.75f, 0.75f}};
    const float* c = col[what & 3];
    for (auto& e : arr) {
        if (what == 0) {
            if (!e.count("x") || !e.count("z")) continue;   // op 59 — место берётся из ростера этапа
            float x = e["x"].get<int>(), y = e.value("y", 0), z = e["z"].get<int>();
            float a = e.value("rotation", 0) * 6.2831853f / 4096.0f;
            seg(x, y, z, x, y - 1800, z, c[0], c[1], c[2]);
            for (int k = 0; k < 12; k++) {   // круг 400 мм у ног
                float a0 = k * 0.5235988f, a1 = (k + 1) * 0.5235988f;
                seg(x + 400 * cosf(a0), y - 5, z + 400 * sinf(a0), x + 400 * cosf(a1), y - 5, z + 400 * sinf(a1), c[0], c[1], c[2]);
            }
            seg(x, y - 5, z, x + 700 * sinf(a), y - 5, z + 700 * cosf(a), 1, 1, 1);   // куда смотрит
            continue;
        }
        if (!e.count("zone")) continue;
        const json& q = e["zone"];
        float y = -425.0f * e.value("floor", 0) - 15;
        float cx = 0, cz = 0;
        for (int k = 0; k < 4; k++) {
            float x0 = q[k][0].get<int>(), z0 = q[k][1].get<int>(), x1 = q[(k + 1) % 4][0].get<int>(), z1 = q[(k + 1) % 4][1].get<int>();
            seg(x0, y, z0, x1, y, z1, c[0], c[1], c[2]);
            cx += x0 / 4; cz += z0 / 4;
        }
        if (what == 1 || what == 2) seg(cx, y, cz, cx, y - (what == 1 ? 600 : 2000), cz, c[0], c[1], c[2]);
    }
}

bool placement_point(const json& e, bool items, float& x, float& y, float& z) {
    if (!items) {
        if (!e.count("x") || !e.count("z") || !e["x"].is_number() || !e["z"].is_number()) return false;
        x = (float)e["x"].get<int>(); y = (float)e.value("y", 0); z = (float)e["z"].get<int>();
        return true;
    }
    if (!e.count("zone") || e["zone"].size() != 4) return false;
    x = z = 0;
    for (int k = 0; k < 4; k++) { x += e["zone"][k][0].get<int>() * 0.25f; z += e["zone"][k][1].get<int>() * 0.25f; }
    y = -425.0f * e.value("floor", 0);
    return true;
}

void placement_overlay_sel(const json& arr, int what, int sel, std::vector<float>& L) {
    for (int i = 0; i < (int)arr.size(); i++) {
        const json& e = arr[i];
        if (e.value("removed", false)) continue;
        json one = json::array({e});
        const size_t from = L.size();
        placement_overlay(one, what, L);
        if (i == sel) {                                   // выбранная — белым, и кольцо пошире
            for (size_t k = from; k + 5 < L.size(); k += 6) { L[k + 3] = 1; L[k + 4] = 1; L[k + 5] = 1; }
            float x, y, z;
            if (placement_point(e, what == 1, x, y, z))
                for (int k = 0; k < 16; k++) {
                    const float a0 = k * 0.3926991f, a1 = (k + 1) * 0.3926991f;
                    L.insert(L.end(), {x + 650 * cosf(a0), y - 8, z + 650 * sinf(a0), 1, 1, 1, x + 650 * cosf(a1), y - 8, z + 650 * sinf(a1), 1, 1, 1});
                }
        }
    }
}

// ---------------------------------------------------------------- поза движения
void App::apply_pose(const FileContent& c, const ActorPart& a, const MotionSet& ms, const Motion& mo) {
    const Bytes& blk = ms.ext ? *ms.ext : c.blocks[ms.block].data;
    float total = std::max(1, mo.ticks) / 30.0f;
    float t = fmodf(anim_time_, total);
    if (t < 0) t += total;
    float acc = 0;
    size_t k = 0;
    for (; k < mo.keys.size(); k++) {
        float d = mo.keys[k].hold / 30.0f;
        if (t < acc + d) break;
        acc += d;
    }
    if (k >= mo.keys.size()) k = mo.keys.size() - 1;
    float frac = std::clamp((t - acc) / std::max(1e-4f, mo.keys[k].hold / 30.0f), 0.0f, 1.0f);
    size_t k2 = (k + 1) % mo.keys.size();
    int nb = (int)a.model.bones.size();
    FramePose f0 = read_frame(blk, mo.data + (uint32_t)mo.keys[k].frame * mo.fsize, nb);
    FramePose f1 = read_frame(blk, mo.data + (uint32_t)mo.keys[k2].frame * mo.fsize, nb);
    FramePose f = f0;
    auto lerp = [&](int a0, int a1) { return (int)lroundf(a0 + (a1 - a0) * frac); };
    auto lerp_ang = [&](int a0, int a1) {
        int d = ((a1 - a0 + 2048) & 4095) - 2048;   // кратчайшая дуга (func_80066630)
        return (int)lroundf(a0 + d * frac) & 4095;
    };
    f.tx = (int16_t)lerp(f0.tx, f1.tx); f.ty = (int16_t)lerp(f0.ty, f1.ty); f.tz = (int16_t)lerp(f0.tz, f1.tz);
    f.ry = (int16_t)lerp_ang(f0.ry, f1.ry);
    for (size_t i = 0; i < f.ang.size(); i++) f.ang[i] = (uint16_t)lerp_ang(f0.ang[i], f1.ang[i]);
    std::vector<BoneXf> bx;
    pose_bones(a.model, f, bx, false);
    v3d_.pose_actor(bx);
}

// ---------------------------------------------------------------- вьюпорт
static void center_text(const char* s) {
    ImVec2 av = ImGui::GetContentRegionAvail(), ts = ImGui::CalcTextSize(s);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (av.x - ts.x) * 0.5f, ImGui::GetCursorPosY() + av.y * 0.45f));
    ImGui::TextDisabled("%s", s);
}

void App::viewport() {
    if (!sel_.f) {
        // 06.10.2026: a folder of the tree picked - what it holds and its Export
        const ExportReq* pr = sel_folder_.empty() ? nullptr : picked_req();
        if (!pr) return;
        const std::string what = strfmt(T("%s: %d to export", "%s: %d к выгрузке"), pr->label.c_str(), pr->count());
        ImVec2 av = ImGui::GetContentRegionAvail();
        const float bw = 220;
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (av.x - ImGui::CalcTextSize(what.c_str()).x) * 0.5f, ImGui::GetCursorPosY() + av.y * 0.42f));
        ImGui::TextUnformatted(what.c_str());
        ImGui::SetCursorPosX((av.x - bw) * 0.5f);
        ImGui::BeginDisabled(job_ != nullptr);
        if (ImGui::Button(T("Export", "Выгрузить"), ImVec2(bw, 34))) start_export_req(ExportReq(*pr));
        ImGui::EndDisabled();
        return;
    }
    Loaded* L = load(sel_.f);
    if (!L) { center_text(T("Cannot read the file", "Файл не читается")); return; }
    build_selection();
    const FileContent& c = L->c;
    switch (sel_.k) {
    case K_MESH: case K_MODEL: case K_MOTION: case K_COLL: case K_CAMS: case K_TRIGS:
    case K_ENEMIES: case K_ITEMS: case K_DOORS: case K_ZONES: case K_PIECES: view_3d(c); break;
    case K_EXE_TEXTS: case K_EXE_ITEMS: case K_EXE_DOORS: case K_ROSTER: view_table(c); break;
    case K_TEXTURE: case K_PTEX: case K_IMAGE: case K_UPLOAD: case K_VRAM: view_image(); break;
    case K_SAMPLE: case K_XA: case K_SONG: view_audio(); break;
    case K_VIDEO: view_video(); break;
    case K_TEXTS: view_texts(c); break;
    case K_SCRIPT: if (c.is_room) view_3d(c); else view_script(c); break;   // 10.10.2026: the scenario next to the room
    default: view_file(c); break;
    }
}

// кнопка «играть/пауза» и шкала времени внизу вьюпорта; true — шкалу двигали
static bool transport(bool& playing, double& t, double total) {
    bool moved = false;
    if (ImGui::Button(playing ? "||" : ">", ImVec2(40, 0))) playing = !playing;
    ImGui::SameLine();
    float ft = (float)t;
    ImGui::SetNextItemWidth(-1);
    int sec = (int)t, tot = (int)total;
    std::string lbl = total < 60 ? strfmt("%.2f / %.2f", t, total) : strfmt("%d:%02d / %d:%02d", sec / 60, sec % 60, tot / 60, tot % 60);
    if (ImGui::SliderFloat("##t", &ft, 0, (float)std::max(0.01, total), lbl.c_str())) {
        t = ft;
        moved = true;
    }
    return moved;
}

void App::view_3d(const FileContent& c) {
    const ActorPart* actor = nullptr;
    const MotionSet* ms = nullptr;
    const Motion* mo = nullptr;
    if (sel_.k == K_MOTION && sel_.a >= 0 && sel_.a < (int)c.actors.size()) {
        actor = &c.actors[sel_.a];
        int si = sel_.b / 10000, mi = sel_.b % 10000;
        if (si >= 0 && si < (int)c.motion_sets.size() && mi < (int)c.motion_sets[si].motions.size()) {
            ms = &c.motion_sets[si];
            mo = &ms->motions[mi];
        }
    }
    ImVec2 sz = ImGui::GetContentRegionAvail();
    const bool panel = (sel_.k == K_ENEMIES || sel_.k == K_ITEMS || sel_.k == K_PIECES || sel_.k == K_SCRIPT) && c.is_room;   // editor on the right
    const float pw = panel ? (sel_.k == K_SCRIPT ? std::min(620.0f, sz.x * 0.5f) : std::min(420.0f, sz.x * 0.45f)) : 0;
    if (panel) sz.x -= pw + ImGui::GetStyle().ItemSpacing.x;
    if (mo) sz.y -= ImGui::GetFrameHeightWithSpacing();
    if (mo) apply_pose(c, *actor, *ms, *mo);
    v3d_.render((int)sz.x, (int)sz.y);
    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)v3d_.texture()), sz, ImVec2(0, 1), ImVec2(1, 0));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 img0 = ImGui::GetItemRectMin();
    // 06.10.2026 (игрок: «в самом вьюпорте, чтобы можно было перетаскивать предметы и спавнеры врагов, трансформить в
    // пространстве вьюпорта, добавлять новые»): щелчок по метке — выбрать, тянуть — по полу, Ctrl+колесо или Q/E — поворот
    // врага, Delete — убрать, правая кнопка по полу — добавить сюда
    bool edit_took = false;
    if (panel && sel_.k != K_SCRIPT) edit_took = viewport_edit(hov, img0.x, img0.y);
    else if (panel) edit_took = scenario_viewport(hov, img0.x, img0.y);   // 14.10.2026: zones and places of the scenario
    if (hov) {
        ImGuiIO& io = ImGui::GetIO();
        if (!edit_took && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) v3d_.orbit(io.MouseDelta.x, io.MouseDelta.y);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) v3d_.pan(io.MouseDelta.x, io.MouseDelta.y);
        if (io.MouseWheel != 0 && !(panel && io.KeyCtrl)) v3d_.zoom(io.MouseWheel);
        if (!edit_took && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) v3d_.fit();
    }
    if (mo) {
        double total = std::max(1, mo->ticks) / 30.0;
        double t = fmod((double)anim_time_, total);
        if (transport(anim_play_, t, total)) { anim_time_ = (float)t; anim_play_ = false; }
    }
    if (panel) {
        ImGui::SameLine();
        ImGui::BeginChild("place", ImVec2(pw, sz.y), ImGuiChildFlags_Borders);
        if (Loaded* L = load(sel_.f)) {
            ensure_edit(*L);
            if (sel_.k == K_PIECES) pieces_panel(*L);
            else if (sel_.k == K_SCRIPT) scenario_panel(*L);
            else placement_panel(*L, sel_.k == K_ITEMS);
        }
        ImGui::EndChild();
    }
}

void App::view_image() {
    if (!img_tex_) return;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), cs = ImGui::GetContentRegionAvail();
    float fit = std::min((cs.x - 20) / img_w_, (cs.y - 20) / img_h_);
    if (img_zoom_ <= 0) {
        img_zoom_ = std::max(0.25f, fit >= 1 ? floorf(fit) : fit);
        img_x_ = img_y_ = 0;
    }
    ImGui::InvisibleButton("canvas", cs, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 c0(p0.x + cs.x * 0.5f + img_x_ - img_w_ * img_zoom_ * 0.5f, p0.y + cs.y * 0.5f + img_y_ - img_h_ * img_zoom_ * 0.5f);
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
        float nz = std::clamp(img_zoom_ * powf(1.25f, io.MouseWheel), 0.1f, 64.0f);
        float mx = (io.MousePos.x - c0.x) / img_zoom_, my = (io.MousePos.y - c0.y) / img_zoom_;
        img_x_ += mx * (img_zoom_ - nz);
        img_y_ += my * (img_zoom_ - nz);
        img_zoom_ = nz;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        img_x_ += io.MouseDelta.x;
        img_y_ += io.MouseDelta.y;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) img_zoom_ = 0;
    c0 = ImVec2(p0.x + cs.x * 0.5f + img_x_ - img_w_ * img_zoom_ * 0.5f, p0.y + cs.y * 0.5f + img_y_ - img_h_ * img_zoom_ * 0.5f);
    ImVec2 c1(c0.x + img_w_ * img_zoom_, c0.y + img_h_ * img_zoom_);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + cs.x, p0.y + cs.y), true);
    const float cell = 12;   // шахматка под прозрачным
    ImVec2 a(std::max(c0.x, p0.x), std::max(c0.y, p0.y)), b(std::min(c1.x, p0.x + cs.x), std::min(c1.y, p0.y + cs.y));
    for (float y = a.y; y < b.y; y += cell)
        for (float x = a.x; x < b.x; x += cell) {
            int ix = (int)((x - c0.x) / cell), iy = (int)((y - c0.y) / cell);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, b.x), std::min(y + cell, b.y)),
                              ((ix + iy) & 1) ? IM_COL32(58, 60, 64, 255) : IM_COL32(44, 46, 50, 255));
        }
    dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)img_tex_), c0, c1);
    dl->PopClipRect();
}

void App::view_audio() {
    if (pcm_.empty()) { center_text(T("No sound", "Звука нет")); return; }
    const double secs = (double)(pcm_.size() / pcm_ch_) / pcm_rate_;
    bool playing = audio_.playing();
    double t = playing ? audio_.position() : play_from_;
    bool want = playing;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), cs = ImGui::GetContentRegionAvail();
    cs.y -= ImGui::GetFrameHeightWithSpacing();
    ImGui::InvisibleButton("wave", cs);
    if (ImGui::IsItemClicked()) {   // клик по волне — играть с этого места
        play_from_ = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / cs.x, 0.0f, 1.0f) * secs;
        audio_.play(pcm_, pcm_ch_, pcm_rate_, play_from_);
        playing = want = true;
        t = play_from_;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + cs.x, p0.y + cs.y), IM_COL32(24, 26, 29, 255));
    float mid = p0.y + cs.y * 0.5f, amp = cs.y * 0.45f;
    dl->AddLine(ImVec2(p0.x, mid), ImVec2(p0.x + cs.x, mid), IM_COL32(60, 62, 66, 255));
    size_t frames = pcm_.size() / std::max(1, pcm_ch_);
    int cols = std::max(1, (int)cs.x);
    for (int x = 0; x < cols && frames; x++) {
        size_t a = (size_t)((double)x / cols * frames), b = std::max(a + 1, (size_t)((double)(x + 1) / cols * frames));
        int mn = 0, mx = 0;
        for (size_t i = a; i < b && i < frames; i++)
            for (int ch = 0; ch < pcm_ch_; ch++) {
                int v = pcm_[i * pcm_ch_ + ch];
                mn = std::min(mn, v); mx = std::max(mx, v);
            }
        dl->AddLine(ImVec2(p0.x + x, mid - mx / 32768.0f * amp), ImVec2(p0.x + x, mid - mn / 32768.0f * amp + 1), IM_COL32(220, 120, 60, 255));
    }
    float px = p0.x + (float)(t / secs) * cs.x;
    dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p0.y + cs.y), IM_COL32(255, 255, 255, 200), 2);
    if (transport(want, t, secs)) {
        play_from_ = t;
        if (playing) audio_.play(pcm_, pcm_ch_, pcm_rate_, play_from_);
    } else if (want != playing) {
        if (want) audio_.play(pcm_, pcm_ch_, pcm_rate_, play_from_ >= secs - 0.01 ? 0 : play_from_);
        else { play_from_ = audio_.position(); audio_.stop(); }
    } else if (!playing && play_from_ > 0 && t >= secs - 0.01) {
        play_from_ = 0;
    }
}

void App::view_video() {
    if (vframes_.empty()) { center_text(T("No video", "Видео нет")); return; }
    const double total = vframes_.size() / kStrFps;
    if (vplay_ && !pcm_.empty()) {
        if (audio_.playing()) vtime_ = audio_.position();
        else vplay_ = false;   // звук кончился
    }
    if (vtime_ >= total) { vtime_ = total; vplay_ = false; }
    int fi = std::clamp((int)(vtime_ * kStrFps), 0, (int)vframes_.size() - 1);
    if (fi != vshown_) {
        std::vector<uint32_t> px;
        if (str_decode(disc_, vframes_[fi], px)) set_image(vframes_[fi].w, vframes_[fi].h, px.data());
        vshown_ = fi;
    }
    ImVec2 p0 = ImGui::GetCursorScreenPos(), cs = ImGui::GetContentRegionAvail();
    cs.y -= ImGui::GetFrameHeightWithSpacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + cs.x, p0.y + cs.y), IM_COL32(0, 0, 0, 255));
    if (img_tex_) {
        float s = std::min(cs.x / img_w_, cs.y / img_h_);
        ImVec2 sz(img_w_ * s, img_h_ * s), a(p0.x + (cs.x - sz.x) * 0.5f, p0.y + (cs.y - sz.y) * 0.5f);
        dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)img_tex_), a, ImVec2(a.x + sz.x, a.y + sz.y));
    }
    ImGui::InvisibleButton("frame", cs);
    bool click = ImGui::IsItemClicked();
    bool want = vplay_ != click;
    double t = vtime_;
    if (transport(want, t, total)) {
        vtime_ = t;
        if (vplay_ && !pcm_.empty()) audio_.play(pcm_, pcm_ch_, pcm_rate_, vtime_);
    } else if (want != vplay_) {
        vplay_ = want;
        if (vplay_) {
            if (vtime_ >= total - 0.05) vtime_ = 0;
            if (!pcm_.empty()) audio_.play(pcm_, pcm_ch_, pcm_rate_, vtime_);
        } else {
            if (!pcm_.empty()) vtime_ = audio_.position();
            audio_.stop();
        }
    }
}

// таблицы программы игры (тексты, предметы, сцены дверей) и ростер этапа — только просмотр; правка — в JSON выгрузки
void App::view_table(const FileContent& c) {
    (void)c;
    struct Col { const char* key; const char* en; const char* ru; float w; };
    std::vector<Col> cols;
    std::vector<const json*> rows;
    std::vector<std::string> groups;   // подпись группы строки (пул текстов, вариант ростера)
    if (sel_.k == K_EXE_TEXTS) {
        cols = {{"index", "#", "#", 50}, {"text", "Text", "Текст", 0}};
        for (auto& p : table_["pools"])
            for (auto& t : p["texts"]) { rows.push_back(&t); groups.push_back(p["pool"].get<std::string>() == "main" ? "" : T("system", "системный")); }
    } else if (sel_.k == K_EXE_ITEMS) {
        cols = {{"id", "id", "id", 50}, {"name", "Name", "Имя", 0}, {"class", "Class", "Класс", 70}, {"name_text", "Text", "Текст", 70},
                {"description_text", "Description", "Описание", 90}};
        for (auto& t : table_["items"]) { rows.push_back(&t); groups.push_back(""); }
    } else if (sel_.k == K_EXE_DOORS) {
        cols = {{"index", "#", "#", 50}, {"name", "Name", "Имя", 110}, {"handler_name", "Action", "Действие", 120}, {"file", "File", "Файл", 60},
                {"camera_eye", "Camera", "Камера", 0}};
        for (auto& t : table_["scenes"]) { rows.push_back(&t); groups.push_back(""); }
    } else {
        cols = {{"index", "#", "#", 40}, {"room", "Room", "Комната", 90}, {"x", "x", "x", 70}, {"y", "y", "y", 70}, {"z", "z", "z", 70},
                {"hp", "HP", "HP", 60}, {"rotation", "Turn", "Поворот", 70}};
        int vi = 0;
        for (auto& v : table_["variants"]) {
            for (auto& d : v["dinosaurs"])
                if (!d.value("empty", false)) { rows.push_back(&d); groups.push_back(table_["variants"].size() > 1 ? strfmt("%d", vi + 1) : ""); }
            vi++;
        }
    }
    if (rows.empty()) { center_text(T("Nothing here", "Пусто")); return; }
    bool grp = std::any_of(groups.begin(), groups.end(), [](const std::string& s) { return !s.empty(); });
    int nc = (int)cols.size() + (grp ? 1 : 0);
    if (!ImGui::BeginTable("tbl", nc, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) return;
    if (grp) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
    for (auto& col : cols)
        ImGui::TableSetupColumn(T(col.en, col.ru), col.w > 0 ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch, col.w);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            ImGui::TableNextRow();
            if (grp) { ImGui::TableNextColumn(); ImGui::TextDisabled("%s", groups[r].c_str()); }
            for (auto& col : cols) {
                ImGui::TableNextColumn();
                const json& v = (*rows[r]).value(col.key, json());
                std::string s = v.is_string() ? v.get<std::string>() : v.is_null() ? std::string() : v.dump();
                for (auto& ch : s) if (ch == '\n') ch = ' ';
                ImGui::TextUnformatted(s.c_str());
            }
        }
    ImGui::EndTable();
}

void App::view_file(const FileContent& c) {
    (void)c;
    center_text(strfmt("%s   %u %s", sel_.f->name.c_str(), sel_.f->size, T("bytes", "байт")).c_str());
}

// ---------------------------------------------------------------- строка состояния: что выбрано
std::string App::describe(const FileContent& c) {
    const DiscFile& f = *sel_.f;
    std::string s = f.stem + "  ·  ";
    auto secs = [](double v) { return strfmt("%.2f %s", v, T("s", "с")); };
    switch (sel_.k) {
    case K_MESH:
        if (sel_.a >= 0 && sel_.a < (int)c.meshes.size())
            s += mesh_label(c.meshes[sel_.a].kind) + strfmt("  ·  %d %s", (int)c.meshes[sel_.a].tris.size(), T("triangles", "треугольников"));
        break;
    case K_MODEL:
    case K_MOTION:
        if (sel_.a >= 0 && sel_.a < (int)c.actors.size()) {
            const ActorPart& a = c.actors[sel_.a];
            s += actor_label(a.kind) + strfmt("  ·  %d %s  ·  %d %s", (int)a.model.bones.size(), T("bones", "костей"), (int)a.model.prims.size(),
                                              T("triangles", "треугольников"));
            int si = sel_.b / 10000, mi = sel_.b % 10000;
            if (sel_.k == K_MOTION && si >= 0 && si < (int)c.motion_sets.size() && mi < (int)c.motion_sets[si].motions.size())
                s += strfmt("  ·  %s %s_%02d  ·  ", T("motion", "движение"), c.motion_sets[si].name.c_str(), mi) +
                     secs(c.motion_sets[si].motions[mi].ticks / 30.0);
        }
        break;
    case K_TEXTURE: s += strfmt("%s %d  ·  %d x %d", T("texture", "текстура"), sel_.a, img_w_, img_h_); break;
    case K_PTEX: {
        Loaded* L = load(sel_.f);
        const auto* cr = L ? &part_crops(*L, sel_.a) : nullptr;
        if (cr && sel_.b >= 0 && sel_.b < (int)cr->size()) s += (*cr)[sel_.b].file_name() + strfmt("  ·  %d x %d", img_w_, img_h_);
        break;
    }
    case K_IMAGE: s += strfmt("%d x %d", img_w_, img_h_); break;
    case K_UPLOAD: case K_VRAM: s += strfmt("%s  ·  %d x %d", T("video memory", "видеопамять"), img_w_, img_h_); break;
    case K_SAMPLE: s += strfmt("%s %03d  ·  ", T("sound", "звук"), sel_.b) + secs((double)pcm_.size() / pcm_rate_) + strfmt("  ·  %d %s", pcm_rate_, T("Hz", "Гц")); break;
    case K_SONG: s += strfmt("%s %02d  ·  ", T("song", "мелодия"), sel_.a) + secs((double)(pcm_.size() / std::max(1, pcm_ch_)) / pcm_rate_); break;
    case K_XA:
        if (sel_.a >= 0 && sel_.a < (int)c.xa.size())
            s += strfmt("%s %02d  ·  ", T("channel", "канал"), c.xa[sel_.a].chan) + secs((double)(pcm_.size() / std::max(1, pcm_ch_)) / pcm_rate_) +
                 strfmt("  ·  %d %s", pcm_rate_, T("Hz", "Гц"));
        break;
    case K_VIDEO:
        if (!vframes_.empty()) {
            int t = (int)(vframes_.size() / kStrFps);
            s += strfmt("%d x %d  ·  %d:%02d  ·  %d %s", vframes_[0].w, vframes_[0].h, t / 60, t % 60, (int)vframes_.size(), T("frames", "кадров"));
        }
        break;
    case K_COLL: {
        int nf = 0, nw = 0;
        for (int i = 0; i < 17; i++) { nf += (int)c.room.coll.floors[i].size(); nw += (int)c.room.coll.walls[i].size(); }
        s += strfmt("%s: %d, %s: %d", T("floors", "полов"), nf, T("walls", "стен"), nw);
        break;
    }
    case K_CAMS: s += strfmt("%s: %d", T("cameras", "камер"), (int)c.room.cameras.size()); break;
    case K_TRIGS: s += strfmt("%s: %d", T("footstep zones", "зон звука шагов"), (int)c.room.triggers.size()); break;
    case K_ENEMIES: case K_ITEMS: case K_DOORS: case K_ZONES: {
        Loaded* L = load(sel_.f);
        if (!L) break;
        ensure_placement(*L);
        ensure_edit(*L);
        json a = json::array();
        for (auto& e : sel_.k == K_ENEMIES ? L->ed.enemies : sel_.k == K_ITEMS ? L->ed.items : sel_.k == K_DOORS ? L->doors : L->zones)
            if (!e.value("removed", false)) a.push_back(e);
        const char* w = sel_.k == K_ENEMIES ? T("enemies", "врагов") : sel_.k == K_ITEMS ? T("items", "предметов")
                        : sel_.k == K_DOORS ? T("doors", "дверей") : T("zones", "зон");
        s += strfmt("%s: %d", w, (int)a.size());
        break;
    }
    case K_PIECES: {
        Loaded* L = load(sel_.f);
        int n = 0;
        if (L) {
            ensure_edit(*L);
            for (int i = 0; i < (int)L->ed.pieces.size(); i++)
                n += L->ed.pieces[i].value("placed", false) && L->ed.pieces[i].value("lead", i) == i ? 1 : 0;
        }
        s += strfmt("placed pieces: %d", n);
        break;
    }
    case K_EXE_TEXTS: s += T("texts of the game program", "тексты программы игры"); break;
    case K_EXE_ITEMS: s += T("item table", "таблица предметов"); break;
    case K_EXE_DOORS: s += T("door, stairs and lift scenes", "сцены дверей, лестниц и лифтов"); break;
    case K_ROSTER: s += T("dinosaurs at the start of the stage", "динозавры на начало этапа"); break;
    case K_TEXTS: s += strfmt("%s: %d", T("texts", "текстов"), (int)c.room.strings.size()); break;
    case K_SCRIPT: s += strfmt("%s: %d", T("subroutines", "подпрограмм"), (int)c.room.script_dir.size()); break;
    default: s += strfmt("%u %s", f.size, T("bytes", "байт")); break;
    }
    return s;
}

}  // namespace dc
