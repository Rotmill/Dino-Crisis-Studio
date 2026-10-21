// Дерево слева: разделы — как папки mods и source; в каждом — содержимое игры ПО ОДНОМУ РАЗУ (каталог library.h):
// персонаж, оружие, предмет, звук, который лежит в данных игры во многих файлах, — одна запись; подмена из mods игра узнаёт
// по содержимому и ставит везде. Фильтр — по подписям (русские и английские), папкам и файлам, без учёта регистра.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imgui.h>

#include <algorithm>
#include <map>

#include "i18n.h"
#include "placement.h"
#include "ui.h"
#include "anim_packs.h"
#include "ui_names.h"

namespace dc {

static std::wstring lowerw(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    if (!w.empty()) CharLowerBuffW(&w[0], (DWORD)w.size());
    return w;
}

bool App::fmatch(std::initializer_list<std::string> parts) const {
    if (flt_.empty()) return true;
    for (auto& p : parts)
        if (lowerw(p).find(flt_) != std::wstring::npos) return true;
    return false;
}

// 06.10.2026: the right-click menu of a node is opened from its name and from the grey hint after it (BeginPopupContextItem
// after the hint caught only the hint: right-clicking a name showed nothing - the player never found Export there)
static std::string ctx_id(const char* id) { return std::string("##ctx_") + id; }

// узел-лист (выбор по клику); menu - правая кнопка открывает меню ctx_id(id)
static bool leaf(const char* id, const std::string& label, const std::string& hint, bool selected, bool menu = false) {
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) fl |= ImGuiTreeNodeFlags_Selected;
    ImGui::TreeNodeEx(id, fl, "%s", label.c_str());
    bool clicked = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();
    if (menu) ImGui::OpenPopupOnItemClick(ctx_id(id).c_str(), ImGuiPopupFlags_MouseButtonRight);
    if (!hint.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hint.c_str());
        if (ImGui::IsItemClicked()) clicked = true;
        if (menu) ImGui::OpenPopupOnItemClick(ctx_id(id).c_str(), ImGuiPopupFlags_MouseButtonRight);
    }
    return clicked;
}

// ветка; clicked - щелчок по имени или подписи (не по стрелке); menu - как у leaf
static bool branch(const char* id, const std::string& label, const std::string& hint, bool selected, bool* clicked = nullptr,
                   bool menu = false) {
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) fl |= ImGuiTreeNodeFlags_Selected;
    const ImGuiID nid = ImGui::GetID(id);
    bool op = ImGui::TreeNodeEx(id, fl, "%s", label.c_str());
    // 06.10.2026 (player: "clicking the name must open it too, not only the arrow"): a click on the name opens or closes the
    // branch as well; the caller still picks the item on that click
    bool lc = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();
    if (lc) ImGui::GetStateStorage()->SetInt(nid, op ? 0 : 1);
    if (menu) ImGui::OpenPopupOnItemClick(ctx_id(id).c_str(), ImGuiPopupFlags_MouseButtonRight);
    if (!hint.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hint.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) lc = true;
        if (menu) ImGui::OpenPopupOnItemClick(ctx_id(id).c_str(), ImGuiPopupFlags_MouseButtonRight);
    }
    if (clicked) *clicked = lc;
    return op;
}

// раздел (папка mods и подпись); при фильтре — раскрыт. req - что в нём (щелчок выбирает раздел, правая кнопка - Export)
bool App::section(const char* key, const char* folder, const char* en, const char* ru, const ExportReq* req) {
    if (!flt_.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    else if (start_open_.find("," + std::string(key) + ",") != std::string::npos) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    const std::string fkey = std::string("sec:") + key;
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (req && sel_folder_ == fkey) fl |= ImGuiTreeNodeFlags_Selected;
    bool op = ImGui::TreeNodeEx(key, fl, "%s", folder);
    bool lc = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();
    if (req) ImGui::OpenPopupOnItemClick(ctx_id(key).c_str(), ImGuiPopupFlags_MouseButtonRight);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T(en, ru));
    if (req) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) lc = true;
        ImGui::OpenPopupOnItemClick(ctx_id(key).c_str(), ImGuiPopupFlags_MouseButtonRight);
        ExportReq& r = folder_req_[fkey];
        r = *req;
        if (r.label.empty()) r.label = folder;
        if (lc) pick_folder(fkey);
        if (ImGui::BeginPopup(ctx_id(key).c_str())) { export_menu(r); ImGui::EndPopup(); }
    }
    return op;
}

// группа внутри раздела (Dinosaurs, Handgun, Music...): как раздел - щелчок выбирает её, правая кнопка - Export
bool App::folder_branch(const std::string& key, const char* id, const std::string& label, const std::string& hint, const ExportReq& req) {
    ExportReq& r = folder_req_[key];
    r = req;
    if (r.label.empty()) r.label = label;
    bool clicked = false;
    const bool op = branch(id, label, hint, sel_folder_ == key, &clicked, true);
    if (clicked) pick_folder(key);
    if (ImGui::BeginPopup(ctx_id(id).c_str())) { export_menu(r); ImGui::EndPopup(); }
    return op;
}

// where a picture of an owner of tree_tex_owned goes under source: a<actor>, m<mesh>, r<room stem>
static std::string owner_dir(const Library& lib, const std::string& owner) {
    if (owner.size() < 2) return "textures";
    if (owner[0] == 'a') { const int i = atoi(owner.c_str() + 1); if (i >= 0 && i < (int)lib.actors.size()) return "characters/" + lib.actors[i].id; }
    if (owner[0] == 'm') { const int i = atoi(owner.c_str() + 1); if (i >= 0 && i < (int)lib.meshes.size()) return lib.meshes[i].cat + "/" + lib.meshes[i].id; }
    if (owner[0] == 'r') return "rooms/" + owner.substr(1);
    return "textures";
}

