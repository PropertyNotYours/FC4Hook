// DirectInput filter. Far Cry 4 reads keyboard and mouse through DirectInput 8 (not window
// messages), so to keep input away from the game we hook IDirectInputDevice8's
// GetDeviceState / GetDeviceData (all DirectInput devices share these, so this covers the
// mouse too). Used (1) while the menu is open: the game gets no keyboard/mouse input, and
// (2) while flying: Space/Ctrl/Shift are ours (up / down / fast) instead of jump, wingsuit and
// crouch. FC4Hook itself reads keys with GetAsyncKeyState and the menu uses window
// messages, which this doesn't touch.
#include "core.h"
#include <cstring>
#include <atomic>

namespace fc4 {

typedef HRESULT (STDMETHODCALLTYPE* GetDeviceState_t)(void* self, DWORD cb, void* data);
typedef HRESULT (STDMETHODCALLTYPE* GetDeviceData_t)(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags);
typedef HRESULT (WINAPI* DI8Create_t)(HINSTANCE, DWORD, const GUID&, void**, void*);

static GetDeviceState_t o_StateW = nullptr, o_StateA = nullptr;
static GetDeviceData_t  o_DataW  = nullptr, o_DataA  = nullptr;

// Keys the game doesn't get while flying: Space/Ctrl (we use them for up/down; the game would
// jump / open the wingsuit / crouch) and Shift (fast fly; the game opens the wingsuit with it).
static const uint8_t kFlyBlocked[] = { 0x39 /*SPACE*/, 0x1D /*LCONTROL*/, 0x9D /*RCONTROL*/, 0x2A /*LSHIFT*/, 0x36 /*RSHIFT*/ };

static bool Flying() {
    static uint64_t serial = ~0ull; static bool flying = false;
    if (StateSerial() != serial) { serial = StateSerial(); flying = GetState("cheat.flying") == "1"; }
    return flying;
}

// Which kind of device is `self`? Cached per device (IDirectInputDevice8::GetCapabilities, slot 3).
enum DevKind { DEV_OTHER = 0, DEV_KEYBOARD = 1, DEV_MOUSE = 2 };
static DevKind Kind(void* self) {
    static void* devs[16] = {}; static uint8_t kinds[16] = {};
    for (int i = 0; i < 16; ++i) if (devs[i] == self) return (DevKind)kinds[i];
    DWORD caps[11] = { sizeof(caps) };                         // DIDEVCAPS
    typedef HRESULT (STDMETHODCALLTYPE* GetCaps_t)(void*, void*);
    DevKind k = DEV_OTHER;
    if (SUCCEEDED(((GetCaps_t)(*(void***)self)[3])(self, caps))) {
        DWORD t = caps[2] & 0xFF;                              // DI8DEVTYPE_*
        k = t == 0x13 ? DEV_KEYBOARD : t == 0x12 ? DEV_MOUSE : DEV_OTHER;
    }
    for (int i = 0; i < 16; ++i) if (!devs[i]) { devs[i] = self; kinds[i] = (uint8_t)k; break; }
    return k;
}

// While the menu is open the game gets no keyboard or mouse input at all (it reads both
// through DirectInput, so blocking window messages isn't enough).
static void FilterState(void* self, DWORD cb, void* data) {
    if (!data) return;
    DevKind k = Kind(self);
    if (g_menuOpen) {
        if ((k == DEV_KEYBOARD && cb == 256) || (k == DEV_MOUSE && (cb == 16 || cb == 20))) memset(data, 0, cb);
        return;
    }
    if (k == DEV_KEYBOARD && cb == 256 && Flying())
        for (uint8_t key : kFlyBlocked) ((uint8_t*)data)[key] = 0;
}

// Buffered input (DIDEVICEOBJECTDATA: dwOfs, dwData, ...). With the menu open, presses and mouse
// movement are dropped but releases are kept, so nothing stays "held" after closing the menu.
static void FilterData(void* self, DWORD cbObj, void* rgdod, DWORD* inOut) {
    if (!rgdod || !inOut || !*inOut || cbObj < 2 * sizeof(DWORD)) return;
    DevKind k = Kind(self);
    bool menu = g_menuOpen, fly = k == DEV_KEYBOARD && Flying();
    if (k == DEV_OTHER || (!menu && !fly)) return;
    uint8_t* base = (uint8_t*)rgdod;
    DWORD n = *inOut, w = 0;
    for (DWORD r = 0; r < n; ++r) {
        DWORD ofs = *(DWORD*)(base + r * cbObj), val = *(DWORD*)(base + r * cbObj + 4);
        bool drop = false;
        if (menu) {
            if (k == DEV_KEYBOARD) drop = (val & 0x80) != 0;                       // key down
            else drop = ofs < 12 || (val & 0x80) != 0;                            // axes, button down
        } else {
            for (uint8_t key : kFlyBlocked) if (ofs == key) drop = true;
        }
        if (drop) continue;
        if (w != r) memmove(base + w * cbObj, base + r * cbObj, cbObj);
        ++w;
    }
    *inOut = w;
}

static HRESULT STDMETHODCALLTYPE hk_StateW(void* s, DWORD cb, void* d) { HRESULT h = o_StateW(s, cb, d); if (SUCCEEDED(h)) FilterState(s, cb, d); return h; }
static HRESULT STDMETHODCALLTYPE hk_StateA(void* s, DWORD cb, void* d) { HRESULT h = o_StateA(s, cb, d); if (SUCCEEDED(h)) FilterState(s, cb, d); return h; }
static HRESULT STDMETHODCALLTYPE hk_DataW(void* s, DWORD c, void* r, DWORD* io, DWORD f) { HRESULT h = o_DataW(s, c, r, io, f); if (SUCCEEDED(h) && !(f & 1 /*DIGDD_PEEK*/)) FilterData(s, c, r, io); return h; }
static HRESULT STDMETHODCALLTYPE hk_DataA(void* s, DWORD c, void* r, DWORD* io, DWORD f) { HRESULT h = o_DataA(s, c, r, io, f); if (SUCCEEDED(h) && !(f & 1)) FilterData(s, c, r, io); return h; }

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

// ------------------------------------------------------------------ XInput (gamepads)
// While the menu is open the game sees an idle pad; FC4Hook's menu (ImGui's own XInput read,
// done on our thread with PadPassThrough set) still gets the real one. After the menu closes,
// the pad stays idle for the game until every button is let go, so the open/close combo
// doesn't also make you sprint / melee.
struct XPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XPad pad; };
typedef DWORD (WINAPI* XGetState_t)(DWORD, XState*);
static XGetState_t o_XGet[3] = {};
static thread_local bool t_padPass = false;
static std::atomic<bool> s_padHeld{ false };      // block until released after the menu closes

