// Изменён ли меш? glTF игрока против оригинала, каким его пишет выгрузка (export.cpp). В список подмен игры (index.txt)
// идёт только изменённое: подменённая модель рисуется нашим путём (dc_meshrep.cpp) — у Регины без своего меша пропали бы
// живая челюсть и лицо, у комнаты — ничего бы не изменилось, но зря. Сравнение — по треугольникам в координатах игры
// (мм) с допуском на округление редактора (Blender пересохраняет числа с ошибкой 1e-7, переставляет вершины и примитивы).
#pragma once
#include "content.h"
#include "gltf_read.h"

namespace dc {

struct SigTri {
    float p[3][3];      // координаты игры: мм, Y вниз, Z вперёд
    float uv[3][2];
    float col[3][3];    // 0..1
    int bone[3];        // кость игры (−1 — без кости)
    std::string img;    // имя картинки без папки и расширения, в нижнем регистре ("" — без картинки)
};

struct SigMesh {
    std::vector<SigTri> tris;
    bool has_col = false, has_bone = false;
};

// оригинал: неподвижный меш (group — номер группы в MeshPart::hdrs, −1 — весь), модель со скелетом (поза покоя)
void sig_static(const MeshPart& m, const Vram& v, int group, SigMesh& out);
void sig_actor(const ActorPart& a, const Vram& v, SigMesh& out);
// glTF игрока: node — узел (с его поддеревом), "" — все меши файла; actor — вершины по скину в позу покоя
bool sig_gltf(const GDoc& d, const std::string& node, bool actor, SigMesh& out, std::string& err);
// то же содержимое (допуск: 0.6 мм, UV 0.001, цвет 6/255; порядок треугольников и вершин — любой)
bool sig_same(const SigMesh& a, const SigMesh& b);
// имя кости узла glTF → кость игры (spine… у Регины, bone_NN у остальных; хвост Blender «.001» не мешает), −1 — не кость
int bone_of_node_name(std::string s);
// узлы glTF с мешами, которые не лежат ни под одним из узлов names (новая геометрия вне групп — игра её не возьмёт)
std::vector<std::string> nodes_outside(const GDoc& d, const std::vector<std::string>& names);

}  // namespace dc
