#include "tgov_win.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <psapi.h>

namespace tgov {

static HMODULE g_self = nullptr;
static FILE* g_log = nullptr;
static SRWLOCK g_log_lock = SRWLOCK_INIT;

static void SetSelfModule() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SetSelfModule), &h);
    g_self = h;
}

HMODULE SelfModule() { return g_self; }
void InitSelfModule() { SetSelfModule(); }

void LogOpen(const char* log_path) {
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    if (log_path && log_path[0]) {
        HANDLE h = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            g_log = _fdopen(_open_osfhandle(reinterpret_cast<intptr_t>(h), 0), "a");
        }
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

void LogClose() {
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

void Logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamped[1100];
    std::snprintf(stamped, sizeof(stamped), "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute,
                  st.wSecond, st.wMilliseconds, buf);

    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log) std::fputs(stamped, g_log);
    ReleaseSRWLockExclusive(&g_log_lock);
    OutputDebugStringA(stamped);
}

std::string SelfDirectory() {
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(SelfModule(), path, MAX_PATH)) return ".";
    std::string s(path);
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? "." : s.substr(0, slash);
}

unsigned HookIatAllModules(const char* module_name, const char* function_name,
                           void* detour, void** original_out) {
    if (original_out) *original_out = nullptr;

    HMODULE modules[1024] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return 0;
    const unsigned count = needed / sizeof(HMODULE);
    if (count == 0 || count > 1024) return 0;

    unsigned total = 0;
    for (unsigned m = 0; m < count; ++m) {
        if (!modules[m] || modules[m] == g_self) continue;
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(modules[m], path, MAX_PATH)) continue;
        if (strstr(path, "governor")) continue;

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
    }
    return total;
}

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

}  // namespace tgov
