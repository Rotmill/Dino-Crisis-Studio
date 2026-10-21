#include "script_edit.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include "placement.h"
#include "script.h"

namespace dc {

int ScriptImage::find(uint32_t old) const {
    for (size_t i = 0; i < atoms.size(); i++)
        if (atoms[i].old == (int64_t)old) return (int)i;
    return -1;
}

int ScriptImage::containing(uint32_t old) const {
    for (size_t i = 0; i < atoms.size(); i++) {
        const ScriptAtom& a = atoms[i];
        if (a.old >= 0 && old >= (uint64_t)a.old && old < (uint64_t)a.old + a.old_len) return (int)i;
    }
    return -1;
}

static bool is_ctrl16(uint16_t w) { return w == 0xC000 || w == 0x0100 || w == 0x0800 || w == 0x0802 || w == 0x9000 || (w >> 12) == 1 || (w >> 12) == 2; }

void script_op_syms(ScriptAtom& a, uint32_t size) {
    a.syms.clear();
    if (a.b.size() < 4) return;
    const uint8_t op = a.b[0];
    const int64_t pc = a.old;
    if (pc >= 0 && (op == 0x09 || op == 0x0B || op == 0x0C || op == 0x0E || op == 0x17)) {
        const int64_t t = pc + (int16_t)le16(&a.b[2]);
        if (t >= 0 && t <= (int64_t)size) a.syms.push_back({2, 0, (uint32_t)t});
    }
    if (pc >= 0 && op == 0x0A) {
        const int64_t t = pc - (int16_t)le16(&a.b[2]);
        if (t >= 0 && t <= (int64_t)size) a.syms.push_back({2, 1, (uint32_t)t});
    }
    auto off32 = [&](int at) {
        if ((size_t)at + 4 > a.b.size()) return;
        const uint32_t v = le32(&a.b[at]);
        if (v < size) a.syms.push_back({at, 2, v});
    };
    if (op == 0x21 || op == 0x67) off32(4);
    if (op == 0x28 && a.b[2] == 1) off32(28);
    if (op == 0x28 && a.b[2] == 7) off32(40);
}

void script_op_syms(ScriptAtom& a) { script_op_syms(a, 0x7FFFFFFF); }

bool script_decompose(const Bytes& blk, const RoomInfo& r, ScriptImage& im, std::string& err) {
    im = ScriptImage();
    if (!r.script_size || r.script_dir.empty() || r.script_off + r.script_size > blk.size()) { err = "the room has no script"; return false; }
    im.off = r.script_off;
    im.size = r.script_size;
    im.dir = r.script_dir;
    const uint32_t so = r.script_off, size = r.script_size;
    const uint32_t dsz = (uint32_t)im.dir.size() * 4;
    std::vector<int> cov(size, -1);   // −1 свободно, −2 каталог, иначе номер команды
    for (uint32_t i = 0; i < dsz && i < size; i++) cov[i] = -2;
    auto ops = script_walk(blk, so, size, im.dir);
    std::vector<ScriptAtom> out;
    {
        ScriptAtom d;
        d.kind = ScriptAtom::DIR;
        d.old = 0;
        d.old_len = dsz;
        d.b.assign(blk.begin() + so, blk.begin() + so + dsz);
        for (size_t i = 0; i < im.dir.size(); i++) d.syms.push_back({(int)i * 4, 2, im.dir[i]});
        out.push_back(std::move(d));
    }
    for (auto& o : ops) {
        if (o.pc < dsz || o.pc + (uint32_t)o.size > size) continue;
        bool clash = false;
        for (int k = 0; k < o.size && !clash; k++) clash = cov[o.pc + k] != -1;
        if (clash) continue;
        for (int k = 0; k < o.size; k++) cov[o.pc + k] = (int)out.size();
        ScriptAtom a;
        a.kind = ScriptAtom::OP;
        a.old = o.pc;
        a.old_len = (uint32_t)o.size;
        a.sub = o.sub;
        a.b.assign(blk.begin() + so + o.pc, blk.begin() + so + o.pc + o.size);
        script_op_syms(a, size);
        out.push_back(std::move(a));
    }
    // строки текста (как их видит приложение — коды текста; начало — с управляющими перед ними)
    for (auto& gs : r.strings) {
        if (gs.off < so || gs.off >= so + size || gs.codes.empty()) continue;
        uint32_t a0 = gs.off - so, e = a0 + (uint32_t)gs.codes.size() * 2;
        if (e > size) continue;
        bool clash = false;
        for (uint32_t k = a0; k < e && !clash; k++) clash = cov[k] != -1;
        if (clash) continue;
        uint32_t s = a0;
        while (s >= 2 && cov[s - 1] == -1 && cov[s - 2] == -1 && is_ctrl16(le16(&blk[so + s - 2]))) s -= 2;
        for (uint32_t k = s; k < e; k++) cov[k] = (int)out.size();
        ScriptAtom a;
        a.kind = ScriptAtom::STRING;
        a.old = s;
        a.old_len = e - s;
        a.text_from = a0 - s;
        a.b.assign(blk.begin() + so + s, blk.begin() + so + e);
        out.push_back(std::move(a));
    }
    // 14.10.2026: texts the room's list misses (codes 0x9000 / 0x2000 inside) stay data here: script.txt of mods names
    // them "data @XXXX", so the atoms must not change; the scenario reads their words with script_text_quote
    // прочее — данные как есть
    for (uint32_t i = 0; i < size;) {
        if (cov[i] != -1) { i++; continue; }
        uint32_t j = i;
        while (j < size && cov[j] == -1) j++;
        ScriptAtom a;
        a.kind = ScriptAtom::DATA;
        a.old = i;
        a.old_len = j - i;
        a.b.assign(blk.begin() + so + i, blk.begin() + so + j);
        out.push_back(std::move(a));
        i = j;
    }
    std::sort(out.begin(), out.end(), [](const ScriptAtom& x, const ScriptAtom& y) { return x.old < y.old; });
    im.atoms = std::move(out);
    return true;
}

// ---------------------------------------------------------------- сборка
namespace {
struct Placed { uint32_t old, old_len, nw, nw_len; };
}

bool script_assemble(const ScriptImage& orig, const std::vector<ScriptAtom>& atoms, Bytes& sec, std::string& err) {
    std::vector<uint32_t> start(atoms.size());
    uint32_t pos = 0;
    for (size_t i = 0; i < atoms.size(); i++) {
        start[i] = pos;
        pos += (uint32_t)atoms[i].b.size();
    }
    // уцелевшие прежние атомы: старое место → новое
    std::vector<Placed> pl;
    for (size_t i = 0; i < atoms.size(); i++)
        if (atoms[i].old >= 0) pl.push_back({(uint32_t)atoms[i].old, atoms[i].old_len, start[i], (uint32_t)atoms[i].b.size()});
    std::sort(pl.begin(), pl.end(), [](const Placed& a, const Placed& b) { return a.old < b.old; });
    for (size_t i = 0; i < atoms.size(); i++) {
        const ScriptAtom& a = atoms[i];
        if (a.old >= 0 && ((int64_t)start[i] - a.old) % 4 != 0) {
            err = strfmt("the script would shift by a number of bytes not divisible by 4 at @%04X: commands must stay aligned", (unsigned)a.old);
            return false;
        }
    }
    // старое место t → новое: внутри уцелевшего — со сдвигом; в удалённом — туда, где продолжается прежний поток
    // (за предыдущим уцелевшим: если на месте удалённого вставили новое — на него)
    auto newpos = [&](uint32_t t) -> uint32_t {
        auto it = std::upper_bound(pl.begin(), pl.end(), t, [](uint32_t v, const Placed& p) { return v < p.old; });
        if (it != pl.begin()) {
            const Placed& p = *(it - 1);
            if (t < p.old + p.old_len) return p.nw + std::min(t - p.old, p.nw_len);
            return p.nw + p.nw_len;   // за ним (удалённое после него)
        }
        return 0;
    };
    // 10.10.2026: new atoms (labels #N) - jumps and the directory may point at them; commands stay 4-byte aligned
    std::map<int, uint32_t> nid_at;
    for (size_t i = 0; i < atoms.size(); i++) {
        if (atoms[i].nid >= 0) nid_at[atoms[i].nid] = start[i];
        if (atoms[i].kind == ScriptAtom::OP && start[i] % 4) {
            err = strfmt("a command would start at a place not divisible by 4 (%u): commands must stay aligned", (unsigned)start[i]);
            return false;
        }
    }
    sec.assign(pos, 0);
    for (size_t i = 0; i < atoms.size(); i++) {
        const ScriptAtom& a = atoms[i];
        if (!a.b.empty()) memcpy(&sec[start[i]], a.b.data(), a.b.size());
        for (const ScriptSym& s : a.syms) {
            const uint32_t p = start[i] + (uint32_t)s.at;
            uint32_t tgt = 0;
            if (s.tnew >= 0) {
                auto it = nid_at.find(s.tnew);
                if (it == nid_at.end()) { err = strfmt("a reference points at #%d, which is not in the script", s.tnew); return false; }
                tgt = it->second;
            } else {
                tgt = newpos(s.target);
            }
            if (s.type == 2) {
                if (p + 4 > sec.size()) { err = "internal error: a reference is outside its command"; return false; }
                put32(&sec[p], tgt);
                continue;
            }
            const int64_t v = s.type == 0 ? (int64_t)tgt - (int64_t)start[i] : (int64_t)start[i] - (int64_t)tgt;
            if (v < -32768 || v > 32767) {
                err = strfmt("a jump at @%04X is too long after the change (%lld bytes)", a.old >= 0 ? (unsigned)a.old : start[i], (long long)v);
                return false;
            }
            if (p + 2 > sec.size()) { err = "internal error: a jump is outside its command"; return false; }
            put16(&sec[p], (uint16_t)(int16_t)v);
        }
    }
    (void)orig;
    return true;
}

// 06.10.2026: a section that moved to the end of the room block leaves its old place dead. Zeros there pack to almost
// nothing, so a room whose packed block was full still takes a few more records (ST106: a copy of a human did not fit by
// 343 bytes - the old copy of the script was packed a second time). Kept as it was when anything in the block points into
// it: an absolute pointer (4-byte aligned) from outside that place.
void section_old_place_clear(Bytes& blk, uint32_t base, uint32_t off, uint32_t size, std::vector<std::string>& notes, const char* what) {
    section_old_pieces_clear(blk, base, off, {{0u, size}}, notes, what);
}

// pieces [from, to) of the old place (offsets from its start): each is cleared unless an absolute pointer anywhere else in the
// block points into it
void section_old_pieces_clear(Bytes& blk, uint32_t base, uint32_t off, const std::vector<std::pair<uint32_t, uint32_t>>& pieces,
                              std::vector<std::string>& notes, const char* what) {
    std::vector<std::pair<uint32_t, uint32_t>> ok;
    for (auto& p : pieces) if (p.second > p.first && off + p.second <= blk.size()) ok.push_back(p);
    if (ok.empty()) return;
    std::vector<char> refd(ok.size(), 0);
    for (size_t k = 0; k + 4 <= blk.size(); k += 4) {
        const uint32_t v = (uint32_t)blk[k] | ((uint32_t)blk[k + 1] << 8) | ((uint32_t)blk[k + 2] << 16) | ((uint32_t)blk[k + 3] << 24);
        if (v < base + off || v >= base + off + ok.back().second + 4) continue;
        for (size_t i = 0; i < ok.size(); i++) {
            const uint32_t lo = base + off + ok[i].first, hi = base + off + ok[i].second;
            if (v >= lo && v < hi && !(k >= off + ok[i].first && k < off + ok[i].second)) refd[i] = 1;
        }
    }
    uint32_t cleared = 0, kept = 0;
    for (size_t i = 0; i < ok.size(); i++) {
        if (refd[i]) { kept += ok[i].second - ok[i].first; continue; }
        memset(&blk[off + ok[i].first], 0, ok[i].second - ok[i].first);
        cleared += ok[i].second - ok[i].first;
    }
    notes.push_back(strfmt("%s: its old place cleared, %u bytes (%u bytes kept: pointed at)", what, cleared, kept));
}

bool script_install(Bytes& blk, uint32_t base, const RoomInfo& r, const Bytes& sec, uint32_t limit, std::vector<std::string>& notes,
                    std::string& err, const ScriptImage* im) {
    const uint32_t so = r.script_off;
    if (sec.size() <= r.script_size) {
        memcpy(&blk[so], sec.data(), sec.size());
        if (r.script_size > sec.size()) memset(&blk[so + sec.size()], 0, r.script_size - sec.size());
        return true;
    }
    while (blk.size() % 4) blk.push_back(0);
    const uint32_t off = (uint32_t)blk.size();
    if (off + sec.size() > limit) {
        err = strfmt("the script grew to %u bytes and does not fit at the end of the room block (%u bytes free)", (unsigned)sec.size(),
                     limit > off ? limit - off : 0u);
        return false;
    }
    blk.insert(blk.end(), sec.begin(), sec.end());
    put32(&blk[5 * 4], base + off);
    notes.push_back(strfmt("the script grew %u -> %u bytes: moved to the end of the room block", r.script_size, (unsigned)sec.size()));
    if (im) {
        // commands, texts and the directory of the old script are reached through the new pointer only; data pieces stay
        std::vector<std::pair<uint32_t, uint32_t>> pieces;
        for (auto& a : im->atoms)
            if (a.kind != ScriptAtom::DATA && a.old >= 0 && a.old_len) pieces.push_back({(uint32_t)a.old, (uint32_t)a.old + a.old_len});
        std::sort(pieces.begin(), pieces.end());
        section_old_pieces_clear(blk, base, so, pieces, notes, "the script");
    }
    return true;
}

bool script_set_text(ScriptAtom& a, const std::vector<uint16_t>& codes, std::string& err) {
    if (a.kind != ScriptAtom::STRING) { err = "not a text"; return false; }
    if (codes.empty() || codes.back() != 0xA000) { err = "a text must end with the end mark"; return false; }
    Bytes nb(a.b.begin(), a.b.begin() + std::min<size_t>(a.text_from, a.b.size()));
    std::vector<uint16_t> c = codes;
    // сдвиг всего, что за строкой, — кратен 4 (выравнивание команд): добиваем пробелом перед меткой конца
    while (((int64_t)(nb.size() + c.size() * 2) - (int64_t)a.old_len) % 4 != 0) c.insert(c.end() - 1, (uint16_t)0x8000);
    for (uint16_t w : c) { nb.push_back((uint8_t)w); nb.push_back((uint8_t)(w >> 8)); }
    a.b = std::move(nb);
    return true;
}

// ---------------------------------------------------------------- текст скрипта
static std::string mnemonic(const ScriptAtom& a) {
    const uint8_t op = a.b.empty() ? 0 : a.b[0];
    switch (op) {
    case 0x09: return strfmt("FOR %d (exit)", a.b.size() >= 6 ? le16(&a.b[4]) : 0);
    case 0x0A: return "NEXT";
    case 0x0B: return "LABEL";
    case 0x0C: return "JMP";
    case 0x0E: return a.b[1] ? strfmt("BRANCH if %d", a.b[1]) : std::string("BRANCH always");
    case 0x17: return "LOOP (exit)";
    case 0x21: return "TEXT";
    case 0x67: return "TABLE";
    default: break;
    }
    Bytes one = a.b;
    one.resize(std::max<size_t>(one.size(), 48), 0);
    auto l = script_listing(one, 0, 0, (uint32_t)a.b.size());
    if (l.empty()) return "";
    std::string t = l[0].text;
    // голые байты листинг пишет как «XX  hex» — их здесь и так видно
    if (t.size() >= 4 && t[2] == ' ' && t[3] == ' ') return "";
    return t;
}

// 10.10.2026: the directory of an atom list - subroutine k -> the atom its entry points at (-1: that command was removed)
std::vector<int> script_dir_atoms(const ScriptImage& orig, const std::vector<ScriptAtom>& atoms) {
    (void)orig;
    std::vector<int> out;
    const ScriptAtom* dir = nullptr;
    for (const ScriptAtom& a : atoms) if (a.kind == ScriptAtom::DIR) dir = &a;
    if (!dir) return out;
    out.assign(dir->b.size() / 4, -1);
    for (const ScriptSym& s : dir->syms) {
        const int k = s.at / 4;
        if (k < 0 || k >= (int)out.size()) continue;
        for (size_t i = 0; i < atoms.size(); i++) {
            const ScriptAtom& a = atoms[i];
            if (a.kind == ScriptAtom::DIR) continue;
            if (s.tnew >= 0 ? a.nid == s.tnew : a.old == (int64_t)s.target) { out[k] = (int)i; break; }
        }
    }
    return out;
}

int script_next_nid(const std::vector<ScriptAtom>& atoms) {
    int m = 0;
    for (const ScriptAtom& a : atoms) m = std::max(m, a.nid);
    return m + 1;
}

std::string script_text_quote(const std::vector<ScriptAtom>& atoms, const ScriptSym& sy, size_t max_len) {
    std::vector<uint16_t> codes;
    auto take = [&](const ScriptAtom& a, size_t from) {
        for (size_t k = from; k + 2 <= a.b.size(); k += 2) {
            const uint16_t w = le16(&a.b[k]);
            codes.push_back(w);
            if (w == 0xA000) return true;
        }
        return false;
    };
    int i = -1;
    for (int k = 0; k < (int)atoms.size() && i < 0; k++) {
        const ScriptAtom& a = atoms[k];
        if (a.kind == ScriptAtom::DIR) continue;
        if (sy.tnew >= 0 ? a.nid == sy.tnew && a.kind == ScriptAtom::STRING
                         : a.old >= 0 && sy.target >= (uint32_t)a.old && sy.target < (uint32_t)a.old + a.old_len)
            i = k;
    }
    if (i < 0) return "(text)";
    const size_t from = sy.tnew >= 0 ? 0 : sy.target - (uint32_t)atoms[i].old;
    bool ended = take(atoms[i], from);
    // a text of the game that starts in data goes on in the atoms right after it (in the game's order)
    for (int k = i + 1; !ended && k < (int)atoms.size() && codes.size() < 600; k++) {
        const ScriptAtom& a = atoms[k];
        if ((a.kind != ScriptAtom::DATA && a.kind != ScriptAtom::STRING) || a.old != atoms[k - 1].old + (int64_t)atoms[k - 1].old_len) break;
        ended = take(a, 0);
    }
    if (!ended) return "(text)";
    std::string s = decode_text(codes.data(), codes.size()), t;
    for (size_t k = 0; k < s.size(); k++) {
        if (s[k] == '{') {   // control codes and glyphs by number: left out of a quote
            const size_t e = s.find('}', k);
            if (e != std::string::npos) {   // a page break between sentences: keep the words apart
                k = e;
                if (!t.empty() && t.back() != ' ') t += ' ';
                continue;
            }
        }
        const char ch = s[k] == '\n' ? ' ' : s[k];
        if (ch == ' ' && (t.empty() || t.back() == ' ')) continue;
        t += ch;
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.size() > max_len) t = t.substr(0, max_len) + "...";
    return "\"" + t + "\"";
}

// a text on one line of script.txt: \n new line, \" quote, \\ backslash (the rest as texts.json writes it)
static std::string text_escape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '\n') o += "\\n";
        else if (ch == '"') o += "\\\"";
        else if (ch == '\\') o += "\\\\";
        else if (ch == '\r') continue;
        else o += ch;
    }
    return o;
}

