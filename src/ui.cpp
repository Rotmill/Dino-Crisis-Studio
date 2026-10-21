#include "ui.h"

#include <chrono>

#include <SDL_syswm.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "gltf.h"
#include "anim_packs.h"
#include "i18n.h"
#include "placement.h"
#include "script.h"
#include "synth.h"
#include "ui_names.h"

#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace dc {

// ---------------------------------------------------------------- диалоги Windows
static HWND hwnd_of(SDL_Window* w) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (w && SDL_GetWindowWMInfo(w, &info)) return info.info.win.window;
    return nullptr;
}

static std::string run_dialog(SDL_Window* w, bool folder, const wchar_t* title, const std::string& start) {
    std::string out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (title) dlg->SetTitle(title);
    if (!folder) {
        COMDLG_FILTERSPEC spec[] = {{L"Dino Crisis (*.bin, *.cue, *.iso)", L"*.bin;*.cue;*.iso"}, {L"*.*", L"*.*"}};
        dlg->SetFileTypes(2, spec);
    }
    if (!start.empty()) {
        IShellItem* si = nullptr;
        fs::path p = u8path(start);
        if (!fs::is_directory(p)) p = p.parent_path();
        if (SUCCEEDED(SHCreateItemFromParsingName(p.wstring().c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    if (SUCCEEDED(dlg->Show(hwnd_of(w)))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = fs::path(p).u8string();
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
    return out;
}

std::string pick_image_dialog(SDL_Window* w) {
    return run_dialog(w, false, g_lang == LANG_RU ? L"Образ диска Dino Crisis" : L"Dino Crisis disc image", "");
}
std::string pick_folder_dialog(SDL_Window* w, const char* title, const std::string& start) {
    std::wstring t = fs::u8path(title).wstring();
    return run_dialog(w, true, t.c_str(), start);
}
void open_in_explorer(const fs::path& p) { ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ---------------------------------------------------------------- звук
Audio::~Audio() {
    if (dev_) SDL_CloseAudioDevice(dev_);
}

void Audio::play(const std::vector<int16_t>& pcm, int ch, int rate, double from_sec) {
    if (!dev_) {
        SDL_AudioSpec want{}, have{};
        want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 2048;
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!dev_) return;
        out_rate_ = have.freq;
    }
    SDL_ClearQueuedAudio(dev_);
    ch = std::max(1, ch);
    size_t frames = pcm.size() / ch;
    size_t start = (size_t)std::max(0.0, from_sec * rate);
    if (start >= frames) { total_ = 0; return; }
    double step = (double)rate / out_rate_;
    size_t outn = (size_t)((frames - start) / step);
    std::vector<int16_t> o(outn * 2);
    for (size_t i = 0; i < outn; i++) {
        double s = start + i * step;
        size_t a = (size_t)s;
        double fr = s - a;
        size_t b = std::min(a + 1, frames - 1);
        for (int c = 0; c < 2; c++) {
            int cc = ch == 2 ? c : 0;
            o[i * 2 + c] = (int16_t)(pcm[a * ch + cc] * (1 - fr) + pcm[b * ch + cc] * fr);
        }
    }
    total_ = o.size() * 2;
    from_ = (double)start / rate;
    SDL_QueueAudio(dev_, o.data(), (Uint32)total_);
    SDL_PauseAudioDevice(dev_, 0);
}

void Audio::stop() {
    if (dev_) SDL_ClearQueuedAudio(dev_);
    total_ = 0;
}
bool Audio::playing() const { return dev_ && total_ && SDL_GetQueuedAudioSize(dev_) > 0; }
double Audio::position() const {
    if (!dev_ || !total_) return from_;
    return from_ + (double)(total_ - SDL_GetQueuedAudioSize(dev_)) / (out_rate_ * 4.0);
}

// ---------------------------------------------------------------- настройки
void App::load_settings() {
    wchar_t* appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        settings_dir_ = fs::path(appdata) / L"DinoCrisisME_Studio";   // 10.10: the modding copy is the main line now
        CoTaskMemFree(appdata);
    } else {
        settings_dir_ = fs::current_path();
    }
    std::error_code ec;
    fs::create_directories(settings_dir_, ec);
    Bytes b;
    if (read_file(settings_dir_ / "settings.json", b)) {
        try {
            json j = json::parse(b.begin(), b.end());
            set_.image = j.value("image", "");
            set_.base = j.value("base", "");
            set_.mods = j.value("mods", "");
            set_.lang = j.value("lang", -1);
            set_.model_format = std::max(0, std::min(2, j.value("model_format", 0)));
            set_.anim_pack = j.value("anim_pack", std::string("all"));
            set_.work = j.value("work", std::string());
        } catch (...) {
        }
    }
    // рядом с игрой (установщик кладёт приложение в папку игры и studio.json с образом диска): mods — папка игры
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) exe_dir_ = path_u8(fs::path(exe).parent_path());
    fs::path ed = u8path(exe_dir_);
    if (set_.image.empty() && read_file(ed / "studio.json", b)) {
        try {
            json j = json::parse(b.begin(), b.end());
            set_.image = j.value("image", "");
            if (set_.lang < 0 && j.count("lang")) set_.lang = j.value("lang", -1);
        } catch (...) {
        }
    }
    if (set_.mods.empty() && fs::is_directory(ed / "mods")) set_.mods = path_u8(ed / "mods");
    // 06.10.2026 (player: "the game installer and the mod studio go separately"): the studio is its own download; unpacked
    // into the game folder (its own subfolder) it still finds the game's mods
    if (set_.mods.empty() && fs::is_directory(ed.parent_path() / "mods") && fs::exists(ed.parent_path() / "DinoCrisisME.exe"))
        set_.mods = path_u8(ed.parent_path() / "mods");
    // 06.10.2026 (player: "everything must be in English"): the Studio interface is always English, whatever the system or
    // the installer language (--lang stays for tests)
    g_lang = LANG_EN;
}

void App::save_settings() {
    if (!shot_path.empty()) return;   // проверочные запуски настройки не трогают
    json j = {{"image", set_.image}, {"base", set_.base}, {"mods", set_.mods}, {"lang", g_lang}, {"model_format", set_.model_format},
              {"anim_pack", set_.anim_pack}, {"work", set_.work}};
    write_text(settings_dir_ / "settings.json", j.dump(1));
}

// ---------------------------------------------------------------- старт
bool App::init(SDL_Window* win, int argc, char** argv) {
    win_ = win;
    load_settings();
    ImGui::GetIO().IniFilename = nullptr;   // раскладка окна фиксированная
    v3d_.init();
    std::string img;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) shot_path = argv[++i];
        else if (a == "--frames" && i + 1 < argc) shot_after = atoi(argv[++i]);
        else if (a == "--select" && i + 1 < argc) start_select_ = argv[++i];
        else if (a == "--item" && i + 1 < argc) start_item_ = argv[++i];
        else if (a == "--lang" && i + 1 < argc) g_lang = atoi(argv[++i]);
        else if (a == "--image" && i + 1 < argc) img = argv[++i];
        else if (a == "--base" && i + 1 < argc) set_.base = argv[++i];
        else if (a == "--mods" && i + 1 < argc) set_.mods = argv[++i];
        else if (a == "--do-export" && i + 1 < argc) do_export_ = argv[++i];
        else if (a == "--do-convert") do_convert_ = true;
        else if (a == "--export-folder" && i + 1 < argc) do_export_folder_ = argv[++i];
        else if (a == "--export-pick") do_export_pick_ = true;
        else if (a == "--no-cull") no_cull_ = true;
        else if (a == "--anim-open" && i + 1 < argc) anim_open_ = argv[++i];
        else if (a == "--anim-pack" && i + 1 < argc) set_.anim_pack = argv[++i];   // checking runs: settings stay untouched
        else if (a == "--filter" && i + 1 < argc) start_filter_ = argv[++i];
        else if (a == "--open" && i + 1 < argc) start_open_ = "," + std::string(argv[++i]) + ",";
        else if (a == "--skeleton") v3d_.skeleton = true;
        else if (a == "--edit-probe") edit_probe_ = true;
        else if (a == "--look" && i + 1 < argc) look_on_ = sscanf(argv[++i], "%f,%f,%f", &look_[0], &look_[1], &look_[2]) == 3;
        else if (a == "--packs") show_packs_ = true;                       // 10.10: the mod packages window open (--shot)
        else if (a == "--test-in-game") do_test_ = true;
        else if (a == "--scn-sel" && i + 1 < argc) scn_sel_req_ = argv[++i];
        else if (a == "--scn-add-scene" && i + 1 < argc) scn_add_req_ = std::string(argv[++i]) == "leap" ? 1 : 0;
        else if (a == "--scn-probe-key" && i + 1 < argc) scn_probe_key_ = argv[++i];
        else if (a == "--scn-drag-probe" && i + 1 < argc) scn_probe_ = sscanf(argv[++i], "%f,%f", &scn_probe_d_[0], &scn_probe_d_[1]) == 2;   // 14.10: the scenario line to pick (its fields below)
        else if (a == "--scn-sub" && i + 1 < argc) scn_sub_req_ = atoi(argv[++i]);   // 10.10: the scenario event to show                   // 10.10: press Test in game for the picked room (checks)
        else if (a == "--work" && i + 1 < argc) set_.work = argv[++i];     // 10.10: the package to work in (checking runs)
        else if (a[0] != '-') img = a;
    }
    if (!shot_path.empty() && shot_after <= 0) shot_after = 20;
    if (img.empty()) img = set_.image;
    if (!img.empty()) open_image(img);
    if (!start_select_.empty() && disc_.is_open()) {
        const DiscFile* f = disc_.find(start_select_.find('.') == std::string::npos ? start_select_ + ".DAT" : start_select_);
        if (!f) f = disc_.find(start_select_);
        if (!f) for (auto& x : disc_.files) if (x.stem == upper(start_select_)) f = &x;
        if (f) {
            Sel s;
            s.f = f;
            s.k = K_FILE;
            // --item mesh:0 | model:0 | motion:0:5 | tex:3 | sample:0:2 | xa:0 | song:0 | video | image:0 | upload:0 | vram | coll | cams |
            //        trigs | texts | script
            if (!start_item_.empty()) {
                static const std::pair<const char*, Kind> kinds[] = {
                    {"mesh", K_MESH}, {"model", K_MODEL}, {"motion", K_MOTION}, {"tex", K_TEXTURE}, {"sample", K_SAMPLE},
                    {"xa", K_XA}, {"song", K_SONG}, {"video", K_VIDEO}, {"image", K_IMAGE}, {"upload", K_UPLOAD}, {"vram", K_VRAM},
                    {"coll", K_COLL}, {"cams", K_CAMS}, {"trigs", K_TRIGS}, {"texts", K_TEXTS}, {"script", K_SCRIPT},
                    {"enemies", K_ENEMIES}, {"items", K_ITEMS}, {"doors", K_DOORS}, {"zones", K_ZONES}, {"pieces", K_PIECES}, {"exe_texts", K_EXE_TEXTS},
                    {"exe_items", K_EXE_ITEMS}, {"exe_doors", K_EXE_DOORS}, {"roster", K_ROSTER}};
                std::string k = start_item_.substr(0, start_item_.find(':'));
                for (auto& kv : kinds) if (k == kv.first) s.k = kv.second;
                size_t p1 = start_item_.find(':');
                if (p1 != std::string::npos) {
                    s.a = atoi(start_item_.c_str() + p1 + 1);
                    size_t p2 = start_item_.find(':', p1 + 1);
                    if (p2 != std::string::npos) s.b = atoi(start_item_.c_str() + p2 + 1);
                }
                // motion:<модель>:<номер> — номер в первом наборе движений модели
                if (s.k == K_MOTION) {
                    Loaded* L = load(f);
                    if (L && s.a >= 0 && s.a < (int)L->c.actors.size() && !L->c.actors[s.a].motion_sets.empty())
                        s.b = L->c.actors[s.a].motion_sets[0] * 10000 + std::max(0, s.b);
                }
            }
            select(s);
        }
    }
    last_ticks_ = SDL_GetPerformanceCounter();
    return true;
}

// mods\names.txt — каждый меш игры по имени: glTF под этим именем в mods игра берёт сама при запуске (dc_meshrep.cpp)
void App::write_names() {
    names_done_ = true;
    const fs::path m = mods_root();
    std::error_code ec;
    if (set_.mods.empty()) return;   // only into the game's own mods folder
    if (!fs::is_directory(m, ec)) return;
    const std::string t = lib_.names_text();
    Bytes cur;
    if (read_file(m / "names.txt", cur) && std::string(cur.begin(), cur.end()) == t) return;
    write_text(m / "names.txt", t);
}

int App::names_only(int argc, char** argv) {
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) exe_dir_ = path_u8(fs::path(exe).parent_path());
    const fs::path ed = u8path(exe_dir_);
    std::string img, mods = path_u8(ed / "mods");
    Bytes b;
    if (read_file(ed / "studio.json", b)) {
        try {
            json j = json::parse(b.begin(), b.end());
            img = j.value("image", "");
        } catch (...) {
        }
    }
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--image" && i + 1 < argc) img = argv[++i];
        else if (a == "--mods" && i + 1 < argc) mods = argv[++i];
    }
    std::string err;
    if (img.empty() || !disc_.open(u8path(img), err)) return 2;
    load_exe(disc_);
    lib_.build(disc_);
    std::error_code ec;
    fs::create_directories(u8path(mods), ec);
    return write_text(u8path(mods) / "names.txt", lib_.names_text()) ? 0 : 3;
}

