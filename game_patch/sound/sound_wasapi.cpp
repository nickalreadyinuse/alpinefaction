// WASAPI audio backend: an in-process IDirectSound / IDirectSoundBuffer implementation whose buffers are mixed in
// software and rendered through WASAPI. The game (and Bink, which is handed the same object) keeps calling
// DirectSound, so loading, streaming, pitch and channel management stay untouched; only the output path changes.
// Nothing here opens a DirectSound device, which is what lets exclusive mode work.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <dsound.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <xlog/xlog.h>
#include "sound_wasapi.h"

// Local copies so no dxguid.lib / INITGUID is needed (values match dsound.h and RF.exe's .rdata)
static constexpr GUID kIID_IDirectSound{0x279AFA83, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
static constexpr GUID kIID_IDirectSoundBuffer{
    0x279AFA85, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

class WasapiBuffer;

// ---------------------------------------------------------------------------------------------------------------
// Shared mixer state
// ---------------------------------------------------------------------------------------------------------------

// Guards every buffer's playback state and the active list. Held by the audio thread for one mix pass (tens of
// microseconds) and by DirectSound calls from the game and Bink threads.
static std::mutex g_mix_mutex;
static std::vector<WasapiBuffer*> g_active_buffers; // each entry holds a reference
static float g_master_gain = 1.0f;                  // primary buffer volume
static std::atomic<HWND> g_coop_hwnd{nullptr};

// Output parameters the buffers need for cursor reporting (written by the audio thread)
static std::atomic<int> g_output_rate{48000};
static std::atomic<int> g_output_chunk_frames{480}; // largest block the mixer consumes per pass

static float mb_to_gain(LONG mb)
{
    return mb <= DSBVOLUME_MIN ? 0.0f : std::pow(10.0f, static_cast<float>(mb) / 2000.0f);
}

static bool app_has_focus()
{
    HWND hwnd = g_coop_hwnd.load(std::memory_order_relaxed);
    if (!hwnd) {
        return true;
    }
    HWND fg = GetForegroundWindow();
    return fg && GetAncestor(fg, GA_ROOTOWNER) == GetAncestor(hwnd, GA_ROOTOWNER);
}

// Accepts the formats a software DirectSound secondary buffer must handle: 8/16-bit PCM, mono or stereo
static bool normalize_format(const WAVEFORMATEX* in, WAVEFORMATEX& out)
{
    if (!in) {
        return false;
    }
    bool pcm = in->wFormatTag == WAVE_FORMAT_PCM;
    if (in->wFormatTag == WAVE_FORMAT_EXTENSIBLE && in->cbSize >= 22) {
        pcm = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(in)->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
    }
    if (!pcm || (in->nChannels != 1 && in->nChannels != 2) || (in->wBitsPerSample != 8 && in->wBitsPerSample != 16)
        || in->nSamplesPerSec < DSBFREQUENCY_MIN || in->nSamplesPerSec > DSBFREQUENCY_MAX) {
        return false;
    }
    out = {};
    out.wFormatTag = WAVE_FORMAT_PCM;
    out.nChannels = in->nChannels;
    out.nSamplesPerSec = in->nSamplesPerSec;
    out.wBitsPerSample = in->wBitsPerSample;
    out.nBlockAlign = static_cast<WORD>(out.nChannels * out.wBitsPerSample / 8);
    out.nAvgBytesPerSec = out.nSamplesPerSec * out.nBlockAlign;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// IDirectSoundBuffer
// ---------------------------------------------------------------------------------------------------------------

class WasapiBuffer final : public IDirectSoundBuffer
{
public:
    WasapiBuffer(bool primary, DWORD flags, const WAVEFORMATEX& fmt, std::shared_ptr<std::vector<uint8_t>> data) :
        m_primary(primary), m_flags(flags), m_fmt(fmt), m_data(std::move(data)), m_freq(fmt.nSamplesPerSec)
    {
        m_frames = m_data && fmt.nBlockAlign ? static_cast<uint32_t>(m_data->size() / fmt.nBlockAlign) : 0;
    }

    WasapiBuffer* duplicate()
    {
        auto* dup = new WasapiBuffer(false, m_flags, m_fmt, m_data);
        std::lock_guard lock{g_mix_mutex};
        dup->m_freq = m_freq;
        dup->m_volume = m_volume;
        dup->m_pan = m_pan;
        return dup;
    }

    bool is_primary() const { return m_primary; }

    // --- mixer side, called with g_mix_mutex held ---

    // Adds this buffer into the stereo mix. Returns false once it stops (finished one-shot or Stop()).
    bool mix(float* out, uint32_t frames, double output_rate, float master, bool focused)
    {
        if (!m_playing || m_frames == 0) {
            return false;
        }
        const double step = m_freq / output_rate;
        float gain = mb_to_gain(m_volume) * master;
        // Stock DirectSound mutes buffers without a focus flag while another app is in front
        if (!focused && !(m_flags & (DSBCAPS_GLOBALFOCUS | DSBCAPS_STICKYFOCUS))) {
            gain = 0.0f;
        }
        const float left_gain = gain * (m_pan > 0 ? mb_to_gain(-m_pan) : 1.0f);
        const float right_gain = gain * (m_pan < 0 ? mb_to_gain(m_pan) : 1.0f);
        const double total = m_frames;

        if (left_gain == 0.0f && right_gain == 0.0f) {
            // Silent: advance the cursor without touching samples so timing and stream refills stay correct
            m_pos += step * frames;
        }
        else {
            double pos = m_pos;
            for (uint32_t f = 0; f < frames; ++f) {
                if (pos >= total) {
                    if (!m_looping) {
                        break;
                    }
                    pos = std::fmod(pos, total);
                }
                const auto i = static_cast<uint32_t>(pos);
                const float t = static_cast<float>(pos - i);
                uint32_t j = i + 1;
                if (j >= m_frames) {
                    j = m_looping ? 0 : i;
                }
                float l0, r0, l1, r1;
                read_frame(i, l0, r0);
                read_frame(j, l1, r1);
                out[f * 2] += (l0 + t * (l1 - l0)) * left_gain;
                out[f * 2 + 1] += (r0 + t * (r1 - r0)) * right_gain;
                pos += step;
            }
            m_pos = pos;
        }

        if (m_pos >= total) {
            if (!m_looping) {
                // ponytail: matches dsoal, a finished one-shot rewinds; stock DS behaviour here is undocumented
                m_playing = false;
                m_pos = 0.0;
                return false;
            }
            m_pos = std::fmod(m_pos, total);
        }
        return true;
    }

    void set_inactive() { m_in_active_list = false; }

    // --- IUnknown ---

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == kIID_IDirectSoundBuffer) {
            AddRef();
            *out = static_cast<IDirectSoundBuffer*>(this);
            return S_OK;
        }
        // No IDirectSound3DBuffer / IKsPropertySet / IDirectSoundNotify: DS3D and EAX are disabled in WASAPI mode
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG refs = --m_refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    // --- IDirectSoundBuffer ---

    HRESULT STDMETHODCALLTYPE GetCaps(LPDSBCAPS caps) override
    {
        if (!caps || caps->dwSize < sizeof(DSBCAPS)) {
            return DSERR_INVALIDPARAM;
        }
        caps->dwFlags = m_flags | (m_primary ? 0 : DSBCAPS_LOCSOFTWARE);
        caps->dwBufferBytes = m_data ? static_cast<DWORD>(m_data->size()) : 0;
        caps->dwUnlockTransferRate = 0;
        caps->dwPlayCpuOverhead = 0;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD play_cursor, LPDWORD write_cursor) override
    {
        DWORD play = 0;
        DWORD write = 0;
        if (!m_primary && m_frames) {
            std::lock_guard lock{g_mix_mutex};
            auto frame = static_cast<uint32_t>(m_pos);
            if (frame >= m_frames) {
                frame = 0;
            }
            uint32_t write_frame = frame;
            if (m_playing) {
                // The mixer has consumed up to the play cursor and may read one more chunk before the next update
                const double lead = static_cast<double>(g_output_chunk_frames.load(std::memory_order_relaxed)) * m_freq
                    / g_output_rate.load(std::memory_order_relaxed);
                write_frame = (frame + static_cast<uint32_t>(lead) + 1) % m_frames;
            }
            play = frame * m_fmt.nBlockAlign;
            write = write_frame * m_fmt.nBlockAlign;
        }
        if (play_cursor) {
            *play_cursor = play;
        }
        if (write_cursor) {
            *write_cursor = write;
        }
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX fmt, DWORD size, LPDWORD written) override
    {
        if (!fmt && !written) {
            return DSERR_INVALIDPARAM;
        }
        if (fmt) {
            if (size < sizeof(WAVEFORMATEX)) {
                return DSERR_INVALIDPARAM;
            }
            *fmt = m_fmt;
            fmt->cbSize = 0;
        }
        if (written) {
            *written = sizeof(WAVEFORMATEX);
        }
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetVolume(LPLONG volume) override
    {
        if (!volume) {
            return DSERR_INVALIDPARAM;
        }
        std::lock_guard lock{g_mix_mutex};
        *volume = m_volume;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPan(LPLONG pan) override
    {
        if (!pan) {
            return DSERR_INVALIDPARAM;
        }
        std::lock_guard lock{g_mix_mutex};
        *pan = m_pan;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFrequency(LPDWORD freq) override
    {
        if (!freq) {
            return DSERR_INVALIDPARAM;
        }
        std::lock_guard lock{g_mix_mutex};
        *freq = m_freq;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD status) override
    {
        if (!status) {
            return DSERR_INVALIDPARAM;
        }
        std::lock_guard lock{g_mix_mutex};
        *status = m_playing ? (DSBSTATUS_PLAYING | (m_looping ? DSBSTATUS_LOOPING : 0)) : 0;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTSOUND, LPCDSBUFFERDESC) override
    {
        return DSERR_ALREADYINITIALIZED;
    }

    HRESULT STDMETHODCALLTYPE Lock(DWORD offset, DWORD bytes, LPVOID* ptr1, LPDWORD bytes1, LPVOID* ptr2,
                                   LPDWORD bytes2, DWORD flags) override
    {
        if (m_primary) {
            return DSERR_PRIOLEVELNEEDED;
        }
        if (!ptr1 || !bytes1) {
            return DSERR_INVALIDPARAM;
        }
        const auto size = static_cast<DWORD>(m_data->size());
        if (flags & DSBLOCK_FROMWRITECURSOR) {
            GetCurrentPosition(nullptr, &offset);
        }
        if (flags & DSBLOCK_ENTIREBUFFER) {
            bytes = size;
        }
        if (offset >= size || bytes == 0 || bytes > size) {
            return DSERR_INVALIDPARAM;
        }
        // Static data, never reallocated: the caller writes directly into the region the mixer reads
        const DWORD first = std::min(bytes, size - offset);
        if (first < bytes && !ptr2) {
            return DSERR_INVALIDPARAM;
        }
        *ptr1 = m_data->data() + offset;
        *bytes1 = first;
        if (ptr2) {
            *ptr2 = first < bytes ? m_data->data() : nullptr;
        }
        if (bytes2) {
            *bytes2 = bytes - first;
        }
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Play(DWORD, DWORD, DWORD flags) override
    {
        if (m_primary) {
            return DS_OK;
        }
        std::lock_guard lock{g_mix_mutex};
        m_looping = (flags & DSBPLAY_LOOPING) != 0;
        m_playing = true;
        if (!m_in_active_list) {
            AddRef();
            g_active_buffers.push_back(this);
            m_in_active_list = true;
        }
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD pos) override
    {
        if (m_primary) {
            return DSERR_INVALIDCALL;
        }
        std::lock_guard lock{g_mix_mutex};
        if (m_frames) {
            m_pos = std::min<uint32_t>(pos / m_fmt.nBlockAlign, m_frames - 1);
        }
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFormat(LPCWAVEFORMATEX fmt) override
    {
        if (!m_primary) {
            return DSERR_INVALIDCALL;
        }
        // The primary format only describes DirectSound's own mixer; ours always mixes at the device rate
        return fmt ? DS_OK : DSERR_INVALIDPARAM;
    }

    HRESULT STDMETHODCALLTYPE SetVolume(LONG volume) override
    {
        if (volume < DSBVOLUME_MIN || volume > DSBVOLUME_MAX) {
            return DSERR_INVALIDPARAM;
        }
        std::lock_guard lock{g_mix_mutex};
        m_volume = volume;
        if (m_primary) {
            g_master_gain = mb_to_gain(volume);
        }
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetPan(LONG pan) override
    {
        if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT) {
            return DSERR_INVALIDPARAM;
        }
        std::lock_guard lock{g_mix_mutex};
        m_pan = pan;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFrequency(DWORD freq) override
    {
        if (m_primary) {
            return DSERR_CONTROLUNAVAIL;
        }
        if (freq == DSBFREQUENCY_ORIGINAL) {
            freq = m_fmt.nSamplesPerSec;
        }
        if (freq < DSBFREQUENCY_MIN || freq > DSBFREQUENCY_MAX) {
            return DSERR_INVALIDPARAM;
        }
        std::lock_guard lock{g_mix_mutex};
        m_freq = freq;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Stop() override
    {
        // The mixer drops (and releases) the buffer on its next pass; the position is kept like DirectSound does
        std::lock_guard lock{g_mix_mutex};
        m_playing = false;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Unlock(LPVOID, DWORD, LPVOID, DWORD) override { return DS_OK; }

    HRESULT STDMETHODCALLTYPE Restore() override { return DS_OK; }

private:
    void read_frame(uint32_t frame, float& left, float& right) const
    {
        const uint8_t* base = m_data->data() + static_cast<size_t>(frame) * m_fmt.nBlockAlign;
        if (m_fmt.wBitsPerSample == 16) {
            int16_t s[2];
            std::memcpy(s, base, m_fmt.nBlockAlign);
            left = s[0] / 32768.0f;
            right = m_fmt.nChannels == 2 ? s[1] / 32768.0f : left;
        }
        else {
            left = (base[0] - 128) / 128.0f;
            right = m_fmt.nChannels == 2 ? (base[1] - 128) / 128.0f : left;
        }
    }

    std::atomic<ULONG> m_refs{1};
    const bool m_primary;
    const DWORD m_flags;
    const WAVEFORMATEX m_fmt;
    const std::shared_ptr<std::vector<uint8_t>> m_data; // shared between duplicates
    uint32_t m_frames = 0;

    // Guarded by g_mix_mutex
    bool m_playing = false;
    bool m_looping = false;
    bool m_in_active_list = false;
    double m_pos = 0.0; // in source frames, fractional for resampling
    DWORD m_freq;
    LONG m_volume = DSBVOLUME_MAX;
    LONG m_pan = DSBPAN_CENTER;
};

// ---------------------------------------------------------------------------------------------------------------
// IDirectSound
// ---------------------------------------------------------------------------------------------------------------

class WasapiDirectSound final : public IDirectSound
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == kIID_IDirectSound) {
            AddRef();
            *out = static_cast<IDirectSound*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG refs = --m_refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out,
                                                LPUNKNOWN outer) override
    {
        if (!desc || !out || desc->dwSize < sizeof(DSBUFFERDESC1)) {
            return DSERR_INVALIDPARAM;
        }
        if (outer) {
            return DSERR_NOAGGREGATION;
        }
        *out = nullptr;

        if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
            WAVEFORMATEX fmt{WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
            *out = new WasapiBuffer(true, desc->dwFlags, fmt, nullptr);
            return DS_OK;
        }

        WAVEFORMATEX fmt;
        if (!normalize_format(desc->lpwfxFormat, fmt)) {
            const WAVEFORMATEX* f = desc->lpwfxFormat;
            xlog::warn("WASAPI: rejected buffer format tag=0x{:04X} {} ch {} bits {} Hz", f ? f->wFormatTag : 0,
                f ? f->nChannels : 0, f ? f->wBitsPerSample : 0, f ? f->nSamplesPerSec : 0);
            return DSERR_BADFORMAT;
        }
        DWORD bytes = desc->dwBufferBytes - desc->dwBufferBytes % fmt.nBlockAlign;
        if (bytes < DSBSIZE_MIN || bytes > DSBSIZE_MAX) {
            return DSERR_INVALIDPARAM;
        }
        auto data = std::make_shared<std::vector<uint8_t>>(bytes, fmt.wBitsPerSample == 8 ? 0x80 : 0x00);
        *out = new WasapiBuffer(false, desc->dwFlags, fmt, std::move(data));
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCaps(LPDSCAPS caps) override
    {
        if (!caps || caps->dwSize < sizeof(DSCAPS)) {
            return DSERR_INVALIDPARAM;
        }
        DWORD size = caps->dwSize;
        std::memset(caps, 0, size);
        caps->dwSize = size;
        caps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT
            | DSCAPS_CONTINUOUSRATE | DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT
            | DSCAPS_SECONDARY16BIT;
        caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
        caps->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
        caps->dwPrimaryBuffers = 1;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER original, LPDIRECTSOUNDBUFFER* out) override
    {
        if (!original || !out) {
            return DSERR_INVALIDPARAM;
        }
        // Every buffer handed out by this object is a WasapiBuffer
        auto* src = static_cast<WasapiBuffer*>(original);
        if (src->is_primary()) {
            *out = nullptr;
            return DSERR_INVALIDCALL;
        }
        *out = src->duplicate();
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND hwnd, DWORD) override
    {
        g_coop_hwnd = hwnd;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE Compact() override { return DS_OK; }

    HRESULT STDMETHODCALLTYPE GetSpeakerConfig(LPDWORD config) override
    {
        if (!config) {
            return DSERR_INVALIDPARAM;
        }
        *config = DSSPEAKER_STEREO;
        return DS_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD) override { return DS_OK; }

    HRESULT STDMETHODCALLTYPE Initialize(LPCGUID) override { return DS_OK; }

private:
    std::atomic<ULONG> m_refs{1};
};

// ---------------------------------------------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------------------------------------------

static void mix_buffers(float* out, uint32_t frames, int output_rate)
{
    std::memset(out, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
    const bool focused = app_has_focus();
    static std::vector<WasapiBuffer*> finished; // audio thread only
    {
        std::lock_guard lock{g_mix_mutex};
        for (size_t i = 0; i < g_active_buffers.size();) {
            WasapiBuffer* buf = g_active_buffers[i];
            if (buf->mix(out, frames, output_rate, g_master_gain, focused)) {
                ++i;
                continue;
            }
            buf->set_inactive();
            finished.push_back(buf);
            g_active_buffers[i] = g_active_buffers.back();
            g_active_buffers.pop_back();
        }
    }
    // Outside the lock: dropping the last reference deletes the buffer
    for (WasapiBuffer* buf : finished) {
        buf->Release();
    }
    finished.clear();
}

// Converts the stereo float mix to the device format
static void write_output(BYTE* dest, const float* mix, uint32_t frames, const WAVEFORMATEX* fmt)
{
    const int channels = fmt->nChannels;
    bool is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        is_float = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    const int bytes_per_sample = fmt->wBitsPerSample / 8;

    for (uint32_t f = 0; f < frames; ++f) {
        const float left = std::clamp(mix[f * 2], -1.0f, 1.0f);
        const float right = std::clamp(mix[f * 2 + 1], -1.0f, 1.0f);
        for (int c = 0; c < channels; ++c) {
            // Stereo goes to the first two speakers; a mono device gets the downmix, the rest stay silent
            float s = channels == 1 ? (left + right) * 0.5f : c == 0 ? left : c == 1 ? right : 0.0f;
            BYTE* out = dest + (static_cast<size_t>(f) * channels + c) * bytes_per_sample;
            if (is_float) {
                std::memcpy(out, &s, sizeof(float));
            }
            else if (bytes_per_sample == 2) {
                auto v = static_cast<int16_t>(std::lround(s * 32767.0f));
                std::memcpy(out, &v, 2);
            }
            else if (bytes_per_sample == 3) {
                auto v = static_cast<int32_t>(std::lround(s * 8388607.0f));
                out[0] = static_cast<BYTE>(v);
                out[1] = static_cast<BYTE>(v >> 8);
                out[2] = static_cast<BYTE>(v >> 16);
            }
            else if (bytes_per_sample == 4) {
                // Also correct for 24-in-32 containers, which are left-justified
                auto v = static_cast<int32_t>(std::lround(static_cast<double>(s) * 2147483647.0));
                std::memcpy(out, &v, 4);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Output device (owned by the audio thread)
// ---------------------------------------------------------------------------------------------------------------

static std::mutex g_status_mutex;
static WasapiStatus g_status;

static void set_status(const WasapiStatus& status)
{
    std::lock_guard lock{g_status_mutex};
    g_status = status;
}

template<typename T>
static void safe_release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static DWORD channel_mask(WORD channels)
{
    switch (channels) {
        case 1: return SPEAKER_FRONT_CENTER;
        case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        case 6: return KSAUDIO_SPEAKER_5POINT1;
        case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
        default: return (1u << channels) - 1;
    }
}

struct OutputDevice
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE event = nullptr;
    WAVEFORMATEX* fmt = nullptr; // CoTaskMemAlloc'd
    UINT32 buffer_frames = 0;
    UINT32 period_frames = 0; // low-latency shared engine period, 0 otherwise
    bool exclusive = false;
    std::string name;

    ~OutputDevice() { close(); }

    void close()
    {
        if (client) {
            client->Stop();
        }
        safe_release(render);
        safe_release(client);
        safe_release(device);
        safe_release(enumerator);
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }
        if (fmt) {
            CoTaskMemFree(fmt);
            fmt = nullptr;
        }
    }

    bool reactivate()
    {
        safe_release(client);
        return SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&client)));
    }

    bool open(bool want_exclusive, int buffer_ms)
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || FAILED(hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            xlog::warn("WASAPI: no default output device (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        read_device_name();
        if (!reactivate() || FAILED(hr = client->GetMixFormat(&fmt))) {
            xlog::warn("WASAPI: failed to activate {} (0x{:08X})", name, static_cast<unsigned>(hr));
            return false;
        }
        event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            return false;
        }

        if (want_exclusive && !init_exclusive(buffer_ms)) {
            xlog::warn("WASAPI: exclusive mode unavailable on {}, using shared mode", name);
            CoTaskMemFree(fmt);
            fmt = nullptr;
            if (!reactivate() || FAILED(client->GetMixFormat(&fmt))) {
                return false;
            }
        }
        if (!exclusive && !init_low_latency_shared(buffer_ms)) {
            if (!client) {
                return false;
            }
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                static_cast<REFERENCE_TIME>(buffer_ms) * 10000, 0, fmt, nullptr);
            if (FAILED(hr)) {
                xlog::warn("WASAPI: shared mode init failed (0x{:08X})", static_cast<unsigned>(hr));
                return false;
            }
        }

        if (FAILED(hr = client->SetEventHandle(event)) || FAILED(hr = client->GetBufferSize(&buffer_frames))
            || FAILED(hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)))) {
            xlog::warn("WASAPI: stream setup failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }

        // Hand the device silence before Start so the first period is not an underrun
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(buffer_frames, &data))) {
            render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (FAILED(hr = client->Start())) {
            xlog::warn("WASAPI: Start failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }

        xlog::info("WASAPI: {} on {} - {} Hz, {} ch, {} bit, buffer {:.1f} ms ({} frames){}",
            exclusive ? "exclusive" : "shared", name, fmt->nSamplesPerSec, fmt->nChannels, fmt->wBitsPerSample,
            1000.0 * buffer_frames / fmt->nSamplesPerSec, buffer_frames,
            period_frames ? std::format(", engine period {:.2f} ms", 1000.0 * period_frames / fmt->nSamplesPerSec)
                          : std::string{});
        return true;
    }

private:
    void read_device_name()
    {
        IPropertyStore* props = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
            return;
        }
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
            char buf[256];
            WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, buf, sizeof(buf), nullptr, nullptr);
            name = buf;
        }
        PropVariantClear(&value);
        props->Release();
    }

    // Exclusive mode: the buffer is also the device period, and the format must be one the hardware takes natively
    bool init_exclusive(int buffer_ms)
    {
        REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(buffer_ms) * 10000;
        REFERENCE_TIME default_period = 0;
        REFERENCE_TIME min_period = 0;
        if (SUCCEEDED(client->GetDevicePeriod(&default_period, &min_period)) && duration < min_period) {
            // Below the minimum every Initialize fails with AUDCLNT_E_INVALID_DEVICE_PERIOD
            xlog::info("WASAPI exclusive: raising {} ms to the device minimum {:.2f} ms", buffer_ms,
                min_period / 10000.0);
            duration = min_period;
        }

        struct Candidate
        {
            DWORD rate;
            WORD channels;
            WORD bits;
            WORD valid_bits;
            GUID sub_format;
        };
        const DWORD native_rate = fmt->nSamplesPerSec;
        const WORD native_channels = fmt->nChannels;
        const GUID pcm = KSDATAFORMAT_SUBTYPE_PCM;
        const GUID flt = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        // 24-in-32 is what most 24-bit USB/HDA drivers expose in exclusive mode
        const Candidate candidates[] = {
            {native_rate, native_channels, 32, 32, flt}, {native_rate, native_channels, 32, 24, pcm},
            {native_rate, native_channels, 24, 24, pcm}, {native_rate, native_channels, 16, 16, pcm},
            {native_rate, 2, 32, 32, flt},               {native_rate, 2, 32, 24, pcm},
            {native_rate, 2, 24, 24, pcm},               {native_rate, 2, 16, 16, pcm},
            {48000, 2, 32, 24, pcm},                     {48000, 2, 16, 16, pcm},
            {44100, 2, 32, 24, pcm},                     {44100, 2, 16, 16, pcm},
        };
        for (const auto& c : candidates) {
            WAVEFORMATEXTENSIBLE wfx{};
            wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wfx.Format.nChannels = c.channels;
            wfx.Format.nSamplesPerSec = c.rate;
            wfx.Format.wBitsPerSample = c.bits;
            wfx.Format.nBlockAlign = static_cast<WORD>(c.channels * c.bits / 8);
            wfx.Format.nAvgBytesPerSec = c.rate * wfx.Format.nBlockAlign;
            wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wfx.Samples.wValidBitsPerSample = c.valid_bits;
            wfx.dwChannelMask = channel_mask(c.channels);
            wfx.SubFormat = c.sub_format;

            HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration,
                duration, &wfx.Format, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 aligned = 0;
                client->GetBufferSize(&aligned);
                REFERENCE_TIME aligned_duration =
                    static_cast<REFERENCE_TIME>(10000000.0 * aligned / c.rate + 0.5);
                if (reactivate()) {
                    hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        aligned_duration, aligned_duration, &wfx.Format, nullptr);
                }
            }
            if (SUCCEEDED(hr)) {
                CoTaskMemFree(fmt);
                fmt = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(wfx)));
                std::memcpy(fmt, &wfx, sizeof(wfx));
                exclusive = true;
                return true;
            }
            xlog::debug("WASAPI exclusive: {} Hz {} ch {}/{} bit rejected (0x{:08X})", c.rate, c.channels, c.bits,
                c.valid_bits, static_cast<unsigned>(hr));
            if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED || !reactivate()) {
                return false;
            }
        }
        return false;
    }

    // Windows 10+: ask the shared engine for a period below its 10 ms default. False leaves the client
    // uninitialized (or null if recreating it failed) for the standard shared path.
    bool init_low_latency_shared(int buffer_ms)
    {
        IAudioClient3* client3 = nullptr;
        if (FAILED(client->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void**>(&client3)))) {
            return false;
        }
        UINT32 default_frames = 0;
        UINT32 fundamental = 0;
        UINT32 min_frames = 0;
        UINT32 max_frames = 0;
        HRESULT hr = client3->GetSharedModeEnginePeriod(fmt, &default_frames, &fundamental, &min_frames, &max_frames);
        const double rate = fmt->nSamplesPerSec;
        UINT32 period = 0;
        if (SUCCEEDED(hr) && fundamental && min_frames <= max_frames) {
            auto wanted = static_cast<UINT32>(rate * buffer_ms / 1000.0);
            period = std::clamp((wanted + fundamental - 1) / fundamental * fundamental, min_frames, max_frames);
            xlog::info("WASAPI shared: engine period default {:.2f} ms, minimum {:.2f} ms",
                1000.0 * default_frames / rate, 1000.0 * min_frames / rate);
        }
        if (!period || period >= default_frames) {
            // Driver lacks small periods, or the requested buffer is not below the default
            client3->Release();
            return false;
        }
        hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, fmt, nullptr);
        if (FAILED(hr)) {
            xlog::warn("WASAPI shared: low-latency init failed (0x{:08X})", static_cast<unsigned>(hr));
            client3->Release();
            if (!reactivate()) {
                client = nullptr;
            }
            return false;
        }
        // The engine runs at whichever period its first stream asked for, so report what is actually in effect
        WAVEFORMATEX* cur_fmt = nullptr;
        UINT32 cur = period;
        if (SUCCEEDED(client3->GetCurrentSharedModeEnginePeriod(&cur_fmt, &cur))) {
            CoTaskMemFree(cur_fmt);
        }
        client3->Release();
        period_frames = cur;
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------------------------------------------

static std::thread g_audio_thread;
// Exit paths that skip snd_term would destroy a joinable std::thread at DLL unload (std::terminate). By then
// ExitProcess has already killed the thread, so detaching is safe. Declared after g_audio_thread: destroyed first.
static struct AudioThreadExitGuard
{
    ~AudioThreadExitGuard()
    {
        if (g_audio_thread.joinable()) {
            g_audio_thread.detach();
        }
    }
} g_audio_thread_exit_guard;
static HANDLE g_wake_event = nullptr; // auto-reset; signalled for quit and reconfigure
static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_cfg_exclusive{false};
static std::atomic<int> g_cfg_buffer_ms{10};
static std::atomic<int> g_render_passes{0};

enum class RenderEnd
{
    woken,
    device_lost,
};

static RenderEnd render_to_device(OutputDevice& dev)
{
    const int rate = static_cast<int>(dev.fmt->nSamplesPerSec);
    g_output_rate = rate;
    g_output_chunk_frames = static_cast<int>(dev.buffer_frames);
    std::vector<float> mix(static_cast<size_t>(dev.buffer_frames) * 2);
    HANDLE waits[2] = {dev.event, g_wake_event};
    int silent_waits = 0;

    while (true) {
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, 250);
        if (r == WAIT_OBJECT_0 + 1) {
            return RenderEnd::woken;
        }
        if (r != WAIT_OBJECT_0) {
            // An exclusive stream whose device was grabbed or unplugged can simply stop signalling
            if (++silent_waits >= 8) {
                xlog::warn("WASAPI: device stopped requesting audio");
                return RenderEnd::device_lost;
            }
            continue;
        }
        silent_waits = 0;

        UINT32 frames = dev.buffer_frames;
        HRESULT hr = S_OK;
        if (!dev.exclusive) {
            UINT32 padding = 0;
            hr = dev.client->GetCurrentPadding(&padding);
            if (SUCCEEDED(hr)) {
                frames -= padding;
            }
        }
        BYTE* data = nullptr;
        if (SUCCEEDED(hr) && frames > 0) {
            hr = dev.render->GetBuffer(frames, &data);
        }
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            xlog::warn("WASAPI: output device lost");
            return RenderEnd::device_lost;
        }
        if (FAILED(hr) || !data) {
            continue;
        }
        mix_buffers(mix.data(), frames, rate);
        write_output(data, mix.data(), frames, dev.fmt);
        dev.render->ReleaseBuffer(frames, 0);
        g_render_passes.fetch_add(1, std::memory_order_relaxed);
    }
}

