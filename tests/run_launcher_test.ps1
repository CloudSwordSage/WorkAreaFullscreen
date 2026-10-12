# run_launcher_test.ps1 - 集成自检：用启动代理启动目标并验证注入。
#
# 这条路径就是生产用法：HookLauncher 以调试器身份启动目标，在目标执行第一条指令之前
# 注入 injector.dll。需要先构建 Release。
param(
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release')
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [Console]::OutputEncoding = [Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)

$launcher = Join-Path $BuildDir 'HookLauncher.exe'
$probe    = Join-Path $BuildDir 'probe_host.exe'
$dll      = Join-Path $BuildDir 'injector.dll'
$cfg      = Join-Path $PSScriptRoot 'probe.yaml'

foreach ($f in @($launcher, $probe, $dll)) {
    if (-not (Test-Path $f)) { throw "缺少产物: $f（请先构建 Release）" }
}

$hookLog = Join-Path $env:TEMP 'WorkAreaFullscreen-hook.log'
$launchLog = Join-Path $env:TEMP 'WorkAreaFullscreen-launcher.log'
Remove-Item $hookLog, $launchLog -ErrorAction SilentlyContinue

# 被注入的 DLL 靠该变量定位测试配置（生产部署时它自己找 %APPDATA%）
$env:HOOKKIT_CONFIG = $cfg
try {
    $raw = & $launcher $probe 3000 2>&1 | Out-String
} finally {
    Remove-Item Env:\HOOKKIT_CONFIG -ErrorAction SilentlyContinue
}

# ---- 1) 注入时机：HookLauncher 必须报告已按配置的延迟完成注入 ----
if (-not (Test-Path $launchLog)) { throw '启动代理没有产生日志' }
$launchText = Get-Content $launchLog -Raw -Encoding utf8
if ($launchText -notmatch '已在目标启动后约 \d+ ms 完成注入') {
    Write-Host $launchText
    throw '启动代理未能报告注入完成'
}

# ---- 2) DLL 侧确认注入时的目标运行时间（这条路径应当是个位/十位毫秒）----
if (-not (Test-Path $hookLog)) { throw '未产生 hook 日志：注入未发生' }
$hookText = Get-Content $hookLog -Raw -Encoding utf8
if ($hookText -notmatch 'rule matched') { throw "DLL 未匹配到规则：`n$hookText" }

$uptimeMs = -1
if ($hookText -match '已运行\s+(\d+)\s*ms') { $uptimeMs = [int]$Matches[1] }
if ($uptimeMs -lt 0) { throw '未能从 hook 日志读到注入时的目标运行时间' }
# probe.yaml 里 inject_delay_ms: 0，因此注入应发生在目标刚起来的很短时间内
if ($uptimeMs -gt 300) {
    throw "注入时机与配置不符：目标已运行 ${uptimeMs} ms（probe.yaml 配的是 0 ms 延迟）"
}

# ---- 3) 改写结果：SDL 居中结果必须等于工作区 ----
$lines  = $raw -split "`r?`n"
$before = ($lines | Where-Object { $_ -match 'before MONITOR' }) -join ' '
$after  = ($lines | Where-Object { $_ -match 'after MONITOR' })  -join ' '
$window = ($lines | Where-Object { $_ -match 'after WINDOW' })   -join ' '
if (-not $before -or -not $window) { Write-Host $raw; throw '未捕获到 probe_host 输出' }

function Get-Rect([string]$line, [string]$field) {
    if ($line -notmatch "$field=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)") {
        throw "解析 $field 失败: $line"
    }
    , @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
}

$afterMonitor = (Get-Rect $after 'rcMonitor') -join ','
$workRect     = Get-Rect $before 'rcWork'
$workArea     = $workRect -join ','
if ($afterMonitor -ne $workArea) {
    throw "断言失败: 注入后 rcMonitor=$afterMonitor != rcWork=$workArea"
}

if ($window -notmatch 'pos=\((-?\d+),(-?\d+)\) size=\((\d+)x(\d+)\)') {
    throw "未捕获到 after WINDOW: $window"
}
$expPos  = "$($workRect[0]),$($workRect[1])"
$expSize = "$($workRect[2] - $workRect[0])x$($workRect[3] - $workRect[1])"
if ("$($Matches[1]),$($Matches[2])" -ne $expPos -or "$($Matches[3])x$($Matches[4])" -ne $expSize) {
    throw "断言失败: SDL 居中结果 ($($Matches[1]),$($Matches[2])) $($Matches[3])x$($Matches[4]) != 工作区 $expPos $expSize"
}

Write-Host "launcher self-check: OK  (启动代理注入于目标运行 ${uptimeMs} ms；rcMonitor -> rcWork；SDL 居中 = 工作区)" -ForegroundColor Green
