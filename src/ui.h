// Окно приложения: сверху — образ диска и две кнопки («Выгрузить всё», «Сконвертировать всё»), слева дерево
// «папка mods → файл → содержимое», в центре один вьюпорт по типу выбранного (3D, картинка, звук, видео, таблица),
// внизу строка состояния (что выбрано, итог работы, папки source и mods).
#pragma once
#include <SDL.h>

#include <atomic>
#include <list>
#include <memory>
#include <thread>

#include "convert.h"
#include "gltf.h"
#include "library.h"
#include "packs.h"
#include "scenario.h"
#include "view3d.h"

namespace dc {

struct Settings {
    std::string image;    // образ диска (Track 1 .bin / .cue)
    std::string base;     // папка, где создаётся source (обычно папка игры)
    std::string mods;     // папка mods (по умолчанию <base>\mods)
    int lang = -1;
    int model_format = 0; // 08.10.2026: формат моделей выгрузки - 0 glTF, 1 FBX, 2 OBJ (ModelFormat)
    std::string anim_pack = "all";   // 08.10.2026: пакет анимаций при выгрузке модели персонажа (anim_packs.h)
    std::string work;     // 10.10.2026: the mod package the studio works in (a folder under mods; empty - mods itself)
};

class Audio {
public:
    ~Audio();
    void play(const std::vector<int16_t>& pcm, int channels, int rate, double from_sec = 0);
    void stop();
    bool playing() const;
    double position() const;   // секунды от начала звука
private:
    SDL_AudioDeviceID dev_ = 0;
    int out_rate_ = 44100;
    size_t total_ = 0;
    double from_ = 0;
};

enum Kind { K_NONE, K_FILE, K_MESH, K_MODEL, K_MOTION, K_TEXTURE, K_SAMPLE, K_SONG, K_XA, K_VIDEO, K_IMAGE, K_UPLOAD, K_VRAM,
            K_COLL, K_CAMS, K_TRIGS, K_TEXTS, K_SCRIPT, K_ENEMIES, K_ITEMS, K_DOORS, K_ZONES, K_PIECES,
            K_EXE_TEXTS, K_EXE_ITEMS, K_EXE_DOORS, K_ROSTER,
            K_PTEX };   // текстура одной части файла: a — модель (индекс) или меш (10000 + индекс), b — номер текстуры

struct Sel {
    const DiscFile* f = nullptr;
    Kind k = K_NONE;
    int a = -1, b = -1;
    bool operator==(const Sel& o) const { return f == o.f && k == o.k && a == o.a && b == o.b; }
    bool operator!=(const Sel& o) const { return !(*this == o); }
};

// 06.10.2026 (player: "I pick a folder like characters - I can export it, everything in it; I pick only a mesh in it - only it
// goes out; all in that spirit"): what one export of a tree node takes
struct ExportReq {
    std::string label;                                   // what it is (button tooltip, the folder panel)
    std::vector<const DiscFile*> files;                  // whole files: rooms with their data, screens, movies, voice, tables
    std::vector<int> actors, meshes;                     // catalog models (Library::actors) and still meshes (Library::meshes)
    std::vector<const DiscFile*> scenes;                 // scene motions of these rooms
    bool sounds_all = false;
    std::set<int> samples, songs;                        // catalog sound (Library::samples / songs)
    std::vector<std::pair<std::string, TexCrop>> textures;   // folder under source, picture
    std::string room_part;                               // files are rooms: only this part (export.h ExportOptions)
    // 08.10.2026: models (actors) with just these motions - one motion, a set or a group picked in the tree; the other actors
    // take the animation pack chosen up top (Settings::anim_pack)
    std::map<int, std::vector<int>> actor_mots;
    std::string mots_tag;                                // their name in the file names (ASCII): the motion, set or group
    int count() const {
        return (int)(files.size() + actors.size() + meshes.size() + scenes.size() + textures.size()) +
               ((sounds_all || !samples.empty() || !songs.empty()) ? 1 : 0);
    }
};

// 10.10.2026 (Test in game, ui_test.cpp): a door into a room - Regina's place and turn there, the door's scene, where from
struct DoorIn { int x = 0, y = 0, z = 0, rot = 0, scene = 0; bool locked = false; std::string from; };

// правка комнаты в приложении (ui_edit.cpp): тексты, враги, предметы, скрипт — до «Сохранить в mods»
struct RoomEdit {
    bool loaded = false;
    std::vector<std::string> texts, texts_orig;
    json enemies, items;          // записи (с copy_of — новые, removed — убранные)
    std::string script, script_orig;
    json pieces;                  // 06.10.2026: CREATE commands of the script (pieces.h), place and turn edited here
    bool dirty_texts = false, dirty_enemies = false, dirty_items = false, dirty_script = false, dirty_pieces = false;
    std::string msg;
    // 10.10.2026: the scenario view of the script (ui_scenario.cpp), rebuilt whenever `script` changes
    ScriptImage scn_im;               // the game's script, decomposed once
    bool scn_im_ok = false;
    std::string scn_from;             // the text the model was built from
    std::vector<ScriptAtom> scn_atoms;
    Scenario scn;
    std::string scn_err;
    int scn_ev = -1;                  // the event (region) shown
    std::string scn_sel;              // the picked command ("@XXXX" / "#N"); "else:@XXXX" - the ELSE line of an IF
    bool scn_text = false;            // the script as text (hex) instead of the list
    std::string scn_tbuf, scn_tkey;   // the text of the picked message while it is edited (and whose it is)
    // 14.10.2026: the room view follows the scenario: enemies whose command differs from the game's stand where the scenario
    // puts them (offset -> their placement values before); the bytes of item zones the view was built with
    std::map<int64_t, json> scn_mirror;
    std::set<std::string> scn_open;   // 14.10.2026: scene blocks opened with More (their commands as lines)
    std::string scn_view_sig;
};

struct Loaded {
    FileContent c;
    std::vector<TexCrop> crops;   // все текстуры файла без повторов (меши и модели)
    bool crops_done = false;
    json enemies, items, doors, zones;   // расстановка комнаты (по запросу)
    bool placement_done = false;
    uint64_t used = 0;
    // каталог: наборы движений из других файлов на модели этого (Регина — общие, оружие, комнаты) и текстуры по частям
    std::vector<std::unique_ptr<Bytes>> ext_blocks;
    std::map<int, int> ext_set;                       // набор каталога → индекс в c.motion_sets
    std::map<int, std::vector<TexCrop>> part_crops;   // часть (модель / 10000 + меш) → её текстуры
    RoomEdit ed;
    Vram view_vram;                                   // 06.10.2026: the room's video memory with CORE's pictures (item box)
    bool view_vram_ok = false;
};

class App {
public:
    bool init(SDL_Window* win, int argc, char** argv);
    void frame();
    void shutdown();
    bool busy() const {
        return job_ != nullptr || !do_export_.empty() || do_convert_ || (lib_job_ && !lib_.ready) || !do_export_folder_.empty() || do_export_pick_;
    }
    int idle_frames = 0;   // кадров подряд без работы (снимок — когда итог уже нарисован)
    bool quit = false;
    std::string shot_path;   // проверка без человека: --shot <png> после N кадров
    int shot_after = 0;
    bool edit_probe_ = false;   // --edit-probe: правка мышью без человека (метка 0 → экран → пол, сдвиг на 1 м), отчёт рядом со снимком
    int frame_no = 0;
    void on_drop(const char* path);
    int names_only(int argc, char** argv);   // --names: mods\names.txt без окна (сборка пакета игры, rebuild.ps1)

private:
    SDL_Window* win_ = nullptr;
    Settings set_;
    std::string exe_dir_;
    // автоматизация для проверок: --do-export NAME|all, --do-convert, затем снимок/выход после работы
    std::string do_export_;
    bool do_convert_ = false;
    // 06.10.2026: --export-folder <key> (a folder of the tree: sec:weapons, characters/dinos, sound/songs...) and --export-pick
    // (what --select/--item picked) - Export of the picked thing, as the button does
    std::string do_export_folder_;
    bool do_export_pick_ = false;
    fs::path settings_dir_;
    Disc disc_;
    std::string disc_err_;
    std::map<const DiscFile*, std::unique_ptr<Loaded>> cache_;
    uint64_t use_clock_ = 0;
    Sel sel_, built_;
    std::string info_;   // что выбрано — для строки состояния
    // 3D
    View3D v3d_;
    bool anim_play_ = true;
    float anim_time_ = 0;
    Uint64 last_ticks_ = 0;
    // 2D
    unsigned img_tex_ = 0;
    int img_w_ = 0, img_h_ = 0;
    float img_zoom_ = 0, img_x_ = 0, img_y_ = 0;   // zoom 0 — вписать при показе
    // звук
    Audio audio_;
    std::vector<int16_t> pcm_;
    int pcm_ch_ = 1, pcm_rate_ = 44100;
    double play_from_ = 0;   // откуда играть (секунды)
    // видео
    std::vector<StrFrame> vframes_;
    int vshown_ = -1;
    double vtime_ = 0;
    bool vplay_ = false;
    // выгрузка / конвертация
    std::unique_ptr<std::thread> job_;
    ExportLog log_;
    ConvertReport conv_;
    bool job_is_convert_ = false;
    bool show_errors_ = false;
    std::string status_;
    // проверки без человека
    std::string start_select_, start_item_;
    bool no_cull_ = false;
    int script_sel_ = 0;

