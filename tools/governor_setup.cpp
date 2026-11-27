#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iostream>
#include <string>

static std::wstring SelfDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

static bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    return dir + L"\\" + name;
}

static std::wstring Trimmed(const std::wstring& s) {
    const size_t first = s.find_first_not_of(L" \t\r\n\"");
    if (first == std::wstring::npos) return L"";
    const size_t last = s.find_last_not_of(L" \t\r\n\"");
    return s.substr(first, last - first + 1);
}

static std::wstring ReadLine() {
    std::wstring line;
    if (!std::getline(std::wcin, line)) return L"";
    return line;
}

static void PromptClose() {
    std::wprintf(L"\nPress Enter to close...");
    ReadLine();
}

static double ReadNumber(const wchar_t* prompt, double fallback) {
    while (true) {
        std::wprintf(L"%s [%g]: ", prompt, fallback);
        const std::wstring raw = Trimmed(ReadLine());
        if (raw.empty()) return fallback;
        wchar_t* end = nullptr;
        const double value = std::wcstod(raw.c_str(), &end);
        if (end && *end == L'\0' && raw.c_str() != end && value >= 0.0) return value;
        std::wprintf(L"Please enter a plain number (or press Enter)\n");
    }
}

static std::string FormatNum(double v) {
    char buf[32] = {};
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    } else {
        std::snprintf(buf, sizeof buf, "%g", v);
    }
    return buf;
}

static bool WriteAsciiFile(const std::wstring& path, const std::string& content) {
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return false;
    const size_t written = fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return written == content.size();
}

static std::wstring PickGameDialog(bool& cancelled) {
    std::wstring result;
    cancelled = false;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return result;
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IFileOpenDialog), reinterpret_cast<void**>(&dlg));
    if (SUCCEEDED(hr)) {
        dlg->SetTitle(L"Pick your game (the .exe you normally double-click)");
        const COMDLG_FILTERSPEC filters[] = {
            {L"Game executables (*.exe)", L"*.exe"},
            {L"All files (*.*)", L"*.*"},
        };
        dlg->SetFileTypes(2, filters);
        dlg->SetOptions(FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        hr = dlg->Show(nullptr);
        if (hr == S_OK) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        } else if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            cancelled = true;
        }
        dlg->Release();
    }
    CoUninitialize();
    return result;
}

static std::wstring PickGameTyped() {
    std::wprintf(L"Type or drag the game's .exe file here:\n> ");
    std::wstring path = Trimmed(ReadLine());
    if (!path.empty() && !FileExists(path)) {
        std::wprintf(L"[X] That file does not exist: %s\n", path.c_str());
        path.clear();
    }
    return path;
}

static std::string BuildIni(double fps, long long cpuPermille, int cores, int spin) {
    std::string ini;
    ini += "[governor]\n";
    ini += "enabled = true\n";
    ini += "\n";
    ini += "[cpu]\n";
    ini += "; Max total CPU usage. 7500 means 75% of the whole machine. 0 = no cap.\n";
    ini += "cpu_rate_permille = " + std::to_string(cpuPermille) + "\n";
    ini += "\n";
    ini += "; Logical cores the game may NOT use (0 = game can use everything).\n";
    ini += "reserve_cores = " + std::to_string(cores) + "\n";
    ini += "\n";
    ini += "; Lower priority keeps the game out of the way of the rest of Windows.\n";
    ini += "priority_class = below_normal\n";
    ini += "\n";
    ini += "; Newly spawned game threads get the same limits.\n";
    ini += "apply_to_new_threads = true\n";
    ini += "thread_sweep_ms = 250\n";
    ini += "\n";
    ini += "[memory]\n";
    ini += "commit_limit_mb = 0\n";
    ini += "\n";
    ini += "[framerate]\n";
    ini += "; fps_limit is used for every graphics API unless a specific one is set.\n";
    ini += "fps_limit = " + FormatNum(fps) + "\n";
    ini += "fps_d3d9 = 0\n";
    ini += "fps_dxgi = 0\n";
    ini += "fps_vulkan = 0\n";
    ini += "fps_opengl = 0\n";
    ini += "\n";
    ini += "[present]\n";
    ini += "; -1 = let the game decide vsync, 0 = force off, 1..4 = force vsync.\n";
    ini += "dxgi_force_sync_interval = -1\n";
    ini += "\n";
    ini += "[pacing]\n";
    ini += "; Precision window in milliseconds. Higher = smoother edges, a bit more CPU.\n";
    ini += "spin_guard_ms = " + std::to_string(spin) + "\n";
    ini += "\n";
    ini += "[power]\n";
    ini += "; Optional: balanced | high | ultimate | saver\n";
    ini += "plan =\n";
    ini += "\n";
    ini += "[vulkan]\n";
    ini += "layer = false\n";
    ini += "pace_point = queue_present\n";
    return ini;
}

