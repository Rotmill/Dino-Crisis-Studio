// Образ диска: .bin (сырые сектора 2352) или .iso (2048), .cue — берём первый FILE. ISO9660 — как dc_mods.c игры.
#include "core.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dc {

// ---------------------------------------------------------------- utils
fs::path u8path(const std::string& s) { return fs::u8path(s); }
std::string path_u8(const fs::path& p) { return p.u8string(); }

bool read_file(const fs::path& p, Bytes& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out.resize((size_t)n);
    if (n > 0) f.read((char*)out.data(), n);
    return (bool)f || f.eof();
}

bool write_file(const fs::path& p, const void* data, size_t n) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (n) f.write((const char*)data, (std::streamsize)n);
    return (bool)f;
}

bool write_text(const fs::path& p, const std::string& s) { return write_file(p, s.data(), s.size()); }

std::string strfmt(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if ((size_t)n < sizeof buf) return std::string(buf, (size_t)n);
    std::string s((size_t)n + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(&s[0], s.size(), fmt, ap);
    va_end(ap);
    s.resize((size_t)n);
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

// SHA-1 (для удаления повторов текстур и сэмплов)
namespace {
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf[64];
    uint64_t len = 0;
    size_t bl = 0;
    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void add(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            size_t k = std::min(n, 64 - bl);
            memcpy(buf + bl, p, k);
            bl += k; p += k; n -= k;
            if (bl == 64) { block(buf); bl = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        add(&pad, 1);
        uint8_t z = 0;
        while (bl != 56) add(&z, 1);
        uint8_t L[8];
        for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
        add(L, 8);
        char s[41];
        for (int i = 0; i < 5; i++) snprintf(s + i * 8, 9, "%08x", h[i]);
        return std::string(s, 40);
    }
};
}  // namespace

std::string sha1_hex(const void* data, size_t n) {
    Sha1 s;
    s.add((const uint8_t*)data, n);
    return s.hex();
}

// ---------------------------------------------------------------- категории = подпапки mods и source
// 05.10.2026 (игрок: структура PC-версии — как образец): Movie → movie, Sound\BGM/SE/VOICE → sound\bgm, sound\se,
// sound\voice. Все ME*/MF*/MR*/MS*/MT*/M__* на PS1 — музыкальные банки (у PC это треки Sound\BGM me_XX, mf_XX…),
// STAGE00.XAS — шумы и гул (не речь), STAGE01..06.XAS — речь.
const char* const kModCategories[] = {"characters", "weapons", "items", "rooms", "doors", "screens", "movie",
                                      "sound\\bgm", "sound\\se", "sound\\voice",
                                      "textures", "collision", "other", nullptr};

const char* category_of(const std::string& dir, const std::string& name, bool* stream) {
    *stream = false;
    const char* n = name.c_str();
    if (dir == "/PSX/MOVIE/") { *stream = true; return "movie"; }
    if (dir == "/PSX/DATA_XA/") { *stream = true; return name == "STAGE00.XAS" ? "sound\\se" : "sound\\voice"; }
    // программа игры (таблицы дверей, предметов, тексты) и оверлеи этапов (ростер динозавров) — в other
    if (dir == "/" && upper(name) == "SLUS_009.22") return "other";
    if (dir == "/PSX/BIN/" && n[0] == 'S' && n[1] == 'T' && n[2] >= '1' && n[2] <= '9' && !strcmp(n + 3, ".BIN")) return "other";
    if (dir != "/PSX/DATA/") return nullptr;
    if (n[0] == 'S' && n[1] == 'T' && isxdigit((unsigned char)n[2])) return "rooms";
    if (!strncmp(n, "DOOR", 4) || !strncmp(n, "UP", 2) || !strncmp(n, "DOWN", 4) || !strncmp(n, "WIRE", 4)) return "doors";
    if (!strncmp(n, "CORE", 4) ||
        ((n[0] == 'P' || n[0] == 'H') && isdigit((unsigned char)n[1]) && isdigit((unsigned char)n[2]) && n[3] == '.'))
        return "characters";
    if (!strncmp(n, "ITEM", 4)) return "items";
    if (n[0] == 'M' && n[1] && strchr("EFRST_", n[1]) && (n[2] == '_' || isxdigit((unsigned char)n[2]))) return "sound\\bgm";
    if (!strncmp(n, "DEMO", 4)) return "other";
    return "screens";
}

// ---------------------------------------------------------------- Disc
Disc::~Disc() { close(); }

void Disc::close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (fp_) fclose(fp_);
    fp_ = nullptr;
    files.clear();
    dino_crisis = false;
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool Disc::open(const fs::path& path, std::string& err) {
    close();
    fs::path bin = path;
    std::string ext = upper(path_u8(path.extension()));
    if (ext == ".CUE") {
        std::ifstream f(path);
        if (!f) { err = "cue"; return false; }
        std::string line, name;
        while (std::getline(f, line)) {
            std::string t = trim(line);
            if (upper(t.substr(0, 5)) == "FILE ") {
                size_t q1 = t.find('"'), q2 = t.rfind('"');
                if (q1 != std::string::npos && q2 > q1) name = t.substr(q1 + 1, q2 - q1 - 1);
                else {
                    std::istringstream is(t.substr(5));
                    is >> name;
                }
                break;
            }
        }
        if (name.empty()) { err = "cue: FILE"; return false; }
        bin = path.parent_path() / u8path(name);
    }
    FILE* fp = _wfopen(bin.wstring().c_str(), L"rb");
    if (!fp) { err = "open: " + path_u8(bin); return false; }
    _fseeki64(fp, 0, SEEK_END);
    fsize_ = (uint64_t)_ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    uint8_t head[16] = {};
    fread(head, 1, 16, fp);
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    raw2352 = memcmp(head, sync, 12) == 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fp_ = fp;
    }
    image_path = bin;
    uint8_t pvd[2048];
    if (!read_user(16, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) {
        err = "iso9660";
        close();
        return false;
    }
    walk(le32(pvd + 156 + 2), le32(pvd + 156 + 10), "/", 0);
    std::sort(files.begin(), files.end(), [](const DiscFile& a, const DiscFile& b) { return a.lba < b.lba; });
    // SYSTEM.CNF → имя EXE
    if (const DiscFile* cnf = find_path("/SYSTEM.CNF")) {
        Bytes b;
        if (read_file(*cnf, b)) {
            std::string s(b.begin(), b.end());
            size_t p = s.find("cdrom:");
            if (p != std::string::npos) {
                size_t a = s.find_first_not_of("\\/", p + 6);
                size_t e = s.find(';', a);
                if (a != std::string::npos) exe_name = s.substr(a, (e == std::string::npos ? s.size() : e) - a);
            }
        }
    }
    dino_crisis = find_path("/PSX/DATA/ST100.DAT") && find_path("/PSX/DATA/CORE.DAT") &&
                  (upper(exe_name) == "SLUS_009.22" || find_path("/SLUS_009.22"));
    return true;
}

bool Disc::read_raw(uint32_t lba, uint8_t out[2352]) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fp_ || !raw2352) return false;
    uint64_t off = (uint64_t)lba * 2352u;
    if (off + 2352 > fsize_) return false;
    _fseeki64(fp_, (long long)off, SEEK_SET);
    return fread(out, 1, 2352, fp_) == 2352;
}

