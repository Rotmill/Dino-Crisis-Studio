// Подписи дерева: папки mods и что внутри файлов (английский / русский).
#pragma once
#include <string>
#include <vector>

#include "content.h"
#include "i18n.h"
#include "library.h"

namespace dc {

struct CatName {
    const char* key;      // папка mods (как DiscFile::cat)
    const char* en;
    const char* ru;
    const char* folder;   // как подписать папку справа (тускло)
    const char* parent;   // для sound\bgm, sound\se, sound\voice — "sound"
    bool group;           // «sound» — только родитель
};

static const CatName kCatNames[] = {
    {"rooms", "Rooms", "Комнаты", "rooms", nullptr, false},
    {"doors", "Doors", "Переходы", "doors", nullptr, false},
    {"characters", "Regina and weapons", "Регина и оружие", "characters", nullptr, false},
    {"items", "Items", "Предметы", "items", nullptr, false},
    {"screens", "Screens", "Экраны", "screens", nullptr, false},
    {"movie", "Movies", "Ролики", "movie", nullptr, false},
    {"sound", "Sound", "Звук", "sound", nullptr, true},
    {"sound\\bgm", "Music", "Музыка", "bgm", "sound", false},
    {"sound\\se", "Effects", "Эффекты", "se", "sound", false},
    {"sound\\voice", "Voice", "Речь", "voice", "sound", false},
    {"other", "Other", "Прочее", "other", nullptr, false},
};

inline std::string mesh_label(const std::string& k) {
    if (k == "room") return T("Room", "Комната");
    if (k == "door") return T("Model", "Модель");
    if (k == "shotgun") return T("Shotgun", "Дробовик");
    if (k == "handgun") return T("Handgun", "Пистолет");
    if (k == "grenade_gun") return T("Grenade gun", "Гранатомёт");
    if (k == "item_box") return T("Item box (on the floor)", "Ящик предмета (на полу)");
    if (k == "dart") return T("Dart", "Дротик");
    return T("Mesh", "Меш");
}

inline std::string actor_label(const std::string& k) {
    if (k == "regina") return T("Regina", "Регина");
    if (k == "human") return T("Human", "Человек");
    if (k == "raptor") return T("Raptor", "Раптор");
    if (k == "trex") return T("T-Rex", "Тираннозавр");
    if (k == "trex_head") return T("T-Rex head", "Голова тираннозавра");
    if (k == "pteranodon") return T("Pteranodon", "Птеранодон");
    if (k == "compsognathus" || k == "small_creature") return T("Compsognathus", "Компсогнат");
    // 06.10.2026: kind 8 of stage 6 - the game's own kill counter names it (PC DINO.exe: RAPTOR, COMPY, THERIZINO)
    if (k == "dino" || k == "therizinosaurus") return T("Therizinosaurus", "Теризинозавр");
    if (k == "prop") return T("Prop", "Предмет");
    return T("Model", "Модель");
}

inline std::string motion_set_label(const MotionSet& ms) {
    if (ms.name.size() == 3 && (ms.name[0] == 'P' || ms.name[0] == 'H')) return ms.name;   // набор оружия
    return T("Motions", "Движения");
}

inline std::string file_hint(const DiscFile& f) {
    if (f.cat == "rooms") {
        if (f.stem.size() >= 5) return strfmt("%c-%s", f.stem[2], f.stem.substr(3).c_str());
        return T("stage", "этап");
    }
    if (f.stem == "CORE") return T("base", "основа");
    if (f.stem.rfind("CORE0", 0) == 0) return strfmt(g_lang == LANG_RU ? "костюм %s" : "costume %s", f.stem.substr(5).c_str());
    if (f.cat == "characters" && (f.stem[0] == 'P' || f.stem[0] == 'H')) return T("weapon", "оружие");
    if (f.cat == "other" && upper(f.name) == "SLUS_009.22") return T("game program", "программа игры");
    if (f.cat == "other" && f.dir == "/PSX/BIN/") return strfmt(g_lang == LANG_RU ? "этап %c" : "stage %c", f.stem[2]);
    if (f.cat == "items") return f.stem == "ITEM2" ? T("Ancient costume", "костюм Ancient") : std::string();
    return std::string();
}

// накладка комнаты: mask 1 — коллизия, 2 — камеры, 4 — зоны звука шагов; пары точек (x,y,z, r,g,b) в координатах игры
void room_overlay(const RoomInfo& r, int mask, std::vector<float>& L);
// расстановка: what 0 — враги, 1 — предметы, 2 — двери, 3 — зоны (JSON из placement.cpp)
void placement_overlay(const json& arr, int what, std::vector<float>& L);
// 06.10.2026: то же по всему массиву правки (убранные пропускаются), sel — выбранная запись (ярче, белым), −1 — нет
void placement_overlay_sel(const json& arr, int what, int sel, std::vector<float>& L);
// место записи расстановки для мыши: враг — ноги (x, y, z); предмет — середина зоны на своём этаже. false — места нет (ростер)
bool placement_point(const json& e, bool items, float& x, float& y, float& z);

// ---------------------------------------------------------------- каталог (library.h): подписи без повторов
// номер варианта из id («raptor_2» → 2, «human_ST106_2» → 2); 0 — первый
inline int id_variant(const std::string& id, const std::string& base) {
    if (id.size() > base.size() + 1 && id.compare(0, base.size(), base) == 0 && id[base.size()] == '_') return atoi(id.c_str() + base.size() + 1);
    return 0;
}
// наборы оружия P/H<оружие><костюм>: 1 — дробовик, 2 — пистолет, 3 — гранатомёт (по вложению кисти: P20 — пистолет)
inline std::string weapon_title(char w) {
    return w == '1' ? T("Shotgun", "Дробовик") : w == '2' ? T("Handgun", "Пистолет") : w == '3' ? T("Grenade gun", "Гранатомёт") : T("Weapon", "Оружие");
}
inline bool weapon_set_file(const std::string& stem) {
    return stem.size() == 3 && (stem[0] == 'P' || stem[0] == 'H') && stem[1] >= '1' && stem[1] <= '3' && stem[2] >= '0' && stem[2] <= '3';
}

inline std::string actor_title(const LibActor& a) {
    if (a.kind == "regina") {
        if (a.id.rfind("regina_costume", 0) == 0) return strfmt(g_lang == LANG_RU ? "Регина, костюм %s" : "Regina, costume %s", a.id.substr(14).c_str());
        return T("Regina", "Регина");
    }
    if (a.kind == "human") {
        const std::string base = "human_" + a.at.f->stem;
        const int v = id_variant(a.id, base);
        return v ? strfmt(g_lang == LANG_RU ? "Человек %d (%s)" : "Human %d (%s)", v, a.at.f->stem.c_str())
                 : strfmt(g_lang == LANG_RU ? "Человек (%s)" : "Human (%s)", a.at.f->stem.c_str());
    }
    if (a.kind == "prop") return strfmt(g_lang == LANG_RU ? "Предмет со скелетом (%s)" : "Rigged prop (%s)", a.at.f->stem.c_str());
    const int v = id_variant(a.id, a.kind);
    return v ? strfmt("%s %d", actor_label(a.kind).c_str(), v) : actor_label(a.kind);
}

inline std::string mesh_title(const LibMesh& m) {
    if (m.kind == "in_hand") {
        const std::string& st = m.at.f->stem;
        // 06.10.2026: в дереве — внутри вида оружия: «В руке, костюм N (P2N)» (кисть костюма в меше)
        if (st.size() == 3) return strfmt(g_lang == LANG_RU ? "В руке, костюм %c (%s)" : "In hand, costume %c (%s)", st[2], st.c_str());
        return strfmt(g_lang == LANG_RU ? "%s в руке (%s)" : "%s in hand (%s)", weapon_title(st.size() > 1 ? st[1] : '0').c_str(), st.c_str());
    }
    if (m.kind == "door") return m.at.f->stem;
    const int v = id_variant(m.id, m.kind);
    const std::string base = mesh_label(m.kind);
    return v ? strfmt("%s %d", base.c_str(), v) : base;
}

inline std::string set_title(const LibSet& s) {
    if (s.id == "CORE") return T("Common (CORE)", "Общие (CORE)");
    if (weapon_set_file(s.id))
        return strfmt(g_lang == LANG_RU ? "%s, костюм %c (%s)" : "%s, costume %c (%s)", weapon_title(s.id[1]).c_str(), s.id[2], s.id.c_str());
    return s.id;
}

// где ещё лежит то же самое: «ST100 +43»
inline std::string places_hint(const LibPlace& at, size_t more) {
    return more ? strfmt("%s +%d", at.f->stem.c_str(), (int)more) : at.f->stem;
}

}  // namespace dc
