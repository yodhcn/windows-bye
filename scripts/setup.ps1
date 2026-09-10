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
    # 注意：7-Zip 的 -o 开关必须"无空格紧贴"目标目录(-o<dir>)，写成 "-o <dir>" 会报
    # "Too short switch: -o" 并静默失败；且原生 exe + Out-Null 下非零退出码不触发 $ErrorActionPreference，
    # 故必须显式检查 $LASTEXITCODE。以 opencv2/core.hpp 是否存在作为"解压成功"的可靠判据(比 include 目录存在更稳)。
    $OcvHead = Join-Path $Third "opencv\opencv\build\include\opencv2\core.hpp"
    if (-not (Test-Path $OcvHead)) {
        $ocv = Join-Path $Third "downloads\opencv-4.10.0-windows.exe"
        New-Item -ItemType Directory -Path (Split-Path $ocv) -Force | Out-Null
        if (-not (Test-Path $ocv) -or (Get-Item $ocv).Length -lt 100000000) {
            Invoke-ProxyDownload "https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-windows.exe" $ocv
        }
        $sevenZip = "C:\Program Files\7-Zip\7z.exe"
        if (-not (Test-Path $sevenZip)) { throw "未找到 7-Zip: $sevenZip（Windows CI 需预装 7-Zip）" }
        $out = & $sevenZip x -y "-o$(Join-Path $Third 'opencv')" $ocv
        if ($LASTEXITCODE -ne 0) { throw "OpenCV 解压失败（7z exit=$LASTEXITCODE）：$($out | Select-Object -Last 5)" }
        if (-not (Test-Path $OcvHead)) { throw "OpenCV 解压后仍缺 $OcvHead，解压产物结构异常。" }
        Write-Host "    OpenCV 已就位: $OcvHead"
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
    # 用 Pikachu 包（检测模型为 SCRFD-500M，约 0.5 GFLOPs），而非 Megatron（SCRFD-2.5G）。
    # 本应用仅做"人脸存在检测"，500M 精度足够且推理算力约为 2.5G 的 1/5，CPU 占用显著更低。
    # 两包目录结构一致（face_detect_pixel_list = 160/320/640），代码无需改动。
    New-Item -ItemType Directory -Path (Join-Path $Root "models") -Force | Out-Null
    $Pack = Join-Path $Root "models\app.pack"
    if (-not (Test-Path $Pack) -or (Get-Item $Pack).Length -lt 10000000) {
        Invoke-ProxyDownload "https://github.com/HyperInspire/InspireFace/releases/download/v1.x/Pikachu" $Pack
    }
} else {
    Write-Host "==> [依赖] -SkipDeps：跳过联网拉取。"
}

