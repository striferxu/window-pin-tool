# WindowPinLite — C++ / Win32 原生版

Python 版的 C++ 重写，同样的功能，体积从 **27 MB 降到 180 KB**。

纯 Win32 API + GDI+ 实现，静态链接 CRT，**不依赖任何运行库**，单个 exe 直接跑。

---

## 与 Python 版的区别

| | Python 版 (`WindowPin.exe`) | C++ 版 (`WindowPinLite.exe`) |
| --- | --- | --- |
| 体积 | 27.3 MB | **180 KB**（x64）/ **178 KB**（x86） |
| 运行时依赖 | 自带 Python 解释器（PyInstaller 打包） | 无（静态链接，纯系统 DLL） |
| 启动速度 | 首次 1–3 秒解包 | 瞬间 |
| 内存占用 | ~30 MB | **~8.6 MB** |
| 功能 | 完整 | 完整（同等功能） |
| 系统支持 | Win7+ | Win7 SP1+ |

> **术语说明**：Python 版是用 PyInstaller **打包**（把解释器和脚本塞进一个 exe，不是编译成机器码）；C++ 版是用 MSVC **编译**成原生机器码。这是体积差 150 倍的根本原因。

---

## 功能

- **单窗口置顶** —— 一次只置顶一个窗口，切换目标时自动取消上一个（多窗口同时置顶会互相干扰 Z 序，所以刻意做成单窗口）
- **三种触发方式** —— 全局热键 / 左键单击托盘图标 / 点击标题栏图钉
- **标题栏图钉** —— 置顶时在标题栏水平居中显示红色 📌（内嵌 Segoe UI Emoji 渲染像素，与 Python 版逐像素一致）
- **退出自动恢复** —— 退出时把所有被置顶的窗口恢复到置顶前的状态（原本就是 topmost 的不会被误取消）
- **置顶加固** —— 能扛住抢置顶的程序，但只在「真被别的 topmost 窗口压住」时才重新提升，避免干扰正常 Z 序
- **右键菜单不被遮挡** —— 菜单打开期间暂停 Z 序调整并隐藏图钉，资源管理器的右键菜单可正常使用
- **热键自定义** —— 支持冲突检测，配置持久化到 `%APPDATA%\WindowPin\config.ini`（**与 Python 版共用同一份配置**）
- **单实例运行** —— 重复启动会把「切换置顶」消息转发给已运行实例

---

## 快速开始

从 [Releases](../../releases) 下载对应版本：

| 文件 | 架构 | 适用系统 |
| --- | --- | --- |
| `WindowPinLite.exe` | x64 | Windows 7 SP1+ 64 位 |
| `WindowPinLite32.exe` | x86 | Windows 7 SP1+ 32 位（64 位系统也能跑） |

双击运行即常驻托盘，无需安装、无需管理员权限。

---

## 从源码构建

需要 **Visual Studio 2022 Build Tools**（或完整 VS2022）的 C++ 桌面开发组件。

```bat
:: 64 位
build.bat

:: 32 位
build32.bat

:: 带日志的调试版（自测用）
build.bat debug
```

> **注意**：构建脚本必须在 **cmd 或 PowerShell** 下运行，**不能在 Git Bash 下运行** —— Git Bash 会把 `/O1` 之类的参数当成路径转义掉。

产物：
- `WindowPinLite.exe`（x64 Release）
- `WindowPinLite32.exe`（x86 Release）
- `WindowPinDbg.exe`（x64 Debug，带日志）

编译参数（Release）：

```
cl /nologo /utf-8 /O1 /MT /EHsc /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /DWINVER=0x0601
   windowpin.cpp windowpin.res
   /link /SUBSYSTEM:WINDOWS,6.01 /OPT:REF /OPT:ICF gdiplus.lib
```

- `/MT` —— 静态链接 CRT，生成的 exe 不需要 VC++ 运行库
- `/O1` —— 优化体积
- `/D_WIN32_WINNT=0x0601` + `/SUBSYSTEM:WINDOWS,6.01` —— 目标系统固定为 Windows 7，不使用任何 Win8+ API
- `/OPT:REF /OPT:ICF` —— 去除未引用代码、合并相同函数

---

## 图标资源

图标不是手绘的，而是**把 Windows 自带的 Segoe UI Emoji 📌 渲染成像素**，所以和系统里看到的 emoji 完全一致。