void App::shutdown() {
    log_.cancel = true;
    if (job_ && job_->joinable()) job_->join();
    if (lib_job_ && lib_job_->joinable()) lib_job_->join();
    if (door_in_job_ && door_in_job_->joinable()) door_in_job_->join();
    free_image();
    save_settings();
}

void App::on_drop(const char* path) { open_image(path); }

void App::open_image(const std::string& path) {
    select(Sel());
    if (lib_job_ && lib_job_->joinable()) lib_job_->join();   // каталог прежнего образа достраивается — дождаться
    cache_.clear();
    disc_err_.clear();
    if (!disc_.open(u8path(path), disc_err_)) {
        status_ = std::string(T("Cannot open: ", "Не открывается: ")) + disc_err_;
        return;
    }
    status_.clear();
    if (!disc_.dino_crisis) status_ = T("Not Dino Crisis (USA)?", "Не похоже на Dino Crisis (USA)");
    load_exe(disc_);
    set_.image = path;
    save_settings();
    start_library();
}

Loaded* App::load(const DiscFile* f) {
    if (!f) return nullptr;
    auto it = cache_.find(f);
    if (it == cache_.end()) {
        auto L = std::make_unique<Loaded>();
        if (!analyze(disc_, *f, L->c)) return nullptr;
        // не больше 8 файлов в памяти
        while (cache_.size() >= 8) {
            auto old = cache_.begin();
            for (auto i = cache_.begin(); i != cache_.end(); ++i)
                if (i->second->used < old->second->used && i->first != sel_.f) old = i;
            if (old->first == sel_.f) break;
            cache_.erase(old);
        }
        it = cache_.emplace(f, std::move(L)).first;
    }
    it->second->used = ++use_clock_;
    return it->second.get();
}

