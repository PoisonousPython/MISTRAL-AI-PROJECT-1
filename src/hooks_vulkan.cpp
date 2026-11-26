#include "tgov_win.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace tgov {

namespace {

typedef std::int32_t VkResult;
typedef struct VkQueue_T* VkQueue;
typedef struct VkInstance_T* VkInstance;
typedef struct VkDevice_T* VkDevice;

constexpr VkResult kVkSuccess = 0;
constexpr VkResult kVkErrorInitFailed = -4;

#if defined(_M_IX86) || defined(__i386__)
#define TGOV_VKCALL __stdcall
#else
#define TGOV_VKCALL
#endif

typedef void* (*PFN_vkVoidFunction)(void);
typedef VkResult(TGOV_VKCALL* PFN_vkQueuePresentKHR)(VkQueue, const void*);
typedef VkResult(TGOV_VKCALL* PFN_vkQueueSubmit)(VkQueue, std::uint32_t, const void*, void*);
typedef PFN_vkVoidFunction(TGOV_VKCALL* PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef PFN_vkVoidFunction(TGOV_VKCALL* PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult(TGOV_VKCALL* PFN_vkEnumerateInstanceLayerProperties)(std::uint32_t*, void*);
typedef VkResult(TGOV_VKCALL* PFN_vkEnumerateInstanceExtensionProperties)(const char*,
                                                                          std::uint32_t*, void*);
typedef VkResult(TGOV_VKCALL* PFN_vkCreateInstance)(const void*, const void*, VkInstance*);

struct VkLayerProperties {
    char layer_name[256];
    std::uint32_t spec_version;
    std::uint32_t implementation_version;
    char description[256];
};

constexpr std::int32_t kLoaderInstanceCreateInfo = 35;
constexpr std::uint32_t kLayerLinkInfo = 0;
constexpr const char* kLayerName = "VK_LAYER_THERMAL_GOVERNOR";

struct VkLayerInstanceLink {
    VkLayerInstanceLink* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
};

struct VkLayerInstanceCreateInfo {
    std::int32_t sType;
    const void* pNext;
    std::uint32_t function;
    union {
        VkLayerInstanceLink* pLayerInfo;
    } u;
};

PFN_vkQueuePresentKHR RealQueuePresent = nullptr;
PFN_vkQueueSubmit RealQueueSubmit = nullptr;
PFN_vkGetInstanceProcAddr NextGipa = nullptr;
PFN_vkGetDeviceProcAddr NextGpd = nullptr;
PFN_vkEnumerateInstanceExtensionProperties LoaderEnumerateExtensions = nullptr;

bool PaceOnPresent() { return Cfg().vulkan_pace_point != "queue_submit"; }

PFN_vkGetInstanceProcAddr LoaderGipa() {
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
    if (!vulkan) vulkan = LoadLibraryA("vulkan-1.dll");
    if (!vulkan) return nullptr;
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")));
}

PFN_vkVoidFunction TGOV_VKCALL LayerGetInstanceProcAddr(VkInstance instance, const char* name);
PFN_vkVoidFunction TGOV_VKCALL LayerGetDeviceProcAddr(VkDevice device, const char* name);
VkResult TGOV_VKCALL LayerEnumerateInstanceLayerProperties(std::uint32_t* count, void* props);
VkResult TGOV_VKCALL LayerEnumerateInstanceExtensionProperties(const char* layer_name,
                                                               std::uint32_t* count, void* props);
VkResult TGOV_VKCALL HookQueuePresent(VkQueue queue, const void* info);
VkResult TGOV_VKCALL HookQueueSubmit(VkQueue queue, std::uint32_t count, const void* submits,
                                     void* fence);

VkResult TGOV_VKCALL LayerCreateInstance(const void* create_info, const void* allocator,
                                         VkInstance* instance) {
    for (auto* it = static_cast<const VkLayerInstanceCreateInfo*>(create_info); it;
         it = static_cast<const VkLayerInstanceCreateInfo*>(it->pNext)) {
        if (it->sType == kLoaderInstanceCreateInfo && it->function == kLayerLinkInfo) {
            if (it->u.pLayerInfo) {
                if (it->u.pLayerInfo->pfnNextGetInstanceProcAddr) {
                    NextGipa = it->u.pLayerInfo->pfnNextGetInstanceProcAddr;
                }
                if (it->u.pLayerInfo->pfnNextGetDeviceProcAddr) {
                    NextGpd = it->u.pLayerInfo->pfnNextGetDeviceProcAddr;
                }
                const_cast<VkLayerInstanceCreateInfo*>(it)->u.pLayerInfo =
                    it->u.pLayerInfo->pNext;
            }
            break;
        }
    }
    if (!NextGipa) NextGipa = LoaderGipa();
    if (!NextGipa) return kVkErrorInitFailed;
    const auto create =
        reinterpret_cast<PFN_vkCreateInstance>(NextGipa(nullptr, "vkCreateInstance"));
    return create ? create(create_info, allocator, instance) : kVkErrorInitFailed;
}

VkResult TGOV_VKCALL LayerEnumerateInstanceLayerProperties(std::uint32_t* count, void* props) {
    static VkLayerProperties ours = {};
    std::snprintf(ours.layer_name, sizeof(ours.layer_name), "%s", kLayerName);
    std::snprintf(ours.description, sizeof(ours.description),
                  "Thermal governor present pacing (governor.dll)");
    ours.spec_version = (1u << 22) | (3u << 12);
    ours.implementation_version = 1;

    if (!count) return kVkErrorInitFailed;
    if (!props) {
        *count = 1;
        return kVkSuccess;
    }
    if (*count == 0) return kVkSuccess;
    *count = 1;
    std::memcpy(props, &ours, sizeof(ours));
    return kVkSuccess;
}

VkResult TGOV_VKCALL LayerEnumerateInstanceExtensionProperties(const char* layer_name,
                                                               std::uint32_t* count,
                                                               void* props) {
    if (layer_name && std::strcmp(layer_name, kLayerName) == 0) {
        if (count) *count = 0;
        return kVkSuccess;
    }
    if (!LoaderEnumerateExtensions) {
        HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");
        LoaderEnumerateExtensions =
            vulkan ? reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                         reinterpret_cast<void*>(GetProcAddress(
                             vulkan, "vkEnumerateInstanceExtensionProperties")))
                   : nullptr;
    }
    if (LoaderEnumerateExtensions) {
        return LoaderEnumerateExtensions(layer_name, count, props);
    }
    if (count) *count = 0;
    return kVkSuccess;
}

PFN_vkVoidFunction TGOV_VKCALL LayerGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&LayerGetInstanceProcAddr);
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&LayerGetDeviceProcAddr);
    }
    if (std::strcmp(name, "vkEnumerateInstanceLayerProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&LayerEnumerateInstanceLayerProperties);
    }
    if (std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&LayerEnumerateInstanceExtensionProperties);
    }
    if (std::strcmp(name, "vkCreateInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&LayerCreateInstance);
    }
    if (!NextGipa) NextGipa = LoaderGipa();
    return NextGipa ? NextGipa(instance, name) : nullptr;
}

