// Звуковой драйвер Dino Crisis для мелодий (разбор 05.10.2026, re/sound.md):
//  * секвенция: 8 Б {00 01, u16 R = 48, u32 темп мкс/четверть}, события «статус, данные, [delta 1 Б, если бит 7 статуса]»;
//    тип = биты 4–6: 0 note off, 1 note on (vel 0 — off), 2 пауза, 3 CC (7 громкость, 10 панорама, 99/6 петля), 4 программа,
//    6 bend, 7 мета (51 — темп 3 Б BE, 2F — конец);
//  * таймер 445,642 Гц; в слоте M = T·1000/R; за тик cnt −= 2 280 000, пока cnt ≤ 0 — события; событие с delta d: cnt = d·M,
//    без delta — cnt = −1; нота звучит со следующего тика;
//  * высота (8005F80C), громкость и панорама (8005EDDC, 8005FA9C) — формулы драйвера бит в бит; ADSR тона — в регистры SPU как
//    есть (огибающая - по описанию SPU в psx-spx); голосов 16, выбор — свободный, иначе самый старый.
#include "synth.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dc {

namespace {

const int32_t kUp[12] = {0x10000, 0x10F39, 0x11F5A, 0x13070, 0x1428A, 0x155B9, 0x16A0A, 0x17F91, 0x19660, 0x1AE8A, 0x1C824, 0x1E343};
const int32_t kDn[12] = {0x10000, 0xF1A2, 0xE412, 0xD745, 0xCB30, 0xBFC9, 0xB505, 0xAADC, 0xA145, 0x9835, 0x8FAD, 0x879C};
const double kTickHz = 33868800.0 / 8.0 / 9500.0;

int32_t note_p(int n) {
    if (n >= 0) {
        uint32_t lo = (uint32_t)(0x1000u << (n / 12)) * (uint32_t)kUp[n % 12];
        return (int32_t)lo >> 16;
    }
    int a = -n;
    return (int32_t)(((int64_t)(0x1000 >> std::min(a / 12, 12)) * kDn[a % 12]) >> 16);
}

int voice_pitch(int note, int shift, int center, int bend, int pbmin, int pbmax) {
    int B = 0;
    if (bend > 0x2000) B = (bend - 0x2000) * (pbmax << 7) / 8191;
    else if (bend < 0x2000) B = -((0x2000 - bend) * (pbmin << 7) / 8191);
    int k = ((note << 7) | (shift & 0x7F)) + B;
    int semi = ((k & 0x3F80) >> 7) - center, fine = k & 0x7F;
    int p0 = note_p(semi), p1 = note_p(semi + 1);
    int d = (p1 - p0) * fine;
    if (d < 0) d += 127;
    return std::clamp(p0 + (d >> 7), 0, 0x3FFF);
}

// ---- огибающая ADSR: по описанию SPU в psx-spx (Martin Korth, «SPU ADSR»): поля регистра, затем каждый шаг
//      cycles = 1 << max(0, shift - 11), step = шаг << max(0, 11 - shift); экспонента вверх выше 0x6000 - ждать вчетверо
//      дольше, экспонента вниз - шаг * уровень / 0x8000 (вниз); уровень 0..0x7FFF; такт - 44,1 кГц (одна выборка)
enum { ATTACK, DECAY, SUSTAIN, RELEASE };

struct EnvPhase { bool up, expo; int shift, step; };

// ADSR1 (младшие 16 бит): 15 атака эксп., 14-10 сдвиг атаки, 9-8 шаг атаки (+7..+4), 7-4 сдвиг спада (шаг -8, эксп.),
// 3-0 уровень поддержки ((N+1)*0x800); ADSR2: 31 поддержка эксп., 30 вниз, 28-24 сдвиг, 23-22 шаг (+7..+4 / -8..-5),
// 21 отпускание эксп., 20-16 сдвиг отпускания (шаг -8)
EnvPhase env_phase(uint32_t adsr, int phase) {
    switch (phase) {
    case ATTACK:  return { true, ((adsr >> 15) & 1u) != 0, (int)((adsr >> 10) & 31u), 7 - (int)((adsr >> 8) & 3u) };
    case DECAY:   return { false, true, (int)((adsr >> 4) & 15u), -8 };
    case SUSTAIN: {
        const bool down = ((adsr >> 30) & 1u) != 0;
        const int k = (int)((adsr >> 22) & 3u);
        return { !down, ((adsr >> 31) & 1u) != 0, (int)((adsr >> 24) & 31u), down ? -8 + k : 7 - k };
    }
    default:      return { false, ((adsr >> 21) & 1u) != 0, (int)((adsr >> 16) & 31u), -8 };
    }
}

struct Env {
    uint16_t level = 0;
    uint32_t wait = 0;      // выборок до следующего шага
    int phase = ATTACK;
    void step(uint32_t adsr) {
        const int sustain_level = ((int)(adsr & 15u) + 1) << 11;
        if (phase == ATTACK && level >= 0x7FFF) phase = DECAY;
        if (phase == DECAY && level <= sustain_level) phase = SUSTAIN;
        if (wait > 0) { wait--; return; }
        const EnvPhase p = env_phase(adsr, phase);
        int cycles = 1 << std::max(0, p.shift - 11);
        int delta = p.step * (1 << std::max(0, 11 - p.shift));
        if (p.expo && p.up && level > 0x6000) cycles *= 4;
        if (p.expo && !p.up) delta = (int)std::floor((double)delta * (double)level / 32768.0);   // вниз с округлением вниз:
                                                                                          // шаг не меньше -1, огибающая доходит до 0
        level = (uint16_t)std::clamp((int)level + delta, 0, 0x7FFF);
        wait = (uint32_t)(cycles - 1);
    }
};

struct Voice {
    bool on = false, pending = false, held = false, ended = false;
    int ch = 0, prog = 0, tone = 0, note = 0, age = 0;
    const VabTone* t = nullptr;
    const VabSample* s = nullptr;
    double pos = 0;
    int pitch = 0x1000;
    int l0 = 0, r0 = 0, l = 0, r = 0;   // громкость на key-on и текущая (прямой режим SPU, 0..0x3FFF)
    uint32_t adsr = 0;
    Env env;
};

struct Chan { int prog = 0, progvol = 127, progpan = 64, cc7 = 127, cc10 = 64, bend = 0x2000; };

// длина события (с delta); 0 — конец/ошибка
int event_len(const uint8_t* d, size_t n, size_t pc, bool* end) {
    *end = false;
    if (pc >= n) return 0;
    uint8_t st = d[pc];
    int type = (st >> 4) & 7, len = 1;
    switch (type) {
    case 0: case 1: case 3: case 6: len += 2; break;
    case 4: len += 1; break;
    case 7:
        if (pc + 1 >= n) return 0;
        if (d[pc + 1] == 0x51) len += 4;
        else if (d[pc + 1] == 0x2F) { len += 1; *end = true; }
        else len += 1;
        break;
    default: break;
    }
    if (st & 0x80) len += 1;
    return pc + (size_t)len <= n ? len : 0;
}

}  // namespace

