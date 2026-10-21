#include "export.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>

#include "gltf.h"
#include "motions.h"
#include "mp4.h"
#include "placement.h"
#include "synth.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include "../deps/stb/stb_image_write.h"

namespace dc {

static json export_fps(const fs::path& dir, const std::string& name, ModelFormat f);
static void write_place_info(const fs::path& p, const std::string& what, const std::vector<uint64_t>& keys, const LibPlace& at,
                             const std::vector<LibPlace>& also, const json& fps);

bool write_png(const fs::path& p, int w, int h, const uint32_t* rgba) {
    Bytes out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            Bytes* o = (Bytes*)ctx;
            o->insert(o->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &out, w, h, 4, rgba, w * 4);
    return !out.empty() && write_file(p, out.data(), out.size());
}

void create_source_tree(const fs::path& root) {
    std::error_code ec;
    for (int i = 0; kModCategories[i]; i++) fs::create_directories(root / u8path(kModCategories[i]), ec);
}

std::string se_group(const DiscFile& f) {
    if (f.cat == "rooms") return "room";
    if (f.cat == "doors") return "door";
    if (f.cat == "characters") return (f.stem[0] == 'P' || f.stem[0] == 'H') ? "weapon" : "common";
    return "common";
}

Exporter::Exporter(Disc& d, const fs::path& s, const ExportOptions& o, ExportLog* log, bool make_tree)
    : d_(d), src_(s), o_(o), log_(log) {
    if (make_tree) create_source_tree(src_);
}

Exporter::~Exporter() {}

bool Exporter::write_model(const FileContent& c, int part, const fs::path& dir, const std::string& name) {
    if (part < 0 || part >= (int)c.actors.size()) return false;
    ActorPart a = c.actors[part];
    a.motion_sets.clear();                       // the game moves it: only the model and its pictures
    std::error_code ec;
    fs::create_directories(dir, ec);
    anim_names_ = nullptr;
    const ModelFormat keep = o_.model_format;
    o_.model_format = ModelFormat::Gltf;        // 08.10.2026: the game reads these as glTF (kinds\index.txt)
    write_actor(dir, name, c, a, c.file ? c.file->stem : std::string());
    o_.model_format = keep;
    return fs::exists(dir / u8path(name + ".gltf"), ec);
}

fs::path sample_wav_path(const fs::path& root, const LibSample& s) {
    const DiscFile& f = *s.at.f;
    const std::string nm = s.nbanks > 1 ? strfmt("bank%d_%03d.wav", s.bank, s.index) : strfmt("%03d.wav", s.index);
    if (f.cat == "sound\\bgm") return root / "sound" / "bgm" / u8path(f.stem) / "instruments" / u8path(nm);
    return root / "sound" / "se" / u8path(se_group(f)) / u8path(f.stem) / u8path(nm);
}

void Exporter::note(const std::string& s) {
    if (log_) log_->add(s);
}

// картинка — рядом с моделью, под именем вырезки (страница, палитра, окно UV): поправленная PNG с тем же именем — подмена
// текстуры во всей игре (mods\textures\index.txt), без подмены меша
fs::path Exporter::texture_file(const fs::path& dir, const TexCrop& c) {
    fs::path p = dir / u8path(c.file_name());
    if (!fs::exists(p)) write_png(p, c.w(), c.h(), c.rgba.data());
    return p;
}

// ---------------------------------------------------------------- статический меш → glTF + OBJ
static inline void cv_pos(float x, float y, float z, float o[3]) { o[0] = x * 0.001f; o[1] = -y * 0.001f; o[2] = -z * 0.001f; }

void Exporter::write_mesh(const fs::path& dir, const std::string& name, const MeshPart& m, const Vram& v,
                          const std::string& owner) {
    auto crops = crops_for_mesh(v, m.tris);
    std::map<std::pair<uint16_t, uint16_t>, const TexCrop*> by;
    std::map<std::pair<uint16_t, uint16_t>, fs::path> png;
    (void)owner;
    for (auto& c : crops) {
        by[{c.tpage, c.clut}] = &c;
        if (o_.textures) png[{c.tpage, c.clut}] = texture_file(dir, c);
    }
    // ---- glTF: узел на группу (у меша из нескольких групп — group_NN: её игра и подменяет), примитив на материал
    {
        Gltf g;
        struct Prim { std::vector<float> pos, uv; std::vector<uint8_t> col; std::vector<uint32_t> idx; std::unordered_map<std::string, uint32_t> map; };
        std::map<std::tuple<int, uint16_t, uint16_t, bool>, Prim> prims;
        const bool by_group = m.ngroups > 1;
        for (auto& t : m.tris) {
            bool semi = (t.code & 2) != 0;
            Prim& p = prims[{by_group ? (int)t.group : 0, t.tpage, t.clut, semi}];
            const TexCrop* c = by[{t.tpage, t.clut}];
            uint32_t tri[3];
            for (int k = 0; k < 3; k++) {
                const MVert& vv = t.v[k];
                uint32_t col = t.rgb[k];
                std::string key((const char*)&vv, sizeof vv);
                key.append((const char*)&col, 4);
                auto it = p.map.find(key);
                if (it != p.map.end()) { tri[k] = it->second; continue; }
                uint32_t id = (uint32_t)(p.pos.size() / 3);
                float q[3];
                cv_pos(vv.x, vv.y, vv.z, q);
                p.pos.insert(p.pos.end(), q, q + 3);
                p.uv.push_back((vv.u - c->u0 + 0.5f) / c->w());
                p.uv.push_back((vv.v - c->v0 + 0.5f) / c->h());
                for (int s = 0; s < 3; s++) p.col.push_back((uint8_t)std::min(255u, ((col >> (8 * s)) & 0xFF) * 2u));
                p.map[key] = id;
                tri[k] = id;
            }
            // лицевая сторона игры после (x,-y,-z) — по часовой; в glTF лицевая — против: 0,2,1
            p.idx.push_back(tri[0]);
            p.idx.push_back(tri[2]);
            p.idx.push_back(tri[1]);
        }
        std::map<std::string, int> mats;
        std::map<int, json> per_group;
        for (auto& kv : prims) {
            const int gi = std::get<0>(kv.first);
            uint16_t tp = std::get<1>(kv.first), cl = std::get<2>(kv.first);
            bool semi = std::get<3>(kv.first);
            Prim& p = kv.second;
            const std::string mk = material_key(tp, cl) + (semi ? "_semi" : "");
            auto mi = mats.find(mk);
            if (mi == mats.end()) {
                int tex = -1;
                if (o_.textures) tex = g.texture(g.image(rel_uri(dir, png[{tp, cl}])));
                mi = mats.emplace(mk, g.material(mk, tex, semi)).first;
            }
            size_t nv = p.pos.size() / 3;
            json attrs = {{"POSITION", g.acc_f(p.pos.data(), nv, 3, true, 34962)},
                          {"TEXCOORD_0", g.acc_f(p.uv.data(), nv, 2, false, 34962)},
                          {"COLOR_0", g.acc_u8n(p.col.data(), nv, 3, 34962)}};
            if (!per_group.count(gi)) per_group[gi] = json::array();
            per_group[gi].push_back({{"attributes", attrs}, {"indices", g.acc_u32(p.idx.data(), p.idx.size(), 34963)}, {"material", mi->second}});
        }
        for (auto& kv : per_group) {
            const std::string nn = by_group ? strfmt("group_%02d", kv.first) : name;
            g.j["meshes"].push_back({{"name", nn}, {"primitives", kv.second}});
            int n = g.node({{"name", nn}, {"mesh", (int)g.j["meshes"].size() - 1}});
            g.j["scenes"][0]["nodes"].push_back(n);
        }
        save_model(g, dir, name);
    }
    // ---- OBJ в родных координатах игры (мм, Y вниз — как exported_rooms и коллизия камеры); 08.10.2026: только рядом с glTF,
    // выбор FBX или OBJ пишет одну модель в своём формате (у OBJ то же имя)
    if (o_.model_format == ModelFormat::Gltf) {
        std::string obj, mtl;
        obj += strfmt("# Dino Crisis - %s, native coordinates (mm, Y down)\nmtllib %s.mtl\n", name.c_str(), name.c_str());
        std::map<std::tuple<int, int, int>, int> vmap;
        std::map<std::pair<int, int>, int> tmap;   // (материал, uv) → индекс vt
        std::string vs, vts, fsx;
        int nv = 0, nt = 0;
        std::vector<std::pair<uint16_t, uint16_t>> order;
        std::map<std::pair<uint16_t, uint16_t>, std::vector<const MTri*>> per;
        for (auto& t : m.tris) {
            auto k = std::make_pair(t.tpage, t.clut);
            if (!per.count(k)) order.push_back(k);
            per[k].push_back(&t);
        }
        std::map<std::tuple<uint16_t, uint16_t, int, int>, int> uvmap;
        for (auto& k : order) {
            const TexCrop* c = by[k];
            std::string mn = material_key(k.first, k.second);
            fsx += "usemtl " + mn + "\n";
            mtl += "newmtl " + mn + "\nKd 1 1 1\nd 1\nillum 1\n";
            if (o_.textures) {
                std::string u = rel_uri(dir, png[k]);
                mtl += "map_Kd " + u + "\nmap_d " + u + "\n";
            }
            mtl += "\n";
            for (const MTri* t : per[k]) {
                int ids[3], tid[3];
                for (int i = 0; i < 3; i++) {
                    auto pk = std::make_tuple((int)t->v[i].x, (int)t->v[i].y, (int)t->v[i].z);
                    auto it = vmap.find(pk);
                    if (it == vmap.end()) {
                        vs += strfmt("v %d %d %d\n", t->v[i].x, t->v[i].y, t->v[i].z);
                        it = vmap.emplace(pk, ++nv).first;
                    }
                    ids[i] = it->second;
                    auto uk = std::make_tuple(k.first, k.second, (int)t->v[i].u, (int)t->v[i].v);
                    auto ut = uvmap.find(uk);
                    if (ut == uvmap.end()) {
                        vts += strfmt("vt %.6f %.6f\n", (t->v[i].u - c->u0 + 0.5f) / c->w(), 1.0f - (t->v[i].v - c->v0 + 0.5f) / c->h());
                        ut = uvmap.emplace(uk, ++nt).first;
                    }
                    tid[i] = ut->second;
                }
                fsx += strfmt("f %d/%d %d/%d %d/%d\n", ids[0], tid[0], ids[1], tid[1], ids[2], tid[2]);
            }
        }
        obj += vs + vts + "g " + name + "\n" + fsx;
        write_text(dir / u8path(name + ".obj"), obj);
        write_text(dir / u8path(name + ".mtl"), mtl);
    }
}

// ---------------------------------------------------------------- скелетная модель → glTF со всеми движениями
struct Quat { float x, y, z, w; };
static Quat to_quat(const M3& a) {
    const float* m = a.m;
    float t = m[0] + m[4] + m[8], x, y, z, w;
    if (t > 0) {
        float s = sqrtf(t + 1.0f) * 2;
        w = 0.25f * s; x = (m[7] - m[5]) / s; y = (m[2] - m[6]) / s; z = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2;
        w = (m[7] - m[5]) / s; x = 0.25f * s; y = (m[1] + m[3]) / s; z = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2;
        w = (m[2] - m[6]) / s; x = (m[1] + m[3]) / s; y = 0.25f * s; z = (m[5] + m[7]) / s;
    } else {
        float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2;
        w = (m[3] - m[1]) / s; x = (m[2] + m[6]) / s; y = (m[5] + m[7]) / s; z = 0.25f * s;
    }
    float n = sqrtf(x * x + y * y + z * z + w * w);
    if (n <= 0) n = 1;
    return {x / n, y / n, z / n, w / n};
}
// S·M·S, S = diag(1,-1,-1): система игры (Y вниз, Z вперёд) → glTF (Y вверх)
static M3 cv_rot(const M3& m) {
    static const float s[3] = {1, -1, -1};
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i * 3 + j] = m.m[i * 3 + j] * s[i] * s[j];
    return r;
}

