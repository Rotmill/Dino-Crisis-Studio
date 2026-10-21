// Движения: glTF (как выгружено приложением — узел root, кости по именам, анимации <набор>_<NN>) → кадры игры.
#pragma once
#include "content.h"
#include "gltf_read.h"

namespace dc {

struct MotionResult {
    int changed = 0, same = 0, relocated = 0;
    std::vector<std::string> notes, errors;
};

// Анимация an glTF персонажа каталога (characters\<id>\<id>.gltf) — в движение motion набора set_index файла c (одно из мест
// движения каталога). Кости — по модели a (имена выгрузки). blocks — индекс в c.blocks → данные (копия, правится на месте).
// Длина по времени та же — кадры на своих местах (не изменённые — байт в байт, без записи); иначе движение пересобирается
// целиком (ключи по 1/30 или 1/15 с, события — по доле времени): на прежнее место, если влезает; движение врага в комнате —
// в конец блока комнаты (allow_relocate), иначе те же кадры реже.
void apply_motion(const GDoc& d, const std::string& doc_name, const ActorPart& a, const GAnim& an, const FileContent& c, int set_index,
                  int motion, std::map<int, Bytes>& blocks, MotionResult& res, bool allow_relocate = true);

// имя модели в выгрузке (как пишет Exporter): raptor, regina, regina_P10
std::string actor_export_name(const FileContent& c, const ActorPart& a);
std::string motion_anim_name(const MotionSet& ms, int index);
// имена костей узлов glTF
std::string bone_node_name(const ActorPart& a, int bone);

}  // namespace dc
