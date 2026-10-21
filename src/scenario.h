// 10.10.2026 (player: "room scripts as readable events: condition -> actions; add, remove, reorder, chains; direct the
// game"): the room script as a tree. Each subroutine of the directory is an event; its commands are nodes; a condition
// and the forward jump over a block (TESTFLAG + BRANCH) is an IF node with its block (and an ELSE block when the block ends
// with a jump over another one); FOR ... NEXT is a LOOP node. Texts and data inside the code stay where they are (DATA
// nodes). Flattening the tree gives the atom list back; the jumps of IF / LOOP nodes are pointed at whatever follows their
// block, by identity (old place or new label), so commands can be added, removed and moved between blocks.
// Untouched, parse + flatten gives the game's bytes (scripttest checks every room).
#pragma once
#include "script_edit.h"

namespace dc {

struct ScnNode {
    enum Kind : uint8_t { CMD, DATA, IF, LOOP };
    Kind k = CMD;
    int atom = -1;               // CMD/DATA: the atom; IF: its conditional jump; LOOP: its FOR
    int atom2 = -1;              // IF with ELSE: the jump over the ELSE block; LOOP: its NEXT
    std::vector<int> cond;       // IF: the commands that set the condition (just before the jump)
    std::vector<ScnNode> a, b;   // IF: THEN / ELSE blocks; LOOP: body (a)
};

struct ScnRegion {               // a stretch of the atom list: from a subroutine entry to the next one
    std::vector<int> subs;       // subroutines that start here (empty: what lies before the first entry)
    int first = 0, end = 0;      // atoms [first, end)
    std::vector<ScnNode> nodes;
};

struct Scenario {
    std::vector<ScnRegion> regions;   // in atom-list order; regions[0] starts with the directory
    std::vector<int> dir;             // subroutine -> atom (script_dir_atoms)
};

// is this command one that only sets the condition word for the next jump (TESTFLAG and the like)
bool scn_is_cond(const ScriptAtom& a);

// the tree of an atom list
Scenario scn_parse(const ScriptImage& orig, const std::vector<ScriptAtom>& atoms);
// back to an atom list: new order; the jumps of IF / LOOP nodes point at what follows their blocks (atoms get labels if new)
bool scn_flatten(const Scenario& s, std::vector<ScriptAtom>& atoms, std::string& err);
// the atoms of the tree in the order flattening gives (indices into the atom list)
std::vector<int> scn_order(const Scenario& s);
// before nodes are taken out of the tree: references to the new atoms of `gone` (jumps, the directory) are moved to the
// command that follows them in the order of `s` (old places need nothing: the assembler maps them to what follows)
void scn_retarget_gone(const Scenario& s, std::vector<ScriptAtom>& atoms, const std::vector<int>& gone);

// ---- edits of an event (region): lines are found by the key of their atom ("@XXXX" old place, "#N" new;
// "else:" + the IF's key = its ELSE line)
std::string scn_key(const ScriptAtom& a);
// where a new line goes: after the picked one; into a picked IF / REPEAT (first line of the block) or ELSE line (first of
// ELSE); nothing picked: before the event's last End
void scn_insert_point(ScnRegion& g, const std::vector<ScriptAtom>& atoms, const std::string& key, std::vector<ScnNode>*& list, int& pos);
// the event's last End stays (without it the game would run on into the next event's commands)
bool scn_is_final_end(const ScnRegion& g, const std::vector<ScriptAtom>& atoms, const std::string& key);
// remove a line (an IF / REPEAT with its blocks; an ELSE line: the ELSE block); references to new atoms move on
bool scn_remove(Scenario& s, std::vector<ScriptAtom>& atoms, int region, const std::string& key);
// move a line up (-1) / down (+1): among neighbours, into a neighbouring block, out of its block at the edge
bool scn_move(ScnRegion& g, const std::vector<ScriptAtom>& atoms, const std::string& key, int dir);

}  // namespace dc
