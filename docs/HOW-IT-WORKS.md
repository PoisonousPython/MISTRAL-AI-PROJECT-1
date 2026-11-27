# How the governor works, start to finish

This page explains what the code does at every timescale it operates on, and shows
the raw code that runs at each step. You can understand the whole system by reading
this — no build or run required.

One honest framing first: nothing in it runs per CPU clock cycle — an injected DLL
can't and shouldn't work at that granularity. Its finest regular beat is
**per frame**, then per sweep, then per reload.

The full loop in one line:

**inject → attach → configure → gate on `enabled` → hook the creation functions →
hook the live present calls → pace every frame → cap CPU/memory in the job →
re-sweep threads every 250 ms → reload or tear down.**

The two heat paths — frame rate and CPU work — are governed by step 5 (per frame)
and steps 6–7 (per sweep); everything else runs once.

---

## 1 — Injection (outside the game, before anything runs)

The injector creates the game **suspended**, injects, then resumes it. The game
never knows it was paused; by the time its first instruction runs, the governor is
already inside.

```cpp
        if (!CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                            nullptr, nullptr, &si, &pi)) {
            std::fwprintf(stderr, L"CreateProcessW failed (err=%lu)\n", GetLastError());
            return 1;
        }
```

```cpp
    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        std::fwprintf(stderr, L"VirtualAllocEx failed (err=%lu)\n", GetLastError());
        return false;
    }
    if (!WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
        std::fwprintf(stderr, L"WriteProcessMemory failed (err=%lu)\n", GetLastError());
        return false;
    }
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW")));
    if (!load_library) {
        std::fwprintf(stderr, L"LoadLibraryW not found in kernel32\n");
        return false;
    }
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
```

```cpp
        std::wprintf(L"ok, resuming\n");
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
```

## 2 — DLL attach (the game's process, one time)

Windows calls `DllMain` when `LoadLibraryW` lands. It sets up logging and the 1 ms
timer resolution, then spawns one worker thread. The game's main thread is never
blocked.

```cpp
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
    } else if (reason == DLL_PROCESS_DETACH) {
        ::timeEndPeriod(1);
        tgov::ShutdownAll();
    }
    return TRUE;
}
```

## 3 — Worker startup: read config, apply rates, gate on `enabled`

The worker's first act is `ReloadNow()`. It parses `thermal_governor.ini` from the
DLL's folder:

```cpp
std::string ConfigPath() {
    const std::string dir = SelfDirectory();
    return dir + "\\thermal_governor.ini";
}
```

The parser lives in `src/core/config.cpp`; here is the start of `ReloadNow` in
`src/runtime/governor.cpp`:

```cpp
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
```

Each INI value goes through the parser with its clamp, e.g. the CPU cap
(negative → 0 = no cap, never above 10000 = 100%):

```cpp
    const double cpu_rate =
        ParseNumber(kv["cpu.cpu_rate_permille"], static_cast<double>(out.cpu_rate_permille));
    out.cpu_rate_permille = cpu_rate < 0.0 ? 0u : static_cast<unsigned>(cpu_rate);
    if (out.cpu_rate_permille > 10000) out.cpu_rate_permille = 10000;
```

Then the four pacer objects get their target interval — per-API limit if set, else
the global one:

```cpp
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
```

The fps number becomes a time interval (`SetRate` runs at startup and every reload):

```cpp
void Pacer::SetRate(double fps) {
    std::lock_guard<std::mutex> lock(mu_);
    interval_ = fps > 0.0 ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(1.0 / fps))
                          : std::chrono::steady_clock::duration{0};
    last_ = std::chrono::steady_clock::time_point{};
}
```

Then the single gate — everything below only happens when the config says enabled:

```cpp
void InstallDxgiHooks() {
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
}
```

## 4 — Hook installation (per API, at startup / on enable)

Each installer IAT-hooks the game's *creation* functions across every loaded
module. This is the DXGI set — the path that covers D3D10, D3D11, and D3D12:

```cpp
void InstallDxgiHooks() {
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
}
```

The IAT hook itself walks each module's own PE import tables — no byte patching,
only import-slot redirection, each write protected by its own `VirtualProtect`:

