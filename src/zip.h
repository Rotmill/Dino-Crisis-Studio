// ZIP for mod packages (10.10.2026): a package folder into a .zip for itch.io, Nexus, GitHub, and a downloaded .zip back into
// mods. Deflate and inflate are the ones the studio already carries (stb_image_write / stb_image, public domain); no ZIP64
// (a mod is far below 4 GB), file names in UTF-8 (flag 11).
#pragma once
#include "core.h"

namespace dc {

struct ZipEntry {
    std::string name;      // UTF-8, '/' between folders; a folder ends with '/'
    uint32_t crc = 0, csize = 0, usize = 0, off = 0;
    uint16_t method = 0;   // 0 stored, 8 deflate
};

// files of dir (recursively) into zip, each under prefix/ (prefix: the package folder name, empty - none)
bool zip_write_dir(const fs::path& dir, const fs::path& zip, const std::string& prefix, std::string& err, int* nfiles = nullptr);

// the list of a zip (false - not a zip this reader takes)
bool zip_list(const Bytes& z, std::vector<ZipEntry>& out, std::string& err);
// the bytes of one entry, checked by its CRC
bool zip_read(const Bytes& z, const ZipEntry& e, Bytes& out, std::string& err);
// a name safe to put under a folder: no drive, no root, no ".." (zip-slip)
bool zip_name_safe(const std::string& n);

}  // namespace dc
