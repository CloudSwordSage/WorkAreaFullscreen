# run_integration.ps1 - 集成自检：把 hookkit.dll 注入 probe_host，断言几何改写生效。
#
# 需要先构建（Release）。在 VS/VSDevShell 中运行：
#   cmake --preset release; cmake --build --preset release
#   pwsh -File tests\run_integration.ps1
param(
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release')
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$injector = Join-Path $BuildDir 'injector.exe'
$dll      = Join-Path $BuildDir 'hookkit.dll'
$probeExe = Join-Path $BuildDir 'probe_host.exe'
$cfg      = Join-Path $PSScriptRoot 'probe.yaml'

foreach ($f in @($injector, $dll, $probeExe)) {
    if (-not (Test-Path $f)) { throw "缺少产物: $f（请先构建 Release）" }
}

# DLL 通过 HOOKKIT_CONFIG 定位测试配置；injector 与目标进程都会继承该变量。
$env:HOOKKIT_CONFIG = $cfg
try {
    Push-Location $BuildDir                       # 配置里的 executable 相对当前目录解析
    try { $raw = & $injector $cfg 2>&1 | Out-String }
    finally { Pop-Location }
} finally {
    Remove-Item Env:\HOOKKIT_CONFIG -ErrorAction SilentlyContinue
}

$lines  = $raw -split "`r?`n"
$before = ($lines | Where-Object { $_ -match 'before MONITOR' }) -join ' '
$after  = ($lines | Where-Object { $_ -match 'after MONITOR' }) -join ' '
$caps   = ($lines | Where-Object { $_ -match 'after CAPS' }) -join ' '

if (-not $before -or -not $after) {
    Write-Host $raw
    throw '未捕获到 probe_host 输出'
}

function Get-Rect([string]$line, [string]$field) {
    if ($line -notmatch "$field=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)") {
        throw "解析 $field 失败: $line"
    }
    , @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
}

$beforeMonitor = (Get-Rect $before 'rcMonitor') -join ','
$afterMonitor  = (Get-Rect $after  'rcMonitor') -join ','
$workRect      = Get-Rect $before 'rcWork'
$workArea      = $workRect -join ','

if ($beforeMonitor -eq $workArea) {
    throw "前提不成立: 基线 rcMonitor 已等于 rcWork，无法验证改写"
}
if ($afterMonitor -ne $workArea) {
    throw "断言失败: 注入后 rcMonitor=$afterMonitor != rcWork=$workArea"
}

if ($caps -notmatch 'HORZRES=(\d+) VERTRES=(\d+)') {
    throw "未捕获到 after CAPS"
}
$expW = $workRect[2] - $workRect[0]
$expH = $workRect[3] - $workRect[1]
if ([int]$Matches[1] -ne $expW -or [int]$Matches[2] -ne $expH) {
    throw "断言失败: GetDeviceCaps=$($Matches[1])x$($Matches[2]) != 工作区 ${expW}x${expH}"
}

Write-Host "integration self-check: OK  (rcMonitor -> rcWork; GetDeviceCaps -> work area size)" -ForegroundColor Green
