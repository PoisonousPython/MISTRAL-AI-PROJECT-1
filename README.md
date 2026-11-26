# MISTRAL-AI-PROJECT-1
Just wanted to create a manual scheduler for games.

---

# Thermal Governor

A single injectable Windows DLL (`governor.dll`) plus a launch-time injector that gives
closed-binary PC games external pacing, CPU, and memory controls — the practical,
process-level analogue of the deliberate work placement PS3 developers did with SPURS:
you decide how much CPU the game may use, which cores it may use, when frames are
allowed to surface, and how much memory it may commit. Works with D3D9, D3D10, D3D11,
D3D12 (DXGI swapchains), OpenGL/WGL, ANGLE (EGL), and Vulkan.

Everything here is external. The governor never modifies game code, never patches
instructions, and never touches a game's internal allocator or scheduler — see
[Honest limitations](#honest-limitations).

## What it does

| Control | Mechanism | Config |
| --- | --- | --- |
| FPS cap (frame pacing) | Pre-present sleep in hooked present calls, per API | `framerate.*`, `pacing.spin_guard_ms` |
| Whole-process CPU hard cap | Job Object `JOB_OBJECT_CPU_RATE_CONTROL` (Windows 8+) | `cpu.cpu_rate_permille` |
| Core placement | `SetThreadAffinityMask` on every thread, re-swept for new threads; or "reserve N low cores" PS3-style | `cpu.affinity_mask`, `cpu.reserve_cores` |
| Thread priority | `SetPriorityClass` for the whole process | `cpu.priority_class` |
| Memory commit cap | Job Object `JOB_OBJECT_LIMIT_PROCESS_MEMORY` | `memory.commit_limit_mb` |
| Power plan | `PowerSetActiveScheme` at start (optional) | `power.plan` |
| Forced vsync / swap interval | Overrides `SyncInterval` on D3D10/11/12 presents | `present.dxgi_force_sync_interval` |

The frame pacer is pre-present (the RTSS approach): it sleeps *before* calling the
game's real present, holding a clean cadence. A late frame resets the cadence instead
of scheduling a catch-up burst. A precision spin window (default 2 ms) covers the tail
of each wait that Windows' `Sleep(1)` grain cannot. `timeBeginPeriod(1)` is set while
loaded.

## Build

Needs CMake 3.20+ and one of:

- Visual Studio 2022 (or Build Tools): `cmake -B build -A x64 && cmake --build build --config Release`
- MinGW-w64: `cmake -B build -G "MinGW Makefiles" && cmake --build build`, add
  `-static -static-libgcc -static-libstdc++` to the link flags if you want a
  dependency-free DLL.

Outputs:

- `governor.dll` — the governor itself
- `governor_injector.exe` — launch/attach injector

## Usage

### 1. Configure

Put `config/thermal_governor.ini` next to `governor.dll`, named `thermal_governor.ini`
(the DLL loads the file from its own directory and logs to `governor.log` in the same
place). Defaults are conservative: caps off, `enabled = false` means the DLL stays idle
after injection.

### 2. Launch

```
governor_injector.exe --launch "C:\Games\SomeGame\SomeGame.exe" --args "-fullscreen" "C:\Tools\thermal-governor\governor.dll"
governor_injector.exe --pid 12345 "C:\Tools\thermal-governor\governor.dll"
governor_injector.exe --pid SomeGame.exe "C:\Tools\thermal-governor\governor.dll"
```

`--launch` starts the process suspended, injects, then resumes — this is the reliable
path because every graphics API is captured from process start. `--pid` attaches to a
running process (id or executable name); hooks installed late still catch every
subsequent present, but swapchains created before injection are simply paced from the
next present onward — no crash, just later coverage.

### 3. Reload

Edits to the INI are picked up by calling the DLL's exported `GovernorReload` (or by
re-injecting; the worker also re-runs scheduler sweeps continuously).

## Hooking design (why it is the safe subset)

Only two hooking primitives are used, both documented COM/PE facts, no byte patching:

1. **IAT hooks** — the DLL walks every loaded module's import tables and replaces
   entries such as `d3d9.dll!Direct3DCreate9`, `dxgi.dll!CreateDXGIFactory{,1,2}`,
   `opengl32.dll!wglSwapBuffers`, `vulkan-1.dll!vkQueuePresentKHR`. The game still
   calls the real functions; the governor just observes what they return.