// ---------------------------------------------------------------- каталог
void App::start_library() {
    if (lib_job_ && lib_job_->joinable()) lib_job_->join();
    lib_job_.reset();
    lib_.ready = false;
    lib_fx_.clear();
    lib_ins_.clear();
    lib_done_ = 0;
    lib_total_ = 0;
    if (!disc_.is_open()) return;
    flags_ready_ = false;
    lib_job_ = std::make_unique<std::thread>([this] {
        lib_.build(disc_, &lib_done_, &lib_total_);
        // 06.10.2026: flags in use (a new item / enemy gets a free one) and the kinds each stage can run
        PlacementCtx c;
        c.ensure(disc_);
        flags7_ = c.used7;
        flags45_ = c.used45;
        flags_bits7_ = c.bits7;
        for (int s = 1; s <= 9; s++) {
            stage_kinds_[s] = 0x7u;
            const DiscFile* f = disc_.find(strfmt("ST%d.BIN", s));
            Bytes ovl;
            if (f && disc_.read_file(*f, ovl)) stage_kinds_[s] = stage_kinds(ovl);
        }
        flags_ready_ = true;
    });
}

int App::ext_set(Loaded& L, int si) {
    if (si < 0 || si >= (int)lib_.sets.size()) return -1;
    auto it = L.ext_set.find(si);
    if (it != L.ext_set.end()) return it->second;
    const LibSet& s = lib_.sets[si];
    int idx = -1;
    if (s.at.f == L.c.file) {                 // набор этого же файла
        idx = s.at.part;
        if (idx >= 0 && idx < (int)L.c.motion_sets.size()) L.c.motion_sets[idx].name = s.id;
    }
    else {
        FileContent fc;
        if (analyze(disc_, *s.at.f, fc) && s.at.part >= 0 && s.at.part < (int)fc.motion_sets.size()) {
            MotionSet ms = fc.motion_sets[s.at.part];
            L.ext_blocks.push_back(std::make_unique<Bytes>(std::move(fc.blocks[ms.block].data)));
            ms.ext = L.ext_blocks.back().get();
            ms.name = s.id;
            idx = (int)L.c.motion_sets.size();
            L.c.motion_sets.push_back(ms);
        }
    }
    L.ext_set[si] = idx;
    return idx;
}

const std::vector<TexCrop>& App::part_crops(Loaded& L, int part) {
    auto it = L.part_crops.find(part);
    if (it != L.part_crops.end()) return it->second;
    std::vector<TexCrop> v;
    if (part < 10000 && part >= 0 && part < (int)L.c.actors.size()) v = crops_for_model(L.c.vram, L.c.actors[part].model);
    else if (part >= 10000 && part - 10000 < (int)L.c.meshes.size()) v = crops_for_mesh(L.c.vram, L.c.meshes[part - 10000].tris);
    return L.part_crops.emplace(part, std::move(v)).first->second;
}

void App::tree_part_textures(const DiscFile* f, int part) {
    Loaded* L = load(f);
    if (!L) return;
    const auto& cr = part_crops(*L, part);
    if (cr.empty()) return;
    const std::string dir = tex_dir(f, part);
    auto menu_all = [&] {   // the pictures are copied only when the menu is open (a frame must not copy megabytes)
        if (!ImGui::BeginPopup(ctx_id("tex").c_str())) return;
        ExportReq all;
        all.label = T("Textures", "Текстуры");
        for (auto& c : cr) all.textures.push_back({dir, c});
        export_menu(all);
        ImGui::EndPopup();
    };
    const bool op = branch("tex", T("Textures", "Текстуры"), strfmt("%d", (int)cr.size()), false, nullptr, true);
    menu_all();
    if (!op) return;
    for (size_t i = 0; i < cr.size(); i++) {
        Sel s{f, K_PTEX, part, (int)i};
        const std::string id = strfmt("t%d", (int)i);
        if (leaf(id.c_str(), strfmt("%02d", (int)i), strfmt("%dx%d", cr[i].w(), cr[i].h()), sel_ == s, true)) select(s);
        if (ImGui::BeginPopup(ctx_id(id.c_str()).c_str())) {
            ExportReq one;
            one.label = cr[i].file_name();
            one.textures.push_back({dir, cr[i]});
            export_menu(one);
            ImGui::EndPopup();
        }
    }
    ImGui::TreePop();
}

// ---------------------------------------------------------------- персонажи
// 06.10.2026 (игрок: «у Регины в каждом костюме повторил все её анимации… одна вещь — одно место, логично размещено»;
// «в персонажах human — это варианты конкретных персонажей: Гейл, Кирк…»). Персонаж — один узел: модели (костюмы, варианты)
// внутри, движения — один раз, у владельца (library.cpp own_motions), текстура — у первого по дереву (tex_owner). Движения
// сцен, где играют вместе Регина и люди, — в комнате (её скрипт решает, кто что играет).
static std::string char_title(const LibChar& c, const Library& lib) {
    if (!c.en.empty()) return T(c.en.c_str(), c.ru.c_str());
    if (!c.actors.empty() && (c.kind == "prop" || c.kind == "model")) return actor_title(lib.actors[c.actors[0]]);
    return actor_label(c.kind);
}
static std::string model_title(const LibActor& a) {
    if (a.kind == "regina") {
        if (a.id.rfind("regina_costume", 0) == 0) return strfmt(g_lang == LANG_RU ? "Костюм %s" : "Costume %s", a.id.substr(14).c_str());
        return T("Main costume", "Основной костюм");
    }
    if (a.var_en == "unarmed") return T("Unarmed", "Без оружия");
    if (a.var_en == "wounded") return T("Wounded", "Раненый");
    if (!a.var_en.empty()) return strfmt(g_lang == LANG_RU ? "Вариант %s" : "Variant %s", a.var_en.c_str());
    return T("Model", "Модель");
}

