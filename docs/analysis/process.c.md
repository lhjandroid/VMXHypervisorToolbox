# process.c -- 逻辑分析

## 1. 文件概述

### 角色与职责
`process.c` 是 VMX Hypervisor Toolbox 中的 **进程跟踪管理模块**，负责：
- **EPROCESS 关键偏移动态发现**：通过扫描当前进程的 EPROCESS 结构，自动定位 `DirectoryTableBase`（即 CR3 值）的字段偏移，从而消除对硬编码偏移的依赖。
- **目标进程管理**：维护一个目标进程列表（最多 16 个），支持添加、移除、更新目标进程的属性（PID、CR3、AAD_HIDE_\* 标志位）。
- **快速 CR3 查找**：提供 `ProcessFindByCr3()` 函数，供 VM-Exit 处理程序在 ISR 上下文中快速确定当前运行进程是否为目标进程、以及启用了哪些隐藏功能。
- **异常拦截回调机制**：提供回调注册接口，使 SVM/VMX 后端能够在目标进程列表变化时同步更新其异常拦截位图。

### 为什么需要这个模块
在 Type-2 蓝 pill 式 Hypervisor 中，VM-Exit 发生时（EPT 违例、CPUID、异常等），Hypervisor 需要快速判断当前 Guest 进程是否是受保护的进程，以及应该对其应用哪些隐藏策略。通过 CR3（页表基址寄存器）匹配是实现进程识别的最直接方式——每个进程的 CR3 指向其独立的页目录，CR3 值天然就是进程的唯一标识。

### 依赖的其他模块
| 被依赖模块 | 头文件 | 使用方式 |
|-----------|-------|---------|
| `log.h` | `driver/log.h` | 日志输出 |
| `shared.h` | `common/shared.h` | AAD_HIDE_\* 标志常量定义 |

---

## 2. 数据结构

### 2.1 目标进程 -- `TARGET_PROCESS (process.h:21-26)`

```c
typedef struct _TARGET_PROCESS {
    ULONG64     Cr3;            /* DirectoryTableBase 用于 CR3 匹配 */
    ULONG       Pid;            /* 进程 ID */
    ULONG       Flags;          /* AAD_HIDE_* 位掩码 */
    BOOLEAN     Active;         /* 槽位是否在使用中 */
} TARGET_PROCESS, *PTARGET_PROCESS;
```

| 字段 | 说明 |
|------|------|
| `Cr3` | 进程的 CR3 值（页表物理基址），用于 VM-Exit 时的快速识别 |
| `Pid` | Windows 进程 ID，由用户态通过 IOCTL 传入 |
| `Flags` | AAD_HIDE_\* 位掩码组合，控制对该进程应用的隐藏技术 |
| `Active` | 标记此槽位是否有效（便于并发安全的惰性删除） |

### 2.2 进程跟踪状态 -- `PROCESS_TRACKING (process.h:28-33)`

```c
typedef struct _PROCESS_TRACKING {
    TARGET_PROCESS  Targets[MAX_TARGET_PROCESSES];  /* 目标进程数组 */
    KSPIN_LOCK      Lock;                            /* 保护 Target 数组的自旋锁 */
    ULONG           ActiveCount;                     /* 当前活动的目标数 */
    BOOLEAN         Initialized;                     /* 模块是否已初始化 */
} PROCESS_TRACKING, *PPROCESS_TRACKING;
```

| 字段 | 说明 |
|------|------|
| `Targets[]` | 固定大小的目标数组，使用数组而非链表以支持 VM-Exit 时无锁读取 |
| `Lock` | 保护 Targets 数组的自旋锁（仅写操作需要） |
| `ActiveCount` | 当前活动目标数，方便快速查询 |

### 2.3 EPROCESS 偏移 -- `EPROCESS_OFFSETS (process.h:44-47)`

```c
typedef struct _EPROCESS_OFFSETS {
    ULONG   DirectoryTableBase;  /* EPROCESS 中 CR3 字段的字节偏移 */
    BOOLEAN Resolved;            /* 偏移是否已成功发现 */
} EPROCESS_OFFSETS, *PEPROCESS_OFFSETS;
```

