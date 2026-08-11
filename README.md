# WeMeet Linux Wayland Screenshare Fix

本项目修复腾讯会议 Linux 客户端在 Wayland (特别是 Hyprland) 环境下屏幕共享时遇到的**闪退崩溃**与**颜色反转**问题。

*特别致谢原项目作者：[wemeet-screenshare-patch (Matheritasiv)](https://github.com/Matheritasiv/wemeet-screenshare-patch)*。

核心技术原理可查看原项目。

---

## Hyprland 下的配置方案

* Hyprland推荐使用的 `xdg-desktop-portal-hyprland`默认使用DMA-BUF进行共享，但是wemeet无法读取，所以要主动配置成shm模式

```Lua
-- ~/.config/hypr/xdph.conf
screencopy {
    force_shm = 1
}
```

* Hyprland推荐使用`xdg-desktop-portal-hyprland`并没有颜色握手问题，所以不需要使用`swap`模式，推荐先尝试使用`none`模式，如果发现会Crash可以尝试使用`straight`模式。
  * 具体的模式实现见下方介绍，Hyprland更新后似乎又需要`straight`模式了

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
