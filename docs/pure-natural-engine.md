# 纯净自然引擎移植设计

## 已落地：P0 自有进程工作集裁剪

社区版的 [PureNaturalEngine.cs](../src/MagicCommunity/PureNaturalEngine.cs) 是一个深模块：外部只在应用启动时调用 `Start`，在桌面接管时调用 `RequestInteractionTrim`，退出时 `Dispose`。阈值、定时器、托管 GC、Win32 工作集 API、失败重试、日志和节电策略都留在模块内部。

接口行为：

- 启动后按 8 / 12 / 20 / 30 / 45 / 120 秒触发裁剪，阈值 64 MiB。
- 之后每 5 分钟检查一次，阈值 120 MiB。
- 桌面接管后延迟 12 秒检查一次，阈值 96 MiB。
- 两次成功裁剪最小间隔 120 秒。
- `CommunityPreferences.WorkingSetTrim` 默认开启；`MAGICCOMMUNITY_DISABLE_WORKING_SET_TRIM=1` 关闭。
- Windows 节电模式暂停裁剪，避免为了低内存数字制造额外磁盘回读。
- 实际裁剪顺序：`GC.Collect(...Optimized...)` → `K32EmptyWorkingSet(GetCurrentProcess())` → 失败时 `SetProcessWorkingSetSize(-1, -1)`。
- 日志名称：`pure_natural_started`、`pure_natural_trim`、`pure_natural_skip`。

## 与官方样本的对应关系

| 官方机制 | 社区版 P0 | 后续阶段 |
| --- | --- | --- |
| `MagicTaskbar.Shared.dll` 定时 `EmptyWorkingSet` | 已落地，阈值和时间表对齐 | 无 |
| `WinTransMonitor` 工作集与截图缓存治理 | 自有 WPF 进程裁剪 | 截图/合成缓存单独接入 |
| `NtSetSystemInformation(SystemMemoryListInformation)` 清理备用页/工作集列表 | P0 未纳入 | P1：独立 native backend 命令，显式用户开关 |
| `NtCreateProcessStateChange / NtChangeProcessState` 冻结后台进程 | P0 未纳入 | P2：只冻结社区版自有 renderer / helper，焦点恢复 |
| `MonProcessControl.sys` 黑名单、进程创建通知、内核工作集边界 | P0 未纳入 | P3：独立签名驱动与配置白名单 |
| `SysMain / whesvc` 停服务、黑名单拦截第三方桌面软件 | P0 未纳入 | P3：独立服务，默认关闭 |
| C 盘目录迁移 + Junction + 预加载 | P0 未纳入 | P4：磁盘迁移向导与自有预启动列表 |

## P1：系统内存列表清理接口

建议放在 `src/system-native`，沿用 `Magic.System.Native/1` 的命名管道协议，新增 `memory-clean` 操作。用户开关默认关闭，启用后由 C# 侧显式请求；native backend 动态解析 `ntdll.dll`：

```text
NtSetSystemInformation(SystemMemoryListInformation = 0x50, command, sizeof(command))
command = 4  ; MemoryPurgeStandbyList
command = 2  ; MemoryEmptyWorkingSets
```

前置步骤：`SeIncreaseQuotaPrivilege`、`SeProfileSingleProcessPrivilege`；返回 `0xC0000061` 时记录 `privilege-not-held` 并停产，不做隐式提权。命令回执包含 `GlobalMemoryStatusEx` 前后差值、Win32 错误码、触发来源。该模块保持独立，避免把 Native API 暴露给 WebView/Widget。

## P2：自有进程冻结接口

`Magic.System.Native/1` 增加 `process-freeze` / `process-resume` 操作，只允许 PID 属于 `CompositionDesktopRuntime` 自有 renderer 或 `MagicSystemNative.exe` 子进程的句柄。Win11 下动态解析：

```text
NtCreateProcessStateChange
NtChangeProcessState
```