### 2.4 常量定义

| 常量 | 值 | 说明 |
|------|-----|------|
| `MAX_TARGET_PROCESSES` | 16 | 最大目标进程数 |
| `EPROCESS_SCAN_SIZE` | 0x700 (1792) | EPROCESS 结构扫描最大字节数 |
| `KNOWN_DTB_OFFSETS[]` | {0x028, 0x018, 0x02C} | 已知的 Windows 版本 DTB 偏移回退表 |

### 2.5 全局变量

```c
PROCESS_TRACKING  g_ProcessTracking;   /* 进程跟踪全局状态 */
EPROCESS_OFFSETS  g_EprocessOffsets;   /* EPROCESS 偏移全局状态 */
```

两个全局变量均无 `extern` 修饰——它们是本模块定义的，通过头文件中的 `extern` 声明暴露给其他模块。

### 2.6 AAD_HIDE_* 标志位（来自 shared.h）

| 标志 | 值 | 功能 |
|------|-----|------|
| `AAD_HIDE_DEBUGGER` | 1<<0 | 隐藏调试器存在（PEB.BeingDebugged 等） |
| `AAD_HIDE_HWBP` | 1<<1 | 硬件断点 DR0-DR7 隐藏 |
| `AAD_HIDE_TIMING` | 1<<2 | RDTSC/RDTSCP 时间偏移补偿 |
| `AAD_HIDE_CPUID` | 1<<3 | CPUID 隐藏 Hypervisor |
| `AAD_HIDE_SYSINFO` | 1<<4 | NtQuerySystemInformation 伪造 |
| `AAD_HIDE_EXCEPTIONS` | 1<<5 | INT 2D/INT 3 行为规范化 |
| `AAD_HIDE_NTCLOSE` | 1<<6 | NtClose 无效句柄异常处理 |
| `AAD_HIDE_THREADINFO` | 1<<7 | NtSetInformationThread HideFromDebugger |
| `AAD_HIDE_HEAP` | 1<<8 | 堆标志隐藏 |
| `AAD_HIDE_PARENT` | 1<<9 | 父进程伪造 |
| `AAD_HIDE_ALL` | 0xFFFFFFFF | 全部启用 |

---

## 3. 核心函数详解

### 3.1 EPROCESS 偏移发现

#### `ValidateDtbOffset() (line 82-118)`

**签名**：`static BOOLEAN ValidateDtbOffset(ULONG Offset)`

**功能**：验证一个候选的 DirectoryTableBase 偏移是否有效。

**验证逻辑**：
1. 获取当前进程（System 进程）的 EPROCESS 指针：`PsGetCurrentProcess()`
2. 读取当前 CR3：`__readcr3()`
3. 读取 EPROCESS 中候选偏移处的值
4. 比较（忽略低 12 位 PCID 标志）：`(StoredCr3 & ~0xFFF) == (CurrentCr3 & ~0xFFF)`
5. 附加验证：值不为零、物理地址 < 48 位上限

**为什么忽略低 12 位**：CR3 的低 12 位包含 PCID（Process-Context Identifier）和标志位，当 `CR4.PCIDE = 1` 时这些位的值不稳定。而物理页帧号（PFN）在高位，是稳定的比较依据。

#### `ProcessResolveOffsets() (line 120-228)`

**签名**：`NTSTATUS ProcessResolveOffsets(VOID)`

**功能**：动态发现 EPROCESS 结构中 `DirectoryTableBase`（CR3）字段的偏移。使用三层策略：

**策略 1 -- CR3 扫描（首选）**：
- 扫描当前 EPROCESS 从偏移 0 到 0x700，每次 8 字节
- 收集所有值匹配当前 CR3 的偏移位置
- 处理 KVA Shadow (KPTI) 的情况：EPROCESS 中可能同时存在 `DirectoryTableBase`（内核 CR3）和 `UserDirectoryTableBase`（用户态 CR3 影子），两者紧邻且相似
- 解决方案：收集所有候选，取**最小偏移**（因为 `DirectoryTableBase` 始终在结构的较早位置）
- 通过 `ValidateDtbOffset()` 最终验证

