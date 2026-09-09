# Windows Bye —— 检查并安装环境，然后完成一次完整构建。
# 一个入口从零走到底：确保工具链(MSVC/ninja) → 拉依赖(Qt/OpenCV/InspireFace/模型) →
# 编译并安装 InspireFace → 编译本应用，产出 exe。产物打包见 package.ps1。
#   powershell -ExecutionPolicy Bypass -File scripts\setup.ps1           # 全新环境：全部步骤
#   powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 -SkipDeps # 依赖已拉好：跳过联网下载，直接编
param([switch]$SkipDeps)
$ErrorActionPreference = "Stop"

$Root  = Join-Path $PSScriptRoot ".."
$Third = Join-Path $Root "third_party"

# ============ 0) 公共小函数 ============
function Invoke-ProxyDownload($Url, $Out) {
    if ($env:HTTPS_PROXY) { curl.exe -L --fail --proxy $env:HTTPS_PROXY -o $Out $Url }
    else                  { curl.exe -L --fail -o $Out $Url }
    if ($LASTEXITCODE -ne 0) { throw "下载失败: $Url" }
}

# ============ 1) 确保 ninja 可用 ============
function Ensure-NinjaOnPath {
    if (Get-Command ninja -ErrorAction SilentlyContinue) { return }
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $py) { throw "ninja 不在 PATH 且未找到 python 以安装 ninja。" }
    $venv = Join-Path $Third "ci-venv"
    if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
        & python -m venv $venv
        if ($LASTEXITCODE -ne 0) { throw "创建 python venv 失败: $venv" }
    }
    $env:PATH = (Join-Path $venv "Scripts") + ";" + $env:PATH
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        & (Join-Path $venv "Scripts\pip.exe") install --quiet --disable-pip-version-check ninja
        if ($LASTEXITCODE -ne 0) { throw "安装 ninja 失败" }
    }
}

# ============ 2) 定位 MSVC vcvars64.bat ============
function Get-VcvarsPath {
    $cands = @(
        (Join-Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer" "vswhere.exe"),
        "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe",
        "C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    $vswhere = $cands | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $vswhere) { throw "未找到 vswhere.exe（请安装 Visual Studio Installer）" }
    $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $inst) { throw "未找到安装 C++ 工作负载的 Visual Studio" }
    $vc = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vc)) { throw "未找到 vcvars64.bat: $vc" }
    return $vc
}

# ============ 3) 拉取依赖（可跳过） ============
if (-not $SkipDeps) {
    Write-Host "==> [依赖] 检查/拉取 Qt / OpenCV / InspireFace / 模型 ..."
    # 3.1 Qt (aqtinstall)
    $AqtVenv = Join-Path $Third "aqt-venv"
    if (-not (Test-Path (Join-Path $AqtVenv "Scripts\python.exe"))) {
        & python -m venv $AqtVenv
        & (Join-Path $AqtVenv "Scripts\python.exe") -m pip install --quiet aqtinstall
    }
    & (Join-Path $AqtVenv "Scripts\python.exe") -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir $Third

    # 3.2 OpenCV
    if (-not (Test-Path (Join-Path $Third "opencv\opencv\build\include"))) {
        $ocv = Join-Path $Third "downloads\opencv-4.10.0-windows.exe"
        New-Item -ItemType Directory -Path (Split-Path $ocv) -Force | Out-Null
        Invoke-ProxyDownload "https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-windows.exe" $ocv
        & "C:\Program Files\7-Zip\7z.exe" x -y -o (Join-Path $Third "opencv") $ocv | Out-Null
    }

    # 3.3 InspireFace 源码 + 3rdparty
    $Isf = Join-Path $Third "InspireFace"
    if (-not (Test-Path (Join-Path $Isf "CMakeLists.txt"))) {
        & git -c http.proxy=$env:HTTPS_PROXY clone --depth 1 https://github.com/HyperInspire/InspireFace.git $Isf
    }
    if (-not (Test-Path (Join-Path $Isf "3rdparty\InspireCV"))) {
        & git -c http.proxy=$env:HTTPS_PROXY clone --recurse-submodules --depth 1 https://github.com/tunmx/inspireface-3rdparty.git (Join-Path $Isf "3rdparty")
    }

    # 3.4 模型包
    New-Item -ItemType Directory -Path (Join-Path $Root "models") -Force | Out-Null
    $Pack = Join-Path $Root "models\app.pack"
    if (-not (Test-Path $Pack) -or (Get-Item $Pack).Length -lt 10000000) {
        Invoke-ProxyDownload "https://github.com/HyperInspire/InspireFace/releases/download/v1.x/Megatron" $Pack
    }
} else {
    Write-Host "==> [依赖] -SkipDeps：跳过联网拉取。"
}

# ============ 4) 编译 ============
Ensure-NinjaOnPath
$Vcvars = Get-VcvarsPath
$RunCMake = { param($src, $guest, $cfg)
    $cmd = "call `"$Vcvars`" >nul && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release $cfg -S `"$src`" -B `"$guest`" && cmake --build `"$guest`" --parallel"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "构建失败: $src" }
}

# 4.1 InspireFace（共享 DLL，MNN 静态）——依赖就绪即可编
$Isf = Join-Path $Third "InspireFace"
if (-not (Test-Path (Join-Path $Isf "CMakeLists.txt"))) {
    throw "缺少 InspireFace 源码，请去掉 -SkipDeps 先拉依赖。"
}
Write-Host "==> [编译] InspireFace ..."
# CMAKE_POLICY_VERSION_MINIMUM=3.5：MNN(InspireFace 3rdparty) 首行 cmake_minimum_required(3.0)，
# 新版 CMake(≥3.5 兼容移除)会直接报错拒绝配置；传该值让 CMake 按 3.5 策略放行，无需改动第三方源码。
# CMAKE_CXX_FLAGS=/D_USE_MATH_DEFINES：InspireFace/InspireCV/MNN 为 Linux 编写用到 POSIX 宏 M_PI，
# MSVC 需 _USE_MATH_DEFINES 才从 <cmath> 暴露。命令行 /D 先于源码生效、值无空格故 cmd/cmake 双解析安全。
# (本机旧 configure 的 CMAKE_CXX_FLAGS 还带 /DNOMINMAX 防 min/max 宏污染；此处值为单宏以免含空格需引号，
#  若后续踩 std::min/max 冲突再改为带引号的多值 -DCMAKE_CXX_FLAGS="/D_USE_MATH_DEFINES /DNOMINMAX"。)
& $RunCMake $Isf (Join-Path $Isf "build") "-DISF_BUILD_SHARED_LIBS=ON -DMNN_BUILD_SHARED_LIBS=OFF -DISF_BUILD_WITH_SAMPLE=OFF -DISF_BUILD_WITH_TEST=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS=/D_USE_MATH_DEFINES"
cmd /c "call `"$Vcvars`" >nul && cmake --install `"$(Join-Path $Isf 'build')`""
if ($LASTEXITCODE -ne 0) { throw "InspireFace 安装失败" }

# 4.2 本应用
Write-Host "==> [编译] windows-bye ..."
& $RunCMake $Root (Join-Path $Root "build") ""
Write-Host "构建成功。下一步运行 scripts\package.ps1 打包便携目录。"
