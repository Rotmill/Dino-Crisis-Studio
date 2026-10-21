#include "anim_packs.h"

#include <algorithm>
#include <map>

namespace dc {

namespace {

bool weapon_set(const std::string& id) {
    return id.size() == 3 && (id[0] == 'P' || id[0] == 'H') && id[1] >= '1' && id[1] <= '3' && id[2] >= '0' && id[2] <= '3';
}

// Regina's CORE by what a motion does (REGINA_NOTES.md: travel and turns measured, the rest read by hip height and pose):
// 0 movement, 1 reactions and damage, 2 actions (crouch, ladders, reaching for doors and pick-ups), 3 other
int core_group(int idx) {
    switch (idx) {
    case 7: case 8: case 15: case 16: case 17: case 18: case 19: case 20: case 26: case 27: case 28: case 29: case 30:
    case 33: case 41:
        return 0;   // backstep and walk with travel, turns, idles, stand / walk / run / backwards, the in-place run cycles
    case 0: case 1: case 2: case 3: case 4: case 5: case 10: case 11: case 12: case 13: case 47: case 48: case 49:
        return 1;   // hits, to the knees, stagger, knocked down forward / back, the hurt walk, collapse, down, get up
    case 6: case 9: case 14: case 21:
        return 2;   // crouched, ladder up / down, reaching out (doors, pick-ups)
    default:
        return 3;   // short one-shots and static poses
    }
}

bool humanoid(const std::string& kind) { return kind == "regina" || kind == "human"; }

void add(std::vector<int>& to, const std::vector<int>& from) { to.insert(to.end(), from.begin(), from.end()); }

}  // namespace

bool actor_is_dino(const LibActor& a) { return a.kind != "regina" && a.kind != "human" && a.kind != "prop"; }

int motion_owner(const Library& lib, int m) {
    for (size_t c = 0; c < lib.chars.size(); c++)
        if (std::find(lib.chars[c].motions.begin(), lib.chars[c].motions.end(), m) != lib.chars[c].motions.end()) return (int)c;
    for (auto& kv : lib.scene_motions)
        if (std::find(kv.second.begin(), kv.second.end(), m) != kv.second.end()) return -2;
    return -1;
}

bool motion_fits(const Library& lib, int m, int actor) {
    if (m < 0 || m >= (int)lib.motions.size() || actor < 0 || actor >= (int)lib.actors.size()) return false;
    const LibActor& a = lib.actors[actor];
    if (lib.motions[m].nbones != a.nbones) return false;
    const int o = motion_owner(lib, m);
    if (o == -2) return humanoid(a.kind);                 // a room's scene: Regina and people play it
    if (o < 0) return false;
    const std::string& ok = lib.chars[o].kind;
    return ok == a.kind || (humanoid(ok) && humanoid(a.kind));
}

int motion_model(const Library& lib, int m) {
    const int o = motion_owner(lib, m);
    if (o >= 0 && !lib.chars[o].actors.empty()) return lib.chars[o].actors[0];
    if (o == -2)
        for (size_t i = 0; i < lib.actors.size(); i++)
            if (lib.actors[i].id == "regina") return (int)i;
    return -1;
}

const AnimPack* find_pack(const std::vector<AnimPack>& packs, const std::string& key) {
    for (auto& p : packs) if (p.key == key) return &p;
    return nullptr;
}

std::vector<AnimPack> anim_packs(const Library& lib, int actor) {
    std::vector<AnimPack> out;
    if (actor < 0 || actor >= (int)lib.actors.size()) return out;
    const LibActor& a = lib.actors[actor];
    std::vector<int> mots;
    if (a.chr >= 0 && a.chr < (int)lib.chars.size()) mots = lib.chars[a.chr].motions;
    // people: the catalog keeps a cutscene at its room (who plays which motion, the room's script decides), so a person's
    // animations are its own few and the scenes of the rooms this model is in (Kirk and Tom have no own ones at all)
    std::vector<AnimPack> scenes;
    if (a.kind == "human") {
        std::vector<const DiscFile*> fs{a.at.f};
        for (auto& p : a.also) fs.push_back(p.f);
        fs.erase(std::remove(fs.begin(), fs.end(), nullptr), fs.end());
        std::sort(fs.begin(), fs.end(), [](const DiscFile* x, const DiscFile* y) { return x->stem < y->stem; });
        fs.erase(std::unique(fs.begin(), fs.end()), fs.end());
        for (const DiscFile* f : fs) {
            auto it = lib.scene_motions.find(f);
            if (it == lib.scene_motions.end() || it->second.empty()) continue;
            scenes.push_back({"scene:" + f->stem, "Scenes: room " + f->stem, "Сцены: комната " + f->stem, it->second, 1});
        }
    }
    std::vector<int> all = mots;
    for (auto& s : scenes) add(all, s.motions);
    out.push_back({"none", "No animation", "Без анимации", {}, 0});
    out.push_back({"all", "All animation", "Вся анимация", all, 0});
    if (all.empty() || actor_is_dino(a)) return out;

    std::map<int, std::vector<int>> by_set;
    std::vector<int> order;
    for (int m : mots) {
        const int si = lib.motions[m].set;
        if (!by_set.count(si)) order.push_back(si);
        by_set[si].push_back(m);
    }
    AnimPack core{"core", "Base: all (CORE)", "Основа: всё (CORE)", {}, 0};
    AnimPack cg[4] = {{"core_move", "Base: movement", "Основа: передвижение", {}, 1},
                      {"core_react", "Base: reactions, damage", "Основа: реакции, урон", {}, 1},
                      {"core_act", "Base: actions, ladders, doors", "Основа: действия, лестницы, двери", {}, 1},
                      {"core_other", "Base: other", "Основа: прочее", {}, 1}};
    AnimPack weapons{"weapons", "Weapons: all", "Оружие: всё", {}, 0};
    AnimPack wp[3] = {{"weapon_1", "Shotgun", "Дробовик", {}, 1},
                      {"weapon_2", "Handgun", "Пистолет", {}, 1},
                      {"weapon_3", "Grenade gun", "Гранатомёт", {}, 1}};
    std::string wsets[3];
    AnimPack doors{"doors", "Doors and ladders", "Двери и лестницы", {}, 0};
    AnimPack rooms{"rooms", "Rooms: all (scenes, actions)", "Комнаты: всё (сцены, действия)", {}, 0};
    std::vector<AnimPack> room_sets, other_sets;
    std::string dsets;
    for (int si : order) {
        const LibSet& s = lib.sets[si];
        const std::vector<int>& ms = by_set[si];
        if (s.id == "CORE") {
            add(core.motions, ms);
            for (int m : ms) cg[core_group(lib.motions[m].at.motion)].motions.push_back(m);
        } else if (weapon_set(s.id)) {
            add(weapons.motions, ms);
            const int w = s.id[1] - '1';
            add(wp[w].motions, ms);
            wsets[w] += " " + s.id;
        } else if (s.at.f && s.at.f->cat == "doors") {
            add(doors.motions, ms);
            dsets += " " + s.id;
        } else if (s.at.f && s.at.f->cat == "rooms") {
            add(rooms.motions, ms);
            room_sets.push_back({"set:" + s.id, "Room " + s.id, "Комната " + s.id, ms, 1});
        } else {
            other_sets.push_back({"set:" + s.id, "Set " + s.id, "Набор " + s.id, ms, 0});
        }
    }
    if (!core.motions.empty()) {
        out.push_back(core);
        for (auto& g : cg) if (!g.motions.empty()) out.push_back(g);
    }
    if (!weapons.motions.empty()) {
        out.push_back(weapons);
        for (int w = 0; w < 3; w++) {
            if (wp[w].motions.empty()) continue;
            wp[w].en += " (" + wsets[w].substr(1) + ")";
            wp[w].ru += " (" + wsets[w].substr(1) + ")";
            out.push_back(wp[w]);
        }
    }
    if (!doors.motions.empty()) {
        doors.en += " (" + dsets.substr(1) + ")";
        doors.ru += " (" + dsets.substr(1) + ")";
        out.push_back(doors);
    }
    if (!rooms.motions.empty()) {
        out.push_back(rooms);
        for (auto& r : room_sets) out.push_back(r);
    }
    for (auto& o : other_sets) out.push_back(o);
    if (scenes.size() == 1) {
        scenes[0].depth = 0;
        out.push_back(scenes[0]);
    } else if (!scenes.empty()) {
        AnimPack sc{"scenes", "Scenes: all (rooms with this model)", "Сцены: все (комнаты с этой моделью)", {}, 0};
        for (auto& s : scenes) add(sc.motions, s.motions);
        out.push_back(sc);
        for (auto& s : scenes) out.push_back(s);
    }
    return out;
}

std::vector<int> anim_pack_motions(const Library& lib, int actor, const std::string& key) {
    const std::vector<AnimPack> packs = anim_packs(lib, actor);
    if (const AnimPack* p = find_pack(packs, key)) return p->motions;
    if (key == "none") return {};
    const AnimPack* all = find_pack(packs, "all");
    return all ? all->motions : std::vector<int>();
}

}  // namespace dc