状态对象句柄由 native backend 持有；连接断开或父进程退出时关闭句柄，由内核撤销冻结。配置字段：`idleMs`、`minWorkingSetBytes`、`focusGraceMs`、`excludedPids`。不接收任意进程名，不终止第三方进程。

## P3/P4 边界

P3 若加入服务停启、黑名单和驱动，按独立签名组件发布，配置使用带 schema 版本、哈希和回滚副本的文件；驱动 IOCTL 控制码、`ZwSetInformationProcess` 参数结构和进程创建回调单独做隔离检查。P4 的 C 盘迁移沿用官方思路：扫描已知目录 → 选择目标盘 → 移动 → 原路径 Junction → 图标/权限/历史记录；用户提供目标路径，记录回滚清单。

## 文件与验证

- [PureNaturalEngine.cs](../src/MagicCommunity/PureNaturalEngine.cs)：模块实现。
- [PureNaturalTests.cs](../src/MagicCommunity/PureNaturalTests.cs)：策略、失败、恢复、禁用、Win32 API 与默认配置测试。
- [Program.cs](../src/MagicCommunity/Program.cs)：启动接入、`--pure-natural-test` 命令、桌面交互触发。
- [CommunityPreferences.cs](../src/MagicCommunity/CommunityPreferences.cs)：用户开关。
- [CommunitySettingsWindow.cs](../src/MagicCommunity/CommunitySettingsWindow.cs)：省电页开关。
- [check-worktree.ps1](../tools/dev/check-worktree.ps1)：把 `pure-natural-test` 纳入构建检查。
- [measure_process_group.py](../tools/p0/measure_process_group.py)：现有只读进程组采样器，可用于 P0 A/B 对照。

验证命令：

```powershell
pwsh -NoProfile -File tools/dev/check-worktree.ps1
```

单项回执：

```powershell
& tools/dotnet/dotnet.exe versions/0.4.9/MagicCommunity.dll --root . --pure-natural-test --report artifacts/pure-natural.json
```

P0 只改变自有进程 Working Set 的呈现口径；`Private Bytes`、`Commit` 与后续软缺页仍需用 `measure_process_group.py` 做 A/B。官方宣传的 `2GB+`、`15%` 和 C 盘节省属于实验室口径，社区版复现时使用实测前后差值，不沿用这些数字。

## 本次验证结果

`pwsh -NoProfile -File tools/dev/check-worktree.ps1` 返回 `exitCode=0`，`allPass=true`。回执目录：`artifacts/check-20261109-181647-f4ed55c1`；`self-test`、`pure-natural-test`、`desktop-card-test`、`custom-card-test`、`desktop-refresh-test`、`desktop-group-test`、`clipboard-move-data-test` 共 7 项全部通过。`pure-natural-test` 覆盖阈值触发、最小间隔、低于阈值、节电暂停、API 失败不推进调度、失败后恢复、禁用引擎、Win32 工作集 API、默认开关及日志边界。

构建中补装了本地 .NET 10.0.403 SDK、Widget 锁定依赖与过渡帧缓存；这些都是 `tools/` 或 `artifacts/` 下的本地工具/产物，仓库源码差异仍限定为上述模块、入口、设置页和测试脚本。完整回执汇总：[pure-natural-validation-summary.json](../artifacts/pure-natural-validation-summary.json)。


## 本轮 P1/P2 实现与运行对照

P1 系统内存列表清理在中完整性级别下仍返回 `STATUS_PRIVILEGE_NOT_HELD`，本轮不改系统安全策略，也不进行隐式提权。

