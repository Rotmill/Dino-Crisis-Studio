// 10.10.2026: the table of room script commands for the scenario view (scn_ops.h). Every entry was read in the game's
// handler (table 0x8009E370; decoding notes in docs\script_vm g1..g8); lengths from script.cpp.
// 14.10.2026: table v3 (all decoded commands, zones by type) + 0x2E 0x2F 0x30 0x52 0x54 0x55 0x57 0x69; IF lines with
// the operator of the jump; objects of SELECT named by what the room does with them.
#include "scn_ops.h"

#include <cmath>
#include <cstring>
#include <map>

#include "placement.h"
#include "script.h"

namespace dc {

const char* scn_cat_name(int c) {
    static const char* n[SC_COUNT] = {"Flow", "Conditions", "Flags", "Objects", "Characters", "Camera", "Messages", "Zones", "Sound", "Room", "Other"};
    return c >= 0 && c < SC_COUNT ? n[c] : "Other";
}

// ---------------------------------------------------------------- objects and fields (SELECT categories; g3 table)
namespace {

struct FieldName { int f; const char* name; };

const std::vector<FieldName>& fields_of_cat(int cat) {
    static const std::vector<FieldName> c0 = {{0, "button presses"}, {2, "game mode"}, {3, "mode step"}, {4, "mode value 4"}, {5, "mode value 5"},
                                              {6, "mode and step"}, {7, "came from room"}, {8, "item used"}, {9, "killed by kind"}, {10, "ending"},
                                              {1, "byte 1"}};
    static const std::vector<FieldName> c1 = {
        {0, "random"},   {1, "global 1"}, {2, "global 2"}, {3, "global 3"}, {4, "global 4"}, {5, "global 5"},  {6, "global 6"},
        {7, "global 7"}, {8, "local 0"},  {9, "local 1"},  {10, "local 2"}, {11, "local 3"}, {14, "local 6"},  {15, "local 7"},
        {16, "local 8"}, {17, "local 9"}, {12, "local 4"}, {13, "local 5"}, {18, "local 10"}, {19, "local 11"}};
    static const std::vector<FieldName> c2 = {
        {3, "x"},          {4, "y"},          {5, "z"},          {7, "turn"},       {6, "tilt"},      {8, "roll"},     {21, "HP"},
        {10, "state"},     {11, "state 1"},   {12, "state 2"},   {13, "state 3"},   {19, "all states"}, {23, "speed x"}, {24, "speed y"},
        {25, "speed z"},   {32, "ai"},        {0, "flags"},      {1, "kind"},       {2, "variant"},   {16, "flags 16"}, {17, "flags 17"},
        {18, "flags 18"},  {26, "flags 26"},  {35, "start x"},   {36, "start y"},   {37, "start z"},  {27, "byte 27"},  {28, "byte 28"},
        {29, "byte 29"},   {30, "byte 30"},   {31, "byte 31"},   {33, "value 33"},  {34, "byte 34"},  {38, "byte 38"},  {39, "byte 39"},
        {40, "byte 40"},   {41, "byte 41"},   {42, "byte 42"}};
    static const std::vector<FieldName> c3 = {{3, "x"},          {4, "y"},          {5, "z"},       {7, "turn"},     {6, "tilt"},
                                              {8, "roll"},       {10, "state"},     {0, "flags"},   {1, "type"},     {2, "byte 2"},
                                              {14, "draw mode"}, {15, "order"},     {27, "byte 27"}, {28, "byte 28"}, {29, "byte 29"},
                                              {30, "byte 30"},   {31, "byte 31"}};
    static const std::vector<FieldName> c4 = {{3, "x"},        {4, "y"},        {5, "z"},        {7, "turn"},     {6, "tilt"},     {8, "roll"},
                                              {23, "speed x"}, {24, "speed y"}, {25, "speed z"}, {27, "gravity"}, {10, "state"},   {11, "state 1"},
                                              {12, "state 2"}, {13, "state 3"}, {0, "flags"},    {1, "type"},     {2, "param"},    {28, "byte 28"},
                                              {29, "byte 29"}, {30, "byte 30"}, {31, "byte 31"}};
    static const std::vector<FieldName> c5 = {{12, "shake x"},  {13, "shake y"},  {14, "offset z"}, {10, "phase"},    {11, "cut"},
                                              {0, "x"},         {1, "y"},         {2, "z"},         {3, "angle x"},  {4, "angle y"},
                                              {5, "angle z"},   {15, "origin x"}, {16, "origin y"}, {17, "origin z"}, {6, "byte 6"},
                                              {7, "byte 7"},    {8, "byte 8"},    {9, "byte 9"}};
    static const std::vector<FieldName> c6 = {{3, "x"}, {4, "y"}, {5, "z"}, {0, "flags"}, {1, "room"}, {2, "death flag"}, {6, "value 6"}, {7, "value 7"}};
    static const std::vector<FieldName> c7 = {{0, "x"}, {1, "y"}, {2, "z"}, {3, "red"}, {4, "green"}, {5, "blue"}};
    static const std::vector<FieldName> c8 = {{0, "state"}, {1, "in use"}, {2, "param"}};
    static const std::vector<FieldName> none;
    switch (cat) {
    case 0: return c0;
    case 1: return c1;
    case 2: return c2;
    case 3: return c3;
    case 4: return c4;
    case 5: return c5;
    case 6: return c6;
    case 7: return c7;
    case 8: return c8;
    default: return none;
    }
}

}  // namespace

const char* scn_cat_object(int cat) {
    static const char* n[9] = {"game", "variables", "actor", "piece", "effect", "camera", "roster dinosaur", "lamp", "mechanism"};
    return cat >= 0 && cat < 9 ? n[cat] : "object";
}

std::string scn_object_name(int cat, int num, const ScnCtx& c) {
    const std::string note = c.obj_note ? c.obj_note(cat, num) : std::string();
    auto with = [&](const std::string& s) { return note.empty() ? s : s + " (" + note + ")"; };
    switch (cat) {
    case 0: return "Game";
    case 1: return "Variables";
    case 2: {
        if (num == 10) return "Regina";
        const int k = c.actor_kind ? c.actor_kind(num) : -1;
        const std::string kn = k >= 0 ? enemy_kind_name(k) : std::string();
        return kn.empty() ? strfmt("Actor %d", num) : strfmt("Actor %d (%s)", num, kn.c_str());
    }
    case 3: return with(strfmt("Piece %d", num));
    case 4: return with(strfmt("Effect %d", num));
    case 5: return "Camera";
    case 6: return strfmt("Roster dinosaur %d", num);
    case 7: return strfmt("Lamp %d", num);
    case 8: return with(strfmt("Mechanism %d", num));
    default: return strfmt("Object %d/%d", cat, num);
    }
}

std::string scn_field_name(int cat, int field) {
    for (auto& f : fields_of_cat(cat)) if (f.f == field) return f.name;
    return std::string();
}

std::vector<int> scn_fields_of(int cat) {
    std::vector<int> r;
    for (auto& f : fields_of_cat(cat)) r.push_back(f.f);
    return r;
}

std::vector<std::pair<int, std::string>> scn_names(const char* names) {
    std::vector<std::pair<int, std::string>> r;
    if (!names) return r;
    const char* p = names;
    while (*p) {
        const char* bar = strchr(p, '|');
        const std::string item = bar ? std::string(p, bar) : std::string(p);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) r.push_back({(int)strtol(item.substr(0, eq).c_str(), nullptr, 0), item.substr(eq + 1)});
        if (!bar) break;
        p = bar + 1;
    }
    return r;
}

