#include "tgov_win.h"

#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>

namespace tgov {

namespace {

constexpr unsigned kSwapChain_Present = 8;
constexpr unsigned kSwapChain1_Present1 = 22;
constexpr unsigned kFactory_CreateSwapChain = 10;
constexpr unsigned kFactory2_CreateSwapChainForHwnd = 15;
constexpr unsigned kFactory2_CreateSwapChainForCoreWindow = 16;
constexpr unsigned kFactory2_CreateSwapChainForComposition = 24;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, ID3D11DeviceContext**,
    D3D_FEATURE_LEVEL*);
typedef HRESULT(WINAPI* PFN_D3D10CreateDeviceAndSwapChain)(
    IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**, ID3D10Device**);
typedef HRESULT(WINAPI* PFN_D3D10CreateDeviceAndSwapChain1)(
    IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT,
    DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D10Device1**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT,
                                                 const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FactoryCreateSwapChain)(IDXGIFactory*, IUnknown*,
                                                               DXGI_SWAP_CHAIN_DESC*,
                                                               IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FactoryCreateSwapChainForHwnd)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FactoryCreateSwapChainForCoreWindow)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
    IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FactoryCreateSwapChainForComposition)(
    IDXGIFactory2*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
typedef void** Vtable;

PFN_CreateDXGIFactory RealCreateDXGIFactory = nullptr;
PFN_CreateDXGIFactory1 RealCreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 RealCreateDXGIFactory2 = nullptr;
PFN_D3D11CreateDeviceAndSwapChain RealD3D11CreateDeviceAndSwapChain = nullptr;
PFN_D3D10CreateDeviceAndSwapChain RealD3D10CreateDeviceAndSwapChain = nullptr;
PFN_D3D10CreateDeviceAndSwapChain1 RealD3D10CreateDeviceAndSwapChain1 = nullptr;

PFN_Present RealPresent = nullptr;
PFN_Present1 RealPresent1 = nullptr;
PFN_FactoryCreateSwapChain RealFactoryCreateSwapChain = nullptr;
PFN_FactoryCreateSwapChainForHwnd RealFactoryCreateSwapChainForHwnd = nullptr;
PFN_FactoryCreateSwapChainForCoreWindow RealFactoryCreateSwapChainForCoreWindow = nullptr;
PFN_FactoryCreateSwapChainForComposition RealFactoryCreateSwapChainForComposition = nullptr;

Vtable g_swapchain_vtable = nullptr;
Vtable g_swapchain1_vtable = nullptr;
Vtable g_factory_vtable = nullptr;
Vtable g_factory2_vtable = nullptr;

void HookFactory(void* factory);
void HookSwapchain(IDXGISwapChain* swapchain);

UINT OverrideSyncInterval(UINT sync_interval) {
    const int force = Cfg().dxgi_force_sync_interval;
    return force >= 0 ? static_cast<UINT>(force) : sync_interval;
}

HRESULT STDMETHODCALLTYPE HookSwapchainPresent(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    PacerDxgi().Wait();
    return RealPresent(self, OverrideSyncInterval(sync_interval), flags);
}

HRESULT STDMETHODCALLTYPE HookSwapchainPresent1(IDXGISwapChain1* self, UINT sync_interval,
                                                UINT flags,
                                                const DXGI_PRESENT_PARAMETERS* params) {
    PacerDxgi().Wait();
    return RealPresent1(self, OverrideSyncInterval(sync_interval), flags, params);
}

void HookSwapchain(IDXGISwapChain* swapchain) {
    if (!swapchain) return;
    VtableHook(swapchain, kSwapChain_Present, reinterpret_cast<void*>(&HookSwapchainPresent),
               reinterpret_cast<void**>(&RealPresent),
               reinterpret_cast<void***>(&g_swapchain_vtable));

    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(__uuidof(IDXGISwapChain1),
                                            reinterpret_cast<void**>(&sc1)))) {
        VtableHook(sc1, kSwapChain1_Present1, reinterpret_cast<void*>(&HookSwapchainPresent1),
                   reinterpret_cast<void**>(&RealPresent1),
                   reinterpret_cast<void***>(&g_swapchain1_vtable));
        sc1->Release();
    }
    Logf("dxgi: swapchain %p hooked", swapchain);
}

HRESULT STDMETHODCALLTYPE HookFCreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                               DXGI_SWAP_CHAIN_DESC* desc,
                                               IDXGISwapChain** out) {
    const HRESULT hr = RealFactoryCreateSwapChain(self, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapchain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookFCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device,
                                                      HWND window,
                                                      const DXGI_SWAP_CHAIN_DESC1* desc,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
                                                      IDXGIOutput* restrict_out,
                                                      IDXGISwapChain1** out) {
    const HRESULT hr =
        RealFactoryCreateSwapChainForHwnd(self, device, window, desc, full, restrict_out, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapchain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookFCreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* device,
                                                            IUnknown* window,
                                                            const DXGI_SWAP_CHAIN_DESC1* desc,
                                                            IDXGIOutput* restrict_out,
                                                            IDXGISwapChain1** out) {
    const HRESULT hr =
        RealFactoryCreateSwapChainForCoreWindow(self, device, window, desc, restrict_out, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapchain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookFCreateSwapChainForComposition(IDXGIFactory2* self,
                                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                                             IDXGIOutput* restrict_out,
                                                             IDXGISwapChain1** out) {
    const HRESULT hr = RealFactoryCreateSwapChainForComposition(self, desc, restrict_out, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapchain(*out);
    return hr;
}

void HookFactory(void* factory) {
    IDXGIFactory* f1 = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(factory)->QueryInterface(
            __uuidof(IDXGIFactory), reinterpret_cast<void**>(&f1)))) {
        VtableHook(f1, kFactory_CreateSwapChain, reinterpret_cast<void*>(&HookFCreateSwapChain),
                   reinterpret_cast<void**>(&RealFactoryCreateSwapChain),
                   reinterpret_cast<void***>(&g_factory_vtable));
        f1->Release();
    }
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(factory)->QueryInterface(
            __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2)))) {
        VtableHook(f2, kFactory2_CreateSwapChainForHwnd, reinterpret_cast<void*>(&HookFCreateSwapChainForHwnd),
                   reinterpret_cast<void**>(&RealFactoryCreateSwapChainForHwnd),
                   reinterpret_cast<void***>(&g_factory2_vtable));
        VtableHook(f2, kFactory2_CreateSwapChainForCoreWindow,
                   reinterpret_cast<void*>(&HookFCreateSwapChainForCoreWindow),
                   reinterpret_cast<void**>(&RealFactoryCreateSwapChainForCoreWindow),
                   reinterpret_cast<void***>(&g_factory2_vtable));
        VtableHook(f2, kFactory2_CreateSwapChainForComposition,
                   reinterpret_cast<void*>(&HookFCreateSwapChainForComposition),
                   reinterpret_cast<void**>(&RealFactoryCreateSwapChainForComposition),
                   reinterpret_cast<void***>(&g_factory2_vtable));
        f2->Release();
    }
    Logf("dxgi: factory %p hooked", factory);
}

