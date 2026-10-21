// «Сконвертировать всё»: папка mods (как её выгружает приложение) → то, что берёт игра.
//   Модели (mods\characters\<имя>, weapons\, items\, doors\, тела комнат rooms\<ИМЯ>\<ИМЯ>.gltf): изменённый меш — строкой
//   в mods\<папка>\index.txt (ключ по содержимому: игра подменяет модель везде, где она есть, dc_meshrep.cpp); анимации
//   <набор>_<NN> glTF персонажа — в движения набора во ВСЕХ файлах, где он лежит (родные файлы mods\<папка>\<ИМЯ>.DAT).
//   Комнаты — коллизия, камеры, шаги, тексты, скрипт, расстановка; экраны 320x240 — картинка; предметы — картинки ITEM;
//   программа и оверлеи этапов — таблицы. Списком для игры: текстуры PNG любого размера (mods\textures\index.txt; картинки
//   лежат рядом с моделями), экраны PNG крупнее 320x240 (mods\screens\index.txt). Звуки WAV и ролики MP4 игра берёт сама.
#pragma once
#include <memory>

#include "export.h"
#include "gltf_read.h"
#include "placement.h"

namespace dc {

struct ConvertReport {
    std::vector<std::string> lines;
    int built = 0, unchanged = 0, errors = 0;
    std::vector<std::string> screen_index;   // строки mods\screens\index.txt (экраны PNG не 320x240 — картинка вместо залитой)
};

// анимация из glTF персонажа — в одно место движения каталога (набор и номер в файле игры)
struct MotionJob {
    std::shared_ptr<GDoc> doc;
    std::string doc_name;                // путь от mods (для отчёта)
    std::shared_ptr<ActorPart> actor;    // модель каталога (имена костей)
    const GAnim* anim = nullptr;         // анимация в doc
    int set_index = -1;                  // набор в FileContent::motion_sets этого файла
    int motion = -1;                     // движение в наборе
};
using MotionJobs = std::map<const DiscFile*, std::vector<MotionJob>>;

// lib — готовый каталог (приложение); nullptr — построить (~2 с)
void convert_all(Disc& d, const fs::path& mods_root, ConvertReport& rep, ExportLog* log = nullptr, const Library* lib = nullptr);
// одна комната: patch_dir = mods\rooms\ST100 (пусто — только движения), out = mods\rooms\ST100.DAT
bool convert_room(Disc& d, const DiscFile& f, const fs::path& patch_dir, const fs::path& out, ConvertReport& rep,
                  const std::vector<MotionJob>* jobs = nullptr, PlacementCtx* pctx = nullptr);
// пересборка контейнера с новым блоком (plain — распакованный вид); каждый блок — в прежнем числе секторов (иначе загрузчик
// игры блок не принимает), файл — того же размера, что на диске
// grow_last (06.10.2026): the block is the file's last one and may outgrow its sectors - the file grows past its place on
// the disc (Dino Crisis ME reads such a room from mods by virtual sectors, dc_mods.c)
bool rebuild_container(const Bytes& orig, const std::vector<DatEntry>& ents, int entry, const Bytes& plain,
                       uint32_t limit, Bytes& out, std::string& err, bool grow_last = false);

}  // namespace dc
