# Windows Bye —— 便携包 DLL 依赖校验 / 导出清单。
#
# 解决的问题：dist\windows-bye 拷到"没装 VC++ 运行时的干净机器"上会闪退。
# 根因是包内缺少 msvcp140.dll / vcruntime140.dll / vcruntime140_1.dll / concrt140.dll
# 等 VC++ 运行时——开发机因为装了 Visual Studio（运行时在 System32）能跑，
# 目标机没有就启动即闪退。本脚本把"被依赖、但既不在包内、也不是 Windows 自带"
# 的 DLL 全部列出来，它们就是闪退元凶；package.ps1 已负责把这些 DLL 拷进包。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\check_deps.ps1           # 只报缺失项
#   powershell -ExecutionPolicy Bypass -File scripts\check_deps.ps1 -All      # 另附完整依赖清单
#   powershell -ExecutionPolicy Bypass -File scripts\check_deps.ps1 -OutFile deps.txt
# 退出码：0=依赖齐全；1=有缺失；2=环境问题（找不到 dumpbin 或 dist 目录）。
param(
    [switch]$All,
    [string]$OutFile
)
$ErrorActionPreference = "Stop"

$Root = Join-Path $PSScriptRoot ".."
$Dist = Join-Path $Root "dist\windows-bye"

if (-not (Test-Path $Dist)) { Write-Host "未找到便携目录: $Dist（请先运行 scripts\package.ps1）" -ForegroundColor Red; exit 2 }

