# 人脸看护：整理便携发布目录 dist\windows-bye（exe + Qt/OpenCV/InspireFace DLL + VC 运行时 + 模型）。
# 用法：先运行 scripts\setup.ps1 编译出 exe，再运行本脚本。
$ErrorActionPreference = "Stop"
$Root  = Join-Path $PSScriptRoot ".."
$Third = Join-Path $Root "third_party"
$Dist  = Join-Path $Root "dist\windows-bye"
$exe   = Join-Path $Dist "windows-bye.exe"

try {
    if (-not (Test-Path $exe)) { throw "未找到 $exe ，请先运行 scripts\setup.ps1 编译。" }

    New-Item -ItemType Directory -Path $Dist -Force | Out-Null

    # 1) Qt DLL/插件
    $QtBin = Join-Path $Third "6.8.3\msvc2022_64\bin"
    $wdq   = Join-Path $QtBin "windeployqt.exe"
    if (Test-Path $wdq) {
        Write-Host "[1/5] windeployqt ..."
        & $wdq --release --no-translations --dir $Dist $exe | Out-Null
    } else {
        Write-Warning "未找到 windeployqt.exe，跳过 Qt 部署"
    }

    # 2) OpenCV 运行库
    $OcvBin = Join-Path $Third "opencv\opencv\build\x64\vc16\bin"
    Write-Host "[2/5] 拷贝 OpenCV DLL ..."
    @("opencv_world4100.dll", "opencv_videoio_ffmpeg4100_64.dll", "opencv_videoio_msmf4100_64.dll") |
        ForEach-Object { $s = Join-Path $OcvBin $_; if (Test-Path $s) { Copy-Item $s $Dist -Force } }

    # 3) InspireFace 运行库（其 DLL 位于构建目录；静态打包了 MNN）
    Write-Host "[3/5] 拷贝 InspireFace DLL ..."
    $IsfDll = Join-Path $Third "InspireFace\build\cpp\inspireface\InspireFace.dll"
    if (Test-Path $IsfDll) {
        Copy-Item $IsfDll $Dist -Force
    } else {
        Write-Warning "未找到 InspireFace.dll"
    }

    # 4) VC++ 运行时（app-local 部署）
    #    这一步是"换电脑闪退"的关键修复：exe/Qt/OpenCV/InspireFace 都依赖
    #    MSVCP140.dll / VCRUNTIME140.dll / VCRUNTIME140_1.dll / concrt140.dll 等。
    #    开发机因为装了 Visual Studio（运行时在 System32）能跑，但目标机若没装
    #    VC++ 可再发行组件就会在启动时直接闪退。把 DLL 放到 exe 同目录，
    #    Windows 的 DLL 搜索顺序里"应用目录"优先于 System32，即可免安装运行。
    Write-Host "[4/5] 拷贝 VC++ 运行时 ..."
    $CrtDirs = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vsPath) { $CrtDirs += (Join-Path $vsPath "VC\Redist\MSVC") }
    }
    # 兜底：直接扫描常见安装位置（BuildTools / Community / Enterprise，版本号不写死）。
    $vsRoot = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio"
    if (Test-Path $vsRoot) {
        Get-ChildItem -Path $vsRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -ne "Installer" } |
            ForEach-Object { $CrtDirs += (Join-Path $_.FullName "VC\Redist\MSVC") }
    }
    $crtDir = $null
    foreach ($d in $CrtDirs) {
        if (-not (Test-Path $d)) { continue }
        # 版本目录形如 14.51.36231，取最高版本。
        $hit = Get-ChildItem -Path $d -Directory -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -match '^\d+\.' } |
               Sort-Object { [version]($_.Name) } -Descending |
               ForEach-Object { Get-ChildItem (Join-Path $_.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue } |
               Select-Object -First 1
        if ($hit) { $crtDir = $hit.FullName; break }
    }
    if ($crtDir) {
        Copy-Item (Join-Path $crtDir "*.dll") $Dist -Force
        Write-Host "      来源: $crtDir"
    } else {
        Write-Warning "未找到 VC++ 运行时目录（VC\Redist\MSVC）。目标机需自行安装 VC++ 可再发行组件，否则会闪退。"
    }

    # 5) 模型目录
    Write-Host "[5/5] 拷贝模型 ..."
    $models = Join-Path $Dist "models"
    New-Item -ItemType Directory -Path $models -Force | Out-Null
    if (Test-Path (Join-Path $Root "models\app.pack")) {
        Copy-Item (Join-Path $Root "models\app.pack") $models -Force
    } else {
        Write-Warning "未找到 models\app.pack"
    }

    Write-Host ""
    Write-Host "完成。便携目录: $Dist"
    Write-Host "将该目录整体拷贝到任意 Windows 10/11 机器，双击 windows-bye.exe 即可运行（无需安装）。"
    Write-Host "提示：可运行 scripts\check_deps.ps1 校验包内 DLL 依赖是否齐全。" -ForegroundColor DarkGray
}
catch {
    Write-Host ""
    Write-Host "打包失败: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "若未找到 exe，请先运行 scripts\setup.ps1 编译。" -ForegroundColor Yellow
    Read-Host "按回车退出"
    exit 1
}