void Exporter::write_actor(const fs::path& dir, const std::string& name, const FileContent& c, const ActorPart& a,
                           const std::string& owner) {
    const Model& md = a.model;
    auto crops = crops_for_model(c.vram, md);
    std::map<std::pair<uint16_t, uint16_t>, const TexCrop*> by;
    std::map<std::pair<uint16_t, uint16_t>, fs::path> png;
    (void)owner;
    for (auto& cr : crops) {
        by[{cr.tpage, cr.clut}] = &cr;
        if (o_.textures) png[{cr.tpage, cr.clut}] = texture_file(dir, cr);
    }
    std::vector<BoneXf> rest;
    rest_bones(md, rest);
    const int nb = (int)md.bones.size();
    Gltf g;
    // вершины: разбиение по (вершина, uv, материал)
    struct P { std::vector<float> pos, nrm, uv; std::vector<uint8_t> jnt; std::vector<float> wgt; std::vector<uint32_t> idx; std::map<std::tuple<int, int>, uint32_t> map; };
    std::map<std::pair<uint16_t, uint16_t>, P> prims;
    for (auto& p : md.prims) {
        P& pp = prims[{p.tpage, p.clut}];
        const TexCrop* cr = by[{p.tpage, p.clut}];
        uint32_t tri[3];
        float tp[3][3], tn[3];
        for (int k = 0; k < 3; k++) {
            int vi = p.vi[k];
            if (vi >= (int)md.verts.size()) vi = 0;
            auto key = std::make_tuple(vi, (int)p.uv[k]);
            auto it = pp.map.find(key);
            int bone = md.vbone[vi];
            const auto& v = md.verts[vi];
            float q[3];
            cv_pos(v[0] + rest[bone].t[0], v[1] + rest[bone].t[1], v[2] + rest[bone].t[2], q);
            memcpy(tp[k], q, sizeof q);
            if (it != pp.map.end()) { tri[k] = it->second; continue; }
            uint32_t id = (uint32_t)(pp.pos.size() / 3);
            pp.pos.insert(pp.pos.end(), q, q + 3);
            const auto& n = md.norms[vi];
            float ln = sqrtf((float)n[0] * n[0] + (float)n[1] * n[1] + (float)n[2] * n[2]);
            if (ln <= 0) ln = 1;
            pp.nrm.push_back(n[0] / ln); pp.nrm.push_back(-n[1] / ln); pp.nrm.push_back(-n[2] / ln);
            pp.uv.push_back(((p.uv[k] & 0xFF) - cr->u0 + 0.5f) / cr->w());
            pp.uv.push_back(((p.uv[k] >> 8) - cr->v0 + 0.5f) / cr->h());
            pp.jnt.push_back((uint8_t)(bone + 1)); pp.jnt.push_back(0); pp.jnt.push_back(0); pp.jnt.push_back(0);
            pp.wgt.push_back(1); pp.wgt.push_back(0); pp.wgt.push_back(0); pp.wgt.push_back(0);
            pp.map[key] = id;
            tri[k] = id;
        }
        // обход: геометрическая нормаль согласуется с нормалью вершины (как export_regina.py)
        float e1[3], e2[3], gn[3];
        for (int i = 0; i < 3; i++) { e1[i] = tp[1][i] - tp[0][i]; e2[i] = tp[2][i] - tp[0][i]; }
        gn[0] = e1[1] * e2[2] - e1[2] * e2[1]; gn[1] = e1[2] * e2[0] - e1[0] * e2[2]; gn[2] = e1[0] * e2[1] - e1[1] * e2[0];
        for (int i = 0; i < 3; i++) tn[i] = pp.nrm[tri[0] * 3 + i];
        if (gn[0] * tn[0] + gn[1] * tn[1] + gn[2] * tn[2] < 0) std::swap(tri[1], tri[2]);
        pp.idx.insert(pp.idx.end(), tri, tri + 3);
    }
    json jp = json::array();
    for (auto& kv : prims) {
        P& p = kv.second;
        size_t nv = p.pos.size() / 3;
        int tex = -1;
        if (o_.textures) tex = g.texture(g.image(rel_uri(dir, png[kv.first])));
        int mat = g.material(material_key(kv.first.first, kv.first.second), tex, false);
        json at = {{"POSITION", g.acc_f(p.pos.data(), nv, 3, true, 34962)}, {"NORMAL", g.acc_f(p.nrm.data(), nv, 3, false, 34962)},
                   {"TEXCOORD_0", g.acc_f(p.uv.data(), nv, 2, false, 34962)}, {"JOINTS_0", g.acc_u8(p.jnt.data(), nv, 4, 34962)},
                   {"WEIGHTS_0", g.acc_f(p.wgt.data(), nv, 4, false, 34962)}};
        jp.push_back({{"attributes", at}, {"indices", g.acc_u32(p.idx.data(), p.idx.size(), 34963)}, {"material", mat}});
    }
    g.j["meshes"].push_back({{"name", name}, {"primitives", jp}});
    // узлы: 0 — меш, 1 — root, 2.. — кости
    g.node({{"name", name}, {"mesh", 0}, {"skin", 0}});
    json rootch = json::array();
    for (int i = 0; i < nb; i++) if (md.bones[i].parent < 0) rootch.push_back(i + 2);
    g.node({{"name", "root"}, {"children", rootch}});
    for (int i = 0; i < nb; i++) {
        const Bone& b = md.bones[i];
        float t[3];
        cv_pos(b.ox, b.oy, b.oz, t);
        // кости Регины — имена из REGINA_NOTES (по смещениям и примитивам), у остальных — номера (по ним читает конвертер)
        json n = {{"name", bone_node_name(a, i)}, {"translation", {t[0], t[1], t[2]}}};
        json ch = json::array();
        for (int k = 0; k < nb; k++) if (md.bones[k].parent == i) ch.push_back(k + 2);
        if (!ch.empty()) n["children"] = ch;
        g.node(n);
    }
    g.j["scenes"][0]["nodes"] = {0, 1};
    std::vector<float> ibm(16 * (nb + 1), 0);
    for (int i = 0; i <= nb; i++) {
        float* m = &ibm[i * 16];
        m[0] = m[5] = m[10] = m[15] = 1;
        if (i > 0) {
            float q[3];
            cv_pos(rest[i - 1].t[0], rest[i - 1].t[1], rest[i - 1].t[2], q);
            m[12] = -q[0]; m[13] = -q[1]; m[14] = -q[2];
        }
    }
    json joints = json::array();
    for (int i = 0; i <= nb; i++) joints.push_back(i + 1);
    g.j["skins"] = json::array({json{{"name", name + "_skin"}, {"skeleton", 1}, {"joints", joints},
                                     {"inverseBindMatrices", g.acc_f(ibm.data(), nb + 1, 16)}}});
    // движения: перенос по земле и рывок ry — на root, высота — на кость-корень, углы — на кости
    json anims = json::array();
    json meta = json::array();
    for (int si : a.motion_sets) {
        const MotionSet& ms = c.motion_sets[si];
        const Bytes& blk = ms.ext ? *ms.ext : c.blocks[ms.block].data;
        for (size_t mi = 0; mi < ms.motions.size(); mi++) {
            std::string an = motion_anim_name(ms, (int)mi);
            if (anim_names_) {
                auto nit = anim_names_->find({si, (int)mi});
                if (nit == anim_names_->end()) continue;   // то же движение уже выгружено (каталог без повторов)
                an = nit->second;
            }
            const Motion& mo = ms.motions[mi];
            std::vector<float> times, rt, rq, ht;
            std::vector<std::vector<float>> bq(nb);
            float t = 0;
            for (size_t ki = 0; ki <= mo.keys.size(); ki++) {
                const MotionKey& k = mo.keys[std::min(ki, mo.keys.size() - 1)];
                times.push_back(t);
                if (ki < mo.keys.size()) t += k.hold / 30.0f;
                FramePose f = read_frame(blk, mo.data + (uint32_t)k.frame * mo.fsize, nb);
                rt.push_back(f.tx * 0.001f); rt.push_back(0); rt.push_back(-f.tz * 0.001f);
                Quat yq = to_quat(cv_rot(euler_xyz(0, f.ry, 0)));
                rq.push_back(yq.x); rq.push_back(yq.y); rq.push_back(yq.z); rq.push_back(yq.w);
                ht.push_back(0); ht.push_back(-f.ty * 0.001f); ht.push_back(0);
                for (int b = 0; b < nb; b++) {
                    Quat q = to_quat(cv_rot(euler_xyz(f.ang[b * 3], f.ang[b * 3 + 1], f.ang[b * 3 + 2])));
                    bq[b].push_back(q.x); bq[b].push_back(q.y); bq[b].push_back(q.z); bq[b].push_back(q.w);
                }
            }
            int at = g.acc_f(times.data(), times.size(), 1, true);
            json samplers = json::array(), channels = json::array();
            auto track = [&](int node, const char* path, const std::vector<float>& v, int nc) {
                samplers.push_back({{"input", at}, {"output", g.acc_f(v.data(), v.size() / nc, nc)}, {"interpolation", "LINEAR"}});
                channels.push_back({{"sampler", (int)samplers.size() - 1}, {"target", {{"node", node}, {"path", path}}}});
            };
            track(1, "translation", rt, 3);
            track(1, "rotation", rq, 4);
            int root_bone = 0;
            for (int i = 0; i < nb; i++) if (md.bones[i].parent < 0) { root_bone = i; break; }
            track(2 + root_bone, "translation", ht, 3);
            for (int b = 0; b < nb; b++) track(2 + b, "rotation", bq[b], 4);
            anims.push_back({{"name", an}, {"samplers", samplers}, {"channels", channels}});
            json ev = json::array();
            for (size_t ki = 0; ki < mo.keys.size(); ki++)
                if (mo.keys[ki].ev_flags) ev.push_back({{"key", ki}, {"flags", mo.keys[ki].ev_flags}, {"id", mo.keys[ki].ev_id}});
            meta.push_back({{"animation", an}, {"set", ms.name}, {"index", mi}, {"keys", mo.keys.size()}, {"frames", mo.nframes},
                            {"ticks", mo.ticks}, {"seconds", mo.ticks / 30.0}, {"events", ev}});
        }
    }
    if (!anims.empty()) g.j["animations"] = anims;
    save_model(g, dir, name);
    json bones = json::array();
    for (int i = 0; i < nb; i++) {
        const Bone& b = md.bones[i];
        bones.push_back({{"index", i}, {"parent", b.parent}, {"offset_mm", {b.ox, b.oy, b.oz}}, {"flags", b.flags}});
    }
    json info = {{"format", "dc-actor-1"}, {"source", c.file->name}, {"fps", 30}, {"units", "glTF: metres, Y up; game: mm, Y down"},
                 {"bones", bones}, {"vertices", md.verts.size()}, {"triangles", md.prims.size()}, {"animations", meta}};
    write_text(dir / u8path(name + ".json"), info.dump(1));
}

