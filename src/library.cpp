#include "library.h"

#include "placement.h"
#include "synth.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace dc {

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
static const uint64_t kFnv0 = 0xCBF29CE484222325ull;

// 06.10.2026 (player: "two kinds of raptors, they even look different"): the texels the model's polygons take (one per corner)
// and their mean colour, named: grey when the colour is weak, else by the weakest channel (olive counts as green - the
// raptors of stages 1-3 are 74/64/45, the grey ones of stage 5 83/70/71)
void model_look(const Vram& v, const Model& m, uint64_t& look, std::string& color) {
    uint64_t h = kFnv0;
    double r = 0, g = 0, b = 0;
    int n = 0;
    std::vector<uint32_t> px;
    for (const APrim& p : m.prims) {
        if (p.part == 0) continue;                       // the mirrored hand: no record of its own
        for (int k = 0; k < 3; k++) {
            v.decode_rect(p.tpage, clut_x(p.clut), clut_y(p.clut), p.uv[k] & 0xFF, p.uv[k] >> 8, 1, 1, px);
            const uint32_t c = px.empty() ? 0u : px[0];
            h = fnv(h, (const uint8_t*)&c, 4);
            if (!(c >> 24)) continue;
            r += c & 255; g += (c >> 8) & 255; b += (c >> 16) & 255; n++;
        }
    }
    look = h;
    color.clear();
    if (!n) return;
    r /= n; g /= n; b /= n;
    const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    if (mx < 1.0 || (mx - mn) / mx < 0.25) color = mx < 50 ? "dark" : "grey";
    else if (mn == b) color = g >= 0.75 * r ? "green" : "brown";
    else if (mn == r) color = "blue";
    else color = "purple";
}

// ---------------------------------------------------------------- ключи (как dc_meshrep.cpp)
uint64_t actor_key_blk(const Bytes& blk, uint32_t base, uint32_t hdr) {
    if (hdr + 24 > blk.size()) return 0;
    auto off = [&](uint32_t a) { return (a & 0x1FFFFFu) - (base & 0x1FFFFFu); };
    const uint32_t np = off(le32(&blk[hdr + 4])), tp = off(le32(&blk[hdr + 8])), qp = off(le32(&blk[hdr + 12]));
    const uint32_t vp = off(le32(&blk[hdr]));
    const int nt = le16(&blk[hdr + 16]), nq = le16(&blk[hdr + 18]), nb = le16(&blk[hdr + 20]);
    if (nb < 1 || nb > 64 || np <= vp || hdr + 24 + nb * 20u > blk.size()) return 0;
    const uint32_t nv = (np - vp) / 8;
    if (np + nv * 8 > blk.size() || tp + nt * 16u > blk.size() || qp + nq * 20u > blk.size()) return 0;
    uint64_t h = fnv(kFnv0, &blk[hdr + 16], 6);
    for (int b = 0; b < nb; b++) {
        h = fnv(h, &blk[hdr + 24 + b * 20], 8);
        h = fnv(h, &blk[hdr + 24 + b * 20 + 16], 4);
    }
    for (uint32_t v = 0; v < nv; v++) h = fnv(h, &blk[np + v * 8], 6);
    for (int i = 0; i < nt; i++) { h = fnv(h, &blk[tp + i * 16], 10); h = fnv(h, &blk[tp + i * 16 + 12], 2); }
    for (int i = 0; i < nq; i++) {
        h = fnv(h, &blk[qp + i * 20], 10); h = fnv(h, &blk[qp + i * 20 + 12], 2); h = fnv(h, &blk[qp + i * 20 + 16], 4);
    }
    return h;
}

uint64_t group_key_blk(const Bytes& blk, uint32_t base, uint32_t hdr, bool lit) {
    if (hdr + 12 > blk.size()) return 0;
    auto off = [&](uint32_t a) { return (a & 0x1FFFFFu) - (base & 0x1FFFFFu); };
    const uint32_t tri = off(le32(&blk[hdr])), quad = off(le32(&blk[hdr + 4]));
    const int nt = le16(&blk[hdr + 8]), nq = le16(&blk[hdr + 10]);
    const uint32_t ts = lit ? 48u : 40u, qs = lit ? 64u : 52u;
    if ((nt && tri + nt * ts > blk.size()) || (nq && quad + nq * qs > blk.size())) return 0;
    uint64_t h = fnv(kFnv0, &blk[hdr + 8], 4);
    for (int i = 0; i < nt; i++)
        for (int k = 0; k < 3; k++) h = fnv(h, &blk[tri + i * ts + k * 8], 6);
    for (int i = 0; i < nq; i++)
        for (int k = 0; k < 4; k++) h = fnv(h, &blk[quad + i * qs + k * 8], 6);
    return h;
}

