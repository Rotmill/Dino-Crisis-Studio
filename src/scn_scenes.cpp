// 14.10.2026: entrance scenes of enemies as script commands (scn_scenes.h). Every command is one the game's own scripts
// use, with the meanings read in docs\script_vm (g2..g8).
#include "scn_scenes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "scn_ops.h"

namespace dc {

const char* scn_scene_name(int kind) {
    return kind == SCENE_LEAP_IN ? "Raptor leaps in (window scene)" : "Raptor comes in (door scene)";
}

bool scn_scene_defaults(const std::vector<ScriptAtom>& atoms, int kind, ScnScene& s, std::string& err) {
    s = ScnScene();
    s.kind = kind;
    // a raptor of this room to copy: its model, motions and looks (0x20 also gives hp and ai)
    int src_rank = -1;
    std::set<int> slots, sig;
    std::map<int, int> floors;
    long cx = 0, cz = 0, nq = 0;
    float bx0 = 1e9f, bx1 = -1e9f, bz0 = 1e9f, bz1 = -1e9f;   // the zones' box: the scene camera stays inside it
    bool door = false;
    int dx = 0, dz = 0;
    for (const ScriptAtom& a : atoms) {
        if (a.kind != ScriptAtom::OP || a.b.size() < 4) continue;
        const uint8_t op = a.b[0];
        if (op == 0x20 || op == 0x59 || op == 0x5B) {
            slots.insert(a.b[1]);
            const int rank = op == 0x20 ? 2 : 1;
            const int mo = op == 0x20 ? 16 : op == 0x59 ? 4 : 12;
            if (a.b[2] == 2 && rank > src_rank && a.b.size() >= (size_t)mo + 8) {
                src_rank = rank;
                s.variant = a.b[3];
                s.model = le32(&a.b[mo]);
                s.motions = le32(&a.b[mo + 4]);
                s.hp = op == 0x20 ? le16(&a.b[6]) : 0;
                s.ai = op == 0x20 ? (uint8_t)(a.b[5] & 0x7F) : 1;   // not "scene only" (0x80)
            }
        }
        // bits of bank 3 the room already uses
        if ((op == 0x25 || op == 0x26 || op == 0x14 || op == 0x18) && a.b[1] == 3) sig.insert(a.b[2]);
        if (op == 0x3A) sig.insert(a.b[3]);
        if (op == 0x5A) sig.insert(a.b[2]);
        if (op == 0x28 && a.b.size() >= 24) {
            if (a.b[2] == 2 && a.b.size() >= 30 && a.b[28] == 3) sig.insert(a.b[29]);
            floors[a.b[22]]++;
            long qx = 0, qz = 0;
            for (int k = 0; k < 4; k++) {
                const float x = (int16_t)le16(&a.b[4 + k * 4]), z = (int16_t)le16(&a.b[6 + k * 4]);
                qx += (long)x; qz += (long)z;
                bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); bz0 = std::min(bz0, z); bz1 = std::max(bz1, z);
            }
            cx += qx; cz += qz; nq += 4;
            if (a.b[2] == 0 && !door) { door = true; dx = (int)(qx / 4); dz = (int)(qz / 4); }
        }
    }
    if (src_rank < 0) { s.variant = 4; s.ai = 1; }   // no raptor here: the caller brings one of another room
    if (s.variant > 8) s.variant = 4;   // a scene raptor's 9 / 10 are past the attack table: a normal fighter
    for (int k = 9; k >= 0 && s.slot < 0; k--) if (!slots.count(k)) s.slot = k;
    if (s.slot < 0) { err = "No free actor slot (a room has 10)"; return false; }
    for (int k = 63; k >= 32 && s.signal < 0; k--) if (!sig.count(k)) s.signal = k;
    if (s.signal < 0) { err = "No free room signal (flag bank 3)"; return false; }
    // places: the room's first door (or the middle of its zones), facing into the room
    const float mx = nq ? (float)cx / nq : 0.0f, mz = nq ? (float)cz / nq : 0.0f;
    int fl = 0, best = -1;
    for (auto& kv : floors) if (kv.second > best) { best = kv.second; fl = kv.first; }
    const float y = -425.0f * fl;
    float px = door ? (float)dx : mx + 2000.0f, pz = door ? (float)dz : mz;
    float ix = mx - px, iz = mz - pz, il = sqrtf(ix * ix + iz * iz);
    if (il < 1) { ix = -1; iz = 0; il = 1; }
    ix /= il; iz /= il;                                    // into the room
    auto set3 = [](int* v, float x, float yy, float z) { v[0] = (int)lroundf(x / 10) * 10; v[1] = (int)lroundf(yy / 10) * 10; v[2] = (int)lroundf(z / 10) * 10; };
    s.turn = (int)lroundf(atan2f(ix, iz) * 4096.0f / 6.2831853f) & 4095;
    if (kind == SCENE_LEAP_IN) {
        // outside behind the opening, lands 2 m inside; the camera further in, past where it lands (checked in the game:
        // a camera between the two had the raptor fly into it), looking at the opening
        set3(s.place, px - ix * 1200, y, pz - iz * 1200);
        set3(s.land, px + ix * 2000, y, pz + iz * 2000);
        set3(s.target, px, y - 900, pz);
        set3(s.eye, px + ix * 5500, y - 1600, pz + iz * 5500);   // 3.5 m past where it lands
    } else {
        // just in front of the door (in it, it stands half in the wall); the camera 3.5 m into the room and up
        set3(s.place, px + ix * 300, y, pz + iz * 300);
        set3(s.target, s.place[0], y - 700, s.place[2]);
        set3(s.eye, s.place[0] + ix * 3500, y - 1800, s.place[2] + iz * 3500);
    }
    if (bx0 < bx1 && bz0 < bz1) {
        s.eye[0] = (int)std::clamp((float)s.eye[0], bx0 + 300, bx1 - 300) / 10 * 10;
        s.eye[2] = (int)std::clamp((float)s.eye[2], bz0 + 300, bz1 - 300) / 10 * 10;
    }
    return true;
}

