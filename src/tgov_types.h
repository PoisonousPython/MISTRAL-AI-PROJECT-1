#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace tgov {

struct Config {
    bool enabled = false;

    unsigned cpu_rate_permille = 0;
    unsigned long long affinity_mask = 0;
    unsigned reserve_core_count = 0;
    std::string priority_class;
    bool reapply_thread_sched = true;
    unsigned thread_sweep_ms = 250;

    unsigned long long commit_limit_mb = 0;

    int dxgi_force_sync_interval = -1;
    double fps_global = 0.0;
    double fps_d3d9 = 0.0;
    double fps_dxgi = 0.0;
    double fps_vulkan = 0.0;
    double fps_opengl = 0.0;
    unsigned spin_guard_ms = 2;

    std::string power_plan;
    bool vulkan_layer = false;
    std::string vulkan_pace_point = "queue_present";
};

struct PacedTarget {
    std::string name;
    double fps = 0.0;
    unsigned long long paces = 0;
};

class Pacer {
public:
    void SetRate(double fps);
    void SetSpinGuard(unsigned spin_ms);
    void Wait();
    unsigned long long Paces() const { return paced_; }

private:
    void PreciseSleep(std::chrono::steady_clock::duration remaining);

    std::mutex mu_;
    std::chrono::steady_clock::duration interval_{0};
    std::chrono::steady_clock::time_point last_{};
    unsigned spin_ms_ = 2;
    unsigned long long paced_ = 0;
};

double EffectiveFps(double per_api);

bool ParseBool(const std::string& v, bool def);
double ParseNumber(const std::string& v, double def);
unsigned long long ParseMask(const std::string& v, unsigned long long def);

bool LoadConfigFromIni(const std::string& path, Config& out);
std::string Trimmed(const std::string& s);

namespace pe {

std::vector<void**> FindIatSlots(const uint8_t* image_base, const char* module_name,
                                 const char* function_name);
unsigned OverwriteIatSlots(const std::vector<void**>& slots, void* detour,
                           void** original_out);

}  // namespace pe

}  // namespace tgov
