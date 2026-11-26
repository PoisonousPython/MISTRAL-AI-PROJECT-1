#include "tgov_win.h"

#include <tlhelp32.h>

#ifndef JOB_OBJECT_CPU_RATE_CONTROL_ENABLE
#define JOB_OBJECT_CPU_RATE_CONTROL_ENABLE 0x00000001
#define JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP 0x00000002
typedef struct _JOBOBJECT_CPU_RATE_CONTROL_INFORMATION {
    DWORD ControlFlags;
    DWORD CpuRate;
} JOBOBJECT_CPU_RATE_CONTROL_INFORMATION;
#endif

#include <powrprof.h>
#include <cstring>
#include <string>
#include <vector>

namespace tgov {

namespace {

struct Guid {
    unsigned long a;
    unsigned short b, c;
    unsigned char d[8];
};

bool ParseGuid(const char* s, Guid& g) {
    unsigned int t[11] = {};
    if (std::sscanf(s, "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &t[0], &t[1], &t[2], &t[3],
                    &t[4], &t[5], &t[6], &t[7], &t[8], &t[9], &t[10]) != 11)
        return false;
    g.a = t[0];
    g.b = static_cast<unsigned short>(t[1]);
    g.c = static_cast<unsigned short>(t[2]);
    g.d[0] = static_cast<unsigned char>(t[3]);
    g.d[1] = static_cast<unsigned char>(t[4]);
    g.d[2] = static_cast<unsigned char>(t[5]);
    g.d[3] = static_cast<unsigned char>(t[6]);
    g.d[4] = static_cast<unsigned char>(t[7]);
    g.d[5] = static_cast<unsigned char>(t[8]);
    g.d[6] = static_cast<unsigned char>(t[9]);
    g.d[7] = static_cast<unsigned char>(t[10]);
    return true;
}

HANDLE g_job = nullptr;
unsigned long long g_thread_mask = 0;
DWORD g_last_error_code = 0;
std::vector<DWORD> g_own_thread_ids;

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
        unsigned long long reserved = (n >= 64) ? ~0ULL : ((1ULL << n) - 1ULL);
        unsigned long long mask = all & ~reserved;
        return mask ? mask : all;
    }
    return 0;
}

bool IsOwnThread(DWORD tid) {
    for (DWORD id : g_own_thread_ids)
        if (id == tid) return true;
    return false;
}

void RegisterCurrentThread() { g_own_thread_ids.push_back(GetCurrentThreadId()); }

void ApplyAffinityToThread(HANDLE thread) {
    if (!g_thread_mask) return;
    SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(g_thread_mask));
}

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

void ApplyPriorityClass() {
    const std::string& p = Cfg().priority_class;
    if (p.empty()) return;
    DWORD cls = 0;
    if (p == "idle") cls = IDLE_PRIORITY_CLASS;
    else if (p == "below_normal") cls = BELOW_NORMAL_PRIORITY_CLASS;
    else if (p == "high") cls = HIGH_PRIORITY_CLASS;
    else if (p == "normal") cls = NORMAL_PRIORITY_CLASS;
    if (!cls) {
        Logf("sched: unknown priority_class '%s'", p.c_str());
        return;
    }
    if (SetPriorityClass(GetCurrentProcess(), cls)) {
        Logf("sched: priority class set to %s", p.c_str());
    } else {
        Logf("sched: SetPriorityClass failed (err=%lu)", GetLastError());
    }
}

void ApplyPowerPlan() {
    const std::string& p = Cfg().power_plan;
    if (p.empty()) return;
    Guid g{};
    std::string lower;
    for (char ch : p) lower.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    if (lower == "balanced")
        g = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0x20, 0x23, 0x97, 0x59, 0xac, 0x74}};
    else if (lower == "high" || lower == "high_performance")
        g = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
    else if (lower == "saver" || lower == "power_saver")
        g = {0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
    else if (lower == "ultimate")
        g = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
    else if (!ParseGuid(p.c_str(), g)) {
        Logf("sched: unknown power plan '%s'", p.c_str());
        return;
    }
    GUID plan{};
    plan.Data1 = g.a;
    plan.Data2 = g.b;
    plan.Data3 = g.c;
    std::memcpy(plan.Data4, g.d, 8);
    if (PowerSetActiveScheme(nullptr, &plan)) {
        Logf("sched: power plan applied '%s'", p.c_str());
    } else {
        Logf("sched: PowerSetActiveScheme failed (err=%lu) - admin rights may be required",
             GetLastError());
    }
}

void ApplyJobLimits() {
    const Config& c = Cfg();
    if (!c.cpu_rate_permille && !c.commit_limit_mb) return;

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

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
    if (c.commit_limit_mb) {
        ext.ProcessMemoryLimit = c.commit_limit_mb * 1024ULL * 1024ULL;
        ext.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &ext, sizeof(ext))) {
            Logf("sched: commit limit failed (err=%lu)", GetLastError());
        } else {
            Logf("sched: commit limit set to %llu MB", c.commit_limit_mb);
        }
    }

    if (c.cpu_rate_permille) {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
        rate.CpuRate = c.cpu_rate_permille * 10;
        rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        if (!SetInformationJobObject(g_job, JobObjectCpuRateControlInformation, &rate, sizeof(rate))) {
            g_last_error_code = GetLastError();
            Logf("sched: cpu rate cap failed (err=%lu) - requires Windows 8+", g_last_error_code);
        } else {
            Logf("sched: cpu rate hard cap set to %.1f%%", c.cpu_rate_permille / 100.0);
        }
    }
}

}  // namespace

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

void SchedulerSweep() {
    if (!Cfg().reapply_thread_sched) return;
    if (!g_thread_mask) return;
    SweepThreads();
}

void SchedulerShutdown() {
    if (g_job) {
        CloseHandle(g_job);
        g_job = nullptr;
    }
}

}  // namespace tgov