// ---------------------------------------------------------------- данные комнаты
static std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += strfmt("%02x", p[i]);
    return s;
}

void Exporter::write_room_data(const fs::path& dir, const FileContent& c) { write_room_json(dir, c, o_.room_part); }

void write_room_json(const fs::path& dir, const FileContent& c, const std::string& only) {
    const RoomInfo& r = c.room;
    const Bytes& blk = c.blocks[c.room_block].data;
    auto want = [&](const char* part) { return only.empty() || only == part; };
    if (r.coll.ok && want("collision")) {
        json rows = json::array();
        for (int i = 0; i < 17; i++) {
            json fl = json::array(), wl = json::array();
            for (auto& f : r.coll.floors[i])
                fl.push_back({{"type", f.type}, {"flags", f.flags}, {"pad", f.pad}, {"x", f.x}, {"z", f.z}, {"sx", f.sx}, {"sz", f.sz},
                              {"h", {f.h[0], f.h[1], f.h[2], f.h[3]}}});
            for (auto& w : r.coll.walls[i])
                wl.push_back({{"type", w.type}, {"flag", w.flag}, {"level_from", w.lvl_lo}, {"level_to", w.lvl_hi}, {"x", w.x}, {"z", w.z},
                              {"sx", w.sx}, {"sz", w.sz}, {"attr", w.attr}, {"spare", w.spare}});
            rows.push_back({{"row", i}, {"pad", r.coll.rows[i].pad}, {"spare", r.coll.rows[i].spare}, {"floors", fl}, {"walls", wl}});
        }
        json j = {{"format", "dc-collision-1"}, {"room", c.file->stem},
                  {"about", "Game collision (what Regina and the dinosaurs walk on and bump into). Row 0: walls for every floor level; "
                            "row L+1: floors and walls of level L (level = -Y/425). Floor: rectangle x..x+sx, z..z+sz with corner heights h "
                            "(type 0; 1 = circle radius sx; 2..6 = triangles). Wall: same shapes, active on levels [level_from, level_to) "
                            "(level_to 0 = all), attr bit 0x8000 = off. Units: mm, Y down."},
                  {"rows", rows}};
        write_text(dir / "collision.json", j.dump(1));
        // наглядно: OBJ (полы — плоскости по высотам углов, стены — призмы)
        std::string obj = "# game collision, native coordinates (mm, Y down)\n";
        int nv = 0;
        for (int i = 0; i < 17; i++) {
            for (size_t k = 0; k < r.coll.floors[i].size(); k++) {
                const FloorRec& f = r.coll.floors[i][k];
                if (!f.sx || !f.sz) continue;
                obj += strfmt("g floor_row%d_%d_type%d\n", i, (int)k, f.type);
                obj += strfmt("v %d %d %d\nv %d %d %d\nv %d %d %d\nv %d %d %d\n", f.x, f.h[0], f.z, f.x + f.sx, f.h[1], f.z,
                              f.x + f.sx, f.h[3], f.z + f.sz, f.x, f.h[2], f.z + f.sz);
                obj += strfmt("f %d %d %d %d\n", nv + 1, nv + 2, nv + 3, nv + 4);
                nv += 4;
            }
            for (size_t k = 0; k < r.coll.walls[i].size(); k++) {
                const WallRec& w = r.coll.walls[i][k];
                obj += strfmt("g wall_row%d_%d_type%d\n", i, (int)k, w.type);
                int x0 = w.x, z0 = w.z, x1 = w.x + w.sx, z1 = w.z + w.sz, y0 = -1200, y1 = 100;
                int px[4] = {x0, x1, x1, x0}, pz[4] = {z0, z0, z1, z1};
                for (int s = 0; s < 4; s++) obj += strfmt("v %d %d %d\nv %d %d %d\n", px[s], y0, pz[s], px[s], y1, pz[s]);
                for (int s = 0; s < 4; s++) {
                    int a = nv + 1 + s * 2, b = nv + 1 + ((s + 1) % 4) * 2;
                    obj += strfmt("f %d %d %d %d\n", a, b, b + 1, a + 1);
                }
                obj += strfmt("f %d %d %d %d\n", nv + 1, nv + 3, nv + 5, nv + 7);
                nv += 8;
            }
        }
        write_text(dir / "collision.obj", obj);
    }
    if (want("cameras")) {
        json cams = json::array();
        for (size_t i = 0; i < r.cameras.size(); i++) {
            const uint8_t* p = r.cameras[i].raw;
            json z1 = json::array(), z2 = json::array();
            for (int k = 0; k < 4; k++) { z1.push_back({s16(p + 4 + k * 4), s16(p + 6 + k * 4)}); z2.push_back({s16(p + 20 + k * 4), s16(p + 22 + k * 4)}); }
            cams.push_back({{"index", i}, {"type", p[0]}, {"mask", p[1]}, {"camera", p[2]}, {"level", p[3]}, {"zone", z1}, {"zone2", z2},
                            {"blend", {p[36], p[37], p[38], p[39]}}, {"target", {s16(p + 44), s16(p + 46), s16(p + 48)}},
                            {"eye", {s16(p + 52), s16(p + 54), s16(p + 56)}}, {"extra", s16(p + 60)}, {"raw", hex(p, 68)}});
        }
        json j = {{"format", "dc-cameras-1"}, {"room", c.file->stem},
                  {"about", "Fixed cameras: each record switches to camera `camera` while the player (on floor `level`) stands in "
                            "`zone` (4 corners x,z). eye/target in mm, Y down. `raw` keeps the bytes not named here."},
                  {"cameras", cams}};
        write_text(dir / "cameras.json", j.dump(1));
    }
    // зоны звука шагов (hdr[6]; разбор 05.10 — не «триггеры»), расстановка из скрипта: враги, предметы, двери, прочие зоны
    if (!r.triggers.empty() && want("footsteps")) write_text(dir / "footsteps.json", room_footsteps_json(c).dump(1));
    {
        if (want("enemies")) { json e = room_enemies_json(c); if (!e["enemies"].empty()) write_text(dir / "enemies.json", e.dump(1)); }
        if (want("items")) { json it = room_items_json(c); if (!it["items"].empty()) write_text(dir / "items.json", it.dump(1)); }
        if (want("doors")) { json d = room_doors_json(c); if (!d["doors"].empty()) write_text(dir / "doors.json", d.dump(1)); }
        if (want("zones")) { json z = room_zones_json(c); if (!z["zones"].empty()) write_text(dir / "zones.json", z.dump(1)); }
    }
    if (want("texts")) {
        json st = json::array();
        for (size_t i = 0; i < r.strings.size(); i++) {
            const GameString& s = r.strings[i];
            st.push_back({{"index", i}, {"script", s.sub}, {"offset", s.off - r.script_off}, {"codes", s.codes.size()}, {"text", s.text}});
        }
        json j = {{"format", "dc-texts-1"}, {"room", c.file->stem},
                  {"about", "Texts of the room (item checks, documents, prompts). \\n = new line, {#N} = game glyph N, {XXXX} = control code. "
                            "A changed text must not be longer than `codes` (including the end mark)."},
                  {"texts", st}};
        write_text(dir / "texts.json", j.dump(1));
    }
    if (r.script_size && want("script")) {
        json dirj = json::array();
        for (size_t i = 0; i < r.script_dir.size(); i++) {
            uint32_t a = r.script_dir[i], b = i + 1 < r.script_dir.size() ? r.script_dir[i + 1] : r.script_size;
            if (b < a || r.script_off + b > blk.size()) b = a;
            dirj.push_back({{"index", i}, {"offset", a}, {"size", b - a}, {"bytes", hex(&blk[r.script_off + a], b - a)}});
        }
        json j = {{"format", "dc-script-1"}, {"room", c.file->stem}, {"size", r.script_size}, {"subroutines", dirj}};
        write_text(dir / "script.json", j.dump(1));
    }
}