- 运行时诊断入口：`--pure-natural-runtime-test`，可选 `--native-backend <MagicSystemNative.exe>`。
- 当前回执：`artifacts/pure-natural-runtime-direct.json`。
- 当前内存条件：物理内存 31 GiB，诊断前可用 14.92 GiB，诊断后可用 14.97 GiB。
- P0：384 MiB 自有夹具后 `K32EmptyWorkingSet` 成功，Working Set 从 34.6 MiB 降至 3.1 MiB，减少 31.5 MiB；Private Bytes 基本不变，符合工作集口径。
- P1：`NtSetSystemInformation` 命令 4/2 均返回 `0xC0000061`，`SeIncreaseQuotaPrivilege` / `SeProfileSingleProcessPrivilege` 在当前令牌中不可用；未观察到系统备用页列表被清理。
- P2：自有 CPU 子进程经 `NtCreateProcessStateChange` 冻结成功，冻结期间 CPU 增量 0 ms，恢复成功且子进程存活。
- P3/P4：配置校验、服务状态只读查询、目录迁移 dry-run 计划已实现；服务停启、驱动、第三方进程动作、Junction 与用户文件移动保持不执行。
- 新增配置测试 `--pure-natural-config-test` 已通过：默认 opt-in/dry-run、读写往返、未知键、体积、边界、计划只读、apply 显式标记。


## 最终实现与运行对照

本轮把 P1/P2 的 Windows API 实现、P3/P4 的策略与只读计划、运行时诊断回执一起纳入社区版；P3/P4 默认仍为 dry-run，不执行服务停启、内核驱动、第三方进程动作、Junction 或用户目录移动。

### 模块

- [PureNaturalConfig.cs](../src/MagicCommunity/PureNaturalConfig.cs)：`Magic.PureNatural.Policy/1` 配置，默认所有主动功能关闭，`mode=dry-run`，字段有边界和未知键校验。
- [PureNaturalConfigTests.cs](../src/MagicCommunity/PureNaturalConfigTests.cs)：配置读写往返、未知键、体积、边界、默认计划只读和 apply 显式标记。
- [PureNaturalDiagnostics.cs](../src/MagicCommunity/PureNaturalDiagnostics.cs)：P0/P1/P2 本地 owned fixture 前后内存对照，P3/P4 服务状态只读查询与迁移计划；`--pure-natural-runtime-test` 可选 `--native-backend`。
- [memory-governor.cpp](../src/system-native/memory-governor.cpp)：`Magic.System.Native/1` 的 `memory-health`、`memory-clean`、`process-freeze`、`process-resume` 深模块。动态解析 ntdll、启用两个特权、记录 `GlobalMemoryStatusEx` 前后值、命令 4/2、NTSTATUS 与 `0xC0000061`；冻结只接受直接子进程并核验完整镜像路径、创建时间、会话及父进程句柄身份。
- [main.cpp](../src/system-native/main.cpp)：四个操作只在普通控制通道，Endpoint 共享一个 MemoryGovernor；断开订阅时 `ResumeAll()`。
- [run-pure-natural-elevated.ps1](../tools/dev/run-pure-natural-elevated.ps1)：同一诊断在高完整性上下文复跑，回执输出到 artifacts。
- [pure-natural-final-summary.json](../artifacts/pure-natural-final-summary.json)：最终对照摘要。

### 实测条件

物理内存 31.29 GiB。最终高完整性复跑前可用 22.46 GiB、内存负载 28%；诊断后可用 25.50 GiB、负载 18%，系统可用物理内存增加 3.05 GiB。最终复跑同时用 [measure_process_group.py](../tools/p0/measure_process_group.py) 只读采样当前 `MagicCommunity.exe`、`MagicSystemNative.exe`、`MagicNativeEntry.exe` 进程组：3 个样本全部 complete，Working Set 合计约 36.53→36.56 MiB，Private Bytes 保持约 288.8 MiB。P1 的系统内存列表命令会影响全局内存状态，这个进程组采样只作现场对照，不把系统级变化归因到单个社区进程。

