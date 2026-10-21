// 10.10.2026: what the commands of the room script mean, for the scenario view (ui_scenario.cpp): a name, a category, a
// readable line with the values, typed fields. Only what was read in the game's handlers goes here (decoding notes:
// docs\script_vm); an unknown command shows its bytes and stays as it is.
// Objects: SELECT (0x22) picks the object later commands act on (category + number, kept per task); SET / CALC / COMPARE /
// PLACE / TURN / SOUND ... work on it. The scenario shows that object by name ("Regina", "Camera", "Piece 5").
// 14.10.2026: the full table (docs\script_vm\table_v3 + 0x2E 0x2F 0x30 0x52 0x54 0x55 0x57 0x69): one opcode can have
// several entries told apart by a byte (0x28 zones by type +2, 0x55 by mode +2, 0x57 by mode +1); conditions of IF read
// with the jump that follows them (scn_if_phrase); objects of SELECT named by what the room does with them.
#pragma once
#include <functional>
#include <map>

#include "script_edit.h"

namespace dc {

enum ScnCat : uint8_t { SC_FLOW, SC_COND, SC_FLAG, SC_OBJECT, SC_ACTOR, SC_CAMERA, SC_MESSAGE, SC_ZONE, SC_SOUND, SC_ROOM, SC_OTHER, SC_COUNT };
const char* scn_cat_name(int c);

enum ScnFt : uint8_t {
    FT_U8, FT_S8, FT_U16, FT_S16, FT_U32, FT_S32,
    FT_FRAMES,    // u16 frames (30 per second)
    FT_FLAG,      // u8 bank at +at, u8 bit at +at+1
    FT_SUB,       // s16 subroutine number
    FT_ROOMCODE,  // u16 room (0x10A = ST10A)
    FT_TEXT,      // u32 reference to a text of the script ($@ / $#)
    FT_XYZ,       // three s16: x, y, z (mm, y down)
    FT_TURN,      // s16, 4096 = 360 degrees
    FT_KIND,      // u8 enemy kind
    FT_SELCAT,    // u8 object category of SELECT
    FT_FIELD,     // u8 field number of the selected object
    FT_ENUM,      // u8 with named values (names: "0=stop|1=play")
    FT_ENUM16,    // u16 with named values
    FT_BITS,      // u8 bit flags with names (names: "1=loop|2=backwards")
    FT_PTR,       // u32 address in the game's memory (hex)
    FT_HEX,       // bytes as they are
    FT_QUAD,      // four corners (s16 x, s16 z) of a zone: 16 bytes
    FT_ITEM,      // u8 item id (255 = random item in an item zone)
    FT_LOCK,      // u8 door lock: 0 none, 1..3, 6..8 key cards, 253..255 never opens, other = key item id
};

struct ScnField {
    const char* name;
    uint8_t at;
    ScnFt t;
    const char* names = nullptr;   // FT_ENUM / FT_BITS: "value=name|value=name"
};

struct ScnOp {
    uint8_t op = 0;
    const char* name = "";       // short English name for the Add menu
    ScnCat cat = SC_OTHER;
    const char* phrase = "";     // readable line: {field} = that field's value, {object} = the selected object
    std::vector<ScnField> fields;
    bool needs_object = false;   // acts on the selected object (SELECT before it)
    bool technical = false;      // room building and setup: shown dimmer
    int8_t sub_at = 0;           // > 0: this entry is for commands with byte [sub_at] == sub_type
    int16_t sub_type = -1;
    bool hidden = false;         // read in the game's scripts but not offered in the Add menu (unsafe or never useful)
};

// what a command line needs to know around it
struct ScnCtx {
    std::function<std::string(const ScriptSym&)> text;   // a text reference -> a short quote
    std::function<std::string(int)> sub;                 // subroutine number -> its title
    std::function<int(int)> actor_kind;                  // actor slot -> kind (from the room's actor commands), -1 unknown
    std::function<std::string(int, int)> obj_note;       // (category, number) -> what the room uses it for ("" none)
    int sel_cat = 0, sel_num = 0;                        // the object selected before the command (a new task: 0, 0)
    int last_test = -1;                                  // the opcode of the last condition command before (raw jumps)
};

// what the room's commands say about its objects: actor kinds, what a piece / effect / mechanism is
struct ScnRoomNames {
    std::map<int, int> slot_kind;          // actor slot -> kind (-1: different kinds in one slot)
    std::map<int, std::string> piece, effect, mech;
};
void scn_room_names(const std::vector<ScriptAtom>& atoms, ScnRoomNames& out);
// actor_kind and obj_note of a context from those names (the names must live as long as the context)
void scn_ctx_bind(ScnCtx& c, const ScnRoomNames& n);

// the description of a command; nullptr: not decoded
const ScnOp* scn_op(const ScriptAtom& a);
// every command that can be added from the menu, by category
std::vector<const ScnOp*> scn_ops_of(ScnCat c);
// the readable line of a command
std::string scn_phrase(const ScriptAtom& a, const ScnCtx& c);
// the same as a line of its own: a condition outside an IF reads "Check ..." / "Compare ... with N"
std::string scn_line_phrase(const ScriptAtom& a, const ScnCtx& c);
// is this command one that only sets the condition word for the next jump (TESTFLAG, COMPARE and the like)
bool scn_is_cond_op(int op);
bool scn_is_compare_op(int op);   // 0x29 / 0x15: equal / less / greater; the others: true / false
// the line of an IF: its condition commands and its jump (the block runs when the jump is NOT taken)
std::string scn_if_phrase(const std::vector<const ScriptAtom*>& cond, const ScriptAtom& jump, const ScnCtx& c);
// a jump that is not part of an IF / LOOP (c.last_test: what set the condition word)
std::string scn_jump_phrase(const ScriptAtom& jump, const ScnCtx& c);
// IF of a comparison: the operators the block can run on, and the jump bit of each (block runs when the jump is not taken)
const std::vector<std::pair<int, const char*>>& scn_compare_ops();
// objects and their fields (0x22 SELECT categories)
std::string scn_object_name(int cat, int num, const ScnCtx& c);
const char* scn_cat_object(int cat);                  // "actor", "piece", "camera" ...
std::string scn_field_name(int cat, int field);       // "" - the category has no such field
std::vector<int> scn_fields_of(int cat);
// a new command of this opcode with neutral values (jump and text fields left to the caller)
ScriptAtom scn_new_atom(uint8_t op);
ScriptAtom scn_new_atom(const ScnOp& o);
// one field's value as an int / set it (FT_FLAG: bank*1000 + bit)
int scn_get(const ScriptAtom& a, const ScnField& f);
void scn_set(ScriptAtom& a, const ScnField& f, int v);
// FT_ENUM / FT_BITS names: (value, name)
std::vector<std::pair<int, std::string>> scn_names(const char* names);
// door lock as words
std::string scn_lock_name(int v);

// 14.10.2026: what of a command is a place in THIS room, to show and drag in the 3D view: points (three s16 x, y, z) with
// the turn that goes with them, and the zone area (four corners x, z on the zone's floor). A door's place is in the room it
// leads to, velocities are not places - they are left out.
struct ScnPoint {
    const char* name;
    int at;            // x, y, z at +at
    int turn_at = -1;  // s16 turn (4096 = 360) of an actor standing there
    int atom = -1;     // 14.10.2026: the atom that holds it (-1: the command itself; a scene block's points are in its lines)
};
std::vector<ScnPoint> scn_points(const ScriptAtom& a);
int scn_quad_at(const ScriptAtom& a);           // -1: no area
float scn_quad_y(const ScriptAtom& a);          // the height of the zone's floor (floor L: y = -425 * L)

}  // namespace dc
