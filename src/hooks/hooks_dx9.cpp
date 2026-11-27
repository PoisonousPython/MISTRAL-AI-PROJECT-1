#include "tgov_win.h"

#include <d3d9.h>

namespace tgov {

namespace {

constexpr unsigned kIDirect3D9_CreateDevice = 16;
constexpr unsigned kIDirect3D9Ex_CreateDeviceEx = 17;
constexpr unsigned kDevice_Present = 17;
constexpr unsigned kDevice_PresentEx = 121;

typedef IDirect3D9*(WINAPI* PFN_Direct3DCreate9)(UINT);
typedef HRESULT(WINAPI* PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDeviceEx)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
    D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*,
                                                HWND, const RGNDATA*, DWORD);
typedef void** Vtable;

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
HRESULT STDMETHODCALLTYPE HookCreateDeviceEx(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);

PFN_Direct3DCreate9 RealDirect3DCreate9 = nullptr;
PFN_Direct3DCreate9Ex RealDirect3DCreate9Ex = nullptr;
PFN_CreateDevice RealCreateDevice = nullptr;
PFN_CreateDeviceEx RealCreateDeviceEx = nullptr;
PFN_Present RealPresent = nullptr;
PFN_Present RealPresentEx = nullptr;
Vtable g_device_vtable = nullptr;
Vtable g_d3d9_vtable = nullptr;

void HookDevice(IDirect3DDevice9* device) {
    IDirect3DDevice9Ex* ex = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                                         reinterpret_cast<void**>(&ex)))) {
        VtableHook(ex, kDevice_PresentEx, reinterpret_cast<void*>(&HookPresentEx),
                   reinterpret_cast<void**>(&RealPresentEx),
                   reinterpret_cast<void***>(&g_device_vtable));
        ex->Release();
    }
    VtableHook(device, kDevice_Present, reinterpret_cast<void*>(&HookPresent),
               reinterpret_cast<void**>(&RealPresent),
               reinterpret_cast<void***>(&g_device_vtable));
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst,
                                      HWND window, const RGNDATA* dirty, DWORD flags) {
    PacerD3D9().Wait();
    return RealPresent(self, src, dst, window, dirty, flags);
}

HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* self, const RECT* src,
                                        const RECT* dst, HWND window, const RGNDATA* dirty,
                                        DWORD flags) {
    PacerD3D9().Wait();
    return RealPresentEx(self, src, dst, window, dirty, flags);
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type,
                                           HWND focus, DWORD behavior,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    const HRESULT hr = RealCreateDevice(self, adapter, type, focus, behavior, pp, out);
    if (SUCCEEDED(hr) && out && *out) HookDevice(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateDeviceEx(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type,
                                             HWND focus, DWORD behavior,
                                             D3DPRESENT_PARAMETERS* pp,
                                             D3DDISPLAYMODEEX* fullscreen,
                                             IDirect3DDevice9Ex** out) {
    const HRESULT hr =
        RealCreateDeviceEx(self, adapter, type, focus, behavior, pp, fullscreen, out);
    if (SUCCEEDED(hr) && out && *out) HookDevice(*out);
    return hr;
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk_version) {
    IDirect3D9* d3d = RealDirect3DCreate9(sdk_version);
    if (d3d) {
        VtableHook(d3d, kIDirect3D9_CreateDevice, reinterpret_cast<void*>(&HookCreateDevice),
                   reinterpret_cast<void**>(&RealCreateDevice),
                   reinterpret_cast<void***>(&g_d3d9_vtable));
    }
    return d3d;
}

HRESULT WINAPI HookDirect3DCreate9Ex(UINT sdk_version, IDirect3D9Ex** out) {
    const HRESULT hr = RealDirect3DCreate9Ex(sdk_version, out);
    if (SUCCEEDED(hr) && out && *out) {
        VtableHook(*out, kIDirect3D9_CreateDevice, reinterpret_cast<void*>(&HookCreateDevice),
                   reinterpret_cast<void**>(&RealCreateDevice),
                   reinterpret_cast<void***>(&g_d3d9_vtable));
        VtableHook(*out, kIDirect3D9Ex_CreateDeviceEx, reinterpret_cast<void*>(&HookCreateDeviceEx),
                   reinterpret_cast<void**>(&RealCreateDeviceEx),
                   reinterpret_cast<void***>(&g_d3d9_vtable));
    }
    return hr;
}

}  // namespace

unsigned InstallD3D9Hooks() {
    unsigned n = 0;
    if (!RealDirect3DCreate9) {
        n += HookIatAllModules("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&HookDirect3DCreate9),
                               reinterpret_cast<void**>(&RealDirect3DCreate9));
    }
    if (!RealDirect3DCreate9Ex) {
        n += HookIatAllModules("d3d9.dll", "Direct3DCreate9Ex", reinterpret_cast<void*>(&HookDirect3DCreate9Ex),
                               reinterpret_cast<void**>(&RealDirect3DCreate9Ex));
    }
    if (n) Logf("d3d9: creation-path hooks installed (%u sites)", n);
    return n;
}

}  // namespace tgov
