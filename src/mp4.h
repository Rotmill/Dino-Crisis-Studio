// Видео в MP4 (H.264 + AAC) средствами Windows (Media Foundation).
#pragma once
#include <functional>

#include "core.h"

namespace dc {

// кадр i → RGBA (байты R,G,B,A); false — чёрный кадр
using FrameFn = std::function<bool(int, std::vector<uint32_t>&)>;
bool write_mp4(const fs::path& path, int w, int h, double fps, int nframes, const FrameFn& frame, const std::vector<int16_t>& pcm,
               int channels, int rate, std::string& err);
bool read_mp4_info(const fs::path& path, int& w, int& h, double& seconds, int& frames, std::string& err);
std::vector<int16_t> resample_pcm(const std::vector<int16_t>& in, int channels, int from, int to);

}  // namespace dc
