// DirectInput filter. Far Cry 4 reads the keyboard through DirectInput 8 (not window
// messages), so to keep keys away from the game we hook IDirectInputDevice8's
// GetDeviceState / GetDeviceData. Used while flying: Space and Ctrl move you up/down
// instead of jumping (wingsuit/parachute) and crouching. FC4Hook itself reads keys with
// GetAsyncKeyState, which this doesn't touch.
#include "core.h"
#include <cstring>

namespace fc4 {

typedef HRESULT (STDMETHODCALLTYPE* GetDeviceState_t)(void* self, DWORD cb, void* data);
typedef HRESULT (STDMETHODCALLTYPE* GetDeviceData_t)(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags);
typedef HRESULT (WINAPI* DI8Create_t)(HINSTANCE, DWORD, const GUID&, void**, void*);

static GetDeviceState_t o_StateW = nullptr, o_StateA = nullptr;
static GetDeviceData_t  o_DataW  = nullptr, o_DataA  = nullptr;

static const uint8_t kBlocked[] = { 0x39 /*SPACE*/, 0x1D /*LCONTROL*/, 0x9D /*RCONTROL*/ };

static bool Blocking() {
    static uint64_t serial = ~0ull; static bool flying = false;
    if (StateSerial() != serial) { serial = StateSerial(); flying = GetState("cheat.flying") == "1"; }
    return flying && !g_menuOpen;
}

static void FilterState(DWORD cb, void* data) {
    if (cb != 256 || !data || !Blocking()) return;           // 256 bytes = keyboard state
    for (uint8_t k : kBlocked) ((uint8_t*)data)[k] = 0;
}

// DIDEVICEOBJECTDATA: dwOfs (DWORD) first. Keyboard offsets are DIK codes; mouse offsets are
// 0..19, so the blocked codes never match mouse data.
static void FilterData(DWORD cbObj, void* rgdod, DWORD* inOut) {
    if (!rgdod || !inOut || !*inOut || cbObj < sizeof(DWORD) || !Blocking()) return;
    uint8_t* base = (uint8_t*)rgdod;
    DWORD n = *inOut, w = 0;
    for (DWORD r = 0; r < n; ++r) {
        DWORD ofs = *(DWORD*)(base + r * cbObj);
        bool drop = false;
        for (uint8_t k : kBlocked) if (ofs == k) drop = true;
        if (drop) continue;
        if (w != r) memmove(base + w * cbObj, base + r * cbObj, cbObj);
        ++w;
    }
    *inOut = w;
}

static HRESULT STDMETHODCALLTYPE hk_StateW(void* s, DWORD cb, void* d) { HRESULT h = o_StateW(s, cb, d); if (SUCCEEDED(h)) FilterState(cb, d); return h; }
static HRESULT STDMETHODCALLTYPE hk_StateA(void* s, DWORD cb, void* d) { HRESULT h = o_StateA(s, cb, d); if (SUCCEEDED(h)) FilterState(cb, d); return h; }
static HRESULT STDMETHODCALLTYPE hk_DataW(void* s, DWORD c, void* r, DWORD* io, DWORD f) { HRESULT h = o_DataW(s, c, r, io, f); if (SUCCEEDED(h) && !(f & 1 /*DIGDD_PEEK*/)) FilterData(c, r, io); return h; }
static HRESULT STDMETHODCALLTYPE hk_DataA(void* s, DWORD c, void* r, DWORD* io, DWORD f) { HRESULT h = o_DataA(s, c, r, io, f); if (SUCCEEDED(h) && !(f & 1)) FilterData(c, r, io); return h; }

static const GUID kIID_DI8W = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID kIID_DI8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID kSysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

// IDirectInput8::CreateDevice is vtable slot 3; device GetDeviceState 9, GetDeviceData 10, Release 2
static bool HookFlavor(DI8Create_t create, const GUID& iid, GetDeviceState_t* oState, void* hkState,
                       GetDeviceData_t* oData, void* hkData, const char* name) {
    void* di = nullptr;
    if (FAILED(create(GetModuleHandleW(nullptr), 0x0800, iid, &di, nullptr)) || !di) return false;
    void** vt = *(void***)di;
    typedef HRESULT (STDMETHODCALLTYPE* CreateDevice_t)(void*, const GUID&, void**, void*);
    void* dev = nullptr;
    bool ok = false;
    if (SUCCEEDED(((CreateDevice_t)vt[3])(di, kSysKeyboard, &dev, nullptr)) && dev) {
        void** dvt = *(void***)dev;
        ok = CreateHook(dvt[9], hkState, (void**)oState) && CreateHook(dvt[10], hkData, (void**)oData);
        ((ULONG (STDMETHODCALLTYPE*)(void*))dvt[2])(dev);
    }
    ((ULONG (STDMETHODCALLTYPE*)(void*))vt[2])(di);
    LogL(ok ? LOG_DEBUG : LOG_WARN, "input: DirectInput8 %s keyboard hooks %s", name, ok ? "installed" : "FAILED");
    return ok;
}

bool InputInit() {
    HMODULE di8 = LoadLibraryW(L"dinput8.dll");
    auto create = di8 ? (DI8Create_t)GetProcAddress(di8, "DirectInput8Create") : nullptr;
    if (!create) { LogL(LOG_WARN, "input: dinput8.dll not available"); return false; }
    bool w = HookFlavor(create, kIID_DI8W, &o_StateW, (void*)hk_StateW, &o_DataW, (void*)hk_DataW, "W");
    // The A and W devices usually share code; if so the second hook reports "already created".
    bool a = HookFlavor(create, kIID_DI8A, &o_StateA, (void*)hk_StateA, &o_DataA, (void*)hk_DataA, "A");
    if (!o_StateA) o_StateA = o_StateW;
    if (!o_DataA) o_DataA = o_DataW;
    return w || a;
}

} // namespace fc4
