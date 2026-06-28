# shadow_ssdt.c -- 逻辑分析

## 1. 文件概述

### 角色与职责
`shadow_ssdt.c` 是 VMX Hypervisor Toolbox 中负责 **Shadow SSDT (KeServiceDescriptorTableShadow) 监控与 Hook 框架** 的核心模块。Shadow SSDT 是 Windows 内核中的第二个系统服务描述表，专门用于 win32k 子系统（NtUser/NtGdi 系列系统调用）。

其职责包括：

- **KTHREAD.ServiceTable 偏移动态发现**：通过扫描 System 进程（PID=4）的内核线程 KTHREAD 结构，找到 `ServiceTable` 字段的字节偏移（该偏移在不同 Windows 版本中各不相同）。
- **KeServiceDescriptorTableShadow 定位**：KeServiceDescriptorTableShadow 是未导出符号，通过搜索 GUI 线程的 KTHREAD.ServiceTable 找到它。
- **W32pServiceTable 地址解析**：从 Shadow 表的第二个描述符（Shadow[1]）中提取 win32k 系统调用表基址和数量。
- **Win32k 模块枚举**：支持 Win10+ 的 win32k 三模块拆分（win32k.sys、win32kbase.sys、win32kfull.sys）。
- **跨会话内存访问**：由于 win32k 映射是 per-Session 的，使用 `KeStackAttachProcess` 附加到 GUI 进程上下文来读取 W32pServiceTable 和 PE 导出表。
- **Hook 协调**：通过 `GenericHookInstall()` / `GenericHookRemove()` 框架，对 NtUser/NtGdi 系统调用部署 EPT/NPT 级别不可见 Hook。
- **监控模式**：支持全量和过滤两种监控模式，与标准 SSDT 模块的 API 风格一致。

### 与标准 SSDT 模块的关键区别
| 方面 | SSDT (ssdt.c) | Shadow SSDT (shadow_ssdt.c) |
|------|---------------|----------------------------|
| 目标表 | `nt!KiServiceTable` | `win32k!W32pServiceTable` |
| 涉及的模块 | ntoskrnl.exe | win32k.sys / win32kbase.sys / win32kfull.sys |
| 表发现方式 | 逆向 Zw\* 存根，暴力扫描 | 扫描 KTHREAD.ServiceTable 字段 |
| 命名空间 | Nt\* 系统调用 | NtUser\*/NtGdi\* 系统调用 |
| Session 感知 | 不需要 | 需要 KeStackAttachProcess 到 GUI 进程 |
| 最大服务数 | 512 | 2048 |
| IOCTL 范围 | 0x80D-0x813 | 0x814-0x81A |

### 依赖的其他模块
| 被依赖模块 | 头文件 | 使用方式 |
|-----------|-------|---------|
| `ssdt.h` | `driver/ssdt.h` | 复用 SSDT_HOOK_MAPPING、SSDT_ENTRY_INFO 等结构 |
| `hv_hook.h` | `driver/hv_hook.h` | 调用 GenericHookInstall/Remove 部署 Hook |
| `log.h` | `driver/log.h` | LOG_INFO / LOG_WARN / LOG_ERROR / LOG_DEBUG |
| `shared.h` | `common/shared.h` | 共享数据结构（HOOK_RULE、SSDT_ENTRY_INFO 等） |
| `ntddk.h` | WDK | 内核 API |
| `ntimage.h` | WDK | PE 结构体解析 |

---

## 2. 数据结构

### 2.1 全局状态 -- `SHADOW_SSDT_STATE (shadow_ssdt.h:61-95)`

