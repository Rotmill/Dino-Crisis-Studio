// ZIP for mod packages (10.10.2026) - see zip.h.
#include "zip.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);   // stb_image_write (export.cpp)

namespace dc {

// ---------------------------------------------------------------- inflate (RFC 1951)
// Our own: the zlib decoder of stb_image (made for PNG) refused valid streams of zips made by Windows and .NET (a short
// stream of fixed blocks with long matches). Canonical Huffman decoding by code length, as the RFC describes it.
namespace {
struct Huff {
    short count[16];     // codes of each length
    short symbol[320];   // symbols ordered by code
};
struct Inflater {
    const uint8_t* in;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool bad = false;
    Bytes& out;
    Inflater(const uint8_t* p, size_t len, Bytes& o) : in(p), n(len), out(o) {}
    int bits(int need) {
        uint32_t v = buf;
        while (cnt < need) {
            if (pos >= n) { bad = true; return 0; }
            v |= (uint32_t)in[pos++] << cnt;
            cnt += 8;
        }
        buf = v >> need;
        cnt -= need;
        return (int)(v & ((1u << need) - 1));
    }
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= bits(1);
            if (bad) return -1;
            const int c = h.count[len];
            if (code - c < first) return h.symbol[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    }
    static bool build(Huff& h, const short* length, int nsym) {
        short offs[16];
        for (int i = 0; i < 16; i++) h.count[i] = 0;
        for (int s = 0; s < nsym; s++) h.count[length[s]]++;
        if (h.count[0] == nsym) return true;            // no codes (allowed for an unused distance table)
        int left = 1;
        for (int len = 1; len < 16; len++) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return false;                  // over-subscribed
        }
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h.count[len]);
        for (int s = 0; s < nsym; s++) if (length[s]) h.symbol[offs[length[s]]++] = (short)s;
        return true;                                     // incomplete codes are allowed (a single distance code)
    }
    bool codes(const Huff& lc, const Huff& dc) {
        static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                        131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
                                        2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(lc);
            if (sym < 0 || bad) return false;
            if (sym < 256) { out.push_back((uint8_t)sym); continue; }
            if (sym == 256) return true;
            sym -= 257;
            if (sym >= 29) return false;
            const int len = lbase[sym] + bits(lext[sym]);
            const int ds = decode(dc);
            if (ds < 0 || ds >= 30 || bad) return false;
            const size_t dist = (size_t)dbase[ds] + (size_t)bits(dext[ds]);
            if (bad || dist > out.size()) return false;
            const size_t from = out.size() - dist;
            for (int k = 0; k < len; k++) out.push_back(out[from + (size_t)k]);
        }
    }
    bool stored() {
        buf = 0; cnt = 0;                                // to the byte boundary
        if (pos + 4 > n) return false;
        const unsigned len = in[pos] | (in[pos + 1] << 8), nlen = in[pos + 2] | (in[pos + 3] << 8);
        pos += 4;
        if (len != (~nlen & 0xFFFFu) || pos + len > n) return false;
        out.insert(out.end(), in + pos, in + pos + len);
        pos += len;
        return true;
    }
    bool fixed() {
        static Huff lc, dc;
        static bool made = false;
        if (!made) {
            short l[288];
            int s = 0;
            for (; s < 144; s++) l[s] = 8;
            for (; s < 256; s++) l[s] = 9;
            for (; s < 280; s++) l[s] = 7;
            for (; s < 288; s++) l[s] = 8;
            build(lc, l, 288);
            for (s = 0; s < 30; s++) l[s] = 5;
            build(dc, l, 30);
            made = true;
        }
        return codes(lc, dc);
    }
    bool dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (bad || nlen > 286 || ndist > 30) return false;
        short l[320] = {0};
        for (int i = 0; i < ncode; i++) l[order[i]] = (short)bits(3);
        Huff hc;
        if (bad || !build(hc, l, 19)) return false;
        int i = 0;
        while (i < nlen + ndist) {
            int sym = decode(hc);
            if (sym < 0 || bad) return false;
            if (sym < 16) { l[i++] = (short)sym; continue; }
            short v = 0;
            int rep;
            if (sym == 16) { if (i == 0) return false; v = l[i - 1]; rep = 3 + bits(2); }
            else if (sym == 17) rep = 3 + bits(3);
            else rep = 11 + bits(7);
            if (bad || i + rep > nlen + ndist) return false;
            while (rep--) l[i++] = v;
        }
        if (l[256] == 0) return false;                   // no end-of-block code
        Huff lc, dc;
        if (!build(lc, l, nlen) || !build(dc, l + nlen, ndist)) return false;
        return codes(lc, dc);
    }
    bool run() {
        for (;;) {
            const int last = bits(1), type = bits(2);
            if (bad) return false;
            bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok) return false;
            if (last) return true;
        }
    }
};
}  // namespace

