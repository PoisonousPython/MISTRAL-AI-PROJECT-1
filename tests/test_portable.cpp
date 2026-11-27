#include "tgov_types.h"

#include <chrono>
#include <cstdio>
#include <cstring>

using namespace tgov;

static int g_failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) {                                             \
            std::printf("PASS: %s\n", msg);                     \
        } else {                                                \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

namespace {

constexpr std::size_t kImgSize = 0x1000;
constexpr uint32_t kImportDir = 0x200;
constexpr uint32_t kDesc0 = 0x200;
constexpr uint32_t kDesc1 = 0x214;
constexpr uint32_t kOft0 = 0x260;
constexpr uint32_t kIat0 = 0x280;
constexpr uint32_t kOft1 = 0x2A0;
constexpr uint32_t kIat1 = 0x2C0;
constexpr uint32_t kName0 = 0x320;
constexpr uint32_t kMod2 = 0x340;
constexpr uint32_t kHint0 = 0x380;
constexpr uint32_t kHint1 = 0x3B0;

void Put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void Put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

void PutNameAt(uint8_t* img, uint32_t rva, const char* s) {
    std::snprintf(reinterpret_cast<char*>(img + rva), 64, "%s", s);
}

uint32_t HintName(uint8_t* img, uint32_t rva, const char* name) {
    Put16(img + rva, 0);
    std::snprintf(reinterpret_cast<char*>(img + rva + 2), 48, "%s", name);
    return rva;
}

void BuildPe(uint8_t* img, bool is64) {
    std::memset(img, 0, kImgSize);
    Put16(img, 0x5A4D);
    Put32(img + 0x3C, 0x80);
    Put32(img + 0x80, 0x00004550);
    Put16(img + 0x84, is64 ? 0x8664 : 0x014C);
    Put16(img + 0x86, 3);
    Put16(img + 0x94, is64 ? 0xF0 : 0xE0);
    Put16(img + 0x96, 0x0022);

    uint8_t* opt = img + 0x80 + 24;
    Put16(opt, is64 ? 0x20B : 0x10B);

    uint8_t* dirs = opt + (is64 ? 112 : 96);
    Put32(dirs + 1 * 8 + 0, kImportDir);
    Put32(dirs + 1 * 8 + 4, 0x80);

    Put32(img + kDesc0 + 0, kOft0);
    Put32(img + kDesc0 + 12, kName0);
    Put32(img + kDesc0 + 16, kIat0);
    Put32(img + kDesc1 + 0, kOft1);
    Put32(img + kDesc1 + 12, kMod2);
    Put32(img + kDesc1 + 16, kIat1);

    PutNameAt(img, kName0, "d3d9.dll");
    PutNameAt(img, kMod2, "DXGI.DLL");
    HintName(img, kHint0, "Direct3DCreate9");
    HintName(img, kHint1, "Present");

    if (is64) {
        Put64(img + kOft0 + 0, kHint0);
        Put64(img + kOft0 + 8, kHint0);  // same function imported twice under one name slot pair
        Put64(img + kOft0 + 16, 0x00000000);

        Put64(img + kIat0 + 0, 0xAAAA0000AAAA0001ULL);
        Put64(img + kIat0 + 8, 0xAAAA0000AAAA0002ULL);
        Put64(img + kIat0 + 16, 0);

        Put64(img + kOft1 + 0, kHint1);
        Put64(img + kIat1 + 0, 0xBBBB000000000001ULL);
        Put64(img + kIat1 + 8, 0);
    } else {
        Put32(img + kOft0 + 0, kHint0);
        Put32(img + kOft0 + 4, kHint0);  // same function imported twice under one name slot pair
        Put32(img + kOft0 + 8, 0x00000000);

        Put32(img + kIat0 + 0, 0xAAAA0001U);
        Put32(img + kIat0 + 4, 0xAAAA0002U);
        Put32(img + kIat0 + 8, 0);

        Put32(img + kOft1 + 0, kHint1);
        Put32(img + kIat1 + 0, 0xBBBB0001U);
        Put32(img + kIat1 + 4, 0);
    }
}

}  // namespace

int main() {
    {
        uint8_t img[kImgSize];
        BuildPe(img, true);

        static int marker = 0;
        void* detour = const_cast<int*>(&marker);

        auto slots = pe::FindIatSlots(img, "D3D9.DLL", "Direct3DCreate9");
        CHECK(slots.size() == 2, "PE64: both import sites found (case-insensitive module)");
        void* orig = nullptr;
        const unsigned hits = pe::OverwriteIatSlots(slots, detour, &orig);
        CHECK(hits == 2, "PE64: both sites overwritten");
        CHECK(orig == reinterpret_cast<void*>(0xAAAA0000AAAA0001ULL),
              "PE64: original pointer captured");
        void** sl = reinterpret_cast<void**>(img + kIat0);
        CHECK(sl[0] == detour && sl[1] == detour, "PE64: IAT slots point at detour");

        const unsigned again =
            pe::OverwriteIatSlots(pe::FindIatSlots(img, "d3d9.dll", "Direct3DCreate9"), detour, nullptr);
        CHECK(again == 0, "PE64: repeat hook is idempotent");

        CHECK(pe::FindIatSlots(img, "d3d9.dll", "EndScene").empty(),
              "PE64: unknown function finds no slot");

        auto dxgi_slots = pe::FindIatSlots(img, "dxgi.dll", "Present");
        CHECK(dxgi_slots.size() == 1, "PE64: second module descriptor matched");
        CHECK(pe::OverwriteIatSlots(dxgi_slots, detour, nullptr) == 1, "PE64: dxgi slot hooked");
        CHECK(*reinterpret_cast<void**>(img + kIat1) == detour, "PE64: dxgi IAT slot points at detour");

        uint8_t alien[64];
        std::memset(alien, 0, sizeof(alien));
        std::memcpy(alien, "NZ", 2);
        CHECK(pe::FindIatSlots(alien, "d3d9.dll", "Direct3DCreate9").empty(),
              "PE: non-PE buffer rejected");

        std::printf("-> PE parse: no crash on malformed input\n");
    }

    {
        uint8_t img[kImgSize];
        BuildPe(img, false);

        static int marker32 = 0;
        void* detour = const_cast<int*>(&marker32);

        auto slots = pe::FindIatSlots(img, "d3d9.dll", "Direct3DCreate9");
        CHECK(slots.size() == 2, "PE32: both import sites found");
        CHECK(*reinterpret_cast<uint32_t*>(slots[0]) == 0xAAAA0001U,
              "PE32: original pointer captured");
        CHECK(pe::OverwriteIatSlots(slots, detour, nullptr) == 2, "PE32: both sites overwritten");

        auto dxgi_slots = pe::FindIatSlots(img, "dxgi.dll", "Present");
        CHECK(dxgi_slots.size() == 1, "PE32: second module descriptor matched");
        CHECK(*reinterpret_cast<void**>(img + kIat1) == reinterpret_cast<void*>(0xBBBB0001U),
              "PE32: dxgi IAT slot holds original before overwrite");
        std::printf("-> PE32 parse: 4-byte thunks walked correctly\n");
    }

    {
        Config c;
        CHECK(LoadConfigFromIni("config/thermal_governor.ini", c), "ini: sample file parses");
        CHECK(c.enabled, "ini: enabled=true");
        CHECK(c.cpu_rate_permille == 7500, "ini: cpu_rate_permille=7500");
        CHECK(c.priority_class == "below_normal", "ini: priority_class=below_normal");
        CHECK(c.fps_global == 60.0, "ini: fps_limit=60");
        CHECK(c.vulkan_pace_point == "queue_present", "ini: vulkan pace point");
    }

    {
        const char* inifile = "tests_tmp.ini";
        Config c2;
        FILE* f = std::fopen(inifile, "wb");
        std::fputs(
            "[cpu]\naffinity_mask = 0x00FF\nreserve_cores = 3\n[framerate]\nfps_d3d9 = 144.5\n",
            f);
        std::fclose(f);
        LoadConfigFromIni(inifile, c2);
        std::remove(inifile);
        CHECK(c2.affinity_mask == 0xFF, "ini: mask 0x00FF parsed");
        CHECK(c2.fps_d3d9 == 144.5, "ini: fps_d3d9=144.5 parsed");
        CHECK(c2.reserve_core_count == 3, "ini: reserve_cores=3 parsed");
    }

    {
        const char* inifile = "tests_tmp.ini";
        Config c3;
        FILE* f = std::fopen(inifile, "wb");
        std::fputs(
            "[cpu]\ncpu_rate_permille = -50\nreserve_cores = -1\nthread_sweep_ms = -100\n"
            "[memory]\ncommit_limit_mb = -1\n"
            "[pacing]\nspin_guard_ms = -5\n[framerate]\nfps_limit = -30\n",
            f);
        std::fclose(f);
        LoadConfigFromIni(inifile, c3);
        std::remove(inifile);
        CHECK(c3.cpu_rate_permille == 0, "ini: negative cpu_rate clamps to 0 (no cap)");
        CHECK(c3.reserve_core_count == 0, "ini: negative reserve_cores clamps to 0");
        CHECK(c3.thread_sweep_ms == 50, "ini: negative thread_sweep clamps to minimum 50");
        CHECK(c3.commit_limit_mb == 0, "ini: negative commit limit clamps to 0");
        CHECK(c3.spin_guard_ms == 0, "ini: negative spin guard clamps to 0");
        CHECK(c3.fps_global <= 0.0, "ini: negative fps means uncapped");

        FILE* f2 = std::fopen(inifile, "wb");
        std::fputs(
            "[cpu]\ncpu_rate_permille = 99999\nthread_sweep_ms = 999999\n[pacing]\nspin_guard_ms = 999\n",
            f2);
        std::fclose(f2);
        LoadConfigFromIni(inifile, c3);
        std::remove(inifile);
        CHECK(c3.cpu_rate_permille == 10000, "ini: oversized cpu_rate clamps to 10000");
        CHECK(c3.thread_sweep_ms == 60000, "ini: oversized thread_sweep clamps to 60000");
        CHECK(c3.spin_guard_ms == 15, "ini: oversized spin_guard clamps to 15");

        FILE* f3 = std::fopen(inifile, "wb");
        std::fputs("[present]\ndxgi_force_sync_interval = -9\n", f3);
        std::fclose(f3);
        LoadConfigFromIni(inifile, c3);
        std::remove(inifile);
        CHECK(c3.dxgi_force_sync_interval == -1, "ini: sync interval below -1 clamps to -1 (no force)");

        FILE* f4 = std::fopen(inifile, "wb");
        std::fputs("[present]\ndxgi_force_sync_interval = 9\n", f4);
        std::fclose(f4);
        LoadConfigFromIni(inifile, c3);
        std::remove(inifile);
        CHECK(c3.dxgi_force_sync_interval == 4, "ini: sync interval above 4 clamps to 4");
    }

    {
        const char* inifile = "tests_tmp.ini";
        Config c4;
        FILE* f = std::fopen(inifile, "wb");
        std::fputs(
            "[governor]\nenabled = true\n"
            "[cpu]\n; Max total CPU usage. 7500 means 75% of the whole machine. 0 = no cap.\n"
            "cpu_rate_permille = 7500\n"
            "; Logical cores the game may NOT use (0 = game can use everything).\n"
            "reserve_cores = 0\npriority_class = below_normal\napply_to_new_threads = true\n"
            "thread_sweep_ms = 250\n"
            "[memory]\ncommit_limit_mb = 0\n"
            "[framerate]\nfps_limit = 60\nfps_d3d9 = 0\nfps_dxgi = 0\nfps_vulkan = 0\n"
            "fps_opengl = 0\n"
            "[present]\ndxgi_force_sync_interval = -1\n"
            "[pacing]\nspin_guard_ms = 2\n"
            "[power]\nplan = \n"
            "[vulkan]\nlayer = false\npace_point = queue_present\n",
            f);
        std::fclose(f);
        const bool loaded = LoadConfigFromIni(inifile, c4);
        std::remove(inifile);
        CHECK(loaded, "wizard contract: wizard-shaped ini loads");
        CHECK(c4.enabled, "wizard contract: enabled");
        CHECK(c4.cpu_rate_permille == 7500, "wizard contract: cpu 75% -> 7500 job units");
        CHECK(c4.reserve_core_count == 0, "wizard contract: reserve cores");
        CHECK(c4.priority_class == "below_normal", "wizard contract: priority class");
        CHECK(c4.reapply_thread_sched, "wizard contract: apply_to_new_threads");
        CHECK(c4.thread_sweep_ms == 250, "wizard contract: thread sweep");
        CHECK(c4.fps_global == 60.0, "wizard contract: fps limit");
        CHECK(c4.dxgi_force_sync_interval == -1, "wizard contract: sync interval");
        CHECK(c4.spin_guard_ms == 2, "wizard contract: spin guard");
        CHECK(c4.power_plan.empty(), "wizard contract: empty power plan ok");
        CHECK(!c4.vulkan_layer, "wizard contract: layer off");
        CHECK(c4.vulkan_pace_point == "queue_present", "wizard contract: pace point");
    }

    {
        Pacer p;
        p.SetRate(0.0);
        const auto t0 = std::chrono::steady_clock::now();
        p.Wait();
        p.Wait();
        const auto dt = std::chrono::steady_clock::now() - t0;
        CHECK(dt < std::chrono::milliseconds{1}, "pacer: rate 0 => unthrottled");

        p.SetRate(2000.0);
        const auto a = std::chrono::steady_clock::now();
        for (int i = 0; i < 4; ++i) p.Wait();
        const auto d = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - a)
                           .count();
        CHECK(d >= 1400, "pacer: 4x 2000fps waits >= ~1.5ms total");
        CHECK(d < 6000, "pacer: 4x 2000fps waits < 6ms total");

        p.SetRate(30.0);
        p.Wait();
        const auto b = std::chrono::steady_clock::now();
        p.Wait();
        const auto d2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - b)
                            .count();
        CHECK(d2 >= 31, "pacer: first paced wait spans a full 33ms interval");
    }

    std::printf("%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