uint64_t song_key(const uint8_t* d, size_t n) {
    size_t pc = 8, end = n;
    for (int guard = 0; guard < 1000000 && pc < n; guard++) {
        bool e = false;
        int len = event_len(d, n, pc, &e);
        if (!len) break;
        pc += (size_t)len;
        if (e) { end = pc; break; }
    }
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < end && i < n; i++) { h ^= d[i]; h *= 0x100000001B3ull; }
    return h;
}

int song_bank(const FileContent& c, int si) {
    if (si < 0 || si >= (int)c.seqs.size()) return -1;
    int entry = c.seqs[si].entry, best = -1;
    for (size_t b = 0; b < c.banks.size(); b++)
        if (c.banks[b].hdr_entry < entry && (best < 0 || c.banks[b].hdr_entry > c.banks[best].hdr_entry)) best = (int)b;
    if (best < 0 && !c.banks.empty()) best = 0;
    return best;
}

bool render_song(const FileContent& c, int si, SongPcm& out, int loops) {
    out = SongPcm();
    int bi = song_bank(c, si);
    if (bi < 0) return false;
    const Vab& v = c.banks[bi].vab;
    const Bytes& sd = c.seqs[si].data;
    if (sd.size() < 11) return false;
    const uint8_t* d = sd.data();
    const size_t n = sd.size();
    const int R = std::max(1, (int)le16(d + 2));
    int64_t M = (int64_t)le32(d + 4) * 1000 / R;
    std::map<uint16_t, const VabSample*> by_addr;
    for (auto& s : v.samples) by_addr[s.addr] = &s;
    Chan ch[16];
    Voice vo[16];
    size_t pc = 8, loop_pc = 0;
    int64_t cnt = 0;
    int nrpn = 0;
    bool loop_set = false, loop_end_pending = false, stopped = false;
    int loop_hits = 0;
    const double spt = 44100.0 / kTickHz;   // кадров на тик
    double tick_acc = 0;
    std::vector<int16_t>& pcm = out.pcm;
    const size_t max_frames = 44100u * 60u * 12u;
    const size_t extra = 44100u * 3u;   // второй проход после конца тела: его начало с хвостами нот — в начало петли
    size_t frame = 0, tail = 0, loop_end_frame = 0;
    (void)loops;
    auto start_loop_mark = [&]() {
        loop_hits++;
        if (loop_hits == 1) out.loop_start = (int)frame;
        if (loop_hits == 2) { out.loop_end = (int)frame; loop_end_frame = frame; }
    };
    auto note_on = [&](int c_, int note, int vel) {
        Chan& C = ch[c_];
        int pv = C.cc7 * C.progvol / 127;
        pv = pv * vel / 127;
        if (C.prog < 0 || C.prog >= v.nprog || C.prog >= (int)v.tones.size()) return;
        int ntones = C.prog < 16 ? v.progs[C.prog][0] : 16;
        for (int ti = 0; ti < ntones && ti < 16; ti++) {
            const VabTone& T = v.tones[C.prog][ti];
            if (note < T.min || note > T.max) continue;
            auto it = by_addr.find(T.addr);
            if (it == by_addr.end()) continue;
            int pan = (C.progpan == 64 && C.cc10 == 64) ? T.pan : (((C.progpan + T.pan) >> 1) + C.cc10) >> 1;
            int vv = pv * T.vol / 127;
            int L = vv, Rr = vv;
            if (pan < 64) Rr = vv * pan >> 6;
            if (pan > 64) L = vv * (127 - pan) >> 6;
            int L2 = (int)(((int64_t)L * L * 133135) >> 17), R2 = (int)(((int64_t)Rr * Rr * 133135) >> 17);
            if (!L2 && !R2) continue;
            // голос: свободный (молчит), иначе самый старый
            int best = -1;
            for (int k = 0; k < 16 && best < 0; k++) if (!vo[k].on && !vo[k].pending) best = k;
            if (best < 0)
                for (int k = 0; k < 16; k++)
                    if (!vo[k].pending && (best < 0 || vo[k].age > vo[best].age)) best = k;
            if (best < 0) continue;
            Voice& V = vo[best];
            V = Voice();
            V.pending = true;
            V.held = true;
            V.ch = c_; V.prog = C.prog; V.tone = ti; V.note = note;
            V.t = &T;
            V.s = it->second;
            V.pitch = voice_pitch(note, T.shift, T.center, C.bend, T.raw[12], T.raw[13]);
            V.l0 = V.l = std::min(L2, 0x3FFF);
            V.r0 = V.r = std::min(R2, 0x3FFF);
            V.adsr = (uint32_t)T.adsr1 | ((uint32_t)T.adsr2 << 16);
        }
    };
    auto note_off = [&](int c_, int note) {
        Chan& C = ch[c_];
        for (auto& V : vo)
            if ((V.on || V.pending) && V.held && V.prog == C.prog && V.note == note) {
                V.held = false;
                if (V.pending) { V.pending = false; V.on = false; }
                else { V.env.phase = RELEASE; V.env.wait = 0; }
            }
    };
    auto exec_events = [&]() {
        cnt -= 2280000;
        int guard = 0;
        while (cnt <= 0 && !stopped && guard++ < 100000) {
            if (pc >= n) { stopped = true; break; }
            size_t at = pc;
            if (loop_set && at == loop_pc) start_loop_mark();
            if (stopped) break;
            uint8_t st = d[pc++];
            int type = (st >> 4) & 7, c_ = st & 15;
            bool at_end = false;
            switch (type) {
            case 0: { int nt = d[pc]; pc += 2; note_off(c_, nt); break; }
            case 1: { int nt = d[pc], vel = d[pc + 1]; pc += 2; if (vel) note_on(c_, nt, vel); else note_off(c_, nt); break; }
            case 3: {
                int cc = d[pc], val = d[pc + 1];
                pc += 2;
                if (cc == 7) {
                    ch[c_].cc7 = val;
                    for (auto& V : vo) if (V.on && V.ch == c_) { V.l = V.l0 * val / 127; V.r = V.r0 * val / 127; }
                } else if (cc == 10) ch[c_].cc10 = val;
                else if (cc == 99) {
                    nrpn = val;
                    if (val == 30) loop_end_pending = true;
                } else if (cc == 6 && nrpn == 20) {
                    nrpn = 0;
                    loop_pc = pc + ((st & 0x80) ? 1 : 0);   // за событием и его delta
                    loop_set = true;
                }
                break;
            }
            case 4: {
                int p = d[pc++];
                ch[c_].prog = p;
                if (p < 16) {
                    ch[c_].progvol = v.mvol * v.progs[p][1] / 127;
                    ch[c_].progpan = (v.progs[p][4] + v.pan + 0xC0) & 0xFF;
                }
                break;
            }
            case 6: {
                int b = d[pc] + d[pc + 1] * 128;
                pc += 2;
                ch[c_].bend = b;
                for (auto& V : vo)
                    if ((V.on || V.pending) && V.ch == c_ && V.t) V.pitch = voice_pitch(V.note, V.t->shift, V.t->center, b, V.t->raw[12], V.t->raw[13]);
                break;
            }
            case 7: {
                int m = d[pc++];
                if (m == 0x51) { int64_t T = ((int64_t)d[pc] << 16) | (d[pc + 1] << 8) | d[pc + 2]; pc += 3; M = T * 1000 / R; }
                else if (m == 0x2F) { pc++; at_end = true; }
                else pc++;
                break;
            }
            default: break;
            }
            int delta = -1;
            if (st & 0x80) delta = d[pc++];
            if (at_end) {
                if (loop_set) { pc = loop_pc; loop_end_pending = false; cnt = -1; continue; }   // конец с петлёй — сразу в начало
                stopped = true;
                break;
            }
            cnt = delta >= 0 ? (int64_t)delta * M : -1;
            if (loop_end_pending && delta > 0 && loop_set) { pc = loop_pc; loop_end_pending = false; }
            (void)at;
        }
    };
    bool seq_done = false;
    while (frame < max_frames) {
        tick_acc += 1.0;
        if (tick_acc >= spt) {
            tick_acc -= spt;
            // 1) ноты прошлого тика начинают звучать
            for (auto& V : vo)
                if (V.pending) { V.pending = false; V.on = true; V.pos = 0; V.env = Env(); V.ended = false; }
            // 2) события
            if (!seq_done) {
                exec_events();
                if (stopped) seq_done = true;
            }
            for (auto& V : vo) if (V.on) V.age++;
        }
        int32_t L = 0, Rs = 0;
        bool any = false;
        for (auto& V : vo) {
            if (!V.on) continue;
            any = true;
            int32_t smp = 0;
            const std::vector<int16_t>& p = V.s->pcm;
            if (!V.ended && !p.empty()) {
                size_t i = (size_t)V.pos;
                double f = V.pos - (double)i;
                auto at = [&](size_t k) -> int32_t {
                    if (k < p.size()) return p[k];
                    if (V.s->loop_start >= 0 && p.size() > (size_t)V.s->loop_start)
                        return p[V.s->loop_start + (k - p.size()) % (p.size() - V.s->loop_start)];
                    return 0;
                };
                int32_t y0 = at(i ? i - 1 : 0), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
                double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
                smp = (int32_t)lround(((c3 * f + c2) * f + c1) * f + y1);
                smp = std::clamp(smp, -32768, 32767);
                V.pos += V.pitch / 4096.0;
                if (V.pos >= (double)p.size()) {
                    if (V.s->loop_start >= 0 && p.size() > (size_t)V.s->loop_start)
                        V.pos = V.s->loop_start + fmod(V.pos - (double)p.size(), (double)(p.size() - V.s->loop_start));
                    else V.ended = true;
                }
            }
            int32_t sh = (smp * (int32_t)V.env.level) >> 15;
            L += (sh * V.l) >> 14;
            Rs += (sh * V.r) >> 14;
            V.env.step(V.adsr);
            if (V.env.phase == RELEASE && V.env.level == 0) V.on = false;
        }
        pcm.push_back((int16_t)std::clamp(L, -32768, 32767));
        pcm.push_back((int16_t)std::clamp(Rs, -32768, 32767));
        frame++;
        if (loop_end_frame && frame >= loop_end_frame + extra) break;   // петля: тело и 3 с второго прохода
        if (seq_done && !loop_end_frame) {
            if (!any || ++tail > 44100u * 4u) break;   // одноразовая: пока звучат хвосты (не дольше 4 с)
        }
    }
    out.loops = out.loop_start >= 0 && out.loop_end > out.loop_start;
    if (out.loops) {
        // бесшовная петля: начало тела — из второго прохода (там звучат хвосты нот конца тела); файл — вступление и одно тело
        const size_t ls = (size_t)out.loop_start, le = (size_t)out.loop_end;
        const size_t k = std::min(extra, std::min(le - ls, frame - le));
        for (size_t i = 0; i < k * 2; i++) pcm[ls * 2 + i] = pcm[le * 2 + i];
        pcm.resize(le * 2);
    } else {
        out.loop_start = out.loop_end = -1;
    }
    return !pcm.empty();
}

}  // namespace dc
