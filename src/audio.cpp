// Audio speed for slow motion. The game's SetSlowMotion only scales the game clock; its audio
// engine (Ubisoft "Dare" on XAudio2 2.7) keeps playing at normal speed. XAudio2 plays each
// sound through a "source voice", and a voice's frequency ratio is its playback speed (and
// pitch). So FC4Hook hooks the source-voice code inside xaudio2_7.dll, remembers every voice
// the game uses and the ratio the game wants for it (doppler etc.), and plays it at
// wanted ratio x AudioSpeed. AudioSpeed 1 = untouched.
//
// The vtables come from a throwaway XAudio2 instance of our own: the functions live in
// xaudio2_7.dll and are shared by every instance, so hooking them covers the game's too, no
// matter whether its engine was created before or after us.
#include "core.h"
#include <objbase.h>
#include <mmreg.h>
#include <map>
#include <mutex>
#include <atomic>

namespace fc4 {

static const GUID kCLSID_XAudio2      = { 0x5a508685, 0xa254, 0x4fba, { 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf } };
static const GUID kCLSID_XAudio2Debug = { 0xdb05ea35, 0x0329, 0x4d4b, { 0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52 } };
static const GUID kIID_IXAudio2       = { 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb } };

// vtable slots (XAudio2 2.7)
enum { X2_Initialize = 5, X2_CreateSourceVoice = 8, X2_CreateMasteringVoice = 10 };
enum { V_DestroyVoice = 18, V_Start = 19, V_SubmitSourceBuffer = 21, V_SetFrequencyRatio = 26 };

typedef HRESULT (STDMETHODCALLTYPE* X2Init_t)(void*, UINT32, UINT32);
typedef HRESULT (STDMETHODCALLTYPE* X2CreateSrc_t)(void*, void**, const WAVEFORMATEX*, UINT32, float, void*, const void*, const void*);
typedef HRESULT (STDMETHODCALLTYPE* X2CreateMaster_t)(void*, void**, UINT32, UINT32, UINT32, UINT32, const void*);
typedef void    (STDMETHODCALLTYPE* VDestroy_t)(void*);
typedef HRESULT (STDMETHODCALLTYPE* VStart_t)(void*, UINT32, UINT32);
typedef HRESULT (STDMETHODCALLTYPE* VSubmit_t)(void*, const void*, const void*);
typedef HRESULT (STDMETHODCALLTYPE* VSetFreq_t)(void*, float, UINT32);

static X2CreateSrc_t o_CreateSrc;
static VDestroy_t    o_Destroy;
static VStart_t      o_Start;
static VSubmit_t     o_Submit;
static VSetFreq_t    o_SetFreq;

struct VoiceInfo { float wanted = 1.f; bool noPitch = false; int gen = 0; };
static std::mutex s_mx;
static std::map<void*, VoiceInfo> s_voices;
static std::atomic<float> s_speed{ 1.f };
static std::atomic<bool> s_voiceHooks{ false };
static std::atomic<int> s_gen{ 0 };                  // bumped on every speed change
static thread_local bool t_inside = false;          // our own SetFrequencyRatio calls


static float Clamp(float r) { return r < 1.f / 1024 ? 1.f / 1024 : r; }

// Voices are only ever touched from the game's own calls on them (never from the Lua thread),
// so a voice can't be destroyed under us and we never call into XAudio2 across threads.
static VoiceInfo* Track(void* v) {                    // s_mx held
    auto it = s_voices.find(v);
    if (it != s_voices.end()) return &it->second;
    if (!o_Destroy || s_voices.size() >= 4096) return nullptr;
    return &s_voices[v];
}

// Bring a voice up to the current speed if it's behind.
static void Sync(void* v) {
    float r;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        VoiceInfo* vi = Track(v);
        int g = s_gen.load();
        if (!vi || vi->gen == g) return;
        vi->gen = g;
        if (vi->noPitch || !o_SetFreq) return;
        float sp = s_speed.load();
        r = sp == 1.f ? vi->wanted : Clamp(vi->wanted * sp);
    }
    t_inside = true;                          // our lock is released: never call XAudio2 holding it
    o_SetFreq(v, r, 0);
    t_inside = false;
}

static void STDMETHODCALLTYPE hk_Destroy(void* v) {
    { std::lock_guard<std::mutex> lk(s_mx); s_voices.erase(v); }
    o_Destroy(v);
}

static HRESULT STDMETHODCALLTYPE hk_Start(void* v, UINT32 flags, UINT32 op) {
    Sync(v);
    return o_Start(v, flags, op);
}

static HRESULT STDMETHODCALLTYPE hk_Submit(void* v, const void* buf, const void* wma) {
    Sync(v);
    return o_Submit(v, buf, wma);
}

