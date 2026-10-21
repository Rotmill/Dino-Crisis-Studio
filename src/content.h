// Разбор файла диска на содержимое: меши, скелетные модели, движения, звуковые банки, секвенции, картинки, XA.
#pragma once
#include "core.h"

namespace dc {

struct Material { uint16_t tpage = 0, clut = 0; bool semi = false; };
struct TexCrop {
    uint16_t tpage = 0, clut = 0;
    int u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    std::vector<uint32_t> rgba;
    std::string file_name() const;   // как export_textures.py: tp0085_cx768_cy506_u0-255_v0-127.png
    std::string hash;                // sha1(w,h,rgba)
    int w() const { return u1 - u0 + 1; }
    int h() const { return v1 - v0 + 1; }
};

// kind — что это (для имён в приложении и в выгрузке): room, door, shotgun, handgun, grenade_gun, in_hand, box, arrow, mesh
struct MeshPart {
    std::string name;               // имя файла в выгрузке (без расширения)
    std::string kind;
    std::vector<MTri> tris;         // MTri::group — индекс в hdrs
    int ngroups = 0;
    uint32_t block_dest = 0;
    int block = -1;                 // индекс в FileContent::blocks
    std::vector<uint32_t> hdrs;     // заголовки групп в блоке (по ним — ключ группы, как у игры)
    bool lit = false;               // записи с нормалями (вложение кости: оружие в руке)
};

// kind: regina, human, raptor, trex, trex_head, pteranodon, compsognathus, dino, prop, model
struct ActorPart {
    std::string name;               // имя файла в выгрузке (без расширения): raptor, raptor_2, regina…
    std::string kind;
    Model model;
    uint32_t block_dest = 0;
    int block = -1;                 // индекс в blocks
    std::vector<int> motion_sets;   // индексы в motion_sets с тем же числом костей
};
std::string actor_kind(int nbones, bool regina_file);

struct MotionSet {
    std::string name;
    int block = -1;                 // индекс в blocks (данные кадров)
    uint32_t block_dest = 0;
    std::vector<Motion> motions;
    int nbones = 0;
    const Bytes* ext = nullptr;     // если движения из другого файла (наборы оружия) — данные здесь
};

struct SoundBank { std::string name; Vab vab; int hdr_entry = -1, body_entry = -1; };
struct SeqData { std::string name; Bytes data; uint32_t param = 0; int entry = -1; };
struct ImagePart { std::string name, label; int w = 0, h = 0; std::vector<uint32_t> rgba; int id = -1; };

// картинки предметов ITEM.DAT / ITEM2.DAT (разбор 05.10.2026, re/items.md): 119 записей по 5 секторов (id = номер записи);
// сектора 0–3 — 8 бит 128×64 плитками 32×32 слова (0,0)(1,0)(0,1)(1,1), сектор 4 — палитра 256 × RGB555; видно 96×64
constexpr int kItemCount = 119, kItemW = 96, kItemH = 64;
void item_decode(const uint8_t* rec, std::vector<uint32_t>& rgba);   // rec — 5 секторов записи
void item_encode(const uint32_t* rgba, uint8_t* rec);                 // в те же 5 секторов (палитра по картинке)

struct FileContent {
    const DiscFile* file = nullptr;
    Bytes raw;
    std::vector<DatEntry> ents;
    std::vector<RamBlock> blocks;
    Vram vram;
    std::vector<Upload> uploads;
    std::vector<MeshPart> meshes;
    std::vector<ActorPart> actors;
    std::vector<MotionSet> motion_sets;
    std::vector<SoundBank> banks;
    std::vector<SeqData> seqs;
    std::vector<ImagePart> images;
    bool is_room = false;
    RoomInfo room;
    int room_block = -1;
    std::vector<XaChannel> xa;
    bool container = false;
    std::string summary;
};

class Library;   // общий кэш (CORE.DAT для костюмов и оружия)

bool analyze(Disc& disc, const DiscFile& f, FileContent& c);
// то же для байтов файла не с диска (например, собранного в mods); f — описание файла диска с тем же именем
bool analyze_raw(Disc& disc, const DiscFile& f, Bytes raw, FileContent& c);

// текстуры: материалы → вырезки по UV (правило export_textures.py)
std::vector<TexCrop> crops_for_mesh(const Vram& v, const std::vector<MTri>& tris);
std::vector<TexCrop> crops_for_model(const Vram& v, const Model& m);
std::string material_key(uint16_t tpage, uint16_t clut);

}  // namespace dc
