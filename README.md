# Windows Bye —— 人脸看护 · 离开自动锁屏

> 一个 Windows 下的"人走锁屏"小工具：通过摄像头实时做人脸存在检测，当画面里持续一段时间没有人脸，就自动锁定工作台；锁屏前主动释放摄像头资源，解锁后自动恢复看护。

## 功能特性

- **纯人脸存在检测（不做身份识别）**：基于 [InspireFace](https://github.com/HyperInspire/InspireFace) SDK 的 **SCRFD-500M** 检测能力（320 检测档位），只判断"有没有人"，不提取特征、不入库、不比对"是不是本人"——隐私友好、CPU 占用低。
- **离开自动锁屏**：画面中无人脸超过设定延迟（可调 1~600 秒）即调用 `LockWorkStation()` 锁定工作台。
- **锁屏资源释放**：通过 **WTS 会话通知**（`WM_WTSSESSION_CHANGE`）感知系统锁屏/解锁——锁定时主动释放摄像头与检测线程，解锁后自动恢复，避免后台空转。
- **低 CPU / 省资源设计**：
  - 检测节流：固定约 2.5 fps（400 ms 一次），无需高帧率。
  - 采集自适应：前台（预览可见）满速保证流畅；后台（隐藏到托盘）自动降到约 3 fps。
  - 预览按需发布：仅当主界面可见（有消费者）时才解码并发布预览帧。
  - 摄像头中断/被占用自动重试（约每 3 秒一次），并有黑帧兜底判断。
- **摄像头设备枚举/选择**：用 DirectShow 枚举，可选设备并记忆。
- **系统托盘常驻**：关闭窗口不退出、隐藏到托盘后台运行；支持开机自启（注册表 Run 项，`--tray` 后台启动）。
- **单实例**：重复启动会让已运行实例弹出主界面，避免多开抢占摄像头。
- **便携发布**：打包后整个目录拷到任意 Windows 机器即可双击运行，不依赖安装。

## 技术栈

| 类别 | 选型 | 说明 |
| ---- | ---- | ---- |
| 语言 | C++17 | |
| GUI 框架 | **Qt 6.8.3**（msvc2022_64，Core/Gui/Widgets/Network） | 深色主题、托盘、单实例 QLocalServer |
| 视频采集 | **OpenCV 4.10**（VideoIO：Media Foundation / DirectShow） | 读摄像头、BGR/RGB 转换 |
| 人脸检测 | **InspireFace SDK**（C++，MNN 推理） | SCRFD 检测-only 会话，`enable_*` 全关 |
| 系统集成 | Win32 API：`Wtsapi32`（会话通知）、`user32`（LockWorkStation）、`dshow`（设备枚举） | |
| 构建 | CMake ≥ 3.21 + Ninja + MSVC（VS2022/2026 Build Tools，C++ 工作负载） | |
| 线程模型 | 双线程：**采集线程** + **检测线程** | 采集写共享帧、检测读共享帧后检测 |
| 推理模型 | `models/app.pack`（Megatron 资源包，约 61 MB） | 检测/特征等打包，实际仅按需载入检测所需模型 |

> **关于模型与内存**：模型包约 61 MB 但**不会被整包载入内存**。`Launch::Load()` 只读包内 manifest，真正的大模型（识别特征网络 r18 约 48 MB）受 `enable_recognition=false` 门控**不会被加载**。检测-only 会话实际只常驻 SCRFD-320 + landmark + refine_net 约 3 MB 权重；运行时主要内存来自 Qt/OpenCV/MNN 运行时与摄像头工作区。

## 工作原理简述

```
摄像头 ──(采集线程, 满速/后台降频)──► 最新帧 ─┐
        ◄───────────────────────────────┘ 检测线程(每 400ms)
                                          │ 克隆最新帧 → InspireFace 检测
                                          ▼
                                  人脸存在?  → 主窗口 500ms 看护计时
                                          │
              无人脸 ≥ 延迟 && 距上次锁屏 ≥10s ──► LockWorkStation() 锁屏
                                          ▲
              WTS 会话通知: 锁屏→释放摄像头 / 解锁→恢复看护
```

- 共享帧用 `std::mutex` 保护，采集线程 clone 写入、检测线程 clone 读取，互不阻塞摄像头读取。
- 检测只回传 `QRect + score`，与预览帧同一像素空间（640×480 采集分辨率），`PreviewWidget` 负责缩放绘制并叠加人脸框。

## 目录结构

```
windows-bye/
├─ src/                    # 应用源码
│  ├─ main.cpp             # 入口：单实例、-d 延迟、--tray、IPC
│  ├─ MainWindow.h/.cpp    # 主窗口、托盘、WTS 锁屏处理、看护计时
│  ├─ EngineWorker.h/.cpp  # 采集线程 + 检测线程（摄像头生命周期）
│  ├─ FaceEngine.h/.cpp    # InspireFace 检测-only 封装（PIMPL）
│  └─ PreviewWidget.h/.cpp # 摄像头预览 + 人脸框叠加
├─ scripts/
│  ├─ setup.ps1           # 检查并安装环境 + 拉依赖 + 编译（InspireFace + 应用）→ 产出 exe
│  ├─ package.ps1         # 打包便携目录 dist\windows-bye（DLL/插件/模型）
│  ├─ smoke_test.ps1      # 冒烟测试：启动 exe 后确认进程存活再关闭
│  └─ tray_test.ps1       # 托盘测试：--tray 后台驻留验证
├─ CMakeLists.txt
├─ models/app.pack         # InspireFace 资源包（运行必需，约 61 MB，默认不入库）
├─ third_party/            # 本地第三方依赖（Qt/OpenCV/InspireFace），不入库
├─ dist/windows-bye/       # 打包产物（exe + DLL + 插件 + models）
└─ .github/workflows/      # GitHub Actions 工作流（build.yml）
```

## 构建环境（Requirements）

- **Windows 10/11 x64**
- **Visual Studio 2022 / 2026 Build Tools**（勾选 *使用 C++ 的桌面开发* 工作负载），或完整的 VS；需要 MSVC `cl.exe`、`vcvars64.bat`、CMake、Ninja。
- **Python 3.x**（供 aqtinstall 拉取 Qt）与 **7-Zip**（供解压 OpenCV，setup 里用了 `C:\Program Files\7-Zip\7z.exe`）。
- 网络可访问 GitHub（拉取 Qt/OpenCV/InspireFace/模型）。如走代理，设置 `HTTP_PROXY`/`HTTPS_PROXY` 环境变量即可（setup 会自动识别）。

> MSVC 定位：`setup.ps1` 用 `vswhere` **自动定位**最新安装的 vcvars64.bat，无需手动改路径，可同时适配 VS2022 / VS2026。

## 本地构建

在 **PowerShell** 中依次执行：

```powershell
# 1) 检查并安装环境 + 拉依赖 + 编译（InspireFace + 应用），产出 exe
#    —— 若依赖已拉好、只想重新编译，可加 -SkipDeps 跳过联网下载
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1

# 2) 打包便携目录 dist\windows-bye（windeployqt + OpenCV/InspireFace DLL + 模型）
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
```

> **脚本精简**：只需 4 个脚本。`setup.ps1` 一个入口完成「检查并安装环境 → 拉依赖 → 编 InspireFace → 编本应用」；`package.ps1` 负责把构建产物打包成可分发目录；`smoke_test.ps1` / `tray_test.ps1` 是冒烟/托盘自测脚本。CI 与本地共用同一套脚本，无需额外的编排脚本。

**产物位置**：`dist\windows-bye\`。将该目录整体拷贝到任意 Windows 机器，双击 `windows-bye.exe` 即可运行（不依赖安装）。直接调试运行时，把 `models\app.pack` 放到 exe 同级 `models\` 下即可。

> 手动逐步构建（不想用脚本）：设好 vcvars 环境后 `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build && cmake --build build`，再用 package.ps1 部署。InspireFace 同样方式先于应用单独编一次。

## 命令行参数

| 参数 | 说明 |
| ---- | ---- |
| `-d <秒>` / `--delay <秒>` | 指定默认"无人脸多久后锁屏"延迟（可被 UI 里已保存的设置覆盖）。测试常用 `-d 600` 避免误锁。 |
| `--tray` | 仅创建托盘图标后台运行，不弹出主界面（供开机自启使用）。 |

## GitHub Actions 自动打包

仓库根目录的 `.github/workflows/build.yml` 会在 GitHub 托管的 Windows 环境中完整执行：检出 → 分别调用 `scripts/setup.ps1`（检查环境 + 拉依赖 + 编 InspireFace + 编应用）和 `scripts/package.ps1`（打包 `dist\windows-bye`）→ 用 `actions/upload-artifact@v4` 上传为 zip 工件。

- 触发方式：`push`（打 `v*` tag 时）以及 `workflow_dispatch`（手动运行）。
- 工件名：`windows-bye`；下载到的 zip 解压即得完整便携目录。
- 运行环境 `windows-latest` 对应 **VS2026（VS18）**，与本地使用的 VS18 工具链一致，能正常编译 Qt msvc2022_64 目标。
- YAML 只负责**分别调用两个脚本**（`setup.ps1` → `package.ps1`），不再依赖中间编排层，本地与 CI 复现同一套逻辑。

> 提交到 GitHub 前，请确认体积大的 `third_party/`、`build/`、`dist/` 已被 `.gitignore` 排除，且 `models/app.pack` 未入库——CI 会由 `setup.ps1` 联网自动拉取全部依赖与模型。

首次在 CI 运行会联网拉取全部依赖（Qt/OpenCV/InspireFace/模型），耗时较长属正常。详细步骤见该文件内注释。

## 使用提示

- 程序运行时以托盘图标常驻；**点关闭按钮只是隐藏到托盘**，需在托盘菜单选"退出"才真正结束。
- 锁屏的判定口径：引擎就绪 + 摄像头输出真实画面 + 画面中无人脸 + 距上次锁屏 ≥10 秒。
- 摄像头被其它软件占用时，程序会持续自动重试并给出状态提示，不崩溃。