std::string scn_lock_name(int v) {
    switch (v) {
    case 0: return "no lock";
    case 1: return "locked from the other side";
    case 2: return "opens from this side";
    case 3: return "DDK lock";
    case 6: return "key card Lv.C";
    case 7: return "key card Lv.B";
    case 8: return "key card Lv.A";
    case 0xFD: return "no need to enter now";
    case 0xFE: return "lift does not work";
    case 0xFF: return "locked";
    default: {
        const std::string n = item_name(v);
        return "needs " + (n.empty() ? strfmt("item %d", v) : n);
    }
    }
}

// ---------------------------------------------------------------- what the room uses its objects for
void scn_room_names(const std::vector<ScriptAtom>& atoms, ScnRoomNames& out) {
    out = ScnRoomNames();
    auto put = [](std::map<int, std::string>& m, int k, const std::string& v) {
        auto it = m.find(k);
        if (it == m.end()) m[k] = v;
        else if (it->second != v) it->second.clear();   // used for different things: no name
    };
    for (auto& a : atoms) {
        if (a.kind != ScriptAtom::OP || a.b.size() < 4) continue;
        const uint8_t op = a.b[0];
        if (op == 0x20 || op == 0x59 || op == 0x5B) {
            auto it = out.slot_kind.find(a.b[1]);
            if (it == out.slot_kind.end()) out.slot_kind[a.b[1]] = a.b[2];
            else if (it->second != a.b[2]) it->second = -1;   // different kinds in one slot: unknown
        } else if (op == 0x28 && a.b[2] == 4 && a.b.size() >= 36 && a.b[34] != 0xFF) {
            const int id = a.b[28];
            const std::string n = id == 0xFF ? std::string("a random item") : item_name(id);
            put(out.piece, a.b[34], "model of " + (n.empty() ? strfmt("item %d", id) : n));
        } else if (op == 0x23 && a.b.size() >= 4 && a.b[2] == 4) {
            put(out.piece, a.b[1], "laser");
        } else if (op == 0x42 && a.b.size() >= 4 && a.b[1] != 0xFF) {
            put(out.effect, a.b[1], a.b[2] == 5 ? std::string("lamp") : a.b[2] == 3 ? std::string("sprite") : strfmt("type %d", a.b[2]));
        } else if (op == 0x58 && a.b[1] != 0xFF) {
            const int t = a.b[2];
            put(out.mech, a.b[1], t == 2 ? std::string("turning piece") : t == 3 ? std::string("DDK lock") : t == 4 ? std::string("laser fence") : strfmt("type %d", t));
        }
    }
}

void scn_ctx_bind(ScnCtx& c, const ScnRoomNames& n) {
    c.actor_kind = [&n](int slot) {
        auto it = n.slot_kind.find(slot);
        return it == n.slot_kind.end() ? -1 : it->second;
    };
    c.obj_note = [&n](int cat, int num) -> std::string {
        const std::map<int, std::string>* m = cat == 3 ? &n.piece : cat == 4 ? &n.effect : cat == 8 ? &n.mech : nullptr;
        if (!m) return std::string();
        auto it = m->find(num);
        return it == m->end() ? std::string() : it->second;
    };
}