**策略 2 -- 已知偏移回退**：
- 如果 CR3 扫描失败（罕见情况），尝试已知的 Windows 通用偏移：
  - `0x028`：Windows 10 1507-22H2、Windows 11 21H2-24H2
  - `0x018`：Windows 7/8 x64
  - `0x02C`：部分 Insider 预览版
- 通过 `ValidateDtbOffset()` 验证每个候选

**策略 3**（注释中提到，未实际实现代码）：
- 使用 `KeAttachProcess` 附加到其他进程，交叉验证 CR3 值

**返回值**：
- `STATUS_SUCCESS`：偏移发现成功
- `STATUS_NOT_FOUND`：所有策略均失败

**KPTI 兼容性修复**：
Windows 启用 KVA Shadow（内核页表隔离）时，EPROCESS 中新增了 `UserDirectoryTableBase` 字段。该字段紧邻 `DirectoryTableBase` 且值相近（用户 CR3 是内核 CR3 的轻量镜像）。旧的"取第一个匹配"策略可能选中 `UserDirectoryTableBase` 而非 `DirectoryTableBase`。

解决方案：收集**所有**匹配偏移，取**最小偏移**——因为 `DirectoryTableBase` 在结构体中的序数位置早于 `UserDirectoryTableBase`，其偏移必然更小。

---

### 3.2 初始化与销毁

#### `ProcessTrackingInit() (line 234-251)`

**签名**：`VOID ProcessTrackingInit(VOID)`

**功能**：初始化进程跟踪子系统。

**流程**：
1. 清零 `g_ProcessTracking` 全局状态
2. 初始化自旋锁 `g_ProcessTracking.Lock`
3. 调用 `ProcessResolveOffsets()` 发现 EPROCESS 偏移
4. 标记 `Initialized = TRUE`
5. 即使偏移发现失败也继续——此时 `g_EprocessOffsets.Resolved = FALSE`，后续 `GetProcessCr3()` 会优雅地失败

#### `ProcessTrackingCleanup() (line 253-257)`

**签名**：`VOID ProcessTrackingCleanup(VOID)`

**功能**：清理进程跟踪状态。将 `Initialized` 设为 FALSE，`ActiveCount` 归零。不需要释放目标进程条目（它们只是数值，没有持有对象引用）。

---

### 3.3 CR3 获取与目标管理

#### `GetProcessCr3() (line 263-292)`

**签名**：`static NTSTATUS GetProcessCr3(ULONG Pid, PULONG64 OutCr3)`

**功能**：获取指定进程的 CR3（DirectoryTableBase）值。

**流程**：
1. 检查 `g_EprocessOffsets.Resolved`——如果偏移未解决，返回 `STATUS_NOT_SUPPORTED`
2. 调用 `PsLookupProcessByProcessId(Pid, &Process)` 获取 EPROCESS 指针
3. 读取 `*(PULONG64)((PUCHAR)Process + g_EprocessOffsets.DirectoryTableBase)`
4. 释放 EPROCESS 引用（`ObDereferenceObject`）
5. 验证 CR3 不为零

**注意**：此函数在调用者上下文中运行（通常为 PASSIVE_LEVEL 的 IOCTL 分发），因此可以安全地调用 `PsLookupProcessByProcessId` 和 `ObDereferenceObject`。

#### `ProcessAddTarget() (line 294-347)`

**签名**：`NTSTATUS ProcessAddTarget(ULONG Pid, ULONG Flags)`

**功能**：添加一个新的目标进程到跟踪列表。

