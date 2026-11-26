$ErrorActionPreference = 'Stop'

Write-Host ''
Write-Host '=============================================='
Write-Host '   Thermal Governor - setup helper'
Write-Host '=============================================='
Write-Host ''

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dll = Join-Path $here 'governor.dll'
$injector = Join-Path $here 'governor_injector.exe'
$iniPath = Join-Path $here 'thermal_governor.ini'

if (-not (Test-Path $dll)) {
    Write-Host '[X] governor.dll was not found next to this script.' -ForegroundColor Red
    Write-Host '    Keep governor.dll, governor_injector.exe and this script in one folder.'
    exit 1
}
if (-not (Test-Path $injector)) {
    Write-Host '[X] governor_injector.exe was not found next to this script.' -ForegroundColor Red
    Write-Host '    Keep governor.dll, governor_injector.exe and this script in one folder.'
    exit 1
}

function Read-Number($prompt, $default) {
    $value = Read-Host "$prompt [$default]"
    if ([string]::IsNullOrWhiteSpace($value)) { return $default }
    while (-not ($value -match '^\d+(\.\d+)?$')) {
        $value = Read-Host 'Please enter a plain number (or press Enter)'
        if ([string]::IsNullOrWhiteSpace($value)) { return $default }
    }
    return [double]$value
}

Add-Type -AssemblyName System.Windows.Forms

$dlg = New-Object System.Windows.Forms.OpenFileDialog
$dlg.Title = 'Pick your game (the .exe you normally double-click)'
$dlg.Filter = 'Game executables (*.exe)|*.exe|All files (*.*)|*.*'
if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
    Write-Host 'Cancelled. Nothing was changed.'
    exit 1
}
$game = $dlg.FileName

Write-Host ''
Write-Host 'Game selected:'
Write-Host "  $game" -ForegroundColor Cyan
Write-Host ''
$fps = Read-Number 'Frame limit (0 = no limit)' 60
$cpu = Read-Number 'Max CPU usage in percent (0 = no cap)' 75
$cores = Read-Number 'Reserve idle cores, 0-4. More = cooler, slightly slower' 0
if ($cores -gt 4) { $cores = 4 }
$spin = Read-Number 'Pacing precision 0-4 milliseconds (2 is a good default)' 2
if ($spin -gt 4) { $spin = 4 }

$cpuPermille = [int]([math]::Round($cpu * 100))
if ($cpuPermille -gt 10000) { $cpuPermille = 10000 }
if ($cpu -eq 0) { $cpuPermille = 0 }

$ini = @"
[governor]
enabled = true

[cpu]
; Max total CPU usage. 7500 means 75% of the whole machine. 0 = no cap.
cpu_rate_permille = $cpuPermille

; Logical cores the game may NOT use (0 = game can use everything).
reserve_cores = $([int]$cores)

; Lower priority keeps the game out of the way of the rest of Windows.
priority_class = below_normal

; Newly spawned game threads get the same limits.
apply_to_new_threads = true
thread_sweep_ms = 250

[memory]
commit_limit_mb = 0

[framerate]
; fps_limit is used for every graphics API unless a specific one is set.
fps_limit = $fps
fps_d3d9 = 0
fps_dxgi = 0
fps_vulkan = 0
fps_opengl = 0

[present]
; -1 = let the game decide vsync, 0 = force off, 1..4 = force vsync.
dxgi_force_sync_interval = -1

[pacing]
; Precision window in milliseconds. Higher = smoother edges, a bit more CPU.
spin_guard_ms = $([int]$spin)

[power]
; Optional: balanced | high | ultimate | saver
plan =

[vulkan]
layer = false
pace_point = queue_present
"@
Set-Content -Path $iniPath -Value $ini -Encoding ASCII

Write-Host ''
Write-Host "[OK] Settings saved to $iniPath" -ForegroundColor Green

$attach = Read-Host 'Is the game already running? (y/N)'
$mode = 'launch'
if ($attach -match '^[Yy]') {
    $mode = 'attach'
}

Write-Host ''
Write-Host 'Starting the game with the governor...' 
Write-Host ''

if ($mode -eq 'launch') {
    & $injector --launch $game $dll
} else {
    $name = [System.IO.Path]::GetFileName($game)
    & $injector --pid $name $dll
}

Write-Host ''
Write-Host '----------------------------------------------'
Write-Host 'If anything looks wrong, check the log:'
Write-Host "  $here\governor.log"
Write-Host 'Tip: settings can be changed at any time with this same helper.'
Write-Host '----------------------------------------------'
exit 0
