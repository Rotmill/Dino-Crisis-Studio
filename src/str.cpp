// Ролики STR (MOVIE\*.STR): видео — битовый поток MDEC версии 2 (кадры 320x160, титры 160x96, 30 кадров в секунду),
// звук — CD-XA того же файла.
//
// Сектор видео: 32 Б заголовка {u16 0x0160, u16 0x8001, u16 кусок, u16 кусков в кадре, u32 номер кадра, u32 байт кадра,
// u16 ширина, u16 высота, …} + 2016 Б данных кадра. Кадр: 8 Б заголовка {u16 число кодов MDEC / 2, u16 0x3800,
// u16 шаг квантования, u16 версия 2}, дальше биты: слова по 16 бит (младший байт первым), внутри слова — со старшего бита.
// Макроблок 16x16 = Cr, Cb, Y0..Y3 по 8x8; макроблоки идут сверху вниз, затем слева направо. Блок: DC — 10 бит со
// знаком, затем коэффициенты по таблице AC MPEG-1 (B.14), «10» — конец блока, «000001» + 6 бит серии + 10 бит уровня —
// выход. Обратное квантование как у MDEC (psx-spx): DC·q[0], AC = (уровень·q[k]·шаг + 4) / 8; таблица — стандартная
// матрица MPEG-1 с q[0] = 2.
#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dc {