**流程**：
1. 验证模块已初始化
2. 调用 `GetProcessCr3()` 将 PID 解析为 CR3 值
3. 加自旋锁
4. 检查 PID 是否已存在——如果存在，更新 Flags 和 Cr3（刷新 CR3，因为进程可能已重新创建）
5. 如果不存在，在 Targets 数组中找第一个空闲槽位（`Active == FALSE`）
6. 写入 PID、CR3、Flags、Active = TRUE，递增计数
7. 释放自旋锁
8. 调用 `ProcessSyncSvmInterceptsAfterConfigChange()` 通知后端更新拦截状态

**容量**：最多 16 个目标，超过则返回 `STATUS_INSUFFICIENT_RESOURCES`。

#### `ProcessRemoveTarget() (line 349-380)`

**签名**：`NTSTATUS ProcessRemoveTarget(ULONG Pid)`

**功能**：从跟踪列表移除一个目标进程。

**流程**：
1. 在自旋锁保护下查找匹配的 PID
2. 将槽位标记为 `Active = FALSE`，清零字段
3. 递减 `ActiveCount`
4. 调用 `ProcessSyncSvmInterceptsAfterConfigChange()` 通知后端

#### `ProcessUpdateConfig() (line 382-408)`

**签名**：`NTSTATUS ProcessUpdateConfig(ULONG Pid, ULONG NewFlags)`

**功能**：更新已跟踪进程的标志位（如增加或移除某个隐藏功能）。

**流程**：
1. 查找匹配 PID
2. 仅更新 `Flags` 字段，不更改 CR3 或 PID
3. 通知后端更新拦截状态

#### `ProcessGetActiveCount() (line 410-413)`

**签名**：`ULONG ProcessGetActiveCount(VOID)`

无锁读取 `ActiveCount`。用于状态查询 IOCTL 的快速响应。

---

### 3.4 快速 CR3 查找（VM-Exit 热路径）

#### `ProcessFindByCr3() (line 464-498)`

**签名**：`PTARGET_PROCESS ProcessFindByCr3(ULONG64 Cr3)`

**这是 VM-Exit 热路径的核心函数**——每次 VM-Exit 至少调用一次。

**调用上下文**：VM-Exit 处理程序（ISR 级别，HIGH_LEVEL IRQL）。

**为什么可以不用自旋锁**：
- Target 数组是固定大小的，槽位 `Active` 标志的写入是原子操作（BOOLEAN 写入在 x64 上是原子的）
- 线程/进程管理 IOCTL 运行在 PASSIVE_LEVEL，不能与 HIGH_LEVEL 的 VM-Exit 处理程序竞争
- 锁定在此处会带来不可接受的性能开销

**CR3 掩码处理**（Bug fix M-2）：
```
Cr3Masked = Cr3 & 0x000FFFFFFFFFF000ULL;
```
- 掩码低 12 位（PCID + 标志位）
- 掩码高 12 位（位 63 的保留 TLB 标志、位 62-52 的保留位）
- 只保留 [51:12] 的实际物理页帧号

**性能特性**：线性扫描最多 16 个条目，每次 VM-Exit 的额外 CPU 开销极小。

---

### 3.5 异常隐藏回调机制

#### `ProcessRegisterExceptionHideToggle() (line 21-29)`

**签名**：`VOID ProcessRegisterExceptionHideToggle(PFN_EXCEPTION_HIDE_TOGGLE Callback)`

**功能**：注册一个后端回调函数，当目标进程配置变化时接收通知。

**调用者**：SVM 或 VMX 的初始化代码（在 DriverEntry 期间，Hypervisor 启动后调用）。

**为什么需要这个机制**：
- 某些 anti-anti-debug 功能（如 AAD_HIDE_EXCEPTIONS）需要 Hypervisor 拦截特定的异常事件（如 #BP/INT3）
- 当目标列表变化时，后端需要更新其 VMCB（AMD）或 VMCS（Intel）中的异常拦截位图
- 这种设计避免 `process.c` 直接依赖 SVM 或 VMX 的具体实现（解耦）

#### `ProcessAnyTargetHasExceptionHiding() (line 423-441)`

**签名**：`BOOLEAN ProcessAnyTargetHasExceptionHiding(VOID)`

