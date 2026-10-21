// 14.10.2026 (player 10.10: "enemies come by the scenario the way the game does it: through a door, jumping in through a
// window, with their own scene - take the game's scenes"): an enemy's entrance as a block of ordinary script commands, so
// it works in any room and in the release as it is. Two scenes of the game, rebuilt from what their code does:
// - come in: the follower scene of 0x5B (func_80059D80 steps 1..6): sounds 7 and 8 at the opening, Regina stops and is
//   hidden, the camera cuts to a fixed view, the raptor appears and plays its entrance (AI state 1 / 0x25 - RELEASE 0x3B
//   with 0x25 gives exactly that word), then the room cameras and the player's control come back. 0x5B itself needs a
//   roster dinosaur of the room next door that was close to Regina - this one comes every time.
// - leap in: the window of ST10A sub 8: the raptor outside, walls ignored (actor field 16 = 0x8000), the camera cut,
//   "leap to" a point inside (0x3A raptor mode 4), shown after 10 frames with the glass sound 144, then back to its AI.
// Both stand inside "If the raptor's death flag is clear": killed once, it does not come again (like the game's own).
#pragma once
#include "scenario.h"
#include "scn_ops.h"

namespace dc {

enum ScnSceneKind { SCENE_COME_IN = 0, SCENE_LEAP_IN = 1, SCENE_COUNT };

struct ScnScene {
    int kind = SCENE_COME_IN;
    int slot = -1;                 // actor slot 0..9
    int death_flag = 0;            // bit of flag bank 4
    int signal = -1;               // bit of flag bank 3 (room signals): "arrived" of the moves
    uint8_t dkind = 2, variant = 0, ai = 1;
    uint16_t hp = 0;
    uint32_t model = 0, motions = 0;
    int place[3] = {0, 0, 0}, turn = 0;   // where the raptor appears (the door / outside the window)
    int land[3] = {0, 0, 0};              // leap in: where it lands
    int eye[3] = {0, 0, 0}, target[3] = {0, 0, 0};   // the scene camera
    // a fighter of its kind ("ROOM MODEL MOTIONS", species_fighter): its motions come in on build; empty - the room's own
    // raptor already is one. Without it a scene's raptor (ST10D: 3 motions) would twist and fly once it hunts
    std::string from;
};

const char* scn_scene_name(int kind);
// values that fit the room (death_flag is the caller's: it must be free in the whole game). false + err: not possible here.
// model 0: the room has no raptor - the caller names a fighter of another room (model, motions, from): the build brings
// its body and motions in, as for any enemy in any room
bool scn_scene_defaults(const std::vector<ScriptAtom>& atoms, int kind, ScnScene& s, std::string& err);
// the scene as a tree node; its atoms are appended to `atoms`
ScnNode scn_scene_build(std::vector<ScriptAtom>& atoms, const ScnScene& s);

// The studio shows such a block as ONE line with plain fields (player 14.10: "a constructor a non-programmer understands";
// "everything still there if wanted" - its commands open with More). Found again by its shape, so script.txt needs nothing.
bool scn_scene_detect(const std::vector<ScriptAtom>& atoms, const ScnNode& n, ScnScene& s);
// what to show and drag in the 3D view: the raptor (place and turn), where it lands, the camera (atom = the line holding it)
std::vector<ScnPoint> scn_scene_points(const std::vector<ScriptAtom>& atoms, const ScnNode& n);
// the copies of the raptor's place inside a block (where it holds, where the sounds play) follow its place; every block
void scn_scenes_fix(Scenario& s, std::vector<ScriptAtom>& atoms);
// the plain fields of the studio back into the block's lines (place, turn, temper, HP, where it lands, the camera)
void scn_scene_apply(std::vector<ScriptAtom>& atoms, const ScnNode& n, const ScnScene& s);
// the scene as a new event (a wait, the scene, End) started at the picked line of `region` ("Start event", a free task
// slot): it never holds up the event it is added to (room start goes on setting the room up)
bool scn_scene_add(Scenario& s, std::vector<ScriptAtom>& atoms, int region, const std::string& key, const ScnScene& sc,
                   int wait, std::string& err);

}  // namespace dc
