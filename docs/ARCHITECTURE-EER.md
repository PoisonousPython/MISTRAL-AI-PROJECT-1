# Thermal Governor — EER Architecture

Enhanced Entity-Relationship model of the project. This document is the contract
every code change must satisfy: **a change compiles, passes tests, and maps onto an
entity or relationship here — or it does not go in.** When the two disagree, fix the
code or the model and retry until both hold, then edit the main project files.

## Project goal (single sentence)

> Let a non-coding player bound how much heat a closed Windows game generates —
> by pacing its frame surfacing, capping its CPU and memory, and steering its
> threads — without modifying the game's code.

## Goal decomposition (what the project NEEDS to do to succeed)

| # | Capability | Why the goal fails without it | Owning entity |
| --- | --- | --- | --- |
| G1 | Get loaded into the game process invisibly and reliably | Nothing else can act inside the game | INJECTOR, GOVERNOR_DLL |
| G2 | Read player intent once, as plain data | No runtime interaction with the player is possible | CONFIG, PARSER, CONTROL_SURFACE |
| G3 | Slow/limit frame surfacing per graphics API | Present rate is the dominant GPU→heat path | CAPTURE, PRESENT_GATEWAY, PACER |
| G4 | Bound CPU rate, core placement, priority, commit | CPU work is the dominant CPU→heat path | SCHEDULER |
| G5 | Fail safe: unconfigured = untouched process | A misconfigured tool that breaks games is discarded | RUNTIME (lifecycle) |
| G6 | Ship to non-coders as a download that just works | The goal's audience cannot build software | DISTRIBUTION |
| G7 | Grow toward a GUI without redesign | Control surface is the only part allowed to change shape | CONTROL_SURFACE, CONFIG file contract |

## Entities and attributes

### CONFIG (plain INI data)
- `enabled: bool`
- `cpu_rate_permille: 0..10000` (Job-Object percent×100; 0 = no cap)
- `affinity_mask: u64`, `reserve_core_count: 0..63`
- `priority_class: {normal, below_normal, idle, high}`
- `reapply_thread_sched: bool`, `thread_sweep_ms: 50..60000`
- `commit_limit_mb: u64` (0 = unlimited)
- `fps_global, fps_d3d9, fps_dxgi, fps_vulkan, fps_opengl: double` (0 = uncapped)
- `dxgi_force_sync_interval: -1..4`
- `spin_guard_ms: 0..15`
- `power_plan: {balanced, high, ultimate, saver, GUID}`

### PACER (frame cadence holder)
- `interval: duration`, `last: timestamp`, `spin_guard: ms`, `paces: counter`
- Contract: first `Wait` = baseline; early `Wait` sleeps to slot; late `Wait` resets
  cadence (no catch-up burst). Sleep happens **outside** its mutex.

### PRESENT_GATEWAY *(supertype — one specialization per graphics API)*
- Attributes: `api`, `pacing_point`, `sync_interval_override`
- Specializations and their fixed ABI slots:
  - `DX9_PRESENT`: `Present`=17, `PresentEx`=121 (via `IDirect3DDevice9Ex`)
  - `DXGI_PRESENT` (D3D10/11/12): `Present`=8, `Present1`=22, optional SyncInterval override
  - `VULKAN_PRESENT`: `vkQueuePresentKHR` or `vkQueueSubmit` (chosen by `pace_point`)
  - `OPENGL_PRESENT`: `wglSwapBuffers`, `wglSwapLayerBuffers`, `eglSwapBuffers`
- Law: calls the real function after pacing; never drops a frame.