PFN_vkVoidFunction TGOV_VKCALL LayerGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&LayerGetDeviceProcAddr);
    }
    const bool is_present = std::strcmp(name, "vkQueuePresentKHR") == 0;
    const bool is_submit = std::strcmp(name, "vkQueueSubmit") == 0;
    if (is_present || is_submit) {
        const bool wanted = is_present ? PaceOnPresent() : !PaceOnPresent();
        void** real_slot = is_present ? reinterpret_cast<void**>(&RealQueuePresent)
                                      : reinterpret_cast<void**>(&RealQueueSubmit);
        const void* hook = is_present ? reinterpret_cast<const void*>(&HookQueuePresent)
                                      : reinterpret_cast<const void*>(&HookQueueSubmit);
        if (wanted) {
            if (!*real_slot && NextGpd) {
                void* resolved = reinterpret_cast<void*>(NextGpd(device, name));
                if (resolved && resolved != hook) {
                    *real_slot = resolved;
                }
            }
            if (*real_slot) {
                return reinterpret_cast<PFN_vkVoidFunction>(const_cast<void*>(hook));
            }
        }
    }
    if (NextGpd) return NextGpd(device, name);
    if (NextGipa) return NextGipa(nullptr, name);
    return nullptr;
}

VkResult TGOV_VKCALL HookQueuePresent(VkQueue queue, const void* info) {
    if (PaceOnPresent()) PacerVulkan().Wait();
    return RealQueuePresent(queue, info);
}

VkResult TGOV_VKCALL HookQueueSubmit(VkQueue queue, std::uint32_t count, const void* submits,
                                     void* fence) {
    if (!PaceOnPresent()) PacerVulkan().Wait();
    return RealQueueSubmit(queue, count, submits, fence);
}

}  // namespace

unsigned InstallVulkanHooks() {
    unsigned n = 0;
    if (PaceOnPresent()) {
        if (!RealQueuePresent) {
            n += HookIatAllModules("vulkan-1.dll", "vkQueuePresentKHR", reinterpret_cast<void*>(&HookQueuePresent),
                                   reinterpret_cast<void**>(&RealQueuePresent));
        }
        if (!n) {
            Logf("vulkan: no static vkQueuePresentKHR import found; dynamically loaded "
                 "titles need the layer mode, see README (vulkan.layer)");
        }
    } else {
        if (!RealQueueSubmit) {
            n += HookIatAllModules("vulkan-1.dll", "vkQueueSubmit", reinterpret_cast<void*>(&HookQueueSubmit),
                                   reinterpret_cast<void**>(&RealQueueSubmit));
        }
        if (!n) {
            Logf("vulkan: no static vkQueueSubmit import found; dynamically loaded "
                 "titles need the layer mode, see README (vulkan.layer)");
        }
    }
    return n;
}

void InstallVulkanLayerExports() {
    if (!Cfg().vulkan_layer) {
        Logf("vulkan: layer exports present but inert unless governor.dll is registered as a "
             "Vulkan layer (see README)");
    }
}

}  // namespace tgov

extern "C" {

TGOV_DLLEXPORT std::int32_t TGOV_VKCALL vkEnumerateInstanceLayerProperties(
    std::uint32_t* count, void* props) {
    return tgov::LayerEnumerateInstanceLayerProperties(count, props);
}

TGOV_DLLEXPORT std::int32_t TGOV_VKCALL vkEnumerateInstanceExtensionProperties(
    const char* layer_name, std::uint32_t* count, void* props) {
    return tgov::LayerEnumerateInstanceExtensionProperties(layer_name, count, props);
}

TGOV_DLLEXPORT void* TGOV_VKCALL vkGetInstanceProcAddr(tgov::VkInstance instance,
                                                              const char* name) {
    return reinterpret_cast<void*>(tgov::LayerGetInstanceProcAddr(instance, name));
}

TGOV_DLLEXPORT void* TGOV_VKCALL vkGetDeviceProcAddr(tgov::VkDevice device,
                                                            const char* name) {
    return reinterpret_cast<void*>(tgov::LayerGetDeviceProcAddr(device, name));
}
}