static bool inflate_raw(const uint8_t* p, size_t n, Bytes& out, size_t expect) {
    out.clear();
    out.reserve(expect);
    Inflater f(p, n, out);
    return f.run();
}

static uint32_t crc32_of(const uint8_t* p, size_t n) {
    static uint32_t tab[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void dos_time(const fs::path& p, uint16_t& t, uint16_t& d) {
    t = 0; d = (1 << 5) | 1;   // 1980-01-01
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &a)) return;
    FILETIME lt;
    WORD dd = 0, tt = 0;
    if (FileTimeToLocalFileTime(&a.ftLastWriteTime, &lt) && FileTimeToDosDateTime(&lt, &dd, &tt)) { t = tt; d = dd; }
}

static void add16(Bytes& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
static void add32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }

bool zip_write_dir(const fs::path& dir, const fs::path& zip, const std::string& prefix, std::string& err, int* nfiles) {
    std::error_code ec;
    std::vector<std::pair<std::string, fs::path>> files;   // name in the zip, the file
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = path_u8(fs::relative(it->path(), dir, ec));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        files.push_back({prefix.empty() ? rel : prefix + "/" + rel, it->path()});
    }
    if (ec) { err = "cannot read " + path_u8(dir) + ": " + ec.message(); return false; }
    if (files.empty()) { err = "the folder is empty"; return false; }
    if (files.size() > 65000) { err = "too many files for a zip"; return false; }
    std::sort(files.begin(), files.end());
    const fs::path tmp = zip.parent_path() / (zip.filename().wstring() + L".part");
    FILE* fp = _wfopen(tmp.wstring().c_str(), L"wb");
    if (!fp) { err = "cannot write " + path_u8(tmp); return false; }
    Bytes cd;
    uint32_t off = 0;
    bool ok = true;
    for (auto& f : files) {
        Bytes data;
        if (!read_file(f.second, data)) { err = "cannot read " + path_u8(f.second); ok = false; break; }
        if (data.size() > 0x7FFFFFFFu) { err = path_u8(f.second) + " is too big for a zip"; ok = false; break; }
        const uint32_t crc = crc32_of(data.data(), data.size());
        uint16_t method = 0;
        Bytes packed;
        if (data.size() > 64) {
            int zl = 0;
            unsigned char* z = stbi_zlib_compress(data.data(), (int)data.size(), &zl, 8);
            if (z && zl > 6 && (size_t)(zl - 6) < data.size() - data.size() / 20) {   // raw deflate: no 2-byte head, no adler
                packed.assign(z + 2, z + zl - 4);
                method = 8;
            }
            free(z);
        }
        const Bytes& body = method ? packed : data;
        uint16_t t, d;
        dos_time(f.second, t, d);
        Bytes h;
        add32(h, 0x04034B50u); add16(h, 20); add16(h, 0x0800); add16(h, method); add16(h, t); add16(h, d);
        add32(h, crc); add32(h, (uint32_t)body.size()); add32(h, (uint32_t)data.size());
        add16(h, (uint16_t)f.first.size()); add16(h, 0);
        h.insert(h.end(), f.first.begin(), f.first.end());
        if (fwrite(h.data(), 1, h.size(), fp) != h.size() || (body.size() && fwrite(body.data(), 1, body.size(), fp) != body.size())) {
            err = "cannot write " + path_u8(tmp); ok = false; break;
        }
        add32(cd, 0x02014B50u); add16(cd, 20); add16(cd, 20); add16(cd, 0x0800); add16(cd, method); add16(cd, t); add16(cd, d);
        add32(cd, crc); add32(cd, (uint32_t)body.size()); add32(cd, (uint32_t)data.size());
        add16(cd, (uint16_t)f.first.size()); add16(cd, 0); add16(cd, 0); add16(cd, 0); add16(cd, 0); add32(cd, 0x20); add32(cd, off);
        cd.insert(cd.end(), f.first.begin(), f.first.end());
        const uint64_t noff = (uint64_t)off + h.size() + body.size();
        if (noff > 0xFFFFFFFFull) { err = "the zip would be bigger than 4 GB"; ok = false; break; }
        off = (uint32_t)noff;
    }
    if (ok) {
        Bytes e;
        add32(e, 0x06054B50u); add16(e, 0); add16(e, 0); add16(e, (uint16_t)files.size()); add16(e, (uint16_t)files.size());
        add32(e, (uint32_t)cd.size()); add32(e, off); add16(e, 0);
        ok = fwrite(cd.data(), 1, cd.size(), fp) == cd.size() && fwrite(e.data(), 1, e.size(), fp) == e.size();
        if (!ok) err = "cannot write " + path_u8(tmp);
    }
    if (fclose(fp) != 0 && ok) { err = "cannot write " + path_u8(tmp); ok = false; }
    if (!ok) { fs::remove(tmp, ec); return false; }
    if (!MoveFileExW(tmp.wstring().c_str(), zip.wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(tmp, ec);
        err = "cannot write " + path_u8(zip);
        return false;
    }
    if (nfiles) *nfiles = (int)files.size();
    return true;
}

