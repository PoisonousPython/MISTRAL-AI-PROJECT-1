#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef DWORD(__stdcall* FnWait)(HANDLE, DWORD);

struct Args {
    enum Mode { kAttach, kLaunch } mode = kAttach;
    std::wstring exe;
    std::wstring exe_args;
    std::wstring dll;
};

static bool ParseArgs(int argc, wchar_t** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--launch" && i + 1 < argc) {
            a.mode = Args::kLaunch;
            a.exe = argv[++i];
        } else if (arg == L"--args" && i + 1 < argc) {
            a.exe_args = argv[++i];
        } else if (arg == L"--pid" && i + 1 < argc) {
            a.mode = Args::kAttach;
            a.exe = argv[++i];
        } else {
            a.dll = arg;
        }
    }
    if (a.dll.empty()) return false;
    if (a.mode == Args::kLaunch && a.exe.empty()) return false;
    if (a.mode == Args::kAttach && a.exe.empty()) return false;
    return true;
}

static std::wstring AbsPathDll(const std::wstring& dll) {
    wchar_t full[MAX_PATH] = {};
    if (GetFullPathNameW(dll.c_str(), MAX_PATH, full, nullptr)) return full;
    return dll;
}

static bool InjectInto(HANDLE process, const std::wstring& dll) {
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
        std::fwprintf(stderr, L"LoadLibraryW not found in kernel32\n", GetLastError());
        return false;
    }
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
    if (!thread) {
        std::fwprintf(stderr, L"CreateRemoteThread failed (err=%lu)\n", GetLastError());
        return false;
    }
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) {
        std::fwprintf(stderr, L"remote LoadLibraryW did not complete in 10s\n");
        TerminateThread(thread, 1);
        CloseHandle(thread);
        return false;
    }
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    if (!exit_code) {
        std::fwprintf(stderr, L"LoadLibraryW returned NULL in the target\n");
        return false;
    }
    return true;
}

static DWORD PidFromName(const std::wstring& name) {
    if (std::wcsstr(name.c_str(), L".exe") && name.find(L'\\') == std::wstring::npos) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    Args a;
    if (!ParseArgs(argc, argv, a)) {
        std::wprintf(
            L"Thermal Governor injector\n"
            L"  governor_injector.exe --launch <game.exe> [--args \"...\"] <governor.dll>\n"
            L"  governor_injector.exe --pid <process-id-or-name.exe> <governor.dll>\n");
        return 2;
    }

    const std::wstring dll = AbsPathDll(a.dll);
    DWORD file_attr = GetFileAttributesW(dll.c_str());
    if (file_attr == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"DLL not found: %s\n", dll.c_str());
        return 1;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t cmdline[4096] = {};

    if (a.mode == Args::kLaunch) {
        std::wstring quoted = L"\"" + a.exe + L"\"";
        if (!a.exe_args.empty()) quoted += L" " + a.exe_args;
        std::wcsncpy(cmdline, quoted.c_str(), 4095);
        if (!CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                            nullptr, nullptr, &si, &pi)) {
            std::fwprintf(stderr, L"CreateProcessW failed (err=%lu)\n", GetLastError());
            return 1;
        }
        std::wprintf(L"launched %s (pid=%lu)\n", a.exe.c_str(), pi.dwProcessId);
        std::wprintf(L"injecting %s ... ", dll.c_str());
        if (!InjectInto(pi.hProcess, dll)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return 1;
        }
        std::wprintf(L"ok, resuming\n");
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }

    DWORD pid = _wtoi(a.exe.c_str());
    if (!pid) pid = PidFromName(a.exe);
    if (!pid) {
        std::fwprintf(stderr, L"target process not found: %s\n", a.exe.c_str());
        return 1;
    }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::fwprintf(stderr, L"OpenProcess(pid=%lu) failed (err=%lu)\n", pid, GetLastError());
        return 1;
    }
    std::wprintf(L"injecting %s into pid %lu ... ", dll.c_str(), pid);
    const bool ok = InjectInto(process, dll);
    std::wprintf(L"%s\n", ok ? L"ok" : L"FAILED");
    CloseHandle(process);
    return ok ? 0 : 1;
}
