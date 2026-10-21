// Dino Crisis ME Studio — командная строка:
//   list     <образ>                       файлы диска по папкам mods и что в них нашлось
//   export   <образ> <папка> [ИМЯ ...]     создать <папка>\source и выгрузить: всё, или названные файлы (ST100),
//                                          вещи каталога (regina, raptor, handgun, in_hand_H20), library — весь каталог
//   convert  <образ> <папка mods>          «Сконвертировать всё» (JSON → родные файлы)
//   selftest <образ>                       проверки: LZSS туда-обратно, коллизия байт в байт, тексты туда-обратно
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

#include "convert.h"
#include "library.h"
#include "anim_packs.h"
#include "motions.h"
#include "placement.h"
#include "mp4.h"
#include "script.h"
#include "script_edit.h"
#include "pieces.h"
#include "packs.h"
#include "scenario.h"
#include "scn_ops.h"
#include "scn_scenes.h"
#include "species.h"

using namespace dc;

// 10.10.2026: mod packages without the window (checks): packs <mods> | packnew <mods> <name> | packzip <mods> <dir> <zip> |
// packinstall <mods> <zip> [replace] | packorder <mods> <dir,-dir,...>
static int pack_cmd(int argc, char** argv) {
    const std::string cmd = argv[1];
    const fs::path mods = u8path(argv[2]);
    std::string err, dir;
    if (cmd == "packs") {
        PackSet ps;
        if (!ps.scan(mods)) { std::cerr << "no folder " << argv[2] << "\n"; return 1; }
        std::cout << "own files " << ps.own_files << "\n";
        for (auto& p : ps.packs) {
            std::cout << (p.on ? "[x] " : "[ ] ") << p.dir << " | " << p.name << " | " << p.version << " | files " << p.files << "\n";
            for (auto& s : p.wins) std::cout << "     wins  " << s << "\n";
            for (auto& s : p.loses) std::cout << "     loses " << s << "\n";
        }
        return 0;
    }
    if (cmd == "packnew" && argc > 3) {
        if (!pack_create(mods, argv[3], dir, err)) { std::cerr << err << "\n"; return 1; }
        std::cout << "new " << dir << "\n";
        return 0;
    }
    if (cmd == "packzip" && argc > 4) {
        int n = 0;
        if (!pack_build_zip(mods, argv[3], u8path(argv[4]), err, &n)) { std::cerr << err << "\n"; return 1; }
        std::cout << "zip " << argv[4] << " files " << n << "\n";
        return 0;
    }
    if (cmd == "packinstall" && argc > 3) {
        bool exists = false;
        const bool replace = argc > 4 && std::string(argv[4]) == "replace";
        if (!pack_install_zip(u8path(argv[3]), mods, replace, dir, exists, err)) {
            std::cerr << err << (exists ? " (exists)" : "") << "\n";
            return exists ? 3 : 1;
        }
        std::cout << "installed " << dir << "\n";
        return 0;
    }
    if (cmd == "packorder" && argc > 3) {
        PackSet ps;
        ps.scan(mods);
        std::vector<ModPack> o;
        std::string list = argv[3];
        size_t a = 0;
        while (a <= list.size()) {
            size_t b = list.find(',', a);
            if (b == std::string::npos) b = list.size();
            std::string d = list.substr(a, b - a);
            a = b + 1;
            if (d.empty()) continue;
            const bool off = d[0] == '-';
            if (off) d = d.substr(1);
            const int i = ps.find(d);
            if (i < 0) { std::cerr << "no package " << d << "\n"; return 1; }
            ModPack p = ps.packs[(size_t)i];
            p.on = !off;
            o.push_back(p);
        }
        ps.packs = o;
        if (!ps.save_order(err)) { std::cerr << err << "\n"; return 1; }
        std::cout << "order saved\n";
        return 0;
    }
    std::cerr << "usage: packs <mods> | packnew <mods> <name> | packzip <mods> <dir> <zip> | packinstall <mods> <zip> [replace] | "
                 "packorder <mods> <dir,-dir,...>\n";
    return 2;
}