2. **COM vtable hooks** — objects gained that way (device, swapchain, factory) get
   their `Present` slots replaced. Slot indices are fixed by the ABI, so they are
   constants, not guesses — [verified below](#vtable-index-verification).

Effects: capturing swapchains at creation time, pacing presents, overriding the
swap interval. `VirtualProtect` is applied only to the exact 8-byte slot being written.

## Vtable index verification

These indices were cross-checked against independent sources (Wine's IDL files,
ReShade's DXGI implementation, OBS Studio's graphics-hook plugin), not from memory:

| Interface | Method | Index | Source |
| --- | --- | --- | --- |
| `IDXGISwapChain` | `Present` | 8 | Wine `dxgi.idl`; ReShade `dxgi.cpp` |
| `IDXGISwapChain1` | `Present1` | 22 | ReShade `dxgi.cpp` |
| `IDXGIFactory` | `CreateSwapChain` | 10 | Wine `dxgi.idl` |
| `IDXGIFactory2` | `CreateSwapChainForHwnd` | 15 | ReShade |
| `IDXGIFactory2` | `CreateSwapChainForCoreWindow` | 16 | Wine IDL method order |
| `IDXGIFactory2` | `CreateSwapChainForComposition` | 24 | ReShade |
| `IDirect3D9` | `CreateDevice` | 16 | Wine `d3d9.idl` ordering |
| `IDirect3D9Ex` | `CreateDeviceEx` | 17 | Wine `d3d9.idl` ordering |
| `IDirect3DDevice9` | `Present` | 17 | Wine IDL; widely used in capture tools |
| `IDirect3DDevice9Ex` | `PresentEx` | 121 | OBS Studio `d3d9-capture.cpp` |
| `IDirect3DSwapChain9` | `Present` | 3 | OBS Studio (not used here, noted for reference) |

## Vulkan notes

Two modes:

- **IAT mode (default)** — works for titles that statically import
  `vkQueuePresentKHR` (or `vkQueueSubmit`; choose with `vulkan.pace_point`).
  Most native Win32 Vulkan games do.
- **Layer mode (`vulkan.layer = true`)** — the DLL also exports the four
  layer entry points (`vkEnumerateInstance{Layer,Extension}Properties`,
  `vk{Instance,Device}GetProcAddr`) under the name `VK_LAYER_THERMAL_GOVERNOR`, and
  participates in loader chaining via `VK_LAYER_LINK_INFO`. For titles that resolve
  all entry points dynamically, register it:

  1. Create `governor.json` next to the DLL:
     ```json
     {
       "file_format_version": "1.0.0",
       "layer": {
         "name": "VK_LAYER_THERMAL_GOVERNOR",
         "type": "GLOBAL",
         "library_path": ".\\governor.dll",
         "api_version": "1.1.0",
         "description": "Thermal governor: frame pacing and CPU placement"
       }
     }
     ```
  2. `set VK_LAYER_PATH=C:\Tools\thermal-governor`
  3. Enable it: `set VK_INSTANCE_LAYERS=VK_LAYER_THERMAL_GOVERNOR`

  This layer path is secondary: if the game never loads the layer, the IAT hooks
  still apply, and if the layer loads, the guard rejects any self-recursive proc
  address so the hooks cannot stack on themselves.

## Honest limitations

Read this before judging the design:

- **This cannot replace a game's internal allocator or scheduler.** A closed binary's
  internal memory layout, thread pool, and job graph are its own. What an external
  DLL can do is exactly what is implemented here: cap the process's total CPU, steer
  which logical cores its threads run on, deprioritize it, cap its commit, pace its
  frame surfacing, and force the swap interval. That set of levers is what actually
  reduces heat/throttling on Windows; the PS3 analogy is directional, not literal —
  the PS3 reserved cores at build time, which is only possible when you own the source.
- **CPU-rate caps require Windows 8+** and fail on some hosts when the game is already
  inside an incompatible job (some launchers create one). The log will say so and the
  rest of the controls still apply.
- **Dynamically-loaded Vulkan functions** are invisible to IAT hooks; that is what
  layer mode addresses above.
- **Injection can trigger anti-cheat.** EAC/BattlEye-style anti-cheat treats DLL
  injection as hostile and can ban the account. Use this on single-player games or
  your own builds only. This is inherent to injection, not specific to this tool.
- Mature prior art doing overlapping work: Special K (present hooks, pacing), RTSS
  (pre-present pacing). The scheduler side (job object CPU rate + affinity sweeps) is
  the part those tools do not do.

## Verification done on this tree

- In-memory PE64/PE32 import-table parsing is covered by a portable test suite
  (`tests/test_portable.cpp`, builds on any platform): dual-site matching across two
  descriptors, case-insensitive module matching, original-pointer capture,
  idempotency, malformed-input rejection, INI parsing, and pacer cadence math —
  all green (`cmake --build build && ctest`).
- All Windows translation units type-check cleanly under `-Wall` (MSVC and
  MinGW-w64 compatible: no MSVC-only extensions required, all function-pointer
  transitions through `reinterpret_cast`).
- Runtime verification must happen on Windows — compile, launch a test via the
  injector with an FPS cap, and confirm `governor.log` shows the hooks installed and
  the pacer engaging. `enabled = false` leaves the process untouched.

## Layout

```
src/tgov_types.h      portable core: config struct, pacer, parse helpers, PE API
src/tgov_win.h        Windows glue declarations
src/config.cpp        INI reader
src/pacer.cpp         frame pacing math (portable core)
src/pe.cpp            import-table walker + IAT overwrite (own byte-level parser)
src/iat.cpp           logging, self-module discovery, IAT hooks across all modules
src/scheduler.cpp     job object, affinity sweeps, priority, power plan
src/hooks_dx9.cpp     D3D9/D3D9Ex capture + Present/PresentEx
src/hooks_dxgi.cpp    D3D10/11/12 via DXGI capture-path + Present/Present1
src/hooks_opengl.cpp  WGL + ANGLE/EGL swap pacing
src/hooks_vulkan.cpp  IAT pacing + optional real Vulkan layer
src/governor.cpp      lifecycle, config reload, worker thread
tools/injector.cpp    suspended-launch / attach injector
config/               sample INI, fully documented
tests/                portable test suite
```