HRESULT WINAPI HookCreateDXGIFactory(REFIID riid, void** out) {
    const HRESULT hr = RealCreateDXGIFactory(riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory(*out);
    return hr;
}

HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** out) {
    const HRESULT hr = RealCreateDXGIFactory1(riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory(*out);
    return hr;
}

HRESULT WINAPI HookCreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    const HRESULT hr = RealCreateDXGIFactory2(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) HookFactory(*out);
    return hr;
}

HRESULT WINAPI HookD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels, UINT num_levels, UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapchain, ID3D11Device** device,
    ID3D11DeviceContext** context, D3D_FEATURE_LEVEL* level_out) {
    const HRESULT hr = RealD3D11CreateDeviceAndSwapChain(
        adapter, driver_type, software, flags, feature_levels, num_levels, sdk_version, desc,
        swapchain, device, context, level_out);
    if (SUCCEEDED(hr) && swapchain && *swapchain) HookSwapchain(*swapchain);
    return hr;
}

HRESULT WINAPI HookD3D10CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D10_DRIVER_TYPE type,
                                                 HMODULE software, UINT flags, UINT sdk_version,
                                                 DXGI_SWAP_CHAIN_DESC* desc,
                                                 IDXGISwapChain** swapchain,
                                                 ID3D10Device** device) {
    const HRESULT hr = RealD3D10CreateDeviceAndSwapChain(adapter, type, software, flags,
                                                         sdk_version, desc, swapchain, device);
    if (SUCCEEDED(hr) && swapchain && *swapchain) HookSwapchain(*swapchain);
    return hr;
}

HRESULT WINAPI HookD3D10CreateDeviceAndSwapChain1(IDXGIAdapter* adapter, D3D10_DRIVER_TYPE type,
                                                  HMODULE software, UINT flags,
                                                  D3D10_FEATURE_LEVEL1 level, UINT sdk_version,
                                                  DXGI_SWAP_CHAIN_DESC* desc,
                                                  IDXGISwapChain** swapchain,
                                                  ID3D10Device1** device) {
    const HRESULT hr = RealD3D10CreateDeviceAndSwapChain1(adapter, type, software, flags, level,
                                                          sdk_version, desc, swapchain, device);
    if (SUCCEEDED(hr) && swapchain && *swapchain) HookSwapchain(*swapchain);
    return hr;
}

}  // namespace

unsigned InstallDxgiHooks() {
    unsigned n = 0;
    if (!RealCreateDXGIFactory) {
        n += HookIatAllModules("dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void*>(&HookCreateDXGIFactory),
                               reinterpret_cast<void**>(&RealCreateDXGIFactory));
    }
    if (!RealCreateDXGIFactory1) {
        n += HookIatAllModules("dxgi.dll", "CreateDXGIFactory1", reinterpret_cast<void*>(&HookCreateDXGIFactory1),
                               reinterpret_cast<void**>(&RealCreateDXGIFactory1));
    }
    if (!RealCreateDXGIFactory2) {
        n += HookIatAllModules("dxgi.dll", "CreateDXGIFactory2", reinterpret_cast<void*>(&HookCreateDXGIFactory2),
                               reinterpret_cast<void**>(&RealCreateDXGIFactory2));
    }
    if (!RealD3D11CreateDeviceAndSwapChain) {
        n += HookIatAllModules("d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                               reinterpret_cast<void*>(&HookD3D11CreateDeviceAndSwapChain),
                               reinterpret_cast<void**>(&RealD3D11CreateDeviceAndSwapChain));
    }
    if (!RealD3D10CreateDeviceAndSwapChain) {
        n += HookIatAllModules("d3d10.dll", "D3D10CreateDeviceAndSwapChain",
                               reinterpret_cast<void*>(&HookD3D10CreateDeviceAndSwapChain),
                               reinterpret_cast<void**>(&RealD3D10CreateDeviceAndSwapChain));
    }
    if (!RealD3D10CreateDeviceAndSwapChain1) {
        n += HookIatAllModules("d3d10_1.dll", "D3D10CreateDeviceAndSwapChain1",
                               reinterpret_cast<void*>(&HookD3D10CreateDeviceAndSwapChain1),
                               reinterpret_cast<void**>(&RealD3D10CreateDeviceAndSwapChain1));
    }
    if (n) Logf("dxgi: capture-path hooks installed (%u sites)", n);
    return n;
}

}  // namespace tgov
