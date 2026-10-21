// An enemy kind of another room put into a room (06.10.2026, player: "make it possible to pick any enemy"). The game takes
// an enemy's model and motions only from the room's own file: a spawn record (op 20 / 59 / 5B) holds the RAM addresses of
// the model and of its motion table there. A kind from another room is copied into the end of the room block - the model
// (header, bones, vertices, normals, polygons: one piece, pointers in the header and in every bone), the motion table and
// each motion of it (header, keys, frames: one piece, one pointer to the frames) - and the record gets the new addresses.
// Its pictures: the room's own model of the same body when the room has one (below); else the copy points at the other
// room's places in video memory (06.10.2026: the former characters\<id>\<id>_in_rooms.gltf is no longer written - the mesh
// path took it for every model of that body in the whole game).
#pragma once
#include <string>

#include "content.h"

namespace dc {

// src - the room that has the kind; model / motions - their addresses there. blk / base - the room block being built
// (unpacked) and its RAM address; limit - its room in RAM (0x5D000: the door model lives from 0x8015D000).
// 06.10.2026: mod / mod_base - the block of the memory for mods (Dino Crisis ME maps it at 0x81000000, memory.c): the table
// and the motions go there, the model too when the room block is full.
// 06.10.2026 (player: "its texture is completely wrong, it slid"): dst - the room being built; when it has the same body
// (actor key: shape and UV, not the picture's page and palette) its own model is taken - its pictures are in this room's
// video memory, a copied model would point at the other room's places. Then only the motions are copied; own_model says so.
static const uint32_t kModRamBase = 0x81000000u;
bool species_put(const FileContent& src, uint32_t model, uint32_t motions, const FileContent* dst, Bytes& blk, uint32_t base,
                 uint32_t limit, Bytes& mod, uint32_t mod_base, uint32_t& new_model, uint32_t& new_motions, bool& own_model,
                 std::string& note, std::string& err);

// 14.10.2026: the motions a table holds (a link {another motion, -1} is not counted: a scene raptor's table has 76 entries,
// all but 3 of them links)
int species_motions(const FileContent& c, uint32_t table);
// 14.10.2026: a fighter of this kind with the body of `model` in `room` (0: any body of the kind) - the record with the
// fullest motion table, in the rooms of the same stage first (no other stage's code needed), then anywhere. For an enemy a
// scenario adds (scn_scenes.h)
struct SpeciesSrc {
    std::string room;
    uint32_t model = 0, motions = 0;
    int nmot = 0, variant = 0, hp = 0;
};
bool species_fighter(Disc& d, const FileContent& room, uint32_t model, int kind, SpeciesSrc& out);

}  // namespace dc