// ---------------------------------------------------------------- the commands
namespace {

// 0x3A ACT modes: Regina and human NPCs share one table, raptors another (g4); others unknown
const char* kActHuman =
    "0=hold|1=walk to|2=run to|3=step back to|4=turn by|5=stop|6=face|7=move to|8=walk straight to|9=run straight to|"
    "10=move straight to|11=turn around|12=go on with the motion to|13=back away to|14=special|15=walk (second set) to";
const char* kActRaptor = "0=hold|1=walk to|2=run to|3=stalk to|4=leap to|5=pounce at Regina|7=finish the motion|9=face|10=trot to";
const char* kZoneFlags = "1=point ahead|2=action button";

std::vector<ScnOp> make_table() {
    std::vector<ScnOp> t;
    auto add = [&](uint8_t op, const char* name, ScnCat cat, const char* phrase, std::vector<ScnField> f, bool obj = false, bool tech = false,
                   int sub = -1, int sub_at = 0, bool hidden = false) -> ScnOp& {
        ScnOp o;
        o.op = op;
        o.name = name;
        o.cat = cat;
        o.phrase = phrase;
        o.fields = std::move(f);
        o.needs_object = obj;
        o.technical = tech;
        o.sub_type = (int16_t)sub;
        o.sub_at = (int8_t)(sub >= 0 ? (sub_at ? sub_at : 2) : 0);
        o.hidden = hidden;
        t.push_back(std::move(o));
        return t.back();
    };
    // ---- flow (g1: task runner func_80055640, 10 slots; slot 0 = room start, 9 = camera, 1..8 = own tasks)
    add(0x01, "Start event", SC_FLOW, "Start {sub} alongside (task {slot})", {{"sub", 2, FT_SUB}, {"slot", 1, FT_U8}});
    add(0x05, "Continue as", SC_FLOW, "Continue as {sub}", {{"sub", 2, FT_SUB}});
    add(0x0F, "Run routine", SC_FLOW, "Run {sub}", {{"sub", 2, FT_SUB}});
    add(0x02, "Wait", SC_FLOW, "Wait {frames}", {{"frames", 2, FT_FRAMES}});
    add(0x03, "Next frame", SC_FLOW, "Wait one frame", {});
    add(0x12, "Wait for message", SC_FLOW, "Wait until the message and voice end", {});
    add(0x18, "Wait for flag", SC_FLOW, "Wait until flag {flag} is set", {{"flag", 1, FT_FLAG}});
    add(0x16, "Wait by value", SC_FLOW, "Wait {object} {field} frames", {{"field", 1, FT_FIELD}}, true);
    add(0x06, "Stop task", SC_FLOW, "Stop the task in slot {slot}", {{"slot", 1, FT_U8}});
    add(0x07, "Pause task", SC_FLOW, "Pause the task in slot {slot}", {{"slot", 1, FT_U8}});
    add(0x08, "Resume task", SC_FLOW, "Resume the task in slot {slot}", {{"slot", 1, FT_U8}});
    add(0x04, "End", SC_FLOW, "End", {});
    add(0x0D, "Nothing", SC_FLOW, "Nothing", {});
    add(0x13, "Nothing (13)", SC_FLOW, "Nothing", {}, false, true, -1, 0, true);
    // g1: 0x10 pops a return address without the end-of-task check (crash at the top level), 0x11 never moves on (freeze)
    add(0x10, "Return (unsafe)", SC_FLOW, "Return to the caller (unsafe at the top)", {}, false, true, -1, 0, true);
    add(0x11, "Hang", SC_FLOW, "Hang the game (never use)", {}, false, true, -1, 0, true);
    // ---- conditions (read by the next BRANCH; the IF line names them, scn_if_phrase)
    add(0x26, "Flag", SC_COND, "flag {flag}", {{"flag", 1, FT_FLAG}});
    add(0x14, "Flag (14)", SC_COND, "flag {flag}", {{"flag", 1, FT_FLAG}}, false, false, -1, 0, true);
    add(0x29, "Value", SC_COND, "{object} {field}", {{"field", 2, FT_FIELD}, {"value", 4, FT_S32}}, true);
    add(0x15, "Value (15)", SC_COND, "{object} {field}", {{"field", 2, FT_FIELD}, {"value", 4, FT_S32}}, true, false, -1, 0, true);
    add(0x52, "Regina has item", SC_COND, "Regina has {item}", {{"item", 1, FT_ITEM}});
    add(0x5C, "Dinosaur in room", SC_COND, "a live dinosaur in room {room}", {{"room", 1, FT_U8}});
    // ---- flags (g2: banks 0 story, 1 control, 2 system, 3 room signals, 4 enemy dead, 7 item taken, 9 door open, 11 keys)
    add(0x25, "Set flag", SC_FLAG, "Flag {flag} = {value}", {{"flag", 1, FT_FLAG}, {"value", 3, FT_U8}});
    add(0x27, "Random number", SC_FLAG, "Random number into global {var}", {{"var", 1, FT_U8}});
    // ---- objects (g2 0x22, g3 0x2A / 0x2B / 0x2F, g4 0x31..0x39, g5 0x43..0x46, 0x48, g6 0x51 / 0x54)
    add(0x22, "Pick object", SC_OBJECT, "With {object}", {{"object", 1, FT_SELCAT}, {"number", 2, FT_U8}});
    add(0x2A, "Set value", SC_OBJECT, "{object} {field} = {value}", {{"field", 2, FT_FIELD}, {"value", 4, FT_S32}}, true);
    add(0x2B, "Change value", SC_OBJECT, "{object} {field} {op} {operand}",
        {{"op", 1, FT_ENUM, "0=+=|1=-=|2=*=|3=/=|4=%=|5=OR|6=AND|7=XOR|8=NOT|9=<<|10=>>"}, {"field", 2, FT_FIELD}, {"operand", 4, FT_S32}}, true);
    add(0x51, "Change by value", SC_OBJECT, "{object} {field} {op} another value",
        {{"op", 1, FT_ENUM, "0=+=|1=-=|2=*=|3=/=|4=%=|5=OR|6=AND|7=XOR|8=NOT|9=<<|10=>>"}, {"field", 2, FT_FIELD}, {"from", 3, FT_SELCAT},
         {"from number", 4, FT_U8}, {"from field", 5, FT_U8}},
        true);
    add(0x36, "Place", SC_OBJECT, "Place {object} at {place}", {{"place", 2, FT_XYZ}}, true);
    add(0x37, "Rotate", SC_OBJECT, "Turn {object} to {turn}", {{"tilt", 2, FT_TURN}, {"turn", 4, FT_TURN}, {"roll", 6, FT_TURN}}, true);
    add(0x48, "Show / hide", SC_OBJECT, "{visible} {what}", {{"what", 1, FT_SELCAT}, {"number", 2, FT_U8}, {"visible", 3, FT_ENUM, "0=Hide|1=Show"}});
    add(0x2F, "Remove", SC_OBJECT, "Remove {what}", {{"what", 1, FT_SELCAT}, {"number", 2, FT_U8}});
    add(0x54, "Attach", SC_OBJECT, "Attach {object} to {parent}", {{"parent", 1, FT_SELCAT}, {"number", 2, FT_U8}}, true, true);
    add(0x43, "Velocity", SC_OBJECT, "Velocity A = {step}", {{"step", 2, FT_XYZ}});
    add(0x44, "Acceleration", SC_OBJECT, "Acceleration B = {accel}", {{"accel", 2, FT_XYZ}});
    add(0x45, "Spin", SC_OBJECT, "Spin C = {spin}", {{"spin", 2, FT_XYZ}});
    add(0x46, "Spin acceleration", SC_OBJECT, "Spin acceleration D = {accel}", {{"accel", 2, FT_XYZ}});
    add(0x31, "Move by velocity", SC_OBJECT, "Move {object} by velocity A", {}, true);
    add(0x32, "Accelerate", SC_OBJECT, "Velocity A += acceleration B", {});
    add(0x33, "Spin by velocity", SC_OBJECT, "Rotate {object} by spin C", {}, true);
    add(0x34, "Spin up", SC_OBJECT, "Spin C += spin acceleration D", {});
    add(0x38, "Save value", SC_OBJECT, "Save {object} {field}", {{"field", 1, FT_FIELD}}, true);
    add(0x39, "Restore value", SC_OBJECT, "Restore {object} {field}", {{"field", 1, FT_FIELD}}, true);
    // ---- characters (g2 0x20 / 0x24; g4 0x3A / 0x3B; g5 0x3D; g6 0x59 / 0x5A / 0x5B; g7 0x61 / 0x66 / 0x68; g8 0x57)
    add(0x20, "Enemy", SC_ACTOR, "{kind} in slot {slot} at {place}",
        {{"slot", 1, FT_U8}, {"kind", 2, FT_KIND}, {"place", 8, FT_XYZ}, {"turn", 14, FT_TURN}, {"variant", 3, FT_U8}, {"death flag", 4, FT_U8},
         {"ai", 5, FT_U8}, {"hp", 6, FT_U16}, {"model", 16, FT_PTR}, {"motions", 20, FT_PTR}});
    add(0x59, "Roster dinosaur", SC_ACTOR, "{kind} of room {from room} as actor {slot}, at one of its places",
        {{"slot", 1, FT_U8}, {"kind", 2, FT_KIND}, {"variant", 3, FT_U8}, {"from room", 16, FT_U8}, {"model", 4, FT_PTR}, {"motions", 8, FT_PTR},
         {"places", 12, FT_PTR}});
    add(0x5B, "Follower", SC_ACTOR, "{kind} from room {from room} may follow Regina in as actor {slot} at {place}{flags}",
        {{"slot", 1, FT_U8}, {"kind", 2, FT_KIND}, {"place", 4, FT_XYZ}, {"turn", 10, FT_TURN}, {"from room", 20, FT_U8}, {"chance", 21, FT_U8},
         {"max distance", 22, FT_U16}, {"flags", 25, FT_ENUM, "0=, with its scene|2=, with its scene at once|3=, no scene"}, {"camera shot", 26, FT_U8},
         {"scene sub", 27, FT_U8}, {"camera target", 28, FT_XYZ}, {"camera eye", 36, FT_XYZ}, {"variant", 3, FT_U8}, {"ai", 24, FT_U8},
         {"model", 12, FT_PTR}, {"motions", 16, FT_PTR}});
    add(0x24, "Play motion", SC_ACTOR, "{object}: motion {motion}{flags}",
        {{"motion", 4, FT_PTR}, {"flags", 1, FT_BITS, "1=loop|2=backwards|4=fast|8=slow|32=pose only"}, {"from frame", 2, FT_U8},
         {"blend frames", 3, FT_U8}},
        true);
    add(0x3A, "Move / turn", SC_ACTOR, "{object}: {mode} {point}, then flag 3/{done flag}",
        {{"mode", 2, FT_ENUM, kActHuman}, {"point", 4, FT_XYZ}, {"done flag", 3, FT_U8}}, true);
    add(0x5A, "Route", SC_ACTOR, "{object}: along {points} points, then flag 3/{done flag}",
        {{"mode", 1, FT_U8}, {"done flag", 2, FT_U8}, {"points", 3, FT_U8}}, true);
    add(0x3B, "Give control back", SC_ACTOR, "{object} back to normal control", {{"state", 1, FT_U8}}, true);
    add(0x3D, "Look", SC_ACTOR, "{object}: {action}",
        {{"action", 1, FT_ENUM,
          "0=look ahead|1=automatic look|2=nod|3=shake head|4=turn head to angles|5=look at actor|6=look at point|7=head free|8=head back to the motion"},
         {"target", 4, FT_XYZ}, {"count", 2, FT_U8}, {"speed", 10, FT_U8}, {"speed sideways", 11, FT_U8}},
        true);
    add(0x61, "Animation flags", SC_ACTOR, "{object}: {op} animation flags{mask}",
        {{"op", 1, FT_ENUM, "1=set|0=clear"}, {"mask", 2, FT_BITS, "32=frozen|64=partial blend|128=arms blend"}}, true);
    add(0x66, "Feet", SC_ACTOR, "{object}: feet {mode}", {{"mode", 1, FT_ENUM, "0=follow the motion|1=not planted|128=planted"}, {"foot", 2, FT_U8}}, true);
    add(0x68, "Borrow motions", SC_ACTOR, "Regina: {mode}", {{"mode", 2, FT_ENUM, "1=motions of another actor|0=her own motions"}, {"actor", 1, FT_U8}});
    add(0x47, "Button mash", SC_ACTOR, "Button mashing {on}", {{"on", 1, FT_ENUM, "1=starts|0=stops"}});
    // g8: Regina +0x240 (weapon) saved to 0x800C5008 and cleared / put back (func_8004FEE8 equips)
    add(0x57, "Weapon away / back", SC_ACTOR, "Regina {state}", {{"state", 2, FT_ENUM, "0=puts her weapon away|1=takes her weapon back"}}, false,
        false, 0, 1);
    add(0x57, "Regina bit 4", SC_ACTOR, "Regina state bit 4 {on}", {{"on", 2, FT_ENUM, "1=on|0=off"}}, false, true, 1, 1, true);
    add(0x57, "Stance flags", SC_ACTOR, "Flag 2/9, 2/10 or 2/11 from how Regina stands", {}, false, true, 2, 1);
    // ---- camera (g3 0x2E / 0x30; g5 0x4B / 0x4C / 0x4D; g4 0x35; g7 0x6D)
    // 14.10.2026: +2 is where it looks, +8 the camera (camera +0x30 / +0x20 as with 0x4C; checked in the game)
    add(0x2E, "Camera shot", SC_CAMERA, "Camera at {eye}, looking at {target}",
        {{"target", 2, FT_XYZ}, {"eye", 8, FT_XYZ}, {"target speed (0 = cut)", 16, FT_S16}, {"eye speed (0 = cut)", 18, FT_S16}});
    add(0x30, "Camera mode", SC_CAMERA, "Camera: {mode}",
        {{"mode", 1, FT_ENUM, "0=back to the room cameras|2=script shots|5=view from actor 0|6=above Regina, at actor 0|9=fixed room camera"},
         {"glide (mode 0) / camera (mode 9)", 2, FT_U8}});
    add(0x4C, "Camera move", SC_CAMERA, "Camera looks at {target} from {eye}",
        {{"target", 2, FT_XYZ}, {"eye", 18, FT_XYZ}, {"target speed", 8, FT_S16}, {"eye speed", 24, FT_S16}, {"target accel", 10, FT_S16},
         {"eye accel", 26, FT_S16}, {"target heading", 12, FT_S16}, {"target turn", 14, FT_S16}, {"eye heading", 28, FT_S16}, {"eye turn", 30, FT_S16}});
    add(0x4B, "Camera orbit", SC_CAMERA, "Camera circles for {frames}",
        {{"frames", 4, FT_FRAMES}, {"pitch step", 6, FT_S16}, {"yaw step", 8, FT_S16}, {"distance step", 10, FT_S16}});
    add(0x4D, "Camera speed", SC_CAMERA, "Camera speed {speed}", {{"which", 1, FT_U8}, {"speed", 2, FT_S16}, {"accel", 4, FT_S16}});
    add(0x6D, "Camera save", SC_CAMERA, "{mode} the camera", {{"mode", 1, FT_ENUM, "1=Save|0=Restore"}});
    add(0x35, "Fade", SC_CAMERA, "Fade {direction} {colour} over {frames}",
        {{"direction", 1, FT_ENUM, "0=in from|1=out to"}, {"colour", 2, FT_ENUM, "0=nothing|1=black|2=white|3=red"}, {"frames", 4, FT_FRAMES}});
    // ---- messages (g2 0x21; g5 0x3E; g7 0x6B)
    add(0x21, "Message", SC_MESSAGE, "Message {text}{pause}", {{"text", 4, FT_TEXT}, {"pause", 2, FT_ENUM16, "0=|65535= (game paused)"}});
    add(0x3E, "Voice line", SC_MESSAGE, "Say voice line {clip}", {{"clip", 2, FT_U16}});
    add(0x6B, "Map destination", SC_MESSAGE, "Show the map with the next destination", {{"room", 2, FT_U8}, {"stage", 3, FT_U8}});
    // ---- zones (g3 0x28 by type, 0x2D; g7 0x65; g5 0x3F / 0x41; g7 0x6C)
    add(0x28, "Door", SC_ZONE, "Zone {zone}: door to {room}{lock}",
        {{"zone", 1, FT_U8}, {"room", 28, FT_ROOMCODE}, {"place", 30, FT_XYZ}, {"turn", 36, FT_TURN}, {"door scene", 38, FT_U8}, {"lock", 40, FT_LOCK},
         {"unlock flag", 39, FT_U8}, {"save prompt", 41, FT_ENUM, "0=no|1=yes"}, {"area", 4, FT_QUAD}, {"floor", 22, FT_U8},
         {"flags", 21, FT_BITS, kZoneFlags}},
        false, false, 0);
    add(0x28, "Message zone", SC_ZONE, "Zone {zone}: message {text}",
        {{"zone", 1, FT_U8}, {"text", 28, FT_TEXT}, {"area", 4, FT_QUAD}, {"floor", 22, FT_U8}, {"flags", 21, FT_BITS, kZoneFlags}}, false, false, 1);
    add(0x28, "Flag zone", SC_ZONE, "Zone {zone}: flag {flag} = {value}",
        {{"zone", 1, FT_U8}, {"flag", 28, FT_FLAG}, {"value", 30, FT_U8}, {"area", 4, FT_QUAD}, {"floor", 22, FT_U8}, {"flags", 21, FT_BITS, kZoneFlags}},
        false, false, 2);
    add(0x28, "Event zone", SC_ZONE, "Zone {zone}: run {sub}",
        {{"zone", 1, FT_U8}, {"sub", 30, FT_SUB}, {"slot", 28, FT_U8}, {"area", 4, FT_QUAD}, {"floor", 22, FT_U8}, {"flags", 21, FT_BITS, kZoneFlags}},
        false, false, 3);
    add(0x28, "Item zone", SC_ZONE, "Zone {zone}: {item} x{count}",
        {{"zone", 1, FT_U8}, {"item", 28, FT_ITEM}, {"count", 30, FT_U16}, {"taken flag", 32, FT_U16}, {"model piece", 34, FT_U8},
         {"pickup", 35, FT_ENUM, "0=normal|128=special"}, {"model", 36, FT_PTR}, {"hidden", 40, FT_U32}, {"area", 4, FT_QUAD}, {"floor", 22, FT_U8},
         {"flags", 21, FT_BITS, kZoneFlags}},
        false, false, 4);
    add(0x28, "Examine zone", SC_ZONE, "Zone {zone}: examine {text}",
        {{"zone", 1, FT_U8}, {"text", 40, FT_TEXT}, {"flag", 44, FT_FLAG}, {"area", 4, FT_QUAD}, {"floor", 22, FT_U8}, {"flags", 21, FT_BITS, kZoneFlags}},
        false, false, 7);
    add(0x28, "Box zone", SC_ZONE, "Zone {zone}: Emergency Box {box}",
        {{"zone", 1, FT_U8}, {"box", 28, FT_U8}, {"plugs", 29, FT_U8}, {"area", 4, FT_QUAD}, {"floor", 22, FT_U8}, {"flags", 21, FT_BITS, kZoneFlags}},
        false, false, 8);
    add(0x28, "Fingerprint zone", SC_ZONE, "Zone {zone}: fingerprint {id}",
        {{"zone", 1, FT_U8}, {"id", 28, FT_U8}, {"area", 4, FT_QUAD}, {"floor", 22, FT_U8}, {"flags", 21, FT_BITS, kZoneFlags}}, false, false, 10);
    add(0x28, "Switch zone", SC_ZONE, "Zone {zone}: switch for flag {flag}",
        {{"zone", 1, FT_U8}, {"flag", 28, FT_FLAG}, {"card", 30, FT_ENUM, "0=no card|6=key card Lv.C|7=key card Lv.B|8=key card Lv.A"},
         {"needs flag 0/", 31, FT_U8}, {"area", 4, FT_QUAD}, {"floor", 22, FT_U8}, {"flags", 21, FT_BITS, kZoneFlags}},
        false, false, 11);
    add(0x2D, "Zone on / off", SC_ZONE, "Zone {zone} {value}", {{"zone", 1, FT_U8}, {"value", 3, FT_ENUM, "0=off|1=on"}});
    add(0x65, "Trigger zone", SC_ZONE, "Trigger zone {zone} now", {{"zone", 1, FT_U8}});
    add(0x3F, "Collision wall", SC_ZONE, "Collision wall {wall}: {set}", {{"wall", 1, FT_U8}, {"set", 3, FT_ENUM, "1=off|0=on"}, {"mask", 4, FT_U16}});
    add(0x6C, "Floor piece", SC_ZONE, "Floor piece {piece}: {set}", {{"piece", 1, FT_U8}, {"set", 3, FT_ENUM, "1=off|0=on"}, {"mask", 2, FT_U8}});
    add(0x41, "Remove floor", SC_ZONE, "Remove floor {floor} of list {list}", {{"list", 1, FT_U8}, {"floor", 2, FT_U8}});
    // ---- sound and music (g3 0x2C, g7, g8 0x55: 0 at a point (func_8005C9A4 with a position), 1 volume and pan
    // (func_8005CA5C), 2 plain)
    add(0x2C, "Sound", SC_SOUND, "{mode} sound {id}", {{"id", 1, FT_U8}, {"mode", 2, FT_ENUM, "1=Play|0=Stop"}});
    add(0x55, "Sound at a point", SC_SOUND, "Play sound {id} at {place}", {{"id", 1, FT_U8}, {"place", 4, FT_XYZ}}, false, false, 0);
    add(0x55, "Sound with volume", SC_SOUND, "Play sound {id}, volume {volume}, pan {pan}",
        {{"id", 1, FT_U8}, {"volume", 4, FT_U16}, {"pan (64 = middle)", 6, FT_U16}}, false, false, 1);
    add(0x55, "Sound everywhere", SC_SOUND, "Play sound {id}", {{"id", 1, FT_U8}}, false, false, 2);
    add(0x63, "Music", SC_SOUND, "{mode} music channel {channel}",
        {{"mode", 1, FT_ENUM, "0=Stop|1=Play|2=Fade in|3=Fade out"}, {"channel", 3, FT_U8}, {"bank", 2, FT_U8}, {"speed", 4, FT_U16},
         {"flags", 6, FT_U16}});
    add(0x6A, "Room music", SC_SOUND, "Music of room {room} = {music}", {{"room", 2, FT_ROOMCODE}, {"music", 4, FT_U16}});
    add(0x67, "Room music table", SC_SOUND, "Music of all rooms from a table", {}, false, true);
    add(0x64, "Footsteps", SC_SOUND, "Footsteps of floor area {area}: material {material}", {{"area", 1, FT_U8}, {"material", 2, FT_U8}});
    // ---- room (g2 0x23; g4 0x3C; g5 0x42; g6 0x58; g7)
    add(0x23, "Room piece", SC_ROOM, "Piece {slot}: mesh {group} at {place}",
        {{"slot", 1, FT_U8}, {"group", 8, FT_PTR}, {"place", 12, FT_XYZ}, {"turn", 20, FT_TURN}, {"tilt", 18, FT_TURN}, {"roll", 22, FT_TURN},
         {"type", 2, FT_U8}, {"sort", 3, FT_U8}, {"order", 4, FT_U16}},
        false, true);
    add(0x42, "Effect", SC_ROOM, "Effect type {type} in slot {slot} at {place}",
        {{"type", 2, FT_U8}, {"slot", 1, FT_U8}, {"place", 4, FT_XYZ}, {"variant", 3, FT_U8}, {"attach", 10, FT_ENUM, "0=none|2=actor part|3=piece"},
         {"attach to", 11, FT_U8}, {"param", 12, FT_U16}, {"param 2", 16, FT_PTR}});
    add(0x58, "Mechanism", SC_ROOM, "Start mechanism type {type} in slot {slot}",
        {{"slot", 1, FT_U8}, {"type", 2, FT_U8}, {"param", 3, FT_U8}, {"param 4", 4, FT_U8}, {"param 5", 5, FT_U8}, {"param 6", 6, FT_U8},
         {"param 7", 7, FT_U8}});
    add(0x5F, "Light", SC_ROOM, "Light {light}: {mode} {value}", {{"light", 1, FT_U8}, {"mode", 2, FT_ENUM, "0=move to|1=colour"}, {"value", 4, FT_XYZ}});
    add(0x5E, "Actors drawn", SC_ROOM, "Draw actors inside the camera area and a quad", {{"areas", 1, FT_U8}, {"quad", 4, FT_QUAD}}, false, true);
    add(0x3C, "Draw order", SC_ROOM, "Draw piece {piece}: {mode}",
        {{"piece", 1, FT_U8}, {"mode", 2, FT_ENUM, "0=by nearest point|1=by farthest point|2=at fixed depth|3=at its origin|4=not drawn"},
         {"depth", 4, FT_U16}},
        false, true);
    add(0x6E, "Effect slots", SC_ROOM, "Keep effect slots 0..{count} for the script", {{"count", 1, FT_U8}}, false, true);
    add(0x6F, "Mechanism slots", SC_ROOM, "Keep mechanism slots 0..{count} for the script", {{"count", 1, FT_U8}}, false, true);
    add(0x60, "Depth sorting", SC_ROOM, "Depth sorting step {shift}", {{"shift", 1, FT_U8}}, false, true);
    add(0x69, "Stage setup", SC_ROOM, "Run the stage setup again (dinosaur roster)", {}, false, true);
    add(0x62, "Vibration", SC_OTHER, "Vibrate the controller", {{"pattern", 4, FT_PTR}});
    return t;
}

const std::vector<ScnOp>& table() {
    static const std::vector<ScnOp> t = make_table();
    return t;
}

// opcode -> its entries (several when told apart by a byte)
const std::map<int, std::vector<const ScnOp*>>& by_op() {
    static std::map<int, std::vector<const ScnOp*>> m = [] {
        std::map<int, std::vector<const ScnOp*>> r;
        for (auto& o : table()) r[o.op].push_back(&o);
        return r;
    }();
    return m;
}

std::string kind_text(int k) {
    const std::string n = enemy_kind_name(k);
    return n.empty() ? strfmt("kind %d", k) : n;
}

std::string frames_text(int f) {
    if (f % 30 == 0) return strfmt("%d s", f / 30);
    return strfmt("%d frames", f);
}

const char* act_modes(const ScnCtx& c) {
    if (c.sel_num == 10) return kActHuman;
    const int k = c.actor_kind ? c.actor_kind(c.sel_num) : -1;
    if (k == 2) return kActRaptor;
    if (k == 1) return kActHuman;
    return nullptr;
}

std::string item_text(int v) {
    if (v == 0xFF) return "a random item";
    const std::string n = item_name(v);
    return n.empty() ? strfmt("item %d", v) : n;
}

std::string target_text(const ScriptAtom& a) {
    for (auto& s : a.syms)
        if (s.at == 2 && s.type == 0) return s.tnew >= 0 ? strfmt("#%d", s.tnew) : strfmt("@%04X", (unsigned)s.target);
    return "?";
}

}  // namespace