// ---------------------------------------------------------------- звук
// эффекты: WAV на частоте первого тона сэмпла (как звучит в игре); инструменты: 44.1 кГц, корневая нота center − shift/128
// (smpl: нота и её доля, петля); мелодии: song_N.wav — сыграно по драйверу игры (вступление и два тела петли, петля в smpl)

// речь и шумы: канал XA → chNN.wav (номер канала диска)
void Exporter::write_xa(const DiscFile& f, const FileContent& c, const fs::path& dir) {
    (void)f;
    for (auto& ch : c.xa) {
        std::vector<int16_t> pcm;
        xa_decode(d_, ch, pcm);
        write_wav(dir / u8path(strfmt("ch%02d.wav", ch.chan)), pcm.data(), pcm.size() / ch.channels, ch.channels, ch.rate);
    }
}

// ролик → <ИМЯ>.mp4 (H.264 + AAC, кадры 1:1, 30 в секунду)
void Exporter::write_movie(const DiscFile& f, const FileContent& c, const fs::path& dir) {
    auto frames = str_scan(d_, f);
    if (frames.empty()) { note("no video frames: " + f.name); return; }
    std::vector<int16_t> pcm;
    int ch = 2, rate = 37800;
    if (!c.xa.empty()) {
        xa_decode(d_, c.xa[0], pcm);
        ch = c.xa[0].channels;
        rate = c.xa[0].rate;
    }
    const int w = frames[0].w, h = frames[0].h;
    std::string err;
    bool ok = write_mp4(dir / u8path(f.stem + ".mp4"), w, h, kStrFps, (int)frames.size(),
                        [&](int i, std::vector<uint32_t>& px) {
                            if (log_ && log_->cancel) return false;
                            return frames[i].w == w && frames[i].h == h && str_decode(d_, frames[i], px);
                        },
                        pcm, ch, rate, err);
    if (!ok) note(f.name + ": " + err);
}

