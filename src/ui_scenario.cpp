// 10.10.2026 (player: "room scripts as readable events: condition -> actions; add, remove, reorder, chains; everything
// that changes is seen and edited in the viewport; as simple as possible"): the scenario of a room script next to the 3D
// view. Events = subroutines (room start, camera views, zones, started or run by other events); commands = readable lines
// (scn_ops.h), IF / ELSE / REPEAT blocks indented; the picked command's fields below; markers of the event in the 3D view.
// The model is the script text (script.txt): every edit goes tree -> atoms -> text, saved and built as before.
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "i18n.h"
#include "placement.h"
#include "scn_ops.h"
#include "scn_scenes.h"
#include "species.h"
#include "ui.h"
#include "ui_names.h"

namespace dc {

namespace {

std::string akey(const ScriptAtom& a) { return a.old >= 0 ? strfmt("@%04X", (unsigned)a.old) : strfmt("#%d", a.nid); }

bool is_jump(const ScriptAtom& a) {
    if (a.kind != ScriptAtom::OP || a.b.empty()) return false;
    const uint8_t op = a.b[0];
    return op == 0x0C || op == 0x0E || op == 0x0B || op == 0x17 || op == 0x09 || op == 0x0A;
}

struct Row {
    int depth = 0;
    bool else_line = false;            // the ELSE line of an IF
    std::vector<ScnNode>* list = nullptr;
    int idx = 0;
    ScnNode& node() const { return (*list)[idx]; }
};

// 14.10.2026 (player: "a constructor a non-programmer understands - and all of it there when wanted"): a scene block
// (scn_scenes.h) is one line with plain fields; More opens its commands as lines like any others
bool scene_closed(const RoomEdit& E, const ScnNode& n) {
    ScnScene t;
    return n.k == ScnNode::IF && n.atom >= 0 && !E.scn_open.count(akey(E.scn_atoms[n.atom])) && scn_scene_detect(E.scn_atoms, n, t);
}

void rows_of(const RoomEdit& E, std::vector<ScnNode>& ns, int depth, std::vector<Row>& out) {
    for (int i = 0; i < (int)ns.size(); i++) {
        ScnNode& n = ns[i];
        if (n.k == ScnNode::DATA) continue;
        out.push_back({depth, false, &ns, i});
        if (n.k == ScnNode::IF) {
            if (scene_closed(E, n)) continue;
            rows_of(E, n.a, depth + 1, out);
            if (n.atom2 >= 0) {
                out.push_back({depth, true, &ns, i});
                rows_of(E, n.b, depth + 1, out);
            }
        } else if (n.k == ScnNode::LOOP) {
            rows_of(E, n.a, depth + 1, out);
        }
    }
}

// every atom of a node and its blocks
void atoms_of(const ScnNode& n, std::vector<int>& out) {
    if (n.atom >= 0) out.push_back(n.atom);
    if (n.atom2 >= 0) out.push_back(n.atom2);
    for (int c : n.cond) out.push_back(c);
    for (auto& x : n.a) atoms_of(x, out);
    for (auto& x : n.b) atoms_of(x, out);
}

// 14.10.2026: markers of an event: the lines inside a closed scene block stay out, the block's own points (the raptor,
// where it lands, the camera) hang on its If line
void scene_marks(const RoomEdit& E, const std::vector<ScnNode>& ns, std::set<int>& hidden, std::map<int, std::vector<ScnPoint>>& pts) {
    for (auto& n : ns) {
        if (scene_closed(E, n)) {
            std::vector<int> in;
            atoms_of(n, in);
            for (int i : in) if (i != n.atom) hidden.insert(i);
            pts[n.atom] = scn_scene_points(E.scn_atoms, n);
            continue;
        }
        scene_marks(E, n.a, hidden, pts);
        scene_marks(E, n.b, hidden, pts);
    }
}

std::string row_key(const RoomEdit& E, const Row& r) {
    const ScnNode& n = r.node();
    const std::string k = akey(E.scn_atoms[n.atom]);
    return r.else_line ? "else:" + k : k;
}

// where a node sits: its list and index, found by the key of its atom
bool find_node(std::vector<ScnNode>& ns, const RoomEdit& E, const std::string& key, std::vector<ScnNode>*& list, int& idx) {
    for (int i = 0; i < (int)ns.size(); i++) {
        ScnNode& n = ns[i];
        if (n.atom >= 0 && akey(E.scn_atoms[n.atom]) == key) { list = &ns; idx = i; return true; }
        if (find_node(n.a, E, key, list, idx) || find_node(n.b, E, key, list, idx)) return true;
    }
    return false;
}

// the node that holds a list (to move out of it), with the list it is in
bool parent_of(std::vector<ScnNode>& ns, const std::vector<ScnNode>* child, std::vector<ScnNode>*& plist, int& pidx) {
    for (int i = 0; i < (int)ns.size(); i++) {
        ScnNode& n = ns[i];
        if (&n.a == child || &n.b == child) { plist = &ns; pidx = i; return true; }
        if (parent_of(n.a, child, plist, pidx) || parent_of(n.b, child, plist, pidx)) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------- model
bool App::scenario_sync(Loaded& L) {
    RoomEdit& E = L.ed;
    if (!E.scn_im_ok) {
        std::string err;
        if (!script_decompose(L.c.blocks[L.c.room_block].data, L.c.room, E.scn_im, err)) { E.scn_err = err; return false; }
        E.scn_im_ok = true;
    }
    if (E.scn_from == E.script && (!E.scn_atoms.empty() || !E.scn_err.empty())) return E.scn_err.empty();
    std::vector<ScriptAtom> atoms;
    std::string err;
    E.scn_from = E.script;
    if (!script_from_text(E.scn_im, E.script, atoms, err)) {
        E.scn_err = err;
        E.scn_atoms.clear();
        E.scn = Scenario();
        return false;
    }
    E.scn_atoms = std::move(atoms);
    E.scn = scn_parse(E.scn_im, E.scn_atoms);
    E.scn_err.clear();
    scenario_refresh(L);
    scenario_view_sync(L);
    return true;
}

bool App::scenario_commit(Loaded& L) {
    RoomEdit& E = L.ed;
    scn_scenes_fix(E.scn, E.scn_atoms);   // 14.10.2026: inside a scene block the copies of the raptor's place follow it
    std::vector<ScriptAtom> atoms = E.scn_atoms;
    std::string err;
    Bytes sec;
    if (!scn_flatten(E.scn, atoms, err) || !script_assemble(E.scn_im, atoms, sec, err)) {
        E.msg = "Not applied: " + err;
        E.scn_from.clear();   // back to the text as it was
        scenario_sync(L);
        return false;
    }
    E.scn_atoms = std::move(atoms);
    E.scn = scn_parse(E.scn_im, E.scn_atoms);
    E.script = script_atoms_text(E.scn_im, E.scn_atoms, L.c.file->stem);
    E.scn_from = E.script;
    E.dirty_script = E.script != E.script_orig;
    E.msg.clear();
    scenario_refresh(L);
    scenario_view_sync(L);
    return true;
}

// the enemy models and the item models of the view where the scenario puts them. The rule is the one of the build: a command
// that differs from the game's wins over the placement panel (its values come back when the command is as in the game)
void App::scenario_view_sync(Loaded& L) {
    RoomEdit& E = L.ed;
    bool remesh = false;
    std::set<int64_t> seen;
    for (const ScriptAtom& a : E.scn_atoms) {
        if (a.kind != ScriptAtom::OP || a.old < 0 || a.b.size() < 16 || (a.b[0] != 0x20 && a.b[0] != 0x5B)) continue;
        const int i = E.scn_im.find((uint32_t)a.old);
        if (i < 0 || E.scn_im.atoms[i].b == a.b) continue;
        const int at = a.b[0] == 0x20 ? 8 : 4, rt = a.b[0] == 0x20 ? 14 : 10;
        const std::string op = strfmt("%02X", a.b[0]);
        for (json& e : E.enemies) {
            if (e.value("offset", -1) != a.old || e.value("op", std::string()) != op || !e.count("x")) continue;
            if (!E.scn_mirror.count(a.old)) E.scn_mirror[a.old] = {{"x", e["x"]}, {"y", e.value("y", 0)}, {"z", e["z"]}, {"rotation", e.value("rotation", 0)}};
            const int x = (int16_t)le16(&a.b[at]), y = (int16_t)le16(&a.b[at + 2]), z = (int16_t)le16(&a.b[at + 4]), r = (int16_t)le16(&a.b[rt]);
            if (e["x"] != x || e.value("y", 0) != y || e["z"] != z || e.value("rotation", 0) != r) {
                e["x"] = x; e["y"] = y; e["z"] = z; e["rotation"] = r;
                remesh = true;
            }
            seen.insert(a.old);
        }
    }
    for (auto it = E.scn_mirror.begin(); it != E.scn_mirror.end();) {   // back as in the game: the panel's values again
        if (seen.count(it->first)) { ++it; continue; }
        for (json& e : E.enemies)
            if (e.value("offset", -1) == it->first)
                for (auto& kv : it->second.items()) e[kv.key()] = kv.value();
        it = E.scn_mirror.erase(it);
        remesh = true;
    }
    std::string sig;   // item models are placed from the script text: rebuilt when an item zone changed
    for (const ScriptAtom& a : E.scn_atoms)
        if (a.kind == ScriptAtom::OP && a.b.size() >= 36 && a.b[0] == 0x28 && a.b[2] == 4) {
            sig += akey(a);
            for (uint8_t v : a.b) sig += (char)v;
        }
    if (sig != E.scn_view_sig) { remesh = remesh || !E.scn_view_sig.empty(); E.scn_view_sig = sig; }
    if (remesh && sel_.k == K_SCRIPT && L.c.file == sel_.f) v3d_.set_mesh(room_view_tris(L), room_vram(L), true);
}

// markers of the shown event: enemies, zones and the other places of its commands (the picked one white)
void App::scenario_refresh(Loaded& L) {
    RoomEdit& E = L.ed;
    std::vector<float> lines;
    if (E.scn_ev < 0 || E.scn_ev >= (int)E.scn.regions.size()) { v3d_.set_lines(lines); return; }
    std::vector<int> ids;
    for (auto& n : E.scn.regions[E.scn_ev].nodes) atoms_of(n, ids);
    std::sort(ids.begin(), ids.end());
    std::set<int> hidden;
    std::map<int, std::vector<ScnPoint>> spts;
    scene_marks(E, E.scn.regions[E.scn_ev].nodes, hidden, spts);
    json en = json::array(), zo = json::array();
    int sel_en = -1, sel_zo = -1;
    auto seg = [&](float x0, float y0, float z0, float x1, float y1, float z1, float r, float g, float b) {
        lines.insert(lines.end(), {x0, y0, z0, r, g, b, x1, y1, z1, r, g, b});
    };
    for (int i : ids) {
        const ScriptAtom& a = E.scn_atoms[i];
        if (a.kind != ScriptAtom::OP || a.b.empty() || hidden.count(i)) continue;
        const bool picked = akey(a) == E.scn_sel;
        const bool scene = spts.count(i) > 0;
        if (scn_quad_at(a) >= 0) {
            json q = json::array();
            for (int k = 0; k < 4; k++) q.push_back({(int)(int16_t)le16(&a.b[4 + k * 4]), (int)(int16_t)le16(&a.b[6 + k * 4])});
            if (picked) sel_zo = (int)zo.size();
            zo.push_back({{"zone", q}, {"floor", (int)a.b[22]}});
        }
        const std::vector<ScnPoint> pts = scene ? spts[i] : scn_points(a);
        float prev[3] = {0, 0, 0};
        for (size_t k = 0; k < pts.size(); k++) {
            const ScnPoint& p = pts[k];
            const ScriptAtom& h = p.atom >= 0 ? E.scn_atoms[p.atom] : a;   // a scene's points are in its lines
            const float x = (int16_t)le16(&h.b[p.at]), y = (int16_t)le16(&h.b[p.at + 2]), z = (int16_t)le16(&h.b[p.at + 4]);
            if (p.turn_at >= 0) {   // an actor stands there: the enemy marker (pole, ring, where it looks)
                if (picked) sel_en = (int)en.size();
                en.push_back({{"x", (int)x}, {"y", (int)y}, {"z", (int)z}, {"rotation", (int)(int16_t)le16(&h.b[p.turn_at])}});
            } else {                // a point: a cross and a short pole
                const float r = picked ? 1.0f : 0.3f, g = picked ? 1.0f : 0.9f, b = picked ? 1.0f : 0.95f;
                seg(x - 250, y, z, x + 250, y, z, r, g, b);
                seg(x, y, z - 250, x, y, z + 250, r, g, b);
                seg(x, y, z, x, y - 600, z, r, g, b);
            }
            // eye and target of a camera: the line it looks along
            if (k > 0 && (a.b[0] == 0x2E || a.b[0] == 0x4C || (a.b[0] == 0x5B && k == 2) || (scene && p.atom == pts[k - 1].atom)))
                seg(prev[0], prev[1], prev[2], x, y, z, 0.55f, 0.55f, 0.6f);
            prev[0] = x; prev[1] = y; prev[2] = z;
        }
    }
    placement_overlay_sel(en, 0, sel_en, lines);
    placement_overlay_sel(zo, 3, sel_zo, lines);
    v3d_.set_lines(lines);
}

// ---------------------------------------------------------------- the panel
void App::scenario_panel(Loaded& L) {
    RoomEdit& E = L.ed;
    // save / as in the game / text
    {
        const bool dirty = E.dirty_script || E.dirty_texts;
        const bool have = fs::exists(room_mods_dir(*L.c.file) / "script.txt");
        ImGui::BeginDisabled(job_ != nullptr);
        if (ImGui::Button(dirty ? "Save to mods *" : "Save to mods")) {
            if (E.dirty_texts) save_edit(L, 0);
            save_edit(L, 3);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!have && !dirty);
        if (ImGui::Button("As in the game")) { revert_edit(L, 3); E.scn_from.clear(); }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Text", &E.scn_text);
        if (!E.msg.empty()) ImGui::TextDisabled("%s", E.msg.c_str());
    }
    if (E.scn_text) {
        // the script as text (hex) - for what the list does not show yet
        ImGui::TextDisabled("Line = command (hex). Keep @XXXX. No @: a new command.");
        std::string& s = E.script;
        ImGui::InputTextMultiline(
            "##script", s.data(), s.capacity() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_CallbackResize,
            [](ImGuiInputTextCallbackData* d) {
                if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                    auto* str = (std::string*)d->UserData;
                    str->resize(d->BufTextLen);
                    d->Buf = str->data();
                }
                return 0;
            },
            &s);
        if (ImGui::IsItemEdited()) E.dirty_script = true;
        return;
    }
    if (!scenario_sync(L)) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", E.scn_err.c_str());
        ImGui::TextDisabled("Fix it in Text.");
        return;
    }
    // names: subroutines by what starts them, texts by their first words
    std::map<int, std::string> sub_title;
    {
        std::set<int> cam, forked, called;
        for (size_t k = 0; k < L.c.room.cameras.size(); k++) cam.insert(L.c.room.cameras[k].raw[2]);
        for (auto& a : E.scn_atoms) {
            if (a.kind != ScriptAtom::OP || a.b.size() < 4) continue;
            if (a.b[0] == 0x01) forked.insert((int16_t)le16(&a.b[2]));
            if (a.b[0] == 0x0F) called.insert((int16_t)le16(&a.b[2]));
        }
        const std::vector<int> dir = script_dir_atoms(E.scn_im, E.scn_atoms);
        for (int k = 0; k < (int)dir.size(); k++) {
            std::string t;
            if (k == 0) t = "Room start";
            else if (cam.count(k)) {
                std::string cs;
                for (size_t c = 0; c < L.c.room.cameras.size(); c++)
                    if (L.c.room.cameras[c].raw[2] == k) cs += (cs.empty() ? "" : ", ") + std::to_string(c);
                t = "Camera " + cs;
            } else if (forked.count(k)) t = strfmt("Event %d", k);
            else if (called.count(k)) t = strfmt("Routine %d", k);
            else t = strfmt("Subroutine %d", k);
            sub_title[k] = t;
        }
        for (auto& g : E.scn.regions) {   // 14.10.2026: an event that is a studio scene - by its name
            if (g.subs.empty()) continue;
            for (auto& n : g.nodes) {
                ScnScene sc;
                if (!scn_scene_detect(E.scn_atoms, n, sc)) continue;
                for (int k : g.subs) sub_title[k] = scn_scene_name(sc.kind);
                break;
            }
        }
    }
    // actor kinds by slot, what a piece / effect / mechanism is (from the room's own commands)
    ScnRoomNames rn;
    scn_room_names(E.scn_atoms, rn);
    ScnCtx nm;
    scn_ctx_bind(nm, rn);
    nm.sub = [&](int k) { auto it = sub_title.find(k); return it == sub_title.end() ? strfmt("subroutine %d", k) : it->second; };
    nm.text = [&](const ScriptSym& sy) { return script_text_quote(E.scn_atoms, sy, 48); };
    // events
    const float h_all = ImGui::GetContentRegionAvail().y;
    std::vector<int> evs;
    for (int r = 0; r < (int)E.scn.regions.size(); r++) if (!E.scn.regions[r].subs.empty()) evs.push_back(r);
    if (E.scn_ev < 0 || std::find(evs.begin(), evs.end(), E.scn_ev) == evs.end()) { E.scn_ev = evs.empty() ? -1 : evs[0]; scenario_refresh(L); }
    if (scn_sub_req_ >= 0) {
        for (int r : evs)
            for (int k : E.scn.regions[r].subs)
                if (k == scn_sub_req_) E.scn_ev = r;
        scn_sub_req_ = -1;
        if (!scn_sel_req_.empty()) { E.scn_sel = scn_sel_req_; scn_sel_req_.clear(); }
        scenario_refresh(L);
    }
    ImGui::BeginChild("events", ImVec2(-1, h_all * 0.28f), ImGuiChildFlags_Borders);
    for (int r : evs) {
        const ScnRegion& g = E.scn.regions[r];
        std::string t;
        for (int k : g.subs) t += (t.empty() ? "" : ", ") + sub_title[k];
        std::vector<Row> rows;
        rows_of(E, E.scn.regions[r].nodes, 0, rows);
        const bool tech = t.rfind("Camera", 0) == 0;   // camera views mostly put room pieces: dimmer
        if (tech) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushID(r);
        if (ImGui::Selectable(t.c_str(), E.scn_ev == r)) { E.scn_ev = r; E.scn_sel.clear(); scenario_refresh(L); }
        ImGui::SameLine(ImGui::GetWindowWidth() - 44);
        ImGui::TextDisabled("%d", (int)rows.size());
        ImGui::PopID();
        if (tech) ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    if (E.scn_ev < 0) return;
    ScnRegion& G = E.scn.regions[E.scn_ev];
    std::vector<Row> rows;
    rows_of(E, G.nodes, 0, rows);
    int cur = -1;
    for (int i = 0; i < (int)rows.size(); i++) if (row_key(E, rows[i]) == E.scn_sel) cur = i;
    // the object each line acts on: SELECT of this task, followed in the order of the lines (a new task starts with 0, 0)
    std::vector<std::pair<int, int>> row_sel(rows.size());
    std::vector<int> row_test(rows.size(), -1);   // the last condition command before the line (raw jumps read it)
    {
        int sc = 0, sn = 0, lt = -1;
        for (int i = 0; i < (int)rows.size(); i++) {
            row_sel[i] = {sc, sn};
            row_test[i] = lt;
            const ScnNode& n = rows[i].node();
            if (!rows[i].else_line && n.k == ScnNode::CMD) {
                const ScriptAtom& a = E.scn_atoms[n.atom];
                if (a.kind == ScriptAtom::OP && a.b.size() >= 4 && a.b[0] == 0x22) { sc = a.b[1]; sn = a.b[2]; }
                if (scn_is_cond(a)) lt = a.b[0];
            } else if (!rows[i].else_line && n.k == ScnNode::IF && !n.cond.empty()) {
                lt = E.scn_atoms[n.cond.back()].b[0];
            }
        }
    }
    // commands of the event
    ImGui::BeginChild("cmds", ImVec2(-1, h_all * 0.40f), ImGuiChildFlags_Borders);
    for (int i = 0; i < (int)rows.size(); i++) {
        const Row& r = rows[i];
        const ScnNode& n = r.node();
        nm.sel_cat = row_sel[i].first;
        nm.sel_num = row_sel[i].second;
        nm.last_test = row_test[i];
        std::string t;
        ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ScnScene sc;
        if (r.else_line) {
            t = "Else";
            col = ImVec4(0.55f, 0.75f, 1.0f, 1);
        } else if (n.k == ScnNode::IF && scn_scene_detect(E.scn_atoms, n, sc)) {
            t = scn_scene_name(sc.kind);
            col = ImVec4(0.98f, 0.68f, 0.38f, 1);
        } else if (n.k == ScnNode::IF) {
            std::vector<const ScriptAtom*> ca;
            for (int k : n.cond) ca.push_back(&E.scn_atoms[k]);
            t = scn_if_phrase(ca, E.scn_atoms[n.atom], nm);
            col = ImVec4(0.55f, 0.75f, 1.0f, 1);
        } else if (n.k == ScnNode::LOOP) {
            t = strfmt("Repeat %d times", (int)le16(&E.scn_atoms[n.atom].b[4]));
            col = ImVec4(0.55f, 0.75f, 1.0f, 1);
        } else {
            const ScriptAtom& a = E.scn_atoms[n.atom];
            t = is_jump(a) ? scn_jump_phrase(a, nm) : scn_line_phrase(a, nm);
            const ScnOp* so = scn_op(a);
            if (!so) col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            else if (so->technical) col = ImVec4(0.62f, 0.62f, 0.62f, 1);
            else if (so->cat == SC_OBJECT && a.b[0] == 0x22) col = ImVec4(0.95f, 0.8f, 0.45f, 1);   // "With ..." lines
        }
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        const std::string lbl = std::string(r.depth * 3, ' ') + t;
        if (ImGui::Selectable(lbl.c_str(), i == cur)) {
            E.scn_sel = row_key(E, r);
            cur = i;
            scenario_refresh(L);
        }
        if (scn_scroll_ && i == cur) ImGui::SetScrollHereY(0.35f);   // picked in the 3D view: the line in sight
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    scn_scroll_ = false;
    ImGui::EndChild();
    // add / delete / up / down
    auto insert_at = [&](std::vector<ScnNode>*& list, int& pos) { scn_insert_point(G, E.scn_atoms, cur >= 0 ? E.scn_sel : std::string(), list, pos); };
    auto add_atom = [&](ScriptAtom a) {
        a.nid = script_next_nid(E.scn_atoms);
        E.scn_atoms.push_back(std::move(a));
        return (int)E.scn_atoms.size() - 1;
    };
    bool edited = false;
    if (ImGui::Button("+ Add") || (scn_add_req_ >= 0 && lib_.ready.load())) ImGui::OpenPopup("add");
    if (ImGui::BeginPopup("add")) {
        std::vector<ScnNode>* list = nullptr;
        int pos = 0;
        ScnNode nn;
        std::vector<ScnNode> pre;   // lines that go right before the new one (the object of a comparison)
        bool made = false;
        int pend_text = -1;
        // 14.10.2026: a new command with values that fit this room: a free task slot, a free zone number, the floor and
        // place of the room's zones, a new text for a text field
        auto new_cmd = [&](const ScnOp& o) {
            ScriptAtom a = scn_new_atom(o);
            std::set<int> slots = {0, 9}, zones;
            std::map<int, int> floors;
            long sx = 0, sz = 0, nq = 0;
            for (auto& x : E.scn_atoms) {
                if (x.kind != ScriptAtom::OP || x.b.size() < 4) continue;
                if (x.b[0] == 0x01) slots.insert(x.b[1]);
                if (x.b[0] == 0x28) {
                    zones.insert(x.b[1]);
                    if (x.b.size() >= 24) {
                        floors[x.b[22]]++;
                        for (int k = 0; k < 4; k++) { sx += (int16_t)le16(&x.b[4 + k * 4]); sz += (int16_t)le16(&x.b[6 + k * 4]); nq++; }
                    }
                    if (x.b[2] == 3 && x.b.size() >= 29) slots.insert(x.b[28]);
                }
            }
            int free_slot = 8;
            for (int k = 1; k <= 8; k++) if (!slots.count(k)) { free_slot = k; break; }
            if (o.op == 0x01) a.b[1] = (uint8_t)free_slot;
            if (o.op == 0x28 && a.b.size() >= 24) {
                int zn = 27;
                for (int k = 0; k < 28; k++) if (!zones.count(k)) { zn = k; break; }
                a.b[1] = (uint8_t)zn;
                int fl = 0, best = -1;
                for (auto& kv : floors) if (kv.second > best) { best = kv.second; fl = kv.first; }
                a.b[22] = (uint8_t)fl;
                if (nq) {
                    const int cx = (int)(sx / nq), cz = (int)(sz / nq);
                    for (int k = 0; k < 4; k++) {
                        put16(&a.b[4 + k * 4], (uint16_t)(int16_t)std::clamp(cx + (int)(int16_t)le16(&a.b[4 + k * 4]), -32768, 32767));
                        put16(&a.b[6 + k * 4], (uint16_t)(int16_t)std::clamp(cz + (int)(int16_t)le16(&a.b[6 + k * 4]), -32768, 32767));
                    }
                }
                if (a.b[2] == 3) a.b[28] = (uint8_t)free_slot;
                if (a.b[2] == 4) {   // a first-aid item until another is picked
                    for (int id = 1; id < 0x70; id++)
                        if (item_name(id).rfind("Med", 0) == 0) { a.b[28] = (uint8_t)id; break; }
                }
            }
            for (auto& f : o.fields) {
                if (f.t != FT_TEXT) continue;
                ScriptAtom st;
                st.kind = ScriptAtom::STRING;
                std::vector<uint16_t> codes;
                std::string err;
                if (!encode_text(o.op == 0x21 ? "New message" : "New text", codes, err) || !script_set_text(st, codes, err)) continue;
                const int si = add_atom(st);
                ScriptSym sy;
                sy.at = f.at;
                sy.type = 2;
                sy.tnew = E.scn_atoms[si].nid;
                a.syms.push_back(sy);
                pend_text = si;   // its DATA node goes to the end of the script after the commands are in (pointers into the tree)
            }
            return add_atom(a);
        };
        if (ImGui::BeginMenu("Flow")) {
            for (const ScnOp* o : scn_ops_of(SC_FLOW))
                if (ImGui::MenuItem(o->name)) { nn.k = ScnNode::CMD; nn.atom = new_cmd(*o); made = true; }
            if (ImGui::BeginMenu("If")) {
                // the block runs when the condition holds (jump 6: past it on "false" / "not equal")
                for (const ScnOp* o : scn_ops_of(SC_COND)) {
                    if (!ImGui::MenuItem(o->name)) continue;
                    if (o->needs_object) {   // a comparison reads the object picked before it: Regina by default
                        ScnNode w;
                        w.atom = new_cmd(*scn_op(scn_new_atom(0x22)));
                        pre.push_back(w);
                    }
                    ScriptAtom br = scn_new_atom(0x0E);
                    br.b[1] = 6;
                    ScriptSym sy;
                    sy.at = 2;
                    sy.type = 0;
                    br.syms.push_back(sy);   // pointed at what follows the block when the tree is flattened
                    nn.k = ScnNode::IF;
                    nn.cond.push_back(new_cmd(*o));
                    nn.atom = add_atom(br);
                    made = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Repeat")) {
                ScriptAtom f = scn_new_atom(0x09), x = scn_new_atom(0x0A);
                put16(&f.b[4], 2);
                ScriptSym s0, s1;
                s0.at = 2;
                s0.type = 0;
                s1.at = 2;
                s1.type = 1;
                f.syms.push_back(s0);
                x.syms.push_back(s1);
                nn.k = ScnNode::LOOP;
                nn.atom = add_atom(f);
                nn.atom2 = add_atom(x);
                ScnNode w;
                w.atom = add_atom(scn_new_atom(0x03));   // a loop never stays empty: one frame each time
                nn.a.push_back(w);
                made = true;
            }
            ImGui::EndMenu();
        }
        // 14.10.2026 (player 10.10: "enemies come as the game does it: through a door, leaping in, with their own scene"):
        // the scene as a new event started from the picked line; then that event is shown, the scene picked
        int scene_kind = -1;
        if (scn_add_req_ >= 0 && lib_.ready.load()) { scene_kind = scn_add_req_; scn_add_req_ = -1; }   // checks
        if (ImGui::BeginMenu("Enemy appears")) {
            for (int k = 0; k < SCENE_COUNT; k++) {
                ScnScene probe;
                std::string why;
                const bool ok = scn_scene_defaults(E.scn_atoms, k, probe, why);
                if (ImGui::MenuItem(k == SCENE_LEAP_IN ? "Raptor leaps in" : "Raptor comes in through a door", nullptr, false, ok)) scene_kind = k;
                if (!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
            }
            ImGui::EndMenu();
        }
        if (scene_kind >= 0) {
            ScnScene sc;
            std::string why;
            scn_scene_defaults(E.scn_atoms, scene_kind, sc, why);
            const int df = new_flag(true);
            sc.death_flag = df >= 0 ? df : 250;
            // a fighter's motions when the room's raptor is a scene's (it would twist and fly once it hunts)
            uint64_t key = 0;
            for (const ActorPart& ap : L.c.actors)
                if (ap.block >= 0 && ap.block < (int)L.c.blocks.size() && L.c.blocks[ap.block].dest + ap.model.hdr == sc.model)
                    key = actor_key_blk(L.c.blocks[ap.block].data, L.c.blocks[ap.block].dest, ap.model.hdr);
            const int own = species_motions(L.c, sc.motions);
            const char st = L.c.file->stem.size() >= 3 ? L.c.file->stem[2] : 0;
            const LibSpecies* best = nullptr;
            if (lib_.ready.load())
                for (auto& s : lib_.species) {
                    if (s.kind != sc.dkind || (sc.model && s.key != key) || !s.f) continue;   // no raptor here: any body
                    auto rank = [&](const LibSpecies& x) { return (x.f->stem.size() >= 3 && x.f->stem[2] == st ? 1000 : 0) + x.nmot; };
                    if (!best || rank(s) > rank(*best)) best = &s;
                }
            if (best && (!sc.model || best->nmot > own * 2)) {
                sc.from = strfmt("%s %08X %08X", best->f->stem.c_str(), best->model, best->motions);
                if (!sc.model) { sc.model = best->model; sc.motions = best->motions; }   // its body comes in too
            }
            if (!sc.model) E.msg = "Not added: the game's raptors are still being read - try again in a moment";
            else if (scn_scene_add(E.scn, E.scn_atoms, E.scn_ev, cur >= 0 ? E.scn_sel : std::string(), sc, 30, why) && scenario_commit(L)) {
                int nr = -1, top = -1;   // the new event: the last subroutine
                for (int r = 0; r < (int)E.scn.regions.size(); r++)
                    for (int k : E.scn.regions[r].subs) if (k > top) { top = k; nr = r; }
                if (nr >= 0) {
                    E.scn_ev = nr;
                    for (auto& n : E.scn.regions[nr].nodes) {
                        ScnScene t;
                        if (scn_scene_detect(E.scn_atoms, n, t)) { E.scn_sel = akey(E.scn_atoms[n.atom]); break; }
                    }
                    scn_scroll_ = true;
                    scenario_refresh(L);
                }
            } else if (!why.empty()) {
                E.msg = "Not added: " + why;
            }
            edited = true;
            ImGui::CloseCurrentPopup();
        }
        bool message = false;
        if (ImGui::BeginMenu("Messages")) {
            if (ImGui::MenuItem("Message")) message = true;
            for (const ScnOp* o : scn_ops_of(SC_MESSAGE))
                if (o->op != 0x21 && ImGui::MenuItem(o->name)) { nn.k = ScnNode::CMD; nn.atom = new_cmd(*o); made = true; }
            ImGui::EndMenu();
        }
        if (message) {
            // a new text at the end of the script, the window that shows it (the game paused, as when Regina examines
            // something) and the wait until the player closes it
            nn.k = ScnNode::CMD;
            nn.atom = new_cmd(*scn_op(scn_new_atom(0x21)));
            put16(&E.scn_atoms[nn.atom].b[2], 0xFFFF);
            ScnNode wn;
            wn.atom = add_atom(scn_new_atom(0x12));
            insert_at(list, pos);
            list->insert(list->begin() + std::min(pos, (int)list->size()), wn);
            made = true;
        }
        for (int c = SC_COND; c < SC_COUNT; c++) {
            auto ops = scn_ops_of((ScnCat)c);
            if (ops.empty() || c == SC_COND || c == SC_MESSAGE) continue;
            if (ImGui::BeginMenu(scn_cat_name(c))) {
                for (const ScnOp* o : ops)
                    if (ImGui::MenuItem(o->name)) { nn.k = ScnNode::CMD; nn.atom = new_cmd(*o); made = true; }
                ImGui::EndMenu();
            }
        }
        if (made) {
            if (!list) insert_at(list, pos);
            pos = std::min(pos, (int)list->size());
            for (auto& w : pre) list->insert(list->begin() + pos++, w);
            list->insert(list->begin() + pos, nn);
            if (pend_text >= 0) {
                ScnNode dn;
                dn.k = ScnNode::DATA;
                dn.atom = pend_text;
                E.scn.regions.back().nodes.push_back(dn);
            }
            E.scn_sel = akey(E.scn_atoms[nn.atom]);
            scenario_commit(L);
            edited = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const bool can_edit = !edited && cur >= 0 && !scn_is_final_end(G, E.scn_atoms, E.scn_sel);
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_edit);
    if (ImGui::Button("Delete") && can_edit) {
        if (scn_remove(E.scn, E.scn_atoms, E.scn_ev, E.scn_sel)) {
            E.scn_sel.clear();
            scenario_commit(L);
        }
        edited = true;
    }
    ImGui::SameLine();
    auto move = [&](int dir) {
        if (scn_move(G, E.scn_atoms, E.scn_sel, dir)) scenario_commit(L);
    };
    if (ImGui::ArrowButton("up", ImGuiDir_Up) && can_edit) {
        move(-1);
        edited = true;
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("down", ImGuiDir_Down) && can_edit) {
        move(1);
        edited = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ Event") && !edited) {
        // a new subroutine: an empty event (End) that "Start event" in another one starts
        const int ai = add_atom(scn_new_atom(0x04));
        for (auto& a : E.scn_atoms) {
            if (a.kind != ScriptAtom::DIR) continue;
            ScriptSym sy;
            sy.at = (int)a.b.size();
            sy.type = 2;
            sy.tnew = E.scn_atoms[ai].nid;
            a.b.insert(a.b.end(), 4, 0);
            a.syms.push_back(sy);
            ScnRegion g;
            g.subs.push_back((int)a.b.size() / 4 - 1);
            ScnNode nn;
            nn.atom = ai;
            g.nodes.push_back(nn);
            E.scn.regions.push_back(std::move(g));
            break;
        }
        if (scenario_commit(L)) E.scn_ev = (int)E.scn.regions.size() - 1;
        E.scn_sel.clear();
        edited = true;
    }
    if (edited) return;   // the model was rebuilt: the fields show from the next frame
    // fields of the picked command
    ImGui::Separator();
    if (cur < 0 || cur >= (int)rows.size()) return;
    const Row& r = rows[cur];
    ScnNode& n = r.node();
    nm.sel_cat = row_sel[cur].first;
    nm.sel_num = row_sel[cur].second;
    bool changed = false;
    auto field_ui = [&](ScriptAtom& a, const ScnField& f) {
        ImGui::PushID(f.at * 64 + f.t);
        int v = scn_get(a, f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        switch (f.t) {
        case FT_FLAG: {
            int bank = v / 1000, bit = v % 1000;
            ImGui::SetNextItemWidth(70);
            bool c1 = ImGui::InputInt("##bank", &bank, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            bool c2 = ImGui::InputInt("##bit", &bit, 0);
            ImGui::SameLine();
            ImGui::TextUnformatted(f.name);
            if (c1 || c2) { scn_set(a, f, std::clamp(bank, 0, 255) * 1000 + std::clamp(bit, 0, 255)); changed = true; }
            break;
        }
        case FT_SUB: {
            const std::string cur_t = nm.sub(v);
            if (ImGui::BeginCombo(f.name, cur_t.c_str())) {
                for (auto& kv : sub_title)
                    if (ImGui::Selectable(kv.second.c_str(), kv.first == v)) { scn_set(a, f, kv.first); changed = true; }
                ImGui::EndCombo();
            }
            break;
        }
        case FT_KIND: {
            static const int kinds[] = {1, 2, 3, 4, 5, 7, 8};
            const std::string cur_t = enemy_kind_name(v).empty() ? std::to_string(v) : enemy_kind_name(v);
            if (ImGui::BeginCombo(f.name, cur_t.c_str())) {
                for (int k : kinds)
                    if (ImGui::Selectable(enemy_kind_name(k).c_str(), k == v)) { scn_set(a, f, k); changed = true; }
                ImGui::EndCombo();
            }
            break;
        }
        case FT_XYZ: {
            int xyz[3] = {(int)(int16_t)le16(&a.b[f.at]), (int)(int16_t)le16(&a.b[f.at + 2]), (int)(int16_t)le16(&a.b[f.at + 4])};
            if (ImGui::InputInt3(f.name, xyz)) {
                for (int k = 0; k < 3; k++) put16(&a.b[f.at + k * 2], (uint16_t)(int16_t)std::clamp(xyz[k], -32768, 32767));
                changed = true;
            }
            break;
        }
        case FT_TURN: {
            int deg = (int)lround(v * 360.0 / 4096.0);
            if (ImGui::SliderInt(f.name, &deg, -180, 180, "%d deg")) { scn_set(a, f, (int)lround(deg * 4096.0 / 360.0)); changed = true; }
            break;
        }
        case FT_FRAMES: {
            if (ImGui::InputInt(f.name, &v, 1, 30)) { scn_set(a, f, std::clamp(v, 0, 65535)); changed = true; }
            ImGui::SameLine();
            ImGui::TextDisabled("%.1f s", v / 30.0);
            break;
        }
        case FT_SELCAT: {
            if (ImGui::BeginCombo(f.name, scn_cat_object(v))) {
                for (int k = 0; k <= 8; k++)
                    if (ImGui::Selectable(scn_cat_object(k), k == v)) { scn_set(a, f, k); changed = true; }
                ImGui::EndCombo();
            }
            break;
        }
        case FT_FIELD: {
            const std::string cn = scn_field_name(nm.sel_cat, v);
            if (ImGui::BeginCombo(f.name, cn.empty() ? strfmt("field %d", v).c_str() : cn.c_str())) {
                for (int k : scn_fields_of(nm.sel_cat))
                    if (ImGui::Selectable(scn_field_name(nm.sel_cat, k).c_str(), k == v)) { scn_set(a, f, k); changed = true; }
                ImGui::EndCombo();
            }
            break;
        }
        case FT_ENUM:
        case FT_ENUM16: {
            auto items = scn_names(f.names);
            if (a.b[0] == 0x3A && !strcmp(f.name, "mode")) {   // the modes of the actor's kind
                const int k = nm.sel_num == 10 ? 1 : (nm.actor_kind ? nm.actor_kind(nm.sel_num) : -1);
                items = scn_names(k == 2 ? "0=hold|1=walk to|2=run to|3=stalk to|4=leap to|5=pounce at Regina|7=finish the motion|9=face|10=trot to"
                                         : f.names);
            }
            std::string cn = strfmt("%d", v);
            for (auto& kv : items) if (kv.first == v) cn = kv.second;
            if (ImGui::BeginCombo(f.name, cn.c_str())) {
                for (auto& kv : items)
                    if (ImGui::Selectable(kv.second.c_str(), kv.first == v)) { scn_set(a, f, kv.first); changed = true; }
                ImGui::EndCombo();
            }
            break;
        }
        case FT_BITS: {
            ImGui::TextUnformatted(f.name);
            for (auto& kv : scn_names(f.names)) {
                bool on = (v & kv.first) != 0;
                ImGui::SameLine();
                if (ImGui::Checkbox(kv.second.c_str(), &on)) { scn_set(a, f, on ? (v | kv.first) : (v & ~kv.first)); changed = true; }
            }
            break;
        }
        case FT_PTR:
            ImGui::TextDisabled("%s: %08X", f.name, (uint32_t)v);
            break;
        case FT_TEXT: {
            auto it = std::find_if(a.syms.begin(), a.syms.end(), [&](const ScriptSym& sy) { return sy.at == f.at; });
            int si = -1;
            if (it != a.syms.end())
                for (int k = 0; k < (int)E.scn_atoms.size() && si < 0; k++) {
                    const ScriptAtom& s2 = E.scn_atoms[k];
                    if (s2.kind != ScriptAtom::STRING) continue;
                    if (it->tnew >= 0 ? s2.nid == it->tnew
                                      : (s2.old >= 0 && it->target >= (uint32_t)s2.old && it->target < (uint32_t)s2.old + s2.old_len))
                        si = k;
                }
            if (si < 0) { ImGui::TextDisabled("%s", it != a.syms.end() ? nm.text(*it).c_str() : "(text)"); break; }
            ScriptAtom& st = E.scn_atoms[si];
            auto text_input = [&](std::string& buf) {
                return ImGui::InputTextMultiline(
                    "##msg", buf.data(), buf.capacity() + 1, ImVec2(-1, ImGui::GetTextLineHeight() * 4.5f), ImGuiInputTextFlags_CallbackResize,
                    [](ImGuiInputTextCallbackData* d) {
                        if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                            auto* str = (std::string*)d->UserData;
                            str->resize(d->BufTextLen);
                            d->Buf = str->data();
                        }
                        return 0;
                    },
                    &buf);
            };
            int ti = -1;   // the game's text in Texts (texts.json numbering)
            if (st.old >= 0)
                for (size_t k = 0; k < L.c.room.strings.size(); k++)
                    if (L.c.room.strings[k].off - L.c.room.script_off == (uint32_t)st.old + st.text_from) ti = (int)k;
            if (st.old < 0 || ti < 0) {   // a new text, or one Texts does not list: kept in the script, re-encoded on every change
                const std::string key = akey(st);
                if (E.scn_tkey != key) {
                    Bytes tb(st.b.begin() + std::min<size_t>(st.text_from, st.b.size()), st.b.end());
                    std::vector<uint16_t> codes(tb.size() / 2);
                    for (size_t k = 0; k < codes.size(); k++) codes[k] = le16(&tb[k * 2]);
                    E.scn_tbuf = decode_text(codes.data(), codes.size());
                    E.scn_tkey = key;
                }
                if (text_input(E.scn_tbuf)) {
                    std::vector<uint16_t> codes;
                    std::string err;
                    if (encode_text(E.scn_tbuf, codes, err) && script_set_text(st, codes, err)) changed = true;
                    else E.msg = err;
                }
            } else {            // the game's text: the same as in Texts (saved with the script)
                if (ti >= (int)E.texts.size()) { ImGui::TextWrapped("%s", nm.text(*it).c_str()); break; }
                if (text_input(E.texts[ti])) E.dirty_texts = true;
            }
            break;
        }
        case FT_ITEM: {
            auto iname = [](int id) {
                if (id == 0xFF) return std::string("random item");
                const std::string n = item_name(id);
                return n.empty() ? strfmt("item %d", id) : n;
            };
            if (ImGui::BeginCombo(f.name, iname(v).c_str())) {
                for (int id = 1; id < 0x70; id++) {
                    const std::string n = item_name(id);
                    if (n.empty()) continue;
                    ImGui::PushID(id);
                    if (ImGui::Selectable(n.c_str(), id == v)) { scn_set(a, f, id); changed = true; }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case FT_LOCK: {
            if (ImGui::BeginCombo(f.name, scn_lock_name(v).c_str())) {
                static const int locks[] = {0, 1, 2, 3, 6, 7, 8, 0xFD, 0xFE, 0xFF};
                for (int k : locks)
                    if (ImGui::Selectable(scn_lock_name(k).c_str(), k == v)) { scn_set(a, f, k); changed = true; }
                ImGui::Separator();
                for (int id = 9; id < 0x70; id++) {   // a key item: used up when the door opens
                    const std::string n = item_name(id);
                    if (n.empty()) continue;
                    ImGui::PushID(id);
                    if (ImGui::Selectable(("needs " + n).c_str(), id == v)) { scn_set(a, f, id); changed = true; }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case FT_ROOMCODE: {
            uint16_t rc = (uint16_t)v;
            if (ImGui::InputScalar(f.name, ImGuiDataType_U16, &rc, nullptr, nullptr, "%X", ImGuiInputTextFlags_CharsHexadecimal)) {
                scn_set(a, f, rc);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", room_code(rc).c_str());
            break;
        }
        case FT_QUAD: {
            // corners in the order of the game's data (x, z); moving them in the 3D view is the next step
            ImGui::TextUnformatted(f.name);
            for (int k = 0; k < 4 && (size_t)f.at + k * 4 + 4 <= a.b.size(); k++) {
                int xz[2] = {(int)(int16_t)le16(&a.b[f.at + k * 4]), (int)(int16_t)le16(&a.b[f.at + k * 4 + 2])};
                ImGui::PushID(k);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
                if (ImGui::InputInt2(strfmt("corner %d", k).c_str(), xz)) {
                    put16(&a.b[f.at + k * 4], (uint16_t)(int16_t)std::clamp(xz[0], -32768, 32767));
                    put16(&a.b[f.at + k * 4 + 2], (uint16_t)(int16_t)std::clamp(xz[1], -32768, 32767));
                    changed = true;
                }
                ImGui::PopID();
            }
            break;
        }
        case FT_HEX:
            break;
        default:
            if (ImGui::InputInt(f.name, &v, 0)) { scn_set(a, f, v); changed = true; }
            break;
        }
        ImGui::PopID();
    };
    ScnScene sc;
    if (r.else_line) {
        ImGui::TextDisabled("Runs when the If above does not.");
    } else if (n.k == ScnNode::IF && scn_scene_detect(E.scn_atoms, n, sc)) {
        // 14.10.2026: the scene in plain fields; its points are dragged in the 3D view; More - its commands as lines
        ImGui::TextDisabled("Drag the raptor%s and the camera in the 3D view.", sc.kind == SCENE_LEAP_IN ? ", where it lands" : "");
        bool ch = false;
        auto row = [&](const char* name, int* v) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
            if (ImGui::InputInt3(name, v)) ch = true;
        };
        row("raptor", sc.place);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::SliderInt("turn", &sc.turn, 0, 4095)) ch = true;
        if (sc.kind == SCENE_LEAP_IN) row("lands at", sc.land);
        row("camera", sc.eye);
        row("looks at", sc.target);
        static const char* temper[3] = {"calm", "normal", "fierce"};   // variants 1 / 4 / 7: rows of the raptor's attack table
        int tv = sc.variant <= 2 ? 0 : sc.variant <= 5 ? 1 : 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::Combo("temper", &tv, temper, 3)) { sc.variant = (uint8_t)(tv * 3 + 1); ch = true; }
        int hp = sc.hp;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::InputInt("HP (0: 750-1000)", &hp, 0)) { sc.hp = (uint16_t)std::clamp(hp, 0, 30000); ch = true; }
        if (!sc.from.empty()) ImGui::TextDisabled("Moves of the fighter of %s (this room's raptor has a scene's few)", sc.from.substr(0, sc.from.find(' ')).c_str());
        const std::string bk = akey(E.scn_atoms[n.atom]);
        bool more = E.scn_open.count(bk) > 0;
        if (ImGui::Checkbox("More: its commands", &more)) {
            if (more) E.scn_open.insert(bk); else E.scn_open.erase(bk);
            scenario_refresh(L);
        }
        if (ch) { scn_scene_apply(E.scn_atoms, n, sc); changed = true; }
    } else if (n.k == ScnNode::IF) {
        for (int k : n.cond) {
            ScriptAtom& a = E.scn_atoms[k];
            if (const ScnOp* o = scn_op(a)) for (auto& f : o->fields) field_ui(a, f);
        }
        ScriptAtom& br = E.scn_atoms[n.atom];
        const int cop = n.cond.empty() ? -1 : E.scn_atoms[n.cond.back()].b[0];
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (scn_is_compare_op(cop)) {
            // the operator lives in the jump after the comparison (the block runs when the jump is not taken)
            const char* cn = "?";
            for (auto& kv : scn_compare_ops()) if (kv.first == br.b[1]) cn = kv.second;
            if (ImGui::BeginCombo("compare", cn)) {
                for (auto& kv : scn_compare_ops())
                    if (ImGui::Selectable(kv.second, kv.first == br.b[1])) { br.b[1] = (uint8_t)kv.first; changed = true; }
                ImGui::EndCombo();
            }
        } else if (br.b[1] == 1 || br.b[1] == 6) {
            int pol = br.b[1] == 1 ? 1 : 0;
            const bool flag = cop == 0x26 || cop == 0x14;
            const char* items[] = {flag ? "is set" : "yes", flag ? "is not set" : "no"};
            if (ImGui::Combo("when", &pol, items, 2)) { br.b[1] = pol ? 1 : 6; changed = true; }
        }
    } else if (n.k == ScnNode::LOOP) {
        ScriptAtom& f = E.scn_atoms[n.atom];
        int cnt = le16(&f.b[4]);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::InputInt("times", &cnt)) { put16(&f.b[4], (uint16_t)std::clamp(cnt, 1, 65535)); changed = true; }
    } else {
        ScriptAtom& a = E.scn_atoms[n.atom];
        if (const ScnOp* o = scn_op(a)) {
            for (auto& f : o->fields) field_ui(a, f);
        } else {
            // not decoded yet: the bytes
            std::string hex;
            for (size_t k = 0; k < a.b.size(); k++) hex += strfmt(k ? " %02X" : "%02X", a.b[k]);
            ImGui::TextDisabled("Command %02X (%d bytes)", a.b.empty() ? 0 : a.b[0], (int)a.b.size());
            ImGui::TextWrapped("%s", hex.c_str());
        }
    }
    if (changed) scenario_commit(L);
}

// ---------------------------------------------------------------- the places of the event in the 3D view
// 14.10.2026 (player: "everything that changes and is assigned is seen in the viewport and edited in it - move, point,
// assign"): a click on a marker picks its command in the list and drags it along the floor at once. The picked command:
// a point - axes X / up / Z, the square in its middle (or the marker) along the floor, the ring of an actor turns it
// (Shift - steps of 11.25 deg; Q / E or Ctrl + wheel - turn); a zone - the square in the middle moves all of it, the
// corners one by one. The bytes change while the mouse is held (the fields below follow); the script is rebuilt when it
// is let go.
namespace {

float seg_dist2(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / l2 : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = ax + vx * t - px, dy = ay + vy * t - py;
    return sqrtf(dx * dx + dy * dy);
}

// a point inside a convex quad on the screen; its area (to prefer the smallest of zones one inside another)
bool in_quad(const float q[4][2], float x, float y, float& area) {
    int pos = 0, neg = 0;
    area = 0;
    for (int k = 0; k < 4; k++) {
        const float* a = q[k];
        const float* b = q[(k + 1) % 4];
        const float c = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
        (c >= 0 ? pos : neg)++;
        area += a[0] * b[1] - b[0] * a[1];
    }
    area = fabsf(area) * 0.5f;
    return pos == 0 || neg == 0;
}

int16_t snap10(float v) { return (int16_t)std::clamp((int)lroundf(v / 10.0f) * 10, -32760, 32760); }

}  // namespace

bool App::scenario_viewport(bool hovered, float img_x, float img_y) {
    Loaded* L = load(sel_.f);
    if (!L) return false;
    ensure_edit(*L);
    if (!scenario_sync(*L)) return false;
    RoomEdit& E = L->ed;
    if (E.scn_text || E.scn_ev < 0 || E.scn_ev >= (int)E.scn.regions.size()) { scn_gz_ = 0; return false; }
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    ImGuiIO& io = ImGui::GetIO();
    const float mx = io.MousePos.x - img_x, my = io.MousePos.y - img_y;
    auto find_key = [&](const std::string& key) {
        for (int i = 0; i < (int)E.scn_atoms.size(); i++)
            if (E.scn_atoms[i].kind == ScriptAtom::OP && akey(E.scn_atoms[i]) == key) return i;
        return -1;
    };
    auto rd = [](const ScriptAtom& a, int at) { return (float)(int16_t)le16(&a.b[at]); };
    if (scn_gz_key_ != E.scn_sel) { scn_gz_key_ = E.scn_sel; scn_gz_pt_ = 0; scn_gz_ = 0; }
    // 14.10.2026: closed scene blocks - their lines are not markers, the block's points are (in its lines: p.atom)
    std::set<int> hidden;
    std::map<int, std::vector<ScnPoint>> spts;
    scene_marks(E, E.scn.regions[E.scn_ev].nodes, hidden, spts);
    auto points_of = [&](int i) { auto it = spts.find(i); return it != spts.end() ? it->second : scn_points(E.scn_atoms[i]); };
    auto host = [&](int i, const ScnPoint& p) -> ScriptAtom& { return E.scn_atoms[p.atom >= 0 ? p.atom : i]; };
    bool took = false;
    // ---- the picked command: its handles on the screen
    int pi = find_key(E.scn_sel);
    std::vector<ScnPoint> pts;
    int qa = -1;
    float qy = 0;
    float ox = 0, oy = 0, oz = 0, so[2] = {0, 0}, tip[3][2] = {}, len_mm = 1000;
    float qs[4][2] = {}, qc[2] = {0, 0};
    std::vector<std::array<float, 2>> psc;   // the screen place of every point (-1e9: off the screen)
    std::vector<ImVec2> ring;
    bool gz_ok = false, q_ok = false, can_turn = false;
    auto geometry = [&]() {
        gz_ok = q_ok = can_turn = false;
        ring.clear();
        psc.clear();
        pts.clear();
        qa = -1;
        if (pi < 0) return;
        const ScriptAtom& a = E.scn_atoms[pi];
        pts = points_of(pi);
        qa = scn_quad_at(a);
        if (qa >= 0) {
            qy = scn_quad_y(a);
            q_ok = true;
            float cx = 0, cz = 0;
            for (int k = 0; k < 4; k++) {
                const float x = rd(a, qa + k * 4), z = rd(a, qa + k * 4 + 2);
                cx += x * 0.25f; cz += z * 0.25f;
                if (!v3d_.project(x, qy, z, qs[k][0], qs[k][1])) q_ok = false;
            }
            if (!v3d_.project(cx, qy, cz, qc[0], qc[1])) q_ok = false;
        }
        if (pts.empty()) return;
        scn_gz_pt_ = std::clamp(scn_gz_pt_, 0, (int)pts.size() - 1);
        for (auto& p : pts) {
            std::array<float, 2> s = {-1e9f, -1e9f};
            const ScriptAtom& h = host(pi, p);
            v3d_.project(rd(h, p.at), rd(h, p.at + 2), rd(h, p.at + 4), s[0], s[1]);
            psc.push_back(s);
        }
        const ScnPoint& p = pts[scn_gz_pt_];
        const ScriptAtom& h = host(pi, p);
        ox = rd(h, p.at); oy = rd(h, p.at + 2); oz = rd(h, p.at + 4);
        can_turn = p.turn_at >= 0;
        float tx, ty;
        if (!v3d_.project(ox, oy, oz, so[0], so[1]) || !v3d_.project(ox + 1000, oy, oz, tx, ty)) return;
        const float px = std::max(4.0f, sqrtf((tx - so[0]) * (tx - so[0]) + (ty - so[1]) * (ty - so[1])));
        len_mm = std::clamp(1000.0f * 95.0f / px, 50.0f, 50000.0f);
        const float ax[3][3] = {{len_mm, 0, 0}, {0, -len_mm, 0}, {0, 0, len_mm}};
        for (int k = 0; k < 3; k++)
            if (!v3d_.project(ox + ax[k][0], oy + ax[k][1], oz + ax[k][2], tip[k][0], tip[k][1])) return;
        if (can_turn)
            for (int k = 0; k <= 48; k++) {
                const float an = k * 6.2831853f / 48, rr = 0.8f * len_mm;
                float rx, ry;
                if (v3d_.project(ox + rr * sinf(an), oy, oz + rr * cosf(an), rx, ry)) ring.push_back(ImVec2(rx, ry));
            }
        gz_ok = true;
    };
    geometry();
    if (scn_probe_) {   // checks: where the handle is, the bytes before the drag, the report after it
        const int ti = scn_probe_key_.empty() ? pi : find_key(scn_probe_key_);
        if (!scn_probe_key_.empty() && ti >= 0) {   // the marker of another command: its first point (the foot) or the zone's middle
            const ScriptAtom& a = E.scn_atoms[ti];
            const auto ps = scn_points(a);
            const int q = scn_quad_at(a);
            float x = 0, y = 0, z = 0, sx, sy;
            if (!ps.empty()) { x = rd(a, ps[0].at); y = rd(a, ps[0].at + 2); z = rd(a, ps[0].at + 4); }
            else if (q >= 0) { y = scn_quad_y(a); for (int k = 0; k < 4; k++) { x += rd(a, q + k * 4) * 0.25f; z += rd(a, q + k * 4 + 2) * 0.25f; } }
            if (v3d_.project(x, y, z, sx, sy)) { scn_probe_t_[0] = img_x + sx; scn_probe_t_[1] = img_y + sy; }
        } else if (q_ok) { scn_probe_t_[0] = img_x + qc[0]; scn_probe_t_[1] = img_y + qc[1]; }
        else if (gz_ok) { scn_probe_t_[0] = img_x + so[0]; scn_probe_t_[1] = img_y + so[1]; }
        auto hex = [&](int i) {
            std::string h;
            if (i >= 0) for (uint8_t v : E.scn_atoms[i].b) h += strfmt("%02X ", v);
            return h;
        };
        if (frame_no == 40) scn_probe_before_ = hex(ti);
        if (frame_no == 66) {
            std::string line;
            const size_t at = E.script.find(E.scn_sel + "  ");
            if (at != std::string::npos) line = E.script.substr(at, E.script.find('\n', at) - at);
            // 14.10.2026: the lines of the whole event (a scene block keeps its points in several of them)
            std::vector<int> ev;
            for (auto& n : E.scn.regions[E.scn_ev].nodes) atoms_of(n, ev);
            for (int i : ev) {
                const size_t p = E.script.find(akey(E.scn_atoms[i]) + "  ");
                if (p != std::string::npos) line += "\n  " + E.script.substr(p, E.script.find('\n', p) - p);
            }
            write_text(u8path(shot_path + ".scn.txt"),
                       strfmt("command %s\nhandle at %.0f %.0f, dragged %.0f %.0f px\nbefore %s\nafter  %s\nscript changed: %s\nline: %s\n",
                              E.scn_sel.c_str(), scn_probe_t0_[0], scn_probe_t0_[1], scn_probe_d_[0], scn_probe_d_[1], scn_probe_before_.c_str(),
                              hex(scn_probe_key_.empty() ? pi : find_key(scn_probe_key_)).c_str(), E.dirty_script ? "yes" : "no", line.c_str()));
        }
    }
    // what is under the mouse: 1..3 axis, 4 the point's square, 5 ring, 6 the zone's middle, 10+k corner k, 20+k point k
    int hot = 0;
    auto near_sq = [&](float x, float y) { return fabsf(mx - x) <= 8 && fabsf(my - y) <= 8; };
    if (hovered && !scn_gz_) {
        if (q_ok) {
            if (near_sq(qc[0], qc[1])) hot = 6;
            for (int k = 0; k < 4 && !hot; k++) if (near_sq(qs[k][0], qs[k][1])) hot = 10 + k;
        }
        if (!hot && gz_ok && near_sq(so[0], so[1])) hot = 4;
        for (int k = 0; k < (int)psc.size() && !hot; k++) if (k != scn_gz_pt_ && near_sq(psc[k][0], psc[k][1])) hot = 20 + k;
        if (!hot && gz_ok) {
            float best = 8.0f;
            for (int k = 0; k < 3; k++) {
                const float d = seg_dist2(mx, my, so[0], so[1], tip[k][0], tip[k][1]);
                if (d < best) { best = d; hot = k + 1; }
            }
            if (!hot && can_turn)
                for (size_t k = 0; k + 1 < ring.size(); k++)
                    if (seg_dist2(mx, my, ring[k].x, ring[k].y, ring[k + 1].x, ring[k + 1].y) < 7) { hot = 5; break; }
        }
    }
    auto begin = [&](int g) {
        scn_gz_ = g;
        scn_gz_moved_ = false;
        scn_gz_mouse_[0] = mx; scn_gz_mouse_[1] = my;
        const ScriptAtom& a = E.scn_atoms[pi];
        if (g == 6 || (g >= 10 && g < 20)) {
            for (int k = 0; k < 8; k++) scn_gz_quad_[k] = (int16_t)le16(&a.b[qa + k * 2]);
            float gx = 0, gzz = 0;
            v3d_.ground(mx, my, qy, gx, gzz);
            scn_gz_start_[0] = gx; scn_gz_start_[1] = qy; scn_gz_start_[2] = gzz;
            return;
        }
        scn_gz_start_[0] = ox; scn_gz_start_[1] = oy; scn_gz_start_[2] = oz;
        scn_gz_rot_ = can_turn ? (int)le16(&host(pi, pts[scn_gz_pt_]).b[pts[scn_gz_pt_].turn_at]) & 4095 : 0;
        float gx, gzz;
        scn_gz_off_[0] = scn_gz_off_[1] = 0;
        if (v3d_.ground(mx, my, oy, gx, gzz)) {
            scn_gz_off_[0] = ox - gx; scn_gz_off_[1] = oz - gzz;
            scn_gz_ang_ = atan2f(gx - ox, gzz - oz);
        }
    };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hot >= 20) { scn_gz_pt_ = hot - 20; geometry(); begin(4); took = true; }
        else if (hot) { begin(hot); took = true; }
        else {
            // a marker of the event under the mouse: a point (its foot, the middle and the top of its pole) within 18 pixels,
            // else the smallest zone the mouse is in
            std::vector<int> ids;
            for (auto& n : E.scn.regions[E.scn_ev].nodes) atoms_of(n, ids);
            int best = -1, best_pt = 0;
            float bd = 18.0f * 18.0f, barea = 1e30f;
            bool best_is_pt = false;
            for (int i : ids) {
                const ScriptAtom& a = E.scn_atoms[i];
                if (a.kind != ScriptAtom::OP || a.b.empty() || hidden.count(i)) continue;
                const auto ps = points_of(i);
                for (int k = 0; k < (int)ps.size(); k++) {
                    const ScriptAtom& h = host(i, ps[k]);
                    const float x = rd(h, ps[k].at), y = rd(h, ps[k].at + 2), z = rd(h, ps[k].at + 4);
                    for (float up : {0.0f, ps[k].turn_at >= 0 ? 900.0f : 300.0f, ps[k].turn_at >= 0 ? 1800.0f : 600.0f}) {
                        float sx, sy;
                        if (!v3d_.project(x, y - up, z, sx, sy)) continue;
                        const float d = (sx - mx) * (sx - mx) + (sy - my) * (sy - my);
                        if (d < bd) { bd = d; best = i; best_pt = k; best_is_pt = true; }
                    }
                }
            }
            if (!best_is_pt)
                for (int i : ids) {
                    const ScriptAtom& a = E.scn_atoms[i];
                    const int q = scn_quad_at(a);
                    if (q < 0 || hidden.count(i)) continue;
                    const float y = scn_quad_y(a);
                    float s[4][2];
                    bool ok = true;
                    for (int k = 0; k < 4 && ok; k++) ok = v3d_.project(rd(a, q + k * 4), y, rd(a, q + k * 4 + 2), s[k][0], s[k][1]);
                    float area;
                    if (ok && in_quad(s, mx, my, area) && area < barea) { barea = area; best = i; }
                }
            if (best >= 0) {
                E.scn_sel = akey(E.scn_atoms[best]);
                scn_gz_key_ = E.scn_sel;
                scn_gz_pt_ = best_is_pt ? best_pt : 0;
                scn_scroll_ = true;
                pi = best;
                geometry();
                if (best_is_pt && gz_ok) begin(4);
                else if (!best_is_pt && q_ok) begin(6);
                scenario_refresh(*L);
                took = true;
            }
        }
    }
    // ---- dragging: the bytes of the picked command
    if (scn_gz_ && pi >= 0) {
        const bool on_quad = scn_gz_ == 6 || (scn_gz_ >= 10 && scn_gz_ < 14);
        ScriptAtom& a = on_quad || pts.empty() ? E.scn_atoms[pi] : host(pi, pts[scn_gz_pt_]);   // a scene's point: its line
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            took = true;
            const Bytes before = a.b;
            if (scn_gz_ == 6 || (scn_gz_ >= 10 && scn_gz_ < 14)) {
                float gx, gzz;
                if (qa >= 0 && v3d_.ground(mx, my, scn_gz_start_[1], gx, gzz)) {
                    const int dx = snap10(gx - scn_gz_start_[0]), dz = snap10(gzz - scn_gz_start_[2]);
                    for (int k = 0; k < 4; k++) {
                        const bool mv = scn_gz_ == 6 || scn_gz_ - 10 == k;
                        put16(&a.b[qa + k * 4], (uint16_t)(int16_t)std::clamp(scn_gz_quad_[k * 2] + (mv ? dx : 0), -32760, 32760));
                        put16(&a.b[qa + k * 4 + 2], (uint16_t)(int16_t)std::clamp(scn_gz_quad_[k * 2 + 1] + (mv ? dz : 0), -32760, 32760));
                    }
                }
            } else if (!pts.empty()) {
                const ScnPoint& p = pts[scn_gz_pt_];
                float nx = scn_gz_start_[0], ny = scn_gz_start_[1], nz = scn_gz_start_[2];
                bool mv = false;
                if (scn_gz_ >= 1 && scn_gz_ <= 3 && gz_ok) {
                    const int k = scn_gz_ - 1;
                    const float ux = tip[k][0] - so[0], uy = tip[k][1] - so[1], l2 = ux * ux + uy * uy;
                    if (l2 > 4) {
                        const float d = ((mx - scn_gz_mouse_[0]) * ux + (my - scn_gz_mouse_[1]) * uy) / l2 * len_mm;   // mm along the axis
                        nx += k == 0 ? d : 0;
                        ny -= k == 1 ? d : 0;
                        nz += k == 2 ? d : 0;
                        mv = true;
                    }
                } else if (scn_gz_ == 4) {
                    float gx, gzz;
                    if (v3d_.ground(mx, my, scn_gz_start_[1], gx, gzz)) { nx = gx + scn_gz_off_[0]; nz = gzz + scn_gz_off_[1]; mv = true; }
                } else if (scn_gz_ == 5 && p.turn_at >= 0) {
                    float gx, gzz;
                    if (v3d_.ground(mx, my, scn_gz_start_[1], gx, gzz)) {
                        const float an = atan2f(gx - scn_gz_start_[0], gzz - scn_gz_start_[2]);
                        int r = (scn_gz_rot_ + (int)lroundf((an - scn_gz_ang_) * 4096.0f / 6.2831853f)) & 4095;
                        if (io.KeyShift) r = ((r + 64) / 128 * 128) & 4095;
                        put16(&a.b[p.turn_at], (uint16_t)r);
                    }
                }
                if (mv) {
                    put16(&a.b[p.at], (uint16_t)snap10(nx));
                    put16(&a.b[p.at + 2], (uint16_t)snap10(ny));
                    put16(&a.b[p.at + 4], (uint16_t)snap10(nz));
                }
            }
            if (a.b != before) { scn_gz_moved_ = true; scenario_refresh(*L); geometry(); }
        } else {
            scn_gz_ = 0;
            if (scn_gz_moved_) { scn_gz_moved_ = false; scenario_commit(*L); return took; }   // the atoms were rebuilt
        }
    }
    // turn of an actor standing at the point: Q / E (1/16) or Ctrl + wheel (1/64)
    if (can_turn && pi >= 0 && !scn_gz_ && hovered && !io.WantTextInput) {
        int step = 0;
        if (io.KeyCtrl && io.MouseWheel != 0) step = io.MouseWheel > 0 ? 64 : -64;
        if (ImGui::IsKeyPressed(ImGuiKey_Q)) step = -256;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) step = 256;
        if (step) {
            ScriptAtom& a = host(pi, pts[scn_gz_pt_]);
            const int at = pts[scn_gz_pt_].turn_at;
            put16(&a.b[at], (uint16_t)((le16(&a.b[at]) + step) & 4095));
            scenario_commit(*L);
            return true;
        }
    }
    // ---- the handles over the view
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(r0, r1, true);
    const ImU32 hotc = IM_COL32(255, 220, 70, 255);
    const int act = scn_gz_ ? scn_gz_ : hot;
    auto square = [&](float x, float y, bool on, ImU32 c) {
        const ImVec2 o(img_x + x, img_y + y);
        dl->AddRectFilled(ImVec2(o.x - 6, o.y - 6), ImVec2(o.x + 6, o.y + 6), on ? hotc : c);
        dl->AddRect(ImVec2(o.x - 6, o.y - 6), ImVec2(o.x + 6, o.y + 6), IM_COL32(20, 20, 20, 200));
    };
    if (q_ok) {
        for (int k = 0; k < 4; k++) {
            const float* a = qs[k];
            const float* b = qs[(k + 1) % 4];
            dl->AddLine(ImVec2(img_x + a[0], img_y + a[1]), ImVec2(img_x + b[0], img_y + b[1]), act == 6 ? hotc : IM_COL32(255, 255, 255, 140), 2.0f);
        }
        for (int k = 0; k < 4; k++) square(qs[k][0], qs[k][1], act == 10 + k, IM_COL32(255, 165, 60, 235));
        square(qc[0], qc[1], act == 6, IM_COL32(240, 240, 240, 235));
    }
    for (int k = 0; k < (int)psc.size(); k++)
        if (k != scn_gz_pt_ && psc[k][0] > -1e8f) square(psc[k][0], psc[k][1], act == 20 + k, IM_COL32(110, 230, 240, 235));
    if (gz_ok) {
        const ImVec2 o(img_x + so[0], img_y + so[1]);
        const ImU32 col[3] = {IM_COL32(235, 70, 70, 255), IM_COL32(95, 215, 95, 255), IM_COL32(80, 140, 255, 255)};
        if (can_turn && ring.size() > 1) {
            std::vector<ImVec2> p2;
            for (auto& p : ring) p2.push_back(ImVec2(img_x + p.x, img_y + p.y));
            dl->AddPolyline(p2.data(), (int)p2.size(), act == 5 ? hotc : IM_COL32(225, 225, 225, 210), 0, act == 5 ? 3.0f : 2.0f);
        }
        static const char* names[3] = {"X", "Y", "Z"};
        for (int k = 0; k < 3; k++) {
            const ImVec2 t(img_x + tip[k][0], img_y + tip[k][1]);
            const ImU32 c = act == k + 1 ? hotc : col[k];
            dl->AddLine(o, t, c, act == k + 1 ? 4.0f : 3.0f);
            float dx = t.x - o.x, dy = t.y - o.y;
            const float l = sqrtf(dx * dx + dy * dy);
            if (l > 1) {
                dx /= l; dy /= l;
                const ImVec2 b(t.x - dx * 12, t.y - dy * 12);
                dl->AddTriangleFilled(ImVec2(t.x + dx * 4, t.y + dy * 4), ImVec2(b.x - dy * 6, b.y + dx * 6), ImVec2(b.x + dy * 6, b.y - dx * 6), c);
                dl->AddText(ImVec2(t.x + dx * 10 - 4, t.y + dy * 10 - 7), c, names[k]);
            }
        }
        square(so[0], so[1], act == 4, IM_COL32(240, 240, 240, 235));
        if (pts.size() > 1) dl->AddText(ImVec2(o.x + 10, o.y + 8), IM_COL32(240, 240, 240, 230), pts[scn_gz_pt_].name);
    }
    dl->PopClipRect();
    return took;
}

void App::input_probe() {
    if (!scn_probe_) return;
    ImGuiIO& io = ImGui::GetIO();
    const int f = frame_no;
    if (f == 40) { scn_probe_t0_[0] = scn_probe_t_[0]; scn_probe_t0_[1] = scn_probe_t_[1]; }
    if (f < 40 || f > 60 || scn_probe_t0_[0] < 0) return;
    const float k = f <= 44 ? 0.0f : std::min(1.0f, (f - 44) / 12.0f);
    io.AddMousePosEvent(scn_probe_t0_[0] + scn_probe_d_[0] * k, scn_probe_t0_[1] + scn_probe_d_[1] * k);
    if (f == 42) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (f == 58) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
}

}  // namespace dc
