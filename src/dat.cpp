// Контейнер .DAT игры, LZSS (func_8002B138 игры), видеопамять 1024x512.
//
// Контейнер: заголовочный сектор 0x800, записи по 16 байт {u32 type, size, dest, param}; данные — с 0x800, каждый блок с
// границы 2048. Типы: 0 — как есть в RAM (dest), 1 — как есть в VRAM, 2 — палитры в VRAM (строками), 3/4 — звуковой банк
// (заголовок 'Gian' / тело ADPCM), 5 — секвенция, 7 — LZSS в RAM, 8 — LZSS в VRAM. Типы 1 и 8 лежат плитками 32x32
// полуслова (одна плитка — один сектор 2048), строками — только 2.
#include "core.h"

#include <algorithm>
#include <cstring>

namespace dc {

std::vector<DatEntry> parse_container(const Bytes& data) {
    std::vector<DatEntry> ents;
    struct Raw { uint32_t t, sz, dest, param; };
    std::vector<Raw> raw;
    size_t lim = std::min<size_t>(0x800, data.size());
    for (size_t off = 0; off + 16 <= lim; off += 16) {
        Raw r{le32(&data[off]), le32(&data[off + 4]), le32(&data[off + 8]), le32(&data[off + 12])};
        if (r.t > 20 || r.sz == 0 || r.sz > data.size()) break;
        raw.push_back(r);
    }
    uint32_t pos = 0x800;
    for (auto& r : raw) {
        if ((size_t)pos + r.sz > data.size()) break;
        DatEntry e;
        e.type = r.t; e.size = r.sz; e.dest = r.dest; e.param = r.param; e.offset = pos;
        ents.push_back(e);
        pos += r.sz;
        pos = (pos + 2047u) & ~2047u;
    }
    return ents;
}

bool lzss_decode(const uint8_t* src, size_t n, Bytes& out) {
    out.clear();
    out.reserve(n * 2);
    size_t pos = 0;
    unsigned ctrl = 0, bits = 0;
    while (pos < n) {
        if (bits == 0) { ctrl = src[pos++]; bits = 8; continue; }
        unsigned bit = ctrl & 1;
        ctrl >>= 1;
        bits--;
        if (bit) {
            out.push_back(src[pos++]);
        } else {
            if (pos + 1 >= n) break;
            unsigned lo = src[pos], hi = src[pos + 1];
            pos += 2;
            size_t off = ((hi & 0x0F) << 8) | lo;
            size_t ln = (hi >> 4) + 2;
            if (off == 0 || off > out.size()) return false;
            for (size_t k = 0; k < ln; k++) out.push_back(out[out.size() - off]);
        }
    }
    return true;
}

// Оптимальный разбор: цена в битах (литерал 9, ссылка 17), самая длинная ссылка на каждой позиции по цепочкам хешей
// 2 байт; кодирует тот же формат, что распаковывает игра.
// Не короче min_size: часть ссылок заменяется литералами (распаковывается в то же самое). Нужна, чтобы блок занимал
// столько же секторов, сколько на диске: загрузчик игры иначе блок не принимает (05.10.2026: титул с правленой картинкой
// сжался на 2 сектора короче — экран остался чёрным, блок в RAM нулями; тот же титул, пережатый в прежние сектора, — цел).
Bytes lzss_encode_min(const uint8_t* src, size_t n, size_t min_size) {
    Bytes best = lzss_encode(src, n);
    if (best.size() >= min_size) return best;
    // разобрать поток на токены
    struct Tok { bool lit; uint16_t off; uint8_t len; size_t at; };
    std::vector<Tok> t;
    {
        size_t pos = 0, out = 0;
        unsigned ctrl = 0, bits = 0;
        while (pos < best.size()) {
            if (bits == 0) { ctrl = best[pos++]; bits = 8; continue; }
            unsigned bit = ctrl & 1;
            ctrl >>= 1;
            bits--;
            if (bit) { t.push_back({true, 0, 1, out}); pos++; out++; }
            else {
                if (pos + 1 >= best.size()) break;
                unsigned lo = best[pos], hi = best[pos + 1];
                pos += 2;
                uint8_t len = (uint8_t)((hi >> 4) + 2);
                t.push_back({false, (uint16_t)(((hi & 0x0F) << 8) | lo), len, out});
                out += len;
            }
        }
    }
    size_t data = 0;
    for (auto& k : t) data += k.lit ? 1 : 2;
    auto total = [&](size_t ntok, size_t d) { return d + (ntok + 7) / 8; };
    size_t ntok = t.size();
    // с конца: ссылку → литералы, пока не дотянем до min_size
    std::vector<bool> expand(t.size(), false);
    for (size_t i = t.size(); i-- > 0 && total(ntok, data) < min_size;) {
        if (t[i].lit) continue;
        expand[i] = true;
        data += t[i].len - 2;
        ntok += t[i].len - 1;
    }
    Bytes out;
    out.reserve(total(ntok, data));
    size_t cpos = 0;
    int b = 8;
    uint8_t ctrl = 0;
    auto put_bit = [&](bool lit) {
        if (b == 8) { cpos = out.size(); out.push_back(0); ctrl = 0; b = 0; }
        if (lit) ctrl |= (uint8_t)(1u << b);
        b++;
        out[cpos] = ctrl;
    };
    for (size_t i = 0; i < t.size(); i++) {
        const Tok& k = t[i];
        if (k.lit) { put_bit(true); out.push_back(src[k.at]); }
        else if (expand[i]) { for (int j = 0; j < k.len; j++) { put_bit(true); out.push_back(src[k.at + j]); } }
        else {
            put_bit(false);
            out.push_back((uint8_t)(k.off & 0xFF));
            out.push_back((uint8_t)(((k.len - 2) << 4) | ((k.off >> 8) & 0x0F)));
        }
    }
    return out;
}

Bytes lzss_encode(const uint8_t* src, size_t n) {
    const int WIN = 4095, MAXL = 17, MINL = 2;
    std::vector<int> best_len(n, 0), best_off(n, 0);
    std::vector<int> head(65536, -1), prev(n, -1);
    for (size_t i = 0; i < n; i++) {
        if (i + 1 < n) {
            unsigned h = src[i] | (src[i + 1] << 8);
            int bl = 0, bo = 0, chain = 0;
            for (int j = head[h]; j >= 0 && (int)i - j <= WIN && chain < 4096; j = prev[j], chain++) {
                int l = 0;
                int lim = (int)std::min<size_t>(MAXL, n - i);
                while (l < lim && src[j + l] == src[i + l]) l++;
                if (l > bl) { bl = l; bo = (int)i - j; if (l == MAXL) break; }
            }
            best_len[i] = bl >= MINL ? bl : 0;
            best_off[i] = bo;
            prev[i] = head[h];
            head[h] = (int)i;
        }
    }
    std::vector<uint32_t> cost(n + 1, 0);
    std::vector<uint8_t> choice(n + 1, 0);
    for (size_t ii = n; ii-- > 0;) {
        uint32_t c = 9 + cost[ii + 1];
        uint8_t ch = 1;
        for (int l = MINL; l <= best_len[ii]; l++) {
            uint32_t cc = 17 + cost[ii + l];
            if (cc < c) { c = cc; ch = (uint8_t)l; }
        }
        cost[ii] = c;
        choice[ii] = ch;
    }
    Bytes out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        size_t cpos = out.size();
        out.push_back(0);
        uint8_t ctrl = 0;
        for (int b = 0; b < 8 && i < n; b++) {
            int l = choice[i];
            if (l <= 1) {
                ctrl |= (uint8_t)(1u << b);
                out.push_back(src[i]);
                i++;
            } else {
                int off = best_off[i];
                out.push_back((uint8_t)(off & 0xFF));
                out.push_back((uint8_t)(((l - 2) << 4) | ((off >> 8) & 0x0F)));
                i += (size_t)l;
            }
        }
        out[cpos] = ctrl;
    }
    return out;
}

