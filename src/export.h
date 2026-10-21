// Экспорт в папку source: та же иерархия, что у mods; исходный файл диска как есть + содержимое в обычных форматах.
#pragma once
#include <atomic>
#include <set>

#include "content.h"
#include "library.h"
#include "model_out.h"

namespace dc {

struct ExportOptions {
    bool native = true;     // файлы диска как есть (то, что кладётся в mods под своим именем)
    bool models = true;     // меши и модели: glTF (+ .bin) и OBJ
    // 08.10.2026 (player: "glTF, FBX and OBJ to choose from"): the format of meshes and models; glTF also keeps the OBJ in
    // the game's own coordinates next to a mesh, FBX and OBJ write only themselves (model_out.h)
    ModelFormat model_format = ModelFormat::Gltf;
    bool textures = true;   // PNG вырезки по материалам, без повторов
    bool sounds = true;     // WAV сэмплов, секвенции
    bool data = true;       // коллизия, камеры, триггеры, тексты, скрипты (JSON)
    bool streams = true;    // речь XA и звук роликов
    // 06.10.2026 (player: "what I pick - only it goes out"): one part of a room file - mesh, collision, cameras, footsteps,
    // enemies, items, doors, zones, texts, script; empty - the whole file (with its game file as is)
    std::string room_part;
};

struct ExportLog {
    std::mutex mu;
    std::vector<std::string> lines;
    std::atomic<int> done{0}, total{0}, errors{0};
    std::atomic<bool> cancel{false}, running{false};
    std::string current;
    void add(const std::string& s) {
        std::lock_guard<std::mutex> lk(mu);
        lines.push_back(s);
    }
};

void create_source_tree(const fs::path& source_root);
bool write_png(const fs::path& p, int w, int h, const uint32_t* rgba);

class Exporter {
public:
    Exporter(Disc& d, const fs::path& source_root, const ExportOptions& o, ExportLog* log, bool make_tree = true);
    ~Exporter();
    // 06.10.2026: model `part` of file content c with its own pictures (from c's video memory), no motions, into dir\name.gltf -
    // an enemy kind put into a room where its pictures are not: Dino Crisis ME draws it from there in that room only
    // (mods\rooms\<ROOM>\kinds\index.txt)
    bool write_model(const FileContent& c, int part, const fs::path& dir, const std::string& name);
    bool export_file(const DiscFile& f);
    void export_files(const std::vector<const DiscFile*>& files);
    // каталог без повторов: персонаж (characters\<id>), меш (weapons|items|doors\<id>), звук (уникальные сэмплы и мелодии)
    // 08.10.2026: only - this list of motions (a pack picked in the window, one motion; nullptr - as before: the character's
    // motions with its main model, none with the others); suffix - after the model's name in the file names (regina_core_move)
    bool export_actor(const Library& lib, int i, const std::vector<int>* only = nullptr, const std::string& suffix = std::string());
    // 06.10.2026: the model of a catalog character without motions into dir\name.gltf (an enemy kind put into another room
    // is drawn by it: the game's mesh path takes any model of the character's folder)
    // 06.10.2026: движения сцен комнаты (Регина и люди вместе) — rooms\<КОМНАТА>\scene\<КОМНАТА>_scene.gltf, на Регине
    bool export_scene_motions(const Library& lib, const DiscFile* room);
    bool export_mesh(const Library& lib, int i);
    // звук каталога; samples / songs - индексы Library::samples / songs (nullptr - все)
    void export_sounds(const Library& lib, const std::set<int>* samples = nullptr, const std::set<int>* songs = nullptr);
    void export_library(const Library& lib);   // всё сразу (вместе со звуком)
    // 06.10.2026: one picture of a model, mesh or room into source\<rel_dir> under its crop name (the name the game knows it by)
    bool export_texture(const fs::path& rel_dir, const TexCrop& c);

private:
    Disc& d_;
    fs::path src_;
    ExportOptions o_;
    ExportLog* log_;
    // выгрузка персонажа каталога: (набор в FileContent::motion_sets, номер) → имя анимации; нет в списке — повтор, не пишется
    const std::map<std::pair<int, int>, std::string>* anim_names_ = nullptr;
    fs::path texture_file(const fs::path& dir, const TexCrop& c);   // картинка рядом с моделью (имя вырезки)
    bool save_model(Gltf& g, const fs::path& dir, const std::string& name);   // dir\name in the chosen format
    void write_mesh(const fs::path& dir, const std::string& name, const MeshPart& m, const Vram& v, const std::string& owner);
    void write_actor(const fs::path& dir, const std::string& name, const FileContent& c, const ActorPart& a,
                     const std::string& owner);
    void write_room_data(const fs::path& dir, const FileContent& c);
    void write_xa(const DiscFile& f, const FileContent& c, const fs::path& dir);
    void write_movie(const DiscFile& f, const FileContent& c, const fs::path& dir);
    void write_images(const FileContent& c, const fs::path& dir);
    void note(const std::string& s);
};

// куда идут звуковые эффекты файла (по образцу PC: Sound\SE\ROOM, DOOR, WEAPON, COMMON)
std::string se_group(const DiscFile& f);
// где лежит WAV сэмпла (игра ищет его в папке первого файла, номер — как в банке): sound\se\<группа>\<ФАЙЛ>\NNN.wav,
// инструмент музыки — sound\bgm\<ФАЙЛ>\instruments\NNN.wav
fs::path sample_wav_path(const fs::path& root, const LibSample& s);
// данные комнаты в JSON (collision, cameras, triggers, texts, script) — то же, что пишет выгрузка; only - одна часть
// (collision, cameras, footsteps, enemies, items, doors, zones, texts, script), пусто - все
void write_room_json(const fs::path& dir, const FileContent& c, const std::string& only = std::string());

}  // namespace dc
