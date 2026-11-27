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

bool VtableHook(void* com_instance, unsigned index, void* detour, void** original_out,
                void*** vtable_out);
bool VtableRestore(void** vtable, unsigned index, void* detour, void* original);

unsigned InstallD3D9Hooks();

Pacer& PacerD3D9();
Pacer& PacerDxgi();
Pacer& PacerVulkan();
Pacer& PacerOpenGl();

double FpsLimit(const char* api_key);
unsigned InstallDxgiHooks();
unsigned InstallOpenGLHooks();
unsigned InstallVulkanHooks();
void InstallVulkanLayerExports();

void SchedulerStart();
void SchedulerSweep();
void SchedulerShutdown();

void ApplyAllPacerRates();

}  // namespace tgov