bool Disc::read_user(uint32_t lba, uint8_t out[2048]) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fp_) return false;
    uint64_t off = raw2352 ? (uint64_t)lba * 2352u + 24u : (uint64_t)lba * 2048u;
    if (off + 2048 > fsize_) return false;
    _fseeki64(fp_, (long long)off, SEEK_SET);
    return fread(out, 1, 2048, fp_) == 2048;
}

bool Disc::read_file(const DiscFile& f, Bytes& out) {
    out.resize((size_t)f.nsec * 2048u);
    for (uint32_t i = 0; i < f.nsec; i++)
        if (!read_user(f.lba + i, out.data() + (size_t)i * 2048u)) return false;
    out.resize(f.size);
    return true;
}

const DiscFile* Disc::find(const std::string& name) const {
    std::string u = upper(name);
    for (auto& f : files)
        if (f.name == u) return &f;
    return nullptr;
}

const DiscFile* Disc::find_path(const std::string& path) const {
    std::string u = upper(path);
    for (auto& f : files)
        if (f.path == u) return &f;
    return nullptr;
}

void Disc::walk(uint32_t lba, uint32_t size, const std::string& prefix, int depth) {
    if (depth > 6 || size == 0 || size > (1u << 22)) return;
    const uint32_t nsec = (size + 2047u) / 2048u;
    Bytes d((size_t)nsec * 2048u);
    for (uint32_t i = 0; i < nsec; i++)
        if (!read_user(lba + i, d.data() + (size_t)i * 2048u)) return;
    uint32_t off = 0;
    while (off < size) {
        const uint32_t rl = d[off];
        if (rl == 0) { off = (off / 2048u + 1u) * 2048u; continue; }
        if (off + rl > nsec * 2048u || rl < 34) break;
        const uint8_t* r = d.data() + off;
        const uint32_t elba = le32(r + 2), elen = le32(r + 10);
        const uint8_t flags = r[25], nl = r[32];
        off += rl;
        if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;
        std::string nm;
        for (int i = 0; i < nl && r[33 + i] != ';'; i++) nm += (char)r[33 + i];
        nm = upper(nm);
        if (flags & 0x02) {
            walk(elba, elen, prefix + nm + "/", depth + 1);
        } else {
            DiscFile f;
            f.dir = prefix;
            f.name = nm;
            f.path = prefix + nm;
            size_t dot = nm.rfind('.');
            f.stem = dot == std::string::npos ? nm : nm.substr(0, dot);
            f.lba = elba;
            f.size = elen;
            f.nsec = (elen + 2047u) / 2048u;
            bool st = false;
            const char* c = category_of(prefix, nm, &st);
            f.cat = c ? c : "";
            f.stream = st;
            files.push_back(f);
        }
    }
}

}  // namespace dc