const ScnOp* scn_op(const ScriptAtom& a) {
    if (a.kind != ScriptAtom::OP || a.b.empty()) return nullptr;
    auto it = by_op().find(a.b[0]);
    if (it == by_op().end()) return nullptr;
    for (const ScnOp* o : it->second) {
        if (o->sub_type < 0) return o;
        if ((size_t)o->sub_at < a.b.size() && a.b[o->sub_at] == o->sub_type) return o;
    }
    return nullptr;
}

std::vector<const ScnOp*> scn_ops_of(ScnCat c) {
    std::vector<const ScnOp*> r;
    for (auto& o : table()) if (o.cat == c && !o.hidden) r.push_back(&o);
    return r;
}

bool scn_is_cond_op(int op) { return op == 0x14 || op == 0x15 || op == 0x26 || op == 0x29 || op == 0x49 || op == 0x52 || op == 0x5C; }
bool scn_is_compare_op(int op) { return op == 0x15 || op == 0x29; }

const std::vector<std::pair<int, const char*>>& scn_compare_ops() {
    // the jump of a COMPARE is taken on: 1 equal, 2 greater, 3 greater or equal, 4 less, 5 less or equal, 6 not equal;
    // the IF block runs when it is not
    static const std::vector<std::pair<int, const char*>> v = {{6, "=="}, {1, "!="}, {5, ">"}, {4, ">="}, {3, "<"}, {2, "<="}};
    return v;
}

