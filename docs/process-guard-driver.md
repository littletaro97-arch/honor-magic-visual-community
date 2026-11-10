# 进程管控内核驱动（MagicProcessGuard）

社区版对 `MonProcessControl.sys` 的对应实现。**当前只有源码，未编译、未签名、未加载。**

## 为什么没有产物

本机只有用户态 Windows SDK（`10.0.26100.0` 的 `shared / ucrt / um / winrt / cppwinrt`），没有 WDK：

| 依赖 | 状态 |
| --- | --- |
| `Include/<ver>/km` 内核头目录 | 不存在 |
| `km/wdm.h` | 不存在 |
| `Inf2Cat.exe` | 不存在 |
| `stampinf.exe` | 不存在 |

`tools/dev/build-process-guard.ps1` 会先检查这些前置条件，缺失时直接抛错：

```text
WDK kernel headers are missing (C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km).
Install the Windows Driver Kit for this SDK version before building the driver.
```

这是刻意的：产出一个未经 `inf2cat` 校验、未经签名的 `.sys` 只会误导后续使用者。

## 驱动做了什么

一个设备 `\Device\MagicProcessGuard`，四个 IOCTL，没有通用执行面：

| IOCTL | 功能 |
| --- | --- |
| `IOCTL_MAGIC_SET_WORKINGSET` | 对指定 PID 设置工作集上下界 |
| `IOCTL_MAGIC_ADD_BLACKLIST` | 加入镜像名黑名单（最多 32 条） |
| `IOCTL_MAGIC_REMOVE_BLACKLIST` | 移出黑名单 |
| `IOCTL_MAGIC_QUERY_STATE` | 查询条目数与最近拦截 PID |

进程创建通知用 `PsSetCreateProcessNotifyRoutineEx`，命中黑名单时把 `CreateInfo->CreationStatus` 置为 `STATUS_ACCESS_DENIED`——**拒绝创建**，而不是创建后终止，避免终止竞态。

工作集设置走 `ZwOpenProcess(PROCESS_SET_QUOTA|PROCESS_QUERY_INFORMATION)` + `ZwSetInformationProcess`，只用最小必要权限。

## 已知不足

- 工作集下界/上界的实际应用方式仍需按 `ProcessWorkingSetWatch` 的真实语义复核；当前代码调用 `ZwSetInformationProcess` 后直接关闭句柄，尚未验证上下界是否真正生效。
- 黑名单只匹配镜像文件名，不匹配完整路径，也不做签名校验。
- 没有白名单优先级，没有按用户会话区分。
- 卸载时直接 `PsSetCreateProcessNotifyRoutineEx(..., TRUE)` 注销，未等待进行中的回调。

## 启用前提

1. 安装与 SDK 同版本的 WDK
2. 编译并生成 `.cat`：`pwsh -File tools/dev/build-process-guard.ps1`
3. 签名：生产环境需 EV 代码签名证书 + Microsoft attestation；本地测试需开 `testsigning` 并安装测试证书
4. `pnputil /add-driver MagicProcessGuard.inf /install`
5. 服务侧通过 `\\.\MagicProcessGuard` 下发 IOCTL

第 3 步在启用了 Secure Boot 的机器上无法用自签名绕过，这是 Windows 的强制策略，不是实现缺陷。

## 用户态客户端（已实现并验证）

`src/MagicCommunity/ProcessGuardClient.cs` 通过 `\\.\MagicProcessGuard` 下发 IOCTL。

关键设计：**驱动是可选的**。设备不存在时，所有调用返回 `false` 并附带 `win32Error`，不抛异常，桌面功能不受影响。

`--process-guard-fixture` 实测（本机无驱动）：

```text
allPass = true
driverAvailable = false
setWorkingSet   -> ok=false win32Error=2  (ERROR_FILE_NOT_FOUND)
addBlacklist    -> ok=false win32Error=2
removeBlacklist -> ok=false win32Error=2
state           -> null

checks:
  client-never-throws-when-absent
  availability-matches-device
  invalid-arguments-rejected-before-ioctl
  driver-absent-is-a-structured-skip
```

参数校验在 IOCTL 之前完成：PID 必须为正、上下界不能为 0、上界不得小于下界、镜像名非空且小于 260 字符。

## 与官方的对照

| 能力 | 官方 `MonProcessControl.sys` | 本驱动 |
| --- | --- | --- |
| 工作集边界 | `ZwSetInformationProcess` + `Set WS OK` 日志 | 已实现调用，未验证生效 |
| 进程创建通知 | `PsSetCreateProcessNotifyRoutineEx` | 已实现 |
| 黑名单 | IOCTL 增删 + 内核终止 | 已实现增删，改为拒绝创建 |
| 终止进程 | `ZwTerminateProcess` | 未实现（用拒绝创建替代） |
| 签名 | 厂商 EV 证书 | 无 |

仅源码层面可对照，运行行为未经验证。
