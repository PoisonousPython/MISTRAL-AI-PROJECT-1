#pragma once

#include "tgov_types.h"
#include "tgov_log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#if defined(_WIN32)
#define TGOV_DLLEXPORT __declspec(dllexport)
#else
#define TGOV_DLLEXPORT
#endif

namespace tgov {

std::string SelfDirectory();
HMODULE SelfModule();
void InitSelfModule();

unsigned HookIatAllModules(const char* module_name, const char* function_name,
                           void* detour, void** original_out);

void VtableHook(void* com_instance, unsigned index, void* detour, void** original_out);

Pacer& PacerD3D9();
Pacer& PacerDxgi();
Pacer& PacerVulkan();
Pacer& PacerOpenGl();

void InstallD3D9Hooks();
void InstallDxgiHooks();
void InstallOpenGLHooks();
void InstallVulkanHooks();
void InstallVulkanLayerExports();

void SchedulerStart();
void SchedulerSweep();
void SchedulerShutdown();

}  // namespace tgov