    void load_settings();
    void save_settings();
    void open_image(const std::string& path);
    Loaded* load(const DiscFile* f);
    void ensure_crops(Loaded& L);
    void ensure_placement(Loaded& L);
    void select(const Sel& s);
    void build_selection();
    void free_image();
    void set_image(int w, int h, const uint32_t* px);
    void apply_pose(const FileContent& c, const ActorPart& a, const MotionSet& ms, const Motion& mo);
    std::string describe(const FileContent& c);
    // каталог диска без повторов (library.h) — строится в своём потоке после открытия образа
    Library lib_;
    std::unique_ptr<std::thread> lib_job_;
    std::atomic<int> lib_done_{0}, lib_total_{0};
    std::map<const DiscFile*, std::vector<int>> lib_fx_, lib_ins_;   // уникальные эффекты / инструменты по первому файлу
    void start_library();
    // 06.10.2026 (player: "an added enemy comes once; killed - never again; like items"): flags the whole game uses (built
    // after the catalog, same thread) - a new item / enemy gets a free one when added, kept in its json, so it stays put
    std::atomic<bool> flags_ready_{false};
    std::set<int> flags7_, flags45_;
    int flags_bits7_ = 512;
    uint32_t stage_kinds_[10] = {};                       // per stage: bit k - kind k has behaviour code (placement.h)
    int new_flag(bool enemy);                             // -1: not ready / none free
    void make_new(json& c, bool items);                   // a new copy: its own flag, seen, a real item
    // 06.10.2026: an enemy is a look (kind + model + the picture it wears) with a role (enemy / scene) and a raptor's temper
    void enemy_look_role(Loaded& L, json& e, bool& changed);
    bool enemy_record_look(Loaded& L, const json& o, uint64_t& key, uint64_t& look, int& nmot);
    std::map<std::pair<const DiscFile*, uint32_t>, std::pair<bool, uint64_t>> look_cache_;   // (room, model) -> its look
    bool names_done_ = false;                             // mods\names.txt записан в этом запуске
    void write_names();
    int ext_set(Loaded& L, int lib_set);                 // индекс набора каталога в L.c.motion_sets (−1 — нет)
    const std::vector<TexCrop>& part_crops(Loaded& L, int part);
    // панели
    void top_bar();
    void tree();
    std::wstring flt_;                                    // фильтр, в нижнем регистре
    std::string start_filter_, start_open_;               // проверки без человека: --filter, --open characters,regina
    float look_[3] = {0, 0, 0};                           // --look x,z,half (mm): a room view fitted to that spot
    bool look_on_ = false;
    bool fmatch(std::initializer_list<std::string> parts) const;
    bool section(const char* key, const char* folder, const char* en, const char* ru, const ExportReq* req = nullptr);
    // 06.10.2026: export of what is picked. A folder of the tree (section or group) is picked by its key - what it holds is
    // gathered while the tree is drawn (the filter applies); an item - its request is set when it is picked
    std::string sel_folder_;
    std::map<std::string, ExportReq> folder_req_;
    bool folder_branch(const std::string& key, const char* id, const std::string& label, const std::string& hint, const ExportReq& req);
    ExportReq pick_req_;
    int last_actor_ = -1;                                // 08.10.2026: the model picked last - a common motion goes out with it
    ExportReq motions_req(const std::vector<int>& mots, int owner, const std::string& what, const std::string& tag);   // these motions with a model
    std::string anim_open_;                              // --anim-open <character>: picked in the tree, the pack list open (--shot)
    ExportReq item_req(const Sel& s);                    // the default request of a picked item
    void pick_folder(const std::string& key);
    void export_menu(const ExportReq& r);                // the right-click menu of a node: Export (what and how many)
    const ExportReq* picked_req();                       // what "Export selected" takes now (nullptr - nothing)
    void start_export_req(const ExportReq& r);
    std::string tex_dir(const DiscFile* f, int part);    // where a picture of this model / mesh / room goes under source
    void tree_char(int ci);                              // персонаж: модели, движения (один раз), текстуры (у владельца)
    void tree_model(int ai, bool inline_model);
    void tree_motions(const LibActor& on, const std::vector<int>& mots, const char* id, const char* en, const char* ru,
                      const ExportReq& req);   // req - what Export of the motions takes (the model that carries them / the scene)
    void tree_tex_owned(const std::vector<LibTex>& tex, const std::string& owner);
    void tree_mesh(int i);
    void tree_part_textures(const DiscFile* f, int part);
    void tree_room(const DiscFile& f);
    void tree_file(const DiscFile& f);
    void tree_sound();
    Sel first_content(const DiscFile& f);
    const DiscFile* open_req_ = nullptr;   // раскрыть узел файла в следующем кадре
    void viewport();
    void view_3d(const FileContent& c);
    void view_image();
    void view_audio();
    void view_video();
    void view_texts(const FileContent& c);
    void view_script(const FileContent& c);
    // 10.10.2026: the scenario of a room script (ui_scenario.cpp): events, commands as readable lines, fields, markers
    void scenario_panel(Loaded& L);
    bool scenario_sync(Loaded& L);            // the model from the script text when it changed
    bool scenario_commit(Loaded& L);          // the edited model -> the script text (false: it does not assemble, undone)
    void scenario_refresh(Loaded& L);
    void scenario_view_sync(Loaded& L);   // 14.10.2026: enemy models and item models of the view after the scenario changed         // the markers of the shown event in the 3D view
    int scn_sub_req_ = -1;                    // --scn-sub N (checks): show the event of subroutine N
    std::string scn_sel_req_;                 // --scn-sel @XXXX (checks, 14.10): pick that line of the event
    int scn_add_req_ = -1;                    // --scn-add-scene in|leap (checks, 14.10): Add > Enemy appears, once the game is read
    // 14.10.2026: places of the scenario dragged in the 3D view (ui_scenario.cpp scenario_viewport): 1..3 axis, 4 along the
    // floor, 5 turn, 6 the whole zone, 10..13 a corner of the zone
    bool scenario_viewport(bool hovered, float img_x, float img_y);
    int scn_gz_ = 0, scn_gz_pt_ = 0, scn_gz_rot_ = 0;
    std::string scn_gz_key_;
    bool scn_gz_moved_ = false, scn_scroll_ = false;
    float scn_gz_mouse_[2] = {0, 0}, scn_gz_start_[3] = {0, 0, 0}, scn_gz_off_[2] = {0, 0}, scn_gz_ang_ = 0;
    int scn_gz_quad_[8] = {};
    // --scn-drag-probe dx,dy (checks): the mouse takes the handle of the picked command (a zone's middle or the point) on
    // frame 40, presses, drags it dx,dy pixels and lets go; report <shot>.scn.txt (bytes before / after, the script line)
public:
    void input_probe();
private:
    bool scn_probe_ = false;
    float scn_probe_d_[2] = {0, 0}, scn_probe_t_[2] = {-1, -1}, scn_probe_t0_[2] = {-1, -1};
    std::string scn_probe_before_, scn_probe_key_;   // --scn-probe-key @XXXX: aim at the marker of that command (not picked)
    // правка комнаты (ui_edit.cpp)
    fs::path room_mods_dir(const DiscFile& f) const;
    void ensure_edit(Loaded& L);
    void save_edit(Loaded& L, int what);     // 0 тексты, 1 враги, 2 предметы, 3 скрипт
    void revert_edit(Loaded& L, int what);
    void edit_bar(Loaded& L, int what, bool dirty);
    void placement_panel(Loaded& L, bool items);
    // 06.10.2026: правка расстановки мышью во вьюпорте (ui_edit.cpp); true — мышь занята правкой (не вращать вид)
    bool viewport_edit(bool hovered, float img_x, float img_y);
    void placement_refresh(Loaded& L, int what);     // marks in the view: 0 enemies, 1 items, 2 pieces (the picked one white)
    void pieces_panel(Loaded& L);                       // placed pieces of the room: list, place, turn, save
    std::vector<MTri> room_view_tris(Loaded& L);        // the room as the game shows it (pieces at their places)
    // 06.10.2026 (player: "what I add is not seen in the view"): enemies (rest pose) and the common item box in the room view
    const Vram& room_vram(Loaded& L);                   // the room's video memory + CORE's (the game has both)
    const FileContent* side_content(const std::string& name);   // CORE.DAT / a room of a kind from elsewhere (not the LRU cache)
    std::string view_fk_sig_;                           // 06.10.2026: kinds of other rooms whose pictures the view holds now
    std::map<std::string, std::unique_ptr<FileContent>> side_c_;
    int edit_sel_ = -1, edit_drag_ = -1;               // выбранная / перетаскиваемая запись (индекс в массиве правки)
    int edit_items_ = 0;                               // what the pick is among: 0 enemies, 1 items, 2 pieces
    const DiscFile* edit_file_ = nullptr;
    float drag_off_[2] = {0, 0}, add_at_[3] = {0, 0, 0};
    bool edit_scroll_ = false;                         // панель: раскрыть и показать выбранную
    // манипулятор выбранной (06.10.2026, игрок: «где оси координат у трансформа предмета — для расстановки в вьюпорте»):
    // 1..3 — ось X / вверх / Z, 4 — по полу (квадрат в центре или сама метка), 5 — поворот (кольцо, враги)
    int gizmo_ = 0;
    float gz_mouse_[2] = {0, 0}, gz_start_[3] = {0, 0, 0}, gz_start_ang_ = 0;
    int gz_start_rot_ = 0;
    void view_file(const FileContent& c);
    void view_table(const FileContent& c);
    json table_;   // таблица для K_EXE_* и K_ROSTER
    void win_errors();
    // 10.10.2026: mod packages (ui_packs.cpp) - on, off, order, the one the studio works in, new, zip in and out
    PackSet packs_;
    bool show_packs_ = false;
    std::string packs_msg_, ask_replace_zip_, ask_replace_dir_;
    char new_pack_name_[128] = {0};
    void win_packs();
    void packs_rescan();
    void packs_order_changed();
    fs::path work_root() const;            // where Convert all and the room editors write: mods or the package
    std::string work_label() const;        // "mods" / "mods: <package>"
    // 10.10.2026: Test in game (ui_test.cpp) - the game from the newest quicksave, through a door into the picked room
    std::map<std::string, DoorIn> door_in_;   // room stem -> a door that leads into it (all rooms, built once in a thread)
    std::mutex door_in_mu_;
    std::atomic<bool> door_in_ready_{false};
    std::unique_ptr<std::thread> door_in_job_;
    void door_in_start();
    const DiscFile* test_room() const;     // the picked room (nullptr - not a room)
    void test_in_game();
    bool do_test_ = false;                 // --test-in-game (checks)
    void status_bar();
    bool choose_base();
    bool choose_mods();
    void start_export(const DiscFile* only);
    void start_export_item(char kind, int i);   // 'c' — персонаж (все модели), 'a' — модель каталога, 'm' — меш каталога
    void start_convert();
    fs::path source_root() const { return u8path(set_.base) / "source"; }
    fs::path mods_root() const { return set_.mods.empty() ? u8path(set_.base) / "mods" : u8path(set_.mods); }
};

// диалоги Windows
std::string pick_image_dialog(SDL_Window* w);
std::string pick_folder_dialog(SDL_Window* w, const char* title, const std::string& start);
void open_in_explorer(const fs::path& p);

}  // namespace dc