void App::ensure_crops(Loaded& L) {
    if (L.crops_done) return;
    std::set<std::string> seen;
    for (auto& m : L.c.meshes)
        for (auto& cr : crops_for_mesh(L.c.vram, m.tris))
            if (seen.insert(cr.hash).second) L.crops.push_back(cr);
    for (auto& a : L.c.actors)
        for (auto& cr : crops_for_model(L.c.vram, a.model))
            if (seen.insert(cr.hash).second) L.crops.push_back(cr);
    L.crops_done = true;
}

void App::ensure_placement(Loaded& L) {
    if (L.placement_done) return;
    L.placement_done = true;
    if (!L.c.is_room) return;
    L.enemies = room_enemies_json(L.c)["enemies"];
    L.items = room_items_json(L.c)["items"];
    L.doors = room_doors_json(L.c)["doors"];
    L.zones = room_zones_json(L.c)["zones"];
}

void App::free_image() {
    if (img_tex_) glDeleteTextures(1, &img_tex_);
    img_tex_ = 0;
    img_w_ = img_h_ = 0;
}

void App::set_image(int w, int h, const uint32_t* px) {
    if (img_tex_ && w == img_w_ && h == img_h_) {
        glBindTexture(GL_TEXTURE_2D, img_tex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        return;
    }
    free_image();
    glGenTextures(1, &img_tex_);
    glBindTexture(GL_TEXTURE_2D, img_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    img_w_ = w;
    img_h_ = h;
}

void App::select(const Sel& s) {
    sel_folder_.clear();   // an item picked: no folder
    if (s == sel_) return;
    audio_.stop();
    sel_ = s;
    pick_req_ = item_req(s);   // the tree sets a wider one for a character / room / file node right after
    built_ = Sel();
    anim_time_ = 0;
    anim_play_ = true;
    img_zoom_ = 0;
    script_sel_ = 0;
    play_from_ = 0;
    vplay_ = false;
    vtime_ = 0;
}

// подготовить вьюпорт под выбранное (3D, картинка, звук, видео)
// the walkable floor of the room (collision floors), in game coordinates; false - none
static bool room_floor_box(const RoomInfo& r, float mn[3], float mx[3]) {
    bool any = false;
    for (int k = 0; k < 3; k++) { mn[k] = 1e9f; mx[k] = -1e9f; }
    for (int row = 0; row < 17; row++)
        for (auto& f : r.coll.floors[row]) {
            if (!f.sx || !f.sz) continue;
            any = true;
            mn[0] = std::min(mn[0], (float)std::min<int>(f.x, f.x + f.sx)); mx[0] = std::max(mx[0], (float)std::max<int>(f.x, f.x + f.sx));
            mn[2] = std::min(mn[2], (float)std::min<int>(f.z, f.z + f.sz)); mx[2] = std::max(mx[2], (float)std::max<int>(f.z, f.z + f.sz));
            for (int k = 0; k < 4; k++) { mn[1] = std::min(mn[1], (float)f.h[k] - 2500.0f); mx[1] = std::max(mx[1], (float)f.h[k]); }
        }
    return any;
}

void App::build_selection() {
    if (built_ == sel_) return;
    built_ = sel_;
    free_image();
    pcm_.clear();
    vframes_.clear();
    vshown_ = -1;
    v3d_.clear();
    Loaded* L = load(sel_.f);
    if (!L) return;
    FileContent& c = L->c;
    int nm = (int)c.meshes.size();
    std::vector<float> lines;
    switch (sel_.k) {
    case K_MESH:
        if (sel_.a >= 0 && sel_.a < nm) {
            // 06.10.2026: the room as the game shows it - pieces the script puts at a place stand there (pieces.h)
            if (c.meshes[sel_.a].kind == "room" && c.is_room) v3d_.set_mesh(room_view_tris(*L), room_vram(*L));
            else v3d_.set_mesh(c.meshes[sel_.a].tris, c.vram);
            v3d_.cull = c.meshes[sel_.a].kind == "room" && !no_cull_;
        }
        break;
    case K_MODEL:
    case K_MOTION:
        if (sel_.a >= 0 && sel_.a < (int)c.actors.size()) {
            v3d_.set_actor(c.actors[sel_.a].model, c.vram);
            v3d_.cull = false;
        }
        break;
    case K_COLL:
    case K_CAMS:
    case K_TRIGS:
        if (nm) {
            v3d_.set_mesh(c.is_room ? room_view_tris(*L) : c.meshes[0].tris, c.is_room ? room_vram(*L) : c.vram);
            v3d_.cull = !no_cull_;
            room_overlay(c.room, sel_.k == K_COLL ? 1 : sel_.k == K_CAMS ? 2 : 4, lines);
            v3d_.set_lines(lines);
        }
        break;
    case K_ENEMIES:
    case K_ITEMS:
    case K_DOORS:
    case K_ZONES:
    case K_PIECES:
    case K_SCRIPT:
        if (sel_.k == K_SCRIPT && !L->c.is_room) break;
        if (nm) {
            ensure_placement(*L);
            ensure_edit(*L);
            v3d_.set_mesh(room_view_tris(*L), room_vram(*L));
            v3d_.cull = !no_cull_;
            if (sel_.k == K_SCRIPT) scenario_refresh(*L);   // 10.10.2026: markers of the shown event
            else if (sel_.k == K_ENEMIES || sel_.k == K_ITEMS || sel_.k == K_PIECES)
                placement_refresh(*L, sel_.k == K_ITEMS ? 1 : sel_.k == K_PIECES ? 2 : 0);   // 06.10.2026: the picked one white
            else {
                json arr = sel_.k == K_DOORS ? L->doors : L->zones;
                json shown = json::array();
                for (auto& e : arr) if (!e.value("removed", false)) shown.push_back(e);
                placement_overlay(shown, sel_.k == K_DOORS ? 2 : 3, lines);
                v3d_.set_lines(lines);
            }
        }
        break;
    case K_EXE_TEXTS: table_ = exe_texts_json(); break;
    case K_EXE_ITEMS: table_ = exe_items_json(); break;
    case K_EXE_DOORS: table_ = exe_door_scenes_json(); break;
    case K_ROSTER: {
        Bytes ovl;
        if (disc_.read_file(*sel_.f, ovl)) table_ = roster_json(ovl, sel_.f->stem[2] - '0');
        break;
    }
    case K_PTEX: {
        const auto& cr = part_crops(*L, sel_.a);
        if (sel_.b >= 0 && sel_.b < (int)cr.size()) set_image(cr[sel_.b].w(), cr[sel_.b].h(), cr[sel_.b].rgba.data());
        break;
    }
    case K_TEXTURE:
        ensure_crops(*L);
        if (sel_.a >= 0 && sel_.a < (int)L->crops.size()) {
            const TexCrop& cr = L->crops[sel_.a];
            set_image(cr.w(), cr.h(), cr.rgba.data());
        }
        break;
    case K_IMAGE:
        if (sel_.a >= 0 && sel_.a < (int)c.images.size()) set_image(c.images[sel_.a].w, c.images[sel_.a].h, c.images[sel_.a].rgba.data());
        break;
    case K_UPLOAD:
        if (sel_.a >= 0 && sel_.a < (int)c.uploads.size()) {
            const Upload& u = c.uploads[sel_.a];
            std::vector<uint32_t> px;
            c.vram.decode_raw15(u.x, u.y, u.w, u.h, px);
            set_image(u.w, u.h, px.data());
        }
        break;
    case K_VRAM: {
        std::vector<uint32_t> px;
        c.vram.decode_raw15(0, 0, 1024, 512, px);
        set_image(1024, 512, px.data());
        break;
    }
    case K_SAMPLE:
        if (sel_.a >= 0 && sel_.a < (int)c.banks.size() && sel_.b >= 0 && sel_.b < (int)c.banks[sel_.a].vab.samples.size()) {
            const VabSample& s = c.banks[sel_.a].vab.samples[sel_.b];
            pcm_ = s.pcm;
            pcm_ch_ = 1;
            pcm_rate_ = sample_rate_for(s, sel_.f->cat != "sound\\bgm");
        }
        break;
    case K_SONG:
        if (sel_.a >= 0 && sel_.a < (int)c.seqs.size()) {
            SongPcm sp;
            if (render_song(c, sel_.a, sp)) {
                pcm_ = std::move(sp.pcm);
                pcm_ch_ = 2;
                pcm_rate_ = 44100;
            }
        }
        break;
    case K_XA:
        if (sel_.a >= 0 && sel_.a < (int)c.xa.size()) {
            xa_decode(disc_, c.xa[sel_.a], pcm_);
            pcm_ch_ = c.xa[sel_.a].channels;
            pcm_rate_ = c.xa[sel_.a].rate;
        }
        break;
    case K_VIDEO:
        vframes_ = str_scan(disc_, *sel_.f);
        if (!c.xa.empty()) {
            xa_decode(disc_, c.xa[0], pcm_);
            pcm_ch_ = c.xa[0].channels;
            pcm_rate_ = c.xa[0].rate;
        }
        break;
    default:
        break;
    }
    {   // 06.10.2026: room views - the camera fits the walkable floor (a sky dome around outdoor rooms made the room a speck)
        float mn[3], mx[3];
        const bool room_view = c.is_room && (sel_.k == K_COLL || sel_.k == K_CAMS || sel_.k == K_TRIGS || sel_.k == K_ENEMIES ||
                                             sel_.k == K_ITEMS || sel_.k == K_DOORS || sel_.k == K_ZONES || sel_.k == K_PIECES ||
                                             (sel_.k == K_MESH && sel_.a >= 0 && sel_.a < nm && c.meshes[sel_.a].kind == "room"));
        if (room_view && room_floor_box(c.room, mn, mx)) v3d_.fit_box(mn, mx);
        if (room_view && look_on_) {
            const float a[3] = {look_[0] - look_[2], -look_[2], look_[1] - look_[2]}, b[3] = {look_[0] + look_[2], 0, look_[1] + look_[2]};
            v3d_.fit_box(a, b);
        }
    }
    info_ = describe(c);
}

// ---------------------------------------------------------------- кадр
void App::frame() {
    Uint64 now = SDL_GetPerformanceCounter();
    float dt = (float)(now - last_ticks_) / (float)SDL_GetPerformanceFrequency();
    last_ticks_ = now;
    if (dt > 0.1f) dt = 0.1f;
    if (shot_path.size()) dt = 1.0f / 30;   // снимок — детерминированное время
    if (anim_play_ && sel_.k == K_MOTION) anim_time_ += dt;
    if (vplay_ && pcm_.empty()) vtime_ += dt;
    frame_no++;
    // автоматизация (проверка без человека): выгрузка, затем «Сконвертировать всё»
    if (frame_no == 3 && !do_export_.empty() && disc_.is_open() && !set_.base.empty()) {
        create_source_tree(source_root());
        if (upper(do_export_) == "ALL") start_export(nullptr);
        else if (sel_.f) start_export(sel_.f);
        do_export_.clear();
    }
    if (frame_no > 3 && do_convert_ && !job_ && do_export_.empty()) {
        start_convert();
        do_convert_ = false;
    }
    // проверка без человека: выгрузка выбранной папки / вещи (как кнопка Export selected), когда каталог готов и дерево нарисовано
    if (frame_no > 3 && !job_ && lib_.ready.load() && !set_.base.empty() && (!do_export_folder_.empty() || do_export_pick_)) {
        static int waited = 0;
        if (!do_export_folder_.empty()) {
            auto it = folder_req_.find(do_export_folder_);
            if (it != folder_req_.end()) {
                pick_folder(do_export_folder_);
                start_export_req(ExportReq(it->second));
                do_export_folder_.clear();
            } else if (++waited > 30) {
                status_ = "no folder " + do_export_folder_ + " in the tree";
                do_export_folder_.clear();
            }
        } else if (anim_open_.empty() || pick_req_.count() || ++waited > 30) {   // --anim-open: the character's node, once drawn
            if (anim_open_.empty() || !pick_req_.count()) pick_req_ = item_req(sel_);
            start_export_req(ExportReq(pick_req_));
            do_export_pick_ = false;
        }
    }
    if (!names_done_ && lib_.ready.load()) write_names();
    // 10.10: the doors into every room (Test in game) - in its own thread once the catalog is there
    if (lib_.ready.load() && !door_in_job_ && !door_in_ready_.load()) door_in_start();
    if (do_test_ && door_in_ready_.load() && test_room()) { do_test_ = false; test_in_game(); }
    idle_frames = busy() ? 0 : idle_frames + 1;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!disc_.is_open()) {
        ImVec2 av = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + av.x * 0.5f - 230, ImGui::GetCursorPosY() + av.y * 0.40f));
        ImGui::BeginGroup();
        if (ImGui::Button(T("Open the disc image...", "Открыть образ диска..."), ImVec2(460, 46))) {
            std::string p = pick_image_dialog(win_);
            if (!p.empty()) open_image(p);
        }
        if (!status_.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", status_.c_str());
        ImGui::EndGroup();
    } else {
        top_bar();
        float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("tree", ImVec2(320, h), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        tree();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("viewport", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        viewport();
        ImGui::EndChild();
        status_bar();
    }
    ImGui::End();
    if (show_errors_) win_errors();
    if (show_packs_ && disc_.is_open()) win_packs();
    if (job_ && !log_.running && job_->joinable()) {
        job_->join();
        job_.reset();
        if (job_is_convert_) {
            status_ = strfmt(T("Built: %d", "Собрано: %d"), conv_.built);
            if (conv_.errors) {
                status_ += strfmt(T(", errors: %d", ", ошибок: %d"), conv_.errors);
                show_errors_ = shot_path.empty();
            }
        } else {
            status_ = strfmt(T("Exported: %d", "Выгружено: %d"), log_.done.load());
            if (log_.errors) {
                status_ += strfmt(T(", errors: %d", ", ошибок: %d"), log_.errors.load());
                show_errors_ = shot_path.empty();
            }
        }
    }
}

// образ диска слева, две кнопки справа
void App::top_bar() {
    std::string img = path_u8(disc_.image_path.filename());
    if (ImGui::Button((img + "##img").c_str())) {
        std::string p = pick_image_dialog(win_);
        if (!p.empty()) open_image(p);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path_u8(disc_.image_path).c_str());
    // 06.10.2026 (player: "source and mods are buttons - make it plain: up top, lit like buttons"): the two folders next to
    // the image. Click: open (choose one, if none yet). Right click: change.
    auto folder = [&](const char* label, bool have, const fs::path& path, bool mods) {
        ImGui::SameLine();
        if (ImGui::Button(label)) {
            if (!have) { if (mods) choose_mods(); else choose_base(); }
            else { std::error_code ec; fs::create_directories(path, ec); open_in_explorer(path); }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { if (mods) choose_mods(); else choose_base(); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s", have ? path_u8(path).c_str() : T("Not chosen", "Не выбрана"),
                              T("Click: open. Right click: change.", "Щелчок: открыть. Правая кнопка: сменить."));
    };
    folder("source", !set_.base.empty(), source_root(), false);
    {   // 10.10.2026: mods - the mod packages window (on, off, order, the one the studio writes into); right click: another folder
        ImGui::SameLine();
        const std::string lbl = work_label() + "##mods";
        if (ImGui::Button(lbl.c_str())) {
            if (set_.mods.empty()) choose_mods();
            else { packs_rescan(); show_packs_ = true; }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) choose_mods();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s", set_.mods.empty() ? T("Not chosen", "Не выбрана") : path_u8(work_root()).c_str(),
                              T("Click: mod packages. Right click: another mods folder.",
                                "Щелчок: пакеты модов. Правая кнопка: другая папка mods."));
    }
    // 06.10.2026 (player: "I pick a folder - I export it; I pick a mesh - only it"): Export selected - the picked folder or item
    const char* bs = T("Export selected", "Выгрузить выбранное");
    const char* bx = T("Export all", "Выгрузить всё");
    const char* bc = T("Convert all", "Сконвертировать всё");
    const char* bt = T("Test in game", "Проверить в игре");   // 10.10.2026: the game right in the picked room (ui_test.cpp)
    const float wt = ImGui::CalcTextSize(bt).x + ImGui::GetStyle().FramePadding.x * 2;
    float ws = ImGui::CalcTextSize(bs).x + ImGui::GetStyle().FramePadding.x * 2;
    float wx = ImGui::CalcTextSize(bx).x + ImGui::GetStyle().FramePadding.x * 2;
    float wc = ImGui::CalcTextSize(bc).x + ImGui::GetStyle().FramePadding.x * 2;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float right = ImGui::GetWindowContentRegionMax().x;
    // 08.10.2026 (player: "when we export - glTF, FBX and OBJ to choose from"): the format of exported meshes and models
    const float wf = ImGui::CalcTextSize("glTF").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFrameHeight() + 6;
    // 08.10.2026 (player: "a model alone or with a pack of its animations, as the game groups them"): the animation pack
    const float wa = ImGui::CalcTextSize("Base: actions, ladders, doors").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFrameHeight() + 6;
    if (job_) {
        float pw = wa + wf + ws + wx + wc + wt + sp * 5;
        ImGui::SameLine(right - pw);
        int total = std::max(1, log_.total.load());
        std::string cur;
        {
            std::lock_guard<std::mutex> lk(log_.mu);
            cur = log_.current;
        }
        ImGui::ProgressBar(log_.total ? (float)log_.done / total : 0.0f, ImVec2(pw - ImGui::GetFrameHeight() - sp, 0), cur.c_str());
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) log_.cancel = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Cancel", "Отменить"));
        return;
    }
    ImGui::SameLine(right - wa - wf - ws - wx - wc - wt - sp * 5);
    {
        const ExportReq* pr = picked_req();
        const bool one = pr && !pr->actor_mots.empty();
        const int pa = (pr && !pr->actors.empty() && lib_.ready.load()) ? pr->actors[0] : -1;
        const std::vector<AnimPack> packs = pa >= 0 ? anim_packs(lib_, pa) : std::vector<AnimPack>{
            {"none", "No animation", "Без анимации", {}, 0}, {"all", "All animation", "Вся анимация", {}, 0}};
        const AnimPack* cur = find_pack(packs, set_.anim_pack);
        std::string shown;
        if (one) shown = T("The picked animation", "Выбранная анимация");
        else if (cur) shown = T(cur->en.c_str(), cur->ru.c_str());
        else shown = set_.anim_pack == "none" ? T("No animation", "Без анимации") : T("All animation", "Вся анимация");
        if (!anim_open_.empty() && pa >= 0) {             // --anim-open: the list open for the shot
            const ImGuiID pid = ImHashStr("##ComboPopup", 0, ImGui::GetID("##anim_pack"));
            if (!ImGui::IsPopupOpen(pid, ImGuiPopupFlags_None)) ImGui::OpenPopupEx(pid);
        }
        ImGui::SetNextItemWidth(wa);
        ImGui::BeginDisabled(one);
        if (ImGui::BeginCombo("##anim_pack", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (const AnimPack& p : packs) {
                std::string lbl = std::string(p.depth ? "    " : "") + T(p.en.c_str(), p.ru.c_str());
                if (p.key != "none") lbl += strfmt("  (%d)", (int)p.motions.size());
                if (ImGui::Selectable(lbl.c_str(), p.key == set_.anim_pack) && p.key != set_.anim_pack) {
                    set_.anim_pack = p.key;
                    save_settings();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", one ? T("The animation picked in the tree goes out with the model shown in Export selected",
                                            "Анимация, выбранная в дереве, выгружается с моделью из Export selected")
                                        : T("Animations of an exported character model: none, all, or one pack as the game groups them "
                                            "(dinosaurs: none or all). One animation, set or group: pick it in the tree.",
                                            "Анимация выгружаемой модели персонажа: без неё, вся или один пакет, как их группирует игра "
                                            "(динозавры: без или вся). Одна анимация, набор или группа: выбери в дереве."));
    }
    ImGui::SameLine();
    {
        static const ModelFormat kf[3] = {ModelFormat::Gltf, ModelFormat::Fbx, ModelFormat::Obj};
        const int cur = std::max(0, std::min(2, set_.model_format));
        ImGui::SetNextItemWidth(wf);
        if (ImGui::BeginCombo("##model_format", model_format_name(kf[cur]))) {
            for (int k = 0; k < 3; k++) {
                if (ImGui::Selectable(model_format_name(kf[k]), k == cur) && k != cur) {
                    set_.model_format = k;
                    save_settings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", k == 2 ? T("The mesh in its rest pose with materials. OBJ keeps no skeleton and no animation.",
                                                       "Меш в позе покоя с материалами. В OBJ нет скелета и анимации.")
                                                   : T("The mesh with its skeleton, skin and every animation.",
                                                       "Меш со скелетом, скином и всеми анимациями."));
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Format of exported meshes and models: glTF, FBX (both with skeleton and animation) or OBJ (mesh only)",
                                      "Формат выгружаемых мешей и моделей: glTF, FBX (оба со скелетом и анимацией) или OBJ (только меш)"));
    }
    ImGui::SameLine();
    {
        const ExportReq* pr = picked_req();
        ImGui::BeginDisabled(pr == nullptr);
        if (ImGui::Button(bs) && pr) start_export_req(ExportReq(*pr));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", pr ? strfmt(T("%s: %d to export into source", "%s: %d в source"), pr->label.c_str(), pr->count()).c_str()
                                       : T("Pick a folder or an item on the left (right click on it: Export)",
                                           "Выбери папку или вещь слева (правая кнопка на ней: Export)"));
    }
    ImGui::SameLine();
    if (ImGui::Button(bx)) start_export(nullptr);
    ImGui::SameLine();
    if (ImGui::Button(bc)) start_convert();
    ImGui::SameLine();
    {
        const DiscFile* tr = test_room();
        ImGui::BeginDisabled(tr == nullptr);
        if (ImGui::Button(bt)) test_in_game();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr ? strfmt(T("The game in %s with your mods, from your newest quicksave",
                                                   "Игра в %s с твоими модами, с последнего быстрого сохранения"), tr->stem.c_str()).c_str()
                                       : T("Pick a room on the left", "Выбери комнату слева"));
    }
}