| 路径 | 中完整性 | 高完整性 |
| --- | --- | --- |
| P0 `K32EmptyWorkingSet` | 成功，Working Set 降约 31 MiB；Private Bytes 基本不变 | 成功，Working Set 降 30.8 MiB；Private Bytes 基本不变 |
| P1 `NtSetSystemInformation` 命令 4/2 | `0xC0000061`，两个特权不可用，未清理内存列表 | 两个命令均 `0x00000000`，可用物理内存增加 1.50 GiB；native backend 另测得 423.0 MiB |
| P2 owned process freeze/resume | 直接 `NtCreateProcessStateChange` 冻结/恢复成功；native backend fallback 冻结/恢复成功 | 直接 state-change 冻结/恢复成功；native backend 的 `memory-clean` 成功，fallback 冻结/恢复成功 |
| P3/P4 | 服务状态只读、迁移计划、配置与 dry-run 校验；服务保持 RUNNING，目录和 Junction 未改 | 同左 |

最终原生后台产物为 `artifacts/native-system/20261109-191954/MagicSystemNative.exe`，SHA-256 `9865BA...DCEE`；`check-native-system.ps1 -Integrated` 返回 `allPass=true`、207 项检查、build.log 无 `error C`。最终 `check-worktree.ps1` 返回 `exitCode=0`、8 项测试 `allPass=true`。完整摘要：[pure-natural-final-summary.json](../artifacts/pure-natural-final-summary.json)。

### 结论

- "占用很少内存" 在社区版有可测效果，但分两层：P0 只降低 Task Manager 常见 Working Set 口径，不降低 Commit/Private Bytes；P1 在高完整性上下文才真正清理系统备用页/工作集列表并释放可用物理内存。
- 中完整性下 P1 返回 `STATUS_PRIVILEGE_NOT_HELD` 是已验证事实，不把可用内存的普通波动算作 P1 成功。P1 生效需要高完整性令牌中 `SeIncreaseQuotaPrivilege` 与 `SeProfileSingleProcessPrivilege` 可启用，程序本身不隐式提权。
- P2 证明自有子进程可被冻结/恢复，CPU 在冻结窗口停止推进；这不等同于官方对第三方进程黑名单/内核驱动的处理，社区版没有实现内核驱动或第三方进程终止。
- P3/P4 目前是配置、只读状态和可审计计划；服务停启、驱动、黑名单、Junction 和目录迁移未实际执行，也没有对当前用户目录做修改。

### 验证命令

```powershell
pwsh -NoProfile -File tools/dev/check-worktree.ps1
pwsh -NoProfile -File tools/dev/check-native-system.ps1 -Integrated
& tools/dotnet/dotnet.exe artifacts/pure-natural-publish/MagicCommunity.dll --root . --pure-natural-runtime-test --report artifacts/pure-natural-runtime-direct-final.json --native-backend artifacts/native-system/20261109-191954/MagicSystemNative.exe
pwsh -NoProfile -File tools/dev/run-pure-natural-elevated.ps1 -NativeBackend artifacts/native-system/20261109-191954/MagicSystemNative.exe
```

### 未验证项

- 不同 Windows 11 build 上 `NtCreateProcessStateChange` ABI 的稳定性；本机已实测 state-change 与 `NtSuspendProcess` fallback 两条路径。
- live desktop 长时 A/B、冷启动时间、软缺页回读和实际续航影响；P0 回执中 `PageFaultCount` 已增加，说明工作集裁剪可能带来后续磁盘/软缺页代价。
- P3/P4 apply 路径的真实服务停启与目录迁移；当前只验证 dry-run、配置边界和执行计划，默认不会对用户环境执行这些动作。

## 官方调度证据与本轮补齐

官方样本的自动调度范围比宣传小，静态证据如下：

| 能力 | 官方自动定时 | 证据 |
| --- | --- | --- |
| P0 自有进程工作集裁剪 | 有 | `StartupWorkingSetTrimDelays` 8/12/20/30/45/120s、`DispatcherTimer` 每 5 分钟、`_interactionTrimTimer`、`ScheduleWorkingSetTrimming` |
| P1 系统内存列表清理 | 无独立定时器证据 | `MemoryManager::ClearMemory` 为可调用方法；日志 `ram clear, stop service/clear process/clear memory` 属一次 CleanMode 三段式动作 |
| P2 进程冻结 | 无独立定时器证据 | `SuspendPidList` + `EnumIconicAndVisiblePid` 为枚举驱动；`Sleep`/`OpenEventW`/`SetEvent` 见于导入表，未定位独立调度器 |
| P3 停服务 | 执行，非定时 | `ServiceManagerCfg.dat` XOR 0x80 得 `SysMain`/`whesvc` `dstStatus="stopped"` |
| P4 目录迁移 | 执行，非定时 | `DiskManager` 含 `CreateJunction`/`SetFolderIcon`/`DenyDeletePermission`/`HandleBatchMoveAction`/`FindSuitableDiskForMigration` |