void Exporter::write_images(const FileContent& c, const fs::path& dir) {
    for (auto& im : c.images) write_png(dir / u8path(im.name + ".png"), im.w, im.h, im.rgba.data());
    for (auto& u : c.uploads) {
        if (u.type == 2) continue;   // палитры
        std::vector<uint32_t> px;
        c.vram.decode_raw15(u.x, u.y, u.w, u.h, px);
        write_png(dir / u8path(strfmt("vram_%d_%d.png", u.x, u.y)), u.w, u.h, px.data());
    }
}

// ---------------------------------------------------------------- файл целиком
bool Exporter::export_file(const DiscFile& f) {
    if (f.cat.empty()) return false;
    fs::path cat = src_ / u8path(f.cat);
    std::error_code ec;
    fs::create_directories(cat, ec);
    const bool part = !o_.room_part.empty();   // one part of a room: no game file, no other parts
    if (o_.native && !part) {
        // файл игры как есть — отдельно (source\game_files\<папка>\ИМЯ): в mods\<папка>\ИМЯ он подменяет файл игры целиком
        const fs::path nd = src_ / "game_files" / u8path(f.cat);
        fs::create_directories(nd, ec);
        if (f.stream) {
            // поток — сырыми секторами 2352 (как его принимает mods)
            Bytes all((size_t)f.nsec * 2352u);
            bool ok = true;
            for (uint32_t i = 0; i < f.nsec && ok; i++) ok = d_.read_raw(f.lba + i, &all[(size_t)i * 2352u]);
            if (ok) write_file(nd / u8path(f.name), all.data(), all.size());
            else note("raw sectors unreadable: " + f.name);
        } else {
            Bytes raw;
            if (d_.read_file(f, raw)) write_file(nd / u8path(f.name), raw.data(), raw.size());
        }
    }
    // программа игры и оверлеи этапов: таблицы в JSON
    if (f.cat == "other" && upper(f.name) == "SLUS_009.22") {
        fs::path dir = cat / u8path(f.stem);   // other\SLUS_009\ (рядом с файлом SLUS_009.22)
        fs::create_directories(dir, ec);
        write_text(dir / "door_scenes.json", exe_door_scenes_json().dump(1));
        write_text(dir / "items.json", exe_items_json().dump(1));
        write_text(dir / "texts.json", exe_texts_json().dump(1));
        return true;
    }
    if (f.cat == "other" && f.dir == "/PSX/BIN/") {
        Bytes ovl;
        if (!d_.read_file(f, ovl)) { note("cannot read " + f.name); return false; }
        json r = roster_json(ovl, f.stem[2] - '0');
        if (!r["variants"].empty()) {
            fs::path dir = cat / u8path(f.stem);
            fs::create_directories(dir, ec);
            write_text(dir / "roster.json", r.dump(1));
        }
        return true;
    }
    FileContent c;
    if (!analyze(d_, f, c)) { note("cannot read " + f.name); return false; }
    fs::path dir = cat / u8path(f.stem);
    bool any = false;
    auto ensure = [&] { fs::create_directories(dir, ec); any = true; };
    if (f.stream) {
        if (f.cat == "movie") { ensure(); write_movie(f, c, dir); }
        else if (o_.streams && !c.xa.empty()) { ensure(); write_xa(f, c, dir); }
        return true;
    }
    // персонажи, оружие, предметы CORE, модели переходов и звук — каталогом без повторов (export_actor/export_mesh/export_sounds);
    // у файла — только своё: тело комнаты (узел на группу) и её данные
    if (o_.models && (!part || o_.room_part == "mesh"))
        for (auto& m : c.meshes)
            if (m.kind == "room") {
                ensure();
                write_mesh(dir, f.stem, m, c.vram, f.stem);
                if (m.block >= 0) {   // ключи групп и отпечаток выгрузки (игра пропускает нетронутую копию)
                    std::vector<uint64_t> keys;
                    for (uint32_t h : m.hdrs) keys.push_back(group_key_blk(c.blocks[m.block].data, c.blocks[m.block].dest, h, false));
                    LibPlace at{&f, m.block, c.blocks[m.block].dest, m.hdrs.empty() ? 0u : m.hdrs[0], 0};
                    write_place_info(dir / u8path(f.stem + ".json"), "room-mesh", keys, at, {}, export_fps(dir, f.stem, o_.model_format));
                }
            }
    if (o_.data && c.is_room && (!part || o_.room_part != "mesh")) { ensure(); write_room_data(dir, c); }
    if (part) return true;
    if (o_.textures && f.cat == "screens" && (!c.images.empty() || !c.uploads.empty())) { ensure(); write_images(c, dir); }
    if (o_.textures && f.cat == "items" && !c.images.empty()) {   // картинки предметов: item_NNN.png (96×64), NNN — id предмета
        ensure();
        for (auto& im : c.images) write_png(dir / u8path(im.name + ".png"), im.w, im.h, im.rgba.data());
    }
    (void)any;
    return true;
}

