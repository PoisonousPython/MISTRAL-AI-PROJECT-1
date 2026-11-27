#include "tgov_win.h"

#include <string>
#include <timeapi.h>

namespace tgov {

namespace {

std::string ConfigPath() {
    const std::string dir = SelfDirectory();
    return dir + "\\thermal_governor.ini";
}

double FpsLimit(const char* api_key) {
    const Config& c = Cfg();
    double per_api = 0.0;
    switch (api_key[0]) {
        case 'd':
            per_api = api_key[1] == '9' ? c.fps_d3d9 : c.fps_dxgi;
            break;
        case 'v':
            per_api = c.fps_vulkan;
            break;
        case 'o':
            per_api = c.fps_opengl;
            break;
        default:
            break;
    }
    return per_api > 0.0 ? per_api : c.fps_global;
}

void ApplyAllPacerRates() {
    PacerD3D9().SetRate(FpsLimit("d9"));
    PacerDxgi().SetRate(FpsLimit("dx"));
    PacerVulkan().SetRate(FpsLimit("vk"));
    PacerOpenGl().SetRate(FpsLimit("gl"));
    PacerD3D9().SetSpinGuard(Cfg().spin_guard_ms);
    PacerDxgi().SetSpinGuard(Cfg().spin_guard_ms);
    PacerVulkan().SetSpinGuard(Cfg().spin_guard_ms);
    PacerOpenGl().SetSpinGuard(Cfg().spin_guard_ms);
}

}  // namespace

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

void ReloadNow() {
    const std::string path = ConfigPath();
    if (LoadConfigFromIni(path, Cfg())) {
        Logf("config: loaded %s", path.c_str());
    } else {
        Logf("config: '%s' not found or unreadable, using built-in defaults "
             "(enabled=false => governor idle)",
             path.c_str());
        Cfg().enabled = false;
    }
    ApplyAllPacerRates();
    if (Cfg().enabled) {
        SchedulerStart();
    } else {
        SchedulerShutdown();
    }
}

namespace {

HANDLE g_worker = nullptr;
HANDLE g_stop_event = nullptr;

DWORD WINAPI WorkerThread(LPVOID) {
    ReloadNow();
    if (!Cfg().enabled) {
        Logf("governor: disabled in config - pacing and scheduling stay off");
        return 0;
    }

    InstallD3D9Hooks();
    InstallDxgiHooks();
    InstallOpenGLHooks();
    InstallVulkanHooks();
    InstallVulkanLayerExports();

    const Config& c = Cfg();
    Logf("governor: active (fps d3d9=%.1f dxgi=%.1f vulkan=%.1f opengl=%.1f, cpu cap=%u permille)",
         c.fps_d3d9 > 0 ? c.fps_d3d9 : c.fps_global,
         c.fps_dxgi > 0 ? c.fps_dxgi : c.fps_global,
         c.fps_vulkan > 0 ? c.fps_vulkan : c.fps_global,
         c.fps_opengl > 0 ? c.fps_opengl : c.fps_global, c.cpu_rate_permille);

    for (;;) {
        const DWORD wait = c.reapply_thread_sched ? c.thread_sweep_ms : 1000;
        if (WaitForSingleObjectEx(g_stop_event, wait, FALSE) != WAIT_TIMEOUT) break;
        SchedulerSweep();
    }
    return 0;
}

void ShutdownAll() {
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

}  // namespace

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