static int usage() {
    std::cerr << "usage: list <image> | export <image> <folder> [NAME...] | convert <image> <mods> | selftest <image>\n";
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    if (cmd.compare(0, 4, "pack") == 0 && cmd != "packsize") return pack_cmd(argc, argv);   // 10.10: mod packages (no image)
    Disc d;
    std::string err;
    if (!d.open(u8path(argv[2]), err)) { std::cerr << "cannot open image: " << err << "\n"; return 1; }
    load_exe(d);
    std::cout << "image: " << path_u8(d.image_path) << " (" << (d.raw2352 ? "raw 2352" : "2048") << "), files " << d.files.size()
              << ", exe " << d.exe_name << ", Dino Crisis: " << (d.dino_crisis ? "yes" : "NO") << "\n";
    auto t0 = std::chrono::steady_clock::now();
    if (cmd == "library") {   // каталог без повторов: модели, меши, наборы движений
        Library lib;
        lib.build(d);
        int nplaces = 0;
        for (auto& m : lib.motions) nplaces += 1 + (int)m.also.size();
        std::cout << "characters " << lib.actors.size() << ", meshes " << lib.meshes.size() << ", motion sets " << lib.sets.size()
                  << ", motions " << lib.motions.size() << " (of " << nplaces << " in all files)\n";
        // персонажи: модели, свои движения, текстуры (каждое — у одного владельца)
        std::map<std::string, int> towners;
        for (auto& kv : lib.tex_owner) towners[kv.second]++;
        int nmot = 0, nscene = 0;
        for (auto& c : lib.chars) {
            nmot += (int)c.motions.size();
            int ntex = 0;
            for (int ai : c.actors) ntex += towners[strfmt("a%d", ai)];
            std::cout << strfmt("char %-16s %-14s motions %4d textures %3d models:", c.id.c_str(), c.kind.c_str(), (int)c.motions.size(), ntex);
            for (int ai : c.actors)
                std::cout << " " << lib.actors[ai].id << (lib.actors[ai].var_en.empty() ? "" : "[" + lib.actors[ai].var_en + "]") << "(" << lib.actors[ai].tex.size() << "t)";
            std::cout << "\n";
        }
        for (auto& kv : lib.scene_motions) { nscene += (int)kv.second.size(); std::cout << "scene " << kv.first->stem << ": " << kv.second.size() << "\n"; }
        std::cout << strfmt("motions owned %d + scene %d of %d\n", nmot, nscene, (int)lib.motions.size());
        {   // без владельца: по числу костей и файлам
            std::set<int> owned;
            for (auto& c : lib.chars) owned.insert(c.motions.begin(), c.motions.end());
            for (auto& kv : lib.scene_motions) owned.insert(kv.second.begin(), kv.second.end());
            std::map<int, std::map<std::string, int>> un;
            for (size_t m = 0; m < lib.motions.size(); m++)
                if (!owned.count((int)m)) un[lib.motions[m].nbones][lib.motions[m].at.f->stem]++;
            for (auto& kv : un) {
                std::cout << "unowned bones " << kv.first << ":";
                for (auto& f : kv.second) std::cout << " " << f.first << "(" << f.second << ")";
                std::cout << "\n";
            }
        }
        int twins = 0;
        for (auto& r : lib.rooms) if (r.twin) { twins++; std::cout << "room twin " << r.f->stem << " = " << r.twin->stem << "\n"; }
        std::cout << "room twins " << twins << ", textures " << lib.tex_owner.size() << "\n";
        for (auto& a : lib.actors) std::cout << "  " << a.id << ": motions " << a.motions.size() << "\n";
        for (auto& a : lib.actors) {
            std::cout << strfmt("actor %-20s %-14s key %016llx bones %2d verts %4d prims %4d in %s (+%d)  sets:", a.id.c_str(), a.kind.c_str(),
                                (unsigned long long)a.key, a.nbones, a.nverts, a.nprims, a.at.f->name.c_str(), (int)a.also.size());
            for (int si : a.sets) std::cout << " " << lib.sets[si].id << "(" << lib.sets[si].count << ")";
            std::cout << "\n";
        }
        for (auto& m : lib.meshes) {
            std::cout << strfmt("mesh  %-28s %-8s %-8s groups %d tris %4d in %s @%05X (+%d)%s keys", m.id.c_str(), m.cat.c_str(), m.kind.c_str(),
                                (int)m.keys.size(), m.ntris, m.at.f->name.c_str(), m.at.hdr, (int)m.also.size(), m.lit ? " lit" : "");
            for (uint64_t k : m.keys) std::cout << strfmt(" %016llx", (unsigned long long)k);
            for (auto& pl : m.also) std::cout << " " << pl.f->stem;
            std::cout << "\n";
        }
        int nsfx = 0, nins = 0, total_sfx = 0;
        for (auto& x : lib.samples) { (x.bgm ? nins : nsfx)++; total_sfx += 1 + (int)x.also.size(); }
        std::cout << strfmt("samples: effects %d, instruments %d (of %d in all files), songs %d\n", nsfx, nins, total_sfx, (int)lib.songs.size());
        for (auto& s : lib.sets)
            std::cout << strfmt("set   %-10s bones %2d motions %3d (+%d)\n", s.id.c_str(), s.nbones, s.count, (int)s.also.size());
    } else if (cmd == "skin") {   // проверка скина моделей: кости, общие вершины, длина рёбер в позе покоя по правилам
        Library lib;
        lib.build(d);
        for (auto& la : lib.actors) {
            FileContent c;
            if (!analyze(d, *la.at.f, c) || la.at.part < 0 || la.at.part >= (int)c.actors.size()) continue;
            const Model& md = c.actors[la.at.part].model;
            const int nb = (int)md.bones.size();
            int back = 0;
            for (int i = 0; i < nb; i++) if (md.bones[i].parent >= i) back++;
            std::vector<std::set<int>> users(md.verts.size());
            for (auto& p : md.prims) for (int k = 0; k < 3; k++) if (p.vi[k] < md.verts.size()) users[p.vi[k]].insert(p.bone);
            int shared = 0, beyond = 0;
            for (auto& u : users) shared += u.size() > 1;
            for (auto& p : md.prims) for (int k = 0; k < 3; k++) beyond += p.vi[k] >= md.verts.size();
            std::vector<BoneXf> rest;
            rest_bones(md, rest);
            auto pos = [&](int vi, int b, float o[3]) { for (int k = 0; k < 3; k++) o[k] = md.verts[vi][k] + rest[b].t[k]; };
            // 4-е полуслово вершины в файле
            std::vector<int> w4(md.verts.size(), -1);
            {
                const Bytes& blk = c.blocks[c.actors[la.at.part].block].data;
                for (int i = 0; i < md.nv_disk && md.vert_off + i * 8 + 8 <= blk.size(); i++) w4[i] = s16(&blk[md.vert_off + i * 8 + 6]);
            }
            int w_eq_max = 0, w_valid = 0;
            for (int i = 0; i < md.nv_disk; i++) { w_valid += w4[i] >= 0 && w4[i] < nb; w_eq_max += w4[i] == md.vbone[i]; }
            // кость по диапазонам: байт +7 записи кости — число троек вершин (func_80073F10: node+66, RTPT по 3 подряд)
            std::vector<int> rb(md.verts.size(), -1);
            int sum = 0;
            for (int b = 0; b < nb; b++)
                for (int k = 0; k < md.bones[b].flags * 3; k++, sum++)
                    if (sum < (int)rb.size()) rb[sum] = b;
            int r_eq_w = 0, r_eq_max = 0, used = 0, used_eq = 0;
            for (int i = 0; i < md.nv_disk; i++) {
                r_eq_w += rb[i] == w4[i];
                r_eq_max += rb[i] == md.vbone[i];
                if (!users[i].empty()) { used++; used_eq += rb[i] == md.vbone[i]; }
            }
            std::cout << strfmt("%s w4: valid %d of %d, equal to vertex-bone %d; ranges: sum %d (verts %d), =w4 %d, =vertex-bone %d, used %d (=vb %d)\n",
                                la.id.c_str(), w_valid, md.nv_disk, w_eq_max, sum, md.nv_disk, r_eq_w, r_eq_max, used, used_eq);
            for (int i = 0; i < md.nv_disk; i++)
                if (!users[i].empty() && rb[i] != md.vbone[i]) {
                    std::string u;
                    for (int b : users[i]) u += strfmt(" %d", b);
                    std::cout << strfmt("     v%d range-bone %d vertex-bone %d w4 %d users%s\n", i, rb[i], md.vbone[i], w4[i], u.c_str());
                }
            auto stats = [&](int rule) {   // 0 — кость вершины (старшая), 1 — кость примитива, 2 — младшая, 3 — w4
                std::vector<float> e;
                for (auto& p : md.prims) {
                    float q[3][3];
                    for (int k = 0; k < 3; k++) {
                        int vi = p.vi[k] < md.verts.size() ? p.vi[k] : 0;
                        int b = rule == 0 ? md.vbone[vi] : rule == 1 ? p.bone : rule == 2 ? *users[vi].begin()
                              : rule == 3 ? (w4[vi] >= 0 && w4[vi] < nb ? w4[vi] : md.vbone[vi]) : (rb[vi] >= 0 ? rb[vi] : md.vbone[vi]);
                        pos(vi, b, q[k]);
                    }
                    float mx = 0;
                    for (int k = 0; k < 3; k++) {
                        float dx = q[k][0] - q[(k + 1) % 3][0], dy = q[k][1] - q[(k + 1) % 3][1], dz = q[k][2] - q[(k + 1) % 3][2];
                        mx = std::max(mx, sqrtf(dx * dx + dy * dy + dz * dz));
                    }
                    e.push_back(mx);
                }
                std::sort(e.begin(), e.end());
                float med = e.empty() ? 0 : e[e.size() / 2];
                int big = 0;
                for (float x : e) big += x > med * 4 && x > 60;
                return strfmt("med %4.0f p99 %5.0f max %5.0f big %3d", med, e.empty() ? 0.f : e[e.size() * 99 / 100], e.empty() ? 0.f : e.back(), big);
            };
            std::cout << strfmt("%-18s bones %2d back %d verts %4d shared %3d beyond %d\n   vertex-bone: %s\n   prim-bone:   %s\n   min-bone:    %s\n   w4-bone:     %s\n   range-bone:  %s\n",
                                la.id.c_str(), nb, back, (int)md.verts.size(), shared, beyond, stats(0).c_str(), stats(1).c_str(), stats(2).c_str(),
                                stats(3).c_str(), stats(4).c_str());
            if (argc > 3 && la.id == argv[3])
                for (int i = 0; i < (int)md.verts.size(); i++) {
                    std::string u;
                    for (int b : users[i]) u += strfmt(" %d", b);
                    std::cout << strfmt("   v%3d %6d %6d %6d w4 %5d users%s\n", i, md.verts[i][0], md.verts[i][1], md.verts[i][2], w4[i], u.c_str());
                }
            if (argc > 3 && la.id == argv[3])
                for (int i = 0; i < nb; i++)
                    std::cout << strfmt("   bone %2d parent %2d off %6d %6d %6d flags %02x nt %3d nq %3d\n", i, md.bones[i].parent, md.bones[i].ox,
                                        md.bones[i].oy, md.bones[i].oz, md.bones[i].flags, md.bones[i].nt, md.bones[i].nq);
        }
    } else if (cmd == "spawnmot") {   // чьи движения: таблицы движений в командах появления (20/59/5B) → модель
        Library lib;
        lib.build(d);
        std::map<uint64_t, std::string> idk;
        for (auto& a : lib.actors) idk[a.key] = a.id;
        std::map<int, std::set<std::string>> ref;   // движение каталога → кто его играет по таблицам
        for (auto& f : d.files) {
            if (f.cat != "rooms" || f.stream) continue;
            FileContent c;
            if (!analyze(d, f, c) || !c.is_room) continue;
            std::map<uint32_t, std::pair<int, int>> hdr2m;
            for (size_t si = 0; si < c.motion_sets.size(); si++) {
                auto& ms = c.motion_sets[si];
                if (ms.block < 0 || ms.ext) continue;
                for (size_t mi = 0; mi < ms.motions.size(); mi++) hdr2m[c.blocks[ms.block].dest + ms.motions[mi].hdr] = {(int)si, (int)mi};
            }
            std::map<uint32_t, int> mdl2a;
            for (size_t ai = 0; ai < c.actors.size(); ai++)
                if (c.actors[ai].block >= 0) mdl2a[c.blocks[c.actors[ai].block].dest + c.actors[ai].model.hdr] = (int)ai;
            auto rd32 = [&](uint32_t addr, uint32_t& v) {
                for (auto& b : c.blocks)
                    if (addr >= b.dest && addr + 4 <= b.dest + b.data.size()) { v = le32(&b.data[addr - b.dest]); return true; }
                return false;
            };
            json en = room_enemies_json(c);
            std::set<std::pair<int, int>> seen_local;
            for (auto& e : en["enemies"]) {
                const uint32_t model = (uint32_t)std::stoul(e.value("model", std::string("0")), nullptr, 16);
                const uint32_t mot = (uint32_t)std::stoul(e.value("motions", std::string("0")), nullptr, 16);
                std::string who = "?";
                auto ai = mdl2a.find(model);
                if (ai != mdl2a.end()) {
                    const ActorPart& a = c.actors[ai->second];
                    uint64_t k = actor_key_blk(c.blocks[a.block].data, c.blocks[a.block].dest, a.model.hdr);
                    who = idk.count(k) ? idk[k] : strfmt("key%016llx", (unsigned long long)k);
                }
                int n = 0, matched = 0;
                uint32_t first = 0;
                for (int i = 0; i < 300; i++) {
                    uint32_t v = 0;
                    if (!rd32(mot + i * 4, v) || v < 0x80000000u || v >= 0x80200000u) break;
                    if (i == 0) first = v;
                    if (mot + i * 4 >= first && first) break;
                    n++;
                    auto it = hdr2m.find(v);
                    if (it != hdr2m.end()) {
                        matched++;
                        seen_local.insert(it->second);
                        const int m = lib.motion_at(&f, it->second.first, it->second.second);
                        if (m >= 0) ref[m].insert(who);
                    }
                }
                std::cout << strfmt("%-6s op %s kind %2d %-16s model %08X table %08X n %3d matched %3d\n", f.stem.c_str(), e.value("op", "").c_str(),
                                    e.value("kind", 0), who.c_str(), model, mot, n, matched);
            }
            // наборы файла: сколько движений не попало ни в одну таблицу
            for (size_t si = 0; si < c.motion_sets.size(); si++) {
                auto& ms = c.motion_sets[si];
                if (ms.block < 0 || ms.ext) continue;
                int un = 0;
                for (size_t mi = 0; mi < ms.motions.size(); mi++) un += !seen_local.count({(int)si, (int)mi});
                std::cout << strfmt("%-6s   set %zu bones %2d motions %3zu not in tables %3d\n", f.stem.c_str(), si, ms.nbones, ms.motions.size(), un);
            }
        }
        // итог по движениям каталога: сколько с одним владельцем, с несколькими, без таблицы
        int one = 0, many = 0, none = 0;
        std::map<std::string, int> combos;
        for (size_t m = 0; m < lib.motions.size(); m++) {
            auto it = ref.find((int)m);
            if (it == ref.end()) { none++; continue; }
            if (it->second.size() == 1) one++;
            else {
                many++;
                std::string k;
                for (auto& s : it->second) k += s + " ";
                combos[k]++;
            }
        }
        std::cout << strfmt("motions: one owner %d, several %d, no table %d\n", one, many, none);
        for (auto& kv : combos) std::cout << "  shared by " << kv.first << ": " << kv.second << "\n";
    } else if (cmd == "meshcheck") {   // вытянутые треугольники неподвижных мешей и тел комнат: ребро ≫ медианы
        Library lib;
        lib.build(d);
        auto check = [&](const std::string& name, const std::vector<MTri>& tris) {
            std::vector<float> e;
            for (auto& t : tris) {
                float mx = 0;
                for (int k = 0; k < 3; k++) {
                    const MVert &a = t.v[k], &b = t.v[(k + 1) % 3];
                    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                    mx = std::max(mx, sqrtf(dx * dx + dy * dy + dz * dz));
                }
                e.push_back(mx);
            }
            if (e.empty()) return;
            std::vector<float> s = e;
            std::sort(s.begin(), s.end());
            const float med = s[s.size() / 2];
            int big = 0;
            for (float x : e) big += x > med * 8 && x > 400;
            std::cout << strfmt("%-30s tris %5d med %6.0f p99 %6.0f max %6.0f big %d\n", name.c_str(), (int)e.size(), med, s[s.size() * 99 / 100], s.back(), big);
        };
        for (auto& m : lib.meshes) {
            FileContent c;
            if (!analyze(d, *m.at.f, c) || m.at.part < 0 || m.at.part >= (int)c.meshes.size()) continue;
            check(m.cat + "/" + m.id, c.meshes[m.at.part].tris);
        }
        for (auto& r : lib.rooms) {
            FileContent c;
            if (!analyze(d, *r.f, c)) continue;
            for (auto& m : c.meshes) if (m.kind == "room") check("rooms/" + r.f->stem, m.tris);
        }
    } else if (cmd == "list") {
        for (auto& f : d.files) {
            if (f.cat.empty()) continue;
            FileContent c;
            analyze(d, f, c);
            std::cout << strfmt("%-16s %-14s %9u  %s\n", f.name.c_str(), f.cat.c_str(), f.size, c.summary.c_str());
        }
    } else if (cmd == "export") {
        if (argc < 4) return usage();
        fs::path root = u8path(argv[3]) / "source";
        ExportLog log;
        ExportOptions o;
        // 08.10.2026: format=gltf|fbx|obj among the names - the format of meshes and models
        for (int i = 4; i < argc; i++) {
            const std::string a = argv[i];
            if (a.rfind("format=", 0) == 0 && !model_format_parse(a.substr(7), o.model_format)) {
                std::cerr << "unknown format: " << a.substr(7) << " (gltf, fbx, obj)\n";
                return 2;
            }
        }
        Exporter ex(d, root, o, &log);
        Library lib;
        lib.build(d);
        std::vector<const DiscFile*> todo;
        std::vector<int> actors, meshes;
        std::vector<const DiscFile*> scenes;
        bool want_lib = argc == 4;
        for (int i = 4; i < argc; i++) {
            const std::string a = argv[i];
            if (a.rfind("format=", 0) == 0 || a.rfind("anim=", 0) == 0 || a.rfind("motion=", 0) == 0) continue;
            if (a == "library") { want_lib = true; continue; }
            if (a.rfind("scene:", 0) == 0) {                   // 06.10.2026: движения сцен комнаты
                for (auto& f : d.files) if (f.cat == "rooms" && f.stem == upper(a.substr(6))) scenes.push_back(&f);
                continue;
            }
            bool hit = false;
            for (size_t k = 0; k < lib.actors.size(); k++) if (lib.actors[k].id == a) { actors.push_back((int)k); hit = true; }
            for (size_t k = 0; k < lib.meshes.size(); k++) if (lib.meshes[k].id == a) { meshes.push_back((int)k); hit = true; }
            for (auto& f : d.files)
                if (!f.cat.empty() && (upper(a) == f.stem || upper(a) == f.name)) { todo.push_back(&f); hit = true; }
            if (!hit) std::cerr << "no such file or catalog name: " << a << "\n";
        }
        if (argc == 4) for (auto& f : d.files) if (!f.cat.empty()) todo.push_back(&f);
        ex.export_files(todo);
        int nlib = 0;
        // 08.10.2026: anim=<pack> (anim_packs.h keys; anim=? lists them) or motion=<NAME> for the listed models
        std::string anim, motion;
        for (int i = 4; i < argc; i++) {
            const std::string a = argv[i];
            if (a.rfind("anim=", 0) == 0) anim = a.substr(5);
            if (a.rfind("motion=", 0) == 0) motion = a.substr(7);
        }
        for (int k : actors) {
            if (anim.empty() && motion.empty()) { nlib += ex.export_actor(lib, k); continue; }
            if (anim == "?") {
                for (const AnimPack& p : anim_packs(lib, k))
                    std::cout << lib.actors[k].id << "  " << (p.depth ? "    " : "") << p.key << "  " << p.en << "  ("
                              << p.motions.size() << ")\n";
                continue;
            }
            std::vector<int> mots;
            std::string suffix;
            int tgt = k;
            if (!motion.empty()) {
                for (size_t m = 0; m < lib.motions.size(); m++) if (lib.motions[m].name == motion) mots.push_back((int)m);
                if (mots.empty()) { std::cerr << "no such motion: " << motion << "\n"; continue; }
                if (!motion_fits(lib, mots[0], k)) {          // not a common one: with the model it is made for
                    tgt = motion_model(lib, mots[0]);
                    if (tgt < 0) { std::cerr << motion << ": no model it is made for\n"; continue; }
                    std::cout << motion << " is not a motion of " << lib.actors[k].id << "'s build: with " << lib.actors[tgt].id << "\n";
                }
                suffix = "_" + motion;
                if (actor_is_dino(lib.actors[tgt])) {         // dinosaurs: no animation or all of it
                    mots = anim_pack_motions(lib, tgt, "all");
                    suffix.clear();
                    std::cout << lib.actors[tgt].id << ": a dinosaur goes with all its animation\n";
                }
            } else {
                const auto packs = anim_packs(lib, k);
                const std::string eff = find_pack(packs, anim) ? anim : (anim == "none" ? "none" : "all");
                mots = anim_pack_motions(lib, k, eff);
                if (eff == "none") suffix = "_mesh";
                else if (eff != "all") { suffix = "_" + eff; std::replace(suffix.begin(), suffix.end(), ':', '_'); }
                std::cout << lib.actors[k].id << ": pack " << eff << ", " << mots.size() << " motions\n";
            }
            nlib += ex.export_actor(lib, tgt, &mots, suffix);
        }
        for (int k : meshes) nlib += ex.export_mesh(lib, k);
        for (const DiscFile* f : scenes) nlib += ex.export_scene_motions(lib, f);
        if (want_lib) { ex.export_library(lib); nlib += (int)(lib.actors.size() + lib.meshes.size()); }
        for (auto& l : log.lines) std::cout << l << "\n";
        std::cout << "exported " << todo.size() << " files, " << nlib << " catalog models, errors " << log.errors << "\n";
    } else if (cmd == "pieces") {
        // pieces <image> [ROOM|all]: CREATE commands with their own place / turn (placed pieces) per room
        const std::string want = argc > 3 ? upper(argv[3]) : std::string("ALL");
        int rooms = 0, total = 0, placed = 0, in_mesh = 0, twice = 0;
        for (auto& f : d.files) {
            if (f.cat != "rooms" || f.stream || (want != "ALL" && f.stem != want)) continue;
            FileContent c;
            if (!analyze(d, f, c) || !c.is_room || c.room_block < 0) continue;
            ScriptImage im;
            std::string err;
            if (!script_decompose(c.blocks[c.room_block].data, c.room, im, err)) { std::cout << f.stem << ": " << err << "\n"; continue; }
            const json p = room_pieces(c, script_to_text(im, f.stem));
            rooms++;
            std::map<std::pair<int, std::string>, int> where;
            for (auto& e : p) {
                total++;
                if (!e["placed"].get<bool>()) continue;
                placed++;
                if (e["gi"].get<int>() >= 0) in_mesh++;
                const std::string k = strfmt("%d,%d,%d", e["x"].get<int>(), e["y"].get<int>(), e["z"].get<int>());
                if (++where[{e["gi"].get<int>(), k}] == 1) {
                    int n = 0;
                    for (auto& w : where) if (w.first.first == e["gi"].get<int>()) n++;
                    if (n == 2) twice++;
                }
                if (want != "ALL")
                    std::cout << strfmt("%s @%04X sub %2d slot %2d grp %s gi %3d place %6d %6d %6d turn %5d %5d %5d anim %d\n", f.stem.c_str(),
                                        e["at"].get<int>(), e["sub"].get<int>(), e["slot"].get<int>(), e["group"].get<std::string>().c_str(),
                                        e["gi"].get<int>(), e["x"].get<int>(), e["y"].get<int>(), e["z"].get<int>(), e["rx"].get<int>(),
                                        e["ry"].get<int>(), e["rz"].get<int>(), e["anim"].get<int>());
            }
        }
        std::cout << strfmt("rooms %d, CREATE %d, with own place %d (%d in the room mesh), groups put at two or more places %d\n",
                            rooms, total, placed, in_mesh, twice);
    } else if (cmd == "convert") {
        if (argc < 4) return usage();
        ConvertReport rep;
        convert_all(d, u8path(argv[3]), rep);
        for (auto& l : rep.lines) std::cout << l << "\n";
        std::cout << "built " << rep.built << ", unchanged " << rep.unchanged << ", errors " << rep.errors << "\n";
        return rep.errors ? 1 : 0;
    } else if (cmd == "scripttest") {
        // scripttest <образ> [ИМЯ]: скрипт каждой комнаты — разобрать и собрать (как есть, через script.txt) — байт в байт;
        // вставка и удаление команды — переходы и ссылки на новых местах
        int ok = 0, bad = 0, rooms = 0, atoms = 0, syms = 0;
        for (auto& f : d.files) {
            if (f.cat != "rooms" || f.stream) continue;
            if (argc > 3 && upper(argv[3]) != f.stem) continue;
            FileContent c;
            if (!analyze(d, f, c) || !c.is_room || !c.room.script_size) continue;
            rooms++;
            const Bytes& blk = c.blocks[c.room_block].data;
            ScriptImage im;
            std::string e;
            if (!script_decompose(blk, c.room, im, e)) { bad++; std::cout << f.stem << ": " << e << "\n"; continue; }
            atoms += (int)im.atoms.size();
            for (auto& a : im.atoms) syms += (int)a.syms.size();
            Bytes sec;
            const Bytes orig(blk.begin() + c.room.script_off, blk.begin() + c.room.script_off + c.room.script_size);
            if (!script_assemble(im, im.atoms, sec, e) || sec != orig) {
                bad++;
                size_t k = 0;
                while (k < sec.size() && k < orig.size() && sec[k] == orig[k]) k++;
                std::cout << f.stem << ": as-is rebuild differs at +" << strfmt("%X", (unsigned)k) << " " << e << "\n";
                continue;
            }
            std::vector<ScriptAtom> back;
            const std::string txt = script_to_text(im, f.stem);
            if (!script_from_text(im, txt, back, e) || !script_assemble(im, back, sec, e) || sec != orig) {
                bad++;
                std::cout << f.stem << ": script.txt round trip differs: " << e << "\n";
                if (argc > 3) write_text(u8path(f.stem + "_script.txt"), txt);
                continue;
            }
            // сдвиг: пустая команда после каждой 3-й — каждая ссылка в новой секции должна вести в тот же атом, что и прежде
            {
                std::vector<ScriptAtom> sh;
                int n = 0;
                for (auto& a : im.atoms) {
                    sh.push_back(a);
                    if (a.kind == ScriptAtom::OP && ++n % 3 == 0) {
                        ScriptAtom nop;
                        nop.kind = ScriptAtom::OP;
                        nop.b = {0x0D, 0, 0, 0};
                        sh.push_back(nop);
                    }
                }
                Bytes s2;
                if (!script_assemble(im, sh, s2, e)) { bad++; std::cout << f.stem << ": shift: " << e << "\n"; continue; }
                std::map<uint32_t, int64_t> back_of;   // новое начало атома → старое смещение
                uint32_t pos = 0;
                for (auto& a : sh) { back_of[pos] = a.old; pos += (uint32_t)a.b.size(); }
                int wrong = 0;
                pos = 0;
                for (auto& a : sh) {
                    for (auto& s : a.syms) {
                        uint32_t t = 0;
                        if (s.type == 2) t = le32(&s2[pos + s.at]);
                        else t = s.type == 0 ? pos + (int16_t)le16(&s2[pos + s.at]) : pos - (int16_t)le16(&s2[pos + s.at]);
                        // цель — начало атома с тем же прежним смещением (или внутри атома, куда указывала прежде)
                        auto it = back_of.upper_bound(t);
                        if (it == back_of.begin()) { wrong++; continue; }
                        --it;
                        const int64_t old_at = it->second;
                        const int ci = im.containing(s.target);
                        if (old_at < 0 || ci < 0 || im.atoms[ci].old != old_at || (int64_t)(t - it->first) != (int64_t)s.target - old_at) wrong++;
                    }
                    pos += (uint32_t)a.b.size();
                }
                if (wrong) { bad++; std::cout << f.stem << ": shift: " << wrong << " references lead elsewhere\n"; continue; }
            }
            // 10.10.2026: new commands with labels, a new subroutine and a new text: atoms -> script.txt -> atoms give the same
            // section; the new directory entry, the jump and the text reference land on the new atoms
            {
                std::vector<ScriptAtom> nw = im.atoms;
                int nid = script_next_nid(nw);
                ScriptAtom w, r, br, t, tx;
                w.kind = r.kind = br.kind = tx.kind = ScriptAtom::OP;
                w.nid = nid++;
                w.b = {0x02, 0, 1, 0};
                r.nid = nid++;
                r.b = {0x04, 0, 0, 0};
                br.nid = nid++;
                br.b = {0x0E, 0, 0, 0};
                br.syms.push_back({2, 0, 0, r.nid});
                t.kind = ScriptAtom::STRING;
                t.nid = nid++;
                std::vector<uint16_t> codes;
                if (!encode_text("Studio \"test\"\nline 2", codes, e) || !script_set_text(t, codes, e)) { bad++; std::cout << f.stem << ": new text: " << e << "\n"; continue; }
                tx.nid = nid++;
                tx.b = {0x21, 0, 0, 0, 0, 0, 0, 0};
                tx.syms.push_back({4, 2, 0, t.nid});
                for (auto* a : {&w, &tx, &br, &t, &r}) nw.push_back(*a);
                const int ksub = (int)nw[0].b.size() / 4;
                nw[0].syms.push_back({(int)nw[0].b.size(), 2, 0, w.nid});
                nw[0].b.insert(nw[0].b.end(), 4, 0);
                Bytes s1, s2;
                std::vector<ScriptAtom> back2;
                const std::string t2 = script_atoms_text(im, nw, f.stem);
                if (!script_assemble(im, nw, s1, e) || !script_from_text(im, t2, back2, e) || !script_assemble(im, back2, s2, e) || s1 != s2) {
                    bad++;
                    std::cout << f.stem << ": new commands round trip: " << (e.empty() ? std::string("sections differ") : e) << "\n";
                    if (argc > 3) write_text(u8path(f.stem + "_new_script.txt"), t2);
                    continue;
                }
                uint32_t pos = 0, pw = 0, pr = 0, pbr = 0, pt = 0, ptx = 0;
                for (auto& a : nw) {
                    if (a.nid == w.nid) pw = pos;
                    if (a.nid == r.nid) pr = pos;
                    if (a.nid == br.nid) pbr = pos;
                    if (a.nid == t.nid) pt = pos;
                    if (a.nid == tx.nid) ptx = pos;
                    pos += (uint32_t)a.b.size();
                }
                const bool good = le32(&s1[ksub * 4]) == pw && pbr + (int16_t)le16(&s1[pbr + 2]) == pr && le32(&s1[ptx + 4]) == pt &&
                                  le32(&s1[0]) == im.dir[0] + 4;
                if (!good) { bad++; std::cout << f.stem << ": new commands: a reference leads elsewhere\n"; continue; }
            }
            ok++;
        }
        std::cout << strfmt("rooms %d: byte-exact %d, failed %d; atoms %d, references %d\n", rooms, ok, bad, atoms, syms);
        if (bad) return 1;
    } else if (cmd == "scenariodump") {
        // 10.10.2026: scenariodump <image> <NAME> <out.txt>: the scenario of a room as the studio shows it (events, lines)
        if (argc < 5) return usage();
        const DiscFile* f = d.find(upper(argv[3]) + ".DAT");
        FileContent c;
        if (!f || !analyze(d, *f, c) || !c.is_room) { std::cerr << "not a room\n"; return 1; }
        ScriptImage im;
        std::string e;
        if (!script_decompose(c.blocks[c.room_block].data, c.room, im, e)) { std::cerr << e << "\n"; return 1; }
        Scenario s = scn_parse(im, im.atoms);
        ScnRoomNames rn;
        scn_room_names(im.atoms, rn);
        ScnCtx nm;
        scn_ctx_bind(nm, rn);
        nm.sub = [](int k) { return strfmt("sub %d", k); };
        nm.text = [&](const ScriptSym& sy) { return script_text_quote(im.atoms, sy, 60); };
        std::ostringstream o;
        int known = 0, all = 0;
        std::function<void(const std::vector<ScnNode>&, int)> pr = [&](const std::vector<ScnNode>& ns, int ind) {
            for (auto& n : ns) {
                if (n.k == ScnNode::DATA) continue;
                const std::string pad(ind * 3, ' ');
                if (n.k == ScnNode::IF) {
                    std::vector<const ScriptAtom*> ca;
                    for (int k : n.cond) ca.push_back(&im.atoms[k]);
                    o << pad << scn_if_phrase(ca, im.atoms[n.atom], nm) << "\n";
                    if (!n.cond.empty()) nm.last_test = im.atoms[n.cond.back()].b[0];
                    pr(n.a, ind + 1);
                    if (n.atom2 >= 0) { o << pad << "Else\n"; pr(n.b, ind + 1); }
                    continue;
                }
                if (n.k == ScnNode::LOOP) {
                    o << pad << strfmt("Repeat %d times", (int)le16(&im.atoms[n.atom].b[4])) << "\n";
                    pr(n.a, ind + 1);
                    continue;
                }
                const ScriptAtom& a = im.atoms[n.atom];
                all++;
                // jumps outside IF / LOOP read the last test; they are part of the flow, not unknown commands
                const bool jmp = a.kind == ScriptAtom::OP && !a.b.empty() &&
                                 (a.b[0] == 0x09 || a.b[0] == 0x0A || a.b[0] == 0x0B || a.b[0] == 0x0C || a.b[0] == 0x0E || a.b[0] == 0x17);
                if (scn_op(a) || jmp) known++;
                o << pad << (scn_op(a) || jmp ? "" : "? ") << (jmp ? scn_jump_phrase(a, nm) : scn_line_phrase(a, nm)) << "\n";
                if (a.b.size() >= 4 && a.b[0] == 0x22) { nm.sel_cat = a.b[1]; nm.sel_num = a.b[2]; }
                if (a.kind == ScriptAtom::OP && !a.b.empty() && scn_is_cond_op(a.b[0])) nm.last_test = a.b[0];
            }
        };
        for (auto& g : s.regions) {
            if (g.subs.empty()) continue;
            o << "\n== sub";
            for (int k : g.subs) o << " " << k;
            o << "\n";
            nm.sel_cat = nm.sel_num = 0;
            pr(g.nodes, 1);
        }
        write_text(u8path(argv[4]), o.str());
        std::cout << f->stem << strfmt(": commands %d, named %d (%.0f%%)\n", all, known, all ? 100.0 * known / all : 0.0);
    } else if (cmd == "scenarioadd") {
        // 10.10.2026: scenarioadd <image> <NAME> <sub> "<text>" <out script.txt>: a message (game paused, waits for the
        // player) before the last End of a subroutine, as the studio's Add > Messages > Message does - for checks in the game
        if (argc < 7) return usage();
        const DiscFile* f = d.find(upper(argv[3]) + ".DAT");
        FileContent c;
        if (!f || !analyze(d, *f, c) || !c.is_room) { std::cerr << "not a room\n"; return 1; }
        ScriptImage im;
        std::string e;
        if (!script_decompose(c.blocks[c.room_block].data, c.room, im, e)) { std::cerr << e << "\n"; return 1; }
        std::vector<ScriptAtom> at = im.atoms;
        Scenario s = scn_parse(im, at);
        const int sub = atoi(argv[4]);
        int reg = -1;
        for (int r = 0; r < (int)s.regions.size(); r++)
            for (int k : s.regions[r].subs) if (k == sub) reg = r;
        if (reg < 0) { std::cerr << "no such subroutine\n"; return 1; }
        auto add = [&](ScriptAtom a) { a.nid = script_next_nid(at); at.push_back(a); return (int)at.size() - 1; };
        ScriptAtom st;
        st.kind = ScriptAtom::STRING;
        std::vector<uint16_t> codes;
        std::string msg = argv[5];
        for (size_t k; (k = msg.find("\\n")) != std::string::npos;) msg.replace(k, 2, "\n");   // \n typed in a shell = a new line
        if (!encode_text(msg, codes, e) || !script_set_text(st, codes, e)) { std::cerr << e << "\n"; return 1; }
        const int si = add(st);
        ScriptAtom tx = scn_new_atom(0x21);
        put16(&tx.b[2], 0xFFFF);
        ScriptSym ts;
        ts.at = 4;
        ts.type = 2;
        ts.tnew = at[si].nid;
        tx.syms.push_back(ts);
        ScnNode tn, wn, dn;
        tn.atom = add(tx);
        wn.atom = add(scn_new_atom(0x12));
        std::vector<ScnNode>* list = nullptr;
        int pos = 0;
        scn_insert_point(s.regions[reg], at, std::string(), list, pos);
        list->insert(list->begin() + pos, wn);
        list->insert(list->begin() + pos, tn);
        dn.k = ScnNode::DATA;
        dn.atom = si;
        s.regions.back().nodes.push_back(dn);
        Bytes sec;
        if (!scn_flatten(s, at, e) || !script_assemble(im, at, sec, e)) { std::cerr << e << "\n"; return 1; }
        write_text(u8path(argv[6]), script_atoms_text(im, at, f->stem));
        std::cout << f->stem << strfmt(": message added to sub %d, script %u -> %u bytes\n", sub, c.room.script_size, (unsigned)sec.size());
    } else if (cmd == "scenarioscene") {
        // 14.10.2026: scenarioscene <image> <NAME> <sub> <in|leap> <out script.txt> [wait] [x,y,z,turn] [land x,y,z]: an enemy
        // comes in as in the game (scn_scenes.h) - a new event started from subroutine <sub>, as the studio's Add does;
        // checks that the block reads back as one scene from the written text
        if (argc < 7) return usage();
        const DiscFile* f = d.find(upper(argv[3]) + ".DAT");
        FileContent c;
        if (!f || !analyze(d, *f, c) || !c.is_room) { std::cerr << "not a room\n"; return 1; }
        ScriptImage im;
        std::string e;
        if (!script_decompose(c.blocks[c.room_block].data, c.room, im, e)) { std::cerr << e << "\n"; return 1; }
        std::vector<ScriptAtom> at = im.atoms;
        Scenario s = scn_parse(im, at);
        const int sub = atoi(argv[4]);
        int reg = -1;
        for (int r = 0; r < (int)s.regions.size(); r++)
            for (int k : s.regions[r].subs) if (k == sub) reg = r;
        if (reg < 0) { std::cerr << "no such subroutine\n"; return 1; }
        ScnScene sc;
        const int kind = std::string(argv[5]) == "leap" ? SCENE_LEAP_IN : SCENE_COME_IN;
        if (!scn_scene_defaults(at, kind, sc, e)) { std::cerr << e << "\n"; return 1; }
        const bool none_here = sc.model == 0;
        {   // a fighter's motions when the room's raptor is a scene's (as the studio does)
            SpeciesSrc src;
            const int own = species_motions(c, sc.motions);
            if (species_fighter(d, c, sc.model, sc.dkind, src) && (!sc.model || src.nmot > own * 2)) {
                sc.from = strfmt("%s %08X %08X", src.room.c_str(), src.model, src.motions);
                if (!sc.model) { sc.model = src.model; sc.motions = src.motions; }   // the room has no raptor: all of it comes in
                std::cout << strfmt("  the fighter of %s (%d motions) comes in on build; the room's own raptor: %d\n", src.room.c_str(), src.nmot, own);
            }
        }
        if (!sc.model) { std::cerr << "no raptor in the game data to bring in\n"; return 1; }
        if (none_here) std::cout << "  no raptor in this room: its body comes in too\n";
        PlacementCtx pc;
        pc.ensure(d);
        for (int b = 255; b >= 1; b--) if (!pc.used45.count(b)) { sc.death_flag = b; break; }
        const int wait = argc > 7 ? atoi(argv[7]) : 30;
        if (argc > 8) sscanf(argv[8], "%d,%d,%d,%d", &sc.place[0], &sc.place[1], &sc.place[2], &sc.turn);
        if (argc > 9) sscanf(argv[9], "%d,%d,%d", &sc.land[0], &sc.land[1], &sc.land[2]);
        if (argc > 8 && kind == SCENE_COME_IN) {   // the camera again for the given place: 3.5 m in front of it, up
            const float a = sc.turn * 6.2831853f / 4096;
            sc.target[0] = sc.place[0]; sc.target[1] = sc.place[1] - 700; sc.target[2] = sc.place[2];
            sc.eye[0] = sc.place[0] + (int)(3500 * sinf(a)); sc.eye[1] = sc.place[1] - 1800; sc.eye[2] = sc.place[2] + (int)(3500 * cosf(a));
        }
        if (!scn_scene_add(s, at, reg, std::string(), sc, wait, e)) { std::cerr << e << "\n"; return 1; }
        Bytes sec;
        if (!scn_flatten(s, at, e) || !script_assemble(im, at, sec, e)) { std::cerr << e << "\n"; return 1; }
        const std::string txt = script_atoms_text(im, at, f->stem);
        write_text(u8path(argv[6]), txt);
        // read back: the text gives the same bytes and the block is found again
        std::vector<ScriptAtom> back;
        Bytes sec2;
        int found = 0;
        if (!script_from_text(im, txt, back, e) || !script_assemble(im, back, sec2, e)) { std::cerr << "read back: " << e << "\n"; return 1; }
        Scenario s2 = scn_parse(im, back);
        std::function<void(const std::vector<ScnNode>&)> walk = [&](const std::vector<ScnNode>& ns) {
            for (auto& n : ns) {
                ScnScene t;
                if (scn_scene_detect(back, n, t)) found++;
                walk(n.a);
                walk(n.b);
            }
        };
        for (auto& g : s2.regions) walk(g.nodes);
        std::cout << f->stem << strfmt(": %s, slot %d, death flag %d, signal %d, model %08X motions %08X\n", scn_scene_name(kind), sc.slot,
                                       sc.death_flag, sc.signal, sc.model, sc.motions)
                  << strfmt("  place %d %d %d turn %d, land %d %d %d, eye %d %d %d, target %d %d %d\n", sc.place[0], sc.place[1],
                            sc.place[2], sc.turn, sc.land[0], sc.land[1], sc.land[2], sc.eye[0], sc.eye[1], sc.eye[2], sc.target[0],
                            sc.target[1], sc.target[2])
                  << strfmt("  script %u -> %u bytes, read back %s, scenes found %d\n", c.room.script_size, (unsigned)sec.size(),
                            sec2 == sec ? "same" : "DIFFERENT", found);
        if (sec2 != sec || found != 1) return 1;
    } else if (cmd == "scenarioedit") {
        // 10.10.2026: scenarioedit <image> [NAME]: the studio's edits on every event of every room (scenario.h): a new empty
        // command moved up and down, an IF with a command inside, a message (new text + TEXT + wait), the first line removed;
        // the result assembles, and its script.txt reads back to the same bytes
        int rooms = 0, ok = 0, bad = 0, events = 0;
        for (auto& f : d.files) {
            if (f.cat != "rooms" || f.stream) continue;
            if (argc > 3 && upper(argv[3]) != f.stem) continue;
            FileContent c;
            if (!analyze(d, f, c) || !c.is_room || !c.room.script_size) continue;
            rooms++;
            ScriptImage im;
            std::string e;
            if (!script_decompose(c.blocks[c.room_block].data, c.room, im, e)) { bad++; continue; }
            std::vector<ScriptAtom> at = im.atoms;
            Scenario s = scn_parse(im, at);
            auto add = [&](ScriptAtom a) { a.nid = script_next_nid(at); at.push_back(a); return (int)at.size() - 1; };
            bool fail = false;
            for (int r = 0; r < (int)s.regions.size() && !fail; r++) {
                ScnRegion& g = s.regions[r];
                if (g.subs.empty()) continue;
                std::string key0;
                for (auto& n : g.nodes)
                    if (n.k == ScnNode::CMD && !scn_is_final_end(g, at, scn_key(at[n.atom]))) { key0 = scn_key(at[n.atom]); break; }
                if (key0.empty()) continue;
                events++;
                std::vector<ScnNode>* list = nullptr;
                int pos = 0;
                // a) an empty command after the first line, moved up, then down twice
                ScnNode nop;
                nop.atom = add(scn_new_atom(0x0D));
                scn_insert_point(g, at, key0, list, pos);
                list->insert(list->begin() + pos, nop);
                const std::string nk = scn_key(at[nop.atom]);
                scn_move(g, at, nk, -1);
                scn_move(g, at, nk, +1);
                scn_move(g, at, nk, +1);
                // b) If flag 0/1 with an empty command inside
                ScnNode in;
                in.k = ScnNode::IF;
                ScriptAtom tf = scn_new_atom(0x26), br = scn_new_atom(0x0E);
                tf.b[2] = 1;
                br.b[1] = 6;
                ScriptSym sy;
                sy.at = 2;
                br.syms.push_back(sy);
                in.cond.push_back(add(tf));
                in.atom = add(br);
                ScnNode inner;
                inner.atom = add(scn_new_atom(0x0D));
                in.a.push_back(inner);
                scn_insert_point(g, at, key0, list, pos);
                list->insert(list->begin() + pos, in);
                // c) a message: a new text at the end of the script, TEXT (game paused) + wait
                ScriptAtom st;
                st.kind = ScriptAtom::STRING;
                std::vector<uint16_t> codes;
                if (!encode_text(strfmt("Test %s sub %d", f.stem.c_str(), g.subs[0]), codes, e) || !script_set_text(st, codes, e)) { fail = true; break; }
                const int si = add(st);
                ScriptAtom tx = scn_new_atom(0x21);
                put16(&tx.b[2], 0xFFFF);
                ScriptSym ts;
                ts.at = 4;
                ts.type = 2;
                ts.tnew = at[si].nid;
                tx.syms.push_back(ts);
                ScnNode tn, wn;
                tn.atom = add(tx);
                wn.atom = add(scn_new_atom(0x12));
                scn_insert_point(g, at, key0, list, pos);
                list->insert(list->begin() + pos, wn);
                list->insert(list->begin() + pos, tn);
                ScnNode dn;
                dn.k = ScnNode::DATA;
                dn.atom = si;
                s.regions.back().nodes.push_back(dn);
                // d) the first line removed
                if (!scn_remove(s, at, r, key0)) { fail = true; std::cout << f.stem << ": cannot remove " << key0 << "\n"; break; }
            }
            std::vector<ScriptAtom> flat = at;
            Bytes s1, s2;
            std::vector<ScriptAtom> back;
            std::string txt;
            if (!fail && scn_flatten(s, flat, e) && script_assemble(im, flat, s1, e)) {
                txt = script_atoms_text(im, flat, f.stem);
                if (script_from_text(im, txt, back, e) && script_assemble(im, back, s2, e) && s1 == s2) { ok++; continue; }
            }
            bad++;
            std::cout << f.stem << ": " << (e.empty() ? std::string("sections differ") : e) << "\n";
            if (argc > 3 && !txt.empty()) write_text(u8path(f.stem + "_edit_script.txt"), txt);
        }
        std::cout << strfmt("rooms %d: ok %d, failed %d; events edited %d\n", rooms, ok, bad, events);
        if (bad) return 1;
    } else if (cmd == "scenariotest") {
        // 10.10.2026: scenariotest <image> [NAME]: the script of every room as a tree (scenario.h) and back - byte-exact;
        // then a new empty command at the end of every IF / LOOP block: the tree keeps its shape and the jumps follow
        int rooms = 0, ok = 0, bad = 0;
        long n_if = 0, n_else = 0, n_loop = 0, n_raw = 0, n_cmd = 0;
        std::function<void(const std::vector<ScnNode>&, long&, long&, long&)> count = [&](const std::vector<ScnNode>& ns, long& i, long& e, long& l) {
            for (auto& x : ns) {
                if (x.k == ScnNode::IF) { i++; if (x.atom2 >= 0) e++; }
                if (x.k == ScnNode::LOOP) l++;
                count(x.a, i, e, l);
                count(x.b, i, e, l);
            }
        };
        for (auto& f : d.files) {
            if (f.cat != "rooms" || f.stream) continue;
            if (argc > 3 && upper(argv[3]) != f.stem) continue;
            FileContent c;
            if (!analyze(d, f, c) || !c.is_room || !c.room.script_size) continue;
            rooms++;
            const Bytes& blk = c.blocks[c.room_block].data;
            const Bytes orig(blk.begin() + c.room.script_off, blk.begin() + c.room.script_off + c.room.script_size);
            ScriptImage im;
            std::string e;
            if (!script_decompose(blk, c.room, im, e)) { bad++; std::cout << f.stem << ": " << e << "\n"; continue; }
            Scenario s = scn_parse(im, im.atoms);
            long i0 = 0, e0 = 0, l0 = 0;
            for (auto& g : s.regions) count(g.nodes, i0, e0, l0);
            n_if += i0; n_else += e0; n_loop += l0;
            for (auto& a : im.atoms)
                if (a.kind == ScriptAtom::OP && !a.b.empty()) {
                    n_cmd++;
                    if (a.b[0] == 0x0C || a.b[0] == 0x0E || a.b[0] == 0x0B || a.b[0] == 0x17) n_raw++;
                }
            n_raw -= i0 + e0;   // jumps that became blocks
            std::vector<ScriptAtom> flat = im.atoms;
            Bytes sec;
            if (!scn_flatten(s, flat, e) || !script_assemble(im, flat, sec, e) || sec != orig) {
                bad++;
                size_t k = 0;
                while (k < sec.size() && k < orig.size() && sec[k] == orig[k]) k++;
                std::cout << f.stem << ": tree round trip differs at +" << strfmt("%X", (unsigned)k) << " " << e << "\n";
                continue;
            }
            // a new empty command (0x0D) at the end of every block: same number of blocks after flatten + parse, assembles
            std::vector<ScriptAtom> mut = im.atoms;
            Scenario s2 = s;
            int nid = script_next_nid(mut);
            std::function<void(std::vector<ScnNode>&)> add = [&](std::vector<ScnNode>& ns) {
                for (auto& x : ns) {
                    if (x.k == ScnNode::IF || x.k == ScnNode::LOOP) {
                        for (auto* blkv : {&x.a, &x.b}) {
                            if (blkv == &x.b && x.atom2 < 0) continue;
                            if (x.k == ScnNode::LOOP && blkv == &x.b) continue;
                            ScriptAtom nop;
                            nop.kind = ScriptAtom::OP;
                            nop.b = {0x0D, 0, 0, 0};
                            nop.nid = nid++;
                            mut.push_back(nop);
                            ScnNode nn;
                            nn.atom = (int)mut.size() - 1;
                            blkv->push_back(nn);
                        }
                    }
                    add(x.a);
                    add(x.b);
                }
            };
            for (auto& g : s2.regions) add(g.nodes);
            Bytes sec2;
            if (!scn_flatten(s2, mut, e) || !script_assemble(im, mut, sec2, e)) { bad++; std::cout << f.stem << ": blocks with a new command: " << e << "\n"; continue; }
            Scenario s3 = scn_parse(im, mut);
            long i3 = 0, e3 = 0, l3 = 0;
            for (auto& g : s3.regions) count(g.nodes, i3, e3, l3);
            if (argc > 4) {   // scenariotest <image> NAME v: the blocks before and after (place of the jump)
                auto at_name = [](const std::vector<ScriptAtom>& v, int k) {
                    return k < 0 ? std::string("?") : v[k].old >= 0 ? strfmt("@%04X", (unsigned)v[k].old) : strfmt("#%d", v[k].nid);
                };
                std::function<void(const std::vector<ScnNode>&, const std::vector<ScriptAtom>&, int)> pr =
                    [&](const std::vector<ScnNode>& ns, const std::vector<ScriptAtom>& v, int ind) {
                        for (auto& x : ns) {
                            if (x.k != ScnNode::IF && x.k != ScnNode::LOOP) continue;
                            std::cout << std::string(ind * 2, ' ') << (x.k == ScnNode::IF ? "IF " : "LOOP ") << at_name(v, x.atom)
                                      << (x.atom2 >= 0 ? " / " + at_name(v, x.atom2) : "") << " (" << x.a.size() << "+" << x.b.size() << ")\n";
                            pr(x.a, v, ind + 1);
                            pr(x.b, v, ind + 1);
                        }
                    };
                std::cout << "-- before\n";
                for (auto& g : s.regions) pr(g.nodes, im.atoms, 0);
                std::cout << "-- after\n";
                for (auto& g : s3.regions) pr(g.nodes, mut, 0);
            }
            // what the game runs is the same: the old commands keep their order, and every jump of an old command lands, through
            // the new empty commands only, on the command it went to before (a jump to the end of nested blocks that end at the
            // same place may leave the inner ones, so the block count alone is no measure)
            {
                std::vector<int64_t> o1, o2;
                for (auto& a : im.atoms) o1.push_back(a.old);
                for (auto& a : mut) if (a.old >= 0) o2.push_back(a.old);
                if (o1 != o2) { bad++; std::cout << f.stem << ": blocks with a new command: the old commands changed their order\n"; continue; }
                std::map<int64_t, int> by_old;
                std::map<int, int> by_nid;
                for (size_t k = 0; k < mut.size(); k++) {
                    if (mut[k].old >= 0 && mut[k].kind != ScriptAtom::DIR) by_old[mut[k].old] = (int)k;
                    if (mut[k].nid >= 0) by_nid[mut[k].nid] = (int)k;
                }
                int wrong = 0;
                for (auto& a : mut) {
                    if (a.old < 0 || a.kind != ScriptAtom::OP || a.b.empty()) continue;
                    const int oi = im.find((uint32_t)a.old);
                    for (auto& sy : a.syms) {
                        if (sy.type == 2) continue;
                        int tn = -1;
                        if (sy.tnew >= 0) { auto it = by_nid.find(sy.tnew); if (it != by_nid.end()) tn = it->second; }
                        else { auto it = by_old.find(sy.target); if (it != by_old.end()) tn = it->second; }
                        while (tn >= 0 && tn < (int)mut.size() && mut[tn].old < 0 && !mut[tn].b.empty() && mut[tn].b[0] == 0x0D) tn++;
                        uint32_t was = 0;
                        for (auto& so : im.atoms[oi].syms) if (so.at == sy.at) was = so.target;
                        if (tn < 0 || tn >= (int)mut.size() || mut[tn].old != (int64_t)was) wrong++;
                    }
                }
                if (wrong) { bad++; std::cout << f.stem << ": blocks with a new command: " << wrong << " jumps land elsewhere\n"; continue; }
            }
            ok++;
        }
        std::cout << strfmt("rooms %d: ok %d, failed %d; commands %ld: IF %ld (with ELSE %ld), LOOP %ld, other jumps %ld\n", rooms, ok, bad, n_cmd,
                            n_if, n_else, n_loop, n_raw);
        if (bad) return 1;
    } else if (cmd == "scriptshift" || cmd == "scripttxt") {
        // scriptshift <образ> <ИМЯ> <out.DAT> [каждая N-я]: проверка перекладчика в игре — после каждой N-й команды пустая
        // 0x0D (всё за ней съезжает); scripttxt <образ> <ИМЯ> <out.txt>: текст скрипта для правки
        if (argc < 5) return usage();
        const DiscFile* f = d.find(upper(argv[3]) + ".DAT");
        FileContent c;
        if (!f || !analyze(d, *f, c) || !c.is_room) { std::cerr << "not a room\n"; return 1; }
        Bytes blk = c.blocks[c.room_block].data;
        ScriptImage im;
        std::string e;
        if (!script_decompose(blk, c.room, im, e)) { std::cerr << e << "\n"; return 1; }
        if (cmd == "scripttxt") { write_text(u8path(argv[4]), script_to_text(im, f->stem)); return 0; }
        const int every = argc > 5 ? atoi(argv[5]) : 5;
        std::vector<ScriptAtom> at;
        int n = 0, ins = 0;
        for (auto& a : im.atoms) {
            at.push_back(a);
            if (a.kind == ScriptAtom::OP && ++n % every == 0 && a.b[0] != 0x04 && a.b[0] != 0x05 && a.b[0] != 0x10 && a.b[0] != 0x11 &&
                a.b[0] != 0x0C && !(a.b[0] == 0x0E && a.b[1] == 0)) {
                ScriptAtom nop;
                nop.kind = ScriptAtom::OP;
                nop.b = {0x0D, 0, 0, 0};
                at.push_back(nop);
                ins++;
            }
        }
        Bytes sec;
        std::vector<std::string> notes;
        if (!script_assemble(im, at, sec, e) || !script_install(blk, 0x80100000, c.room, sec, 0x5D000, notes, e)) { std::cerr << e << "\n"; return 1; }
        Bytes raw, file;
        d.read_file(*f, raw);
        if (!rebuild_container(raw, c.ents, c.blocks[c.room_block].entry, blk, f->nsec * 2048u, file, e)) { std::cerr << e << "\n"; return 1; }
        write_file(u8path(argv[4]), file.data(), file.size());
        std::cout << f->stem << ": inserted " << ins << " empty commands, script " << c.room.script_size << " -> " << sec.size() << " bytes\n";
        for (auto& x : notes) std::cout << "  " << x << "\n";
    } else if (cmd == "roomjson") {
        // roomjson <образ> <файл .DAT> <папка>: данные комнаты из файла (например, собранного в mods) — для сверки
        if (argc < 5) return usage();
        Bytes raw;
        if (!read_file(u8path(argv[3]), raw)) { std::cerr << "cannot read " << argv[3] << "\n"; return 1; }
        const DiscFile* f = d.find(upper(path_u8(u8path(argv[3]).filename())));
        if (!f) { std::cerr << "no disc file of that name\n"; return 1; }
        FileContent c;
        if (!analyze_raw(d, *f, raw, c) || !c.is_room) { std::cerr << "not a room file\n"; return 1; }
        write_room_json(u8path(argv[4]), c);
        int ntri = 0;
        for (auto& m : c.meshes) ntri += (int)m.tris.size();
        std::cout << "room " << f->stem << ": mesh tris " << ntri << ", models " << c.actors.size() << ", texts " << c.room.strings.size()
                  << ", cameras " << c.room.cameras.size() << ", file " << raw.size() << " bytes (room for " << f->nsec * 2048u << ")\n";
    } else if (cmd == "strpng") {
        // strpng <образ> <ИМЯ.STR> <кадр> <out.png>: один кадр ролика — проверка декодера
        if (argc < 6) return usage();
        const DiscFile* f = d.find(upper(argv[3]));
        if (!f) { std::cerr << "no such file\n"; return 1; }
        auto frames = str_scan(d, *f);
        int k = atoi(argv[4]);
        if (k < 0 || k >= (int)frames.size()) { std::cerr << "frames: " << frames.size() << "\n"; return 1; }
        std::vector<uint32_t> px;
        bool ok = str_decode(d, frames[k], px);
        if (!px.empty()) write_png(u8path(argv[5]), frames[k].w, frames[k].h, px.data());
        std::cout << "frames " << frames.size() << ", frame " << frames[k].number << " " << frames[k].w << "x" << frames[k].h
                  << " decoded " << (ok ? "ok" : "FAILED") << "\n";
    } else if (cmd == "vram") {
        // vram <образ> <ИМЯ.DAT> <out.bin>: видеопамять файла 1024x512x16 (как её видит приложение) — сверка с игрой
        if (argc < 5) return usage();
        const DiscFile* f = d.find(upper(argv[3]));
        if (!f) { std::cerr << "no such file\n"; return 1; }
        FileContent c;
        if (!analyze(d, *f, c)) { std::cerr << "cannot read\n"; return 1; }
        FILE* o = fopen(argv[4], "wb");
        if (!o) return 1;
        fwrite(c.vram.px.data(), 2, c.vram.px.size(), o);
        fclose(o);
        std::cout << "uploads " << c.uploads.size() << "\n";
        for (auto& u : c.uploads) std::cout << u.x << "," << u.y << " " << u.w << "x" << u.h << "\n";
    } else if (cmd == "vramuse") {
        // vramuse <image>: every video memory upload of every file of the disc - "<FILE> <cat> <type> x y w h" (06.10.2026: where
        // the game never puts pictures - room for the pictures of an enemy of another room)
        int n = 0;
        for (auto& f : d.files) {
            if (f.stream) continue;
            FileContent c;
            if (!analyze(d, f, c)) continue;
            for (auto& u : c.uploads) {
                std::cout << strfmt("%s %s %d %d %d %d %d\n", f.name.c_str(), f.cat.empty() ? "-" : f.cat.c_str(), u.type, u.x, u.y, u.w, u.h);
                n++;
            }
        }
        std::cout << n << " uploads\n";
    } else if (cmd == "blocks") {
        // blocks <образ> <ИМЯ.DAT> <папка>: распакованные блоки RAM файла — <адрес>.bin (сверка с памятью игры)
        if (argc < 5) return usage();
        const DiscFile* f = d.find(upper(argv[3]));
        if (!f) { std::cerr << "no such file\n"; return 1; }
        FileContent c;
        if (!analyze(d, *f, c)) { std::cerr << "cannot read\n"; return 1; }
        std::error_code ec;
        fs::create_directories(u8path(argv[4]), ec);
        for (auto& b : c.blocks) {
            write_file(u8path(argv[4]) / u8path(strfmt("%08X.bin", b.dest)), b.data.data(), b.data.size());
            std::cout << strfmt("%08X %u", b.dest, (unsigned)b.data.size()) << "\n";
        }
        for (auto& a : c.actors)
            std::cout << "actor " << actor_export_name(c, a) << " block " << strfmt("%08X", a.block_dest) << " verts "
                      << strfmt("+%X", a.model.vert_off) << " n " << a.model.nv_disk << "\n";
    } else if (cmd == "species") {
        // species <image>: enemy kinds of the game for the enemy drop-down (06.10.2026) - the source room, addresses, motions
        Library lib;
        lib.build(d);
        for (auto& s : lib.species)
            std::cout << strfmt("%-6s kind %2d %-15s variant %2d actor %-16s model %08X motions %08X (%d) ai %d hp %d\n", s.f->stem.c_str(),
                                s.kind, s.kind_name.c_str(), s.variant, s.actor.c_str(), s.model, s.motions, s.nmot, s.ai, s.hp);
        std::cout << lib.species.size() << " kinds\n";
    } else if (cmd == "packsize") {
        // packsize <image> <NAME.DAT>: packed blocks - size on the disc, sectors, our re-pack (06.10.2026: room fits)
        if (argc < 4) return usage();
        const DiscFile* f = d.find(upper(argv[3]));
        Bytes raw;
        if (!f || !d.read_file(*f, raw)) { std::cerr << "no such file\n"; return 1; }
        for (auto& e : parse_container(raw)) {
            if (e.type != 7 && e.type != 8) continue;
            Bytes plain;
            if (!lzss_decode(&raw[e.offset], e.size, plain)) { std::cout << "decode failed\n"; continue; }
            Bytes re = lzss_encode(plain.data(), plain.size());
            std::cout << strfmt("type %u dest %08X plain %u disc %u (%u sectors, room %u) ours %u (%+d)\n", e.type, e.dest,
                                (unsigned)plain.size(), e.size, (e.size + 2047u) / 2048u, ((e.size + 2047u) / 2048u) * 2048u,
                                (unsigned)re.size(), (int)re.size() - (int)e.size);
        }
    } else if (cmd == "mp4info") {
        // mp4info <образ> <файл.mp4>: размер кадра, длительность, число кадров (проверка выгрузки)
        if (argc < 4) return usage();
        int w = 0, h = 0, n = 0;
        double sec = 0;
        std::string e;
        bool ok = read_mp4_info(u8path(argv[3]), w, h, sec, n, e);
        std::cout << "mp4 " << (ok ? "ok" : "FAILED " + e) << ": " << w << "x" << h << ", " << sec << " s, frames " << n << "\n";
    } else if (cmd == "selftest") {
        int bad = 0, nblk = 0;
        uint64_t orig_sz = 0, our_sz = 0;
        int rooms = 0, coll_ok = 0, coll_bad = 0, texts = 0, text_bad = 0, models = 0, motions = 0, samples = 0;
        uint32_t max_room = 0;
        for (auto& f : d.files) {
            if (f.cat.empty() || f.stream) continue;
            Bytes raw;
            if (!d.read_file(f, raw)) continue;
            auto ents = parse_container(raw);
            for (auto& e : ents) {
                if (e.type != 7 && e.type != 8) continue;
                Bytes plain, re, back;
                if (!lzss_decode(&raw[e.offset], e.size, plain)) { bad++; std::cout << "decode failed: " << f.name << "\n"; continue; }
                re = lzss_encode(plain.data(), plain.size());
                if (!lzss_decode(re.data(), re.size(), back) || back != plain) { bad++; std::cout << "roundtrip failed: " << f.name << "\n"; }
                nblk++;
                orig_sz += e.size;
                our_sz += re.size();
            }
            if (f.cat == "rooms") {
                FileContent c;
                analyze(d, f, c);
                models += (int)c.actors.size();
                for (auto& s : c.motion_sets) motions += (int)s.motions.size();
                for (auto& b : c.banks) samples += (int)b.vab.samples.size();
                if (!c.is_room) continue;
                rooms++;
                const Bytes& blk = c.blocks[c.room_block].data;
                max_room = std::max<uint32_t>(max_room, (uint32_t)blk.size());
                if (c.room.coll.ok) {
                    Bytes sec = build_collision(c.room.coll);
                    bool same = sec.size() == c.room.coll.size && memcmp(sec.data(), &blk[c.room.coll.off], sec.size()) == 0;
                    if (same) coll_ok++;
                    else {
                        coll_bad++;
                        if (coll_bad <= 5) std::cout << "collision rebuild differs: " << f.name << " (" << sec.size() << " vs " << c.room.coll.size << ")\n";
                    }
                }
                for (auto& s : c.room.strings) {
                    std::vector<uint16_t> codes;
                    std::string e;
                    texts++;
                    if (!encode_text(s.text, codes, e) || codes != s.codes) {
                        text_bad++;
                        if (text_bad <= 5) std::cout << "text roundtrip differs: " << f.name << ": " << s.text.substr(0, 60) << " (" << e << ")\n";
                    }
                }
            } else {
                FileContent c;
                analyze(d, f, c);
                models += (int)c.actors.size();
                for (auto& s : c.motion_sets) motions += (int)s.motions.size();
                for (auto& b : c.banks) samples += (int)b.vab.samples.size();
            }
        }
        std::cout << strfmt("LZSS: %d blocks, roundtrip failures %d, game %llu bytes, ours %llu bytes (%.1f%%)\n", nblk, bad,
                            (unsigned long long)orig_sz, (unsigned long long)our_sz, orig_sz ? 100.0 * our_sz / orig_sz : 0.0);
        std::cout << strfmt("rooms %d (largest block %u bytes), collision byte-exact %d, differs %d\n", rooms, max_room, coll_ok, coll_bad);
        std::cout << strfmt("texts %d, roundtrip differs %d\n", texts, text_bad);
        std::cout << strfmt("models %d, motions %d, sound samples %d\n", models, motions, samples);
        if (bad || coll_bad || text_bad) return 1;
    } else {
        return usage();
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "done in " << ms << " ms\n";
    return 0;
}
