#include "tgov_win.h"

#include <psapi.h>
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

HANDLE g_worker = nullptr;
HANDLE g_stop_event = nullptr;
DWORD g_module_count = 0;

DWORD CurrentModuleCount() {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return g_module_count;
    const DWORD count = needed / sizeof(HMODULE);
    return count < 1024 ? count : 1024;
}

DWORD WINAPI WorkerThread(LPVOID) {
    const Config& c = Cfg();
    if (c.enabled) {
        Logf("governor: active (fps d3d9=%.1f dxgi=%.1f vulkan=%.1f opengl=%.1f, cpu cap=%u permille)",
             c.fps_d3d9 > 0 ? c.fps_d3d9 : c.fps_global,
             c.fps_dxgi > 0 ? c.fps_dxgi : c.fps_global,
             c.fps_vulkan > 0 ? c.fps_vulkan : c.fps_global,
             c.fps_opengl > 0 ? c.fps_opengl : c.fps_global, c.cpu_rate_permille);
    } else {
        Logf("governor: disabled in config - pacing and scheduling stay off");
    }

    for (;;) {
        const DWORD wait = Cfg().reapply_thread_sched ? Cfg().thread_sweep_ms : 1000;
        if (WaitForSingleObjectEx(g_stop_event, wait, FALSE) != WAIT_TIMEOUT) break;
        SchedulerSweep();
        if (Cfg().enabled) {
            const DWORD count = CurrentModuleCount();
            if (count != g_module_count) {
                g_module_count = count;
                Logf("hooks: module set changed, rescanning imports");
                InstallD3D9Hooks();
                InstallDxgiHooks();
                InstallOpenGLHooks();
                InstallVulkanHooks();
                InstallVulkanLayerExports();
            }
        }
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

void ReloadNow();

void GovernorStart() {
    if (!g_stop_event) {
        LogOpen((SelfDirectory() + "\\governor.log").c_str());
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        Logf("governor: loaded into process, host=%s", path);
        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_stop_event) {
            Logf("governor: event creation failed (err=%lu)", GetLastError());
            return;
        }
        ::timeBeginPeriod(1);
        g_module_count = CurrentModuleCount();
    }
    ReloadNow();
    if (!g_worker) {
        g_worker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (!g_worker) {
            Logf("governor: worker thread creation failed (err=%lu)", GetLastError());
        }
    }
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
        InstallD3D9Hooks();
        InstallDxgiHooks();
        InstallOpenGLHooks();
        InstallVulkanHooks();
        InstallVulkanLayerExports();
        SchedulerStart();
    } else {
        SchedulerShutdown();
    }
}

}  // namespace

}  // namespace tgov

extern "C" {

TGOV_DLLEXPORT void GovernorReload() { tgov::GovernorStart(); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        tgov::InitSelfModule();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (tgov::g_stop_event) {
            ::timeEndPeriod(1);
            tgov::ShutdownAll();
        }
    }
    return TRUE;
}
}