社区版在官方只有一次性动作的地方补了显式定时器（`PureNaturalScheduler`），默认关闭，需要用户在 `data/pure-natural.json` 中开启：

- `systemMemoryClean` + `systemCleanIntervalMs`（默认 30 分钟，范围 1 分钟–24 小时）
- `ownedProcessFreeze` + `ownedFreezeIntervalMs`（默认 10 分钟）、`freezePids`（仅从自有子进程集合中筛选）

调度器与诊断共用 `PureNaturalNative` 原生原语，避免特权与 NTSTATUS 处理分叉；`Dispose` 时恢复全部被冻结的自有子进程。环境变量 `MAGICCOMMUNITY_DISABLE_PURE_NATURAL_SCHEDULE=1` 可整体关闭。

## P3/P4 可执行实现

`PureNaturalOperations` 提供 `Apply` / `ApplyWithHome` / `Rollback`，通过 `IPureNaturalActions` 缝隙隔离 Windows 适配器：

- 默认 `mode=dry-run`：只返回计划，不调用适配器。
- `mode=apply` 且 `serviceControls=true`：`sc.exe query/stop/query` 记录 before/after；单个服务失败不中断后续。
- `mode=apply` 且 `diskMigration=true` 且提供 `migrationTargetRoot`：`Directory.Move` → `mklink /J` → 写回滚回执；Junction 失败立即把目录移回原路径。
- 拒绝目标已存在、源为重解析点、目标根不存在。
- `Rollback` 读取回执：重启服务、删除 Junction、把目录移回。

实测（本机非管理员）：

| 验证 | 结果 |
| --- | --- |
| P4 真实文件系统夹具 | `movedExists=true`、`junctionIsLink=true`、`readableThroughJunction=true`、`restored=true`、`targetCleared=true` |
| P3 非管理员停服务 | `whesvc` `Changed=false`、before/after 均 `RUNNING`、`ExitCode=5`（拒绝），回执保留 |
| `check-worktree.ps1` | `exitCode=0`，12 项测试全部 `allPass=true` |

P3 的停服务需要管理员令牌；非管理员下按设计返回结构化拒绝，不做隐式提权。

## 官方提权架构与社区版差异

官方 `Launcher.exe`（SHA-256 `FFA227D95D0F3E7C0D8C1BDC19F7604169E23C25EB85892DECF2C6C1694A38E8`）保留完整符号，可直接读出服务创建与提权路径：

```text
OpenSCManagerW / CreateServiceW / CreateAndStartService(bool)
StartServiceWithRetry(struct SC_HANDLE__ *)
StopMagicAnimationService / DeleteMagicAnimationService
CreateGuardService / StopGuardService / DeleteGuardService
RunMagicVisualsAsAdmin(const wchar_t *)
sc.exe sdset MagicAnimationService D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)
```

`SY`=LocalSystem、`BA`=Builtin Administrators。官方把内存清理、停服务、驱动 IOCTL 交给以 LocalSystem 运行的 `MagicAnimationService`，UI 保持普通完整性，日常启动不触发 UAC；另有 `GuardService` 与 `MonProcessControl.sys`。

社区版没有服务层，两个特权只能在调用进程令牌里启用，因此：

