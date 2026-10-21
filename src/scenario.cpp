// 10.10.2026: the room script as a tree (scenario.h). Parsing is conservative: a block becomes an IF / LOOP only when
// nothing jumps into it from outside and nothing in it jumps out (except to the place right after it), so flattening the
// tree can re-point the block's own jumps by structure without changing what the game runs.
#include "scenario.h"
#include "scn_ops.h"

#include <algorithm>
#include <map>
#include <set>

namespace dc {

bool scn_is_cond(const ScriptAtom& a) {
    if (a.kind != ScriptAtom::OP || a.b.empty()) return false;
    return scn_is_cond_op(a.b[0]);   // 14.10.2026: TESTFLAG, COMPARE, has item, dinosaur in room (scn_ops.cpp)
}

namespace {

struct Ctx {
    const std::vector<ScriptAtom>& at;
    std::vector<std::vector<int>> jt;   // per atom: target atoms of its jumps (s16 jumps only), -1 unknown
    std::vector<int> nin;               // per atom: how many jumps land on it
    std::vector<std::vector<int>> from; // per atom: which atoms jump to it
    explicit Ctx(const std::vector<ScriptAtom>& a) : at(a) {}
};

bool is_jump_op(const ScriptAtom& a) {
    if (a.kind != ScriptAtom::OP || a.b.empty()) return false;
    const uint8_t op = a.b[0];
    return op == 0x09 || op == 0x0A || op == 0x0B || op == 0x0C || op == 0x0E || op == 0x17;
}

// every jump of every atom in [i0, i1) lands in [lo, hi] (inclusive hi: the place right after the block)
bool jumps_stay(const Ctx& c, int i0, int i1, int lo, int hi, int skip = -1) {
    for (int i = i0; i < i1; i++) {
        if (i == skip) continue;
        for (int t : c.jt[i])
            if (t < lo || t > hi) return false;
    }
    return true;
}

// nothing outside [i0, i1) jumps to an atom inside (i0 excluded unless check_first)
bool no_way_in(const Ctx& c, int i0, int i1, bool check_first, int allowed_from = -1) {
    for (int i = check_first ? i0 : i0 + 1; i < i1; i++)
        for (int f : c.from[i])
            if ((f < i0 || f >= i1) && f != allowed_from) return false;
    return true;
}

void parse_block(const Ctx& c, int i0, int i1, std::vector<ScnNode>& out) {
    for (int i = i0; i < i1;) {
        const ScriptAtom& a = c.at[i];
        if (a.kind != ScriptAtom::OP) {
            ScnNode n;
            n.k = ScnNode::DATA;
            n.atom = i;
            out.push_back(std::move(n));
            i++;
            continue;
        }
        const uint8_t op = a.b.empty() ? 0 : a.b[0];
        // IF: conditional forward jump over a block that keeps to itself
        if (op == 0x0E && a.b.size() >= 4 && a.b[1] != 0 && c.jt[i].size() == 1 && c.jt[i][0] > i && c.jt[i][0] <= i1 &&
            no_way_in(c, i + 1, c.jt[i][0], true)) {
            const int t = c.jt[i][0];
            // ELSE: the block ends with an unconditional jump over another block that keeps to itself and is entered only
            // through this IF (checked first: that jump leaves the THEN block, so the plain IF test below refuses it)
            int t2 = -1;
            const int j = t - 1;
            if (j > i && c.at[j].kind == ScriptAtom::OP && c.at[j].b.size() >= 4 && c.at[j].b[0] == 0x0E && c.at[j].b[1] == 0 &&
                c.jt[j].size() == 1) {
                const int e = c.jt[j][0];
                if (e > t && e <= i1 && c.from[t].size() == 1 && c.from[t][0] == i && no_way_in(c, t, e, false) &&
                    jumps_stay(c, t, e, t, e) && jumps_stay(c, i + 1, j, i + 1, j))
                    t2 = e;
            }
            if (t2 >= 0 || jumps_stay(c, i + 1, t, i + 1, t)) {
                ScnNode n;
                n.k = ScnNode::IF;
                n.atom = i;
                // the condition command just before the jump (it was emitted as a plain command); 14.10.2026: only that
                // one - a test before it was overwritten by it, so it stays a line of its own
                if (!out.empty() && out.back().k == ScnNode::CMD && scn_is_cond(c.at[out.back().atom]) && out.back().atom == i - 1) {
                    n.cond.push_back(out.back().atom);
                    out.pop_back();
                }
                if (t2 >= 0) {
                    n.atom2 = j;
                    parse_block(c, i + 1, j, n.a);
                    parse_block(c, t, t2, n.b);
                    out.push_back(std::move(n));
                    i = t2;
                } else {
                    parse_block(c, i + 1, t, n.a);
                    out.push_back(std::move(n));
                    i = t;
                }
                continue;
            }
        }
        // LOOP: FOR (exit jump forward) ... NEXT (jump back to the first command of the body) right before the exit
        if (op == 0x09 && c.jt[i].size() == 1) {
            const int x = c.jt[i][0], j = x - 1;
            if (x > i + 1 && x <= i1 && c.at[j].kind == ScriptAtom::OP && !c.at[j].b.empty() && c.at[j].b[0] == 0x0A && c.jt[j].size() == 1 &&
                c.jt[j][0] == i + 1 && i + 1 < j && no_way_in(c, i + 1, j + 1, false, j) && jumps_stay(c, i + 1, j, i + 1, j)) {
                ScnNode n;
                n.k = ScnNode::LOOP;
                n.atom = i;
                n.atom2 = j;
                parse_block(c, i + 1, j, n.a);
                out.push_back(std::move(n));
                i = x;
                continue;
            }
        }
        ScnNode n;
        n.k = ScnNode::CMD;
        n.atom = i;
        out.push_back(std::move(n));
        i++;
    }
}

}  // namespace

Scenario scn_parse(const ScriptImage& orig, const std::vector<ScriptAtom>& atoms) {
    Scenario s;
    s.dir = script_dir_atoms(orig, atoms);
    Ctx c(atoms);
    const int n = (int)atoms.size();
    c.jt.assign(n, {});
    c.nin.assign(n, 0);
    c.from.assign(n, {});
    std::map<int64_t, int> by_old;
    std::map<int, int> by_nid;
    for (int i = 0; i < n; i++) {
        if (atoms[i].old >= 0 && atoms[i].kind != ScriptAtom::DIR) by_old[atoms[i].old] = i;
        if (atoms[i].nid >= 0) by_nid[atoms[i].nid] = i;
    }
    for (int i = 0; i < n; i++) {
        if (!is_jump_op(atoms[i])) continue;
        for (const ScriptSym& sy : atoms[i].syms) {
            if (sy.type == 2) continue;
            int t = -1;
            if (sy.tnew >= 0) {
                auto it = by_nid.find(sy.tnew);
                if (it != by_nid.end()) t = it->second;
            } else {
                auto it = by_old.find(sy.target);
                if (it != by_old.end()) t = it->second;
            }
            c.jt[i].push_back(t);
            if (t >= 0) {
                c.nin[t]++;
                c.from[t].push_back(i);
            }
        }
    }
    // regions: from each subroutine entry to the next one (in list order)
    std::map<int, std::vector<int>> starts;
    for (size_t k = 0; k < s.dir.size(); k++) if (s.dir[k] >= 0) starts[s.dir[k]].push_back((int)k);
    std::vector<std::pair<int, std::vector<int>>> cuts;
    cuts.push_back({0, {}});
    for (auto& kv : starts) {
        if (kv.first == 0) cuts[0].second = kv.second;
        else cuts.push_back(kv);
    }
    for (size_t r = 0; r < cuts.size(); r++) {
        ScnRegion g;
        g.subs = cuts[r].second;
        g.first = cuts[r].first;
        g.end = r + 1 < cuts.size() ? cuts[r + 1].first : n;
        // a jump out of a region is fine (shared code): blocks are only folded when they keep to themselves
        parse_block(c, g.first, g.end, g.nodes);
        s.regions.push_back(std::move(g));
    }
    return s;
}

namespace {

struct Flat {
    std::vector<int> order;                            // atom indices in the new order
    struct Fix { int atom; int sym_at; int target_pos; };   // jump field of atom -> the atom at position target_pos
    std::vector<Fix> fixes;
};

void flat_nodes(const std::vector<ScnNode>& ns, Flat& f);

void flat_node(const ScnNode& n, Flat& f) {
    switch (n.k) {
    case ScnNode::CMD:
    case ScnNode::DATA:
        f.order.push_back(n.atom);
        break;
    case ScnNode::IF: {
        for (int a : n.cond) f.order.push_back(a);
        f.order.push_back(n.atom);
        const size_t fix_if = f.fixes.size();
        f.fixes.push_back({n.atom, 2, -1});
        flat_nodes(n.a, f);
        if (n.atom2 >= 0) {
            f.order.push_back(n.atom2);
            const size_t fix_else = f.fixes.size();
            f.fixes.push_back({n.atom2, 2, -1});
            f.fixes[fix_if].target_pos = (int)f.order.size();   // the first command of ELSE
            flat_nodes(n.b, f);
            f.fixes[fix_else].target_pos = (int)f.order.size(); // what follows the ELSE block
        } else {
            f.fixes[fix_if].target_pos = (int)f.order.size();   // what follows the block
        }
        break;
    }
    case ScnNode::LOOP: {
        f.order.push_back(n.atom);
        const size_t fix_for = f.fixes.size();
        f.fixes.push_back({n.atom, 2, -1});
        const int body = (int)f.order.size();
        flat_nodes(n.a, f);
        f.order.push_back(n.atom2);
        f.fixes.push_back({n.atom2, 2, body});              // NEXT: back to the first command of the body
        f.fixes[fix_for].target_pos = (int)f.order.size();  // FOR: out, past NEXT
        break;
    }
    }
}

void flat_nodes(const std::vector<ScnNode>& ns, Flat& f) {
    for (const ScnNode& n : ns) flat_node(n, f);
}

}  // namespace

std::vector<int> scn_order(const Scenario& s) {
    Flat f;
    for (const ScnRegion& g : s.regions) flat_nodes(g.nodes, f);
    return f.order;
}

void scn_retarget_gone(const Scenario& s, std::vector<ScriptAtom>& atoms, const std::vector<int>& gone) {
    const std::vector<int> ord = scn_order(s);
    std::set<int> g(gone.begin(), gone.end());
    std::map<int, int> succ;   // nid of a gone atom -> the atom that follows it
    for (size_t p = 0; p < ord.size(); p++) {
        if (!g.count(ord[p]) || atoms[ord[p]].nid < 0) continue;
        size_t q = p + 1;
        while (q < ord.size() && g.count(ord[q])) q++;
        if (q < ord.size()) succ[atoms[ord[p]].nid] = ord[q];
    }
    if (succ.empty()) return;
    int nid = script_next_nid(atoms);
    for (auto& a : atoms)
        for (auto& sy : a.syms) {
            auto it = sy.tnew >= 0 ? succ.find(sy.tnew) : succ.end();
            if (it == succ.end()) continue;
            ScriptAtom& t = atoms[it->second];
            if (t.old >= 0 && t.kind != ScriptAtom::DIR) { sy.target = (uint32_t)t.old; sy.tnew = -1; }
            else { if (t.nid < 0) t.nid = nid++; sy.tnew = t.nid; }
        }
}

bool scn_flatten(const Scenario& s, std::vector<ScriptAtom>& atoms, std::string& err) {
    Flat f;
    for (const ScnRegion& g : s.regions) flat_nodes(g.nodes, f);
    std::vector<char> seen(atoms.size(), 0);
    for (int a : f.order) {
        if (a < 0 || a >= (int)atoms.size() || seen[a]) { err = "internal error: a command is twice in the scenario"; return false; }
        seen[a] = 1;
    }
    std::vector<ScriptAtom> out;
    out.reserve(f.order.size() + 1);
    std::vector<int> pos_of(atoms.size(), -1);
    for (size_t p = 0; p < f.order.size(); p++) pos_of[f.order[p]] = (int)p;
    int nid = script_next_nid(atoms);
    // the targets of structured jumps: the atom at a position (the position past the end: nothing follows - an error)
    std::vector<ScriptAtom> work = atoms;
    for (const Flat::Fix& x : f.fixes) {
        if (x.target_pos < 0 || x.target_pos >= (int)f.order.size()) {
            err = "a block is at the very end of the script: nothing follows it (add RETURN)";
            return false;
        }
        ScriptAtom& j = work[x.atom];
        ScriptAtom& t = work[f.order[x.target_pos]];
        ScriptSym* sy = nullptr;
        for (ScriptSym& z : j.syms) if (z.at == x.sym_at && z.type != 2) sy = &z;
        if (!sy) {
            ScriptSym z;
            z.at = x.sym_at;
            z.type = j.b[0] == 0x0A ? 1 : 0;
            j.syms.push_back(z);
            sy = &j.syms.back();
        }
        if (t.old >= 0 && t.kind != ScriptAtom::DIR) {
            sy->target = (uint32_t)t.old;
            sy->tnew = -1;
        } else {
            if (t.nid < 0) t.nid = nid++;
            sy->tnew = t.nid;
        }
    }
    for (int a : f.order) out.push_back(work[a]);
    // labels given above to new atoms that jumps point at: carry them over (work holds them)
    atoms = std::move(out);
    return true;
}

// ---------------------------------------------------------------- edits
std::string scn_key(const ScriptAtom& a) { return a.old >= 0 ? strfmt("@%04X", (unsigned)a.old) : strfmt("#%d", a.nid); }

namespace {

bool find_line(std::vector<ScnNode>& ns, const std::vector<ScriptAtom>& at, const std::string& key, std::vector<ScnNode>*& list, int& idx) {
    for (int i = 0; i < (int)ns.size(); i++) {
        ScnNode& n = ns[i];
        if (n.atom >= 0 && scn_key(at[n.atom]) == key) { list = &ns; idx = i; return true; }
        if (find_line(n.a, at, key, list, idx) || find_line(n.b, at, key, list, idx)) return true;
    }
    return false;
}

bool parent_line(std::vector<ScnNode>& ns, const std::vector<ScnNode>* child, std::vector<ScnNode>*& plist, int& pidx) {
    for (int i = 0; i < (int)ns.size(); i++) {
        ScnNode& n = ns[i];
        if (&n.a == child || &n.b == child) { plist = &ns; pidx = i; return true; }
        if (parent_line(n.a, child, plist, pidx) || parent_line(n.b, child, plist, pidx)) return true;
    }
    return false;
}

void node_atoms(const ScnNode& n, std::vector<int>& out) {
    if (n.atom >= 0) out.push_back(n.atom);
    if (n.atom2 >= 0) out.push_back(n.atom2);
    for (int c : n.cond) out.push_back(c);
    for (auto& x : n.a) node_atoms(x, out);
    for (auto& x : n.b) node_atoms(x, out);
}

}  // namespace

void scn_insert_point(ScnRegion& g, const std::vector<ScriptAtom>& atoms, const std::string& key, std::vector<ScnNode>*& list, int& pos) {
    const bool el = key.rfind("else:", 0) == 0;
    std::vector<ScnNode>* l = nullptr;
    int i = 0;
    if (!key.empty() && find_line(g.nodes, atoms, el ? key.substr(5) : key, l, i)) {
        ScnNode& n = (*l)[i];
        if (el && n.k == ScnNode::IF) { list = &n.b; pos = 0; return; }
        if (n.k == ScnNode::IF || n.k == ScnNode::LOOP) { list = &n.a; pos = 0; return; }
        list = l;
        pos = i + 1;
        return;
    }
    list = &g.nodes;
    pos = (int)g.nodes.size();
    for (int k = (int)g.nodes.size() - 1; k >= 0; k--) {
        const ScnNode& n = g.nodes[k];
        if (n.k == ScnNode::DATA) continue;
        if (n.k == ScnNode::CMD && !atoms[n.atom].b.empty() && atoms[n.atom].b[0] == 0x04) pos = k;
        break;
    }
}

bool scn_is_final_end(const ScnRegion& g, const std::vector<ScriptAtom>& atoms, const std::string& key) {
    for (int k = (int)g.nodes.size() - 1; k >= 0; k--) {
        const ScnNode& n = g.nodes[k];
        if (n.k == ScnNode::DATA) continue;
        return n.k == ScnNode::CMD && !atoms[n.atom].b.empty() && atoms[n.atom].b[0] == 0x04 && scn_key(atoms[n.atom]) == key;
    }
    return false;
}

bool scn_remove(Scenario& s, std::vector<ScriptAtom>& atoms, int region, const std::string& key) {
    if (region < 0 || region >= (int)s.regions.size()) return false;
    ScnRegion& g = s.regions[region];
    if (scn_is_final_end(g, atoms, key)) return false;
    const bool el = key.rfind("else:", 0) == 0;
    std::vector<ScnNode>* l = nullptr;
    int i = 0;
    if (!find_line(g.nodes, atoms, el ? key.substr(5) : key, l, i)) return false;
    ScnNode& n = (*l)[i];
    std::vector<int> gone;
    if (el) {
        if (n.k != ScnNode::IF || n.atom2 < 0) return false;
        gone.push_back(n.atom2);
        for (auto& x : n.b) node_atoms(x, gone);
    } else {
        node_atoms(n, gone);
    }
    scn_retarget_gone(s, atoms, gone);
    if (el) {
        n.atom2 = -1;
        n.b.clear();
    } else {
        l->erase(l->begin() + i);
    }
    return true;
}

bool scn_move(ScnRegion& g, const std::vector<ScriptAtom>& atoms, const std::string& key, int dir) {
    if (key.rfind("else:", 0) == 0 || scn_is_final_end(g, atoms, key)) return false;
    std::vector<ScnNode>* list = nullptr;
    int i = 0;
    if (!find_line(g.nodes, atoms, key, list, i)) return false;
    ScnNode n = (*list)[i];
    const int j = i + dir;
    if (j >= 0 && j < (int)list->size()) {
        ScnNode& nb = (*list)[j];
        if (nb.k == ScnNode::IF || nb.k == ScnNode::LOOP) {   // into the neighbouring block
            list->erase(list->begin() + i);
            ScnNode& b2 = (*list)[dir > 0 ? i : j];
            std::vector<ScnNode>& into = dir > 0 ? b2.a : (b2.atom2 >= 0 ? b2.b : b2.a);
            if (dir > 0) into.insert(into.begin(), n);
            else into.push_back(n);
            return true;
        }
        if (nb.k == ScnNode::CMD && list == &g.nodes && scn_is_final_end(g, atoms, scn_key(atoms[nb.atom]))) return false;
        std::swap((*list)[i], (*list)[j]);
        return true;
    }
    std::vector<ScnNode>* plist = nullptr;
    int pidx = 0;
    if (!parent_line(g.nodes, list, plist, pidx)) return false;   // the top of the event: stays
    list->erase(list->begin() + i);
    plist->insert(plist->begin() + (dir > 0 ? pidx + 1 : pidx), n);
    return true;
}

}  // namespace dc
