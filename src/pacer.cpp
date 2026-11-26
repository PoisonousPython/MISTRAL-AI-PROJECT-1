#include "tgov_types.h"

#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace tgov {

double EffectiveFps(double per_api) { return per_api > 0.0 ? per_api : 0.0; }

void Pacer::SetSpinGuard(unsigned spin_ms) { spin_ms_ = spin_ms; }

void Pacer::SetRate(double fps) {
    std::lock_guard<std::mutex> lock(mu_);
    interval_ = fps > 0.0 ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(1.0 / fps))
                          : std::chrono::steady_clock::duration{0};
    last_ = std::chrono::steady_clock::time_point{};
}

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

void Pacer::Wait() {
    if (interval_.count() == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::steady_clock::now();
    if (last_.time_since_epoch().count() != 0) {
        const auto wake = last_ + interval_;
        if (now < wake) {
            PreciseSleep(wake - now);
            last_ = wake;
            ++paced_;
            return;
        }
        last_ = now;
        ++paced_;
        return;
    }
    last_ = now;
}

}  // namespace tgov