bool App::choose_base() {
    std::string start = !set_.base.empty() ? set_.base : exe_dir_;
    std::string p = pick_folder_dialog(win_, T("Where to create the source folder", "Где создать папку source"), start);
    if (p.empty()) return false;
    set_.base = p;
    create_source_tree(source_root());
    save_settings();
    return true;
}

bool App::choose_mods() {
    std::string start = !set_.mods.empty() ? set_.mods : !set_.base.empty() ? set_.base : exe_dir_;
    std::string p = pick_folder_dialog(win_, T("The game's mods folder", "Папка mods игры"), start);
    if (p.empty()) return false;
    // the game folder itself picked: its mods folder
    if (fs::exists(u8path(p) / "DinoCrisisME.exe")) p = path_u8(u8path(p) / "mods");
    set_.mods = p;
    names_done_ = false;   // names.txt into the newly chosen folder
    save_settings();
    return true;
}

// ---------------------------------------------------------------- выгрузка
// «Выгрузить всё» (only = nullptr): файлы (комнаты, экраны, ролики, речь, программа) и каталог без повторов (персонажи,
// оружие, предметы, переходы, звук); only — один файл
void App::start_export(const DiscFile* only) {
    if (job_) return;
    if (set_.base.empty() && !choose_base()) return;
    std::vector<const DiscFile*> files;
    if (only) files.push_back(only);
    else
        for (auto& f : disc_.files) if (!f.cat.empty()) files.push_back(&f);
    if (files.empty()) return;
    create_source_tree(source_root());
    log_.lines.clear();
    log_.cancel = false;
    log_.running = true;
    log_.done = 0;
    log_.errors = 0;
    log_.total = (int)files.size();
    job_is_convert_ = false;
    status_.clear();
    fs::path root = source_root();
    const bool with_lib = !only;
    const int fmt = set_.model_format;
    job_ = std::make_unique<std::thread>([this, root, files, with_lib, fmt] {
        if (with_lib) {   // каталог строится своим потоком после открытия образа (~2 с) — дождаться
            {
                std::lock_guard<std::mutex> lk(log_.mu);
                log_.current = T("catalog", "каталог");
            }
            while (!lib_.ready.load() && !log_.cancel) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (lib_.ready) log_.total = (int)files.size() + (int)(lib_.actors.size() + lib_.meshes.size()) + 1;
        }
        ExportOptions eo;
        eo.model_format = (ModelFormat)fmt;
        Exporter ex(disc_, root, eo, &log_);
        ex.export_files(files);
        if (with_lib && !log_.cancel && lib_.ready) ex.export_library(lib_);
        log_.running = false;
    });
}

