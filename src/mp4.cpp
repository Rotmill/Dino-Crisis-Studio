// Видео в MP4 (H.264 + AAC) и обратно — средствами Windows (Media Foundation), без сторонних библиотек.
#include "mp4.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <windows.h>

#include <algorithm>
#include <cmath>

namespace dc {

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct MfScope {
    bool com = false, mf = false;
    MfScope() {
        HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(h);   // RPC_E_CHANGED_MODE — поток уже в другом режиме, это нормально
        mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    }
    ~MfScope() {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    }
};

std::string hr_text(const char* what, HRESULT h) { return strfmt("%s: 0x%08X", what, (unsigned)h); }

}  // namespace

std::vector<int16_t> resample_pcm(const std::vector<int16_t>& in, int ch, int from, int to) {
    if (from == to || in.empty() || ch < 1) return in;
    size_t frames = in.size() / ch;
    size_t outn = (size_t)((double)frames * to / from);
    std::vector<int16_t> out(outn * ch);
    const double step = (double)from / to;
    for (size_t i = 0; i < outn; i++) {
        double s = i * step;
        size_t a = (size_t)s;
        double f = s - a;
        size_t b = std::min(a + 1, frames - 1);
        for (int c = 0; c < ch; c++) out[i * ch + c] = (int16_t)lround(in[a * ch + c] * (1 - f) + in[b * ch + c] * f);
    }
    return out;
}

