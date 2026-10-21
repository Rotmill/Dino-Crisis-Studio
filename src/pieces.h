// Placed pieces of a room: CREATE (0x23) commands of the room script that put a mesh group at its own place and turn
// (doors, hatches, boxes, fans, pipes...). 06.10.2026, player: "the dynamic objects stand in the middle by default - put them
// where the game puts them, and that position is what we will edit". Command layout (memory dc_room_placed_pieces):
// +1 slot, +2 animation, +3 OT mode, +4 u16 OT slot, +8 mesh group (RAM address), +12..+17 place s16 x3, +18..+23 turn
// s16 x3 (4096 = 360), +24/+28 animation pointers. The game puts a piece at  place + R*point  with R = Rx*Ry*Rz (the same as
// the runtime, pt_renderer.c bp_rot). A CREATE with no place and no turn is the room body, already in room coordinates.
#pragma once
#include <string>
#include <vector>

#include "content.h"
#include "gltf.h"

namespace dc {

// pieces from the script text (script.txt view, so saved edits count):
// [{ "at": command offset, "sub": subroutine, "slot", "group": "8011A84C", "gi": group index in the room mesh (-1 - not in
//    it), "x","y","z" (mm, game Y down), "rx","ry","rz" (4096 = 360), "anim": byte, "placed": place or turn non-zero }]
json room_pieces(const FileContent& c, const std::string& script_text);
// room triangles as the game shows them: a group the script puts at a place - moved and turned there (put twice - twice);
// groups no CREATE names stay as they are
std::vector<MTri> room_tris_placed(const FileContent& c, const json& pieces);
// write place and turn of the pieces back into their command lines (bytes +12..+23); false - a line is gone
bool pieces_to_script(const json& pieces, std::string& script_text, std::string& err);
// 06.10.2026 (player: "the item placeholders are at their places, but the models still stand in the middle - spawn them
// where the game does"). Item models as the game puts them (zone handler 0x800574E0 -> func_8001A638): the model group of
// the item record at the middle of the zone diagonal (corners 0 and 2), Y 0, turn 0 (weapons and ammo, id < 11: Z 3072),
// then the script's own edits of that section right after the item: SELECT 3 s (22 03 s 00), 36 (place), 37 (turn),
// 2A (fields 3..8 = x y z rx ry rz). A zone moved in the Studio moves those x/z edits with it (as the save does).
// items: the room's item records (Items panel, with copies and removed). Entries like room_pieces plus "item": true.
json room_item_models(const FileContent& c, const std::string& script_text, const json& items);
void piece_rot(int rx, int ry, int rz, float M[9]);

}  // namespace dc