// ---------------------------------------------------------------- каталог без повторов
// отпечаток выгрузки: FNV-1a 64 файлов <имя>.gltf и <имя>.bin подряд — игра (dc_meshrep.cpp, names.txt) по нему узнаёт
// нетронутую копию из source в mods и её не подменяет
static uint64_t files_fnv(const fs::path& dir, const std::string& name, std::initializer_list<const char*> exts) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const char* ext : exts) {
        Bytes b;
        if (!read_file(dir / u8path(name + ext), b)) continue;
        for (uint8_t c : b) { h ^= c; h *= 0x100000001B3ull; }
    }
    return h;
}

// 08.10.2026: отпечатки того, что эта выгрузка написала: glTF - «export_fnv» (.gltf + .bin) и OBJ в координатах игры
// рядом - «export_fnv_obj» (.obj + .mtl; раньше его не было, и нетронутую копию игра брала по этому OBJ); FBX -
// «export_fnv_fbx»; OBJ - «export_fnv_obj». Прочие поля в <имя>.json остаются как были.
static json export_fps(const fs::path& dir, const std::string& name, ModelFormat f) {
    json j = json::object();
    std::error_code ec;
    auto put = [&](const char* field, std::initializer_list<const char*> exts) {
        j[field] = strfmt("%016llx", (unsigned long long)files_fnv(dir, name, exts));
    };
    if (f == ModelFormat::Fbx) put("export_fnv_fbx", {".fbx"});
    else if (f == ModelFormat::Obj) put("export_fnv_obj", {".obj", ".mtl"});
    else {
        put("export_fnv", {".gltf", ".bin"});
        if (fs::exists(dir / u8path(name + ".obj"), ec)) put("export_fnv_obj", {".obj", ".mtl"});
    }
    return j;
}

