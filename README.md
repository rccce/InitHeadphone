# InitHeadphone.kext

部分 Clevo 笔记本的耳机口后面有一个 SMBus 耳机放大器（地址 0x73）。睡眠唤醒后它不会被重新初始化，耳机就没声音。这个 kext 在开机 3 秒后、以及每次唤醒 1 秒后，通过 VoodooSMBus 把 init-headphone 的初始化序列写给放大器。

初始化序列取自 [Unrud/init-headphone](https://github.com/Unrud/init-headphone)（GPL-3.0-or-later）的 `init` 命令（effect1），本项目沿用同一许可。

## 工作方式

1. `InitHeadphoneLoader` 挂在 VoodooSMBus 控制器上，用 `callPlatformFunction("VoodooSMBusCompanionDevice")` 请求在 0x73 建一个设备节点（VoodooPS2 为触控板建节点用的也是这个接口）。
2. `InitHeadphone` 匹配这个节点，之后所有读写都经过 VoodooSMBus，不会和触控板抢总线。
3. 序列：`0x0a←0x41`，读回并原样写回寄存器 0x04、0x09，`0x00←0x86`（关输出），写 effect 的 5 个寄存器，`0x00←0x82`（开输出）。失败时每秒重试一次，最多 5 次。

需要 VoodooSMBus 3.0（`de.leo-labs.VoodooSMBus`）。

## 编译

仓库里放 `InitHeadphone.cpp`、`Info.plist` 和 `.github/workflows/build.yml`。推送后 GitHub Actions 自动编译，在这次运行的 Artifacts 里下载 `InitHeadphone`，解压得到 `InitHeadphone.kext`。编译失败时下载 `build-log`。

## 安装（OpenCore）

1. 把 `InitHeadphone.kext` 放进 `EFI/OC/Kexts/`。
2. 在 `config.plist` 的 `Kernel → Add` 里加一项，位置必须在 `VoodooSMBus.kext` 之后（例如紧跟在 `RMISMBus.kext` 后面）：

| 键 | 值 |
|---|---|
| Arch | Any |
| BundlePath | InitHeadphone.kext |
| Comment | Clevo headphone amp |
| Enabled | True |
| ExecutablePath | Contents/MacOS/InitHeadphone |
| MaxKernel | （空） |
| MinKernel | （空） |
| PlistPath | Contents/Info.plist |

3. 重启。

## 检查

```bash
ioreg -rc InitHeadphone | grep -E '"(InitCount|LastError|Effect)"'
log show --last 15m --style compact --predicate 'eventMessage CONTAINS "InitHeadphone" OR eventMessage CONTAINS "VSMB"'
```

`InitCount` 是成功初始化的次数：开机后为 1，以后每次唤醒加 1。`LastError` 为 0 表示最近一次成功。

## 选项

`Info.plist` 里 `IOKitPersonalities → InitHeadphone → Effect`：0 到 6，默认 1（与 `init-headphone init` 相同，低音增强）。0 为无效果。

## 停用

把 `config.plist` 里这一项的 `Enabled` 改为 False，重启。