```c
typedef struct _SHADOW_SSDT_STATE {
    BOOLEAN     Initialized;

    /* KTHREAD 偏移发现结果 */
    KTHREAD_OFFSETS KthreadOffsets;

    /* 发现结果 */
    ULONG64     KeServiceDescriptorTableShadowVa; /* Shadow 表基址 */
    ULONG64     W32pServiceTableVa;               /* Shadow[1].Base = W32pServiceTable */
    ULONG       ServiceCount;                     /* Shadow[1].Limit = 服务数 */

    /* GUI 进程上下文（持有引用计数用于 KeStackAttachProcess） */
    PEPROCESS   GuiProcess;

    /* 地址缓存 */
    ULONG64     ResolvedAddresses[SHADOW_SSDT_MAX_SERVICES];

    /* 名称缓存 */
    WCHAR       NameCache[SHADOW_SSDT_MAX_SERVICES][SSDT_MAX_NAME_LEN];
    BOOLEAN     NamesPopulated;

    /* win32k 模块信息（支持 Win10+ 拆分模块） */
    ULONG               Win32kModuleCount;
    WIN32K_MODULE_INFO   Win32kModules[MAX_WIN32K_MODULES];

    /* Hook 映射链表 */
    PSSDT_HOOK_MAPPING  HookListHead;
    ULONG               HookCount;
    KSPIN_LOCK          HookLock;

    /* 监控模式 */
    ULONG       MonitorMode;   /* SSDT_MONITOR_OFF/ALL/FILTERED */
    ULONG       MonitorPid;
} SHADOW_SSDT_STATE;
```

| 字段 | 说明 |
|------|------|
| `KthreadOffsets.ServiceTableOffset` | 动态发现的 KTHREAD 中 ServiceTable 指针的字节偏移 |
| `KeServiceDescriptorTableShadowVa` | 未导出的 KeServiceDescriptorTableShadow 地址 |
| `W32pServiceTableVa` | win32k 系统调用表基地址（Shadow[1].Base） |
| `GuiProcess` | 持有引用的 GUI 进程 EPROCESS，供 KeStackAttachProcess 使用 |
| `Win32kModules[]` | 枚举到的 win32k 系列模块基址、大小、路径信息 |
| `ResolvedAddresses[]` | 预解析的 Shadow SSDT 条目函数地址 |

### 2.2 KTHREAD 偏移 -- `KTHREAD_OFFSETS (shadow_ssdt.h:32-35)`

```c
typedef struct _KTHREAD_OFFSETS {
    BOOLEAN Resolved;
    ULONG   ServiceTableOffset;  /* KTHREAD.ServiceTable 的字节偏移 */
} KTHREAD_OFFSETS;
```

### 2.3 Win32k 模块信息 -- `WIN32K_MODULE_INFO (shadow_ssdt.h:50-55)`

```c
typedef struct _WIN32K_MODULE_INFO {
    ULONG64 Base;
    ULONG   Size;
    CHAR    Name[64];     /* 短名称：win32k.sys / win32kbase.sys / win32kfull.sys */
    WCHAR   Path[260];    /* NT 路径 */
} WIN32K_MODULE_INFO;
```

支持最多 4 个模块（`MAX_WIN32K_MODULES = 4`），覆盖 Win10+ 三模块拆分。

### 2.4 内部枚举/结构（局部于 shadow_ssdt.c）

#### `KSERVICE_TABLE_DESCRIPTOR_SHADOW (shadow_ssdt.c:157-162)`

```c
typedef struct _KSERVICE_TABLE_DESCRIPTOR_SHADOW {
    PLONG       Base;     /* 系统调用表基址 */
    PULONG      Count;    /* 服务计数指针 */
    ULONG64     Limit;    /* 服务数上限 */
    PUCHAR      Number;   /* 表编号 */
} KSERVICE_TABLE_DESCRIPTOR_SHADOW;
```

KeServiceDescriptorTable 和 KeServiceDescriptorTableShadow 都包含 2-4 个这样的描述符：
- Shadow[0] = ntoskrnl SSDT（与主表相同）
- Shadow[1] = win32k W32pServiceTable（Shadow SSDT）

#### `SYSTEM_PROCESS_INFORMATION / SYSTEM_THREAD_INFORMATION (shadow_ssdt.c:83-133)`

本地重新声明的 `ZwQuerySystemInformation` 数据结构和相关辅助类型，用于进程/线程枚举。采用 `#pragma pack(push, 1)` 确保精确布局。

### 2.5 共享数据结构（来自 shared.h）

| 结构体 | 用途 |
|--------|------|
| `SSDT_ENTRY_INFO` | 单个 Shadow SSDT 条目信息（与主 SSDT 复用） |
| `HOOK_RULE` | Hook 行为控制 |
| `VMX_SHADOW_SSDT_MONITOR_REQUEST` | Shadow SSDT 监控请求 |
| `VMX_SHADOW_SSDT_HOOK_REQUEST` | Shadow SSDT Hook 请求 |
| `VMX_SHADOW_SSDT_INIT_RESPONSE` | 初始化响应（返回 W32pServiceTableVa、服务数、win32k 基址） |

