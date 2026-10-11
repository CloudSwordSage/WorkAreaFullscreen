# run_watch_test.ps1 - 回归自检：宿主「扫描进程 -> 延迟注入」这条路径。
#
# 与 run_integration.ps1 的区别：那条走 --inject 单次注入，这条走常驻宿主的后台轮询。
# 需要先构建 Release。会短暂弹出主窗口（常驻宿主是无界面不工作的）。
param(
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release')
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$app      = Join-Path $BuildDir 'WorkAreaFullscreen.exe'
$probeExe = Join-Path $BuildDir 'probe_host.exe'
foreach ($f in @($app, $probeExe)) {
    if (-not (Test-Path $f)) { throw "缺少产物: $f（请先构建 Release）" }
}

$cfgPath = Join-Path $env:TEMP 'waf_watch_test.yaml'
$hookLog = Join-Path $env:TEMP 'WorkAreaFullscreen-hook.log'

@'
enabled: true
inject_delay_ms: 300
scan_interval_ms: 300

ui:
  minimize_to_tray: true

targets:
  - executable: probe_host.exe
    enabled: true
    strategy: sdl2-gdi
    geometry: work_area
    hooks:
      GetMonitorInfoW:
        enabled: true
        fields:
          rcMonitor: rcWork
        scope: global
      GetDeviceCaps:
        enabled: true
        indices:
          - HORZRES
          - VERTRES
        scope: global
    safety:
      verify_callsite: false
'@ | Set-Content -Path $cfgPath -Encoding utf8

Remove-Item $hookLog -ErrorAction SilentlyContinue
# 被注入的 DLL 靠该变量定位测试配置（生产部署时它自己找 %APPDATA%）
$env:HOOKKIT_CONFIG = $cfgPath

$appProc   = $null
$probeProc = $null
try {
    $appProc = Start-Process -FilePath $app -ArgumentList '--config', $cfgPath -PassThru
    Start-Sleep -Seconds 3                     # 等宿主进入扫描循环

    $probeProc = Start-Process -FilePath $probeExe -ArgumentList '9000' -PassThru
    Start-Sleep -Seconds 6                     # 覆盖扫描间隔 + 注入延迟

    if (-not (Test-Path $hookLog)) { throw '未产生 hook 日志：目标未被注入' }
    $log = Get-Content $hookLog -Raw

    if ($log -notmatch 'rule matched')            { throw "DLL 未匹配到规则：`n$log" }
    if ($log -notmatch '2 component\(s\) active') { throw "注入未完成（组件未全部安装）：`n$log" }

    Write-Host 'watch self-check: OK  (发现进程 -> 延迟 -> 自动注入 2 个组件)' -ForegroundColor Green
} finally {
    foreach ($p in @($appProc, $probeProc)) {
        if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item Env:\HOOKKIT_CONFIG -ErrorAction SilentlyContinue
    Remove-Item $cfgPath -ErrorAction SilentlyContinue
}