namespace {

struct Builder {
    std::vector<ScriptAtom>& at;
    int add(ScriptAtom a) {
        a.nid = script_next_nid(at);
        at.push_back(std::move(a));
        return (int)at.size() - 1;
    }
    static ScriptAtom op(uint8_t o) {
        ScriptAtom a = scn_new_atom(o);
        std::fill(a.b.begin() + 1, a.b.end(), 0);
        return a;
    }
    static void xyz(ScriptAtom& a, int at, const int* v) { for (int k = 0; k < 3; k++) put16(&a.b[at + k * 2], (uint16_t)(int16_t)v[k]); }
    ScnNode cmd(const ScriptAtom& a) {
        ScnNode n;
        n.atom = add(a);
        return n;
    }
    ScnNode b4(uint8_t o, uint8_t b1, uint8_t b2, uint8_t b3) {
        ScriptAtom a = op(o);
        a.b[1] = b1; a.b[2] = b2; a.b[3] = b3;
        return cmd(a);
    }
    ScnNode select(int n) { return b4(0x22, 2, (uint8_t)n, 0); }
    ScnNode show(int n, bool on) { return b4(0x48, 2, (uint8_t)n, on ? 1 : 0); }
    ScnNode flag(int bank, int bit, int v) { return b4(0x25, (uint8_t)bank, (uint8_t)bit, (uint8_t)v); }
    ScnNode release(int state) { return b4(0x3B, (uint8_t)state, 0, 0); }
    ScnNode wait(int frames) {
        ScriptAtom a = op(0x02);
        put16(&a.b[2], (uint16_t)frames);
        return cmd(a);
    }
    ScnNode frame() { return cmd(op(0x03)); }
    ScnNode act(int mode, int done, const int* p) {   // 0x3A: the selected actor moves / holds, then flag 3/done
        ScriptAtom a = op(0x3A);
        a.b[2] = (uint8_t)mode; a.b[3] = (uint8_t)done;
        xyz(a, 4, p);
        return cmd(a);
    }
    ScnNode sound_at(int id, const int* p) {           // 0x55 mode 0: the sound at a point
        ScriptAtom a = op(0x55);
        a.b[1] = (uint8_t)id;
        xyz(a, 4, p);
        return cmd(a);
    }
    ScnNode set(int field, int v) {                    // 0x2A: field of the selected object
        ScriptAtom a = op(0x2A);
        a.b[2] = (uint8_t)field;
        put32(&a.b[4], (uint32_t)v);
        return cmd(a);
    }
    ScnNode camera(const int* eye, const int* target) {   // 0x2E with speeds 0: a cut (+2 where it looks, +8 the camera)
        ScriptAtom a = op(0x2E);
        xyz(a, 2, target);
        xyz(a, 8, eye);
        return cmd(a);
    }
    ScnNode jump(int bit) {
        ScriptAtom a = op(0x0E);
        a.b[1] = (uint8_t)bit;
        ScriptSym sy;
        sy.at = 2;
        sy.type = 0;
        a.syms.push_back(sy);   // pointed past the block when the tree is flattened
        ScnNode n;
        n.k = ScnNode::IF;
        n.atom = add(a);
        return n;
    }
    // "If <cond> holds: <body>" - the jump skips the body when it does not
    ScnNode if_(const ScriptAtom& cond, int skip_bit, std::vector<ScnNode> body) {
        const int ci = add(cond);
        ScnNode n = jump(skip_bit);
        n.cond.push_back(ci);
        n.a = std::move(body);
        return n;
    }
    ScnNode repeat(int count, std::vector<ScnNode> body) {
        ScriptAtom f = op(0x09), x = op(0x0A);
        put16(&f.b[4], (uint16_t)count);
        ScriptSym s0, s1;
        s0.at = 2; s0.type = 0;
        s1.at = 2; s1.type = 1;
        f.syms.push_back(s0);
        x.syms.push_back(s1);
        ScnNode n;
        n.k = ScnNode::LOOP;
        n.atom = add(f);
        n.atom2 = add(x);
        n.a = std::move(body);
        return n;
    }
};

}  // namespace