int scn_get(const ScriptAtom& a, const ScnField& f) {
    const size_t n = a.b.size();
    auto u8 = [&](int k) { return (size_t)k < n ? (int)a.b[k] : 0; };
    auto u16v = [&](int k) { return (size_t)k + 2 <= n ? (int)le16(&a.b[k]) : 0; };
    switch (f.t) {
    case FT_U8: case FT_KIND: case FT_SELCAT: case FT_FIELD: case FT_ENUM: case FT_BITS: case FT_ITEM: case FT_LOCK: return u8(f.at);
    case FT_S8: return (int)(int8_t)u8(f.at);
    case FT_U16: case FT_FRAMES: case FT_ROOMCODE: case FT_ENUM16: return u16v(f.at);
    case FT_S16: case FT_SUB: case FT_TURN: case FT_XYZ: return (int)(int16_t)u16v(f.at);
    case FT_U32: case FT_S32: case FT_TEXT: case FT_PTR: return (size_t)f.at + 4 <= n ? (int)le32(&a.b[f.at]) : 0;
    case FT_FLAG: return u8(f.at) * 1000 + u8(f.at + 1);
    case FT_HEX: case FT_QUAD: return 0;
    }
    return 0;
}

void scn_set(ScriptAtom& a, const ScnField& f, int v) {
    const size_t n = a.b.size();
    auto p8 = [&](int k, int x) { if ((size_t)k < n) a.b[k] = (uint8_t)x; };
    auto p16 = [&](int k, int x) { if ((size_t)k + 2 <= n) put16(&a.b[k], (uint16_t)x); };
    switch (f.t) {
    case FT_U8: case FT_KIND: case FT_S8: case FT_SELCAT: case FT_FIELD: case FT_ENUM: case FT_BITS: case FT_ITEM: case FT_LOCK: p8(f.at, v); break;
    case FT_U16: case FT_FRAMES: case FT_ROOMCODE: case FT_S16: case FT_SUB: case FT_TURN: case FT_XYZ: case FT_ENUM16: p16(f.at, v); break;
    case FT_U32: case FT_S32: case FT_PTR: if ((size_t)f.at + 4 <= n) put32(&a.b[f.at], (uint32_t)v); break;
    case FT_FLAG: p8(f.at, v / 1000); p8(f.at + 1, v % 1000); break;
    case FT_TEXT: case FT_HEX: case FT_QUAD: break;
    }
}