# ---- 定位 dumpbin.exe（随 Visual Studio 提供，用于读取 PE 导入表）----
function Find-Dumpbin {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $inst = $null
    if (Test-Path $vswhere) {
        $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    }
    if ($inst) {
        $hit = Get-ChildItem (Join-Path $inst "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue |
               Sort-Object { [version]($_.Name) } -Descending |
               ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\dumpbin.exe" } |
               Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($hit) { return $hit }
    }
    # 兜底：直接扫常见安装位置（版本号不写死）。
    foreach ($ed in @("BuildTools", "Community", "Professional", "Enterprise")) {
        $base = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\18\$ed\VC\Tools\MSVC"
        if (-not (Test-Path $base)) { continue }
        $hit = Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
               Sort-Object { [version]($_.Name) } -Descending |
               ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\dumpbin.exe" } |
               Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($hit) { return $hit }
    }
    return $null
}

$Dumpbin = Find-Dumpbin
if (-not $Dumpbin) { Write-Host "未找到 dumpbin.exe（需安装带 C++ 工作负载的 Visual Studio）" -ForegroundColor Red; exit 2 }
Write-Host "使用 dumpbin: $Dumpbin"

# ---- Windows 系统自带 DLL（无需随包，也不代表"干净机器上一定没有"的 VC 运行时）----
# 注意：这里刻意"不"包含 msvcp140/vcruntime140/concrt140 等 VC++ 运行时，
# 它们必须随包发布，否则目标机没装 VC++ 可再发行组件就会闪退。
$SysPrefix = @('api-ms-win-', 'ext-ms-win-')
$Sys = @(
    'kernel32.dll','user32.dll','advapi32.dll','shell32.dll','ole32.dll','oleaut32.dll',
    'gdi32.dll','gdi32full.dll','ws2_32.dll','winmm.dll','version.dll','netapi32.dll',
    'userenv.dll','mpr.dll','authz.dll','wtsapi32.dll','d3d11.dll','dxgi.dll','dwmapi.dll',
    'opengl32.dll','glu32.dll','bcrypt.dll','crypt32.dll','dnsapi.dll','iphlpapi.dll',
    'secur32.dll','shlwapi.dll','comdlg32.dll','uxtheme.dll','dwrite.dll','d2d1.dll',
    'propsys.dll','setupapi.dll','cfgmgr32.dll','rpcrt4.dll','combase.dll','msvcrt.dll',
    'ntdll.dll','imm32.dll','mswsock.dll','d3d9.dll','dxva2.dll','avrt.dll','mfplat.dll',
    'mf.dll','mfreadwrite.dll','mfuuid.dll','strmiids.dll','oleacc.dll','wldap32.dll',
    'normaliz.dll','psapi.dll','powrprof.dll','dbghelp.dll','winhttp.dll','wininet.dll',
    'urlmon.dll','ncrypt.dll','dcomp.dll','windowscodecs.dll','twinapi.dll','hid.dll',
    'ksuser.dll','dsound.dll','pdh.dll','sechost.dll','imagehlp.dll','wintrust.dll',
    'cryptbase.dll','gpapi.dll','d3d12.dll','cryptui.dll','winspool.drv','shcore.dll',
    'd3dcompiler_47.dll','msimg32.dll','usp10.dll','d3d10warp.dll','uiautomationcore.dll',
    'cryptsp.dll','dpapi.dll','slc.dll','wlanapi.dll','wer.dll','cryptdll.dll','ncryptsslp.dll'
)

function Get-PeImports([string]$file) {
    $out = & $Dumpbin /DEPENDENTS $file 2>$null
    $names = New-Object System.Collections.Generic.List[string]
    $inSection = $false
    foreach ($ln in $out) {
        if ($ln -match 'Image has the following dependencies') { $inSection = $true; continue }
        if (-not $inSection) { continue }
        if ($ln -match '^\s*Summary') { break }
        $m = [regex]::Match($ln, '([A-Za-z0-9_.\-]+\.dll)')
        if ($m.Success) { $names.Add($m.Groups[1].Value) }
    }
    return $names
}

# ---- 收集包内所有 PE 文件与已有文件名 ----
$peFiles = Get-ChildItem $Dist -Recurse -File -ErrorAction SilentlyContinue |
           Where-Object { $_.Extension -in '.dll', '.exe' }
$have = New-Object System.Collections.Generic.HashSet[string] ([StringComparer]::OrdinalIgnoreCase)
Get-ChildItem $Dist -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { [void]$have.Add($_.Name) }

# ---- 逐文件解析导入，汇总"谁依赖了谁" ----
$deps = @{}
foreach ($f in $peFiles) {
    foreach ($d in (Get-PeImports $f.FullName)) {
        $key = $d.ToLower()
        if (-not $deps.ContainsKey($key)) { $deps[$key] = New-Object System.Collections.Generic.List[string] }
        $deps[$key].Add($f.Name)
    }
}

# ---- 判定缺失：被依赖 && 不在包内 && 非系统 DLL ----
$missing = @()
foreach ($d in ($deps.Keys | Sort-Object)) {
    if ($have.Contains($d)) { continue }
    if ($Sys -contains $d) { continue }
    $isSysPrefix = $false
    foreach ($p in $SysPrefix) { if ($d.StartsWith($p)) { $isSysPrefix = $true; break } }
    if ($isSysPrefix) { continue }
    $missing += $d
}

Write-Host ""
if ($All) {
    Write-Host "=== 全部依赖（去重）===" -ForegroundColor Cyan
    foreach ($d in ($deps.Keys | Sort-Object)) {
        Write-Host ("  {0,-34} <- {1}" -f $d, (($deps[$d] | Select-Object -Unique) -join ', '))
    }
    Write-Host ""
}

Write-Host "=== 缺失的非系统 DLL（会导致换机闪退）===" -ForegroundColor Cyan
if ($missing.Count -eq 0) {
    Write-Host "  (无) —— 便携包依赖齐全，可免安装运行。" -ForegroundColor Green
} else {
    foreach ($d in $missing) {
        Write-Host ("  {0}" -f $d) -ForegroundColor Red
        Write-Host ("      <- {0}" -f (($deps[$d] | Select-Object -Unique) -join ', '))
    }
    Write-Host ""
    Write-Host ("共 {0} 个缺失项。请重新运行 scripts\package.ps1 补齐（其中 VC++ 运行时由脚本自动拷贝）。" -f $missing.Count) -ForegroundColor Yellow
}

if ($OutFile) {
    $lines = @("=== 全部依赖 ===")
    foreach ($d in ($deps.Keys | Sort-Object)) {
        $lines += ("{0} <- {1}" -f $d, (($deps[$d] | Select-Object -Unique) -join ', '))
    }
    $lines += ""
    $lines += "=== 缺失的非系统 DLL ==="
    if ($missing.Count -eq 0) { $lines += "(无)" } else { $lines += $missing }
    Set-Content -Path $OutFile -Value $lines -Encoding UTF8
    Write-Host "依赖清单已写出: $OutFile" -ForegroundColor Cyan
}

if ($missing.Count -gt 0) { exit 1 }
exit 0