// текстуры, которые принадлежат этой вещи (в других местах той же картинки нет — она у первого владельца)
void App::tree_tex_owned(const std::vector<LibTex>& tex, const std::string& owner) {
    std::vector<const LibTex*> own;
    for (auto& t : tex) {
        auto it = lib_.tex_owner.find(t.hash);
        if (it != lib_.tex_owner.end() && it->second == owner) own.push_back(&t);
    }
    if (own.empty()) return;
    const std::string dir = owner_dir(lib_, owner);
    // the picture of an owned texture (nullptr - its file does not read)
    auto crop_of = [&](const LibTex& t, int* ci_out) -> const TexCrop* {
        Loaded* L = load(t.f);
        if (!L) return nullptr;
        const auto& cr = part_crops(*L, t.part);
        for (size_t k = 0; k < cr.size(); k++)
            if (cr[k].hash == t.hash) { if (ci_out) *ci_out = (int)k; return &cr[k]; }
        return nullptr;
    };
    const bool op = branch("tex", T("Textures", "Текстуры"), strfmt("%d", (int)own.size()), false, nullptr, true);
    if (ImGui::BeginPopup(ctx_id("tex").c_str())) {   // copies of the pictures only while the menu is open
        ExportReq all;
        all.label = T("Textures", "Текстуры");
        for (const LibTex* t : own) if (const TexCrop* c = crop_of(*t, nullptr)) all.textures.push_back({dir, *c});
        export_menu(all);
        ImGui::EndPopup();
    }
    if (!op) return;
    for (size_t i = 0; i < own.size(); i++) {
        const LibTex& t = *own[i];
        int ci = -1;
        const TexCrop* c = crop_of(t, &ci);
        if (!c) continue;
        Sel s{t.f, K_PTEX, t.part, ci};
        const std::string id = strfmt("t%d", (int)i);
        if (leaf(id.c_str(), strfmt("%02d", (int)i), strfmt("%dx%d  %s", c->w(), c->h(), t.f->stem.c_str()), sel_ == s, true)) {
            select(s);
            pick_req_ = ExportReq();
            pick_req_.label = c->file_name();
            pick_req_.textures.push_back({dir, *c});   // into the owner's folder (the same name the export of the owner writes)
        }
        if (ImGui::BeginPopup(ctx_id(id.c_str()).c_str())) {
            ExportReq one;
            one.label = c->file_name();
            one.textures.push_back({dir, *c});
            export_menu(one);
            ImGui::EndPopup();
        }
    }
    ImGui::TreePop();
}

// движения (по группам наборов: общие, оружие, комнаты, переходы), показ — на модели on
void App::tree_motions(const LibActor& on, const std::vector<int>& mots, const char* id, const char* en, const char* ru,
                       const ExportReq& req) {
    if (mots.empty()) return;
    if (start_open_.find(",motions,") != std::string::npos) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    // the motions go out with the model that carries them (its glTF) or with the room's scene - Export of any of them takes that
    const bool mop = branch(id, T(en, ru), strfmt("%d", (int)mots.size()), false, nullptr, true);
    if (ImGui::BeginPopup(ctx_id(id).c_str())) { export_menu(req); ImGui::EndPopup(); }
    if (!mop) return;
    std::map<int, std::vector<int>> by_set;   // набор первого места → движения
    std::vector<int> order;
    int on_ai = -1;                           // the model that shows them (the owner's main model, Regina for scenes)
    for (size_t i = 0; i < lib_.actors.size(); i++) if (&lib_.actors[i] == &on) on_ai = (int)i;
    for (int m : mots) {
        const int si = lib_.motions[m].set;
        if (!by_set.count(si)) order.push_back(si);
        by_set[si].push_back(m);
    }
    const char* gname[4][2] = {{"Common", "Общие"}, {"Weapons", "Оружие"}, {"Rooms", "Комнаты"}, {"Doors", "Переходы"}};
    std::vector<int> grp[4];
    for (int si : order) {
        const LibSet& s = lib_.sets[si];
        int g = weapon_set_file(s.at.f->stem) ? 1 : s.at.f->cat == "rooms" ? 2 : s.at.f->cat == "doors" ? 3 : 0;
        grp[g].push_back(si);
    }
    int ngroups = 0;
    for (auto& g : grp) ngroups += !g.empty();
    for (int g = 0; g < 4; g++) {
        if (grp[g].empty()) continue;
        int n = 0;
        for (int si : grp[g]) n += (int)by_set[si].size();
        const std::string gid = strfmt("g%d", g);
        bool gop = ngroups == 1 || branch(gid.c_str(), T(gname[g][0], gname[g][1]), strfmt("%d", n), false, nullptr, true);
        if (ngroups > 1 && ImGui::BeginPopup(ctx_id(gid.c_str()).c_str())) {
            std::vector<int> gm;
            for (int si : grp[g]) gm.insert(gm.end(), by_set[si].begin(), by_set[si].end());
            static const char* gtag[4] = {"common", "weapons", "rooms", "doors"};
            export_menu(motions_req(gm, on_ai, T(gname[g][0], gname[g][1]), gtag[g]));
            ImGui::EndPopup();
        }
        if (!gop) continue;
        for (int si : grp[g]) {
            const LibSet& s = lib_.sets[si];
            const std::string sid = strfmt("s%d", si);
            const bool sop = branch(sid.c_str(), set_title(s), strfmt("%d", (int)by_set[si].size()), false, nullptr, true);
            if (ImGui::BeginPopup(ctx_id(sid.c_str()).c_str())) { export_menu(motions_req(by_set[si], on_ai, s.id, s.id)); ImGui::EndPopup(); }
            if (!sop) continue;
            Loaded* L = load(on.at.f);
            const int fi = L ? ext_set(*L, si) : -1;
            if (fi >= 0 && fi < (int)L->c.motion_sets.size()) {
                const MotionSet& ms = L->c.motion_sets[fi];
                for (int m : by_set[si]) {
                    const LibMotion& lm = lib_.motions[m];
                    const int mi = lm.at.motion;
                    if (mi < 0 || mi >= (int)ms.motions.size()) continue;
                    Sel sm{on.at.f, K_MOTION, on.at.part, fi * 10000 + mi};
                    std::string hint = strfmt("%.2f %s", ms.motions[mi].ticks / 30.0f, T("s", "с"));
                    if (!lm.also.empty()) hint += strfmt("  +%d", (int)lm.also.size());
                    const std::string lid = strfmt("m%d", m);
                    if (leaf(lid.c_str(), lm.name, hint, sel_ == sm, true)) { select(sm); pick_req_ = motions_req({m}, on_ai, lm.name, lm.name); }
                    if (ImGui::BeginPopup(ctx_id(lid.c_str()).c_str())) { export_menu(motions_req({m}, on_ai, lm.name, lm.name)); ImGui::EndPopup(); }
                    if (!lm.also.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", strfmt(T("The same motion is in %d more place(s): an edit goes everywhere",
                                                         "То же движение ещё в %d местах: правка расходится везде"),
                                                       (int)lm.also.size()).c_str());
                }
            }
            ImGui::TreePop();
        }
        if (ngroups > 1) ImGui::TreePop();
    }
    ImGui::TreePop();
}