ScnNode scn_scene_build(std::vector<ScriptAtom>& atoms, const ScnScene& s) {
    Builder B{atoms};
    std::vector<ScnNode> body;
    auto put = [&](ScnNode n) { body.push_back(std::move(n)); };
    // the raptor: placed, held where it stands, hidden until its moment
    {
        ScriptAtom a = Builder::op(0x20);
        a.b[1] = (uint8_t)s.slot; a.b[2] = s.dkind; a.b[3] = s.variant; a.b[4] = (uint8_t)s.death_flag; a.b[5] = s.ai;
        put16(&a.b[6], s.hp);
        Builder::xyz(a, 8, s.place);
        put16(&a.b[14], (uint16_t)s.turn);
        put32(&a.b[16], s.model);
        put32(&a.b[20], s.motions);
        a.from = s.from;
        put(B.cmd(a));
    }
    put(B.select(s.slot));
    put(B.act(0, s.signal, s.place));   // hold (raptor mode 0)
    put(B.show(s.slot, false));
    if (s.kind == SCENE_LEAP_IN) {
        // ST10A sub 8: event flags, the camera on the window, the leap through the wall, the glass, back to its AI
        put(B.flag(1, 2, 1));
        put(B.flag(2, 2, 1));
        put(B.camera(s.eye, s.target));
        put(B.frame());
        put(B.select(s.slot));
        put(B.set(16, 0x8000));             // walls do not stop it while it leaps
        put(B.flag(3, s.signal, 0));
        put(B.act(4, s.signal, s.land));    // leap to the point inside, then flag 3/signal
        put(B.wait(10));
        put(B.show(s.slot, true));
        put(B.sound_at(0x90, s.place));     // the window breaks
        // wait for the landing, 3 s at most (the loop never leaves early: a FOR left by a jump stays open)
        {
            ScriptAtom t = Builder::op(0x26);
            t.b[1] = 3; t.b[2] = (uint8_t)s.signal;
            std::vector<ScnNode> w;
            w.push_back(B.if_(t, 1, {B.frame()}));
            put(B.repeat(90, std::move(w)));
        }
        put(B.flag(3, s.signal, 0));
        put(B.select(s.slot));
        put(B.set(16, 0));
        put(B.release(2));                  // back to its AI (2: where the raptor's 0x3A moves return)
        put(B.wait(15));                    // a moment of it standing there, then the room cameras
        put(B.b4(0x30, 0, 0, 0));
        put(B.flag(1, 2, 0));
        put(B.flag(2, 2, 0));
    } else {
        // the follower scene of 0x5B: event flags, two sounds at the opening, Regina stops, the cut, the entrance
        put(B.flag(1, 2, 1));
        put(B.flag(1, 5, 1));
        put(B.sound_at(7, s.place));
        put(B.wait(5));
        put(B.sound_at(8, s.place));
        put(B.wait(2));
        put(B.select(10));
        {
            const int zero[3] = {0, 0, 0};
            put(B.act(5, s.signal, zero));   // Regina stops (human mode 5)
        }
        put(B.wait(20));
        put(B.show(10, false));
        put(B.camera(s.eye, s.target));
        put(B.select(s.slot));
        put(B.show(s.slot, true));
        put(B.release(0x25));               // AI word 0x2501: the raptor's entrance
        // while it enters (state byte 1 stays 0x25), 5 s at most
        {
            std::vector<ScnNode> w;
            w.push_back(B.select(s.slot));
            ScriptAtom c = Builder::op(0x29);
            c.b[2] = 11;
            put32(&c.b[4], 0x25);
            w.push_back(B.if_(c, 6, {B.frame()}));
            put(B.repeat(150, std::move(w)));
        }
        put(B.b4(0x30, 0, 0, 0));           // the room cameras
        put(B.select(10));
        put(B.show(10, true));
        put(B.release(0));                  // the player has Regina again
        put(B.flag(3, s.signal, 0));
        put(B.flag(1, 2, 0));
        put(B.flag(1, 5, 0));
    }
    // all of it only while the raptor is alive: killed, it does not come again
    ScriptAtom t = Builder::op(0x26);
    t.b[1] = 4; t.b[2] = (uint8_t)s.death_flag;
    return B.if_(t, 1, std::move(body));
}

