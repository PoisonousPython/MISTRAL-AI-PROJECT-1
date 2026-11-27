#include "tgov_win.h"

#include "tgov_types.h"

#include <string>
#include <timeapi.h>

namespace tgov {

namespace {

Config g_cfg;
bool g_initialized = false;
bool g_shutdown = false;
HANDLE g_worker = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_time_knob = nullptr;

Config& MutableCfg() { return g_cfg; }

}  // namespace

Config& Cfg() { return g_cfg; }

double FpsLimit(const char* api_key) {
    double per_api = 0.0;
    switch (api_key[0]) {
        case 'd':
            per_api = api_key[1] == '9' ? g_cfg.fps_d3d9 : g_cfg.fps_dxgi;
            break;
        case 'v':
            per_api = g_cfg.fps_vulkan;
            break;
        case 'o':
            per_api = g_cfg.fps_opengl;
            break;
        default:
            break;
    }
    return per_api > 0.0 ? per_api : g_cfg.fps_global;
}

Pacer& PacerD3D9() {
    static Pacer p;
    return p;
}
Pacer& PacerDxgi() {
    static Pacer p;
    return p;
}
Pacer& PacerVulkan() {
    static Pacer p;
    return p;
}
Pacer& PacerOpenGl() {
    static Pacer p;
    return p;
}

void ApplyAllPacerRates() {
    PacerD3D9().SetRate(FpsLimit("d9"));
    PacerDxgi().SetRate(FpsLimit("dx"));
    PacerVulkan().SetRate(FpsLimit("vk"));
    PacerOpenGl().SetRate(FpsLimit("gl"));
    PacerD3D9().SetSpinGuard(g_cfg.spin_guard_ms);
    PacerDxgi().SetSpinGuard(g_cfg.spin_guard_ms);
    PacerVulkan().SetSpinGuard(g_cfg.spin_guard_ms);
    PacerOpenGl().SetSpinGuard(g_cfg.spin_guard_ms);
}

static std::string ConfigPath() {
    std::string dir = SelfDirectory();
    return dir + "\\thermal_governor.ini";
}

void ReloadNow() {
    const std::string path = ConfigPath();
    if (LoadConfigFromIni(path, g_cfg)) {
        Logf("config: loaded %s", path.c_str());
    } else {
        Logf("config: '%s' not found or unreadable, using built-in defaults "
             "(enabled=false => governor idle)",
             path.c_str());
        g_cfg.enabled = false;
    }
    ApplyAllPacerRates();
    SchedulerStart();
}

static DWORD WINAPI WorkerThread(LPVOID) {
    ReloadNow();
    if (!g_cfg.enabled) {
        Logf("governor: disabled in config - pacing and scheduling stay off");
        g_initialized = false;
        return 0;
    }
    g_initialized = true;

    InstallD3D9Hooks();
    InstallDxgiHooks();
    InstallOpenGLHooks();
    InstallVulkanHooks();
    InstallVulkanLayerExports();

    Logf("governor: active (fps d3d9=%.1f dxgi=%.1f vulkan=%.1f opengl=%.1f, cpu cap=%u permille)",
         g_cfg.fps_d3d9 > 0 ? g_cfg.fps_d3d9 : g_cfg.fps_global,
         g_cfg.fps_dxgi > 0 ? g_cfg.fps_dxgi : g_cfg.fps_global,
         g_cfg.fps_vulkan > 0 ? g_cfg.fps_vulkan : g_cfg.fps_global,
         g_cfg.fps_opengl > 0 ? g_cfg.fps_opengl : g_cfg.fps_global, g_cfg.cpu_rate_permille);

    for (;;) {
        const DWORD wait = g_cfg.reapply_thread_sched ? g_cfg.thread_sweep_ms : 1000;
        if (WaitForSingleObjectEx(g_stop_event, wait, FALSE) != WAIT_TIMEOUT) break;
        SchedulerSweep();
    }
    return 0;
}

static void ShutdownAll() {
    if (g_stop_event) {
        SetEvent(g_stop_event);
        if (g_worker) {
            WaitForSingleObject(g_worker, 2000);
            CloseHandle(g_worker);
            g_worker = nullptr;
        }
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
    SchedulerShutdown();
    LogClose();
}

}  // namespace tgov

extern "C" {

TGOV_DLLEXPORT void GovernorReload() { tgov::ReloadNow(); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        tgov::InitSelfModule();
        const std::string dir = tgov::SelfDirectory();
        tgov::LogOpen((dir + "\\governor.log").c_str());
        tgov::Logf("governor: loaded into process...");
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        tgov::Logf("governor: host=%s", path);
        ::timeBeginPeriod(1);

        tgov::g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        tgov::g_worker = CreateThread(nullptr, 0, tgov::WorkerThread, nullptr, 0, nullptr);
        if (!tgov::g_worker) {
            tgov::Logf("governor: worker thread creation failed (err=%lu)", GetLastError());
        }
    } else if (reason == DLL_PROCESS_DETACH && tgov::g_worker != nullptr) {
        ::timeEndPeriod(1);
        tgov::ShutdownAll();
    }
    return TRUE;
}
}
