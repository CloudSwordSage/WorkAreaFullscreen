# run_integration.ps1 - 集成自检：让主程序对 probe_host 做一次注入，断言几何改写生效。
#
# 需要先构建（Release）：
#   cmake --preset release; cmake --build --preset release
param(
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release')
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$app      = Join-Path $BuildDir 'WorkAreaFullscreen.exe'
$probeExe = Join-Path $BuildDir 'probe_host.exe'
$dll      = Join-Path $BuildDir 'injector.dll'
$cfg      = Join-Path $PSScriptRoot 'probe.yaml'

foreach ($f in @($app, $probeExe, $dll)) {
    if (-not (Test-Path $f)) { throw "缺少产物: $f（请先构建 Release）" }
}

# 被注入的 DLL 通过 HOOKKIT_CONFIG 定位测试配置；生产部署时它自己找 %APPDATA%。
$env:HOOKKIT_CONFIG = $cfg
try {
    $raw = & $app --config $cfg --inject $probeExe 2>&1 | Out-String
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
    throw '未捕获到 after CAPS'
}
$expW = $workRect[2] - $workRect[0]
$expH = $workRect[3] - $workRect[1]
if ([int]$Matches[1] -ne $expW -or [int]$Matches[2] -ne $expH) {
    throw "断言失败: GetDeviceCaps=$($Matches[1])x$($Matches[2]) != 工作区 ${expW}x${expH}"
}

Write-Host 'integration self-check: OK  (rcMonitor -> rcWork; GetDeviceCaps -> work area size)' -ForegroundColor Green