namespace {

bool is_op(const std::vector<ScriptAtom>& at, const ScnNode& n, uint8_t op) {
    return n.k == ScnNode::CMD && n.atom >= 0 && at[n.atom].kind == ScriptAtom::OP && at[n.atom].b.size() >= 4 && at[n.atom].b[0] == op;
}
bool is4(const std::vector<ScriptAtom>& at, const ScnNode& n, uint8_t op, int b1, int b2, int b3) {
    if (!is_op(at, n, op)) return false;
    const Bytes& b = at[n.atom].b;
    return (b1 < 0 || b[1] == b1) && (b2 < 0 || b[2] == b2) && (b3 < 0 || b[3] == b3);
}
bool is_leap(const std::vector<ScriptAtom>& at, const ScnNode& x) { return is4(at, x, 0x3A, -1, 4, -1) && at[x.atom].b.size() >= 10; }
bool is_cam(const std::vector<ScriptAtom>& at, const ScnNode& x) { return is_op(at, x, 0x2E) && at[x.atom].b.size() >= 14; }

bool fix_one(const ScnNode& n, std::vector<ScriptAtom>& at) {
    ScnScene s;
    if (!scn_scene_detect(at, n, s)) return false;
    for (size_t i = 2; i < n.a.size(); i++) {
        const ScnNode& x = n.a[i];
        const bool snd = is_op(at, x, 0x55) && at[x.atom].b.size() >= 10 && at[x.atom].b[2] == 0;
        if (i == 2 || snd)
            for (int k = 0; k < 3; k++) put16(&at[x.atom].b[4 + k * 2], (uint16_t)(int16_t)s.place[k]);
    }
    return true;
}

void fix_nodes(std::vector<ScnNode>& ns, std::vector<ScriptAtom>& at) {
    for (auto& n : ns) {
        if (fix_one(n, at)) continue;
        fix_nodes(n.a, at);
        fix_nodes(n.b, at);
    }
}

}  // namespace