static HRESULT STDMETHODCALLTYPE hk_SetFreq(void* v, float ratio, UINT32 op) {
    if (t_inside) return o_SetFreq(v, ratio, op);
    float sp = s_speed.load();
    {
        std::lock_guard<std::mutex> lk(s_mx);
        VoiceInfo* vi = Track(v);
        if (vi) { vi->wanted = ratio; vi->gen = s_gen.load(); }
    }
    return o_SetFreq(v, sp == 1.f ? ratio : Clamp(ratio * sp), op);
}

static void HookVoiceVtable(void* voice) {
    if (s_voiceHooks.exchange(true)) return;
    void** vt = *(void***)voice;
    int n = 0;
    n += CreateHook(vt[V_DestroyVoice], (void*)hk_Destroy, (void**)&o_Destroy) ? 1 : 0;
    n += CreateHook(vt[V_Start], (void*)hk_Start, (void**)&o_Start) ? 1 : 0;
    n += CreateHook(vt[V_SubmitSourceBuffer], (void*)hk_Submit, (void**)&o_Submit) ? 1 : 0;
    n += CreateHook(vt[V_SetFrequencyRatio], (void*)hk_SetFreq, (void**)&o_SetFreq) ? 1 : 0;
    LogL(n == 4 ? LOG_DEBUG : LOG_WARN, "audio: source voice hooks %d/4", n);
}

static HRESULT STDMETHODCALLTYPE hk_CreateSrc(void* x2, void** out, const WAVEFORMATEX* fmt, UINT32 flags, float maxRatio,
                                              void* cb, const void* sends, const void* fx) {
    HRESULT hr = o_CreateSrc(x2, out, fmt, flags, maxRatio, cb, sends, fx);
    if (SUCCEEDED(hr) && out && *out) {
        HookVoiceVtable(*out);
        {
            std::lock_guard<std::mutex> lk(s_mx);
            VoiceInfo* vi = Track(*out);
            if (vi) { vi->noPitch = (flags & 0x2) != 0; vi->gen = -1; }   // XAUDIO2_VOICE_NOPITCH
        }
        Sync(*out);
    }
    return hr;
}

// Run on our init thread. Makes a private XAudio2 2.7 engine just to find the vtables.
bool AudioInit() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    void* x2 = nullptr;
    HRESULT hr = CoCreateInstance(kCLSID_XAudio2, nullptr, CLSCTX_INPROC_SERVER, kIID_IXAudio2, &x2);
    if (FAILED(hr)) hr = CoCreateInstance(kCLSID_XAudio2Debug, nullptr, CLSCTX_INPROC_SERVER, kIID_IXAudio2, &x2);
    if (FAILED(hr) || !x2) { LogL(LOG_WARN, "audio: XAudio2 2.7 not available (0x%08lx); slow motion won't slow sound", hr); if (SUCCEEDED(co)) CoUninitialize(); return false; }
    void** vt = *(void***)x2;
    bool ok = CreateHook(vt[X2_CreateSourceVoice], (void*)hk_CreateSrc, (void**)&o_CreateSrc) != 0;
    // Also get a source voice of our own right away, so voices the game made before we
    // hooked are covered too (they're picked up the next time they play).
    if (SUCCEEDED(((X2Init_t)vt[X2_Initialize])(x2, 0, 0xFFFFFFFE /* XAUDIO2_DEFAULT_PROCESSOR */))) {
        void* master = nullptr;
        if (SUCCEEDED(((X2CreateMaster_t)vt[X2_CreateMasteringVoice])(x2, &master, 0, 0, 0, 0, nullptr)) && master) {
            WAVEFORMATEX wf = {}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = 44100;
            wf.wBitsPerSample = 16; wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 88200;
            void* src = nullptr;
            X2CreateSrc_t create = o_CreateSrc ? o_CreateSrc : (X2CreateSrc_t)vt[X2_CreateSourceVoice];
            if (SUCCEEDED(create(x2, &src, &wf, 0, 2.f, nullptr, nullptr, nullptr)) && src) {
                HookVoiceVtable(src);
                (o_Destroy ? o_Destroy : (VDestroy_t)(*(void***)src)[V_DestroyVoice])(src);
            }
            ((VDestroy_t)(*(void***)master)[V_DestroyVoice])(master);
        }
    }
    ((ULONG (STDMETHODCALLTYPE*)(void*))vt[2])(x2);          // Release
    if (SUCCEEDED(co)) CoUninitialize();
    LogL(ok ? LOG_DEBUG : LOG_WARN, "audio: XAudio2 hooks %s", ok ? "installed" : "FAILED");
    return ok;
}

// 1 = normal. Each voice picks it up the next time the game feeds or starts it: streamed sound
// (music, ambience, engines) within a fraction of a second, one-shot sounds when they next play.
void AudioSetSpeed(float speed) {
    if (!(speed > 0.f)) speed = 1.f;
    if (speed > 1.f) speed = 1.f;            // XAudio2 caps a voice's max ratio at creation; faster isn't safe
    if (speed == s_speed.load()) return;
    s_speed = speed;
    ++s_gen;
}

int AudioVoiceCount() { std::lock_guard<std::mutex> lk(s_mx); return (int)s_voices.size(); }

} // namespace fc4