std::string scn_phrase(const ScriptAtom& a, const ScnCtx& c) {
    const ScnOp* o = scn_op(a);
    if (!o) {
        // not decoded yet: the listing's name if it has one, else the opcode
        Bytes one = a.b;
        one.resize(std::max<size_t>(one.size(), 48), 0);
        auto l = script_listing(one, 0, 0, (uint32_t)a.b.size());
        std::string t = l.empty() ? std::string() : l[0].text;
        if (t.size() >= 4 && t[2] == ' ' && t[3] == ' ') t = strfmt("Command %02X", a.b.empty() ? 0 : a.b[0]);
        return t;
    }
    std::string out;
    for (const char* p = o->phrase; *p;) {
        if (*p != '{') { out += *p++; continue; }
        const char* e = strchr(p, '}');
        if (!e) { out += p; break; }
        const std::string key(p + 1, e);
        p = e + 1;
        if (key == "object") {   // the object picked before; SELECT itself names the one it picks
            out += a.b[0] == 0x22 && a.b.size() >= 3 ? scn_object_name(a.b[1], a.b[2], c) : scn_object_name(c.sel_cat, c.sel_num, c);
            continue;
        }
        const ScnField* f = nullptr;
        for (auto& x : o->fields) if (key == x.name) f = &x;
        if (!f) continue;
        const int v = scn_get(a, *f);
        switch (f->t) {
        case FT_FRAMES: out += frames_text(v); break;
        case FT_FLAG: out += strfmt("%d/%d", v / 1000, v % 1000); break;
        case FT_SUB: out += c.sub ? c.sub(v) : strfmt("subroutine %d", v); break;
        case FT_ROOMCODE: out += room_code(v); break;
        case FT_KIND: out += kind_text(v); break;
        case FT_ITEM: out += item_text(v); break;
        case FT_LOCK: if (v) out += " (" + scn_lock_name(v) + ")"; break;
        case FT_TURN: out += strfmt("%d deg", (int)lround(v * 360.0 / 4096.0)); break;
        case FT_XYZ:
            out += strfmt("%d, %d, %d", (int)(int16_t)le16(&a.b[f->at]), (int)(int16_t)le16(&a.b[f->at + 2]), (int)(int16_t)le16(&a.b[f->at + 4]));
            break;
        case FT_SELCAT: out += scn_object_name(v, a.b.size() > 2 ? a.b[2] : 0, c); break;
        case FT_FIELD: {
            const std::string n = scn_field_name(c.sel_cat, v);
            out += n.empty() ? strfmt("field %d", v) : n;
            break;
        }
        case FT_ENUM:
        case FT_ENUM16: {
            const char* names = f->names;
            if (a.b[0] == 0x3A && !strcmp(f->name, "mode")) names = act_modes(c);
            bool found = false;
            for (auto& kv : scn_names(names))
                if (kv.first == v) { out += kv.second; found = true; break; }
            if (!found) out += strfmt("%s %d", f->name, v);
            break;
        }
        case FT_BITS: {
            for (auto& kv : scn_names(f->names)) if (v & kv.first) out += ", " + kv.second;
            break;
        }
        case FT_PTR: out += (uint32_t)v >= 0x80000000u ? strfmt("%08X", (uint32_t)v) : strfmt("#%d", v); break;
        case FT_TEXT: {
            const ScriptSym* sy = nullptr;
            for (auto& s : a.syms) if (s.at == f->at && s.type == 2) sy = &s;
            out += sy && c.text ? c.text(*sy) : std::string("(text)");
            break;
        }
        default: out += std::to_string(v); break;
        }
    }
    // the fixed camera of mode 9 / the glide of mode 0 belong to the line
    if (a.b[0] == 0x30 && a.b.size() >= 3) {
        if (a.b[1] == 9) out += strfmt(" %d", a.b[2]);
        else if (a.b[1] == 0 && a.b[2]) out += " (glide)";
    }
    if (a.b[0] == 0x2E && a.b.size() >= 20 && le16(&a.b[16]) == 0 && le16(&a.b[18]) == 0) out += " (cut)";
    return out;
}

