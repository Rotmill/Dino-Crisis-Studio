// Скрипт комнаты Dino Crisis: ВМ func_80055758, таблица обработчиков 0x8009E370 (до 128 опкодов). Длины — как в разборе
// 17.09.2026 (skipq/scrdis.py): таблица по опкоду, 0x5A = 4 + 8·n, 0x28 — по подкоду (+2) через таблицу EXE 0x80019310.
#include "script.h"

#include <cstring>

namespace dc {

static ExeImage g_exe;

bool load_exe(Disc& d) {
    g_exe = ExeImage();
    const DiscFile* f = d.exe_name.empty() ? nullptr : d.find(upper(d.exe_name));
    if (!f) return false;
    Bytes b;
    if (!d.read_file(*f, b) || b.size() < 0x800 || memcmp(b.data(), "PS-X EXE", 8) != 0) return false;
    g_exe.addr = le32(&b[0x18]);
    g_exe.data = std::move(b);
    return true;
}
const ExeImage& exe() { return g_exe; }

// длины 0x00..0x6F — сверены с прибавкой к PC в каждом обработчике (разбор 05.10, re/plc/oplen.py): все 120 комнат
// обходятся без неизвестных опкодов; 0x70 и выше в скриптах нет
static const uint8_t kOpSize[112] = {
    1,  4, 4, 4, 4,  4,  4,  4, 4, 8,  4, 4,  4, 4,  4,  4,  4, 4, 4, 4, 4,  8, 4,  4, 4, 1,  1, 1, 1, 1,  1,  1,
    24, 8, 4, 32, 8, 4,  4,  4, 32, 8, 8, 8,  4, 4,  20, 4,  4, 4, 4, 4, 4,  8, 8,  8, 4, 4,  12, 4, 8, 12, 4,  8,
    12, 4, 20, 8, 8, 8,  8,  4, 4, 4,  4, 12, 32, 8, 8,  8,  8, 8, 4, 12, 4, 12, 28, 8, 8, 20, 4, 44, 4, 4,  20, 12,
    4,  4, 8, 8, 4,  4,  4,  8, 8, 4,  8, 4,  4, 4,  4,  4};

int script_op_size(const uint8_t* pc, size_t avail) {
    if (!avail) return 0;
    int op = pc[0];
    if (op == 0x5A) return avail >= 4 ? 4 + 8 * pc[3] : 0;
    if (op == 0x28) {   // зона: длина по типу (байт +2), таблица 0x80019310
        if (avail < 3 || pc[2] >= 12) return 0;
        switch (pc[2]) {
        case 0: return 48;
        case 1: return 40;
        case 3: return 36;
        case 4: return 44;
        case 7: return 52;
        default: return 32;
        }
    }
    return op < 112 ? kOpSize[op] : 0;
}

std::vector<ScriptOp> script_listing(const Bytes& blk, uint32_t so, uint32_t a, uint32_t b) {
    std::vector<ScriptOp> out;
    uint32_t pc = a;
    while (pc < b && so + pc < blk.size()) {
        const uint8_t* p = &blk[so + pc];
        size_t avail = std::min<size_t>(b - pc, blk.size() - (so + pc));
        int sz = script_op_size(p, avail);
        ScriptOp o;
        o.off = pc;
        o.op = p[0];
        o.size = sz;
        auto hex = [&](int n) {
            std::string s;
            for (int i = 0; i < n && i < (int)avail; i++) s += strfmt("%02X ", p[i]);
            return s;
        };
        auto s16v = [&](int k) { return (int)(int16_t)(p[k] | (p[k + 1] << 8)); };
        switch (p[0]) {
        case 0x04: o.text = "RETURN"; break;
        case 0x10: o.text = "POP_RETURN"; break;   // g1: no end-of-task check (crash at the top level)
        case 0x0F: o.text = strfmt("CALL %d", s16v(2)); break;
        case 0x01: o.text = strfmt("FORK %d", s16v(2)); break;
        case 0x05: o.text = strfmt("RESTART %d", s16v(2)); break;
        case 0x0C: o.text = strfmt("JMP %04X", pc + s16v(2)); break;
        case 0x0E: o.text = strfmt("BRANCH %d %04X", (int8_t)p[1], pc + s16v(2)); break;
        case 0x26: o.text = strfmt("TESTFLAG %d/%d", p[1], p[2]); break;
        case 0x25: o.text = strfmt("SETFLAG %d/%d = %d", p[1], p[2], p[3]); break;
        case 0x2F: o.text = strfmt("DESTROY %d %d", p[1], p[2]); break;
        case 0x48: o.text = strfmt("SHOW %d %d = %d", p[1], p[2], p[3]); break;
        case 0x22: o.text = strfmt("SELECT %d %d", p[1], p[2]); break;   // категория +1, номер +2 (разбор 05.10)
        case 0x11: o.text = "HANG"; break;   // g1: never moves on, the game freezes
        case 0x20:
            if (avail >= 16) o.text = strfmt("ACTOR slot %d kind %d hp %d at %d %d %d turn %d", p[1], p[2], le16(p + 6), s16v(8), s16v(10), s16v(12), s16v(14));
            break;
        case 0x59: o.text = strfmt("ROSTER_ACTOR slot %d kind %d room %02X", p[1], p[2], avail > 16 ? p[16] : 0); break;
        case 0x5B: if (avail >= 12) o.text = strfmt("FOLLOWER slot %d kind %d at %d %d %d", p[1], p[2], s16v(4), s16v(6), s16v(8)); break;
        case 0x28: {
            static const char* zt[12] = {"door", "message", "set flag", "run", "item", "state", "action", "examine", "box", "empty", "event", "switch"};
            o.text = strfmt("ZONE %d %s", p[1], p[2] < 12 ? zt[p[2]] : "?");
            break;
        }
        case 0x36: o.text = strfmt("PLACE %d %d %d", s16v(2), s16v(4), s16v(6)); break;
        case 0x37: o.text = strfmt("TURN %d", s16v(2)); break;
        case 0x2D: o.text = strfmt("ZONE_ON %d = %d", p[1], p[3]); break;
        case 0x65: o.text = strfmt("ZONE_RUN %d", p[1]); break;
        case 0x64: o.text = strfmt("STEP_MATERIAL %d = %d", p[1], p[2]); break;
        case 0x63: o.text = "MUSIC " + hex(sz > 1 ? sz : 4).substr(3); break;
        case 0x21: o.text = strfmt("TEXT %06X", avail >= 8 ? le32(p + 4) : 0); break;
        case 0x02: o.text = strfmt("WAIT %d", p[2] | (p[3] << 8)); break;
        case 0x18: o.text = strfmt("WAITFLAG %d/%d", p[1], p[2]); break;
        case 0x23:
            if (avail >= 24)
                o.text = strfmt("CREATE %d  %06X  %d %d %d", p[1], le32(p + 8) & 0x1FFFFF, s16v(12), s16v(14), s16v(16));
            break;
        default: break;
        }
        if (o.text.empty()) o.text = strfmt("%02X  ", p[0]) + hex(sz > 0 ? std::min(sz, 24) : 4);
        out.push_back(o);
        if (sz <= 0) break;
        pc += sz;
    }
    return out;
}

}  // namespace dc
