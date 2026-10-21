// Каталог всего диска без повторов: модели персонажей, меши (оружие, оружие в руке, предметы, переходы, общие куски комнат),
// наборы движений. Повтор в данных игры (тот же раптор в тридцати комнатах, то же оружие в каждом файле костюма) — одна
// запись: ключ по содержимому — тот же, по которому игра (dc_meshrep.cpp) узнаёт модель и подменяет её мешем из mods везде.
// Строится один раз после открытия образа (свой поток, ~2 с).
#pragma once
#include <atomic>
#include <map>
#include <tuple>

#include "content.h"

namespace dc {

// ключи — как в игре (dc_meshrep.cpp): блок, его адрес в RAM, смещение заголовка в блоке
uint64_t actor_key_blk(const Bytes& blk, uint32_t base, uint32_t hdr);
uint64_t group_key_blk(const Bytes& blk, uint32_t base, uint32_t hdr, bool lit);
// группы со светом (вложения костей: оружие в руке): {треугольники = свой адрес+12, квады = треугольники+nt·48, nt, nq}, 48/64 Б
std::vector<MeshGroup> scan_lit_groups(const Bytes& blk, uint32_t base);
void collect_lit_tris(const Bytes& blk, const MeshGroup& g, std::vector<MTri>& out);

// текстура вещи каталога: хеш картинки (вырезка страница+палитра+окно UV) и где её взять (файл, часть: модель / 10000 + меш)
struct LibTex { std::string hash; const DiscFile* f = nullptr; int part = -1; };

struct LibPlace {
    const DiscFile* f = nullptr;
    int block = -1;          // индекс в FileContent::blocks
    uint32_t dest = 0;       // адрес блока в RAM
    uint32_t hdr = 0;        // смещение заголовка в блоке
    int part = -1;           // индекс в FileContent::actors / meshes
};

struct LibSet {              // набор движений (без повторов по содержимому)
    std::string id, label;   // id — имя файла, где он впервые («CORE», «P10», «ST105»)
    LibPlace at;
    std::vector<LibPlace> also;
    int nbones = 0, count = 0;
    uint64_t hash = 0;
};

// движение без повторов: то же движение (ключи и кадры) в наборах разных файлов — одна запись; анимация в glTF персонажа —
// по первому месту (<набор>_<NN>), правка расходится во все места
struct LibMotionPlace {
    const DiscFile* f = nullptr;
    int set = -1;                  // индекс в FileContent::motion_sets этого файла
    int motion = -1;               // номер движения в наборе
};
struct LibMotion {
    std::string name;              // <набор каталога>_<NN> первого места
    int set = -1;                  // набор каталога первого места (Library::sets)
    int nbones = 0;
    uint64_t hash = 0;
    LibMotionPlace at;
    std::vector<LibMotionPlace> also;
};

struct LibActor {
    std::string id, kind, label;   // id — папка в source/mods: regina, regina_costume1, raptor, gail, gail_wounded…
    uint64_t key = 0;
    LibPlace at;                   // первый файл: вид и текстуры — из его видеопамяти
    std::vector<LibPlace> also;
    int nbones = 0, nverts = 0, nprims = 0;
    std::vector<int> sets;         // наборы движений файлов модели (Library::sets) — для разбора
    std::vector<int> motions;      // = движения персонажа у основной модели (Library::chars), у вариантов — пусто
    int chr = -1;                  // персонаж (Library::chars)
    std::string var_en, var_ru;    // вариант у персонажа: «costume 1», «unarmed», «wounded»; пусто — основная модель
    std::vector<LibTex> tex;       // текстуры модели во всех местах (другая раскраска в другой комнате — своя), без повторов
};

// 06.10.2026 (игрок: «у Регины в каждом костюме повторил все анимации… дубликатов не нужно: одна вещь — одно место, логично
// размещено»): персонаж — модели (костюмы, варианты) и его движения; каждое движение каталога — ровно у одного персонажа или
// у одной комнаты (движения сцен, где играют Регина и люди вместе).
struct LibChar {
    std::string id, kind, en, ru;  // id — папка основной модели
    std::vector<int> actors;       // модели (Library::actors), первая — основная
    std::vector<int> motions;      // свои движения (Library::motions), по порядку наборов
};

struct LibMesh {                   // неподвижный меш
    std::string id, kind, label, cat;   // cat — раздел: weapons, rooms (общие куски комнат), doors, items
    std::vector<uint64_t> keys;    // ключи групп меша
    bool lit = false;              // записи с нормалями (вложение кости)
    LibPlace at;
    std::vector<LibPlace> also;
    std::vector<uint32_t> groups;  // заголовки групп в блоке at
    int ntris = 0;
    std::vector<LibTex> tex;       // текстуры во всех местах, без повторов
};

struct LibSample {                 // сэмпл звука (эффект или инструмент музыки) без повторов: ключ — байты ADPCM, как у игры
    LibPlace at;                   // первый файл: туда же кладётся WAV (sound\se\<группа>\<ФАЙЛ>\NNN.wav), игра узнаёт его везде
    int bank = 0, index = 0, nbanks = 1;
    uint64_t hash = 0;
    bool bgm = false;
    double secs = 0;
    std::vector<LibPlace> also;
};

struct LibSong {                   // мелодия без повторов (ключ — как у игры, song_key)
    LibPlace at;
    int index = 0;
    uint64_t key = 0;
    std::vector<LibPlace> also;
};

struct LibRoom {                   // тело комнаты: ключи групп (узлы glTF group_NN), как у игры
    const DiscFile* f = nullptr;
    std::vector<uint64_t> keys;
    std::vector<LibTex> tex;       // текстуры тела
    const DiscFile* twin = nullptr;   // комната с тем же телом раньше по списку (тело — там)
};

// 06.10.2026: an enemy kind as a room carries it (the room's spawn record: model and motion table addresses there) - every
// kind of the game once (model key + kind + variant), from the room whose motion table is the longest
struct LibSpecies {
    const DiscFile* f = nullptr;
    int kind = 0, variant = 0, ai = -1, hp = -1, nmot = 0;
    uint32_t model = 0, motions = 0;
    uint64_t key = 0;
    std::string kind_name, actor;   // actor - the model's id in the catalog (raptor, pteranodon...)
    // 06.10.2026 (player: "two kinds of raptors, they even look different - that is all the kinds"): the look - the texels
    // the model's polygons take in its room (hash) and the colour they give; one kind can wear several (raptor: green, grey)
    uint64_t look = 0;
    std::string color;
};
// the look of a model in a room's video memory (hash of the texels its polygons take, the colour they give)
void model_look(const Vram& v, const Model& m, uint64_t& look, std::string& color);

class Library {
public:
    void build(Disc& d, std::atomic<int>* done = nullptr, std::atomic<int>* total = nullptr);
    std::vector<LibActor> actors;
    std::vector<LibMesh> meshes;
    std::vector<LibSet> sets;
    std::vector<LibMotion> motions;
    std::vector<LibSample> samples;
    std::vector<LibSong> songs;
    std::vector<LibRoom> rooms;
    std::vector<LibChar> chars;    // персонажи по порядку дерева
    std::vector<LibSpecies> species;   // 06.10.2026: enemy kinds of the game (the enemy drop-down)
    // движения сцен комнаты (люди и Регина в одной сцене; кто кого играет — решает скрипт), каждое — у одной комнаты
    std::map<const DiscFile*, std::vector<int>> scene_motions;
    // текстура (хеш) → чья она в дереве: «a<модель>», «m<меш>», «r<ИМЯ комнаты>» — первое место по порядку дерева
    std::map<std::string, std::string> tex_owner;
    std::atomic<bool> ready{false};
    const LibActor* actor(const std::string& id) const;
    const LibMesh* mesh(const std::string& id) const;
    const LibRoom* room(const DiscFile* f) const;
    // меши разделов по порядку дерева: оружие по видам (дробовик, пистолет, гранатомёт, дротик), в руке — после моделей
    std::vector<int> meshes_of(const std::string& cat) const;
    int motion(const std::string& name) const;                    // по имени анимации, −1 — нет
    int motion_at(const DiscFile* f, int set, int motion) const;  // движение каталога по месту, −1 — нет
    // mods\names.txt: каждый меш игры — ключ и путь, под которым его glTF в mods игра берёт сама (без конвертации)
    std::string names_text() const;
private:
    std::map<std::string, int> motion_by_name_;
    std::map<std::tuple<const DiscFile*, int, int>, int> motion_by_place_;
    void build_chars();
    void own_motions();
    void own_textures();
    // за разбор: модели файла (ключ, кости) и таблицы движений в командах появления (кто, места движений)
    std::map<const DiscFile*, std::vector<std::pair<uint64_t, int>>> file_actors_;
    struct Spawn { const DiscFile* f; uint64_t key; std::vector<std::pair<int, int>> mot; };
    std::vector<Spawn> spawns_;
};

}  // namespace dc
