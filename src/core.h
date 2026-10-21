// Dino Crisis ME Studio — ядро: образ диска, контейнеры игры, видеопамять, меши, персонажи, звук, тексты.
// Всё читается из образа диска игрока (Track 1 .bin или .cue); ничего из игры в приложении нет.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace dc {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------- utils
inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline int16_t s16(const uint8_t* p) { return (int16_t)le16(p); }
inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

fs::path u8path(const std::string& s);
std::string path_u8(const fs::path& p);
bool read_file(const fs::path& p, Bytes& out);
bool write_file(const fs::path& p, const void* data, size_t n);
bool write_text(const fs::path& p, const std::string& s);
std::string sha1_hex(const void* data, size_t n);
std::string strfmt(const char* fmt, ...);
std::string upper(std::string s);

// ---------------------------------------------------------------- disc
// Категории — как подпапки mods (dc_mods.c игры): rooms, doors, characters, items, screens, sounds\effects,
// sounds\stage, sounds\speech, videos, other. Пустая — файл не из моддинга (код игры /PSX/BIN, EXE).
struct DiscFile {
    std::string path;      // "/PSX/DATA/ST100.DAT"
    std::string dir;       // "/PSX/DATA/"
    std::string name;      // "ST100.DAT"
    std::string stem;      // "ST100"
    std::string cat;       // "rooms", "sounds\\speech" ...
    uint32_t lba = 0, size = 0, nsec = 0;
    bool stream = false;   // XA / STR — сектора целиком
};

class Disc {
public:
    ~Disc();
    bool open(const fs::path& path, std::string& err);
    void close();
    bool is_open() const { return fp_ != nullptr; }
    // сырой сектор 2352 (для образа 2048 — синтез заголовка Mode2 Form1 не делаем: потоки требуют 2352)
    bool read_raw(uint32_t lba, uint8_t out[2352]);
    // данные пользователя Form 1 (2048)
    bool read_user(uint32_t lba, uint8_t out[2048]);
    bool read_file(const DiscFile& f, Bytes& out);
    const DiscFile* find(const std::string& name) const;       // по имени без регистра: "ST100.DAT"
    const DiscFile* find_path(const std::string& path) const;  // "/PSX/DATA/ST100.DAT"
    std::vector<DiscFile> files;
    fs::path image_path;
    bool raw2352 = true;
    bool dino_crisis = false;   // нашли SLUS_009.22 и /PSX/DATA/ST100.DAT
    std::string exe_name;       // из SYSTEM.CNF
private:
    void walk(uint32_t lba, uint32_t size, const std::string& prefix, int depth);
    FILE* fp_ = nullptr;
    uint64_t fsize_ = 0;
    std::mutex mu_;
};

const char* category_of(const std::string& dir, const std::string& name, bool* stream);
extern const char* const kModCategories[];   // все подпапки mods (как у установщика), nullptr в конце

// ---------------------------------------------------------------- container (.DAT) и LZSS
struct DatEntry {
    uint32_t type = 0, size = 0, dest = 0, param = 0, offset = 0;
    int x() const { return (int)(dest & 0xFFFF); }
    int y() const { return (int)(dest >> 16); }
    int w() const { return (int)(param & 0xFFFF); }
    int h() const { return (int)(param >> 16); }
};
std::vector<DatEntry> parse_container(const Bytes& data);
bool lzss_decode(const uint8_t* src, size_t n, Bytes& out);
Bytes lzss_encode(const uint8_t* src, size_t n);   // оптимальный разбор, формат игры
Bytes lzss_encode_min(const uint8_t* src, size_t n, size_t min_size);   // не короче min_size (литералами)
// распакованные блоки (тип 7 — LZSS, тип 0 — как есть) по адресу назначения
struct RamBlock { uint32_t dest = 0; int type = 0; int entry = 0; Bytes data; };
std::vector<RamBlock> ram_blocks(const Bytes& file, const std::vector<DatEntry>& ents);

// ---------------------------------------------------------------- VRAM
struct Upload { int entry = 0, type = 0, x = 0, y = 0, w = 0, h = 0; };
struct Vram {
    std::vector<uint16_t> px;
    Vram() : px(1024 * 512, 0) {}
    void blit(const uint8_t* d, size_t n, int x, int y, int w, int h);
    void blit_tiled(const uint8_t* d, size_t n, int x, int y, int w, int h);
    // типы 1 (как есть, плитками 32x32), 2 (палитры, строками), 8 (LZSS, плитками)
    void apply(const Bytes& file, const std::vector<DatEntry>& ents, std::vector<Upload>* log = nullptr);
    uint16_t at(int x, int y) const { return (x >= 0 && x < 1024 && y >= 0 && y < 512) ? px[(size_t)y * 1024 + x] : 0; }
    // RGBA (байты R,G,B,A в памяти) — правило export_textures.py: 0x0000 прозрачен, бит STP не учитывается
    void decode_rect(uint16_t tpage, int cx, int cy, int u0, int v0, int w, int h, std::vector<uint32_t>& out) const;
    void decode_raw15(int x, int y, int w, int h, std::vector<uint32_t>& out) const;
};
uint32_t c15_rgba(uint16_t c);
inline int clut_x(uint16_t clut) { return (clut & 0x3F) * 16; }
inline int clut_y(uint16_t clut) { return (clut >> 6) & 0x1FF; }