```cpp
        uint8_t* image = reinterpret_cast<uint8_t*>(modules[m]);
        auto slots = pe::FindIatSlots(image, module_name, function_name);
        for (void** slot : slots) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(slot, &mbi, sizeof(mbi))) continue;
            if (mbi.State != MEM_COMMIT) continue;

            void* orig = nullptr;
            DWORD old_protect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) continue;
            const unsigned n = pe::OverwriteIatSlots({slot}, detour, &orig);
            DWORD restore = 0;
            VirtualProtect(slot, sizeof(void*), old_protect, &restore);
            if (n == 0) continue;

            if (orig && original_out && *original_out == nullptr) *original_out = orig;
            ++total;
            Logf("iat: %s!%s in %s (orig=%p)", module_name, function_name, path, orig);
        }
```

When the game then calls `CreateDXGIFactory…`/`D3D11CreateDeviceAndSwapChain`, the
hook lets the real call through and grabs the swap chain it produced:

```cpp
HRESULT STDMETHODCALLTYPE HookFCreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                               DXGI_SWAP_CHAIN_DESC* desc,
                                               IDXGISwapChain** out) {
    const HRESULT hr = RealFactoryCreateSwapChain(self, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapchain(*out);
    return hr;
```

and writes our detour directly into that live COM object's vtable, slot 8
(`Present`):

```cpp
void HookSwapchain(IDXGISwapChain* swapchain) {
    if (!swapchain) return;
    VtableHook(swapchain, kSwapChain_Present, reinterpret_cast<void*>(&HookSwapchainPresent),
               reinterpret_cast<void**>(&RealPresent));

    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(__uuidof(IDXGISwapChain1),
                                            reinterpret_cast<void**>(&sc1)))) {
        VtableHook(sc1, kSwapChain1_Present1, reinterpret_cast<void*>(&HookSwapchainPresent1),
                   reinterpret_cast<void**>(&RealPresent1));
        sc1->Release();
    }
    Logf("dxgi: swapchain %p hooked", swapchain);
}
```

```cpp
void VtableHook(void* com_instance, unsigned index, void* detour, void** original_out) {
    void** vtable = *reinterpret_cast<void***>(com_instance);
    void** slot = &vtable[index];
    if (*slot == detour) return;

    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) return;
    if (original_out) *original_out = *slot;
    *slot = detour;
    DWORD restore = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &restore);

    Logf("vtable: obj=%p slot=%u (orig=%p)", com_instance, index,
         original_out ? *original_out : nullptr);
}
```

D3D9 works the same way — `Direct3DCreate9` is IAT-hooked, then the device's
`Present` (slot 17) and `PresentEx` (slot 121) are vtable-hooked:

```cpp
void HookDevice(IDirect3DDevice9* device) {
    IDirect3DDevice9Ex* ex = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                                         reinterpret_cast<void**>(&ex)))) {
        VtableHook(ex, kDevice_PresentEx, reinterpret_cast<void*>(&HookPresentEx),
                   reinterpret_cast<void**>(&RealPresentEx));
        ex->Release();
    }
    VtableHook(device, kDevice_Present, reinterpret_cast<void*>(&HookPresent),
               reinterpret_cast<void**>(&RealPresent));
}
```

## 5 — The hot path: every single frame, forever

This is the "clock cycle" of the project. Every present call from the game now
passes through the detour first, then the real GPU call:

```cpp
HRESULT STDMETHODCALLTYPE HookSwapchainPresent(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    PacerDxgi().Wait();
    return RealPresent(self, OverrideSyncInterval(sync_interval), flags);
}
```

```cpp
HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst,
                                      HWND window, const RGNDATA* dirty, DWORD flags) {
    PacerD3D9().Wait();
    return RealPresent(self, src, dst, window, dirty, flags);
}
```

```cpp
BOOL WINAPI HookWglSwapBuffers(HDC dc) {
    PacerOpenGl().Wait();
    return RealWglSwapBuffers(dc);
}
```

```cpp
VkResult TGOV_VKCALL HookQueuePresent(VkQueue queue, const void* info) {
    if (PaceOnPresent()) PacerVulkan().Wait();
    return RealQueuePresent(queue, info);
}
```