### 2.6 Shadow SSDT 相关常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `SHADOW_SSDT_MAX_SERVICES` | 2048 | Win11 约 1400，留有充足余量 |
| `SHADOW_SSDT_MONITOR_MAX_FILTER` | 64 | 过滤模式最大索引数 |
| `MAX_WIN32K_MODULES` | 4 | win32k 最大模块数 |
| `SSDT_MAX_NAME_LEN` | 128 | 函数名最大长度 |

---

## 3. 核心函数详解

### 3.0 内部辅助函数

#### `IsKernelAddress() (line 168-171)`

```c
static __inline BOOLEAN IsKernelAddress(ULONG64 Addr)
{
    return (Addr >= 0xFFFF800000000000ULL);
}
```

用于快速判断一个地址是否属于内核空间。x64 架构中内核地址的高 16 位为 0xFFFF。

#### `IsInWin32kRange() (line 544-555)`

```c
static BOOLEAN IsInWin32kRange(ULONG64 Va)
```

检查一个 VA 是否在任意已发现的 win32k 模块地址范围内。用于验证解码后的函数地址是否属于 win32k。

---

### 3.1 KTHREAD.ServiceTable 偏移动态发现

#### `KthreadResolveServiceTableOffset() (line 187-304)`

**签名**：`static NTSTATUS KthreadResolveServiceTableOffset(VOID)`

**这是整个 Shadow SSDT 发现的基础，解决了 KTHREAD 结构的版本兼容性问题。**

**背景**：KTHREAD 结构体中的 `ServiceTable` 字段在不同 Windows 版本中的偏移量不同，不能硬编码。

**算法**：
1. **获取 KeServiceDescriptorTable 参考值**：
   - 通过 `MmGetSystemRoutineAddress(L"KeServiceDescriptorTable")` 获取已知地址（该表是导出的）

2. **枚举 System 进程（PID=4）的线程 ID**：
   - 调用 `ZwQuerySystemInformation(SystemProcessInformation)` 获取全系统进程信息
   - 找到 PID=4 的进程，提取前两个线程的 TID（Tid1、Tid2）
   - System 进程的线程从不初始化 win32k，因此 `KTHREAD.ServiceTable` 始终指向 `KeServiceDescriptorTable`

3. **扫描 KTHREAD 寻找 ServiceTable 指针**：
   - 通过 `PsLookupThreadByThreadId` 获取线程 1 的 PETHREAD 指针
   - 从偏移 0 到 0x400（1024 字节），按 QWORD（8 字节）扫描
   - 寻找值等于 `KeServiceDescriptorTable` 的字段
   - 使用 `__try/__except` 保护内存访问

4. **双线程交叉验证**：
   - 对线程 2 在同一偏移处读取验证
   - 确保两个 System 线程的该字段值均为 `KeServiceDescriptorTable`

**结果**：将发现的偏移保存到 `g_ShadowSsdtState.KthreadOffsets.ServiceTableOffset`。

**设计考量**：
- 扫描范围 0x400 字节（1024 字节）覆盖了 KTHREAD 的完整大小
- 只扫描 QWORD 对齐的位置（偏移为 8 的倍数）
- 双线程验证降低误匹配风险
- System 进程（PID=4）的线程是安全的参考（永不初始化 GUI）

---

### 3.2 KeServiceDescriptorTableShadow 发现

#### `ShadowSsdtDiscover() (line 318-458)`

**签名**：`static NTSTATUS ShadowSsdtDiscover(VOID)`

**这是定位未导出符号 KeServiceDescriptorTableShadow 的核心算法。**

**算法**：
1. **获取参考值**：再次获取 `KeServiceDescriptorTable` 地址用于对比

2. **枚举所有进程和线程**：
   - 遍历系统中所有进程（跳过 PID 0 和 PID 4）
   - 对每个非系统线程，读取其 `KTHREAD.ServiceTable` 值（使用动态发现的偏移）
   - 寻找值不等于 `KeServiceDescriptorTable` 且不等于 0 且在内核地址范围内的线程
   - 这样的线程就是 GUI 线程，其 ServiceTable 指向 `KeServiceDescriptorTableShadow`