std::vector<MeshGroup> scan_lit_groups(const Bytes& blk, uint32_t base) {
    std::vector<MeshGroup> out;
    for (uint32_t pos = 0; pos + 12 <= blk.size(); pos += 4) {
        const uint32_t a = le32(&blk[pos]), b = le32(&blk[pos + 4]);
        const uint16_t t = le16(&blk[pos + 8]), q = le16(&blk[pos + 10]);
        if (a != base + pos + 12 || b != a + t * 48u || t + q == 0 || t > 2000 || q > 2000) continue;
        if (pos + 12 + t * 48u + q * 64u > blk.size()) continue;
        bool sane = true;   // страница текстуры (пустое полуслово первой нормали) — в пределах VRAM
        for (uint32_t i = 0; i < t && sane; i++) sane = le16(&blk[pos + 12 + i * 48 + 30]) < 0x200;
        for (uint32_t i = 0; i < q && sane; i++) sane = le16(&blk[pos + 12 + t * 48 + i * 64 + 38]) < 0x200;
        if (!sane) continue;
        MeshGroup g;
        g.hdr = pos; g.tri_off = pos + 12; g.nt = t; g.quad_off = pos + 12 + t * 48u; g.nq = q;
        out.push_back(g);
        pos = g.quad_off + q * 64u - 4;
    }
    return out;
}

// треугольники группы со светом (вид в приложении и выгрузка): вершины 8 Б (x, y, z, u, v), страница +30/+38, палитра +38/+46
void collect_lit_tris(const Bytes& blk, const MeshGroup& g, std::vector<MTri>& out) {
    auto rv = [&](uint32_t o) { MVert v; v.x = s16(&blk[o]); v.y = s16(&blk[o + 2]); v.z = s16(&blk[o + 4]); v.u = blk[o + 6]; v.v = blk[o + 7]; return v; };
    for (uint32_t i = 0; i < g.nt; i++) {
        const uint32_t o = g.tri_off + i * 48u;
        MTri t;
        for (int k = 0; k < 3; k++) t.v[k] = rv(o + k * 8);
        t.tpage = le16(&blk[o + 30]); t.clut = le16(&blk[o + 38]);
        t.code = 0x34;
        t.rec = o;
        out.push_back(t);
    }
    for (uint32_t i = 0; i < g.nq; i++) {
        const uint32_t o = g.quad_off + i * 64u;
        MVert vs[4];
        for (int k = 0; k < 4; k++) vs[k] = rv(o + k * 8);
        static const int order[2][3] = {{0, 1, 2}, {1, 3, 2}};
        for (int h = 0; h < 2; h++) {
            MTri t;
            for (int k = 0; k < 3; k++) t.v[k] = vs[order[h][k]];
            t.tpage = le16(&blk[o + 38]); t.clut = le16(&blk[o + 46]);
            t.code = 0x3C;
            t.rec = o; t.part = (uint8_t)(1 + h);
            out.push_back(t);
        }
    }
}

static uint64_t set_hash(const Bytes& blk, const MotionSet& ms) {
    uint64_t h = kFnv0;
    for (auto& m : ms.motions) {
        const uint32_t hdr_end = m.data;
        if (m.hdr + 8 <= blk.size() && hdr_end <= blk.size() && m.hdr < hdr_end) {
            const uint32_t nk = le32(&blk[m.hdr + 4]) + 1;
            h = fnv(h, &blk[m.hdr + 4], 4);
            if (m.hdr + 8 + nk * 4 <= blk.size()) h = fnv(h, &blk[m.hdr + 8], nk * 4);
        }
        const uint32_t end = m.data + (uint32_t)m.nframes * (uint32_t)m.fsize;
        if (end <= blk.size()) h = fnv(h, &blk[m.data], end - m.data);
    }
    return h;
}

// одно движение: заголовок (ключи) и кадры
static uint64_t motion_hash(const Bytes& blk, const Motion& m) {
    uint64_t h = kFnv0;
    if (m.hdr + 8 <= blk.size() && m.hdr < m.data) {
        const uint32_t nk = le32(&blk[m.hdr + 4]) + 1;
        h = fnv(h, &blk[m.hdr + 4], 4);
        if (m.hdr + 8 + nk * 4 <= blk.size()) h = fnv(h, &blk[m.hdr + 8], nk * 4);
    }
    const uint32_t end = m.data + (uint32_t)m.nframes * (uint32_t)m.fsize;
    if (end <= blk.size() && m.data < end) h = fnv(h, &blk[m.data], end - m.data);
    return h ^ ((uint64_t)m.fsize << 1);
}