static bool text_unescape(const std::string& s, size_t& p, std::string& out) {
    out.clear();
    if (p >= s.size() || s[p] != '"') return false;
    for (p++; p < s.size(); p++) {
        const char ch = s[p];
        if (ch == '"') { p++; return true; }
        if (ch == '\\' && p + 1 < s.size()) {
            const char n = s[++p];
            out += n == 'n' ? '\n' : n;
            continue;
        }
        out += ch;
    }
    return false;
}

static std::string string_atom_text(const ScriptAtom& a) {
    Bytes t(a.b.begin() + std::min<size_t>(a.text_from, a.b.size()), a.b.end());
    std::vector<uint16_t> codes(t.size() / 2);
    for (size_t k = 0; k < codes.size(); k++) codes[k] = le16(&t[k * 2]);
    return decode_text(codes.data(), codes.size());
}

std::string script_atoms_text(const ScriptImage& orig, const std::vector<ScriptAtom>& atoms, const std::string& room) {
    std::ostringstream o;
    o << "; Dino Crisis ME Studio: script of room " << room << "\n"
      << "; One line = one command in hex. @XXXX at the start = where it is in the game: keep it (jumps are fixed by it).\n"
      << "; A new command = a line without @. Delete a line = remove the command. ->@XXXX = jump to that place,\n"
      << "; $@XXXX = reference to a text or table at that place. Texts are edited in Texts. ';' starts a comment.\n\n";
    const std::vector<int> dir = script_dir_atoms(orig, atoms);
    std::map<int, std::vector<int>> subs_at;   // atom -> subroutines starting there
    for (size_t k = 0; k < dir.size(); k++) if (dir[k] >= 0) subs_at[dir[k]].push_back((int)k);
    auto ref = [](const ScriptSym& s) {
        const char* pre = s.type == 2 ? "$" : "->";
        return s.tnew >= 0 ? strfmt(" %s#%d", pre, s.tnew) : strfmt(" %s@%04X", pre, s.target);
    };
    for (size_t i = 0; i < atoms.size(); i++) {
        const ScriptAtom& a = atoms[i];
        if (a.kind == ScriptAtom::DIR) {
            o << "; directory of " << dir.size() << " subroutines (kept automatically)\n";
            continue;
        }
        auto s = subs_at.find((int)i);
        if (s != subs_at.end())
            for (int k : s->second) o << "\nsub " << strfmt("%02d", k) << "\n";
        if (a.kind == ScriptAtom::STRING) {
            std::string txt = string_atom_text(a);
            if (a.old < 0) {   // a new text: the whole of it on its line
                o << strfmt("text #%d \"", a.nid) << text_escape(txt) << "\"\n";
                continue;
            }
            {   // 14.10.2026: the game's text changed in the scenario: its words on its line too
                const int oi = orig.find((uint32_t)a.old);
                if (oi >= 0 && orig.atoms[oi].kind == ScriptAtom::STRING && orig.atoms[oi].b != a.b) {
                    o << strfmt("text @%04X \"", (unsigned)a.old) << text_escape(txt) << "\"\n";
                    continue;
                }
            }
            for (auto& ch : txt) if (ch == '\n' || ch == '\r' || ch == ';') ch = ' ';   // одна строка, без комментария внутри
            if (txt.size() > 70) txt = txt.substr(0, 70) + "...";
            o << strfmt("text @%04X", (unsigned)a.old) << "   ; \"" << txt << "\"\n";
            continue;
        }
        std::string line = a.kind == ScriptAtom::DATA ? strfmt("data @%04X ", (unsigned)a.old)
                           : a.old >= 0              ? strfmt("@%04X ", (unsigned)a.old)
                           : a.nid >= 0              ? strfmt("#%d ", a.nid)
                                                     : std::string();
        for (size_t k = 0; k < a.b.size();) {
            const ScriptSym* sym = nullptr;
            for (const ScriptSym& sy : a.syms) if (sy.at == (int)k) sym = &sy;
            if (sym) {
                line += ref(*sym);
                k += sym->type == 2 ? 4 : 2;
                continue;
            }
            line += strfmt(" %02X", a.b[k]);
            k++;
        }
        if (!line.empty() && line[0] == ' ') line.erase(0, 1);
        if (a.kind == ScriptAtom::OP) {
            const std::string m = mnemonic(a);
            if (!m.empty()) line += "   ; " + m;
        }
        o << line << "\n";
        if (a.kind == ScriptAtom::OP && !a.from.empty()) o << "from " << a.from << "\n";
    }
    return o.str();
}

