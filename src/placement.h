// Расстановка и параметры: враги, предметы, двери, зоны (скрипт комнаты), зоны звука шагов (hdr[6]), ростер
// динозавров этапа (оверлей /PSX/BIN/STn.BIN), таблицы EXE (сцены дверей, предметы, тексты). Разбор 05.10.2026
// (скретчпад re/placement.md). Все правки — на месте: длины записей не меняются.
#pragma once
#include <set>

#include "content.h"
#include "gltf.h"

namespace dc {

struct ScriptOpAt { int sub = 0; uint32_t pc = 0; int op = 0, size = 0; };
// обход скрипта по потоку управления от входов подпрограмм (тексты внутри скрипта не задеваются)
std::vector<ScriptOpAt> script_walk(const Bytes& blk, uint32_t script_off, uint32_t script_size, const std::vector<uint32_t>& dir);

// JSON комнаты: enemies, items, doors, zones, footsteps; exe — для имён предметов (может быть пустым)
json room_enemies_json(const FileContent& c);
json room_items_json(const FileContent& c);
json room_doors_json(const FileContent& c);
json room_zones_json(const FileContent& c);
json room_footsteps_json(const FileContent& c);
// правки полей в блоке комнаты (тот же размер) — у записей с "offset"; добавленные ("copy_of") и убранные ("removed": true)
// — room_placement_structure; false — ошибка в err
bool apply_room_placement(const json& j, const char* what, Bytes& blk, const RoomInfo& r, std::string& err);

// что занято во всей игре: флаги «взят» предметов (банк 7) и биты банка 7, которые трогают скрипты; 06.10.2026 - номера
// флагов смерти врагов: команда появления 0x20 не ставит врага, если поднят бит банка 4 с номером из байта +4 записи, ИИ
// поднимает его при смерти (0x8003D824) и держит тем же номером состояние в банке 5 - занятым считается номер любого из двух
std::set<int> used_enemy_flags(Disc& d);
struct PlacementCtx {
    bool ready = false;
    std::set<int> used7;
    int bits7 = 512;
    std::set<int> used45;                   // 06.10.2026: death flags of enemies (banks 4 and 5, one number for both)
    void ensure(Disc& d);
};
struct ScriptAtom;
// враги и предметы: убрать (removed) и добавить копию (copy_of: смещение записи-образца; поля — как у неё, свои — из
// записи). Свободные слот актёра (0..9), номер зоны (0..27), флаг «взят» (банк 7), флаг смерти врага (банки 4/5) и секция
// модели предмета (0..31) берутся сами, если не заданы. 06.10.2026 (игрок: «добавленный враг появляется один раз, убил - больше
// нет; как предметы»): новые враги (0x20) и предметы встают в начало кода входа в комнату (подпрограмма 0 до первой проверки
// флага или перехода) - их не прячут ветки сюжета; копия врага с флагом 0 (появлялся бы всегда) получает свой флаг.
bool room_placement_structure(const json& j, const char* what, std::vector<ScriptAtom>& atoms, PlacementCtx& ctx,
                              std::vector<std::string>& notes, std::string& err);
// 06.10.2026: an item of the common box (CORE, group 80180000) with id < 11 and no turn of its own - the game would lay it on
// its side; the build gives it a turn 0, 0, 0
bool item_core_upright(const json& e);
// 06.10.2026: actor kinds with behaviour code in stage n (1..9): the ACTOR op starts a kind by the stage overlay's table
// (STn.BIN at 0x800E8000; a kind with a null entry gets a model but no behaviour). Bit k - kind k has code.
uint32_t stage_kinds(const Bytes& ovl);
// 06.10.2026: the kind table itself - its offset in the overlay (RAM 0x800E8000 + off) and its 32 entries (0 - no kind)
bool stage_kind_table(const Bytes& ovl, uint32_t& off, uint32_t ent[32]);
// banks of other stages' overlays in the memory for mods (runtime memory.c memory_fk_code): 8 x 128 KB from here
static const uint32_t kForeignBankBase = 0x82E00000u, kForeignBankSize = 0x20000u, kForeignBanks = 8;
static const uint32_t kOverlayBase = 0x800E8000u;

// ростер этапа n (1..9) в файле оверлея: смещения записей (10 × 24 Б) — один или два варианта
std::vector<uint32_t> roster_offsets(const Bytes& ovl, int stage);
json roster_json(const Bytes& ovl, int stage);
bool apply_roster(const json& j, Bytes& ovl, int stage, std::string& err);

// EXE: сцены дверей, предметы, тексты
json exe_door_scenes_json();
json exe_items_json();
json exe_texts_json();
bool apply_exe(const json& j, const char* what, Bytes& exe_file, std::string& err);

// имена
std::string item_name(int id);
std::string enemy_kind_name(int kind);
std::string room_code(int v);   // 0x10A → "ST10A"

}  // namespace dc