std::string Library::names_text() const {
    std::string t = "# Dino Crisis ME: every mesh of the game by name. A glTF or GLB put at this path into mods replaces it at the next\n"
                    "# start of the game (no conversion needed; an untouched copy from source is skipped). kind key tag path [tab node]\n";
    for (auto& a : actors)
        t += strfmt("actor %016llx %s characters/%s/%s.gltf\n", (unsigned long long)a.key, a.kind == "regina" ? "regina" : "-", a.id.c_str(), a.id.c_str());
    for (auto& m : meshes)
        for (size_t g = 0; g < m.keys.size(); g++)
            t += strfmt("group %016llx %s %s/%s/%s.gltf", (unsigned long long)m.keys[g], m.kind == "in_hand" ? "in_hand" : "-", m.cat.c_str(),
                        m.id.c_str(), m.id.c_str()) + (m.keys.size() > 1 ? strfmt("\tgroup_%02d", (int)g) : std::string()) + "\n";
    for (auto& r : rooms)
        for (size_t g = 0; g < r.keys.size(); g++)
            t += strfmt("group %016llx - rooms/%s/%s.gltf", (unsigned long long)r.keys[g], r.f->stem.c_str(), r.f->stem.c_str()) +
                 (r.keys.size() > 1 ? strfmt("\tgroup_%02d", (int)g) : std::string()) + "\n";
    return t;
}

int Library::motion(const std::string& name) const {
    auto it = motion_by_name_.find(name);
    return it == motion_by_name_.end() ? -1 : it->second;
}
int Library::motion_at(const DiscFile* f, int set, int motion) const {
    auto it = motion_by_place_.find({f, set, motion});
    return it == motion_by_place_.end() ? -1 : it->second;
}

const LibActor* Library::actor(const std::string& id) const {
    for (auto& a : actors) if (a.id == id) return &a;
    return nullptr;
}
const LibMesh* Library::mesh(const std::string& id) const {
    for (auto& m : meshes) if (m.id == id) return &m;
    return nullptr;
}

// люди игры по модели (ключ — как у игры, dc_meshrep.cpp). Опознаны 06.10.2026 по лицам (tools\face_eyes\chars) и комнатам.
struct KnownPerson { uint64_t key; const char* chr; const char* en; const char* ru; const char* var_id; const char* var_en; const char* var_ru; };
static const KnownPerson kPeople[] = {
    {0x1d8a2fe2b339e4dcull, "gail", "Gail", "Гейл", "", "", ""},
    {0x8c933cde1b38da55ull, "gail", "Gail", "Гейл", "unarmed", "unarmed", "без оружия"},
    {0x1394123139f7f024ull, "gail", "Gail", "Гейл", "wounded", "wounded", "раненый"},
    {0x0cf3a5ed1a97749dull, "rick", "Rick", "Рик", "", "", ""},
    {0x16285db3bf49c899ull, "rick", "Rick", "Рик", "unarmed", "unarmed", "без оружия"},
    {0xaf6d773ee51c78f9ull, "kirk", "Dr. Kirk", "Доктор Кирк", "", "", ""},
    {0x0471ee8730bfbff4ull, "tom", "Tom", "Том", "", "", ""},
    {0x1fd49f3131008cf7ull, "scientist", "Scientist", "Учёный", "", "", ""},
    {0x09c3f3c9f3bb2deaull, "scientist_woman", "Scientist (woman)", "Учёная", "", "", ""},
};

// персонажи: Регина (4 костюма), динозавры по виду, люди по таблице, прочие модели — каждая сама по себе
void Library::build_chars() {
    chars.clear();
    std::map<std::string, int> by;
    auto add = [&](const std::string& id, const std::string& kind, const std::string& en, const std::string& ru, int ai) {
        auto it = by.find(id);
        if (it == by.end()) {
            LibChar c;
            c.id = id; c.kind = kind; c.en = en; c.ru = ru;
            by[id] = (int)chars.size();
            chars.push_back(c);
            it = by.find(id);
        }
        LibChar& c = chars[it->second];
        // основная модель — без варианта
        if (actors[ai].var_en.empty()) c.actors.insert(c.actors.begin(), ai);
        else c.actors.push_back(ai);
        actors[ai].chr = it->second;
    };
    static const char* order[] = {"regina", "raptor", "trex", "trex_head", "pteranodon", "compsognathus", "dino", "human", "prop", "model"};
    for (const char* kind : order)
        for (size_t i = 0; i < actors.size(); i++) {
            LibActor& a = actors[i];
            if (a.kind != kind) continue;
            if (a.kind == "regina") { add("regina", "regina", "Regina", "Регина", (int)i); continue; }
            if (a.kind == "human") {
                const KnownPerson* kp = nullptr;
                for (const KnownPerson& x : kPeople) if (x.key == a.key) kp = &x;
                if (kp) add(kp->chr, "human", kp->en, kp->ru, (int)i);
                else add(a.id, "human", "Human (" + a.at.f->stem + ")", "Человек (" + a.at.f->stem + ")", (int)i);
                continue;
            }
            if (a.kind == "prop" || a.kind == "model") { add(a.id, a.kind, "", "", (int)i); continue; }
            // динозавры: один вид — один персонаж, вторая модель вида — вариант по файлу
            const bool first = !by.count(a.kind);
            if (!first) { a.var_en = a.at.f->stem; a.var_ru = a.at.f->stem; }
            add(a.kind, a.kind, "", "", (int)i);
        }
    // порядок людей: Гейл, Рик, Кирк, Том, учёные, прочие — как в таблице
    std::stable_sort(chars.begin(), chars.end(), [&](const LibChar& x, const LibChar& y) {
        auto rank = [&](const LibChar& c) {
            for (int k = 0; k < (int)(sizeof order / sizeof order[0]); k++) if (c.kind == order[k]) return k * 100;
            return 9900;
        };
        auto pr = [&](const LibChar& c) {
            for (int k = 0; k < (int)(sizeof kPeople / sizeof kPeople[0]); k++) if (c.id == kPeople[k].chr) return k;
            return 99;
        };
        const int rx = rank(x) + (x.kind == "human" ? pr(x) : 0), ry = rank(y) + (y.kind == "human" ? pr(y) : 0);
        return rx < ry;
    });
    for (size_t c = 0; c < chars.size(); c++)
        for (int ai : chars[c].actors) actors[ai].chr = (int)c;
}