### CAPTURE *(supertype — creation-time interception)*
- Attributes: `entry_point(s)`, `hook_primitive` ∈ {IAT, vtable}
- Specializations:
  - `DX9_CAPTURE`: `Direct3DCreate9{,Ex}` (IAT) → `CreateDevice`=16 / `CreateDeviceEx`=17 (vtable)
  - `DXGI_CAPTURE`: `CreateDXGIFactory{,1,2}` (IAT) + `D3D1xCreateDeviceAndSwapChain*`
    (IAT fallback) → factory slots 10/15/16/24 (vtable) → swapchain
  - `OPENGL_CAPTURE`: `wglSwap*`/`eglSwapBuffers` (IAT, direct pacing — no object capture)
  - `VULKAN_CAPTURE`: IAT mode **or** loader-layer mode (`VK_LAYER_THERMAL_GOVERNOR`,
    `VK_LAYER_LINK_INFO` chaining, self-recursion guard)
- Law: IAT + COM vtable only. No byte patching, no detour trampolines.

### HOOK_PLUMBING
- `HookIatAllModules(module, function, detour) → sites-hooked`
- `VtableHook(obj, slot, detour) → original` (fire-and-forget; the DLL unloads with the process, so there is no restore path)
- `VirtualProtect` scoped to the exact 8-byte slot being written.

### PE_PARSER
- Portable reader over raw mapped images: MZ/PE validation, opt-header magic
  (0x10B/0x20B), import descriptor walk, name→IAT-slot resolution.
- Little-endian targets only (documented constraint).

### SCHEDULER
- `job: JobObject` (CPU-rate hard cap, commit limit), `thread_mask: u64`,
  `own_thread_ids: set`, `sweep_period: ms`
- Powers the game process via: priority class, affinity (explicit or reserved-low-cores),
  power plan, and re-sweeps newly spawned game threads.
- Reload semantics: setting a value to 0 removes that limit. Shutdown/disable clears
  both caps while the job handle still exists, then releases it.
- Existence order inside one pass: mask built from CONFIG → priority → power plan →
  job caps; nothing applies before its input values exist.

### RUNTIME (governor lifecycle)
- `state ∈ {idle, active}`; worker thread; reload entry point (`GovernorReload`);
  log file. `enabled = false` ⇒ state stays `idle`, process untouched (G5).
- Reload is the single gate: it re-reads CONFIG, re-applies PACER rates, installs
  the CAPTURE hooks when `enabled` is true (idempotent, so an enable at runtime
  still installs), and starts or stops the SCHEDULER to match.

### GOVERNOR_DLL
- The loadable agent. Aggregates RUNTIME + CONFIG + 4×PACER + CAPTURE/SCHEDULER wiring.

### INJECTOR (tool)
- `--launch` (CREATE_SUSPENDED → inject → resume) | `--pid <id|name.exe>`
- Guards: target must exist, must be 64-bit (WOW64 check — explicit player-readable
  error otherwise). Never leaves a half-launched game behind.

### CONTROL_SURFACE *(supertype — produces CONFIG, invokes INJECTOR)*
- `WIZARD` (now): `governor_setup.exe` (C++ console, COM file dialog), 4 questions, writes INI, launches.
- `GUI` (future): same two responsibilities, nicer shell. Replaces nothing else.
- GUI (later) will read CONFIG values and LOG output; no status struct is kept in code before a consumer exists.

### HOST_PROCESS
- The game. Attributes observed: pid, exe name, bitness. Never modified in code.

### DISTRIBUTION
- CI: Linux tests + Windows MSVC build; zip artifact
  (`governor.dll`, `governor_injector.exe`, CONTROL_SURFACE, sample CONFIG);
  GitHub Release on `v*` tags.

### LOG
- Append-only timestamped lines from RUNTIME, CAPTURE, SCHEDULER. Player-visible
  diagnostics (`governor.log` next to the DLL).

## Relationships