static int RunInjector(const std::wstring& injector, const std::wstring& cmdTail) {
    const std::wstring cmdline = L"\"" + injector + L"\" " + cmdTail;
    std::wstring mutableCmd = cmdline;
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::wprintf(L"[X] Could not start governor_injector.exe (error %lu).\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exitCode);
}

int wmain() {
    std::wprintf(L"\n==============================================\n");
    std::wprintf(L"   Thermal Governor - setup helper\n");
    std::wprintf(L"==============================================\n\n");

    const std::wstring dir = SelfDirectory();
    const std::wstring dll = JoinPath(dir, L"governor.dll");
    const std::wstring injector = JoinPath(dir, L"governor_injector.exe");
    const std::wstring iniPath = JoinPath(dir, L"thermal_governor.ini");

    if (!FileExists(dll)) {
        std::wprintf(L"[X] governor.dll was not found next to this program.\n");
        std::wprintf(L"    If you opened the zip directly, extract all files to a folder first,\n");
        std::wprintf(L"    then run this from that folder. Keep governor.dll, governor_injector.exe\n");
        std::wprintf(L"    and this program in one folder.\n");
        PromptClose();
        return 1;
    }
    if (!FileExists(injector)) {
        std::wprintf(L"[X] governor_injector.exe was not found next to this program.\n");
        std::wprintf(L"    Extract all files to a folder first, then run this from that folder.\n");
        PromptClose();
        return 1;
    }

    bool cancelled = false;
    std::wstring game = PickGameDialog(cancelled);
    if (cancelled) {
        std::wprintf(L"Cancelled. Nothing was changed.\n");
        PromptClose();
        return 1;
    }
    if (game.empty()) game = PickGameTyped();
    if (game.empty()) {
        std::wprintf(L"Cancelled. Nothing was changed.\n");
        PromptClose();
        return 1;
    }

    std::wprintf(L"\nGame selected:\n  %s\n\n", game.c_str());
    const double fps = ReadNumber(L"Frame limit (0 = no limit)", 60);
    const double cpu = ReadNumber(L"Max CPU usage in percent (0 = no cap)", 75);
    double cores = ReadNumber(L"Reserve idle cores, 0-4. More = cooler, slightly slower", 0);
    if (cores > 4) cores = 4;
    double spin = ReadNumber(L"Pacing precision 0-4 milliseconds (2 is a good default)", 2);
    if (spin > 4) spin = 4;

    long long cpuPermille = static_cast<long long>(std::llround(cpu * 100.0));
    if (cpuPermille > 10000) cpuPermille = 10000;
    if (cpu == 0.0) cpuPermille = 0;

    if (!WriteAsciiFile(iniPath, BuildIni(fps, cpuPermille, static_cast<int>(cores), static_cast<int>(spin)))) {
        std::wprintf(L"[X] Could not write %s (error %lu).\n", iniPath.c_str(), GetLastError());
        PromptClose();
        return 1;
    }
    std::wprintf(L"\n[OK] Settings saved to %s\n", iniPath.c_str());

    std::wprintf(L"\nIs the game already running? (y/N): ");
    const bool attach = std::wcspbrk(Trimmed(ReadLine()).c_str(), L"yY") != nullptr;

    std::wprintf(L"\nStarting the game with the governor...\n\n");
    int exitCode;
    if (attach) {
        exitCode = RunInjector(injector, L"--pid \"" + game + L"\" \"" + dll + L"\"");
    } else {
        exitCode = RunInjector(injector, L"--launch \"" + game + L"\" \"" + dll + L"\"");
    }
    if (exitCode != 0) {
        std::wprintf(L"\nThe injector reported a problem (exit code %d).\n", exitCode);
    }

    std::wprintf(L"\n----------------------------------------------\n");
    std::wprintf(L"If anything looks wrong, check the log:\n");
    std::wprintf(L"  %s\\governor.log\n", dir.c_str());
    std::wprintf(L"Tip: settings can be changed at any time with this same helper.\n");
    std::wprintf(L"----------------------------------------------\n");
    PromptClose();
    return exitCode;
}
