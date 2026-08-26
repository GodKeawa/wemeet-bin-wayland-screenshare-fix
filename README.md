# Wemeet Linux Wayland Screenshare Fix

本项目致力于彻底修复腾讯会议 (Wemeet) Linux 客户端在 Wayland (特别是 Hyprland) 环境下进行屏幕共享时遇到的**闪退崩溃**与**颜色反转**等问题。

* *特别致谢原项目作者：[wemeet-screenshare-patch (Matheritasiv)](https://github.com/Matheritasiv/wemeet-screenshare-patch)*
* 感谢[YoungJurry](https://github.com/YoungJurry)的测试与问题报告，为本项目确定了TOCTOU竞态的原因

---

## 问题分析

Wemeet 在 Linux 下的屏幕共享依赖于底层的 `xdg-desktop-portal`（用于屏幕选择鉴权）和 `PipeWire`（用于视频流传输）。

### 1：DBus 握手竞态

* **标准流程**：客户端向 Portal 发起 `CreateSession` -> `SelectSources`（等待用户选择屏幕并确认） -> 收到成功响应后发起 `Start` -> 最后获取 `OpenPipeWireRemote` 视频流文件描述符。
* **Wemeet 的缺陷**：Wemeet 在发送 `SelectSources` 请求后，**完全没有等待系统的成功响应**，就立刻向系统发送了 `Start` 请求。在 Hyprland 等需要弹窗让用户截图选择的桌面环境下，这种“抢跑”会导致 `Start` 请求被拒，屏幕共享直接失败。

### 2：颜色格式协商失败

* **Wemeet 的缺陷**：内部死板地硬编码了只请求 `BGRx` (Id 8) 格式。如果系统的 Compositor 只能提供 `RGBx` (Id 7)，协商就会失败，造成有视频流但画面黑屏或红蓝反转。

### 3：TOCTOU 内存卸载

* **标准流程**：PipeWire 触发回调 -> 客户端借出共享内存 (SHM) 帧 -> 客户端渲染完毕 -> 客户端归还帧。
* **Wemeet 的缺陷**：Wemeet 拿到帧内存指针后，直接扔给后台线程 (`libxcast.so`) 异步处理，但**并没有按规范锁定该内存池**。当系统负载较高或分辨率较高时，Wemeet 处理过慢会导致系统 Portal 耗尽缓冲池 (`Out of buffers`)，进而强行剥夺并 `unmap` 该块内存。此时 Wemeet 仍在复制该内存，瞬间触发 `SIGSEGV` 段错误闪退。这是典型的 **TOCTOU** 竞态条件。

---

## 修复方案

### 1. 修复 DBus 竞态与颜色格式协商 (`patch.py`)

* **握手修复**：使用 Python 脚本对二进制文件进行微调，将 `Start` 请求调用强行移入 `SelectSources` 的成功回调内部。强制 Wemeet 遵循严格的同步时序，解决无法开始共享的问题。
* **格式欺骗**：部分后端（如 xdg-desktop-portal-hyprland）在协商阶段优先甚至强制要求 `RGBx`，而 Wemeet 硬编码只请求 `BGRx` 导致协商失败。我们将请求强行篡改为 `RGBx` 骗过系统，再将响应篡改回 `BGRx` 骗过 Wemeet 的内部检查。

### 2. 视频帧缓存与传递 (`libhook.so`)

为了解决 TOCTOU 崩溃，我们作为中介代替Wemeet管理PipeWire的Buffers，同时将我们的静态Buffer直接提供给Wemeet：

1. **PipeWire 帧缓存**：
   在 Wemeet 的 PipeWire 线程索要画面时，C 代码将真实画面拷贝到我们预先分配的**静态内存池**中。拷贝完成后，C 代码**在同一个线程内立刻替 Wemeet 归还内存**。这样便可遵守 PipeWire 规范，保证系统缓冲池充裕，解决 `Out of buffers` 和内存卸载导致的段错误。
2. **libxcast.so 帧传递**：
   `libhook.so` 会在运行时自动扫描 Wemeet 内部组件 `libxcast.so` 的内存，利用特征码找到其 2D 渲染拷贝函数 (`copy_image`)，并实时植入Trampoline Hook，从而劫持 Wemeet 自身的渲染管线，我们将之前缓存到静态buffer中的帧传递给后续处理逻辑，保证内存稳定。
3. **AVX-512 / AVX2 加速**：
   当 Wemeet 处理我们的 Fake Buffer 时，其渲染过程被我们的 Hook 拦截。我们使用纯 C/SIMD 代码替换掉 Wemeet 的拷贝循环，利用 GCC `__builtin_cpu_supports` 进行探测，如果您的 CPU 支持，将会启动 AVX-512 或 AVX2 指令集 进行操作。

---

## 使用说明

### 前置配置 (针对 Hyprland)

Hyprland 默认使用 GPU 零拷贝 (DMA-BUF) 分配显存，但 Wemeet 只支持读取普通系统内存。因此必须强制 Hyprland 提供 SHM 内存：

```ini
# ~/.config/hypr/xdph.conf
screencopy {
    force_shm = 1
}
```

### 编译选项配置

在本项目根目录的 [PKGBUILD](PKGBUILD) 中，约第 18 行提供了宏配置选项：

```bash
# Options for screenshare hook mode:
# - "none"     : 仅应用 Python DBus 握手补丁，不启动 C 语言的 PipeWire 内存拦截
# - "straight" : 开启视频帧缓存，修复 TOCTOU 闪退崩溃
# - "swap"     : 开启视频帧缓存，并在拷贝时利用 AVX 翻转 BGRx/RGBx 颜色通道（如遇偏色可开启）
_hook_mode="straight"

# Options for debug mode:
# - 0 : Disable debug logs.
# - 1 : Enable detailed debug logs (printed to stderr).
_debug_mode=1
```

*(目前的 Hyprland 没有颜色问题，建议首选 `"straight"` 模式并开启调试，并不会打印大量log)*

### 部署步骤

1. 克隆本仓库，根据需要调整 `PKGBUILD`。
2. 运行打包并安装：

```bash
makepkg -sicf (--skipchecksums)
```