3. **三重验证**：
   - **验证 1**：`Shadow[0].Base == KeServiceDescriptorTable[0].Base`（ntoskrnl 部分必须一致）
   - **验证 2**：`Shadow[0].Limit == KeServiceDescriptorTable[0].Limit`（ntoskrnl 部分的上限一致）
   - **验证 3**：`Shadow[1].Limit` 必须在合理范围内（>0 且 `< SHADOW_SSDT_MAX_SERVICES`）

4. **保存结果**：
   - `W32pServiceTableVa = ShadowTable[1].Base`
   - `ServiceCount = ShadowTable[1].Limit`

5. **获取 GUI 进程引用**：
   - 通过 `PsLookupProcessByProcessId` 获取发现时使用的 GUI 进程的 EPROCESS
   - 保存到 `g_ShadowSsdtState.GuiProcess`（持有引用计数）
   - 后续所有 win32k 内存访问都通过 `KeStackAttachProcess` 附加到此进程

**为什么需要这个过程**：
- `KeServiceDescriptorTableShadow` 是未导出的内核符号
- 它包含两个描述符：Shadow[0]（ntoskrnl，与主表共享）和 Shadow[1]（win32k）
- 只有 GUI 线程的 KTHREAD.ServiceTable 指向 Shadow 表

---

### 3.3 Win32k 模块枚举

#### `ShadowSsdtGetWin32kModules() (line 468-538)`

**签名**：`static NTSTATUS ShadowSsdtGetWin32kModules(VOID)`

**功能**：枚举系统加载的所有名称以 "win32k" 开头的内核模块。

**算法**：
1. 调用 `ZwQuerySystemInformation(SystemModuleInformation)` 获取系统模块列表
2. 遍历所有模块，检查文件名是否以 "win32k" 开头（不区分大小写）
3. 保存模块基址、大小、短名称和完整路径

**支持 Win10+ 拆分**：
- Win7/8：只有 `win32k.sys`
- Win10+：`win32k.sys`（存根）+ `win32kbase.sys` + `win32kfull.sys`
- 三者都可能导出 NtUser/NtGdi 函数

---

### 3.4 Shadow SSDT 地址解析

#### `ShadowSsdtResolveAllAddresses() (line 567-608)`

**签名**：`static NTSTATUS ShadowSsdtResolveAllAddresses(VOID)`

**功能**：在 GUI 进程上下文内解码所有 Shadow SSDT 条目。

**关键约束**：必须在 GUI 进程上下文中执行——使用 `KeStackAttachProcess` 附加到之前发现的 GUI 进程。

**算法**：
1. 调用 `KeStackAttachProcess(g_ShadowSsdtState.GuiProcess, &ApcState)`
2. 从 `W32pServiceTableVa` 读取每个条目
3. 使用与标准 SSDT 相同的编码公式解码：`FuncVa = TableBase + (Entry >> 4)`
4. 验证解码后的地址是否在 win32k 模块范围内
5. 调用 `KeUnstackDetachProcess(&ApcState)` 恢复上下文

---

### 3.5 名称解析

#### `ShadowSsdtPopulateNamesForModule() (line 621-690)`

**签名**：`static ULONG ShadowSsdtPopulateNamesForModule(ULONG64 ModuleBase, ULONG ModuleSize)`

**功能**：遍历单个 win32k 模块的 PE 导出表，将 NtUser\*/NtGdi\* 导出函数地址与已解析的 Shadow SSDT 地址匹配。

**关键筛选**：只关心以 `"NtU"` 或 `"NtG"` 开头的导出函数，即 `NtUser*` 和 `NtGdi*` 系列。

#### `ShadowSsdtPopulateNames() (line 697-724)`

**签名**：`static NTSTATUS ShadowSsdtPopulateNames(VOID)`

**功能**：在 GUI 进程上下文中，对所有已发现的 win32k 模块调用 `ShadowSsdtPopulateNamesForModule()`，汇总匹配数。

---

### 3.6 表查询 API

#### `ShadowSsdtGetEntryInfo() (line 730-765)`