std::string script_to_text(const ScriptImage& im, const std::string& room) { return script_atoms_text(im, im.atoms, room); }

static bool parse_hex_line(const std::string& s, size_t from, Bytes& b, std::vector<ScriptSym>& syms, std::string& err) {
    std::istringstream in(s.substr(from));
    std::string tok;
    while (in >> tok) {
        if (tok.rfind("->@", 0) == 0 || tok.rfind("$@", 0) == 0 || tok.rfind("->#", 0) == 0 || tok.rfind("$#", 0) == 0) {
            const bool off = tok[0] == '$';
            const bool lab = tok[off ? 1 : 2] == '#';
            const std::string h = tok.substr(off ? 2 : 3);
            char* e = nullptr;
            const unsigned long t = strtoul(h.c_str(), &e, lab ? 10 : 16);
            if (h.empty() || *e) { err = "bad reference " + tok; return false; }
            ScriptSym sym;
            sym.at = (int)b.size();
            sym.type = off ? 2 : (!b.empty() && b[0] == 0x0A ? 1 : 0);
            if (lab) sym.tnew = (int)t;
            else sym.target = (uint32_t)t;
            syms.push_back(sym);
            b.insert(b.end(), off ? 4 : 2, 0);
            continue;
        }
        if (tok.size() != 2 || !isxdigit((unsigned char)tok[0]) || !isxdigit((unsigned char)tok[1])) { err = "bad byte " + tok; return false; }
        b.push_back((uint8_t)strtoul(tok.c_str(), nullptr, 16));
    }
    return true;
}