bool scn_scene_detect(const std::vector<ScriptAtom>& at, const ScnNode& n, ScnScene& s) {
    if (n.k != ScnNode::IF || n.cond.size() != 1 || n.atom2 >= 0 || n.a.size() < 8) return false;
    const ScriptAtom& c = at[n.cond[0]];
    const ScriptAtom& j = at[n.atom];
    if (c.kind != ScriptAtom::OP || c.b.size() < 4 || c.b[0] != 0x26 || c.b[1] != 4 || j.b.size() < 2 || j.b[1] != 1) return false;
    const int df = c.b[2];
    if (!is_op(at, n.a[0], 0x20) || at[n.a[0].atom].b.size() < 24 || at[n.a[0].atom].b[4] != df) return false;
    const ScriptAtom& e = at[n.a[0].atom];
    const int slot = e.b[1];
    if (!is4(at, n.a[1], 0x22, 2, slot, -1) || !is4(at, n.a[2], 0x3A, -1, 0, -1) || at[n.a[2].atom].b.size() < 10 ||
        !is4(at, n.a[3], 0x48, 2, slot, 0) || !is4(at, n.a[4], 0x25, 1, 2, 1))
        return false;
    int kind;
    if (is4(at, n.a[5], 0x25, 2, 2, 1)) kind = SCENE_LEAP_IN;
    else if (is4(at, n.a[5], 0x25, 1, 5, 1)) kind = SCENE_COME_IN;
    else return false;
    s = ScnScene();
    s.kind = kind;
    s.slot = slot;
    s.death_flag = df;
    s.signal = at[n.a[2].atom].b[3];
    s.dkind = e.b[2];
    s.variant = e.b[3];
    s.ai = e.b[5];
    s.hp = le16(&e.b[6]);
    for (int k = 0; k < 3; k++) s.place[k] = (int16_t)le16(&e.b[8 + k * 2]);
    s.turn = le16(&e.b[14]) & 4095;
    s.model = le32(&e.b[16]);
    s.motions = le32(&e.b[20]);
    s.from = e.from;
    for (auto& x : n.a) {
        if (is_cam(at, x))
            for (int k = 0; k < 3; k++) {
                s.target[k] = (int16_t)le16(&at[x.atom].b[2 + k * 2]);
                s.eye[k] = (int16_t)le16(&at[x.atom].b[8 + k * 2]);
            }
        if (kind == SCENE_LEAP_IN && is_leap(at, x))
            for (int k = 0; k < 3; k++) s.land[k] = (int16_t)le16(&at[x.atom].b[4 + k * 2]);
    }
    return true;
}

std::vector<ScnPoint> scn_scene_points(const std::vector<ScriptAtom>& at, const ScnNode& n) {
    std::vector<ScnPoint> r;
    ScnScene s;
    if (!scn_scene_detect(at, n, s)) return r;
    r.push_back({"raptor", 8, 14, n.a[0].atom});
    if (s.kind == SCENE_LEAP_IN)
        for (auto& x : n.a)
            if (is_leap(at, x)) { r.push_back({"lands", 4, -1, x.atom}); break; }
    for (auto& x : n.a)
        if (is_cam(at, x)) {
            r.push_back({"camera", 8, -1, x.atom});
            r.push_back({"looks at", 2, -1, x.atom});
            break;
        }
    return r;
}

void scn_scenes_fix(Scenario& s, std::vector<ScriptAtom>& atoms) {
    for (auto& g : s.regions) fix_nodes(g.nodes, atoms);
}

