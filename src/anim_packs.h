// 08.10.2026 (player: "a character's model alone or with a pack of its animations, grouped as the game groups them; one
// animation by clicking it - with the picked model when it is a common one, else with its own character; dinosaurs: none or
// all"): the animation packs of a model. The game keeps motions in sets by file - CORE (the base), the weapon sets P/H<w><c>,
// the sets of rooms and of door transitions; Regina's CORE is split further by what its motions do (measured and checked by
// pose: tools\dc_extract\REGINA_NOTES.md of the PT tree).
#pragma once
#include "library.h"

namespace dc {

struct AnimPack {
    std::string key;            // none, all, core, core_move, core_react, core_act, core_other, weapons, weapon_1..3, doors,
                                // rooms, set:<SET>, scenes, scene:<ROOM> (people: the cutscenes of the rooms they are in)
    std::string en, ru;
    std::vector<int> motions;   // Library::motions
    int depth = 0;              // 1 - one set inside its group (indented in the list)
};

bool actor_is_dino(const LibActor& a);                                  // dinosaurs: no animation or all of it
int motion_owner(const Library& lib, int motion);                       // its character (Library::chars), -2 a room's scene, -1 none
bool motion_fits(const Library& lib, int motion, int actor);            // a common motion of this model's build
int motion_model(const Library& lib, int motion);                       // the model a motion is made for (the owner's main one)
std::vector<AnimPack> anim_packs(const Library& lib, int actor);
// the motions of the pack `key` for this model; a key the model has not: none - none, otherwise all
std::vector<int> anim_pack_motions(const Library& lib, int actor, const std::string& key);
const AnimPack* find_pack(const std::vector<AnimPack>& packs, const std::string& key);

}  // namespace dc