// With no device, keep buffers advancing in real time: one-shots finish, streams keep refilling and the game never
// waits on a frozen play cursor. Returns early when woken.
static void render_to_nowhere(int duration_ms)
{
    constexpr int rate = 48000;
    constexpr uint32_t max_frames = rate / 50;
    g_output_rate = rate;
    g_output_chunk_frames = static_cast<int>(max_frames);
    std::vector<float> scratch(max_frames * 2);
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto last = start;
    double carry = 0.0;
    while (clock::now() - start < std::chrono::milliseconds{duration_ms}) {
        if (WaitForSingleObject(g_wake_event, 10) == WAIT_OBJECT_0) {
            return;
        }
        const auto now = clock::now();
        carry += std::chrono::duration<double>(now - last).count() * rate;
        last = now;
        auto frames = static_cast<uint32_t>(std::min<double>(carry, max_frames));
        carry -= frames;
        if (frames) {
            mix_buffers(scratch.data(), frames, rate);
        }
    }
}

static void audio_thread_main(std::promise<bool> first_open)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
    bool reported = false;

    while (!g_quit) {
        OutputDevice dev;
        const bool want_exclusive = g_cfg_exclusive;
        const int buffer_ms = g_cfg_buffer_ms;
        const bool ok = dev.open(want_exclusive, buffer_ms);
        if (!reported) {
            reported = true;
            first_open.set_value(ok);
            if (!ok) {
                break; // the caller falls back to real DirectSound
            }
        }

        if (ok) {
            WasapiStatus status;
            status.device_open = true;
            status.exclusive = dev.exclusive;
            status.exclusive_requested = want_exclusive;
            status.device_name = dev.name;
            status.sample_rate = static_cast<int>(dev.fmt->nSamplesPerSec);
            status.channels = dev.fmt->nChannels;
            status.bits = dev.fmt->wBitsPerSample;
            status.buffer_frames = static_cast<int>(dev.buffer_frames);
            status.period_frames = static_cast<int>(dev.period_frames);
            set_status(status);

            const RenderEnd end = render_to_device(dev);
            dev.close();
            set_status({});
            if (end == RenderEnd::woken) {
                continue; // quit or reconfigure
            }
        }
        // Device lost or unavailable: keep time moving and retry, picking up a new default device if there is one
        render_to_nowhere(2000);
    }

    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------------------------

