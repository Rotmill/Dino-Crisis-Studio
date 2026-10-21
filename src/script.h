// Скрипт комнаты: длины опкодов и листинг для просмотра (известные — по имени, прочие — байтами).
#pragma once
#include "core.h"

namespace dc {

struct ExeImage {
    Bytes data;           // файл PS-X EXE с диска игрока
    uint32_t addr = 0;    // адрес загрузки
    bool ok() const { return !data.empty(); }
    bool has(uint32_t a, uint32_t n) const { return a >= addr && (size_t)(a - addr) + 0x800 + n <= data.size(); }
    uint32_t u32(uint32_t a) const { return has(a, 4) ? le32(&data[0x800 + (a - addr)]) : 0; }
    const uint8_t* ptr(uint32_t a) const { return has(a, 1) ? &data[0x800 + (a - addr)] : nullptr; }
};
// EXE из образа (SYSTEM.CNF → SLUS_009.22); общий для приложения
bool load_exe(Disc& d);
const ExeImage& exe();

int script_op_size(const uint8_t* pc, size_t avail);   // 0 — неизвестно/конец
struct ScriptOp { uint32_t off = 0; int op = 0, size = 0; std::string text; };
// подпрограмма [a, b) от начала секции скрипта (script_off — смещение секции в блоке)
std::vector<ScriptOp> script_listing(const Bytes& blk, uint32_t script_off, uint32_t a, uint32_t b);

}  // namespace dc