namespace {

struct Vlc { const char* code; int run, level; };
// MPEG-1, таблица B.14 (без кода конца блока и выхода); за кодом — бит знака
const Vlc kVlc[] = {
    {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"00101", 0, 3}, {"00111", 3, 1}, {"00110", 4, 1},
    {"000110", 1, 2}, {"000111", 5, 1}, {"000101", 6, 1}, {"000100", 7, 1}, {"0000110", 0, 4}, {"0000100", 2, 2},
    {"0000111", 8, 1}, {"0000101", 9, 1}, {"00100110", 0, 5}, {"00100001", 0, 6}, {"00100101", 1, 3},
    {"00100100", 3, 2}, {"00100111", 10, 1}, {"00100011", 11, 1}, {"00100010", 12, 1}, {"00100000", 13, 1},
    {"0000001010", 0, 7}, {"0000001100", 1, 4}, {"0000001011", 2, 3}, {"0000001111", 4, 2}, {"0000001001", 5, 2},
    {"0000001110", 14, 1}, {"0000001101", 15, 1}, {"0000001000", 16, 1}, {"000000011101", 0, 8},
    {"000000011000", 0, 9}, {"000000010011", 0, 10}, {"000000010000", 0, 11}, {"000000011011", 1, 5},
    {"000000010100", 2, 4}, {"000000011100", 3, 3}, {"000000010010", 4, 3}, {"000000011110", 6, 2},
    {"000000010101", 7, 2}, {"000000010001", 8, 2}, {"000000011111", 17, 1}, {"000000011010", 18, 1},
    {"000000011001", 19, 1}, {"000000010111", 20, 1}, {"000000010110", 21, 1}, {"0000000011010", 0, 12},
    {"0000000011001", 0, 13}, {"0000000011000", 0, 14}, {"0000000010111", 0, 15}, {"0000000010110", 1, 6},
    {"0000000010101", 1, 7}, {"0000000010100", 2, 5}, {"0000000010011", 3, 4}, {"0000000010010", 5, 3},
    {"0000000010001", 9, 2}, {"0000000010000", 10, 2}, {"0000000011111", 22, 1}, {"0000000011110", 23, 1},
    {"0000000011101", 24, 1}, {"0000000011100", 25, 1}, {"0000000011011", 26, 1}, {"00000000011111", 0, 16},
    {"00000000011110", 0, 17}, {"00000000011101", 0, 18}, {"00000000011100", 0, 19}, {"00000000011011", 0, 20},
    {"00000000011010", 0, 21}, {"00000000011001", 0, 22}, {"00000000011000", 0, 23}, {"00000000010111", 0, 24},
    {"00000000010110", 0, 25}, {"00000000010101", 0, 26}, {"00000000010100", 0, 27}, {"00000000010011", 0, 28},
    {"00000000010010", 0, 29}, {"00000000010001", 0, 30}, {"00000000010000", 0, 31}, {"000000000011000", 0, 32},
    {"000000000010111", 0, 33}, {"000000000010110", 0, 34}, {"000000000010101", 0, 35}, {"000000000010100", 0, 36},
    {"000000000010011", 0, 37}, {"000000000010010", 0, 38}, {"000000000010001", 0, 39}, {"000000000010000", 0, 40},
    {"000000000011111", 1, 8}, {"000000000011110", 1, 9}, {"000000000011101", 1, 10}, {"000000000011100", 1, 11},
    {"000000000011011", 1, 12}, {"000000000011010", 1, 13}, {"000000000011001", 1, 14}, {"0000000000010011", 1, 15},
    {"0000000000010010", 1, 16}, {"0000000000010001", 1, 17}, {"0000000000010000", 1, 18}, {"0000000000010100", 6, 3},
    {"0000000000011010", 11, 2}, {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2}, {"0000000000010111", 14, 2},
    {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2}, {"0000000000011111", 27, 1}, {"0000000000011110", 28, 1},
    {"0000000000011101", 29, 1}, {"0000000000011100", 30, 1}, {"0000000000011011", 31, 1},
};

// дерево кодов: узел {потомок 0, потомок 1}; лист — отрицательный: −(1 + индекс в kVlc); −1000 — конец блока, −1001 — выход
struct Trie {
    std::vector<std::array<int, 2>> n;
    Trie() {
        n.push_back({0, 0});
        for (size_t i = 0; i < sizeof kVlc / sizeof kVlc[0]; i++) add(kVlc[i].code, -(int)(1 + i));
        add("10", -1000);
        add("000001", -1001);
    }
    void add(const char* c, int leaf) {
        int cur = 0;
        for (const char* p = c; *p; p++) {
            int b = *p - '0';
            if (!p[1]) { n[cur][b] = leaf; return; }
            if (n[cur][b] <= 0) {
                n.push_back({0, 0});
                n[cur][b] = (int)n.size() - 1;
            }
            cur = n[cur][b];
        }
    }
};
const Trie& trie() { static Trie t; return t; }

const int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                         41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                         30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const int kQuant[64] = {2,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
                        34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
                        35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

struct Bits {
    const uint8_t* d;
    size_t n, pos = 0;   // в битах
    int get() {
        size_t w = (pos >> 4) * 2;
        if (w + 1 >= n) { pos++; return 0; }
        int v = (le16(d + w) >> (15 - (pos & 15))) & 1;
        pos++;
        return v;
    }
    int getn(int k) { int v = 0; while (k--) v = (v << 1) | get(); return v; }
    bool over() const { return (pos >> 3) > n + 4; }
};

float g_cos[8][8];   // cos((2x+1)uπ/16)·C(u)/2
void init_cos() {
    static bool done = false;
    if (done) return;
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            g_cos[x][u] = (float)(cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0) * (u == 0 ? sqrt(0.5) : 1.0) * 0.5);
    done = true;
}

void idct(const int* blk, float* out) {
    float tmp[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += g_cos[y][v] * blk[v * 8 + u];
            tmp[y * 8 + u] = s;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += g_cos[x][u] * tmp[y * 8 + u];
            out[y * 8 + x] = s;
        }
}

// один блок 8x8: false — поток кончился или сломан
bool read_block(Bits& b, int qscale, float* out) {
    int blk[64] = {};
    int dc = b.getn(10);
    if (dc & 0x200) dc -= 0x400;
    blk[0] = dc * kQuant[0];
    const Trie& t = trie();
    int k = 0;
    for (;;) {
        int node = 0;
        int leaf = 0;
        for (int depth = 0; depth < 17; depth++) {
            int nx = t.n[node][b.get()];
            if (nx < 0) { leaf = nx; break; }
            if (nx == 0) return false;
            node = nx;
        }
        if (leaf == 0) return false;
        if (leaf == -1000) break;
        int run, level;
        if (leaf == -1001) {
            run = b.getn(6);
            level = b.getn(10);
            if (level & 0x200) level -= 0x400;
        } else {
            const Vlc& v = kVlc[-leaf - 1];
            run = v.run;
            level = b.get() ? -v.level : v.level;
        }
        k += run + 1;
        if (k > 63) return false;
        int p = kZigzag[k];
        int val = (level * kQuant[p] * qscale + 4) / 8;
        blk[p] = std::clamp(val, -1024, 1023);
        if (b.over()) return false;
    }
    idct(blk, out);
    return true;
}

}  // namespace