```bash
python make_assets.py
```

这个脚本会生成：

| 文件 | 用途 |
| --- | --- |
| `assets/pin_26x22.png` | 标题栏图钉贴图（26×22，16px 字号，与 Python 版逐像素一致） |
| `assets/windowpin.ico` | 多尺寸图标（16/20/24/32/40/48/64/128/256） |
| `pin_data.h` | 内嵌进 exe 的 PNG 字节数组（自动生成，勿手改） |

**ICO 格式说明**：小尺寸（≤64px）用传统 BMP 帧，只有 128/256 用 PNG 压缩帧。原因是 **Windows 7 的图标加载器对小尺寸 PNG 帧支持不好**，会导致托盘图标或 exe 图标显示空白。这是 Win7 兼容性的关键处理。

---

## 自测

```bash
WindowPinDbg.exe --selftest
```

覆盖 **43 项检查**，包括：

- 图钉位图渲染（透明背景、红色主体、四角透明、中心不透明）
- 单窗口切换（置顶 B 时 A 自动取消）
- 置顶加固（反复加固后仍置顶）
- 托盘左键路径（前台是任务栏时能回退到正确的目标窗口）
- 右键菜单不被压住（构造拥有窗口模拟菜单，验证 Z 序）
- 菜单打开时图钉自动隐藏 / 关闭后恢复
- 幂等置顶（重复置顶同一窗口不叠加）
- 目标窗口销毁后自动清理
- 退出恢复（真实 `WM_CLOSE` 路径，验证窗口状态被还原）

日志输出到 `%TEMP%\windowpin.log`。

> 说明：测试是**进程内自测**（程序自己创建窗口验证逻辑）。在没有交互桌面的环境里，跨进程操作窗口会失败（`SetWindowPos` 返回 `err=1400`），因此不能靠外部脚本驱动 UI。

---

## 代码结构

`windowpin.cpp` 单文件，约 1200 行，按职责分段：

| 段落 | 内容 |
| --- | --- |
| 配置读写 | `LoadConfig` / `SaveConfig`，读写 `%APPDATA%\WindowPin\config.ini` |
| 图钉绘制 | `LoadPinPng` / `BuildPinImage`，GDI+ 解码内嵌 PNG |
| 图标加载 | `LoadAppIcon`，从 exe 资源按尺寸加载 |
| 置顶操作 | `IsValidTarget` / `ApplyTopmost` / `AddPin` / `RemovePin` |
| 图钉窗口 | `PlacePin` / `PinProc`，分层窗口 + `UpdateLayeredWindow` |
| 跟随加固 | `IsMenuOpenOnThread` / `OtherTopmostAbove` / `FollowAll`，150ms 定时器 |
| 热键管理 | `RegHotkey` / `ApplyHotkey`，含冲突检测 |
| 托盘 | `AddTray` / `ShowTrayMenu` / `NotifyBalloon` |
| 候选窗口 | `PickTarget`，托盘点击时前台变任务栏的回退逻辑 |
| 宿主窗口 | `HostProc`，消息循环与命令分发 |
| 自测 | `RunSelfTest`，43 项检查 |

关键技术点：

- **分层窗口绘制**：`WS_EX_LAYERED` + `UpdateLayeredWindow` + 预乘 Alpha 的 BGRA，才能画出带透明通道的彩色图钉
- **置顶加固的条件化**：`ApplyTopmost` 发现窗口已经是 topmost 就直接返回，不再 `SetWindowPos` —— 否则会把窗口重新提到最前，压住它自己的右键菜单
- **单窗口模式**：`AddPin` 在添加新目标前先清空 `g_pins`，确保同时只有一个置顶
- **退出恢复**：`PinEntry::prevTopmost` 记录置顶前的状态，退出时据此还原而不是无条件取消

---

## 兼容性说明

- **Windows 7 SP1 及以上**，x86 / x64 均可
- 静态链接 CRT，不需要安装 VC++ 运行库
- 导入的 API 全部来自 `USER32 / GDI32 / SHELL32 / KERNEL32 / ole32 / gdiplus`，没有任何 Win8+ 才有的函数
- 图标资源针对 Win7 的 PNG 帧缺陷做了特殊处理

---

## License

MIT，与主项目一致。