**签名**：`NTSTATUS ShadowSsdtGetEntryInfo(ULONG Index, PSSDT_ENTRY_INFO Out)`

**功能**：获取单个 Shadow SSDT 条目的完整信息。与标准 SSDT 的对应函数不同，此函数需要：
1. 附加到 GUI 进程上下文（通过 `KeStackAttachProcess`）
2. 从 `W32pServiceTableVa` 读取原始值
3. 退出 GUI 上下文

#### `ShadowSsdtDumpTable() (line 767-794)`

批量转储 Shadow SSDT 表，与标准 SSDT 对应函数逻辑相同但操作的是 Shadow 表。

#### `ShadowSsdtFindIndexByName() (line 796-819)`

**功能**：通过 NtUser/NtGdi 函数名查找 Shadow SSDT 索引。与标准 SSDT 不同的是，此函数没有 `MmGetSystemRoutineAddress` 回退机制——因为 NtUser/NtGdi 函数未从 ntoskrnl 导出，所以只在名称缓存中搜索。

---

### 3.7 Hook 操作

#### `ShadowSsdtHookByIndex() (line 849-924)`

**签名**：`NTSTATUS ShadowSsdtHookByIndex(ULONG Index, PHOOK_RULE Rule, PULONG OutHookId)`

**与标准 SSDT Hook 的关键区别**：
1. 在调用 `GenericHookInstall()` 之前，需要 `KeStackAttachProcess` 到 GUI 进程上下文
   - 这是因为 win32k 的代码页是 per-Session 映射的
   - EPT/NPT Hook 安装时需要读取目标页面的内容来分析代码结构
2. 其他逻辑（重复检查、链表管理、锁保护）与 `SsdtHookByIndex` 完全一致

#### `ShadowSsdtHookByName() (line 926-941)`

通过函数名安装 Hook，先查索引再调用 `ShadowSsdtHookByIndex()`。

---

### 3.8 监控与生命周期

#### `ShadowSsdtSetMonitorMode() (line 1041-1117)`

与 `SsdtSetMonitorMode()` 逻辑完全对应，支持 OFF / ALL / FILTERED 三种模式。

#### `ShadowSsdtStopMonitoring() (line 1119-1160)`

与 `SsdtStopMonitoring()` 逻辑完全对应，只移除 `IsMonitorHook == TRUE` 的 Hook。

#### `ShadowSsdtInitialize() (line 1166-1229)`

**签名**：`NTSTATUS ShadowSsdtInitialize(VOID)`

**初始化序列**（严格的 5 步骤顺序依赖）：

```
ShadowSsdtInitialize()
  |
  +-- 前提：g_SsdtState.Initialized 必须为 TRUE
  |    （标准 SSDT 必须先初始化）
  |
  +-> Step 1: KthreadResolveServiceTableOffset()
  |    发现 KTHREAD.ServiceTable 偏移
  |
  +-> Step 2: ShadowSsdtDiscover()
  |    找到 KeServiceDescriptorTableShadow 和 GUI 进程
  |
  +-> Step 3: ShadowSsdtGetWin32kModules()
  |    枚举 win32k 模块（非致命，失败可继续）
  |
  +-> Step 4: ShadowSsdtResolveAllAddresses()
  |    在 GUI 进程上下文中解码所有 Shadow SSDT 地址
  |
  +-> Step 5: ShadowSsdtPopulateNames()
  |    匹配 NtUser/NtGdi 名称（非致命）
```

**设计要点**：
- Shadow SSDT 依赖标准 SSDT 先初始化（因为需要 `KeServiceDescriptorTable` 作为参考值）
- Step 3 和 Step 5 是尽力而为的（失败不阻塞整体初始化）
- 如果 Step 4 失败，会释放 GUI 进程的引用并返回错误

#### `ShadowSsdtCleanup() (line 1231-1252)`

停止监控 -> 移除所有 Hook -> 释放 GUI 进程引用 -> 标记未初始化。

---

## 4. 控制流与逻辑流程

### 4.1 KTHREAD.ServiceTable 偏移发现流程