// у каждого движения — один владелец. 1) таблица движений в команде появления: модель с тем же числом костей — её персонаж;
// движение на 15 костей в таблице динозавра — Регины (парные: захват, укус). 2) иначе по местам: 15 костей в файле без людей
// (CORE, оружие, переходы, комнаты без людей) — Регина; в комнате с людьми — движение сцены этой комнаты (его ставит скрипт и
// Регине, и людям); другое число костей — персонаж с таким скелетом в том же файле. Первое место с владельцем решает.
void Library::own_motions() {
    std::map<uint64_t, int> chr_of_key;
    for (auto& a : actors) chr_of_key[a.key] = a.chr;
    int regina = -1;
    for (size_t c = 0; c < chars.size(); c++) if (chars[c].kind == "regina") regina = (int)c;
    std::vector<int> own(motions.size(), -1);
    std::vector<const DiscFile*> scene(motions.size(), nullptr);
    for (const Spawn& sp : spawns_) {
        auto ck = chr_of_key.find(sp.key);
        if (ck == chr_of_key.end() || ck->second < 0) continue;
        int nb = 0;
        for (auto& a : actors) if (a.key == sp.key) nb = a.nbones;
        for (auto& pm : sp.mot) {
            const int m = motion_at(sp.f, pm.first, pm.second);
            if (m < 0 || own[m] >= 0) continue;
            if (motions[m].nbones == nb) own[m] = ck->second;
            else if (motions[m].nbones == 15 && regina >= 0) own[m] = regina;
        }
    }
    auto human_in = [&](const DiscFile* f) {
        auto it = file_actors_.find(f);
        if (it == file_actors_.end()) return false;
        for (auto& ka : it->second) {
            auto ck = chr_of_key.find(ka.first);
            if (ck != chr_of_key.end() && ck->second >= 0 && chars[ck->second].kind == "human") return true;
        }
        return false;
    };
    for (size_t m = 0; m < motions.size(); m++) {
        if (own[m] >= 0) continue;
        const int nb = motions[m].nbones;
        std::vector<LibMotionPlace> places{motions[m].at};
        places.insert(places.end(), motions[m].also.begin(), motions[m].also.end());
        for (const LibMotionPlace& p : places) {
            if (nb == 15) {
                if (human_in(p.f)) { if (!scene[m] && p.f->cat == "rooms") scene[m] = p.f; continue; }
                if (regina >= 0) { own[m] = regina; break; }
                continue;
            }
            auto it = file_actors_.find(p.f);
            if (it == file_actors_.end()) continue;
            for (auto& ka : it->second)
                if (ka.second == nb) {
                    auto ck = chr_of_key.find(ka.first);
                    if (ck != chr_of_key.end() && ck->second >= 0) { own[m] = ck->second; break; }
                }
            if (own[m] >= 0) break;
        }
    }
    scene_motions.clear();
    for (auto& c : chars) c.motions.clear();
    std::vector<int> ord;
    for (size_t m = 0; m < motions.size(); m++) ord.push_back((int)m);
    std::stable_sort(ord.begin(), ord.end(), [&](int x, int y) {
        if (motions[x].set != motions[y].set) return motions[x].set < motions[y].set;
        return motions[x].at.motion < motions[y].at.motion;
    });
    for (int m : ord) {
        if (own[m] >= 0) chars[own[m]].motions.push_back(m);
        else if (scene[m]) scene_motions[scene[m]].push_back(m);
    }
    for (auto& a : actors) a.motions.clear();
    for (auto& c : chars) if (!c.actors.empty()) actors[c.actors[0]].motions = c.motions;
}

