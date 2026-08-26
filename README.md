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
* **Wemeet 的缺陷 (最严重的崩溃)**：Wemeet 拿到帧内存指针后，直接扔给后台线程 (`libxcast.so`) 异步处理，但**并没有按规范锁定该内存池**。当系统负载较高或分辨率极高时（如 4K 拷贝耗时较长），Wemeet 处理过慢会导致系统 Portal 耗尽缓冲池 (`Out of buffers`)，进而强行剥夺并 `unmap` 该块内存。此时 Wemeet 仍在复制该内存，瞬间触发 `SIGSEGV` 段错误闪退。这就是典型的 **TOCTOU (Time-of-check to time-of-use)** 竞态条件。

---

## 修复方案

### 1 修复 DBus 竞态与颜色格式 (`patch.py`)

* **握手修复**：我们使用 Python 脚本对二进制文件进行微调，将 `Start` 请求的调用强行移入 `SelectSources` 的成功回调函数内部。强制 Wemeet 遵循严格的同步时序。
* **格式欺骗**：将硬编码的 `BGRx` 请求篡改为 `RGBx` 以通过协商，并在收到系统响应后篡改回 `BGRx`，骗过 Wemeet 的内部检查。

### 2 修复内存生命周期 (`libhook.so`)

为了解决 TOCTOU 崩溃，我们编写了纯 C 语言的原生拦截库 (`libhook.so`)，利用 `LD_PRELOAD` 和 `dlsym` 拦截 PipeWire 底层 API，**彻底解耦系统缓冲池与 Wemeet 的处理周期**：

1. 拦截 `pw_stream_dequeue_buffer`。当 Wemeet 想要获取帧时，C 代码瞬间将真实画面拷贝到我们自己申请的私有堆内存中。
2. 拷贝完成（微秒级）后，C 代码**立刻替 Wemeet 归还**真实的内存给系统。这样系统缓冲池永远不会枯竭，流永远不会被强行销毁。
3. 我们将指向私有内存的假 Buffer 塞给 Wemeet。不论 Wemeet 怎么异步拖沓，它操作的永远是我们绝对安全的私有内存，从根本上杜绝了段错误。
   *(注：颜色通道的红蓝翻转也在这一步利用 AVX2 指令集高效完成。)*

---

## 使用说明

### 前置配置 (针对 Hyprland)

Hyprland 默认使用 GPU 零拷贝 (DMA-BUF) 分配显存，但 Wemeet 非常原始，只支持读取普通系统内存。因此您必须强制 Hyprland 提供 SHM 内存：

```ini
# ~/.config/hypr/xdph.conf
screencopy {
    force_shm = 1
}
```

### 编译模式选择

在本项目根目录的 [PKGBUILD](PKGBUILD) 中，约第 18 行提供了 `_hook_mode` 选项以应对不同环境的需求：

```bash
# Options for screenshare hook mode:
# - "none"     : 仅应用 Python DBus 握手补丁，不启动 C 语言的 PipeWire 内存拦截。
# - "straight" : 开启 C 语言 PipeWire 内存拦截，彻底修复 TOCTOU 闪退崩溃，但不翻转颜色。
# - "swap"     : 开启 C 语言 PipeWire 内存拦截，并在 C 中利用 AVX2 高效翻转 BGRx/RGBx 颜色通道。
_hook_mode="none"
```

*(由于目前 Hyprland 的色彩没有问题，所以不需要swap，建议首选 `"straight"`。)*

### 部署步骤

1. 克隆本仓库，根据需要修改 `PKGBUILD` 中的 `_hook_mode`。
2. 运行打包并安装：

```bash
makepkg -sicf
```