void scn_scene_apply(std::vector<ScriptAtom>& at, const ScnNode& n, const ScnScene& s) {
    ScnScene cur;
    if (!scn_scene_detect(at, n, cur)) return;
    ScriptAtom& e = at[n.a[0].atom];
    e.b[3] = s.variant;
    put16(&e.b[6], s.hp);
    for (int k = 0; k < 3; k++) put16(&e.b[8 + k * 2], (uint16_t)(int16_t)s.place[k]);
    put16(&e.b[14], (uint16_t)(s.turn & 4095));
    for (auto& x : n.a) {
        if (is_cam(at, x))
            for (int k = 0; k < 3; k++) {
                put16(&at[x.atom].b[2 + k * 2], (uint16_t)(int16_t)s.target[k]);
                put16(&at[x.atom].b[8 + k * 2], (uint16_t)(int16_t)s.eye[k]);
            }
        if (s.kind == SCENE_LEAP_IN && is_leap(at, x))
            for (int k = 0; k < 3; k++) put16(&at[x.atom].b[4 + k * 2], (uint16_t)(int16_t)s.land[k]);
    }
    fix_one(n, at);
}

bool scn_scene_add(Scenario& s, std::vector<ScriptAtom>& at, int region, const std::string& key, const ScnScene& sc,
                   int wait, std::string& err) {
    if (region < 0 || region >= (int)s.regions.size()) { err = "No event picked"; return false; }
    // a free task slot (1..8): not one the room's events or run zones use
    std::set<int> slots = {0, 9};
    int dir = -1;
    for (int i = 0; i < (int)at.size(); i++) {
        const ScriptAtom& x = at[i];
        if (x.kind == ScriptAtom::DIR && dir < 0) dir = i;
        if (x.kind != ScriptAtom::OP || x.b.size() < 4) continue;
        if (x.b[0] == 0x01) slots.insert(x.b[1]);
        if (x.b[0] == 0x28 && x.b.size() >= 29 && x.b[2] == 3) slots.insert(x.b[28]);
    }
    int slot = -1;
    for (int k = 1; k <= 8 && slot < 0; k++) if (!slots.count(k)) slot = k;
    if (slot < 0) { err = "No free task slot (a room runs 8 at most)"; return false; }
    if (dir < 0) { err = "The script has no event list"; return false; }
    auto add = [&](ScriptAtom a) {
        a.nid = script_next_nid(at);
        at.push_back(std::move(a));
        return (int)at.size() - 1;
    };
    // the new event: a wait (the room is seen first), the scene, End
    ScnRegion g;
    ScnNode w, e;
    {
        ScriptAtom wa = scn_new_atom(0x02);
        put16(&wa.b[2], (uint16_t)std::max(1, wait));
        w.atom = add(wa);
    }
    g.nodes.push_back(w);
    g.nodes.push_back(scn_scene_build(at, sc));
    e.atom = add(scn_new_atom(0x04));
    g.nodes.push_back(e);
    int sub;
    {
        ScriptAtom& d = at[dir];
        ScriptSym sy;
        sy.at = (int)d.b.size();
        sy.type = 2;
        sy.tnew = at[w.atom].nid;
        d.b.insert(d.b.end(), 4, 0);
        d.syms.push_back(sy);
        sub = (int)d.b.size() / 4 - 1;
    }
    g.subs.push_back(sub);
    // the line that starts it, after the picked one
    ScriptAtom st = scn_new_atom(0x01);
    st.b[1] = (uint8_t)slot;
    put16(&st.b[2], (uint16_t)sub);
    ScnNode sn;
    sn.atom = add(st);
    std::vector<ScnNode>* list = nullptr;
    int pos = 0;
    // nothing picked: right after the event's first line (its last End is often not reached - ST106 room start returns on
    // most of its branches; before the first line is before the event's entry). The scene waits in its own task, so
    // starting it early holds nothing up
    if (key.empty()) {
        list = &s.regions[region].nodes;
        while (pos < (int)list->size() && (*list)[pos].k == ScnNode::DATA) pos++;
        const ScnNode* f = pos < (int)list->size() ? &(*list)[pos] : nullptr;
        if (f && !(f->k == ScnNode::CMD && at[f->atom].kind == ScriptAtom::OP && !at[f->atom].b.empty() && at[f->atom].b[0] == 0x04)) pos++;
    } else {
        scn_insert_point(s.regions[region], at, key, list, pos);
    }
    if (!list) { err = "No place for it in this event"; return false; }
    list->insert(list->begin() + std::min(pos, (int)list->size()), sn);
    s.regions.push_back(std::move(g));
    return true;
}

}  // namespace dc