// ---------------------------------------------------------------- выгрузка выбранного (06.10.2026)
// player: "I pick a folder like characters - I can export it, everything in it; I pick only a mesh in it - only it goes out"
std::string App::tex_dir(const DiscFile* f, int part) {
    if (!f) return "textures";
    if (lib_.ready.load()) {
        if (part >= 0 && part < 10000) {
            for (auto& a : lib_.actors) {
                if (a.at.f == f && a.at.part == part) return "characters/" + a.id;
                for (auto& p : a.also) if (p.f == f && p.part == part) return "characters/" + a.id;
            }
        } else if (part >= 10000) {
            Loaded* L = load(f);
            const int mi = part - 10000;
            if (L && mi < (int)L->c.meshes.size() && L->c.meshes[mi].kind == "room") return "rooms/" + f->stem;
            for (auto& m : lib_.meshes) {
                if (m.at.f == f && m.at.part == mi) return m.cat + "/" + m.id;
                for (auto& p : m.also) if (p.f == f && p.part == mi) return m.cat + "/" + m.id;
            }
        }
    }
    return f->cat + "/" + f->stem;
}

// what an item takes when it is picked (the tree widens it for a character, a room and a file node)
ExportReq App::item_req(const Sel& s) {
    ExportReq r;
    if (!s.f) return r;
    const DiscFile* f = s.f;
    auto whole = [&] { r.label = upper(f->name) == "SLUS_009.22" ? f->name : f->stem; r.files.push_back(f); };
    auto part = [&](const char* p) { r.label = f->stem + ": " + p; r.files.push_back(f); r.room_part = p; };
    const bool lib = lib_.ready.load();
    switch (s.k) {
    case K_MODEL: case K_MOTION:
        if (lib)
            for (size_t i = 0; i < lib_.actors.size(); i++) {
                const LibActor& a = lib_.actors[i];
                bool here = a.at.f == f && a.at.part == s.a;
                for (auto& p : a.also) here = here || (p.f == f && p.part == s.a);
                if (here) { r.label = a.id; r.actors.push_back((int)i); return r; }
            }
        whole();
        return r;
    case K_MESH: {
        Loaded* L = load(f);
        if (L && s.a >= 0 && s.a < (int)L->c.meshes.size() && L->c.meshes[s.a].kind == "room") { part("mesh"); return r; }
        if (lib)
            for (size_t i = 0; i < lib_.meshes.size(); i++) {
                const LibMesh& m = lib_.meshes[i];
                bool here = m.at.f == f && m.at.part == s.a;
                for (auto& p : m.also) here = here || (p.f == f && p.part == s.a);
                if (here) { r.label = m.id; r.meshes.push_back((int)i); return r; }
            }
        whole();
        return r;
    }
    case K_PTEX: {
        Loaded* L = load(f);
        if (L) {
            const auto& cr = part_crops(*L, s.a);
            if (s.b >= 0 && s.b < (int)cr.size()) { r.label = cr[s.b].file_name(); r.textures.push_back({tex_dir(f, s.a), cr[s.b]}); return r; }
        }
        return r;
    }
    case K_ENEMIES: part("enemies"); return r;
    case K_ITEMS: part("items"); return r;
    case K_DOORS: part("doors"); return r;
    case K_ZONES: part("zones"); return r;
    case K_PIECES: case K_SCRIPT: part("script"); return r;
    case K_COLL: part("collision"); return r;
    case K_TRIGS: part("footsteps"); return r;
    case K_CAMS: part("cameras"); return r;
    case K_TEXTS: part("texts"); return r;
    case K_SONG:
        if (lib)
            for (size_t i = 0; i < lib_.songs.size(); i++)
                if (lib_.songs[i].at.f == f && lib_.songs[i].index == s.a) { r.label = strfmt("%s song %d", f->stem.c_str(), s.a); r.songs.insert((int)i); return r; }
        return r;
    case K_SAMPLE:
        if (lib)
            for (size_t i = 0; i < lib_.samples.size(); i++) {
                const LibSample& sm = lib_.samples[i];
                if (sm.at.f == f && sm.bank == s.a && sm.index == s.b) { r.label = strfmt("%s %03d", f->stem.c_str(), s.b); r.samples.insert((int)i); return r; }
            }
        return r;
    default:
        whole();
        return r;
    }
}