# ============ 4) 编译 ============
# 做法：先用 cmd 跑一次 vcvars64.bat 并把结果环境(set 输出)逐行导入当前 PowerShell 进程，
# 之后 cmake / ninja 直接用 PowerShell 数组调用——可精确传含空格的值(如 CMAKE_CXX_FLAGS 的多个 /D 宏)，
# 彻底规避 cmd /c 拼字符串时引号/空格解析的坑。
function Import-VcvarsEnv([string]$VcvarsBat) {
    $lines = & cmd /c "`"$VcvarsBat`" >nul 2>&1 && set"
    foreach ($ln in $lines) {
        if ($ln -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
}
# 防 min/max 宏污染 std::min/max(InspireFace 源码直接调 std::min/max 且拉入 windows.h，
# 无 /DNOMINMAX 会 C2589/C4002，complex 内部 numeric_limits::max 也被波及)与提供 M_PI(_USE_MATH_DEFINES)。
$CxxWinFix = '/D_USE_MATH_DEFINES /DNOMINMAX'
$Isf = Join-Path $Third "InspireFace"
if (-not (Test-Path (Join-Path $Isf "CMakeLists.txt"))) {
    throw "缺少 InspireFace 源码，请去掉 -SkipDeps 先拉依赖。"
}

# ---- 4.0 修上游 bug：InspireFace 顶层的 if(APPLE)...else() 把"非 Apple"一律当 linux，
#      于是 cpp/inspireface/CMakeLists.txt 末尾会执行 if(PLAT STREQUAL "linux") ... 链接 dl，
#      MSVC 下找不到 dl.lib -> LNK1181: cannot open input file 'dl.lib'。
#      注意 PLAT 是普通变量(set 非 CACHE)且被强制成 linux，无法用 -DPLAT=... 覆盖。
#      又因 third_party 被 .gitignore、CI 每次全新 clone，补丁必须写在这里(克隆之后、configure 之前)才会到 CI。
#      故对 cpp/inspireface/CMakeLists.txt 做幂等外科补丁：仅把裸 dl 保护到 NOT WIN32 之下，Linux/Apple 行为不变。
$isfCml = Join-Path $Isf 'cpp\inspireface\CMakeLists.txt'
if (Test-Path $isfCml) {
    $cmlText = [IO.File]::ReadAllText($isfCml)
    # 幂等锚定：匹配 find_package(Threads) 之后"紧跟"的裸 dl 行。已打补丁后 find_package 后跟的是
    # if(NOT WIN32)，此锚不再匹配 → 天然幂等。切勿只用裸 dl 单行做 .Contains 子串判断：
    # 补丁生成的 if 内行(8空格缩进)含 4空格前缀子串，会反复命中并把 if(NOT WIN32) 层层套娃打坏文件。
    $oldDlBlock = @'
    find_package(Threads REQUIRED)
    set(LINK_THIRD_LIBS ${LINK_THIRD_LIBS} ${CMAKE_THREAD_LIBS_INIT} dl)
'@.TrimEnd("`r", "`n")
    $dlGuard = @'
    find_package(Threads REQUIRED)
    if(NOT WIN32)
        set(LINK_THIRD_LIBS ${LINK_THIRD_LIBS} ${CMAKE_THREAD_LIBS_INIT} dl)
    else()
        set(LINK_THIRD_LIBS ${LINK_THIRD_LIBS} ${CMAKE_THREAD_LIBS_INIT})
    endif()
'@.TrimEnd("`r", "`n")
    if ($cmlText.Contains($oldDlBlock)) {
        $cmlText = $cmlText.Replace($oldDlBlock, $dlGuard)
        [IO.File]::WriteAllText($isfCml, $cmlText, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "    [patch] 已对 InspireFace 的 dl 链接加 NOT WIN32 保护。"
    } else {
        Write-Host "    [patch] InspireFace 的 dl 补丁已就位(或源已变更)，跳过。"
    }
}

# ---- 4.1 InspireFace（共享 DLL，MNN 静态）----
Ensure-NinjaOnPath
$Vcvars = Get-VcvarsPath
Write-Host "==> [编译] InspireFace ..."
Import-VcvarsEnv $Vcvars
# CMAKE_POLICY_VERSION_MINIMUM=3.5：MNN 首行 cmake_minimum_required(3.0)，新版 CMake 拒 <3.5，传该值放行。
$isfArgs = @(
    '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    '-DISF_BUILD_SHARED_LIBS=ON', '-DMNN_BUILD_SHARED_LIBS=OFF',
    '-DISF_BUILD_WITH_SAMPLE=OFF', '-DISF_BUILD_WITH_TEST=OFF',
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
    "-DCMAKE_CXX_FLAGS=$CxxWinFix",
    '-S', $Isf, '-B', (Join-Path $Isf 'build')
)
& cmake @isfArgs
if ($LASTEXITCODE -ne 0) { throw "InspireFace 配置失败" }
& cmake --build (Join-Path $Isf 'build') --parallel
if ($LASTEXITCODE -ne 0) { throw "InspireFace 编译失败" }
& cmake --install (Join-Path $Isf 'build')
if ($LASTEXITCODE -ne 0) { throw "InspireFace 安装失败" }

# ---- 4.2 本应用 ----
Write-Host "==> [编译] windows-bye ..."
$appArgs = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-S', $Root, '-B', (Join-Path $Root 'build'))
& cmake @appArgs
if ($LASTEXITCODE -ne 0) { throw "windows-bye 配置失败" }
& cmake --build (Join-Path $Root 'build') --parallel
if ($LASTEXITCODE -ne 0) { throw "windows-bye 编译失败" }
Write-Host "构建成功。下一步运行 scripts\package.ps1 打包便携目录。"