// 08.10.2026: модель в выбранном формате (ExportOptions::model_format); glTF, собранный выгрузкой, - источник всех трёх
bool Exporter::save_model(Gltf& g, const fs::path& dir, const std::string& name) {
    switch (o_.model_format) {
    case ModelFormat::Fbx: return write_fbx(g, dir / u8path(name + ".fbx"));
    case ModelFormat::Obj: return write_obj(g, dir / u8path(name + ".obj"));
    default: return g.save(dir / u8path(name + ".gltf"));
    }
}

// сведения о вещи каталога рядом с ней: ключ (по нему игра узнаёт модель и подменяет её мешем из mods везде), где лежит
static void write_place_info(const fs::path& p, const std::string& what, const std::vector<uint64_t>& keys, const LibPlace& at,
                             const std::vector<LibPlace>& also, const json& fps) {
    json files = json::array({at.f->name});
    for (auto& a : also) if (std::find(files.begin(), files.end(), a.f->name) == files.end()) files.push_back(a.f->name);
    json k = json::array();
    for (uint64_t x : keys) k.push_back(strfmt("%016llx", (unsigned long long)x));
    json j = json::object();
    Bytes b;
    if (read_file(p, b)) { try { j = json::parse(b.begin(), b.end()); } catch (...) { j = json::object(); } }
    j["format"] = j.value("format", std::string("dc-") + what + "-1");
    j["file"] = at.f->name;
    j["files"] = files;
    j["keys"] = k;
    for (auto it = fps.begin(); it != fps.end(); ++it) j[it.key()] = it.value();
    write_text(p, j.dump(1));
}

// модель la с движениями mots (каталога; анимация <набор>_<NN> первого места) — в dir/<name>.gltf
static bool actor_with_motions(Disc& d, const Library& lib, const LibActor& la, const std::vector<int>& mots, FileContent& c,
                               ActorPart& a, std::vector<std::unique_ptr<Bytes>>& keep, std::map<std::pair<int, int>, std::string>& names) {
    if (!analyze(d, *la.at.f, c) || la.at.part < 0 || la.at.part >= (int)c.actors.size()) return false;
    a = c.actors[la.at.part];
    a.motion_sets.clear();
    std::map<int, int> local_of;                    // набор каталога → набор в c.motion_sets
    for (int m : mots) {
        if (m < 0 || m >= (int)lib.motions.size()) continue;
        const LibMotion& lm = lib.motions[m];
        if (lm.nbones != (int)a.model.bones.size()) continue;
        const int si = lm.set;
        if (si < 0 || si >= (int)lib.sets.size()) continue;
        const LibSet& s = lib.sets[si];
        auto it = local_of.find(si);
        int local = -1;
        if (it != local_of.end()) local = it->second;
        else {
            if (s.at.f == la.at.f && s.at.part >= 0 && s.at.part < (int)c.motion_sets.size()) {
                local = s.at.part;
                c.motion_sets[local].name = s.id;
            } else {
                FileContent o;
                if (analyze(d, *s.at.f, o) && s.at.part >= 0 && s.at.part < (int)o.motion_sets.size()) {
                    MotionSet ms = o.motion_sets[s.at.part];
                    keep.push_back(std::make_unique<Bytes>(std::move(o.blocks[ms.block].data)));
                    ms.ext = keep.back().get();
                    ms.name = s.id;
                    local = (int)c.motion_sets.size();
                    c.motion_sets.push_back(ms);
                }
            }
            local_of[si] = local;
            if (local >= 0) a.motion_sets.push_back(local);
        }
        if (local >= 0 && lm.at.motion >= 0 && lm.at.motion < (int)c.motion_sets[local].motions.size())
            names[{local, lm.at.motion}] = lm.name;
    }
    return true;
}

// 06.10.2026 (игрок: «одна анимация — в одном месте»): движения — только в основной модели персонажа и только свои (library.cpp
// own_motions); у костюмов и вариантов (тот же скелет) анимаций нет; движения сцен — у комнаты (export_scene_motions)
bool Exporter::export_actor(const Library& lib, int i, const std::vector<int>* only, const std::string& suffix) {
    if (i < 0 || i >= (int)lib.actors.size()) return false;
    const LibActor& la = lib.actors[i];
    std::vector<int> mots;
    if (only) mots = *only;
    else if (la.chr >= 0 && la.chr < (int)lib.chars.size() && !lib.chars[la.chr].actors.empty() && lib.chars[la.chr].actors[0] == i)
        mots = lib.chars[la.chr].motions;
    FileContent c;
    ActorPart a;
    std::vector<std::unique_ptr<Bytes>> keep;
    std::map<std::pair<int, int>, std::string> names;
    if (!actor_with_motions(d_, lib, la, mots, c, a, keep, names)) { note("cannot read " + la.at.f->name); return false; }
    std::error_code ec;
    fs::path dir = src_ / "characters" / u8path(la.id);
    fs::create_directories(dir, ec);
    const std::string name = la.id + suffix;
    anim_names_ = &names;
    write_actor(dir, name, c, a, la.at.f->stem);
    anim_names_ = nullptr;
    write_place_info(dir / u8path(name + ".json"), "actor", {la.key}, la.at, la.also, export_fps(dir, name, o_.model_format));
    return true;
}