// the constant of a comparison in the words of its field: a room, an item, a kind
static std::string cmp_value(const ScriptAtom& a, const ScnCtx& c) {
    const int v = a.b.size() >= 8 ? (int)le32(&a.b[4]) : 0, f = a.b.size() >= 3 ? a.b[2] : -1;
    if (c.sel_cat == 0 && f == 7 && v > 0 && v < 0x1000) return room_code(v);
    if (c.sel_cat == 0 && f == 8 && v > 0 && v < 0x70 && !item_name(v).empty()) return item_name(v);
    if (c.sel_cat == 2 && f == 1 && !enemy_kind_name(v).empty()) return enemy_kind_name(v);
    return std::to_string(v);
}

std::string scn_line_phrase(const ScriptAtom& a, const ScnCtx& c) {
    const int op = a.kind == ScriptAtom::OP && !a.b.empty() ? a.b[0] : -1;
    if (!scn_op(a) || !scn_is_cond_op(op)) return scn_phrase(a, c);
    if (scn_is_compare_op(op)) return "Compare " + scn_phrase(a, c) + " with " + cmp_value(a, c);
    return "Check: " + scn_phrase(a, c);
}

std::string scn_if_phrase(const std::vector<const ScriptAtom*>& cond, const ScriptAtom& jump, const ScnCtx& c) {
    const int bn = jump.b.size() >= 2 ? jump.b[1] : 0;
    if (cond.empty()) {
        // the condition word was set further up (a chain of jumps on one test)
        return bn == 6 ? "If the last check is true" : bn == 1 ? "If the last check is false" : strfmt("If the last check (bit %d off)", bn);
    }
    const ScriptAtom& a = *cond.back();
    const int op = a.b.empty() ? 0 : a.b[0];
    const std::string p = scn_phrase(a, c);
    if (scn_is_compare_op(op)) {
        for (auto& kv : scn_compare_ops())
            if (kv.first == bn) return "If " + p + " " + kv.second + " " + cmp_value(a, c);
        return strfmt("If %s (bit %d off)", p.c_str(), bn);
    }
    // a test leaves "true" (2) or "false" (64): jump 6 skips the block on false, jump 1 on true
    if (bn != 1 && bn != 6) return strfmt("If %s (bit %d off)", p.c_str(), bn);
    const bool yes = bn == 6;
    switch (op) {
    case 0x26:
    case 0x14: return "If " + p + (yes ? " is set" : " is not set");
    case 0x52: {
        const std::string it = item_text(a.b.size() >= 2 ? a.b[1] : 0);
        return yes ? "If Regina has " + it : "If Regina does not have " + it;
    }
    case 0x5C: return strfmt(yes ? "If a live dinosaur is in room %d" : "If no live dinosaur is in room %d", a.b.size() >= 2 ? a.b[1] : 0);
    default: return yes ? "If " + p : "If not: " + p;
    }
}