**功能**：检查是否有任何活动目标启用了 `AAD_HIDE_EXCEPTIONS` 标志。

**流程**：
1. 加自旋锁（此函数可能在 PASSIVE_LEVEL 调用）
2. 遍历 Targets 数组，检查 `Active && (Flags & AAD_HIDE_EXCEPTIONS)`
3. 释放自旋锁

#### `ProcessSyncSvmInterceptsAfterConfigChange() (line 443-457)`

**签名**：`static VOID ProcessSyncSvmInterceptAfterConfigChange(VOID)`

**功能**：在每次目标配置变更（添加/移除/更新）后，调用注册的回调以同步后端异常拦截状态。

**调用链**：
```
ProcessAddTarget/RemoveTarget/UpdateConfig
  -> ProcessSyncSvmInterceptsAfterConfigChange()
    -> g_ExceptionHideToggleCb(ProcessAnyTargetHasExceptionHiding())
```

---

### 3.6 内联辅助函数（process.h 中定义）

```c
FORCEINLINE BOOLEAN IsTargetProcess(ULONG64 Cr3)
{
    return ProcessFindByCr3(Cr3) != NULL;
}

FORCEINLINE BOOLEAN IsFeatureEnabled(ULONG64 Cr3, ULONG FeatureFlag)
{
    PTARGET_PROCESS Target = ProcessFindByCr3(Cr3);
    if (Target) {
        return (Target->Flags & FeatureFlag) != 0;
    }
    return FALSE;
}
```

这两个内联函数是 VM-Exit 处理程序中最常用的判断接口：
- `IsTargetProcess(Cr3)`：当前进程是否受保护
- `IsFeatureEnabled(Cr3, AAD_HIDE_CPUID)`：当前进程是否启用了某特定隐藏功能

---

## 4. 控制流与逻辑流程

### 4.1 初始化流程

```
DriverEntry
  -> CPU 检测（Intel/AMD）
  -> 初始化 Hypervisor 后端
  -> // 某处调用：
     ProcessTrackingInit()
       -> RtlZeroMemory(&g_ProcessTracking)
       -> KeInitializeSpinLock(&Lock)
       -> ProcessResolveOffsets()
            -> PsGetCurrentProcess() + __readcr3()
            -> CR3 扫描 (0..0x700, step 8)
               -> 收集所有匹配偏移
               -> 取最小偏移
               -> ValidateDtbOffset() 验证
            -> 如果失败: 尝试已知偏移回退
         -> g_EprocessOffsets.DirectoryTableBase = 偏移
       -> g_ProcessTracking.Initialized = TRUE
```

### 4.2 目标添加流程

```
用户态调用: DeviceIoControl(IOCTL_VMX_SET_TARGET, VMX_TARGET_INFO{Pid, Flags})
  -> vmxdrv.c IOCTL 调度
     -> ProcessAddTarget(Pid, Flags)
        -> GetProcessCr3(Pid, &Cr3)         // PID -> CR3 翻译
             -> PsLookupProcessByProcessId(Pid)
             -> *(EPROCESS + DtbOffset)
             -> ObDereferenceObject(Process)
        -> 自旋锁保护
        -> 查找是否已存在？（更新或新建）
        -> 写入槽位
        -> 释放自旋锁
        -> ProcessSyncSvmInterceptsAfterConfigChange()
```

### 4.3 VM-Exit 时进程识别流程

```
VM-Exit 发生（EPT 违例 / CPUID / MOV CR3 / 异常...）
  -> vm_exit.c / svm_exit.c 调度
     -> // 需要判断当前 Guest 进程是否为目标
     -> GuestCr3 = HvReadGuestCr3()            // 从 VMCS/VMCB 读取
     -> Target = ProcessFindByCr3(GuestCr3)     // CR3 匹配
        -> Cr3Masked = Cr3 & 0x000FFFFFFFFFF000ULL
        -> 线性扫描 Targets[0..15]
        -> 返回 PTARGET_PROCESS 或 NULL
     -> if (Target)
     ->    if (Target->Flags & AAD_HIDE_CPUID)
     ->       处理 CPUID 隐藏...
     ->    if (Target->Flags & AAD_HIDE_EXCEPTIONS)
     ->       处理异常拦截...
```