std::vector<StrFrame> str_scan(Disc& disc, const DiscFile& f) {
    std::vector<StrFrame> out;
    std::map<uint32_t, size_t> by;
    uint8_t s[2352];
    for (uint32_t i = 0; i < f.nsec; i++) {
        if (!disc.read_raw(f.lba + i, s)) break;
        if (s[18] & 0x04) continue;   // звук
        const uint8_t* u = s + 24;
        if (le16(u) != 0x0160) continue;
        uint32_t num = le32(u + 8);
        int chunk = le16(u + 4), nchunk = le16(u + 6);
        auto it = by.find(num);
        if (it == by.end()) {
            StrFrame fr;
            fr.number = num;
            fr.size = le32(u + 12);
            fr.w = le16(u + 16);
            fr.h = le16(u + 18);
            fr.sectors.assign(std::max(1, nchunk), 0);
            fr.first_sector = i;
            it = by.emplace(num, out.size()).first;
            out.push_back(fr);
        }
        StrFrame& fr = out[it->second];
        if (chunk >= 0 && chunk < (int)fr.sectors.size()) fr.sectors[chunk] = f.lba + i;
    }
    std::sort(out.begin(), out.end(), [](const StrFrame& a, const StrFrame& b) { return a.number < b.number; });
    return out;
}

bool str_decode(Disc& disc, const StrFrame& fr, std::vector<uint32_t>& rgba) {
    init_cos();
    if (fr.w <= 0 || fr.h <= 0 || fr.w > 640 || fr.h > 480) return false;
    Bytes data;
    data.reserve(fr.sectors.size() * 2016);
    uint8_t s[2352];
    for (uint32_t lba : fr.sectors) {
        if (!lba || !disc.read_raw(lba, s)) return false;
        data.insert(data.end(), s + 24 + 32, s + 24 + 32 + 2016);
    }
    if (data.size() < 8 || le16(&data[2]) != 0x3800) return false;
    int qscale = le16(&data[4]), ver = le16(&data[6]);
    if (ver != 2) return false;
    Bits b{data.data() + 8, data.size() - 8};
    const int mbw = (fr.w + 15) / 16, mbh = (fr.h + 15) / 16;
    rgba.assign((size_t)fr.w * fr.h, 0xFF000000u);
    float cr[64], cb[64], y[4][64];
    for (int mx = 0; mx < mbw; mx++)
        for (int my = 0; my < mbh; my++) {
            if (!read_block(b, qscale, cr) || !read_block(b, qscale, cb)) return mx > 0;
            for (int k = 0; k < 4; k++)
                if (!read_block(b, qscale, y[k])) return mx > 0;
            for (int py = 0; py < 16; py++)
                for (int px = 0; px < 16; px++) {
                    int X = mx * 16 + px, Y = my * 16 + py;
                    if (X >= fr.w || Y >= fr.h) continue;
                    int ci = (py >> 1) * 8 + (px >> 1);
                    float CR = cr[ci], CB = cb[ci];
                    float l = y[(py >> 3) * 2 + (px >> 3)][(py & 7) * 8 + (px & 7)];
                    int R = (int)lroundf(l + 1.402f * CR) + 128;
                    int G = (int)lroundf(l - 0.3437f * CB - 0.7143f * CR) + 128;
                    int B = (int)lroundf(l + 1.772f * CB) + 128;
                    R = std::clamp(R, 0, 255); G = std::clamp(G, 0, 255); B = std::clamp(B, 0, 255);
                    rgba[(size_t)Y * fr.w + X] = 0xFF000000u | ((uint32_t)B << 16) | ((uint32_t)G << 8) | (uint32_t)R;
                }
        }
    return true;
}

}  // namespace dc