static std::string oem_to_u8(const std::string& s) {
    bool hi = false;
    for (unsigned char c : s) hi |= c >= 0x80;
    if (!hi) return s;
    int n = MultiByteToWideChar(CP_OEMCP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, s.data(), (int)s.size(), &w[0], n);
    return path_u8(fs::path(w));
}

bool zip_list(const Bytes& z, std::vector<ZipEntry>& out, std::string& err) {
    out.clear();
    if (z.size() < 22) { err = "not a zip"; return false; }
    size_t eo = std::string::npos;
    const size_t lo = z.size() > 65557 ? z.size() - 65557 : 0;
    for (size_t i = z.size() - 22 + 1; i-- > lo;)
        if (le32(&z[i]) == 0x06054B50u) { eo = i; break; }
    if (eo == std::string::npos) { err = "not a zip"; return false; }
    const uint32_t n = le16(&z[eo + 10]), cdsz = le32(&z[eo + 12]), cdo = le32(&z[eo + 16]);
    if ((uint64_t)cdo + cdsz > z.size()) { err = "a broken zip (or ZIP64, not supported)"; return false; }
    size_t p = cdo;
    for (uint32_t k = 0; k < n; k++) {
        if (p + 46 > z.size() || le32(&z[p]) != 0x02014B50u) { err = "a broken zip"; return false; }
        ZipEntry e;
        const uint16_t flags = le16(&z[p + 8]);
        e.method = le16(&z[p + 10]);
        e.crc = le32(&z[p + 16]); e.csize = le32(&z[p + 20]); e.usize = le32(&z[p + 24]);
        const uint16_t nl = le16(&z[p + 28]), xl = le16(&z[p + 30]), cl = le16(&z[p + 32]);
        const uint32_t lho = le32(&z[p + 42]);
        if (p + 46 + nl > z.size()) { err = "a broken zip"; return false; }
        e.name.assign((const char*)&z[p + 46], nl);
        if (!(flags & 0x0800)) e.name = oem_to_u8(e.name);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        if ((uint64_t)lho + 30 > z.size() || le32(&z[lho]) != 0x04034B50u) { err = "a broken zip"; return false; }
        e.off = lho + 30 + le16(&z[lho + 26]) + le16(&z[lho + 28]);
        if ((uint64_t)e.off + e.csize > z.size()) { err = "a broken zip"; return false; }
        if (flags & 1) { err = "the zip is encrypted"; return false; }
        out.push_back(e);
        p += 46 + nl + xl + cl;
    }
    return true;
}

bool zip_read(const Bytes& z, const ZipEntry& e, Bytes& out, std::string& err) {
    out.clear();
    if (e.method == 0) {
        out.assign(z.begin() + e.off, z.begin() + e.off + e.csize);
    } else if (e.method == 8) {
        if (!inflate_raw(&z[e.off], e.csize, out, e.usize)) { err = e.name + ": cannot unpack"; return false; }
    } else {
        err = e.name + ": a packing this studio does not read (method " + std::to_string(e.method) + ")";
        return false;
    }
    if (out.size() != e.usize || crc32_of(out.data(), out.size()) != e.crc) { err = e.name + ": damaged in the zip"; return false; }
    return true;
}

bool zip_name_safe(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
    size_t a = 0;
    while (a <= n.size()) {
        size_t b = n.find_first_of("/\\", a);
        if (b == std::string::npos) b = n.size();
        if (n.compare(a, b - a, "..") == 0 && b - a == 2) return false;
        a = b + 1;
    }
    return true;
}

}  // namespace dc
