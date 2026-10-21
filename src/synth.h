// Мелодии игры (записи типа 5) по банку VAB файла — как их играет звуковой драйвер Dino Crisis (разбор 05.10.2026, re/sound.md).
#pragma once
#include "content.h"

namespace dc {

struct SongPcm {
    std::vector<int16_t> pcm;   // стерео, 44.1 кГц
    int loop_start = -1, loop_end = -1;   // кадры (петля мелодии, если есть)
    bool loops = false;
};

// intro + loops тел петли (у мелодии с петлёй); у одноразовой — до конца (+ затухание голосов)
bool render_song(const FileContent& c, int seq_index, SongPcm& out, int loops = 2);
// банк мелодии: ближайшая предыдущая запись VAB в файле (правило драйвера; сверено на всех 289 записях)
int song_bank(const FileContent& c, int seq_index);
// ключ мелодии для подмены WAV в игре: FNV-1a 64 байтов записи до FF 2F 00 включительно
uint64_t song_key(const uint8_t* d, size_t n);

}  // namespace dc