// ---------------------------------------------------------------- статические меши (комнаты, двери, оружие)
struct MVert { int16_t x = 0, y = 0, z = 0; uint8_t u = 0, v = 0; };
struct MTri {
    MVert v[3];
    uint16_t tpage = 0, clut = 0;
    uint32_t rgb[3] = {0x808080, 0x808080, 0x808080};
    uint8_t code = 0;     // команда GPU (0x34/0x3C …; бит 1 — полупрозрачный)
    uint16_t group = 0;
    uint32_t rec = 0;     // запись полигона в блоке (смещение): треугольник 40 Б, квад 52 Б
    uint8_t part = 0;     // 0 — треугольник, 1 — квад (0,1,2), 2 — квад (1,3,2)
};
struct MeshGroup { uint32_t hdr = 0, tri_off = 0, quad_off = 0; uint16_t nt = 0, nq = 0; };
// комната: таблица из 9 указателей, группа 0 на +0x24/+0x28
std::vector<MeshGroup> room_groups(const Bytes& blk, uint32_t base);
// цепочка групп с начала блока (двери, оружие, проп в CORE)
std::vector<MeshGroup> chain_groups(const Bytes& blk, uint32_t base, uint32_t start = 0);
void collect_tris(const Bytes& blk, const std::vector<MeshGroup>& groups, std::vector<MTri>& out);
uint32_t groups_end(const std::vector<MeshGroup>& g);   // смещение конца последней группы

// ---------------------------------------------------------------- скелетные модели и движения
struct Bone {
    int16_t ox = 0, oy = 0, oz = 0;
    int parent = -1;
    uint8_t flags = 0;
    uint32_t tri_off = 0, quad_off = 0;
    uint16_t nt = 0, nq = 0;
};
struct APrim {
    uint16_t vi[3]; uint16_t uv[3]; uint16_t tpage, clut; int bone;
    uint32_t rec = 0;    // запись в блоке: треугольник 16 Б, квад 20 Б
    uint8_t part = 0;    // 0 — нет записи (зеркальная кисть), 1 — треугольник, 2 — квад (0,1,2), 3 — квад (1,3,2)
};
struct Model {
    uint32_t hdr = 0, end = 0;         // смещения в блоке
    std::vector<Bone> bones;
    std::vector<std::array<int16_t, 3>> verts, norms;
    std::vector<APrim> prims;
    std::vector<int> vbone;            // кость каждой вершины (жёсткий скин; на шве — старшая кость)
    int n_tri = 0, n_quad = 0;
    uint32_t vert_off = 0, norm_off = 0;   // массивы вершин и нормалей в блоке (по 8 Б)
    int nv_disk = 0;                   // вершин в файле (дальше — достроенные, без записи)
};
std::vector<uint32_t> scan_models(const Bytes& blk, uint32_t base);
bool parse_model(const Bytes& blk, uint32_t base, uint32_t hdr, Model& m, bool mirror_hand = false);

struct MotionKey { uint8_t hold, frame, ev_flags, ev_id; };
struct Motion {
    uint32_t hdr = 0, data = 0;        // смещения в блоке
    std::vector<MotionKey> keys;
    int nframes = 0, ticks = 0, nbones = 0, fsize = 0;
};
int motion_frame_size(int nbones);
// подряд лежащие движения, начиная с start; nbones — для всех (0 — подобрать по следующему заголовку)
std::vector<Motion> walk_motions(const Bytes& blk, uint32_t base, uint32_t start, int nbones);
// все движения блока (поиск заголовков по сигнатуре), с определением числа костей
std::vector<Motion> scan_motions(const Bytes& blk, uint32_t base);
struct FramePose { int16_t tx, ty, tz, rx, ry, rz; std::vector<uint16_t> ang; };
FramePose read_frame(const Bytes& blk, uint32_t off, int nbones);

// матрицы 3x3 (строки), PS1: X вправо, Y вниз, Z вперёд; M = Rx·Ry·Rz (libgte RotMatrix)
struct M3 { float m[9]; };
M3 euler_xyz(int ax, int ay, int az);
M3 mul(const M3& a, const M3& b);
void mulv(const M3& a, const float v[3], float out[3]);
struct BoneXf { M3 r; float t[3]; };
// поза: кость-корень берёт перенос кадра (tx,ty,tz) и ry (рывок актёра); rx/rz не применяются (REGINA_NOTES)
void pose_bones(const Model& m, const FramePose& f, std::vector<BoneXf>& out, bool root_motion = true);
void rest_bones(const Model& m, std::vector<BoneXf>& out);