void PadPassThrough(bool on) { t_padPass = on; }

static DWORD FilterPad(XGetState_t orig, DWORD user, XState* st) {
    DWORD r = orig(user, st);
    if (r != 0 || !st || t_padPass) return r;
    if (g_menuOpen) { s_padHeld = true; memset(&st->pad, 0, sizeof(st->pad)); return r; }
    if (s_padHeld) {
        bool any = st->pad.buttons || st->pad.lt > 30 || st->pad.rt > 30;
        if (any) memset(&st->pad, 0, sizeof(st->pad)); else s_padHeld = false;
    }
    return r;
}
static DWORD WINAPI hk_XGet0(DWORD u, XState* s) { return FilterPad(o_XGet[0], u, s); }
static DWORD WINAPI hk_XGet1(DWORD u, XState* s) { return FilterPad(o_XGet[1], u, s); }
static DWORD WINAPI hk_XGet2(DWORD u, XState* s) { return FilterPad(o_XGet[2], u, s); }

// Real pad state for FC4Hook itself (the open-menu combo).
bool PadRead(int user, uint16_t* buttons) {
    for (auto f : o_XGet) if (f) {
        XState st = {};
        if (f((DWORD)user, &st) != 0) return false;
        *buttons = st.pad.buttons;
        return true;
    }
    return false;
}

// Triggers 0..255 (left, right) of pad 0, for scripts (aim detection, vehicle boost).
bool PadTriggers(int user, int* lt, int* rt) {
    for (auto f : o_XGet) if (f) {
        XState st = {};
        if (f((DWORD)user, &st) != 0) return false;
        *lt = st.pad.lt; *rt = st.pad.rt;
        return true;
    }
    return false;
}

static void HookXInput() {
    static const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    void* hooks[] = { (void*)hk_XGet0, (void*)hk_XGet1, (void*)hk_XGet2 };
    void* done[3] = {};
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        HMODULE m = LoadLibraryW(dlls[i]);
        void* f = m ? (void*)GetProcAddress(m, "XInputGetState") : nullptr;
        if (!f) continue;
        bool dup = false; for (void* d : done) if (d == f) dup = true;
        if (dup) continue;
        if (CreateHook(f, hooks[i], (void**)&o_XGet[i])) { done[i] = f; ++n; }
    }
    LogL(n ? LOG_DEBUG : LOG_WARN, "input: XInput hooks %d", n);
}

bool InputInit() {
    HookXInput();
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