bool write_mp4(const fs::path& path, int w, int h, double fps, int nframes, const FrameFn& frame, const std::vector<int16_t>& pcm_in,
               int ch, int rate, std::string& err) {
    MfScope scope;
    if (!scope.mf) { err = "Media Foundation is not available"; return false; }
    // звук: AAC принимает 44.1 / 48 кГц, 16 бит, моно или стерео
    const int arate = 48000;
    const int ach = ch == 2 ? 2 : 1;
    std::vector<int16_t> pcm = resample_pcm(pcm_in, ach, rate, arate);
    IMFAttributes* at = nullptr;
    IMFSinkWriter* sw = nullptr;
    IMFMediaType *vo = nullptr, *vi = nullptr, *ao = nullptr, *ai = nullptr;
    DWORD vs = 0, as = 0;
    HRESULT hr = MFCreateAttributes(&at, 2);
    if (SUCCEEDED(hr)) at->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    std::error_code ec;
    fs::remove(path, ec);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path.wstring().c_str(), nullptr, at, &sw);
    if (FAILED(hr)) { err = hr_text("MFCreateSinkWriterFromURL", hr); release(at); return false; }
    const UINT32 fr_num = (UINT32)lround(fps * 1000), fr_den = 1000;
    // видео
    hr = MFCreateMediaType(&vo);
    if (SUCCEEDED(hr)) hr = vo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = vo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = vo->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::max(1500000, w * h * 40));
    if (SUCCEEDED(hr)) hr = vo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(vo, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vo, MF_MT_FRAME_RATE, fr_num, fr_den);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vo, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = sw->AddStream(vo, &vs);
    if (FAILED(hr)) { err = hr_text("video stream", hr); goto done; }
    hr = MFCreateMediaType(&vi);
    if (SUCCEEDED(hr)) hr = vi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = vi->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = vi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(vi, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vi, MF_MT_FRAME_RATE, fr_num, fr_den);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vi, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = vi->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4));   // строки сверху вниз
    if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(vs, vi, nullptr);
    if (FAILED(hr)) { err = hr_text("video input", hr); goto done; }
    // звук
    if (!pcm.empty()) {
        hr = MFCreateMediaType(&ao);
        if (SUCCEEDED(hr)) hr = ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if (SUCCEEDED(hr)) hr = ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, arate);
        if (SUCCEEDED(hr)) hr = ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ach);
        if (SUCCEEDED(hr)) hr = ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        if (SUCCEEDED(hr)) hr = sw->AddStream(ao, &as);
        if (FAILED(hr)) { err = hr_text("audio stream", hr); goto done; }
        hr = MFCreateMediaType(&ai);
        if (SUCCEEDED(hr)) hr = ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(hr)) hr = ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, arate);
        if (SUCCEEDED(hr)) hr = ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ach);
        if (SUCCEEDED(hr)) hr = ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ach * 2);
        if (SUCCEEDED(hr)) hr = ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, arate * ach * 2);
        if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(as, ai, nullptr);
        if (FAILED(hr)) { err = hr_text("audio input", hr); goto done; }
    }
    hr = sw->BeginWriting();
    if (FAILED(hr)) { err = hr_text("BeginWriting", hr); goto done; }
    {
        const LONGLONG dur = (LONGLONG)llround(1e7 / fps);
        std::vector<uint32_t> px;
        size_t apos = 0;
        const size_t aframes = pcm.size() / ach;
        for (int i = 0; i < nframes && SUCCEEDED(hr); i++) {
            if (!frame(i, px) || px.size() < (size_t)w * h) px.assign((size_t)w * h, 0xFF000000u);
            IMFMediaBuffer* buf = nullptr;
            IMFSample* smp = nullptr;
            hr = MFCreateMemoryBuffer((DWORD)(w * h * 4), &buf);
            BYTE* p = nullptr;
            if (SUCCEEDED(hr)) hr = buf->Lock(&p, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                for (size_t k = 0; k < (size_t)w * h; k++) {   // RGBA → BGRX
                    uint32_t c = px[k];
                    p[k * 4 + 0] = (BYTE)(c >> 16);
                    p[k * 4 + 1] = (BYTE)(c >> 8);
                    p[k * 4 + 2] = (BYTE)c;
                    p[k * 4 + 3] = 255;
                }
                buf->Unlock();
                hr = buf->SetCurrentLength((DWORD)(w * h * 4));
            }
            if (SUCCEEDED(hr)) hr = MFCreateSample(&smp);
            if (SUCCEEDED(hr)) hr = smp->AddBuffer(buf);
            if (SUCCEEDED(hr)) hr = smp->SetSampleTime(i * dur);
            if (SUCCEEDED(hr)) hr = smp->SetSampleDuration(dur);
            if (SUCCEEDED(hr)) hr = sw->WriteSample(vs, smp);
            release(smp);
            release(buf);
            // звук — до конца этого кадра
            size_t upto = std::min(aframes, (size_t)((double)(i + 1) * arate / fps));
            if (i == nframes - 1) upto = aframes;
            while (SUCCEEDED(hr) && apos < upto) {
                size_t n = std::min<size_t>(upto - apos, 4800);
                IMFMediaBuffer* ab = nullptr;
                IMFSample* as_ = nullptr;
                hr = MFCreateMemoryBuffer((DWORD)(n * ach * 2), &ab);
                BYTE* q = nullptr;
                if (SUCCEEDED(hr)) hr = ab->Lock(&q, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    memcpy(q, &pcm[apos * ach], n * ach * 2);
                    ab->Unlock();
                    hr = ab->SetCurrentLength((DWORD)(n * ach * 2));
                }
                if (SUCCEEDED(hr)) hr = MFCreateSample(&as_);
                if (SUCCEEDED(hr)) hr = as_->AddBuffer(ab);
                if (SUCCEEDED(hr)) hr = as_->SetSampleTime((LONGLONG)llround(apos * 1e7 / arate));
                if (SUCCEEDED(hr)) hr = as_->SetSampleDuration((LONGLONG)llround(n * 1e7 / arate));
                if (SUCCEEDED(hr)) hr = sw->WriteSample(as, as_);
                release(as_);
                release(ab);
                apos += n;
            }
        }
        if (FAILED(hr)) err = hr_text("WriteSample", hr);
        HRESULT hf = sw->Finalize();
        if (SUCCEEDED(hr) && FAILED(hf)) { hr = hf; err = hr_text("Finalize", hf); }
    }
done:
    release(ai); release(ao); release(vi); release(vo); release(sw); release(at);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------- чтение MP4 (проверка выгрузки)
bool read_mp4_info(const fs::path& path, int& w, int& h, double& seconds, int& frames, std::string& err) {
    MfScope scope;
    if (!scope.mf) { err = "Media Foundation is not available"; return false; }
    IMFSourceReader* rd = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &rd);
    if (FAILED(hr)) { err = hr_text("MFCreateSourceReaderFromURL", hr); return false; }
    IMFMediaType* t = nullptr;
    hr = rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &t);
    UINT32 W = 0, H = 0;
    if (SUCCEEDED(hr)) MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &W, &H);
    release(t);
    w = (int)W; h = (int)H;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    seconds = 0;
    if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) seconds = pv.uhVal.QuadPart / 1e7;
    PropVariantClear(&pv);
    frames = 0;
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    for (;;) {
        DWORD flags = 0;
        IMFSample* s = nullptr;
        hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, &s);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { release(s); break; }
        if (s) frames++;
        release(s);
    }
    release(rd);
    return w > 0;
}

}  // namespace dc