`Wait()` is the pacing decision. It compares now against the next scheduled slot;
early → book the slot and sleep; late (a stutter) → reset the cadence to now. When
no fps limit is configured, `Wait()` returns after one comparison — the hook cost
is a lock, a compare, and an unlock, with no sleep:

```cpp
void Pacer::Wait() {
    if (interval_.count() == 0) return;
    std::chrono::steady_clock::time_point wake;
    bool sleep_needed = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        if (last_.time_since_epoch().count() == 0) {
            last_ = now;
            return;
        }
        wake = last_ + interval_;
        if (now < wake) {
            last_ = wake;
            sleep_needed = true;
        } else {
            last_ = now;
        }
    }
    if (sleep_needed) PreciseSleep(wake - std::chrono::steady_clock::now());
}
```

The sleep itself is coarse first, spin-accurate at the end — that is what makes a
60/144 fps limit land on time without burning a whole core:

```cpp
void Pacer::PreciseSleep(std::chrono::steady_clock::duration remaining) {
    using clock = std::chrono::steady_clock;
    const auto spin = std::chrono::milliseconds(spin_ms_);
    const auto deadline = clock::now() + remaining;
    for (;;) {
        auto left = deadline - clock::now();
        if (left <= std::chrono::nanoseconds{0}) return;
#if defined(_WIN32)
        if (left > spin) {
            Sleep(1);
        } else {
            std::this_thread::yield();
        }
#else
        std::this_thread::sleep_for(left > spin ? spin : std::chrono::microseconds{50});
#endif
    }
}
```

## 6 — Scheduler start (once, after the gate)

Priority, power plan, then the Job Object that owns CPU and memory:

```cpp
void SchedulerStart() {
    RegisterCurrentThread();
    g_thread_mask = BuildThreadMask();
    ApplyPriorityClass();
    ApplyPowerPlan();
    ApplyJobLimits();

    if (g_thread_mask) {
        SweepThreads();
        Logf("sched: thread affinity mask 0x%llx applied (initial sweep)", g_thread_mask);
    } else {
        Logf("sched: no cpu scheduling limits requested");
    }
}
```

The affinity mask either comes from the config or is derived from "reserve the
lowest N cores":

```cpp
unsigned long long BuildThreadMask() {
    const Config& c = Cfg();
    if (c.affinity_mask) return c.affinity_mask;
    if (c.reserve_core_count) {
        unsigned long long all = 0;
        DWORD_PTR pm = 0;
        DWORD_PTR sm = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) && pm) {
            all = static_cast<unsigned long long>(pm);
        } else {
            all = ~0ULL;
        }
        const unsigned n = c.reserve_core_count;
        if (n >= 64) return 0;
        const unsigned long long reserved = (1ULL << n) - 1ULL;
        const unsigned long long mask = all & ~reserved;
        return mask ? mask : all;
    }
    return 0;
}
```

Priority class and power plan are single Windows calls:

```cpp
    if (SetPriorityClass(GetCurrentProcess(), cls)) {
        Logf("sched: priority class set to %s", p.c_str());
    } else {
        Logf("sched: SetPriorityClass failed (err=%lu)", GetLastError());
    }
```

```cpp
    if (PowerSetActiveScheme(nullptr, &plan)) {
        Logf("sched: power plan applied '%s'", p.c_str());
    } else {
        Logf("sched: PowerSetActiveScheme failed (err=%lu) - admin rights may be required",
             GetLastError());
    }
```

The Job Object is created, the whole game process is pushed into it, and the two
thermal levers are set:

```cpp
    if (!g_job) {
        g_job = CreateJobObjectA(nullptr, "ThermalGovernor");
        if (!g_job) {
            Logf("sched: CreateJobObject failed (err=%lu)", GetLastError());
            return;
        }
        if (!AssignProcessToJobObject(g_job, GetCurrentProcess())) {
            Logf("sched: AssignProcessToJobObject failed (err=%lu) - process may already be in an incompatible job", GetLastError());
            CloseHandle(g_job);
            g_job = nullptr;
            return;
        }
        Logf("sched: process assigned to governor job object");
    }

    if (c.commit_limit_mb) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
        ext.ProcessMemoryLimit = c.commit_limit_mb * 1024ULL * 1024ULL;
        ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &ext, sizeof(ext))) {
            Logf("sched: commit limit failed (err=%lu)", GetLastError());
        } else {
            Logf("sched: commit limit set to %llu MB", c.commit_limit_mb);
        }
    } else {
        ClearCommitLimit();
    }

    if (c.cpu_rate_permille) {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
        rate.CpuRate = c.cpu_rate_permille;
        rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        if (!SetInformationJobObject(g_job, JobObjectCpuRateControlInformation, &rate, sizeof(rate))) {
            const DWORD err = GetLastError();
            Logf("sched: cpu rate cap failed (err=%lu) - requires Windows 8+", err);
        } else {
            Logf("sched: cpu rate hard cap set to %.1f%%", c.cpu_rate_permille / 100.0);
        }
    } else {
        ClearCpuRateCap();
    }
```

## 7 — The steady loop: every 250 ms

The worker sleeps, wakes, and re-clamps any *new* threads the game spawned
(loaders, streaming workers, shader compilers):

```cpp
    for (;;) {
        const DWORD wait = c.reapply_thread_sched ? c.thread_sweep_ms : 1000;
        if (WaitForSingleObjectEx(g_stop_event, wait, FALSE) != WAIT_TIMEOUT) break;
        SchedulerSweep();
    }
    return 0;
```

```cpp
void SchedulerSweep() {
    if (!Cfg().reapply_thread_sched) return;
    if (!g_thread_mask) return;
    SweepThreads();
}
```

```cpp
void SweepThreads() {
    if (!g_thread_mask) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (IsOwnThread(te.th32ThreadID)) continue;
            HANDLE t = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (!t) continue;
            ApplyAffinityToThread(t);
            CloseHandle(t);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}
```

## 8 — Reload, disable, and exit

An external tool can call the exported reload entry point at any time; it re-runs
steps 3–6 safely (hook installation is idempotent, and any limit set to 0 is
actively cleared):

```cpp
TGOV_DLLEXPORT void GovernorReload() { tgov::ReloadNow(); }
```

Disable/shutdown clears the caps while the job handle still exists, then releases
it:

```cpp
void SchedulerShutdown() {
    ClearJobCaps();
    if (g_job) {
        CloseHandle(g_job);
        g_job = nullptr;
    }
}
```

```cpp
void ClearCpuRateCap() {
    if (!g_job) return;
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
    if (QueryInformationJobObject(g_job, JobObjectCpuRateControlInformation, &rate, sizeof(rate),
                                  nullptr) &&
        (rate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE)) {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION off{};
        if (SetInformationJobObject(g_job, JobObjectCpuRateControlInformation, &off, sizeof(off))) {
            Logf("sched: cpu rate cap removed");
        }
    }
}
```

On process exit, everything is torn down in order — worker joined, caps cleared,
log closed, timer resolution restored:

```cpp
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
```

---

## What it deliberately does not do

- No per-instruction/per-clock work, no byte patching, no detour trampolines —
  IAT and COM vtable writes only, each `VirtualProtect` scoped to one 8-byte slot.
- No per-allocation memory placement. In a closed binary you cannot redirect
  individual heap allocations to "fast/slow memory" the way PS3 titles did at
  build time; the honest lever is the process-wide commit cap, and that is what
  is implemented.
- No per-thread priority juggling per frame — one process priority class, set
  once and on reload.

## Where the code lives

| Step | Module | Files |
| --- | --- | --- |
| 1 | INJECTOR | `tools/injector.cpp` |
| 2, 3, 8 | RUNTIME | `src/runtime/governor.cpp` |
| 3 | CONFIG | `src/core/config.cpp`, `src/core/tgov_types.h` |
| 4 | HOOK_PLUMBING + CAPTURE | `src/hooks/iat.cpp`, `src/hooks/hooks_*.cpp` |
| 5 | PACER + PRESENT_GATEWAY | `src/core/pacer.cpp`, `src/hooks/hooks_*.cpp` |
| 6, 7 | SCHEDULER | `src/scheduler/scheduler.cpp` |

The formal entity model behind this walkthrough is
[docs/ARCHITECTURE-EER.md](ARCHITECTURE-EER.md).