// 08.10.2026 (player: "by clicking an animation - only it with the picked mesh when it is a common one, else with the character
// it is made for"): these motions with the model picked last when they fit it (Regina's costumes, people - the same build),
// else with the model that shows them
ExportReq App::motions_req(const std::vector<int>& mots, int owner, const std::string& what, const std::string& tag) {
    ExportReq r;
    if (mots.empty()) return r;
    bool fits = last_actor_ >= 0;
    for (int m : mots) fits = fits && motion_fits(lib_, m, last_actor_);
    const int tgt = fits ? last_actor_ : owner >= 0 ? owner : motion_model(lib_, mots[0]);
    if (tgt < 0 || tgt >= (int)lib_.actors.size()) return r;
    if (actor_is_dino(lib_.actors[tgt])) {        // player: dinosaurs - no animation or all of it (the list up top decides)
        r.label = lib_.actors[tgt].id;
        r.actors.push_back(tgt);
        return r;
    }
    r.label = what + " " + T("with", "с") + " " + lib_.actors[tgt].id;
    r.actors.push_back(tgt);
    r.actor_mots[tgt] = mots;
    r.mots_tag = tag;
    return r;
}

// модель персонажа: меш и её текстуры (inline — единственная модель персонажа: без своего узла)
void App::tree_model(int ai, bool inline_model) {
    const LibActor& a = lib_.actors[ai];
    ImGui::PushID(("a_" + a.id).c_str());
    const Sel ms{a.at.f, K_MODEL, a.at.part, -1};
    ExportReq req;   // this one model (06.10.2026, player: "I pick only the mesh - only it goes out")
    req.label = a.id;
    req.actors.push_back(ai);
    bool op = true;
    if (!inline_model) {
        bool clicked = false;
        op = branch("n", model_title(a), places_hint(a.at, a.also.size()), sel_ == ms, &clicked, true);
        if (clicked) { select(ms); pick_req_ = req; last_actor_ = ai; }
        if (ImGui::BeginPopup(ctx_id("n").c_str())) { export_menu(req); ImGui::EndPopup(); }
    }
    if (op) {
        if (leaf("model", T("Mesh", "Меш"), strfmt("%d %s", a.nprims, T("triangles", "треугольников")), sel_ == ms, true)) {
            select(ms);
            pick_req_ = req;
            last_actor_ = ai;
        }
        if (ImGui::BeginPopup(ctx_id("model").c_str())) { export_menu(req); ImGui::EndPopup(); }
        tree_tex_owned(a.tex, strfmt("a%d", ai));
        if (!inline_model) ImGui::TreePop();
    }
    ImGui::PopID();
}