void App::pick_folder(const std::string& key) {
    select(Sel());
    sel_folder_ = key;
}

const ExportReq* App::picked_req() {
    if (!sel_folder_.empty()) {
        auto it = folder_req_.find(sel_folder_);
        return it != folder_req_.end() && it->second.count() ? &it->second : nullptr;
    }
    return pick_req_.count() ? &pick_req_ : nullptr;
}

void App::export_menu(const ExportReq& r) {
    const int n = r.count();
    const std::string lbl = n > 1 ? strfmt(T("Export (%d)", "Выгрузить (%d)"), n) : std::string(T("Export", "Выгрузить"));
    if (ImGui::MenuItem(lbl.c_str(), nullptr, false, job_ == nullptr && n > 0)) start_export_req(r);
}

void App::start_export_req(const ExportReq& r) {
    if (job_ || r.count() == 0) return;
    const bool need_lib = !r.actors.empty() || !r.meshes.empty() || !r.scenes.empty() || r.sounds_all || !r.samples.empty() || !r.songs.empty();
    if (need_lib && !lib_.ready) return;
    if (set_.base.empty() && !choose_base()) return;
    create_source_tree(source_root());
    log_.lines.clear();
    log_.cancel = false;
    log_.running = true;
    log_.done = 0;
    log_.errors = 0;
    log_.total = r.count();
    job_is_convert_ = false;
    status_.clear();
    fs::path root = source_root();
    const int fmt = set_.model_format;
    const std::string pack = set_.anim_pack;
    job_ = std::make_unique<std::thread>([this, root, r, fmt, pack] {
        ExportOptions o;
        o.room_part = r.room_part;
        o.model_format = (ModelFormat)fmt;
        Exporter ex(disc_, root, o, &log_);
        ex.export_files(r.files);
        auto step = [&](const std::string& what, bool ok) {
            {
                std::lock_guard<std::mutex> lk(log_.mu);
                log_.current = what;
            }
            if (!ok) log_.errors++;
            log_.done++;
        };
        for (int k : r.actors) {
            if (log_.cancel) break;
            // 08.10.2026: the model's motions - the ones picked in the tree, else the pack chosen up top; the whole character
            // keeps the model's own name, a part gets its name after it (regina_mesh, regina_core_move, regina_CORE_05)
            std::vector<int> mots;
            std::string suffix;
            auto am = r.actor_mots.find(k);
            if (am != r.actor_mots.end()) {
                mots = am->second;
                suffix = "_" + (r.mots_tag.empty() ? std::string("part") : r.mots_tag);
            } else {
                // a character's node takes all its models: the pack goes with the main one, its costumes and variants stay
                // without animation as before (no duplicates); a model picked by itself takes the pack
                const LibActor& la = lib_.actors[k];
                bool with_main = false;
                if (la.chr >= 0 && lib_.chars[la.chr].actors[0] != k)
                    for (int o : r.actors) with_main = with_main || o == lib_.chars[la.chr].actors[0];
                if (with_main) { step(la.id, ex.export_actor(lib_, k)); continue; }
                const std::vector<AnimPack> packs = anim_packs(lib_, k);
                std::string eff = find_pack(packs, pack) ? pack : (pack == "none" ? "none" : "all");
                mots = anim_pack_motions(lib_, k, eff);
                if (eff == "none") suffix = "_mesh";
                else if (eff != "all") { suffix = "_" + eff; std::replace(suffix.begin(), suffix.end(), ':', '_'); }
            }
            step(lib_.actors[k].id + suffix, ex.export_actor(lib_, k, &mots, suffix));
        }
        for (int k : r.meshes) { if (log_.cancel) break; step(lib_.meshes[k].id, ex.export_mesh(lib_, k)); }
        for (const DiscFile* f : r.scenes) { if (log_.cancel) break; step(f->stem + "\\scene", ex.export_scene_motions(lib_, f)); }
        if (!log_.cancel && (r.sounds_all || !r.samples.empty() || !r.songs.empty())) {
            ex.export_sounds(lib_, r.sounds_all ? nullptr : &r.samples, r.sounds_all ? nullptr : &r.songs);
            step("sound", true);
        }
        for (auto& t : r.textures) { if (log_.cancel) break; step(t.second.file_name(), ex.export_texture(u8path(t.first), t.second)); }
        log_.running = false;
    });
}

