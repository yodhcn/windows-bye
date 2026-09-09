# 人脸看护：整理便携发布目录 dist\windows-bye（exe + Qt/OpenCV/InspireFace DLL + 模型 + 启动脚本）。
# 用法：先运行 scripts\build.ps1，再运行本脚本。
$ErrorActionPreference = "Stop"
$Root  = Join-Path $PSScriptRoot ".."
$Third = Join-Path $Root "third_party"
$Dist  = Join-Path $Root "dist\windows-bye"
$exe   = Join-Path $Dist "windows-bye.exe"
if (-not (Test-Path $exe)) { throw "未找到 $exe ，请先运行 scripts\build.ps1" }

New-Item -ItemType Directory -Path $Dist -Force | Out-Null

# 1) Qt DLL/插件
$QtBin = Join-Path $Third "6.8.3\msvc2022_64\bin"
$wdq   = Join-Path $QtBin "windeployqt.exe"
if (Test-Path $wdq) {
    Write-Host "[1/4] windeployqt ..."
    & $wdq --release --no-translations --dir $Dist $exe | Out-Null
}

# 2) OpenCV 运行库
$OcvBin = Join-Path $Third "opencv\opencv\build\x64\vc16\bin"
Write-Host "[2/4] 拷贝 OpenCV DLL ..."
@("opencv_world4100.dll", "opencv_videoio_ffmpeg4100_64.dll", "opencv_videoio_msmf4100_64.dll") |
    ForEach-Object { $s = Join-Path $OcvBin $_; if (Test-Path $s) { Copy-Item $s $Dist -Force } }

# 3) InspireFace 运行库（其 DLL 位于构建目录；静态打包了 MNN）
Write-Host "[3/4] 拷贝 InspireFace DLL ..."
$IsfDll = Join-Path $Third "InspireFace\build\cpp\inspireface\InspireFace.dll"
if (Test-Path $IsfDll) {
    Copy-Item $IsfDll $Dist -Force
} else {
    Write-Warning "未找到 InspireFace.dll"
}

# 4) 模型目录
Write-Host "[4/4] 拷贝模型 ..."
$models = Join-Path $Dist "models"
New-Item -ItemType Directory -Path $models -Force | Out-Null
if (Test-Path (Join-Path $Root "models\app.pack")) {
    Copy-Item (Join-Path $Root "models\app.pack") $models -Force
}

Write-Host "完成。便携目录: $Dist"
Write-Host "将该目录整体拷贝到任意 Windows 机器，双击 windows-bye.exe 即可运行（不依赖 run.bat）。"