void App::tree_char(int ci) {
    const LibChar& c = lib_.chars[ci];
    if (c.actors.empty()) return;
    const LibActor& a0 = lib_.actors[c.actors[0]];
    ImGui::PushID(("c_" + c.id).c_str());
    const Sel ms{a0.at.f, K_MODEL, a0.at.part, -1};
    if (start_open_.find("," + c.id + ",") != std::string::npos) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    std::string hint = places_hint(a0.at, a0.also.size());
    if (c.actors.size() > 1)
        hint = strfmt(g_lang == LANG_RU ? "%d модели" : "%d models", (int)c.actors.size());
    ExportReq req;   // the character: all its models (the motions go with the main one)
    req.label = char_title(c, lib_);
    req.actors = c.actors;
    bool clicked = false;
    bool op = branch("n", char_title(c, lib_), hint, sel_ == ms, &clicked, true);
    if (clicked) { select(ms); pick_req_ = req; last_actor_ = c.actors[0]; }
    if (!anim_open_.empty() && anim_open_ == c.id && !pick_req_.count()) { select(ms); pick_req_ = req; last_actor_ = c.actors[0]; }
    if (ImGui::BeginPopup(ctx_id("n").c_str())) { export_menu(req); ImGui::EndPopup(); }
    if (op) {
        if (c.actors.size() == 1) tree_model(c.actors[0], true);
        else for (int ai : c.actors) tree_model(ai, false);
        ExportReq mreq;
        mreq.label = a0.id;
        mreq.actors.push_back(c.actors[0]);
        tree_motions(a0, c.motions, "mot", "Motions", "Движения", mreq);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// неподвижный меш: оружие, оружие в руке, предмет, модель перехода
void App::tree_mesh(int i) {
    const LibMesh& m = lib_.meshes[i];
    ImGui::PushID(("m_" + m.id).c_str());
    const Sel s{m.at.f, K_MESH, m.at.part, -1};
    ExportReq req;
    req.label = m.id;
    req.meshes.push_back(i);
    bool clicked = false;
    bool op = branch("n", mesh_title(m), places_hint(m.at, m.also.size()), sel_ == s, &clicked, true);
    if (clicked) { select(s); pick_req_ = req; }
    if (ImGui::BeginPopup(ctx_id("n").c_str())) { export_menu(req); ImGui::EndPopup(); }
    if (op) {
        if (leaf("mesh", T("Mesh", "Меш"), strfmt("%d %s", m.ntris, T("triangles", "треугольников")), sel_ == s, true)) {
            select(s);
            pick_req_ = req;
        }
        if (ImGui::BeginPopup(ctx_id("mesh").c_str())) { export_menu(req); ImGui::EndPopup(); }
        tree_tex_owned(m.tex, strfmt("m%d", i));
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// комната: только своё — тело, текстуры тела, расстановка, данные (персонажи — в «Персонажах», звуки — в «Звуке»)
void App::tree_room(const DiscFile& f) {
    ImGui::PushID(&f);
    const bool is_sel = sel_.f == &f && sel_.k == K_FILE;
    ExportReq req;   // the room: its file (body, data, the game file as is) and the motions of its scenes
    req.label = f.stem;
    req.files.push_back(&f);
    if (lib_.ready.load() && lib_.scene_motions.count(&f)) req.scenes.push_back(&f);
    bool clicked = false;
    bool op = branch("f", f.stem, file_hint(f), is_sel, &clicked, true);
    if (clicked) { select(first_content(f)); pick_req_ = req; }
    if (ImGui::BeginPopup(ctx_id("f").c_str())) { export_menu(req); ImGui::EndPopup(); }
    if (!op) { ImGui::PopID(); return; }
    Loaded* L = load(&f);
    if (!L) { ImGui::TreePop(); ImGui::PopID(); return; }
    FileContent& c = L->c;
    auto pick = [&](Kind k, int a, int b) { select(Sel{&f, k, a, b}); };
    auto is = [&](Kind k, int a, int b) { return sel_ == Sel{&f, k, a, b}; };
    // one part of the room (06.10.2026): the right-click menu of a part exports only it
    auto part_menu = [&](const char* id, const char* part, const char* en) {
        if (!ImGui::BeginPopup(ctx_id(id).c_str())) return;
        ExportReq r;
        r.label = f.stem + ": " + en;
        r.files.push_back(&f);
        r.room_part = part;
        export_menu(r);
        ImGui::EndPopup();
    };
    const LibRoom* lr = lib_.ready.load() ? lib_.room(&f) : nullptr;
    for (size_t i = 0; i < c.meshes.size(); i++)
        if (c.meshes[i].kind == "room") {
            std::string hint = strfmt("%d %s", (int)c.meshes[i].tris.size(), T("triangles", "треугольников"));
            if (lr && lr->twin) hint += strfmt(g_lang == LANG_RU ? "  = %s" : "  = %s", lr->twin->stem.c_str());
            const std::string mid = strfmt("m%d", (int)i);
            if (leaf(mid.c_str(), T("Room", "Комната"), hint, is(K_MESH, (int)i, -1), true))
                pick(K_MESH, (int)i, -1);
            if (lr && lr->twin && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", strfmt(T("The same room body as %s: an edit of either goes to both",
                                                 "То же тело комнаты, что у %s: правка одной идёт в обе"), lr->twin->stem.c_str()).c_str());
            part_menu(mid.c_str(), "mesh", "mesh");
            if (lr) tree_tex_owned(lr->tex, "r" + f.stem);
        }
    // движения сцен этой комнаты (Регина и люди вместе; кто что играет — решает скрипт) — показ на Регине
    if (lib_.ready.load()) {
        auto sm = lib_.scene_motions.find(&f);
        const LibActor* rg = lib_.actor("regina");
        if (sm != lib_.scene_motions.end() && rg) {
            ExportReq sreq;
            sreq.label = f.stem + ": scene motions";
            sreq.scenes.push_back(&f);
            tree_motions(*rg, sm->second, "scm", "Scene motions", "Движения сцен", sreq);
        }
    }
    if (c.is_room) {
        ensure_placement(*L);
        ensure_edit(*L);
        auto live = [](const json& a) { int n = 0; for (auto& e : a) n += !e.value("removed", false); return n; };
        if (leaf("enm", T("Enemies", "Враги"), strfmt("%d", live(L->ed.enemies)), is(K_ENEMIES, -1, -1), true)) pick(K_ENEMIES, -1, -1);
        part_menu("enm", "enemies", "enemies");
        if (leaf("itm", T("Items", "Предметы"), strfmt("%d", live(L->ed.items)), is(K_ITEMS, -1, -1), true)) pick(K_ITEMS, -1, -1);
        part_menu("itm", "items", "items");
        if (!L->doors.empty()) {
            if (leaf("dor", T("Doors", "Двери"), strfmt("%d", (int)L->doors.size()), is(K_DOORS, -1, -1), true)) pick(K_DOORS, -1, -1);
            part_menu("dor", "doors", "doors");
        }
        if (!L->zones.empty()) {
            if (leaf("zon", T("Zones", "Зоны"), strfmt("%d", (int)L->zones.size()), is(K_ZONES, -1, -1), true)) pick(K_ZONES, -1, -1);
            part_menu("zon", "zones", "zones");
        }
        {   // 06.10.2026: pieces the script puts at their own place (doors, hatches, boxes, fans...) - they live in the script
            int np = 0;
            for (int i = 0; i < (int)L->ed.pieces.size(); i++)
                np += L->ed.pieces[i].value("placed", false) && L->ed.pieces[i].value("lead", i) == i ? 1 : 0;
            if (np) {
                if (leaf("pcs", T("Placed pieces", "Поставленные куски"), strfmt("%d", np), is(K_PIECES, -1, -1), true)) pick(K_PIECES, -1, -1);
                part_menu("pcs", "script", "script");
            }
        }
        if (c.room.coll.ok) {
            if (leaf("coll", T("Collision", "Коллизия"), "", is(K_COLL, -1, -1), true)) pick(K_COLL, -1, -1);
            part_menu("coll", "collision", "collision");
        }
        if (!c.room.triggers.empty()) {
            if (leaf("trigs", T("Footsteps", "Звук шагов"), "", is(K_TRIGS, -1, -1), true)) pick(K_TRIGS, -1, -1);
            part_menu("trigs", "footsteps", "footsteps");
        }
        if (!c.room.cameras.empty()) {
            if (leaf("cams", T("Cameras", "Камеры"), strfmt("%d", (int)c.room.cameras.size()), is(K_CAMS, -1, -1), true)) pick(K_CAMS, -1, -1);
            part_menu("cams", "cameras", "cameras");
        }
        if (!c.room.strings.empty()) {
            if (leaf("texts", T("Texts", "Тексты"), strfmt("%d", (int)c.room.strings.size()), is(K_TEXTS, -1, -1), true)) pick(K_TEXTS, -1, -1);
            part_menu("texts", "texts", "texts");
        }
        if (!c.room.script_dir.empty()) {
            if (leaf("script", T("Script", "Скрипт"), "", is(K_SCRIPT, -1, -1), true)) pick(K_SCRIPT, -1, -1);
            part_menu("script", "script", "script");
        }
    }
    ImGui::TreePop();
    ImGui::PopID();
}

// звук: мелодии и сэмплы — без повторов; речь и шумы — по файлам XA
void App::tree_sound() {
    if (lib_fx_.empty() && lib_ins_.empty())
        for (size_t i = 0; i < lib_.samples.size(); i++) (lib_.samples[i].bgm ? lib_ins_ : lib_fx_)[lib_.samples[i].at.f].push_back((int)i);
    std::vector<int> songs;
    for (size_t i = 0; i < lib_.songs.size(); i++)
        if (fmatch({lib_.songs[i].at.f->stem, "song", "мелодия", "music", "музыка"})) songs.push_back((int)i);
    auto files_match = [&](const std::map<const DiscFile*, std::vector<int>>& m, const char* w1, const char* w2) {
        std::vector<const DiscFile*> out;
        for (auto& kv : m) if (fmatch({kv.first->stem, w1, w2})) out.push_back(kv.first);
        return out;
    };
    auto fx = files_match(lib_fx_, "effects", "эффекты");
    auto ins = files_match(lib_ins_, "instruments", "инструменты");
    std::vector<const DiscFile*> voice, noise;
    for (auto& f : disc_.files) {
        if (f.cat == "sound\\voice" && fmatch({f.stem, "voice", "речь"})) voice.push_back(&f);
        if (f.cat == "sound\\se" && fmatch({f.stem, "noise", "шумы"})) noise.push_back(&f);
    }
    if (songs.empty() && fx.empty() && ins.empty() && voice.empty() && noise.empty()) return;
    // 06.10.2026: what each folder of the sound holds (Export of the folder / the section)
    ExportReq rsongs, rfx, rins, rvoice, rnoise, rall;
    rsongs.label = T("Music", "Музыка");
    for (int i : songs) rsongs.songs.insert(i);
    auto add_samples = [&](ExportReq& r, const std::vector<const DiscFile*>& files, std::map<const DiscFile*, std::vector<int>>& by) {
        for (auto* f : files) for (int i : by[f]) r.samples.insert(i);
    };
    rfx.label = T("Effects", "Эффекты");
    add_samples(rfx, fx, lib_fx_);
    rins.label = T("Instruments", "Инструменты");
    add_samples(rins, ins, lib_ins_);
    rvoice.label = T("Voice", "Речь");
    rvoice.files = voice;
    rnoise.label = T("Noises", "Шумы");
    rnoise.files = noise;
    rall.label = "sound";
    rall.songs = rsongs.songs;
    rall.samples = rfx.samples;
    rall.samples.insert(rins.samples.begin(), rins.samples.end());
    rall.files = voice;
    rall.files.insert(rall.files.end(), noise.begin(), noise.end());
    if (!section("sound", "sound", "Sound", "Звук", &rall)) return;
    if (!songs.empty() && folder_branch("sound/songs", "songs", T("Music", "Музыка"), strfmt("%d", (int)songs.size()), rsongs)) {
        for (int i : songs) {
            const LibSong& sg = lib_.songs[i];
            Sel s{sg.at.f, K_SONG, sg.index, -1};
            const std::string id = strfmt("q%d", i);
            if (leaf(id.c_str(), strfmt("%s %02d", sg.at.f->stem.c_str(), sg.index), sg.also.empty() ? "" : strfmt("+%d", (int)sg.also.size()), sel_ == s, true))
                select(s);
            if (ImGui::BeginPopup(ctx_id(id.c_str()).c_str())) {
                ExportReq r;
                r.label = strfmt("%s song %d", sg.at.f->stem.c_str(), sg.index);
                r.songs.insert(i);
                export_menu(r);
                ImGui::EndPopup();
            }
        }
        ImGui::TreePop();
    }
    auto samples = [&](const char* id, const char* en, const char* ru, const std::vector<const DiscFile*>& files,
                       std::map<const DiscFile*, std::vector<int>>& by, const ExportReq& rgroup) {
        if (files.empty()) return;
        int total = 0;
        for (auto* f : files) total += (int)by[f].size();
        if (!folder_branch(std::string("sound/") + id, id, T(en, ru), strfmt("%d", total), rgroup)) return;
        for (const DiscFile* f : files) {
            ImGui::PushID(f);
            ExportReq rf;
            rf.label = f->stem;
            for (int i : by[f]) rf.samples.insert(i);
            if (folder_branch(std::string("sound/") + id + "/" + f->stem, "f", f->stem, strfmt("%d", (int)by[f].size()), rf)) {
                for (int i : by[f]) {
                    const LibSample& sm = lib_.samples[i];
                    Sel s{f, K_SAMPLE, sm.bank, sm.index};
                    std::string lbl = sm.nbanks > 1 ? strfmt("%d-%03d", sm.bank + 1, sm.index) : strfmt("%03d", sm.index);
                    std::string hint = strfmt("%.2f %s", sm.secs, T("s", "с"));
                    if (!sm.also.empty()) hint += strfmt("  +%d", (int)sm.also.size());
                    const std::string sid = strfmt("s%d", i);
                    if (leaf(sid.c_str(), lbl, hint, sel_ == s, true)) select(s);
                    if (ImGui::BeginPopup(ctx_id(sid.c_str()).c_str())) {
                        ExportReq r;
                        r.label = f->stem + " " + lbl;
                        r.samples.insert(i);
                        export_menu(r);
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    };
    samples("fx", "Effects", "Эффекты", fx, lib_fx_, rfx);
    samples("ins", "Instruments", "Инструменты", ins, lib_ins_, rins);
    if (!voice.empty() && folder_branch("sound/voice", "voice", T("Voice", "Речь"), strfmt("%d", (int)voice.size()), rvoice)) {
        for (auto* f : voice) tree_file(*f);
        ImGui::TreePop();
    }
    if (!noise.empty() && folder_branch("sound/noise", "noise", T("Noises", "Шумы"), "", rnoise)) {
        for (auto* f : noise) tree_file(*f);
        ImGui::TreePop();
    }
    ImGui::TreePop();
}

void App::tree() {
    static char buf[96] = "";
    if (!start_filter_.empty()) {
        snprintf(buf, sizeof buf, "%s", start_filter_.c_str());
        flt_ = lowerw(buf);
        start_filter_.clear();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##flt", T("Filter", "Фильтр"), buf, sizeof buf)) flt_ = lowerw(buf);
    folder_req_.clear();   // gathered again while the tree is drawn (what each shown folder holds, under the filter)
    ImGui::BeginChild("treelist");
    const bool ready = lib_.ready.load();
    if (!ready) ImGui::TextDisabled("%s %d / %d", T("Cataloguing the disc...", "Каталог диска..."), lib_done_.load(), lib_total_.load());
    if (ready) {
        // персонажи (library.h chars — по порядку): Регина; динозавры; люди; прочие модели со скелетом
        std::vector<int> grp[4];
        for (size_t ci = 0; ci < lib_.chars.size(); ci++) {
            const LibChar& c = lib_.chars[ci];
            bool ok = fmatch({char_title(c, lib_), c.id, c.kind, actor_label(c.kind)});
            for (int ai : c.actors) {
                const LibActor& a = lib_.actors[ai];
                ok = ok || fmatch({model_title(a), a.id, a.at.f->stem});
            }
            if (!ok) continue;
            const int g = c.kind == "regina" ? 0 : c.kind == "human" ? 2 : (c.kind == "prop" || c.kind == "model") ? 3 : 1;
            grp[g].push_back((int)ci);
        }
        // 06.10.2026: what the section and each group hold - every model of every character shown there
        ExportReq rchars, rgrp[4];
        rchars.label = "characters";
        for (int g = 0; g < 4; g++)
            for (int ci : grp[g])
                for (int ai : lib_.chars[ci].actors) { rgrp[g].actors.push_back(ai); rchars.actors.push_back(ai); }
        if ((!grp[0].empty() || !grp[1].empty() || !grp[2].empty() || !grp[3].empty()) &&
            section("characters", "characters", "Characters", "Персонажи", &rchars)) {
            for (int ci : grp[0]) tree_char(ci);
            const char* gname[4][2] = {{"", ""}, {"Dinosaurs", "Динозавры"}, {"People", "Люди"}, {"Other", "Прочие"}};
            const char* gid[4] = {"", "dinos", "people", "others"};
            for (int g = 1; g < 4; g++) {
                if (grp[g].empty()) continue;
                if (!flt_.empty() || start_open_.find(std::string(",") + gid[g] + ",") != std::string::npos)
                    ImGui::SetNextItemOpen(true, flt_.empty() ? ImGuiCond_Once : ImGuiCond_Always);
                if (!folder_branch(std::string("characters/") + gid[g], gid[g], T(gname[g][0], gname[g][1]), strfmt("%d", (int)grp[g].size()), rgrp[g]))
                    continue;
                for (int ci : grp[g]) tree_char(ci);
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
        auto meshes_of = [&](const char* cat) {
            std::vector<int> v;
            for (size_t i = 0; i < lib_.meshes.size(); i++) {
                const LibMesh& m = lib_.meshes[i];
                if (m.cat == cat && fmatch({mesh_title(m), m.id, m.kind, m.at.f->stem})) v.push_back((int)i);
            }
            return v;
        };
        auto wp = meshes_of("weapons");
        ExportReq rwp;
        rwp.label = "weapons";
        rwp.meshes = wp;
        if (!wp.empty() && section("weapons", "weapons", "Weapons", "Оружие", &rwp)) {
            // вид оружия: модель (на полу, в ящике) и «в руке» Регины по костюмам — P/H<вид><костюм>
            auto wkind = [&](const LibMesh& m) {
                if (m.kind == "in_hand") { const std::string& st = m.at.f->stem; return st.size() > 1 ? st[1] - '0' : 9; }
                return m.kind == "shotgun" ? 1 : m.kind == "handgun" ? 2 : m.kind == "grenade_gun" ? 3 : m.kind == "dart" ? 4 : 9;
            };
            std::map<int, std::vector<int>> byk;
            for (int i : lib_.meshes_of("weapons")) {
                const LibMesh& m = lib_.meshes[i];
                if (std::find(wp.begin(), wp.end(), i) == wp.end()) continue;
                byk[wkind(m)].push_back(i);
            }
            for (auto& kv : byk) {
                const char* kn[2] = {"Weapon", "Оружие"};
                if (kv.first == 1) { kn[0] = "Shotgun"; kn[1] = "Дробовик"; }
                else if (kv.first == 2) { kn[0] = "Handgun"; kn[1] = "Пистолет"; }
                else if (kv.first == 3) { kn[0] = "Grenade gun"; kn[1] = "Гранатомёт"; }
                else if (kv.first == 4) { kn[0] = "Dart"; kn[1] = "Дротик"; }
                if (!flt_.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                ExportReq rk;
                rk.label = T(kn[0], kn[1]);
                rk.meshes = kv.second;
                if (!folder_branch(strfmt("weapons/wk%d", kv.first), strfmt("wk%d", kv.first).c_str(), T(kn[0], kn[1]),
                                   strfmt("%d", (int)kv.second.size()), rk))
                    continue;
                for (int i : kv.second) tree_mesh(i);
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
        auto it = meshes_of("items");
        std::vector<const DiscFile*> pics;
        for (auto& f : disc_.files) if (f.cat == "items" && fmatch({f.stem, "items", "предметы", "картинки"})) pics.push_back(&f);
        ExportReq rit;
        rit.label = "items";
        rit.files = pics;
        rit.meshes = it;
        if ((!it.empty() || !pics.empty()) && section("items", "items", "Items", "Предметы", &rit)) {
            for (auto* f : pics) tree_file(*f);
            for (int i : it) tree_mesh(i);
            ImGui::TreePop();
        }
    }
    {
        std::vector<const DiscFile*> rooms;
        for (auto& f : disc_.files) if (f.cat == "rooms" && fmatch({f.stem, f.name, file_hint(f), "rooms", "комнаты"})) rooms.push_back(&f);
        std::sort(rooms.begin(), rooms.end(), [](const DiscFile* a, const DiscFile* b) { return a->name < b->name; });
        ExportReq rrm;   // the rooms with the motions of their scenes
        rrm.label = "rooms";
        rrm.files = rooms;
        if (ready) for (auto* f : rooms) if (lib_.scene_motions.count(f)) rrm.scenes.push_back(f);
        if (!rooms.empty() && section("rooms", "rooms", "Rooms", "Комнаты", &rrm)) {
            for (auto* f : rooms) tree_room(*f);
            ImGui::TreePop();
        }
    }
    if (ready) {
        std::vector<int> dr;
        for (size_t i = 0; i < lib_.meshes.size(); i++) {
            const LibMesh& m = lib_.meshes[i];
            if (m.cat == "doors" && fmatch({mesh_title(m), m.id, "doors", "переходы", "двери"})) dr.push_back((int)i);
        }
        ExportReq rdr;
        rdr.label = "doors";
        rdr.meshes = dr;
        if (!dr.empty() && section("doors", "doors", "Doors", "Переходы", &rdr)) {
            for (int i : dr) tree_mesh(i);
            ImGui::TreePop();
        }
    }
    auto plain = [&](const char* cat, const char* en, const char* ru) {
        std::vector<const DiscFile*> v;
        for (auto& f : disc_.files) if (f.cat == cat && fmatch({f.stem, f.name, file_hint(f), cat, en, ru})) v.push_back(&f);
        std::sort(v.begin(), v.end(), [](const DiscFile* a, const DiscFile* b) { return a->name < b->name; });
        ExportReq r;
        r.label = cat;
        r.files = v;
        if (v.empty() || !section(cat, cat, en, ru, &r)) return;
        for (auto* f : v) tree_file(*f);
        ImGui::TreePop();
    };
    plain("screens", "Screens", "Экраны");
    plain("movie", "Movies", "Ролики");
    if (ready) tree_sound();
    plain("other", "Other", "Прочее");
    ImGui::EndChild();
}

// что показать по клику на файл: главное содержимое (комната, картинка, видео, таблица), иначе сам файл
Sel App::first_content(const DiscFile& f) {
    Sel s;
    s.f = &f;
    s.k = K_FILE;
    Loaded* L = load(&f);
    if (!L) return s;
    const FileContent& c = L->c;
    if (f.cat == "movie") s.k = K_VIDEO;
    else if (f.cat == "other" && upper(f.name) == "SLUS_009.22") s.k = K_EXE_TEXTS;
    else if (f.cat == "other" && f.dir == "/PSX/BIN/") s.k = K_ROSTER;
    else if (!c.meshes.empty()) { s.k = K_MESH; s.a = 0; }
    else if (!c.images.empty()) { s.k = K_IMAGE; s.a = 0; }
    else if (!c.xa.empty()) { s.k = K_XA; s.a = 0; }
    else if (c.container && !c.uploads.empty() && (f.cat == "screens" || f.cat == "items")) s.k = K_VRAM;
    return s;
}

// файл без каталога: экраны, ролики, картинки предметов, речь, программа игры, оверлеи этапов
void App::tree_file(const DiscFile& f) {
    ImGui::PushID(&f);
    bool is_sel = sel_.f == &f && sel_.k == K_FILE;
    if (open_req_ == &f) { ImGui::SetNextItemOpen(true); open_req_ = nullptr; }
    ExportReq req;   // the whole file (each part of it goes out with it)
    req.label = upper(f.name) == "SLUS_009.22" ? f.name : f.stem;
    req.files.push_back(&f);
    bool clicked = false;
    bool op = branch("f", upper(f.name) == "SLUS_009.22" ? f.name : f.stem, file_hint(f), is_sel, &clicked, true);
    if (clicked) {
        select(first_content(f));
        pick_req_ = req;
        if (!op) open_req_ = &f;
    }
    if (ImGui::BeginPopup(ctx_id("f").c_str())) { export_menu(req); ImGui::EndPopup(); }
    if (!op) { ImGui::PopID(); return; }
    Loaded* L = load(&f);
    if (!L) { ImGui::TreePop(); ImGui::PopID(); return; }
    FileContent& c = L->c;
    auto pick = [&](Kind k, int a, int b) { select(Sel{&f, k, a, b}); };
    auto is = [&](Kind k, int a, int b) { return sel_ == Sel{&f, k, a, b}; };
    // a part of the file: picked by a click; its Export takes the whole file (the parts are written together)
    auto fleaf = [&](const std::string& id, const std::string& lbl, const std::string& hint, Kind k, int a) {
        if (leaf(id.c_str(), lbl, hint, is(k, a, -1), true)) pick(k, a, -1);
        if (ImGui::BeginPopup(ctx_id(id.c_str()).c_str())) { export_menu(req); ImGui::EndPopup(); }
    };
    if (f.cat == "movie") {
        fleaf("vid", T("Video", "Видео"), "", K_VIDEO, -1);
        ImGui::TreePop();
        ImGui::PopID();
        return;
    }
    for (size_t i = 0; i < c.images.size(); i++) {
        const ImagePart& im = c.images[i];
        std::string lbl = im.id >= 0 ? strfmt("%03d", im.id) : std::string(T("Picture", "Картинка"));
        std::string hint = im.id >= 0 ? item_name(im.id) : std::string();
        fleaf(strfmt("i%d", (int)i), lbl, hint, K_IMAGE, (int)i);
    }
    if (!c.xa.empty()) {
        for (size_t i = 0; i < c.xa.size(); i++) {
            const XaChannel& x = c.xa[i];
            double secs = (double)x.sectors.size() * (x.channels == 2 ? 2016 : 4032) / x.rate;
            fleaf(strfmt("x%d", (int)i), strfmt("%s %02d", T("channel", "канал"), x.chan), strfmt("%.1f %s", secs, T("s", "с")), K_XA, (int)i);
        }
    }
    if (f.cat == "other" && upper(f.name) == "SLUS_009.22") {
        fleaf("xt", T("Texts", "Тексты"), "", K_EXE_TEXTS, -1);
        fleaf("xi", T("Items", "Предметы"), "", K_EXE_ITEMS, -1);
        fleaf("xd", T("Door scenes", "Сцены дверей"), "", K_EXE_DOORS, -1);
    }
    if (f.cat == "other" && f.dir == "/PSX/BIN/") fleaf("ro", T("Dinosaurs of the stage", "Динозавры этапа"), "", K_ROSTER, -1);
    if (c.container && !c.uploads.empty() && (f.cat == "screens" || f.cat == "items")) {
        bool vclicked = false;
        const bool vop = branch("vr", T("Video memory", "Видеопамять"), "", is(K_VRAM, -1, -1), &vclicked, true);
        if (vclicked) pick(K_VRAM, -1, -1);
        if (ImGui::BeginPopup(ctx_id("vr").c_str())) { export_menu(req); ImGui::EndPopup(); }
        if (vop) {
            for (size_t i = 0; i < c.uploads.size(); i++) {
                const Upload& u = c.uploads[i];
                if (u.type == 2) continue;   // палитры
                fleaf(strfmt("u%d", (int)i), strfmt("%d,%d", u.x, u.y), strfmt("%dx%d", u.w, u.h), K_UPLOAD, (int)i);
            }
            ImGui::TreePop();
        }
    }
    ImGui::TreePop();
    ImGui::PopID();
}

}  // namespace dc
