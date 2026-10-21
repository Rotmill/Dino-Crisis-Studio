// Скрипт комнаты как правимый список: команды, строки текста и прочие данные секции («атомы»). Правки — заменить,
// убрать, вставить команду, текст любой длины — и сборка секции заново: всё, что ссылается на места внутри скрипта,
// пересчитывается (разбор 06.10.2026 по коду обработчиков ВМ и данным 120 комнат):
//   каталог подпрограмм (u32 смещения в начале секции);
//   переходы — s16 по +2 от PC: 0x09 FOR (выход), 0x0B метка, 0x0C JMP, 0x0E BRANCH, 0x17 цикл; 0x0A NEXT — назад (pc − s16);
//   смещения от начала скрипта (u32): 0x21 TEXT +4, зона 0x28 «сообщение» (тип 1) +28, «осмотр» (тип 7) +40, 0x67 +4 (таблица).
// Других ссылок внутрь секции нет: номера подпрограмм (CALL, 0x05, зоны тип 3, ракурсы) — индексы каталога; адреса вне
// скрипта (модели, движения, меши) не двигаются. Все сдвиги кратны 4 (строки добиваются пробелом) — выравнивание команд
// прежнее. Секция встаёт на место, если не длиннее; иначе — в конец блока комнаты (указатель hdr[5]).
#pragma once
#include "core.h"

namespace dc {

struct ScriptSym {           // поле, которое после сборки переписывается по новому месту цели
    int at = 0;              // смещение поля в байтах атома
    uint8_t type = 0;        // 0 — s16 переход вперёд (цель − pc), 1 — s16 назад (pc − цель, 0x0A), 2 — u32 смещение от начала скрипта
    uint32_t target = 0;     // цель — СТАРОЕ смещение от начала скрипта
    int tnew = -1;           // 10.10.2026: or a NEW atom (its nid; script.txt ->#N / $#N), then target is not used
};

struct ScriptAtom {
    enum Kind : uint8_t { DATA, OP, STRING, DIR };
    Kind kind = DATA;
    int64_t old = -1;        // старое смещение от начала скрипта (−1 — новая команда)
    uint32_t old_len = 0;
    Bytes b;                 // содержимое (у команды — с прежними значениями полей-ссылок, их перепишет сборка)
    std::vector<ScriptSym> syms;
    int sub = -1;            // подпрограмма (команды; первая, которая до неё доходит)
    uint32_t text_from = 0;  // строка: где начинаются коды текста (до — управляющие: цвет, окно)
    int nid = -1;            // 10.10.2026: a new atom's own number (script.txt label #N): jumps and the directory can point at it
    // 14.10.2026: an enemy (20) the scenario adds: "ROOM MODEL MOTIONS" of a fighter of its kind - the build brings its whole
    // set of motions in (a scene's raptor has 3, the hunting brain asks for ~65: the model would twist and fly).
    // script.txt: the line "from ROOM MODEL MOTIONS" right after the enemy's line
    std::string from;
};

struct ScriptImage {
    uint32_t off = 0, size = 0;          // секция в блоке
    std::vector<uint32_t> dir;           // каталог: старые смещения подпрограмм
    std::vector<ScriptAtom> atoms;       // по старому порядку
    int find(uint32_t old) const;        // атом, начинающийся на old (−1 — нет)
    int containing(uint32_t old) const;  // атом, содержащий old
};

// разложить секцию скрипта блока комнаты на атомы
bool script_decompose(const Bytes& blk, const RoomInfo& r, ScriptImage& im, std::string& err);
// поля-ссылки команды (по опкоду и типу зоны); pc — старое место (для переходов), −1 — новая команда (её переходы — по
// уже записанным в atom.syms целям)
void script_op_syms(ScriptAtom& a);
// собрать секцию из атомов (новый порядок — порядок в atoms; атомы прежнего образа, которых нет, — удалены: ссылки на них
// уходят на следующий уцелевший); orig — исходный образ (для карты старых смещений)
bool script_assemble(const ScriptImage& orig, const std::vector<ScriptAtom>& atoms, Bytes& sec, std::string& err);
// поставить секцию в блок: на место (если влезает) или в конец блока с переводом hdr[5]; limit — предел блока
// a section moved to the end of the room block: zeros at its old place unless something still points into it
void section_old_place_clear(Bytes& blk, uint32_t base, uint32_t off, uint32_t size, std::vector<std::string>& notes, const char* what);
void section_old_pieces_clear(Bytes& blk, uint32_t base, uint32_t off, const std::vector<std::pair<uint32_t, uint32_t>>& pieces,
                              std::vector<std::string>& notes, const char* what);
// im: the old script's pieces - when the section moves, its commands, texts and directory are cleared at the old place
bool script_install(Bytes& blk, uint32_t base, const RoomInfo& r, const Bytes& sec, uint32_t limit, std::vector<std::string>& notes,
                    std::string& err, const ScriptImage* im = nullptr);

// текст скрипта для правки (script.txt): строка = команда в hex; @XXXX — её место в игре; переход ->@XXXX, ссылка $@XXXX
// 10.10.2026: a new command may carry a label #N (jumps ->#N, references $#N point at it); `sub NN` past the game's
// directory = a new subroutine starting at the next line; `text #N "..."` = a new text (escapes as in texts.json)
std::string script_to_text(const ScriptImage& im, const std::string& room);
// the same for an edited atom list (new commands, texts and subroutines included)
std::string script_atoms_text(const ScriptImage& orig, const std::vector<ScriptAtom>& atoms, const std::string& room);
// обратно: атомы из текста (строки с @ — прежние команды/данные, без @ — новые; пропавшие — удалены)
bool script_from_text(const ScriptImage& orig, const std::string& text, std::vector<ScriptAtom>& out, std::string& err);
// 10.10.2026: the directory of an atom list - subroutine k -> atom index (-1: its command was removed)
std::vector<int> script_dir_atoms(const ScriptImage& orig, const std::vector<ScriptAtom>& atoms);
// a fresh number for a new atom (max nid + 1)
int script_next_nid(const std::vector<ScriptAtom>& atoms);

// строка текста: новые коды (без управляющих в начале — они прежние; с 0xA000 в конце) → атом строки
bool script_set_text(ScriptAtom& a, const std::vector<uint16_t>& codes, std::string& err);
// 14.10.2026: a text reference as a short quote ("..." or "(text)"): read from the bytes up to the end mark, across atoms
// (a text of the game may start in data and go on in a text atom), control codes left out; for showing only
std::string script_text_quote(const std::vector<ScriptAtom>& atoms, const ScriptSym& sy, size_t max_len);

}  // namespace dc