std::string scn_jump_phrase(const ScriptAtom& a, const ScnCtx& c) {
    if (a.b.empty()) return "Jump";
    const uint8_t op = a.b[0];
    if (op == 0x09) return strfmt("Loop %d times, to the end at %s", a.b.size() >= 6 ? (int)le16(&a.b[4]) : 0, target_text(a).c_str());
    if (op == 0x0A) return "Loop again";
    if (op == 0x17) return "Loop (count from a value)";
    if (op == 0x0B) return "Mark";
    const int bn = op == 0x0E && a.b.size() >= 2 ? a.b[1] : 0;
    const std::string to = "go to " + target_text(a);
    if (bn == 0) return "Then " + to;
    if (scn_is_compare_op(c.last_test)) {
        static const char* w[7] = {"", "equal", "greater", "greater or equal", "less", "less or equal", "not equal"};
        return bn <= 6 ? strfmt("If %s, %s", w[bn], to.c_str()) : "Jump (never)";
    }
    if (c.last_test == 0x26 || c.last_test == 0x14) return bn == 1 ? "If the flag is set, " + to : bn == 6 ? "If the flag is not set, " + to : "Jump (never)";
    return bn == 1 ? "If true, " + to : bn == 6 ? "If false, " + to : "Jump (never)";
}

ScriptAtom scn_new_atom(uint8_t op) {
    for (auto& o : table())
        if (o.op == op) return scn_new_atom(o);
    ScnOp o;
    o.op = op;
    return scn_new_atom(o);
}

ScriptAtom scn_new_atom(const ScnOp& o) {
    const uint8_t op = o.op;
    ScriptAtom a;
    a.kind = ScriptAtom::OP;
    uint8_t probe[64] = {op};
    if (o.sub_type >= 0 && o.sub_at > 0) probe[o.sub_at] = (uint8_t)o.sub_type;   // 0x28: the size goes by the type
    int n = script_op_size(probe, sizeof probe);
    if (n <= 0) n = 4;
    a.b.assign((size_t)n, 0);
    a.b[0] = op;
    if (o.sub_type >= 0 && o.sub_at > 0 && o.sub_at < n) a.b[o.sub_at] = (uint8_t)o.sub_type;
    switch (op) {
    case 0x02: put16(&a.b[2], 30); break;        // one second
    case 0x22: a.b[1] = 2; a.b[2] = 10; break;   // Regina
    case 0x29: a.b[2] = 21; break;               // HP of the selected actor
    case 0x2C: a.b[2] = 1; break;                // play
    case 0x63: a.b[1] = 1; a.b[2] = 5; a.b[3] = 1; break;   // play channel 1 with the event bank
    case 0x55:
        if (o.sub_type == 1) { put16(&a.b[4], 100); put16(&a.b[6], 64); }   // loud, in the middle
        break;
    case 0x57: a.b[2] = o.sub_type == 0 ? 0 : 1; break;
    case 0x28: {
        const int ty = o.sub_type < 0 ? 1 : o.sub_type;
        a.b[20] = (uint8_t)ty;   // the zone checker's copy of the type
        // a square of 1 m around the room's origin (the caller moves it where the room has zones)
        static const int q[8] = {-500, -500, 500, -500, 500, 500, -500, 500};
        for (int k = 0; k < 8; k++) put16(&a.b[4 + k * 2], (uint16_t)(int16_t)q[k]);
        a.b[21] = (ty == 2 || ty == 3) ? 0 : 2;   // walking in sets a flag / runs an event; the others want the action button
        switch (ty) {
        case 0: a.b[40] = 0xFF; break;            // "It is locked." until a target room is chosen
        case 1: put16(&a.b[32], 0xFFFF); break;   // as 486 of the game's message zones
        case 2: a.b[28] = 3; a.b[30] = 1; break;  // a room signal flag 3/0
        case 3: a.b[28] = 8; break;               // task slot (the caller picks a free one)
        case 4: put16(&a.b[30], 1); a.b[34] = 0xFF; break;   // one, no model piece; the caller picks the item
        case 7: put16(&a.b[32], 24); put16(&a.b[34], 13); a.b[44] = 0xFF; break;   // window as the game's, no flag
        case 10: a.b[28] = 1; break;
        case 11: a.b[28] = 3; break;
        default: break;
        }
        break;
    }
    default: break;
    }
    return a;
}

std::vector<ScnPoint> scn_points(const ScriptAtom& a) {
    std::vector<ScnPoint> r;
    if (a.kind != ScriptAtom::OP || a.b.empty()) return r;
    const size_t n = a.b.size();
    auto pt = [&](const char* name, int at, int turn_at = -1) { if ((size_t)at + 6 <= n) r.push_back({name, at, turn_at}); };
    switch (a.b[0]) {
    case 0x20: pt("place", 8, 14); break;
    case 0x5B: pt("place", 4, 10); pt("camera target", 28); pt("camera eye", 36); break;
    case 0x36: pt("place", 2); break;
    case 0x3A: {   // modes without a point: hold, turn by, stop, turn around, special
        const int m = n > 2 ? a.b[2] : 0;
        if (m != 0 && m != 4 && m != 5 && m != 11 && m != 14) pt("point", 4);
        break;
    }
    case 0x3D: if (n > 1 && a.b[1] == 6) pt("target", 4); break;          // look at a point
    case 0x42: if (n > 10 && a.b[10] != 2 && a.b[10] != 3) pt("place", 4); break;   // attached: an offset, not a place
    case 0x55: if (n > 2 && a.b[2] == 0) pt("place", 4); break;           // sound at a point
    case 0x5F: if (n > 2 && a.b[2] == 0) pt("value", 4); break;           // light moved to (mode 1 is a colour)
    case 0x2E: pt("eye", 8); pt("target", 2); break;
    case 0x4C: pt("target", 2); pt("eye", 18); break;
    default: break;
    }
    return r;
}

int scn_quad_at(const ScriptAtom& a) {
    return a.kind == ScriptAtom::OP && a.b.size() >= 24 && a.b[0] == 0x28 ? 4 : -1;
}

float scn_quad_y(const ScriptAtom& a) { return a.b.size() > 22 ? -425.0f * a.b[22] : 0.0f; }

}  // namespace dc