```
KthreadResolveServiceTableOffset()
  |
  +-> MmGetSystemRoutineAddress(L"KeServiceDescriptorTable") -> 参考值
  |
  +-> ZwQuerySystemInformation(SystemProcessInformation)
  |    枚举所有进程，找到 PID=4 的线程列表 -> Tid1, Tid2
  |
  +-> PsLookupThreadByThreadId(Tid1) -> Thread1
  |
  +-> for (Offset = 0; Offset < 0x400; Offset += 8)
  |    读取 *(PULONG64)((PUCHAR)Thread1 + Offset)
  |    比较 KeServiceDescriptorTable -> 找到匹配偏移
  |
  +-> 可选验证：PsLookupThreadByThreadId(Tid2) -> Thread2
      检查 *(PULONG64)((PUCHAR)Thread2 + Candidate) == KeServiceDescriptorTable
```

### 4.2 KeServiceDescriptorTableShadow 发现流程

```
ShadowSsdtDiscover()
  |
  +-> 获取 KeServiceDescriptorTable 参考值
  |
  +-> ZwQuerySystemInformation(SystemProcessInformation) -> 遍历
  |
  +-> for each process > PID 4 with threads:
  |     for each thread:
  |       读取 KTHREAD + ServiceTableOffset
  |       如果值 != KeServiceDescriptorTable && 在内核空间：
  |         -> Shadow 候选地址，记录 PID
  |
  +-> 三重验证 Shadow 表结构
  |    Shadow[0].Base == Normal[0].Base
  |    Shadow[0].Limit == Normal[0].Limit
  |    Shadow[1].Limit 在合理范围内
  |
  +-> PsLookupProcessByProcessId(GuiPid) -> 持有 EPROCESS 引用
```

### 4.3 Hook 安装流程

```
ShadowSsdtHookByIndex(Index, Rule, &HookId)
  |
  +-> 验证初始化 && 索引有效性
  +-> 自旋锁 -> 检查重复 -> 释放
  +-> 获取 ResolvedAddresses[Index]
  |
  +-> KeStackAttachProcess(GuiProcess, &ApcState)
  +-> GenericHookInstall(FuncVa, 0, Name, Rule, &HookId)
  +-> KeUnstackDetachProcess(&ApcState)
  |
  +-> 分配 SSDT_HOOK_MAPPING 节点
  +-> 自旋锁 -> 插入链表 -> 释放
```

---

## 5. 与其他模块的交互

### 5.1 与 `ssdt.c/ssdt.h` 的交互

`shadow_ssdt.c` 与 `ssdt.c` 密切相关：

1. **前提依赖**：Shadow SSDT 要求标准 SSDT 先初始化（`g_SsdtState.Initialized == TRUE`），因为发现过程中需要 `KeServiceDescriptorTable` 作为参考值
2. **结构复用**：复用 `SSDT_HOOK_MAPPING` 链表节点结构
3. **IOCTL 序列**：用户态必须先调用 `--ssdt-init` 再调用 `--shadow-ssdt-init`

### 5.2 与 `hv_hook.c/hv_hook.h` 的交互

与标准 SSDT 模块一样，所有实际的 Hook 机制全部委托给通用 Hook 框架。区别在于调用 `GenericHookInstall()` 时需要先附加到 GUI 进程上下文。

### 5.3 与 `shared.h` 的交互

Shadow SSDT 的 IOCTL 代码为 0x814-0x81A，与标准 SSDT（0x80D-0x813）相邻。定义了专用的请求/响应结构体，但底层条目信息结构体（`SSDT_ENTRY_INFO`）与标准 SSDT 共用。

### 5.4 与 `log.h` 的交互

使用四级日志系统，关键诊断信息包括：
- 发现的 KTHREAD.ServiceTable 偏移
- 发现的 KeServiceDescriptorTableShadow 地址
- GUI 进程 PID
- win32k 模块列表
- 名称匹配统计

---

## 6. 关键设计要点

### 6.1 KeServiceDescriptorTableShadow 的发现策略

这是本项目最精巧的部分之一。因为 `KeServiceDescriptorTableShadow` 是未导出符号，无法通过 `MmGetSystemRoutineAddress` 获得。发现策略利用了 Windows 内核的关键属性：

1. **KTHREAD.ServiceTable**：每个线程的 KTHREAD 结构中有一个字段指向当前线程使用的系统服务描述表
2. **System 线程 vs GUI 线程**：
   - System（PID=4）的线程不加载 win32k -> ServiceTable 指向 `KeServiceDescriptorTable`
   - GUI 线程（如 explorer.exe）加载了 win32k -> ServiceTable 指向 `KeServiceDescriptorTableShadow`
