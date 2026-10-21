// Звук: банки 'Gian' (тип 3 — заголовок, тип 4 — тело SPU-ADPCM), WAV, CD-XA (речь и музыка STAGE*.XAS).
//
// Заголовок 'Gian' (разведка 10.08.2026): +0x04 u8 0xFF; +0x06 u16 программ; +0x08 u16 тонов; +0x0A u16 сэмплов;
// +0x0C u8 громкость; +0x0D u8 панорама; +0x10 — 16 программ по 8 Б {u8 тонов, u8 громкость, …}; +0x90 — по программе
// 16 тонов по 24 Б {u8 prior, mode, vol, pan, center, shift, min, max; 8 Б vib/por/pb; u16 adsr1, adsr2, prog,
// адрес SPU >> 3}. Размер заголовка = 0x90 + программ*384. Сэмпл начинается по адресу тона*8 − адрес тела.
#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

namespace dc {

static const int kF0[5] = {0, 60, 115, 98, 122};
static const int kF1[5] = {0, 0, -52, -55, -60};

static inline int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

void adpcm_decode(const uint8_t* src, size_t n, std::vector<int16_t>& out, int* loop_start) {
    out.clear();
    int s1 = 0, s2 = 0, ls = -1;
    if (loop_start) *loop_start = -1;
    for (size_t b = 0; b + 16 <= n; b += 16) {
        int shift = src[b] & 0x0F, filter = (src[b] >> 4) & 0x07, flags = src[b + 1];
        if (filter > 4) filter = 4;
        if (shift > 12) shift = 9;
        if (flags & 4) ls = (int)out.size();
        // петля — только если блок конца велит повторять (флаги 3); «начало петли» ставят почти всем сэмплам
        if ((flags & 3) == 3 && loop_start) *loop_start = ls < 0 ? 0 : ls;
        for (int i = 0; i < 28; i++) {
            int nib = (src[b + 2 + i / 2] >> ((i & 1) * 4)) & 0x0F;
            int s = (int16_t)(nib << 12) >> shift;
            s += (s1 * kF0[filter] + s2 * kF1[filter] + 32) >> 6;
            int16_t v = clamp16(s);
            out.push_back(v);
            s2 = s1;
            s1 = v;
        }
        if (flags & 1) break;
    }
}

// Частота, на которой сэмпл звучит, как в игре. Эффект: драйвер ставит ноту (min << 8) | shift при ноте сэмпла center << 8
// (func_8005CBF8 → _spu_note2pitch), т.е. 44100·2^((min + shift/128 − center)/12); сверено 05.10 по регистрам SPU в сейвах —
// 22 голоса эффектов из 22 (прежнее правило «нота 60» не совпало ни разу; у тона 12 банка min = 60, отсюда и совпадение
// с PC-звуком ST1 №8). Выше +24 полутонов драйвер упирается в 0x3FFF. Инструмент музыки — 44.1 кГц.
int sample_rate_for(const VabSample& s, bool sfx) {
    if (!sfx || s.center < 24 || s.center > 120) return 44100;
    double semis = s.min + s.shift / 128.0 - s.center;
    if (semis > 24) semis = 24;
    return (int)(44100.0 * pow(2.0, semis / 12.0) + 0.5);
}

bool parse_vab(const uint8_t* h, size_t hn, const uint8_t* body, size_t bn, uint32_t dest, Vab& v) {
    v = Vab();
    if (hn < 0x90 || memcmp(h, "Gian", 4) != 0) return false;
    v.nprog = le16(h + 6);
    v.ntone = le16(h + 8);
    v.nvag = le16(h + 10);
    v.mvol = h[12];
    v.pan = h[13];
    v.dest = dest;
    if (v.nprog < 0 || v.nprog > 128 || 0x90u + (size_t)v.nprog * 384u > hn) return false;
    for (int i = 0; i < 16; i++) {
        std::array<uint8_t, 8> p;
        memcpy(p.data(), h + 0x10 + i * 8, 8);
        v.progs.push_back(p);
    }
    std::set<uint16_t> addrs;
    std::map<uint16_t, const uint8_t*> first;   // первый тон с этим сэмплом
    for (int p = 0; p < v.nprog; p++) {
        std::vector<VabTone> tl;
        for (int t = 0; t < 16; t++) {
            const uint8_t* q = h + 0x90 + p * 384 + t * 24;
            VabTone tn;
            memcpy(tn.raw, q, 24);
            tn.prior = q[0]; tn.mode = q[1]; tn.vol = q[2]; tn.pan = q[3]; tn.center = q[4]; tn.shift = q[5];
            tn.min = q[6]; tn.max = q[7];
            tn.adsr1 = le16(q + 16); tn.adsr2 = le16(q + 18); tn.prog = le16(q + 20); tn.addr = le16(q + 22);
            tl.push_back(tn);
            int ntones = p < 16 ? v.progs[p][0] : 16;
            if (t < ntones && tn.addr) {
                addrs.insert(tn.addr);
                if (!first.count(tn.addr)) first[tn.addr] = q;
            }
        }
        v.tones.push_back(tl);
    }
    for (uint16_t a : addrs) {
        int64_t off = (int64_t)a * 8 - (int64_t)dest;
        if (off < 0 || (size_t)off >= bn) continue;
        VabSample s;
        s.addr = a;
        s.off = (uint32_t)off;
        adpcm_decode(body + off, bn - (size_t)off, s.pcm, &s.loop_start);
        s.len = (uint32_t)((s.pcm.size() / 28) * 16);
        const uint8_t* q = first[a];
        s.center = q[4]; s.shift = q[5]; s.min = q[6];
        v.samples.push_back(std::move(s));
    }
    v.ok = true;
    return true;
}

bool write_wav(const fs::path& p, const int16_t* pcm, size_t frames, int ch, int rate, int root, int loop, uint32_t fraction, int loop_end) {
    uint32_t data = (uint32_t)(frames * ch * 2);
    const uint32_t smpl = root >= 0 ? (loop >= 0 ? 36 + 24 : 36) : 0;   // тело чанка smpl
    Bytes b(44 + data + (smpl ? 8 + smpl : 0));
    memcpy(&b[0], "RIFF", 4); put32(&b[4], (uint32_t)b.size() - 8); memcpy(&b[8], "WAVE", 4);
    memcpy(&b[12], "fmt ", 4); put32(&b[16], 16); put16(&b[20], 1); put16(&b[22], (uint16_t)ch);
    put32(&b[24], (uint32_t)rate); put32(&b[28], (uint32_t)(rate * ch * 2)); put16(&b[32], (uint16_t)(ch * 2)); put16(&b[34], 16);
    memcpy(&b[36], "data", 4); put32(&b[40], data);
    if (data) memcpy(&b[44], pcm, data);
    if (smpl) {
        uint8_t* s = &b[44 + data];
        memcpy(s, "smpl", 4);
        put32(s + 4, smpl);
        put32(s + 8 + 8, (uint32_t)(1000000000.0 / rate));   // период сэмпла, нс
        put32(s + 8 + 12, (uint32_t)root);                    // корневая нота MIDI
        put32(s + 8 + 16, fraction);                          // её доля (тонкая подстройка тона)
        put32(s + 8 + 28, loop >= 0 ? 1u : 0u);               // число петель
        if (loop >= 0) {
            uint8_t* l = s + 8 + 36;
            put32(l + 8, (uint32_t)loop);
            put32(l + 12, (uint32_t)(loop_end >= 0 ? loop_end : (frames ? frames - 1 : 0)));
        }
    }
    return write_file(p, b.data(), b.size());
}

// ---------------------------------------------------------------- CD-XA
std::vector<XaChannel> xa_scan(Disc& disc, const DiscFile& f) {
    std::map<int, XaChannel> by;
    uint8_t s[2352];
    for (uint32_t i = 0; i < f.nsec; i++) {
        if (!disc.read_raw(f.lba + i, s)) break;
        uint8_t file = s[16], chan = s[17], sub = s[18], coding = s[19];
        if (!(sub & 0x04)) continue;              // не аудио
        int key = file * 256 + chan;
        XaChannel& c = by[key];
        if (c.sectors.empty()) {
            c.file = file; c.chan = chan;
            c.channels = (coding & 1) ? 2 : 1;
            c.rate = ((coding >> 2) & 3) == 1 ? 18900 : 37800;
        }
        c.sectors.push_back(f.lba + i);
    }
    std::vector<XaChannel> out;
    for (auto& kv : by) out.push_back(kv.second);
    return out;
}

void xa_decode(Disc& disc, const XaChannel& ch, std::vector<int16_t>& out) {
    out.clear();
    int h[2][2] = {{0, 0}, {0, 0}};
    uint8_t s[2352];
    const bool st = ch.channels == 2;
    for (uint32_t lba : ch.sectors) {
        if (!disc.read_raw(lba, s)) break;
        const uint8_t* d = s + 24;
        for (int g = 0; g < 18; g++) {
            const uint8_t* grp = d + g * 128;
            int16_t unit[8][28];
            for (int u = 0; u < 8; u++) {
                int param = grp[4 + u];
                int shift = param & 0x0F, filter = (param >> 4) & 3;
                if (shift > 12) shift = 9;
                int c = st ? (u & 1) : 0;
                for (int j = 0; j < 28; j++) {
                    int byte = grp[16 + 4 * j + (u >> 1)];
                    int nib = (u & 1) ? (byte >> 4) : (byte & 0x0F);
                    int v = (int16_t)(nib << 12) >> shift;
                    v += (h[c][0] * kF0[filter] + h[c][1] * kF1[filter] + 32) >> 6;
                    int16_t o = clamp16(v);
                    h[c][1] = h[c][0];
                    h[c][0] = o;
                    unit[u][j] = o;
                }
            }
            if (!st) {
                for (int u = 0; u < 8; u++)
                    for (int j = 0; j < 28; j++) out.push_back(unit[u][j]);
            } else {
                for (int u = 0; u < 8; u += 2)
                    for (int j = 0; j < 28; j++) { out.push_back(unit[u][j]); out.push_back(unit[u + 1][j]); }
            }
        }
    }
}

}  // namespace dc