---

## 5. 与其他模块的交互

### 5.1 与 `vmxdrv.c`（驱动程序入口）的交互

`vmxdrv.c` 处理用户态 IOCTL 请求，并将目标管理操作委托给 `process.c`：
- `IOCTL_VMX_SET_TARGET` -> `ProcessAddTarget(Pid, Flags)`
- `IOCTL_VMX_REMOVE_TARGET` -> `ProcessRemoveTarget(Pid)`
- `IOCTL_VMX_SET_CONFIG` -> `ProcessUpdateConfig(Pid, NewFlags)`
- `IOCTL_VMX_QUERY_STATUS` -> `ProcessGetActiveCount()`

### 5.2 与 `vmx_exit.c` / `svm_exit.c`（VM-Exit 处理）的交互

这是最关键的交互点。VM-Exit 处理器的热路径中通过 `ProcessFindByCr3()` 判断当前 Guest 上下文：

```
// VM-Exit 处理伪代码
GuestCr3 = HvReadGuestCr3();
Target = ProcessFindByCr3(GuestCr3);
if (Target) {
    switch(ExitReason) {
        case EXIT_REASON_CPUID:
            if (Target->Flags & AAD_HIDE_CPUID) { ... }
            break;
        case EXIT_REASON_EXCEPTION:
            if (Target->Flags & AAD_HIDE_EXCEPTIONS) { ... }
            break;
        ...
    }
}
```

### 5.3 与 `svm_init.c` / `vmx_init.c`（后端初始化）的交互

后端初始化代码调用 `ProcessRegisterExceptionHideToggle()` 注册一个回调：
```c
// SVM 后端的初始化
ProcessRegisterExceptionHideToggle(SvmToggleExceptionIntercept);
// 或 VMX 后端的初始化
ProcessRegisterExceptionHideToggle(VmxToggleExceptionIntercept);
```

当目标列表变化时，注册的回调被调用，后端相应更新 VMCB 的异常拦截位或 VMCS 的 Exception Bitmap。

### 5.4 与 `anti_anti_debug.c` 的交互

`anti_anti_debug.c` 在 VM-Exit 处理时使用 `IsFeatureEnabled()` 或直接访问 `Target->Flags` 来判断需要对当前 Guest 进程应用哪些 anti-anti-debug 技术。例如：
- `AAD_HIDE_DEBUGGER` -> 隐藏 PEB.BeingDebugged
- `AAD_HIDE_HWBP` -> 清除 DR 寄存器
- `AAD_HIDE_TIMING` -> 设置 TSC 偏移

### 5.5 与 `hv_mem.c`（内存引擎）的交互

`hv_mem.c` 需要进程的 CR3 来翻译虚拟地址到物理地址。`process.c` 提供的 `g_EprocessOffsets.DirectoryTableBase` 偏移在 `hv_mem.c` 中也可能被使用，或该模块通过 `ProcessFindByCr3()` 获取进程上下文。

### 5.6 与 `shared.h` 的交互

`process.c` 不直接使用 `shared.h` 中的结构体，但 `process.h` 中引用的 `AAD_HIDE_*` 标志位定义在 `shared.h` 中。这些标志位通过用户态 IOCTL 传入，由 `process.c` 存储，供其他内核模块检查。

---

## 6. 关键设计要点

### 6.1 EPROCESS.DirectoryTableBase 偏移的自动发现

**为什么不能硬编码**：
- EPROCESS 结构体在不同的 Windows 主版本（Win7/8/10/11）和构建版本中偏移各不相同
- 即使在同一 Windows 版本的不同更新中，偏移也可能发生变化
- 一个需要跨版本兼容的 Hypervisor 不能依赖任何硬编码偏移