| 能力 | 官方 | 社区版 |
| --- | --- | --- |
| 特权承载 | LocalSystem 服务 | UI 进程整体提权 |
| 日常是否触发 UAC | 否 | 是 |
| 权限边界 | UI 普通完整性 + IPC | 全进程完整令牌 |
| 内核管控 | `MonProcessControl.sys` | 未实现 |

实测（管理员令牌，两次独立运行）：可用物理内存分别增加 `4490.4 MiB` 与 `3525.0 MiB`，命令 4/2 均返回 `0x00000000`，`privilegeNotHeld=false`。自动调度回执 `enabled=true, systemCleanIntervalMs=900000`，周期 15 分钟。

结论：数值效果可复现，权限架构不等价。要达到官方形态需新增 LocalSystem 服务并在 UI 与服务之间加 IPC，这部分尚未实现。

## LocalSystem 服务层（与官方架构对齐）

社区版新增 `MagicMemoryService.exe`，以 LocalSystem 运行，承接所有需要特权的动作。桌面进程保持普通令牌，通过命名管道请求服务执行。

### 命令集（固定，无通用执行面）

| 命令 | 行为 | 允许列表 |
| --- | --- | --- |
| `health` | 返回服务标识与版本 | — |
| `clean` | 启用两个特权并执行命令 4/2 | — |
| `stop-service:<name>` | 停止宿主服务 | 仅 `SysMain`、`whesvc` |
| `migrate-folder:<folder>\0<target>\0<profile>` | 移动目录并建 Junction | 仅 `Desktop`、`Downloads`、`Documents` |

参数使用 NUL 分隔，避免 Windows 路径中的 `|` 或空格破坏解析。

### 安全边界

- 管道 SDDL：`D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`
- 客户端会话校验：`GetNamedPipeClientProcessId` + `ProcessIdToSessionId`，拒绝会话 0
- 未知命令、未知服务名、未知目录名一律返回结构化拒绝，不执行
- Junction 建立失败时自动把目录移回，回执记录 `rolledBack`

### 与官方的对应

| 能力 | 官方 | 社区版 |
| --- | --- | --- |
| 特权服务 | `MagicAnimationService`（LocalSystem） | `MagicCommunityMemoryService`（LocalSystem） |
| 创建方式 | `OpenSCManagerW` + `CreateServiceW` + SDDL | `sc create obj= LocalSystem` + 同构 SDDL |
| UI 权限 | 普通完整性 | 普通完整性 |
| 调用通道 | IPC | 命名管道 + 会话校验 |
| 内核驱动 | `MonProcessControl.sys` | 未实现 |
| 守护服务 | `GuardService` | 未实现（`MagicDesktopGuard.exe` 非服务） |

### 普通权限实测（`--memory-service-fixture`）

```text
admin = False

clean:  PurgeStatus=0  EmptyStatus=0  PrivilegeEnabled=true  PrivilegeError=0
        AvailableDeltaBytes = 770,437,120 ~ 1,276,391,424
        Effective = true

stop-service: whesvc  RUNNING -> STOPPED  Stopped=true  (已有实例时为 1062 ERROR_SERVICE_ALREADY_STOPPED)
stop-service: Spooler -> {"ok":false,"error":"service-not-allowed"}
migrate: Desktop 成功 moved+junctioned；不存在目标 -> win32Error=3
migrate: Pictures -> {"ok":false,"error":"folder-not-allowed"}
```

8 项检查全部通过，均在桌面普通令牌下完成，无 UAC。

### 安装与卸载

- 安装器解压后校验 `installed-desktop.json` 全部哈希，不匹配即抛错
- 安装前先停旧服务并等待进程退出，避免镜像占用导致复制失败
- 服务随安装自动 `sc create obj= LocalSystem start= auto` 并启动
- `MagicCommunity-Setup-*.exe --uninstall` 停服务、删除服务、移除安装目录，保留用户数据

### 仍未实现

- 内核驱动等价物（进程创建通知、工作集边界、黑名单终止）
- 独立 `GuardService`
- 服务端尚无 P4 回滚命令（回执已记录 `rolledBack`，回滚仍由客户端侧完成）