bool script_from_text(const ScriptImage& orig, const std::string& text, std::vector<ScriptAtom>& out, std::string& err) {
    out.clear();
    for (const ScriptAtom& a : orig.atoms)
        if (a.kind == ScriptAtom::DIR) out.push_back(a);
    if (out.empty()) { err = "the script has no directory"; return false; }
    const size_t ndir0 = out[0].b.size() / 4;
    std::istringstream in(text);
    std::string line;
    int ln = 0;
    std::vector<char> used(orig.atoms.size(), 0);
    std::set<int> labels;
    int pending_sub = -1;              // a new subroutine whose entry is the next command line
    std::vector<std::pair<int, size_t>> new_subs;   // (subroutine, atom index in out)
    int auto_nid = 1000000;            // entries of new subroutines on unlabeled new commands
    while (std::getline(in, line)) {
        ln++;
        auto fail = [&](const std::string& m) { err = strfmt("line %d: ", ln) + m; return false; };
        // a new text keeps ';' and quotes inside its "..."
        {
            size_t p0 = line.find_first_not_of(" \t");
            if (p0 != std::string::npos && line.compare(p0, 6, "text @") == 0) {
                char* e = nullptr;
                const uint32_t lab = (uint32_t)strtoul(line.c_str() + p0 + 6, &e, 16);
                size_t p = (size_t)(e - line.c_str());
                while (p < line.size() && line[p] == ' ') p++;
                if (e != line.c_str() + p0 + 6 && p < line.size() && line[p] == '"') {
                    std::string txt;
                    if (!text_unescape(line, p, txt)) return fail("a text must be in quotes");
                    const int i = orig.find(lab);
                    if (i < 0 || orig.atoms[i].kind != ScriptAtom::STRING) return fail(strfmt("no text at @%04X", lab));
                    if (used[i]) return fail(strfmt("@%04X twice", lab));
                    used[i] = 1;
                    std::vector<uint16_t> codes;
                    std::string e2;
                    if (!encode_text(txt, codes, e2)) return fail(e2);
                    ScriptAtom a = orig.atoms[i];
                    if (!script_set_text(a, codes, e2)) return fail(e2);
                    out.push_back(std::move(a));
                    continue;
                }
            }
            if (p0 != std::string::npos && line.compare(p0, 6, "text #") == 0) {
                char* e = nullptr;
                const long n = strtol(line.c_str() + p0 + 6, &e, 10);
                size_t p = (size_t)(e - line.c_str());
                while (p < line.size() && line[p] == ' ') p++;
                std::string txt;
                if (e == line.c_str() + p0 + 6 || n < 0) return fail("text without #number");
                if (!text_unescape(line, p, txt)) return fail("a new text must be in quotes");
                if (labels.count((int)n)) return fail(strfmt("#%d twice", (int)n));
                labels.insert((int)n);
                std::vector<uint16_t> codes;
                std::string e2;
                if (!encode_text(txt, codes, e2)) return fail(e2);
                ScriptAtom a;
                a.kind = ScriptAtom::STRING;
                a.nid = (int)n;
                if (!script_set_text(a, codes, e2)) return fail(e2);
                out.push_back(std::move(a));
                continue;
            }
        }
        const size_t sc = line.find(';');
        if (sc != std::string::npos) line.resize(sc);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        line = line.substr(p);
        if (line.rfind("sub ", 0) == 0) {
            const int k = atoi(line.c_str() + 4);
            if (k >= (int)ndir0) {
                if (k != (int)(ndir0 + new_subs.size())) return fail(strfmt("new subroutines are numbered in order: expected sub %02d", (int)(ndir0 + new_subs.size())));
                if (pending_sub >= 0) return fail(strfmt("sub %02d has no command", pending_sub));
                pending_sub = k;
            }
            continue;
        }
        auto take_label = [&](size_t at, uint32_t& lab, size_t& next) -> bool {
            if (line.size() <= at || line[at] != '@') return false;
            char* e = nullptr;
            lab = (uint32_t)strtoul(line.c_str() + at + 1, &e, 16);
            next = (size_t)(e - line.c_str());
            return e != line.c_str() + at + 1;
        };
        if (line.rfind("text ", 0) == 0) {
            uint32_t lab = 0;
            size_t nx = 0;
            if (!take_label(5, lab, nx)) return fail("text without @place");
            const int i = orig.find(lab);
            if (i < 0 || orig.atoms[i].kind != ScriptAtom::STRING) return fail(strfmt("no text at @%04X", lab));
            if (used[i]) return fail(strfmt("@%04X twice", lab));
            used[i] = 1;
            out.push_back(orig.atoms[i]);
            continue;
        }
        if (line.rfind("data ", 0) == 0) {
            uint32_t lab = 0;
            size_t nx = 0;
            if (!take_label(5, lab, nx)) return fail("data without @place");
            const int i = orig.find(lab);
            if (i < 0 || orig.atoms[i].kind != ScriptAtom::DATA) return fail(strfmt("no data at @%04X", lab));
            if (used[i]) return fail(strfmt("@%04X twice", lab));
            used[i] = 1;
            ScriptAtom a = orig.atoms[i];
            a.b.clear();
            a.syms.clear();
            if (!parse_hex_line(line, nx, a.b, a.syms, err)) return fail(err);
            out.push_back(std::move(a));
            continue;
        }
        if (line.rfind("from ", 0) == 0) {   // 14.10.2026: whose fighter's motions the enemy of the line above takes
            if (out.empty() || out.back().kind != ScriptAtom::OP || out.back().b.empty() || out.back().b[0] != 0x20)
                return fail("from: only right after an enemy line (20)");
            char room[16] = {};
            unsigned mo = 0, mt = 0;
            if (sscanf(line.c_str() + 5, "%15s %x %x", room, &mo, &mt) != 3) return fail("from: ROOM MODEL MOTIONS");
            out.back().from = strfmt("%s %08X %08X", room, mo, mt);
            continue;
        }
        ScriptAtom a;
        a.kind = ScriptAtom::OP;
        size_t from = 0;
        uint32_t lab = 0;
        if (line[0] == '@') {
            size_t nx = 0;
            if (!take_label(0, lab, nx)) return fail("bad @place");
            const int i = orig.find(lab);
            if (i < 0 || orig.atoms[i].kind != ScriptAtom::OP) return fail(strfmt("no command at @%04X", lab));
            if (used[i]) return fail(strfmt("@%04X twice", lab));
            used[i] = 1;
            a.old = lab;
            a.old_len = orig.atoms[i].old_len;
            a.sub = orig.atoms[i].sub;
            from = nx;
        } else if (line[0] == '#') {
            char* e = nullptr;
            const long n = strtol(line.c_str() + 1, &e, 10);
            if (e == line.c_str() + 1 || n < 0) return fail("bad #label");
            if (labels.count((int)n)) return fail(strfmt("#%d twice", (int)n));
            labels.insert((int)n);
            a.nid = (int)n;
            from = (size_t)(e - line.c_str());
        }
        if (!parse_hex_line(line, from, a.b, a.syms, err)) return fail(err);
        if (a.b.empty()) return fail("empty command");
        const int need = script_op_size(a.b.data(), a.b.size());
        if (need != (int)a.b.size()) return fail(strfmt("command %02X must be %d bytes, the line has %d", a.b[0], need, (int)a.b.size()));
        if (pending_sub >= 0) {
            if (a.old < 0 && a.nid < 0) a.nid = auto_nid++;
            new_subs.push_back({pending_sub, out.size()});
            pending_sub = -1;
        }
        out.push_back(std::move(a));
    }
    if (pending_sub >= 0) { err = strfmt("sub %02d has no command", pending_sub); return false; }
    // every #label a reference names must exist
    for (const ScriptAtom& a : out)
        for (const ScriptSym& s : a.syms)
            if (s.tnew >= 0 && !labels.count(s.tnew)) { err = strfmt("a reference points at #%d, which is not in the script", s.tnew); return false; }
    // new subroutines: the directory grows (old entries keep their numbers)
    for (auto& ns : new_subs) {
        ScriptAtom& d = out[0];
        const ScriptAtom& e = out[ns.second];
        ScriptSym s;
        s.at = (int)d.b.size();
        s.type = 2;
        if (e.old >= 0) s.target = (uint32_t)e.old;
        else s.tnew = e.nid;
        d.b.insert(d.b.end(), 4, 0);
        d.syms.push_back(s);
    }
    return true;
}

}  // namespace dc
