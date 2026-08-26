# WeMeet Linux Wayland Screenshare Fix

本项目修复腾讯会议 Linux 客户端在 Wayland (特别是 Hyprland) 环境下屏幕共享时遇到的**闪退崩溃**与**颜色反转**问题。

*特别致谢原项目作者：[wemeet-screenshare-patch (Matheritasiv)](https://github.com/Matheritasiv/wemeet-screenshare-patch)*。

核心技术原理可查看原项目。

---

## Hyprland 下的配置方案

* Hyprland推荐使用的 `xdg-desktop-portal-hyprland`默认使用DMA-BUF进行共享，但是wemeet无法读取，所以要主动配置成shm模式

```ini
# ~/.config/hypr/xdph.conf
screencopy {
    force_shm = 1
}
```

* Hyprland推荐使用`xdg-desktop-portal-hyprland`并没有颜色握手问题，所以不需要使用`swap`模式，推荐先尝试使用`none`模式，如果发现会Crash可以尝试使用`straight`模式。
  * 具体的模式实现见下方介绍，Hyprland更新后似乎又需要`straight`模式了

### Hyprland 新版本实测记录（2026-08-24）

以下组合已在 x86_64 环境实测：

| 组件 | 版本 |
| --- | --- |
| Tencent Meeting | `3.26.10.401-5` |
| Hyprland | `0.56.2` |
| xdg-desktop-portal | `1.22.1` |
| xdg-desktop-portal-hyprland | `1.4.1` |
| PipeWire | `1.6.8` |
| WirePlumber | `0.5.15` |

在这套组合中，仅修复 Portal 握手顺序并启用 `force_shm` 仍可能发生竞态崩溃：腾讯会议可以收到首帧，但稍后可能在已经解除映射的 SHM 源地址上执行延迟复制。三次 core dump 的调用链完全一致：

```text
libxcast.so + 0x106b7ec (rep movsb)
libxcast.so + 0xfedd10
libscreen_share_module.so + 0x6b196d
```

崩溃时 `rep movsb` 的源指针与内核报告的缺页地址相同，说明问题位于帧缓冲生命周期，而不是颜色协商失败。

后续重复测试表明，`straight` **只能降低竞态概率，不能彻底解决问题**。最初两轮短时测试正常结束，但之后连续两次共享均发生崩溃：

1. 一次直接崩溃在 `libhook.so` 的 `getenv.loop_64px` AVX2 复制循环。hook 先用 `msync` 检查映射是否存在，但复制约 64 KiB 后源映射即被异步解除，说明该检查存在 TOCTOU，不能固定映射生命周期。
2. 另一次崩溃在 `pw_stream_queue_buffer()`，客户端尝试操作已经失效的 PipeWire 缓冲区引用；同时 XDPH 日志出现多次 `Out of buffers`、重试和流销毁。

因此，不应把 `straight + force_shm` 描述为这套版本组合的稳定修复。`straight` 不定义 `SWAP_COLORS`、不会执行 BGRx/RGBx 通道翻转，但其当前实现仍属于实验性 workaround。若继续实验，应保留 core dump 并重点检查 `libhook.so` 复制循环和 `pw_stream_queue_buffer()` 调用链。

若需要确认实际使用了 SHM，可以查看：

```bash
journalctl --user -u xdg-desktop-portal-hyprland.service | grep 'force_shm'
```

> 二进制偏移严格绑定腾讯会议版本；升级客户端后应重新校验原始字节和文件哈希，不要直接复用旧补丁。

---

## 编译配置与使用说明

在项目根目录的 [PKGBUILD](PKGBUILD) 中，我们引入了 `_hook_mode` 配置选项（约第 18 行），您可以根据系统环境进行个性化配置：

```bash
# Options for screenshare hook mode:
# - "none"     : 仅 Hook 握手。完全不执行任何内存缓冲拷贝和翻转。
# - "straight" : 开启缓冲 Hook，但不翻转颜色（直连拷贝，用于解决部分 compositor 上因异步内存卸载造成的原生崩溃）。
# - "swap"     : 开启缓冲 Hook，并翻转 BGRx/RGBx 颜色通道（对应原作者的红蓝反转修正方案）。
_hook_mode="none"
```

## 部署步骤

1. 克隆本仓库，根据需要修改 [PKGBUILD](PKGBUILD) 中的 `_hook_mode`。
2. 运行打包并安装：`makepkg -sicf (--skipchecksums)`