// одна вещь каталога: 'c' — персонаж (все его модели; движения — в основной), 'a' — одна модель, 'm' — меш (оружие, предмет,
// переход)
void App::start_export_item(char kind, int i) {
    if (job_ || !lib_.ready) return;
    if (set_.base.empty() && !choose_base()) return;
    create_source_tree(source_root());
    log_.lines.clear();
    log_.cancel = false;
    log_.running = true;
    log_.done = 0;
    log_.errors = 0;
    std::vector<int> items;
    if (kind == 'c') { if (i >= 0 && i < (int)lib_.chars.size()) items = lib_.chars[i].actors; kind = 'a'; }
    else items.push_back(i);
    log_.total = (int)items.size();
    job_is_convert_ = false;
    status_.clear();
    fs::path root = source_root();
    const int fmt = set_.model_format;
    job_ = std::make_unique<std::thread>([this, root, kind, items, fmt] {
        ExportOptions eo;
        eo.model_format = (ModelFormat)fmt;
        Exporter ex(disc_, root, eo, &log_);
        for (int k : items) {
            {
                std::lock_guard<std::mutex> lk(log_.mu);
                log_.current = kind == 'a' ? lib_.actors[k].id : lib_.meshes[k].id;
            }
            const bool ok = kind == 'a' ? ex.export_actor(lib_, k) : ex.export_mesh(lib_, k);
            if (!ok) log_.errors++;
            log_.done++;
        }
        log_.running = false;
    });
}

