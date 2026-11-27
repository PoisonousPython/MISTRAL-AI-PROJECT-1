#include "tgov_types.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace tgov {

std::string Trimmed(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool ParseBool(const std::string& v, bool def) {
    std::string t;
    for (char c : v) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
    if (t == "0" || t == "false" || t == "no" || t == "off") return false;
    return def;
}

double ParseNumber(const std::string& v, double def) {
    if (v.empty()) return def;
    char* end = nullptr;
    double x = std::strtod(v.c_str(), &end);
    if (end == v.c_str()) return def;
    return x;
}

unsigned long long ParseMask(const std::string& v, unsigned long long def) {
    if (v.empty()) return def;
    std::string t = v;
    if (t.rfind("0x", 0) == 0 || t.rfind("0X", 0) == 0) t = t.substr(2);
    char* end = nullptr;
    unsigned long long x = std::strtoull(t.c_str(), &end, 16);
    if (end == t.c_str()) return def;
    return x;
}

bool LoadConfigFromIni(const std::string& path, Config& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    std::unordered_map<std::string, std::string> kv;
    std::string section;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = Trimmed(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = Trimmed(s.substr(1, s.size() - 2));
            for (char& c : section) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trimmed(s.substr(0, eq));
        std::string val = Trimmed(s.substr(eq + 1));
        if (key.empty()) continue;
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        kv[section + "." + key] = val;
        kv[key] = val;
    }
    std::fclose(f);

    out.enabled = ParseBool(kv["governor.enabled"], out.enabled);

    const double cpu_rate =
        ParseNumber(kv["cpu.cpu_rate_permille"], static_cast<double>(out.cpu_rate_permille));
    out.cpu_rate_permille = cpu_rate < 0.0 ? 0u : static_cast<unsigned>(cpu_rate);
    if (out.cpu_rate_permille > 10000) out.cpu_rate_permille = 10000;
    out.affinity_mask = ParseMask(kv["cpu.affinity_mask"], out.affinity_mask);
    const double reserve =
        ParseNumber(kv["cpu.reserve_cores"], static_cast<double>(out.reserve_core_count));
    out.reserve_core_count = reserve < 0.0 ? 0u : static_cast<unsigned>(reserve);
    out.priority_class = Trimmed(kv["cpu.priority_class"]);
    out.reapply_thread_sched = ParseBool(kv["cpu.apply_to_new_threads"], out.reapply_thread_sched);
    const double sweep =
        ParseNumber(kv["cpu.thread_sweep_ms"], static_cast<double>(out.thread_sweep_ms));
    out.thread_sweep_ms = sweep < 50.0 ? 50u : static_cast<unsigned>(sweep < 60000.0 ? sweep : 60000.0);

    const double commit =
        ParseNumber(kv["memory.commit_limit_mb"], static_cast<double>(out.commit_limit_mb));
    out.commit_limit_mb = commit < 0.0 ? 0ULL : static_cast<unsigned long long>(commit);

    out.dxgi_force_sync_interval =
        static_cast<int>(ParseNumber(kv["present.dxgi_force_sync_interval"], out.dxgi_force_sync_interval));
    if (out.dxgi_force_sync_interval > 4) out.dxgi_force_sync_interval = 4;

    out.fps_global = ParseNumber(kv["framerate.fps_limit"], out.fps_global);
    out.fps_d3d9 = ParseNumber(kv["framerate.fps_d3d9"], out.fps_d3d9);
    out.fps_dxgi = ParseNumber(kv["framerate.fps_dxgi"], out.fps_dxgi);
    out.fps_vulkan = ParseNumber(kv["framerate.fps_vulkan"], out.fps_vulkan);
    out.fps_opengl = ParseNumber(kv["framerate.fps_opengl"], out.fps_opengl);
    const double spin =
        ParseNumber(kv["pacing.spin_guard_ms"], static_cast<double>(out.spin_guard_ms));
    out.spin_guard_ms = spin < 0.0 ? 0u : static_cast<unsigned>(spin);
    if (out.spin_guard_ms > 15) out.spin_guard_ms = 15;

    out.power_plan = Trimmed(kv["power.plan"]);
    out.vulkan_layer = ParseBool(kv["vulkan.layer"], out.vulkan_layer);
    out.vulkan_pace_point = Trimmed(kv["vulkan.pace_point"]);
    if (out.vulkan_pace_point != "queue_submit") out.vulkan_pace_point = "queue_present";

    return true;
}

}  // namespace tgov