// текстура — у первого по дереву: персонажи (по порядку, модели по порядку), оружие, предметы, комнаты, переходы
void Library::own_textures() {
    tex_owner.clear();
    auto put = [&](const std::vector<LibTex>& v, const std::string& who) {
        for (auto& t : v) if (!t.hash.empty() && !tex_owner.count(t.hash)) tex_owner[t.hash] = who;
    };
    for (auto& c : chars)
        for (int ai : c.actors) put(actors[ai].tex, strfmt("a%d", ai));
    for (const char* cat : {"weapons", "items"})
        for (int mi : meshes_of(cat)) put(meshes[mi].tex, strfmt("m%d", mi));
    std::vector<const LibRoom*> rs;
    for (auto& r : rooms) rs.push_back(&r);
    std::sort(rs.begin(), rs.end(), [](const LibRoom* a, const LibRoom* b) { return a->f->name < b->f->name; });
    for (auto* r : rs) put(r->tex, "r" + r->f->stem);
    for (int mi : meshes_of("doors")) put(meshes[mi].tex, strfmt("m%d", mi));
}

const LibRoom* Library::room(const DiscFile* f) const {
    for (auto& r : rooms) if (r.f == f) return &r;
    return nullptr;
}

// меши раздела по порядку дерева. Оружие: вид (дробовик, пистолет, гранатомёт, дротик), в виде — модели оружия, затем в руке
// (по имени файла: P10, P11… H10…)
std::vector<int> Library::meshes_of(const std::string& cat) const {
    std::vector<int> v;
    for (size_t i = 0; i < meshes.size(); i++) if (meshes[i].cat == cat) v.push_back((int)i);
    auto wkind = [&](const LibMesh& m) {
        if (m.kind == "in_hand") {
            const std::string& st = m.at.f->stem;
            return st.size() > 1 ? (st[1] == '1' ? 0 : st[1] == '2' ? 1 : st[1] == '3' ? 2 : 4) : 4;
        }
        return m.kind == "shotgun" ? 0 : m.kind == "handgun" ? 1 : m.kind == "grenade_gun" ? 2 : m.kind == "dart" ? 3 : 4;
    };
    std::stable_sort(v.begin(), v.end(), [&](int x, int y) {
        const LibMesh &a = meshes[x], &b = meshes[y];
        if (cat == "weapons") {
            if (wkind(a) != wkind(b)) return wkind(a) < wkind(b);
            const bool ha = a.kind == "in_hand", hb = b.kind == "in_hand";
            if (ha != hb) return !ha;
            if (ha) return a.at.f->stem < b.at.f->stem;
            return false;
        }
        if (cat == "doors") return a.at.f->name < b.at.f->name;
        return false;
    });
    return v;
}