std::vector<RamBlock> ram_blocks(const Bytes& file, const std::vector<DatEntry>& ents) {
    std::vector<RamBlock> out;
    for (size_t k = 0; k < ents.size(); k++) {
        const DatEntry& e = ents[k];
        if (e.type != 7 && e.type != 0) continue;
        RamBlock b;
        b.dest = e.dest;
        b.type = (int)e.type;
        b.entry = (int)k;
        if (e.type == 7) {
            if (!lzss_decode(&file[e.offset], e.size, b.data)) continue;
        } else {
            b.data.assign(file.begin() + e.offset, file.begin() + e.offset + e.size);
        }
        out.push_back(std::move(b));
    }
    return out;
}

// ---------------------------------------------------------------- VRAM
uint32_t c15_rgba(uint16_t c) {
    uint32_t r = (c & 31u) << 3, g = ((c >> 5) & 31u) << 3, b = ((c >> 10) & 31u) << 3;
    r |= r >> 5; g |= g >> 5; b |= b >> 5;
    uint32_t a = c == 0 ? 0u : 255u;
    return r | (g << 8) | (b << 16) | (a << 24);
}

void Vram::blit(const uint8_t* d, size_t n, int x, int y, int w, int h) {
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy >= 512) break;
        for (int col = 0; col < w; col++) {
            size_t s = ((size_t)row * w + col) * 2;
            if (s + 1 >= n) return;
            int dx = x + col;
            if (dx < 1024) px[(size_t)dy * 1024 + dx] = le16(d + s);
        }
    }
}