bool Exporter::export_scene_motions(const Library& lib, const DiscFile* room) {
    auto sm = lib.scene_motions.find(room);
    const LibActor* rg = lib.actor("regina");
    if (sm == lib.scene_motions.end() || sm->second.empty() || !rg) return true;
    FileContent c;
    ActorPart a;
    std::vector<std::unique_ptr<Bytes>> keep;
    std::map<std::pair<int, int>, std::string> names;
    if (!actor_with_motions(d_, lib, *rg, sm->second, c, a, keep, names)) { note("cannot read " + rg->at.f->name); return false; }
    std::error_code ec;
    const std::string name = room->stem + "_scene";
    fs::path dir = src_ / "rooms" / u8path(room->stem) / "scene";
    fs::create_directories(dir, ec);
    anim_names_ = &names;
    write_actor(dir, name, c, a, rg->at.f->stem);
    anim_names_ = nullptr;
    return true;
}

bool Exporter::export_mesh(const Library& lib, int i) {
    if (i < 0 || i >= (int)lib.meshes.size()) return false;
    const LibMesh& lm = lib.meshes[i];
    FileContent c;
    if (!analyze(d_, *lm.at.f, c) || lm.at.part < 0 || lm.at.part >= (int)c.meshes.size()) { note("cannot read " + lm.at.f->name); return false; }
    std::error_code ec;
    fs::path dir = src_ / u8path(lm.cat) / u8path(lm.id);
    fs::create_directories(dir, ec);
    write_mesh(dir, lm.id, c.meshes[lm.at.part], c.vram, lm.at.f->stem);
    write_place_info(dir / u8path(lm.id + ".json"), "mesh", lm.keys, lm.at, lm.also, export_fps(dir, lm.id, o_.model_format));
    return true;
}

// звук без повторов: сэмпл — в папку первого файла, где он лежит (игра узнаёт его по содержимому везде), мелодия — так же
void Exporter::export_sounds(const Library& lib, const std::set<int>* only_samples, const std::set<int>* only_songs) {
    std::map<const DiscFile*, std::vector<const LibSample*>> by;
    for (size_t i = 0; i < lib.samples.size(); i++)
        if (!only_samples || only_samples->count((int)i)) by[lib.samples[i].at.f].push_back(&lib.samples[i]);
    std::map<const DiscFile*, std::vector<const LibSong*>> songs;
    for (size_t i = 0; i < lib.songs.size(); i++)
        if (!only_songs || only_songs->count((int)i)) songs[lib.songs[i].at.f].push_back(&lib.songs[i]);
    std::set<const DiscFile*> files;
    for (auto& kv : by) files.insert(kv.first);
    for (auto& kv : songs) files.insert(kv.first);
    std::error_code ec;
    for (const DiscFile* f : files) {
        if (log_ && log_->cancel) return;
        if (log_) { std::lock_guard<std::mutex> lk(log_->mu); log_->current = f->name; }
        FileContent c;
        if (!analyze(d_, *f, c)) continue;
        const bool sfx = f->cat != "sound\\bgm";
        for (const LibSample* s : by[f]) {
            if (s->bank >= (int)c.banks.size() || s->index >= (int)c.banks[s->bank].vab.samples.size()) continue;
            const VabSample& vs = c.banks[s->bank].vab.samples[s->index];
            fs::path p = sample_wav_path(src_, *s);
            fs::create_directories(p.parent_path(), ec);
            int root = vs.center >= 0 && vs.center < 128 ? vs.center : 60;
            uint32_t frac = 0;
            if (sfx) root = std::clamp(vs.min, 0, 127);
            else if (vs.shift > 0) { root = std::max(0, vs.center - 1); frac = (uint32_t)(((uint64_t)(128 - vs.shift) << 32) / 128); }
            write_wav(p, vs.pcm.data(), vs.pcm.size(), 1, sample_rate_for(vs, sfx), root, vs.loop_start, frac);
        }
        for (const LibSong* s : songs[f]) {
            SongPcm sp;
            if (!render_song(c, s->index, sp)) continue;
            fs::path dir = src_ / "sound" / "bgm" / u8path(f->stem);
            fs::create_directories(dir, ec);
            write_wav(dir / u8path(strfmt("song_%d.wav", s->index)), sp.pcm.data(), sp.pcm.size() / 2, 2, 44100, sp.loops ? 60 : -1,
                      sp.loop_start, 0, sp.loops ? sp.loop_end - 1 : -1);
        }
    }
}

bool Exporter::export_texture(const fs::path& rel_dir, const TexCrop& c) {
    if (c.w() <= 0 || c.h() <= 0 || c.rgba.empty()) return false;
    std::error_code ec;
    const fs::path dir = src_ / rel_dir;
    fs::create_directories(dir, ec);
    return write_png(dir / u8path(c.file_name()), c.w(), c.h(), c.rgba.data());
}

void Exporter::export_library(const Library& lib) {
    for (size_t i = 0; i < lib.actors.size(); i++) {
        if (log_ && log_->cancel) return;
        if (log_) { std::lock_guard<std::mutex> lk(log_->mu); log_->current = lib.actors[i].id; }
        if (!export_actor(lib, (int)i) && log_) log_->errors++;
        if (log_) log_->done++;
    }
    for (size_t i = 0; i < lib.meshes.size(); i++) {
        if (log_ && log_->cancel) return;
        if (log_) { std::lock_guard<std::mutex> lk(log_->mu); log_->current = lib.meshes[i].id; }
        if (!export_mesh(lib, (int)i) && log_) log_->errors++;
        if (log_) log_->done++;
    }
    for (auto& kv : lib.scene_motions) {                      // 06.10.2026: движения сцен — в папку комнаты
        if (log_ && log_->cancel) return;
        if (log_) { std::lock_guard<std::mutex> lk(log_->mu); log_->current = kv.first->stem + "\\scene"; }
        if (!export_scene_motions(lib, kv.first) && log_) log_->errors++;
    }
    if (o_.sounds) export_sounds(lib);
    if (log_) log_->done++;
}

// счётчики log_ (total, done, running) ведёт вызывающий: за файлами может идти каталог в том же потоке
void Exporter::export_files(const std::vector<const DiscFile*>& files) {
    if (log_ && log_->total < (int)files.size()) log_->total = (int)files.size();
    for (const DiscFile* f : files) {
        if (log_ && log_->cancel) break;
        if (log_) {
            std::lock_guard<std::mutex> lk(log_->mu);
            log_->current = f->name;
        }
        bool ok = false;
        try {
            ok = export_file(*f);
        } catch (const std::exception& e) {
            note(std::string("error: ") + f->name + ": " + e.what());
        }
        if (!ok && log_) log_->errors++;
        if (log_) log_->done++;
    }
}

}  // namespace dc