void Library::build(Disc& d, std::atomic<int>* done, std::atomic<int>* total) {
    actors.clear(); meshes.clear(); sets.clear(); motions.clear(); samples.clear(); songs.clear(); rooms.clear(); chars.clear();
    scene_motions.clear(); tex_owner.clear(); file_actors_.clear(); spawns_.clear();
    motion_by_name_.clear(); motion_by_place_.clear();
    std::unordered_map<uint64_t, int> motkey;
    std::vector<const DiscFile*> files;
    for (auto& f : d.files)
        if (f.cat == "characters" || f.cat == "rooms" || f.cat == "doors" || f.cat == "sound\\bgm" || (f.cat == "screens" && !f.stream))
            files.push_back(&f);
    // CORE первым, затем костюмы, оружие, комнаты, переходы — порядок имён и «первого файла»
    auto rank = [](const DiscFile* f) {
        if (f->stem == "CORE") return 0;
        if (f->stem.rfind("CORE", 0) == 0) return 1;
        if (f->cat == "characters") return 2;
        if (f->cat == "rooms") return 3;
        if (f->cat == "doors") return 4;
        return 5;
    };
    std::stable_sort(files.begin(), files.end(), [&](const DiscFile* a, const DiscFile* b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return a->name < b->name;
    });
    if (total) *total = (int)files.size();
    species.clear();
    std::unordered_map<uint64_t, int> akey, skey, mkey, smpkey, songkey;
    std::map<std::string, int> kind_n;
    for (const DiscFile* f : files) {
        FileContent c;
        if (done) (*done)++;
        if (!analyze(d, *f, c) || !c.container) continue;
        // звук: сэмплы банков (ключ — байты ADPCM от начала до блока с меткой конца, как dc_mods.c) и мелодии
        const bool bgm = f->cat == "sound\\bgm";
        for (size_t bi = 0; bi < c.banks.size(); bi++) {
            const SoundBank& sb = c.banks[bi];
            if (sb.body_entry < 0) continue;
            const DatEntry& be = c.ents[sb.body_entry];
            if ((size_t)be.offset + be.size > c.raw.size()) continue;
            const uint8_t* body = &c.raw[be.offset];
            for (size_t si = 0; si < sb.vab.samples.size(); si++) {
                const VabSample& s = sb.vab.samples[si];
                if (s.off >= be.size) continue;
                uint32_t nbytes = 0;
                for (uint32_t b = s.off; b + 16 <= be.size; b += 16) { nbytes += 16; if (body[b + 1] & 1) break; }
                const uint64_t h = fnv(kFnv0, body + s.off, nbytes) ^ ((uint64_t)nbytes << 1);
                LibPlace pl{f, -1, 0, 0, (int)bi};
                auto it = smpkey.find(h);
                if (it != smpkey.end()) { samples[it->second].also.push_back(pl); continue; }
                LibSample ls;
                ls.at = pl; ls.bank = (int)bi; ls.index = (int)si; ls.nbanks = (int)c.banks.size(); ls.hash = h; ls.bgm = bgm;
                ls.secs = (double)s.pcm.size() / std::max(1, sample_rate_for(s, !bgm));
                smpkey[h] = (int)samples.size();
                samples.push_back(ls);
            }
        }
        for (size_t qi = 0; qi < c.seqs.size(); qi++) {
            const uint64_t k = song_key(c.seqs[qi].data.data(), c.seqs[qi].data.size());
            LibPlace pl{f, -1, 0, 0, (int)qi};
            auto it = songkey.find(k);
            if (it != songkey.end()) { songs[it->second].also.push_back(pl); continue; }
            LibSong sg;
            sg.at = pl; sg.index = (int)qi; sg.key = k;
            songkey[k] = (int)songs.size();
            songs.push_back(sg);
        }
        if (bgm || f->cat == "screens") continue;
        // тело комнаты: ключи групп
        if (c.is_room)
            for (auto& m : c.meshes)
                if (m.kind == "room" && m.block >= 0) {
                    LibRoom r;
                    r.f = f;
                    for (uint32_t h : m.hdrs) r.keys.push_back(group_key_blk(c.blocks[m.block].data, c.blocks[m.block].dest, h, false));
                    for (auto& cr : crops_for_mesh(c.vram, m.tris)) r.tex.push_back({cr.hash, f, 10000 + (int)(&m - &c.meshes[0])});
                    rooms.push_back(std::move(r));
                    break;
                }
        // наборы движений
        std::vector<int> file_sets;
        for (size_t si = 0; si < c.motion_sets.size(); si++) {
            const MotionSet& ms = c.motion_sets[si];
            if (ms.block < 0 || ms.ext) continue;
            const uint64_t h = set_hash(c.blocks[ms.block].data, ms);
            LibPlace pl{f, ms.block, c.blocks[ms.block].dest, ms.motions.empty() ? 0u : ms.motions[0].hdr, (int)si};
            // движения набора без повторов (в том числе у повтора набора — то же самое, только места)
            const int set_idx = skey.count(h) ? skey[h] : (int)sets.size();
            for (size_t mi = 0; mi < ms.motions.size(); mi++) {
                const uint64_t mh = motion_hash(c.blocks[ms.block].data, ms.motions[mi]);
                const LibMotionPlace mp{f, (int)si, (int)mi};
                auto mt = motkey.find(mh);
                if (mt != motkey.end()) { motions[mt->second].also.push_back(mp); motion_by_place_[{f, (int)si, (int)mi}] = mt->second; continue; }
                LibMotion lm;
                lm.set = set_idx; lm.nbones = ms.nbones; lm.hash = mh; lm.at = mp;
                lm.name = strfmt("#%d", (int)mi);   // имя — после имён наборов
                motkey[mh] = (int)motions.size();
                motion_by_place_[{f, (int)si, (int)mi}] = (int)motions.size();
                motions.push_back(std::move(lm));
            }
            auto it = skey.find(h);
            if (it != skey.end()) { sets[it->second].also.push_back(pl); file_sets.push_back(it->second); continue; }
            LibSet s;
            s.id = f->stem;
            s.at = pl;
            s.nbones = ms.nbones;
            s.count = (int)ms.motions.size();
            s.hash = h;
            skey[h] = (int)sets.size();
            file_sets.push_back((int)sets.size());
            sets.push_back(std::move(s));
        }
        // модели персонажей (свои; Регина «из CORE» для наборов оружия — не модель файла)
        bool own15 = false;
        for (size_t ai = 0; ai < c.actors.size(); ai++) {
            const ActorPart& a = c.actors[ai];
            if (a.block < 0) continue;
            const RamBlock& b = c.blocks[a.block];
            const uint64_t k = actor_key_blk(b.data, b.dest, a.model.hdr);
            if (!k) continue;
            if (a.model.bones.size() == 15 && f->cat != "characters") own15 = true;
            LibPlace pl{f, a.block, b.dest, a.model.hdr, (int)ai};
            file_actors_[f].push_back({k, (int)a.model.bones.size()});
            int idx;
            auto it = akey.find(k);
            if (it != akey.end()) { actors[it->second].also.push_back(pl); idx = it->second; }
            else {
                LibActor la;
                la.key = k;
                la.at = pl;
                la.kind = a.kind;
                la.nbones = (int)a.model.bones.size();
                la.nverts = a.model.nv_disk;
                la.nprims = (int)a.model.prims.size();
                idx = (int)actors.size();
                akey[k] = idx;
                actors.push_back(std::move(la));
            }
            // текстуры этого места: та же модель в другой комнате бывает в другой раскраске — своя картинка
            for (auto& cr : crops_for_model(c.vram, a.model)) {
                auto& tv = actors[idx].tex;
                if (std::none_of(tv.begin(), tv.end(), [&](const LibTex& t) { return t.hash == cr.hash; })) tv.push_back({cr.hash, f, (int)ai});
            }
            // движения файла с тем же числом костей — этой модели
            for (int si : file_sets)
                if (sets[si].nbones == (int)a.model.bones.size() &&
                    std::find(actors[idx].sets.begin(), actors[idx].sets.end(), si) == actors[idx].sets.end())
                    actors[idx].sets.push_back(si);
        }
        // наборы на 15 костей без своей модели в файле (оружие P/H, Регина в комнатах и переходах) — Регине
        if (!own15)
            for (int si : file_sets)
                if (sets[si].nbones == 15)
                    for (auto& la : actors)
                        if (la.kind == "regina" && std::find(la.sets.begin(), la.sets.end(), si) == la.sets.end()) la.sets.push_back(si);
        // команды появления комнаты (20/59/5B): модель и таблица её движений — кто какие движения играет
        if (c.is_room) {
            std::map<uint32_t, std::pair<int, int>> hdr2m;
            for (size_t si = 0; si < c.motion_sets.size(); si++) {
                const MotionSet& ms = c.motion_sets[si];
                if (ms.block < 0 || ms.ext) continue;
                for (size_t mi = 0; mi < ms.motions.size(); mi++) hdr2m[c.blocks[ms.block].dest + ms.motions[mi].hdr] = {(int)si, (int)mi};
            }
            std::map<uint32_t, uint64_t> mdl2key;
            for (const ActorPart& a : c.actors)
                if (a.block >= 0) mdl2key[c.blocks[a.block].dest + a.model.hdr] = actor_key_blk(c.blocks[a.block].data, c.blocks[a.block].dest, a.model.hdr);
            auto rd32 = [&](uint32_t addr, uint32_t& v) {
                for (auto& b : c.blocks)
                    if (addr >= b.dest && addr + 4 <= b.dest + b.data.size()) { v = le32(&b.data[addr - b.dest]); return true; }
                return false;
            };
            const json en = room_enemies_json(c);
            if (en.is_object() && en.count("enemies"))
                for (auto& e : en["enemies"]) {
                    const uint32_t model = (uint32_t)std::stoul(e.value("model", std::string("0")), nullptr, 16);
                    const uint32_t tab = (uint32_t)std::stoul(e.value("motions", std::string("0")), nullptr, 16);
                    auto mk = mdl2key.find(model);
                    if (mk == mdl2key.end() || !tab) continue;
                    Spawn sp;
                    sp.f = f;
                    sp.key = mk->second;
                    uint32_t first = 0;
                    for (int i = 0; i < 300; i++) {
                        uint32_t v = 0;
                        if (!rd32(tab + i * 4, v) || v < 0x80000000u || v >= 0x80200000u) break;
                        if (i == 0) first = v;
                        if (first && tab + i * 4 >= first) break;   // таблица кончилась там, где начались сами движения
                        auto hm = hdr2m.find(v);
                        if (hm != hdr2m.end()) sp.mot.push_back(hm->second);
                    }
                    {   // 06.10.2026: the kind for the enemy drop-down of every room
                        LibSpecies s;
                        s.f = f; s.kind = e.value("kind", 0); s.variant = e.value("variant", 0);
                        s.ai = e.value("ai", -1); s.hp = e.value("hp", -1); s.nmot = (int)sp.mot.size();
                        s.model = model; s.motions = tab; s.key = mk->second; s.kind_name = e.value("kind_name", std::string());
                        for (const ActorPart& a : c.actors)                      // its look in this room
                            if (a.block >= 0 && c.blocks[a.block].dest + a.model.hdr == model) { model_look(c.vram, a.model, s.look, s.color); break; }
                        bool have = false;
                        for (auto& o : species)
                            if (o.key == s.key && o.kind == s.kind && o.variant == s.variant && o.look == s.look) {
                                have = true;
                                if (s.nmot > o.nmot) o = s;
                                break;
                            }
                        if (!have && s.nmot > 0) species.push_back(s);
                    }
                    if (!sp.mot.empty()) spawns_.push_back(std::move(sp));
                }
        }
        // неподвижные меши (без тел комнат: тело комнаты — своё у каждой, её куски с тем же содержимым игра и так меняет везде)
        for (size_t mi = 0; mi < c.meshes.size(); mi++) {
            const MeshPart& mp = c.meshes[mi];
            if (mp.block < 0 || mp.hdrs.empty() || mp.kind == "room") continue;
            const RamBlock& b = c.blocks[mp.block];
            std::string cat = mp.kind == "door" ? "doors" : (mp.kind == "item_box" || mp.kind == "prop") ? "items" : "weapons";
            std::vector<uint64_t> keys;
            int nt = 0;
            for (uint32_t h : mp.hdrs) keys.push_back(group_key_blk(b.data, b.dest, h, mp.lit));
            nt = (int)mp.tris.size();
            uint64_t mk = kFnv0;
            for (uint64_t k : keys) mk = fnv(mk, (const uint8_t*)&k, 8);
            LibPlace pl{f, mp.block, b.dest, mp.hdrs[0], (int)mi};
            auto it = mkey.find(mk);
            if (it != mkey.end()) {
                meshes[it->second].also.push_back(pl);
                for (auto& cr : crops_for_mesh(c.vram, mp.tris)) {
                    auto& tv = meshes[it->second].tex;
                    if (std::none_of(tv.begin(), tv.end(), [&](const LibTex& t) { return t.hash == cr.hash; })) tv.push_back({cr.hash, f, 10000 + (int)mi});
                }
                continue;
            }
            LibMesh m;
            m.cat = cat; m.kind = mp.kind; m.keys = keys; m.lit = mp.lit; m.at = pl; m.ntris = nt; m.groups = mp.hdrs;
            for (auto& cr : crops_for_mesh(c.vram, mp.tris)) m.tex.push_back({cr.hash, f, 10000 + (int)mi});
            mkey[mk] = (int)meshes.size();
            meshes.push_back(std::move(m));
        }
    }
    // имена (папки) и подписи. Люди — персонажи игры по модели (ключ по содержимому; опознаны по лицам и комнатам, 06.10.2026):
    // у Гейл три модели (с оружием, без оружия, раненый), у Рика две; одна модель учёного (ST202, ST303 — разные раскраски)
    std::map<std::string, int> n;
    for (auto& a : actors) {
        std::string base = a.kind;
        if (a.kind == "regina") {
            const std::string& st = a.at.f->stem;   // CORE / CORE0N
            base = (st == "CORE" || st == "CORE00") ? "regina" : "regina_costume" + st.substr(5);
            if (base != "regina") { a.var_en = "costume " + st.substr(5); a.var_ru = "костюм " + st.substr(5); }
        } else if (a.kind == "human") {
            base = "human_" + a.at.f->stem;
            for (const KnownPerson& kp : kPeople)
                if (kp.key == a.key) {
                    base = kp.var_en[0] ? std::string(kp.chr) + "_" + kp.var_id : std::string(kp.chr);
                    a.var_en = kp.var_en; a.var_ru = kp.var_ru;
                }
        }
        int k = ++n[base];
        a.id = k == 1 ? base : strfmt("%s_%d", base.c_str(), k);
    }
    std::map<std::string, int> nm;
    for (auto& m : meshes) {
        std::string base = m.kind;
        if (m.kind == "in_hand") base = "in_hand_" + m.at.f->stem;
        else if (m.kind == "door") base = "door_" + m.at.f->stem;
        int k = ++nm[base];
        m.id = k == 1 ? base : strfmt("%s_%d", base.c_str(), k);
    }
    // в одном файле бывает несколько наборов (Регина и раптор в ST100): второй и далее — <файл>_2, _3 (анимации <набор>_<NN>)
    std::map<std::string, int> ns;
    for (auto& s : sets) {
        int k = ++ns[s.id];
        if (k > 1) s.id = strfmt("%s_%d", s.id.c_str(), k);
        s.label = s.id;
    }
    for (size_t i = 0; i < motions.size(); i++) {
        LibMotion& m = motions[i];
        m.name = strfmt("%s_%02d", sets[m.set].id.c_str(), m.at.motion);
        motion_by_name_[m.name] = (int)i;
    }
    build_chars();
    own_motions();
    own_textures();
    for (auto& s : species)
        for (auto& a : actors)
            if (a.key == s.key) { s.actor = a.id; break; }
    // двойники комнат: тело то же (все ключи групп), что у комнаты раньше по списку — тело показывается там
    std::map<std::vector<uint64_t>, const DiscFile*> body;
    std::vector<int> ord;
    for (size_t i = 0; i < rooms.size(); i++) ord.push_back((int)i);
    std::sort(ord.begin(), ord.end(), [&](int x, int y) { return rooms[x].f->name < rooms[y].f->name; });
    for (int i : ord) {
        if (rooms[i].keys.empty()) continue;
        auto it = body.find(rooms[i].keys);
        if (it != body.end()) rooms[i].twin = it->second;
        else body[rooms[i].keys] = rooms[i].f;
    }
    file_actors_.clear();
    spawns_.clear();
    ready = true;
}

}  // namespace dc