void Vram::blit_tiled(const uint8_t* d, size_t n, int x, int y, int w, int h) {
    size_t src = 0;
    for (int ty = 0; ty < h && src < n; ty += 32) {
        int th = std::min(32, h - ty);
        for (int tx = 0; tx < w && src < n; tx += 32) {
            for (int row = 0; row < th; row++) {
                int dy = y + ty + row;
                for (int col = 0; col < 32; col++) {
                    size_t s = src + ((size_t)row * 32 + col) * 2;
                    if (s + 1 >= n) break;
                    int dx = x + tx + col;
                    if (tx + col < w && dy < 512 && dx < 1024) px[(size_t)dy * 1024 + dx] = le16(d + s);
                }
            }
            src += (size_t)32 * th * 2;
        }
    }
}

void Vram::apply(const Bytes& file, const std::vector<DatEntry>& ents, std::vector<Upload>* log) {
    Bytes tmp;
    for (size_t k = 0; k < ents.size(); k++) {
        const DatEntry& e = ents[k];
        if (e.type == 8) {
            if (!lzss_decode(&file[e.offset], e.size, tmp)) continue;
            blit_tiled(tmp.data(), tmp.size(), e.x(), e.y(), e.w(), e.h());
        } else if (e.type == 1) {
            blit_tiled(&file[e.offset], e.size, e.x(), e.y(), e.w(), e.h());
        } else if (e.type == 2) {
            blit(&file[e.offset], e.size, e.x(), e.y(), e.w(), e.h());
        } else {
            continue;
        }
        if (log) log->push_back(Upload{(int)k, (int)e.type, e.x(), e.y(), e.w(), e.h()});
    }
}

void Vram::decode_rect(uint16_t tp, int cx, int cy, int u0, int v0, int w, int h, std::vector<uint32_t>& out) const {
    out.assign((size_t)w * h, 0);
    const int tpx = (tp & 15) * 64, tpy = ((tp >> 4) & 1) * 256, mode = (tp >> 7) & 3;
    uint32_t pal[256];
    if (mode < 2)
        for (int i = 0; i < 256; i++) pal[i] = c15_rgba(at(cx + i, cy));
    for (int v = 0; v < h; v++) {
        int yy = tpy + v0 + v;
        for (int u = 0; u < w; u++) {
            int uu = u0 + u;
            uint32_t c;
            if (mode == 0) {
                uint16_t word = at(tpx + (uu >> 2), yy);
                c = pal[(word >> ((uu & 3) * 4)) & 15];
            } else if (mode == 1) {
                uint16_t word = at(tpx + (uu >> 1), yy);
                c = pal[(word >> ((uu & 1) * 8)) & 255];
            } else {
                c = c15_rgba(at(tpx + uu, yy));
            }
            out[(size_t)v * w + u] = c;
        }
    }
}

void Vram::decode_raw15(int x, int y, int w, int h, std::vector<uint32_t>& out) const {
    out.assign((size_t)w * h, 0);
    for (int v = 0; v < h; v++)
        for (int u = 0; u < w; u++) out[(size_t)v * w + u] = c15_rgba(at(x + u, y + v)) | 0xFF000000u;
}

}  // namespace dc