IDirectSound* wasapi_create_direct_sound(bool exclusive, int buffer_ms)
{
    if (g_audio_thread.joinable()) {
        return nullptr;
    }
    g_cfg_exclusive = exclusive;
    g_cfg_buffer_ms = buffer_ms;
    g_quit = false;
    g_wake_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake_event) {
        return nullptr;
    }

    std::promise<bool> first_open;
    std::future<bool> opened = first_open.get_future();
    g_audio_thread = std::thread{audio_thread_main, std::move(first_open)};
    if (opened.wait_for(std::chrono::seconds{5}) != std::future_status::ready || !opened.get()) {
        wasapi_shutdown();
        return nullptr;
    }
    return new WasapiDirectSound;
}

void wasapi_shutdown()
{
    if (!g_audio_thread.joinable()) {
        return;
    }
    g_quit = true;
    SetEvent(g_wake_event);
    g_audio_thread.join();
    CloseHandle(g_wake_event);
    g_wake_event = nullptr;

    // Drop the mixer's references; the game has released its own by now (snd_ds_close)
    std::vector<WasapiBuffer*> active;
    {
        std::lock_guard lock{g_mix_mutex};
        active.swap(g_active_buffers);
        for (WasapiBuffer* buf : active) {
            buf->set_inactive();
        }
    }
    for (WasapiBuffer* buf : active) {
        buf->Release();
    }
    xlog::info("WASAPI: shut down");
}

bool wasapi_is_active()
{
    return g_audio_thread.joinable();
}

void wasapi_configure(bool exclusive, int buffer_ms)
{
    if (!wasapi_is_active() || (g_cfg_exclusive == exclusive && g_cfg_buffer_ms == buffer_ms)) {
        return;
    }
    g_cfg_exclusive = exclusive;
    g_cfg_buffer_ms = buffer_ms;
    // Only the device is reopened; buffers and their playback positions carry over
    SetEvent(g_wake_event);
}

WasapiStatus wasapi_get_status()
{
    std::lock_guard lock{g_status_mutex};
    return g_status;
}

int wasapi_get_playing_buffer_count()
{
    std::lock_guard lock{g_mix_mutex};
    return static_cast<int>(g_active_buffers.size());
}

int wasapi_get_render_passes()
{
    return g_render_passes.load(std::memory_order_relaxed);
}