3. **Shadow 表的结构**：`KeServiceDescriptorTableShadow[0]` 与 `KeServiceDescriptorTable[0]` 相同（ntoskrnl SSDT），`Shadow[1]` 是 win32k 表

### 6.2 win32k 会话隔离的处理

win32k 的一大特性是 **per-Session 映射**：
- 不同终端服务会话中的 win32k 代码映射到不同的物理页面
- 函数地址（VA）是相同的，但物理页帧不同
- 这意味着在非 GUI 进程上下文中读取 W32pServiceTable 的内容是不安全的

解决方案：`KeStackAttachProcess` / `KeUnstackDetachProcess`
- 所有对 win32k 内存的读取（包括 Shadow SSDT 条目和 PE 导出表）都在发现的 GUI 进程上下文中执行
- 在调用 `GenericHookInstall()` 之前也需要附加，因为 EPT Hook 需要检查目标页面的当前内容
- 这些函数来自 `ntifs.h`，在 shadow_ssdt.c 中手动声明了 `KAPC_STATE` 结构

### 6.3 名称识别的二字符前缀

名称解析只识别两种前缀：
- **NtUser***：例如 `NtUserGetMessage`、`NtUserSendInput`
- **NtGdi***：例如 `NtGdiBitBlt`、`NtGdiTextOut`

这是 win32k 系统调用的命名惯例。标准 SSDT 的 Nt\* 函数（如 NtCreateFile、NtOpenProcess）不会出现在 Shadow SSDT 中。这种方法防止了名称识别冲突。

### 6.4 与 SSDT 模块的对称设计

`shadow_ssdt.c` 的设计刻意与 `ssdt.c` 保持高度对称：

| 功能 | SSDT 函数 | Shadow SSDT 函数 |
|------|-----------|-----------------|
| 初始化 | `SsdtInitialize()` | `ShadowSsdtInitialize()` |
| 销毁 | `SsdtCleanup()` | `ShadowSsdtCleanup()` |
| 单条目查询 | `SsdtGetEntryInfo()` | `ShadowSsdtGetEntryInfo()` |
| 批量转储 | `SsdtDumpTable()` | `ShadowSsdtDumpTable()` |
| 按索引 Hook | `SsdtHookByIndex()` | `ShadowSsdtHookByIndex()` |
| 按名称 Hook | `SsdtHookByName()` | `ShadowSsdtHookByName()` |
| 按索引解除 | `SsdtUnhookByIndex()` | `ShadowSsdtUnhookByIndex()` |
| 全部解除 | `SsdtUnhookAll()` | `ShadowSsdtUnhookAll()` |
| 设置监控 | `SsdtSetMonitorMode()` | `ShadowSsdtSetMonitorMode()` |
| 停止监控 | `SsdtStopMonitoring()` | `ShadowSsdtStopMonitoring()` |

这种对称性降低了维护成本，使用户态 CLI 处理 Shadow SSDT 命令的逻辑与标准 SSDT 几乎相同。

### 6.5 地址解析的容错性

`ShadowSsdtResolveAllAddresses()` 在地址验证上的宽松策略：
- 检查解码后的地址是否在 win32k 地址范围内的代码（`IsInWin32kRange()`）会报告结果，但即使函数地址不在已知模块范围内，解析仍然继续进行并存储该地址
- 代码注释解释道："Still store it; may be in a module we didn't detect"
- 这种容错设计考虑了极端情况——未检测到的第三方 win32k 扩展模块或特殊构建

### 6.6 Win10+ 三模块拆分支持

Windows 10 将 win32k 拆分为三个模块：
- **win32k.sys**：存根模块（瘦层）
- **win32kbase.sys**：基础功能（字体、文本渲染等）
- **win32kfull.sys**：完整功能（窗口管理、输入处理等）

Shadow SSDT 的系统调用可能分布在三个模块的任何位置。名称解析代码通过枚举所有以 "win32k" 开头的模块并使用 `ShadowSsdtPopulateNamesForModule()` 依次遍历每个模块的导出表来解决这个问题。