// ---------------------------------------------------------------- комната: секции блока
struct FloorRec { uint8_t type, flags; uint16_t pad; int16_t x, z, sx, sz, h[4]; };
struct WallRec { uint8_t type, flag, lvl_lo, lvl_hi; int16_t x, z, sx, sz; uint16_t attr; int16_t spare; };
struct CollRow { uint8_t na = 0, nb = 0; uint16_t pad = 0; uint32_t offa = 0, offb = 0, spare = 0; };
struct Collision {
    bool ok = false;
    uint32_t off = 0, size = 0;   // смещение секции в блоке и её размер
    CollRow rows[17];
    std::vector<std::vector<FloorRec>> floors;   // по строкам (17)
    std::vector<std::vector<WallRec>> walls;     // по строкам (17)
};
struct CameraRec { uint8_t raw[68]; };
struct TriggerRec { uint8_t raw[20]; };
struct GameString { uint32_t sub = 0, off = 0; std::vector<uint16_t> codes; std::string text; };
struct RoomInfo {
    bool ok = false;
    uint32_t ptr[9] = {};
    std::vector<MeshGroup> groups;
    uint32_t mesh_end = 0;
    Collision coll;
    std::vector<CameraRec> cameras;   uint32_t cam_off = 0;
    std::vector<TriggerRec> triggers; uint32_t trig_off = 0;
    uint32_t script_off = 0, script_size = 0;
    std::vector<uint32_t> script_dir;    // смещения подпрограмм от начала секции
    std::vector<GameString> strings;
};
bool parse_room(const Bytes& blk, uint32_t base, RoomInfo& r);
bool parse_collision(const Bytes& blk, uint32_t base, uint32_t off, uint32_t limit, Collision& c);
Bytes build_collision(const Collision& c);   // обратно в байты секции (тот же порядок)

// ---------------------------------------------------------------- тексты игры
std::string decode_text(const uint16_t* codes, size_t n);
bool encode_text(const std::string& s, std::vector<uint16_t>& out, std::string& err);
std::vector<GameString> scan_strings(const Bytes& blk, uint32_t from, uint32_t to);

// ---------------------------------------------------------------- звук
struct VabTone { uint8_t prior, mode, vol, pan, center, shift, min, max; uint16_t adsr1, adsr2, prog, addr; uint8_t raw[24]; };
// center/shift/min — у ПЕРВОГО тона (по программам и тонам), который берёт этот сэмпл: по нему частота выгрузки
struct VabSample { uint32_t off = 0, len = 0; uint16_t addr = 0; int loop_start = -1; std::vector<int16_t> pcm; int center = 60, shift = 0, min = 60; };
struct Vab {
    bool ok = false;
    int nprog = 0, ntone = 0, nvag = 0, mvol = 0, pan = 0;
    uint32_t dest = 0;                     // адрес SPU тела
    std::vector<std::array<uint8_t, 8>> progs;   // 16 записей по 8 байт
    std::vector<std::vector<VabTone>> tones;     // по программам, 16 тонов
    std::vector<VabSample> samples;              // уникальные по адресу, в порядке адреса
};
bool parse_vab(const uint8_t* hdr, size_t hn, const uint8_t* body, size_t bn, uint32_t dest, Vab& v);
void adpcm_decode(const uint8_t* src, size_t n, std::vector<int16_t>& out, int* loop_start = nullptr);
// эффект звучит на ноте min тона: 44100·2^((min + shift/128 − center)/12) (разбор драйвера 05.10, сверено по регистрам SPU);
// инструмент музыки — 44.1 кГц на ноте center − shift/128
int sample_rate_for(const VabSample& s, bool sfx);
// root_note >= 0 — чанк smpl (корневая нота, её доля и петля) для сэмплеров; loop_end < 0 — до конца
bool write_wav(const fs::path& p, const int16_t* pcm, size_t frames, int channels, int rate, int root_note = -1, int loop_start = -1,
               uint32_t pitch_fraction = 0, int loop_end = -1);

struct XaChannel { int file = 0, chan = 0, rate = 0, channels = 1; std::vector<uint32_t> sectors; };
std::vector<XaChannel> xa_scan(Disc& disc, const DiscFile& f);
void xa_decode(Disc& disc, const XaChannel& ch, std::vector<int16_t>& out);

// ---------------------------------------------------------------- ролики STR (MDEC, версия 2)
struct StrFrame { uint32_t number = 0, size = 0, first_sector = 0; int w = 0, h = 0; std::vector<uint32_t> sectors; };
std::vector<StrFrame> str_scan(Disc& disc, const DiscFile& f);   // кадры по номеру; сектора кусков по порядку
bool str_decode(Disc& disc, const StrFrame& fr, std::vector<uint32_t>& rgba);
constexpr double kStrFps = 30.0;   // все ролики диска: кадр на 5 секторов при 150 секторах в секунду

}  // namespace dc