| Relationship | Cardinality | Notes |
| --- | --- | --- |
| HOST_PROCESS —has→ GOVERNOR_DLL | 1 : 0..1 | 0 = unconfigured/idle (G5) |
| GOVERNOR_DLL —holds→ CONFIG | 1 : 1 | snapshot, re-read on reload |
| GOVERNOR_DLL —runs→ RUNTIME | 1 : 1 | lifecycle owner |
| CONTROL_SURFACE —writes→ CONFIG | N : 1 | WIZARD now, GUI later |
| CONTROL_SURFACE —invokes→ INJECTOR | N : 1 | one call per play session |
| INJECTOR —loads→ GOVERNOR_DLL | N : 1 | into HOST_PROCESS |
| GOVERNOR_DLL —instantiates→ PACER | 1 : 4 | one per API specialization |
| PACER —paces→ PRESENT_GATEWAY | 1 : 1 | per API (paired specializations) |
| CAPTURE —discovers→ PRESENT_GATEWAY | 1 : N | objects appear at runtime |
| CAPTURE, PRESENT_GATEWAY —use→ HOOK_PLUMBING | N : 2 | IAT, vtable |
| HOOK_PLUMBING —queries→ PE_PARSER | N : 1 | slot resolution |
| PE_PARSER —reads→ MODULE_LAYOUT | 1 : N | every loaded module |
| SCHEDULER —controls→ HOST_PROCESS | 1 : 1 | job + priority + power plan |
| SCHEDULER —applies→ GAME_THREAD | 1 : N | mask re-swept for new threads |
| RUNTIME, CAPTURE, SCHEDULER —write→ LOG | N : 1 | append-only |
| DISTRIBUTION —packages→ {GOVERNOR_DLL, INJECTOR, CONTROL_SURFACE, CONFIG} | 1 : 4 | zip + release |

## Module ↔ entity mapping (source layout after modularization)

| Directory | Entity(ies) | Files | CMake target |
| --- | --- | --- | --- |
| `src/core/` | CONFIG, PACER, CONTROL_SURFACE contract, LOG (interface) | `tgov_types.h`, `tgov_log.h`, `config.cpp`, `pacer.cpp` | `tgov_core` |
| `src/pe/` | PE_PARSER | `pe.cpp` | `tgov_pe` |
| `src/hooks/` | HOOK_PLUMBING, CAPTURE×4, PRESENT_GATEWAY×4, LOG (implementation) | `tgov_win.h`, `iat.cpp`, `hooks_dx9.cpp`, `hooks_dxgi.cpp`, `hooks_opengl.cpp`, `hooks_vulkan.cpp` | `tgov_hooks` (WIN32) |
| `src/scheduler/` | SCHEDULER | `scheduler.cpp` | `tgov_sched` (WIN32) |
| `src/runtime/` | RUNTIME, GOVERNOR_DLL | `governor.cpp` | `governor` (SHARED) |
| `tools/` | INJECTOR, CONTROL_SURFACE (WIZARD) | `injector.cpp`, `governor_setup.cpp` | `governor_injector`, `governor_setup` (WIN32) |
| `tests/` | PE_PARSER, CONFIG, PACER contracts | `test_portable.cpp` | `tgov_tests` |
| `docs/` | this model | `ARCHITECTURE-EER.md` | — |

Dependency direction (allowed edges only):

```
runtime → scheduler, hooks → {core, pe} → (nothing)
tools/injector, tools/governor_setup  (standalone; share only the CONFIG file contract)
```

CONFIG's storage and access point (`Config& Cfg()`) live in `src/core/config.cpp`
(implementation, not just the declaration) — one codebase for the whole CONFIG
entity. `CONFIG.enabled` gates everything: RUNTIME installs hooks and starts the
SCHEDULER only when it is true, and reload re-evaluates it.

The LOG interface lives in `src/core/tgov_log.h` so every writer listed above
(RUNTIME, CAPTURE, SCHEDULER) can reach it along an allowed edge; its
implementation lives in HOOK_PLUMBING (`iat.cpp`) and is linked only into the
final DLL.

## Change protocol (from the project owner)

1. Decide which entity/relationship the change touches; update this file in the
   same change if you add/alter either.
2. Compile (portable tests + Windows TU typecheck + CI). If it does not compile,
   fix and retry — never weaken the model to make code pass.
3. Only then edit the main project files, keeping each entity's attributes inside
   its owning module (no cross-module reach-ins; data flows along the edges above).
