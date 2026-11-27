#include <cstdint>

#include <cstring>
#include <vector>

namespace tgov {
namespace pe {

namespace {

uint16_t Rd16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;  // little-endian targets only
}

uint32_t Rd32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

uint64_t Rd64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    return v;
}

bool Ieq(const uint8_t* a, uint32_t alen, const char* b) {
    uint32_t i = 0;
    for (; i < alen && b[i]; ++i) {
        char ca = static_cast<char>(a[i]);
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return i == alen && b[i] == '\0';
}

uint32_t StrLenZ(const uint8_t* p, uint32_t max) {
    uint32_t n = 0;
    while (n < max && p[n]) ++n;
    return n;
}

struct Layout {
    uint32_t import_dir_rva = 0;
    uint32_t thunk_size = 4;
};

bool ParseLayout(const uint8_t* img, Layout& out) {
    if (Rd16(img) != 0x5A4D) return false;
    const uint32_t e_lfanew = Rd32(img + 0x3C);
    if (e_lfanew < 0x40 || e_lfanew > 0x40000000) return false;
    if (Rd32(img + e_lfanew) != 0x00004550) return false;
    const uint16_t opt_magic = Rd16(img + e_lfanew + 24);
    if (opt_magic == 0x10B) {
        out.thunk_size = 4;
        out.import_dir_rva = e_lfanew + 24 + 96;
    } else if (opt_magic == 0x20B) {
        out.thunk_size = 8;
        out.import_dir_rva = e_lfanew + 24 + 112;
    } else {
        return false;
    }
    return true;
}

}  // namespace

std::vector<void**> FindIatSlots(const uint8_t* image_base, const char* module_name,
                                 const char* function_name) {
    std::vector<void**> slots;
    Layout layout;
    if (!ParseLayout(image_base, layout)) return slots;

    const uint32_t import_rva = Rd32(image_base + layout.import_dir_rva + 8);
    if (import_rva == 0) return slots;

    const uint8_t* desc = image_base + import_rva;
    for (;; desc += 20) {
        const uint32_t oft = Rd32(desc + 0);
        const uint32_t name_rva = Rd32(desc + 12);
        const uint32_t ft = Rd32(desc + 16);
        if (ft == 0) break;
        if (oft == 0 || name_rva == 0) continue;

        const uint8_t* mod_name = image_base + name_rva;
        const uint32_t mod_len = StrLenZ(mod_name, 64);
        if (!Ieq(mod_name, mod_len, module_name)) continue;

        const uint8_t* names = image_base + oft;
        const uint8_t* iat = image_base + ft;
        const uint32_t stride = layout.thunk_size;

        for (uint32_t i = 0;; ++i) {
            const uint64_t raw =
                stride == 8 ? Rd64(names + i * 8) : static_cast<uint64_t>(Rd32(names + i * 4));
            if (raw == 0) break;
            const uint64_t ord_flag = stride == 8 ? (1ULL << 63) : 0x80000000ULL;
            if (raw & ord_flag) continue;

            const uint32_t fname_rva = static_cast<uint32_t>(
                stride == 8 ? (raw & 0x0000FFFFFFFFFFFFULL) : (raw & 0x7FFFFFFFULL));
            const uint8_t* fn = image_base + fname_rva + 2;
            const uint32_t fn_len = StrLenZ(fn, 256);
            if (Ieq(fn, fn_len, function_name)) {
                slots.push_back(reinterpret_cast<void**>(const_cast<uint8_t*>(iat) + i * stride));
            }
        }
    }
    return slots;
}

unsigned OverwriteIatSlots(const std::vector<void**>& slots, void* detour,
                           void** original_out) {
    unsigned overwritten = 0;
    for (void** slot : slots) {
        if (*slot == detour) continue;
        if (original_out && *original_out == nullptr) *original_out = *slot;
        *slot = detour;
        ++overwritten;
    }
    return overwritten;
}

}  // namespace pe
}  // namespace tgov