// ---------------------------------------------------------------- конвертация mods
void App::start_convert() {
    if (job_) return;
    if (set_.mods.empty() && !choose_mods()) return;   // the game's mods folder, never a silent one next to source
    conv_ = ConvertReport();
    log_.lines.clear();
    log_.cancel = false;
    log_.running = true;
    log_.done = 0;
    log_.total = 0;
    job_is_convert_ = true;
    status_.clear();
    fs::path mods = work_root();   // 10.10: the package the studio works in (or mods itself)
    job_ = std::make_unique<std::thread>([this, mods] {
        convert_all(disc_, mods, conv_, &log_, lib_.ready ? &lib_ : nullptr);
        log_.running = false;
    });
}

// список ошибок (выгрузки или конвертации) — только когда они есть
void App::win_errors() {
    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(status_.c_str(), &show_errors_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    std::vector<std::string> lines;
    if (job_is_convert_) lines = conv_.lines;
    else {
        std::lock_guard<std::mutex> lk(log_.mu);
        lines = log_.lines;
    }
    ImGui::BeginChild("lines", ImVec2(620, std::min(420.0f, 8 + ImGui::GetTextLineHeightWithSpacing() * (lines.size() + 1))));
    for (auto& l : lines) ImGui::TextWrapped("%s", l.c_str());
    ImGui::EndChild();
    if (ImGui::Button("OK", ImVec2(120, 0))) show_errors_ = false;
    ImGui::End();
}

// строка состояния: что выбрано · итог работы · source и mods (клик — открыть, правый клик — сменить)
// путь, укороченный слева до ширины w: «…\base\source»
static std::string fit_path(const std::string& p, float w) {
    if (ImGui::CalcTextSize(p.c_str()).x <= w) return p;
    size_t pos = 0;
    for (;;) {
        size_t n = p.find('\\', pos + 1);
        if (n == std::string::npos) return "…" + p.substr(p.rfind('\\') == std::string::npos ? 0 : p.rfind('\\'));
        std::string s = "…" + p.substr(n);
        if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
        pos = n;
    }
}

void App::status_bar() {
    float x0 = ImGui::GetCursorPosX();
    const ExportReq* fr = sel_folder_.empty() ? nullptr : picked_req();
    if (sel_.f) ImGui::TextUnformatted(info_.c_str());
    else if (fr) ImGui::TextUnformatted(fr->label.c_str());
    else ImGui::TextDisabled("%s", T("Pick something on the left", "Выбери что-нибудь слева"));
    float info_w = ImGui::GetItemRectSize().x;
    float sp = ImGui::GetStyle().ItemSpacing.x * 3;
    float right = ImGui::GetWindowContentRegionMax().x;
    // 06.10.2026: the source and mods folders moved up (top_bar, buttons) - here only the result of the last job
    if (!status_.empty()) {
        ImGui::SameLine(std::max(x0 + info_w + sp, right - ImGui::CalcTextSize(status_.c_str()).x));
        ImGui::TextColored(ImVec4(0.95f, 0.62f, 0.30f, 1), "%s", status_.c_str());
        if (ImGui::IsItemClicked() && (conv_.errors || log_.errors)) show_errors_ = true;
    }
}

}  // namespace dc