**发现原理**：
1. 在 System 进程上下文中运行时，`__readcr3()` 返回当前加载的页表基址
2. EPROCESS 结构的 `DirectoryTableBase` 字段保存了该进程的 CR3 值
3. 扫描 EPROCESS 结构寻找一个匹配当前 CR3 的 64 位值
4. 唯一匹配的位置就是 `DirectoryTableBase` 字段

**KPTI/KVA Shadow 的特殊处理**：
KPTI 引入后，EPROCESS 中新增了一个 `UserDirectoryTableBase` 字段（用于用户态页表），其值接近 `DirectoryTableBase`。简单取第一个匹配可能选错。解决方案是取所有候选中的最小偏移。

### 6.2 无锁 CR3 查找的设计权衡

`ProcessFindByCr3()` 是 **无锁的线性扫描**：

**为什么无锁是安全的**：
- 写操作（Add/Remove/UpdateConfig）在 `PASSIVE_LEVEL` 的 IOCTL 调度中执行，持有自旋锁
- 读操作（ProcessFindByCr3）在 `HIGH_LEVEL` 的 VM-Exit 处理中执行
- 写操作不会被读操作抢占——IOCTL 调度不会在 VM-Exit 处理过程中被调用
- 读操作不会被写操作干扰——VM-Exit 处理是关中断的，写操作无法执行
- `BOOLEAN Active` 的写入是原子的

**为什么选择数组而不是链表**：
- 数组支持 O(1) 索引和线性扫描，而链表需要指针解引用
- 数组元素在内存中是连续的，有更好的缓存局部性
- 最多 16 个元素，线性扫描的开销微不足道（几十个 CPU 周期）

### 6.3 CR3 掩码与 PCID 兼容性

CR3 在 x64 中的布局（Intel SDM Vol.3 4.5）：
```
Bit 63    : 保留，用于 MOV CR3 时的 TLB 保持标志（写入时有效，读出时为 0）
Bit 62:52 : 保留（必须为 0）
Bit 51:12 : 物理页帧号（PML4 基址）
Bit 11:0  : PCID（Process-Context Identifier）或忽略位
```

`ProcessFindByCr3()` 使用掩码 `0x000FFFFFFFFFF000ULL` 只提取物理页帧号部分，确保：
- PCID 开关机不影响匹配（某些系统 PCID 启用，某些不启用）
- 位 63 的 TLB 保持标志不影响匹配（即使 guest 写入带该标志的 CR3）
- 遵循 Intel 手册：CR3 比较应只基于 PFN

### 6.4 后端解耦设计

`process.c` 不直接调用 SVM 或 VMX 的任何函数，而是通过回调函数注册机制与后端交互：

```
process.c                   后端 (svm_init.c / vmx_init.c)
    |                                |
    |<--- ProcessRegisterException --|
    |         HideToggle(callback)   |
    |                                |
    |--- ProcessAddTarget(...)       |
    |     -> g_ExceptionHideToggleCb |
    |        (anyHasHiding) -------->| 更新 VMCB/VMCS 异常拦截
```

这种设计的优势：
- `process.c` 不需要包含 `svm.h` 或 `vmx.h`
- 新增后端（如果有）只需注册自己的回调
- 编译时没有依赖循环
- 运行时没有条件分支（if Intel / if AMD）——静态的回调指针调用

### 6.5 容错性与优雅降级

- **偏移发现失败**：`ProcessResolveOffsets()` 失败时，`ProcessTrackingInit()` 仍然继续，`GetProcessCr3()` 会优雅地返回 `STATUS_NOT_SUPPORTED`
- **进程不存在**：`PsLookupProcessByProcessId` 失败时，`ProcessAddTarget` 将错误返回给用户态
- **数组满**：返回 `STATUS_INSUFFICIENT_RESOURCES`，用户态可以移除不需要的进程后重试
- **空 CR3 匹配**：`ProcessFindByCr3` 在无目标时返回 `NULL`，VM-Exit 处理程序根据 NULL 判断不执行任何隐藏策略
