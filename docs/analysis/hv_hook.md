# hv_hook.c / hv_hook.h — 逻辑分析

## 1. 文件概述

### 角色与职责

`hv_hook.c` 和 `hv_hook.h` 实现了一个**通用EPT/NPT不可见函数钩子框架**，是整个VMXToolbox超管系统中最为核心的能力层之一。它提供了一套与硬件平台（Intel VMX / AMD SVM）无关的、动态的、用户可控的函数拦截机制。

核心能力：
- 通过EPT（Extended Page Tables）/ NPT（Nested Page Tables）二级页表分裂技术，在**不修改Guest内核代码页**的前提下实现对任意函数地址的拦截
- 由于不对代码页做任何写操作，因此可以完全绕过 PatchGuard（内核代码完整性检查）
- 支持无限数量的钩子（通过动态Thunk分配，每4KB页可容纳170个Thunk桩）
- 支持四种拦截动作：放行（Passthrough）、仅记录（Log-only）、拦截（Block）、修改返回值（Modify-retval）

### 依赖的其他模块

| 模块 | 文件 | 依赖关系 |
|------|------|----------|
| 抽象层 | `hv_ops.h` | 通过 `HvHookFunction()` / `HvUnhookFunction()` 宏调用底层EPT/NPT操作 |
| VMX定义 | `vmx.h` | 使用 `VMX_TAG` 等常量 |
| 日志 | `log.h` | 使用 `LOG_INFO` / `LOG_WARN` / `LOG_ERROR` 宏 |
| 共享定义 | `common/shared.h` | 使用 `HOOK_RULE`、`HOOK_EVENT`、`HOOK_ACTION_*` 等结构体 |
| 汇编调度器 | `hv_hook_asm.asm` | `AsmGenericHookDispatcher` — ASM入口点 |

---

## 2. 数据结构

### 2.1 Thunk桩布局 (`hv_hook.h`)

```c
#define THUNK_STUB_SIZE    24   // 每个桩24字节
#define THUNKS_PER_PAGE    170  // 每4KB页容纳170个桩
#define THUNK_BITMAP_WORDS 3    // (170+63)/64 = 3个64位字用于位图
```

每个桩的结构（24字节）：

| 偏移 | 字节码 | 含义 |
|------|--------|------|
| +0 | `49 BA [8字节]` | `mov r10, <hook_id>` — 将Hook ID载入R10（10字节） |
| +10 | `FF 25 00 00 00 00` | `jmp [rip+0]` — 间接跳转到后面8字节指向的地址（6字节） |
| +16 | `[8字节地址]` | 调度器的绝对地址（8字节） |

**设计要点**：R10在Windows x64 ABI中被定义为易失寄存器，不用于参数传递，因此劫持R10是安全的。RCX/RDX/R8/R9（前4个参数）和栈参数保持完整，原始函数调用不受影响。

### 2.2 THUNK_PAGE — Thunk页结构

```c
typedef struct _THUNK_PAGE {
    struct _THUNK_PAGE *Next;          // 链表指针
    PVOID               CodeBase;      // 4KB可执行页
    ULONG               Capacity;      // 固定为 THUNKS_PER_PAGE (170)
    ULONG               UsedCount;     // 已用槽数
    ULONG               BaseId;        // 本页分配的第一个Hook ID（调试辅助）
    ULONG64             SlotBitmap[3]; // H-3: 槽位分配位图（1=已用）
} THUNK_PAGE, *PTHUNK_PAGE;
```

**H-3优化（桶复用）**：框架初始版本是严格追加分配的（`UsedCount` 单调递增），从不回收已释放的槽位，导致频繁的Hook/Unhook操作（如SSDT监控开关）会持续泄漏Thunk页。H-3修复引入 `SlotBitmap` 位图，释放的槽位可被后续 `AllocateThunk` 重新使用。

### 2.3 HOOK_DECISION — 决策结构（ASM/C共享）

```c
typedef struct _HOOK_DECISION {
    ULONG       Action;             // +0x00: HOOK_ACTION_*
    ULONG       Pad0;               // +0x04: 对齐填充
    ULONG64     BlockReturnValue;   // +0x08: BLOCK动作时的返回值
    ULONG64     NewReturnValue;     // +0x10: MODIFY_RETVAL动作时的新返回值
    PVOID       Trampoline;         // +0x18: 蹦床函数地址（调用原始函数）
    BOOLEAN     ShouldLog;          // +0x20: 是否需要记录日志
    UCHAR       Pad1[7];           // +0x21: 对齐填充
} HOOK_DECISION;                   // 总大小: 0x28 = 40字节
```

**结构对齐至关重要**：此结构在 `GenericHookDecide()`（C代码）中填充，由 `AsmGenericHookDispatcher`（汇编代码）读取。偏移量必须完全一致，任何不匹配都导致调度器行为异常。

### 2.4 GENERIC_HOOK_ENTRY — 钩子条目

```c
typedef struct _GENERIC_HOOK_ENTRY {
    struct _GENERIC_HOOK_ENTRY *Next;   // 链表指针
    BOOLEAN     Active;
    ULONG       HookId;                // 唯一ID，单调递增
    ULONG64     TargetVirtualAddress;   // 目标函数VA
    ULONG       ProcessId;             // 0=全局/内核
    WCHAR       FunctionName[128];     // 函数名
    HOOK_RULE   Rule;                  // 行为规则
    PVOID       Trampoline;            // EPT/NPT蹦床（调用原始函数）
    PVOID       ThunkAddress;          // Thunk地址（EPT钩子的跳转目标）
    volatile LONG64 HitCount;          // 命中次数统计
} GENERIC_HOOK_ENTRY;
```

### 2.5 GENERIC_HOOK_STATE — 全局状态

```c
typedef struct _GENERIC_HOOK_STATE {
    PGENERIC_HOOK_ENTRY HookListHead;      // 钩子条目链表头
    ULONG               HookCount;
    ULONG               NextHookId;        // 自增ID
    KSPIN_LOCK          Lock;              // 保护链表和Thunk位图的锁
    PTHUNK_PAGE         ThunkPageHead;     // Thunk页链表头
    ULONG               ThunkPageCount;
    HOOK_EVENT          EventRing[512];    // 事件环形缓冲
    volatile LONG       EventWriteIndex;
    volatile LONG       EventReadIndex;
    volatile LONG       EventCount;
    KSPIN_LOCK          EventLock;         // 事件缓冲的锁
    BOOLEAN             Initialized;
} GENERIC_HOOK_STATE;
```

### 2.6 HOOK_RULE — 钩子规则（来自 `shared.h`）

```c
typedef struct _HOOK_RULE {
    ULONG       Action;             // HOOK_ACTION_*
    ULONG       TargetPid;          // 0=全局，>0=特定进程
    ULONG64     BlockReturnValue;   // BLOCK时返回的值
    ULONG64     NewReturnValue;     // MODIFY_RETVAL时覆盖的值
    BOOLEAN     LogEnabled;         // 是否记录事件日志
} HOOK_RULE;
```

四种动作类型：
| 宏 | 值 | 行为 |
|----|-----|------|
| `HOOK_ACTION_PASSTHROUGH` | 0 | 执行原始函数，仅计数 |
| `HOOK_ACTION_LOG_ONLY` | 1 | 执行原始函数，记录每次调用 |
| `HOOK_ACTION_BLOCK` | 2 | 跳过原始函数，返回 `BlockReturnValue` |
| `HOOK_ACTION_MODIFY_RETVAL` | 3 | 执行原始函数，用 `NewReturnValue` 覆盖返回值 |

---

## 3. 核心函数详解

### 3.1 `BuildThunkStub` — 构建Thunk桩

```c
static VOID BuildThunkStub(PUCHAR Base, ULONG HookId, ULONG64 DispatcherAddr)
```

- **功能**：在给定的内存基址处写入24字节的Thunk桩代码
- **参数**：
  - `Base`：目标内存地址（在可执行Thunk页内）
  - `HookId`：钩子唯一标识符，写入 `mov r10, HookId`
  - `DispatcherAddr`：汇编调度器 `AsmGenericHookDispatcher` 的地址
- **逻辑**：写入 `49 BA <8字节ID>` + `FF 25 00 00 00 00` + `<8字节调度器地址>`

### 3.2 `AllocateThunkPage` / `FreeThunkPage` — Thunk页生命周期

```c
static PTHUNK_PAGE AllocateThunkPage(ULONG BaseId)
static VOID FreeThunkPage(PTHUNK_PAGE Page)
```

- `AllocateThunkPage`：分配 `THUNK_PAGE` 结构体 + 一个4KB NonPagedPool可执行页
- `FreeThunkPage`：释放代码页和结构体
- **注意**：`NonPagedPool` 在WDK 7600目标上默认是可执行的

### 3.3 `AllocateThunk` — 分配Thunk槽（核心分配函数）

```c
static PVOID AllocateThunk(ULONG HookId)
```

- **功能**：在已有的Thunk页链表中查找空闲槽位（H-3：通过 `SlotBitmap` 位图），若无则分配新页
- **返回值**：Thunk桩的虚拟地址（用于EPT映射）
- **核心流程**：
  1. 遍历 `g_GenericHookState.ThunkPageHead` 链表
  2. 对每个页，扫描 `SlotBitmap[3]` 找到第一个0位
  3. 若找到空闲槽：标记位图，递增 `UsedCount`，调用 `BuildThunkStub` 写入桩代码
  4. 若 `UsedCount` 与实际位图不一致（数据竞争留痕），修复 `UsedCount = Capacity`
  5. 若所有页满：分配新页，头插法入链表，使用槽0
- **设计要点**：`UsedCount` 是不精确的（可能在竞争条件下滞后），因此查找始终扫描真实位图

### 3.4 `FreeThunk` — 释放Thunk槽

```c
static VOID FreeThunk(PVOID ThunkAddr)
```

- **功能**：释放之前由 `AllocateThunk` 分配的槽位，使其可被重用
- **安全性分析**（代码注释中的详细论述）：
  - 调用此函数前，`HvUnhookFunction` 已重建EPT/NPT映射并刷新所有CPU的TLB
  - 因此不会有新的Guest VA通过此Thunk分发
  - 正在执行 `AsmGenericHookDispatcher` 的CPU不受影响（调度器在单独的代码页中，且不保留指向Thunk的栈帧指针）
  - x86 icache一致性保证：修改最近执行过的可写内存会自动自修改icache行
  - 零风险槽重用：TLB同步完成后槽才被标记为空闲
- **执行顺序**：先 `RtlZeroMemory`（清空桩字节），后清除位图位（在锁保护下原子操作）
- **内存策略**：即使 `UsedCount` 降到0，也**不释放Thunk页**（减少重分配开销，简化生命周期分析）

### 3.5 `FindHookById` / `FindHookByAddress` — 查找函数

```c
static PGENERIC_HOOK_ENTRY FindHookById(ULONG HookId)
static PGENERIC_HOOK_ENTRY FindHookByAddress(ULONG64 TargetVa)
```

- 线性搜索链表，只返回 `Active == TRUE` 的条目
- `FindHookById` 用于调试/移除操作
- `FindHookByAddress` 用于重复安装检测

### 3.6 `GenericHookInit` / `GenericHookCleanup` — 初始化/清理

```c
NTSTATUS GenericHookInit(VOID)
VOID GenericHookCleanup(VOID)
```

- `GenericHookInit`：清零全局状态，初始化两个自旋锁（`Lock` 和 `EventLock`），`NextHookId` 从1开始
- `GenericHookCleanup`：调用 `GenericHookRemoveAll()` 卸载所有钩子，释放所有Hook条目和Thunk页

### 3.7 `GenericHookInstall` — 安装钩子（核心入口）

```c
NTSTATUS GenericHookInstall(
    ULONG64     TargetVa,
    ULONG       ProcessId,
    const WCHAR *FunctionName,
    PHOOK_RULE  Rule,
    PULONG      OutHookId
)
```

**功能**：在指定虚拟地址安装EPT/NPT不可见钩子。

**核心流程**：

1. **重复检查**：调用 `FindHookByAddress(TargetVa)`，若已存在则返回 `STATUS_ALREADY_REGISTERED`
2. **分配ID**：`NextHookId++`，保证单调递增
3. **分配Thunk**：`AllocateThunk(HookId)`，获取Thunk桩地址
4. **用户态VA处理（L-5修复）**：
   - 判断标准：`TargetVa < 0x0000800000000000` 即x64规范地址下半部分
   - 若为用户态VA（如 `ntdll!Nt*` 转发的目标），调用 `PsLookupProcessByProcessId` 查找目标进程
   - 调用 `KeStackAttachProcess` 附加到目标进程上下文（使 `MmGetPhysicalAddress` 返回正确的物理页）
   - 否则内核态VA无需进程上下文
5. **安装EPT/NPT钩子**：`HvHookFunction(TargetVa, ThunkAddr, &Trampoline)` — 底层执行页分裂
6. **分配Hook条目**：`ExAllocatePoolWithTag(NonPagedPool, sizeof(GENERIC_HOOK_ENTRY))`
7. **填充条目**：激活状态、ID、目标地址、蹦床地址、Thunk地址、规则、函数名
8. **插入链表**：头插法加到 `HookListHead`
9. **返回Hook ID**：通过 `OutHookId` 输出

**错误处理**：
- Thunk分配失败 → `STATUS_INSUFFICIENT_RESOURCES`
- 用户态VA但 `ProcessId=0` → `STATUS_INVALID_PARAMETER`
- `PsLookupProcessByProcessId` 失败 → 返回相应NTSTATUS
- `HvHookFunction` 失败 → 返回错误码
- Hook条目内存分配失败 → 调用 `HvUnhookFunction` 回滚EPT/NPT映射

### 3.8 `GenericHookRemove` / `GenericHookRemoveAll` — 移除钩子

```c
NTSTATUS GenericHookRemove(ULONG HookId)
VOID GenericHookRemoveAll(VOID)
```

- `GenericHookRemove(HookId)`：
  1. 遍历链表找到匹配ID的条目
  2. 调用 `HvUnhookFunction` 恢复EPT/NPT原始映射（含跨CPU TLB刷新）
  3. 从链表中摘除条目
  4. 调用 `FreeThunk` 回收Thunk槽
  5. 释放Hook条目内存
- `GenericHookRemoveAll()`：遍历链表，逐个卸载

### 3.9 `GenericHookGetInfo` / `GenericHookGetCount` — 查询

```c
NTSTATUS GenericHookGetInfo(ULONG HookId, PVMX_HOOK_INFO OutInfo)
ULONG GenericHookGetCount(VOID)
```

- `GenericHookGetInfo`：按ID查询钩子详细信息（地址、PID、规则、命中计数等）
- `GenericHookGetCount`：返回当前活跃钩子总数

### 3.10 `GenericHookDecide` — 决策函数（C语言部分）

```c
VOID NTAPI GenericHookDecide(
    ULONG64         HookIndex,
    ULONG64         CallerRetAddr,
    PHOOK_DECISION  OutDecision
)
```

**调用者**：由 `AsmGenericHookDispatcher`（汇编入口）调用。

**核心流程**：
1. `FindHookById(HookIndex)` — 通过R10中的Hook ID查找条目
2. **守卫检查**：若条目不存在/非活跃/无蹦床，返回 `HOOK_ACTION_PASSTHROUGH`
3. **PID过滤器**：若 `Rule.TargetPid != 0`，比较当前进程PID；不匹配则放行
4. **更新命中计数**：`InterlockedIncrement64(&Entry->HitCount)` — 原子操作保证线程安全
5. **填充决策**：根据 `Entry->Rule.Action` 返回对应动作
6. **日志判定**：`ShouldLog` 在 `LOG_ONLY` 动作或 `LogEnabled` 时置为TRUE

**注意**：此函数可能在任何IRQL下被调用（取决于Hook目标函数的调用点），因此必须使用自旋锁保护内部数据。

### 3.11 `GenericHookPostCall` — 后调用处理

```c
VOID NTAPI GenericHookPostCall(
    ULONG64     HookIndex,
    ULONG       Action,
    ULONG64     FinalRetVal,
    ULONG64     CallerRetAddr,
    ULONG64     ShouldLog
)
```

- 在汇编调度器执行完蹦床（原始函数）后调用
- 若 `ShouldLog` 为TRUE，调用 `HookLogEvent` 记录事件

### 3.12 事件日志系统

```c
VOID HookLogEvent(ULONG HookId, ULONG Pid, ULONG64 CallerAddr,
                   ULONG64 FinalRetVal, ULONG ActionTaken)
ULONG HookLogRead(PHOOK_EVENT OutputBuffer, ULONG MaxEntries)
```

- **环形缓冲区**：512个 `HOOK_EVENT` 条目
- `HookLogEvent`：获取系统时间，在 `EventLock` 保护下写入，自动处理翻转和覆盖
- `HookLogRead`：线性读取，支持批量消费
- **消费者**：用户态通过 `IOCTL_VMX_GET_HOOK_EVENTS` 获取事件日志

---

## 4. 控制流与逻辑流程

### 4.1 完整的函数拦截路径

```
Guest代码执行 → 目标函数地址
    ↓ (EPT/NPT缺页)
VM-Exit (EPT Violation) → 二级页表截获
    ↓
EPT/NPT处理程序 (ept.c/npt.c)：
    - 检查GPA是否映射到Thunk页
    - 是 → 设置新RIP为Thunk地址
    - 否 → 正常处理EPT缺页
    ↓
AsmGenericHookDispatcher (hv_hook_asm.asm)：
    - 保存Guest上下文 (pushad风格)
    - mov r10 中的Hook ID → RCX (第一个参数)
    - 调用 GenericHookDecide() → 获取 HOOK_DECISION
    ↓
GenericHookDecide 决策：
    ├── PASSTHROUGH → 跳转到Trampoline → 原始函数
    ├── LOG_ONLY    → 跳转到Trampoline → 原始函数 → 记录日志
    ├── BLOCK       → 返回 BlockReturnValue (不执行原始函数)
    └── MODIFY_RETVAL → 跳转到Trampoline → 原始函数 → 覆盖返回值
    ↓
GenericHookPostCall → 记录事件（可选）
    ↓
恢复Guest上下文 → VMRESUME → Guest继续执行
```

### 4.2 安装流程

```
GenericHookInstall()
    ├── [检查] FindHookByAddress() → 重复则返回 STATUS_ALREADY_REGISTERED
    ├── [分配] NextHookId++ → HookId
    ├── [分配] AllocateThunk(HookId) → ThunkAddr
    ├── [判断] TargetVa < 0x0000800000000000?
    │   ├── 是：KeStackAttachProcess(目标进程)
    │   └── 否：直接进行
    ├── [安装] HvHookFunction(TargetVa, ThunkAddr, &Trampoline)
    │   ├── 失败：回滚Thunk，返回错误
    │   └── 成功：得到Trampoline地址
    ├── [分配] ExAllocatePool(GENERIC_HOOK_ENTRY)
    │   └── 失败：HvUnhookFunction(TargetVa)，返回错误
    ├── [填充] 设置Hook条目所有字段
    ├── [链接] 头插法加入 HookListHead
    └── [返回] *OutHookId = HookId
```

### 4.3 卸载流程

```
GenericHookRemove(HookId)
    ├── [查找] 遍历链表找到挂载条目
    ├── [卸载] HvUnhookFunction(TargetVa) → TJLB刷新
    ├── [摘除] 从链表移除
    ├── [回收] FreeThunk(ThunkAddr) → 位图清零
    └── [释放] ExFreePool(Entry)
```

### 4.4 错误处理路径

| 条件 | 处理方式 |
|------|----------|
| Thunk页分配失败 | 返回 `STATUS_INSUFFICIENT_RESOURCES` |
| 重复安装（相同VA） | 返回 `STATUS_ALREADY_REGISTERED` |
| 用户态VA但未指定PID | 回滚Thunk分配，返回 `STATUS_INVALID_PARAMETER` |
| 进程查找失败 | 返回 `PsLookupProcessByProcessId` 的错误码 |
| EPT钩子安装失败 | 回滚并返回错误码 |
| Hook条目内存不足 | 卸载EPT钩子，返回 `STATUS_INSUFFICIENT_RESOURCES` |
| 移除不存在的Hook ID | 返回 `STATUS_NOT_FOUND` |
| 查询不存在的Hook ID | 返回 `STATUS_NOT_FOUND` |

---

## 5. 与其他模块的交互

### 5.1 通过 hv_ops vtable 的调用关系

```
hv_hook.c                     hv_ops.h 宏               ept.c / npt.c
──────────                    ──────────                ──────────────
GenericHookInstall ────────→  HvHookFunction() ──────→  EptHookFunction()
                    ────────→  g_HvOps->HookFunction()   NptHookFunction()

GenericHookRemove ─────────→  HvUnhookFunction() ────→  EptUnhookFunction()
                                                          NptUnhookFunction()
```

`hv_hook.c` 不直接处理EPT/NPT页表分裂的细节，而是通过 `hv_ops.h` 中定义的宏 `HvHookFunction(t,h,o)` 进行抽象调用。底层实现由 `g_HvOps->HookFunction` 函数指针决定，该指针在驱动初始化时根据CPU厂商（Intel/AMD）注册为 `EptHookFunction` 或 `NptHookFunction`。

### 5.2 与汇编代码的接口

```
hv_hook.h 声明:           hv_hook_asm.asm 实现:
──────                   ────────────────
AsmGenericHookDispatcher → 汇编入口点
                            内部调用 GenericHookDecide (C)
                            内部调用 GenericHookPostCall (C)
```

`HOOK_DECISION` 结构体是ASM与C之间的共享契约，偏移量必须精确匹配。

### 5.3 与日志模块的交互

调用 `log.h` 的 `LOG_INFO` / `LOG_WARN` / `LOG_ERROR` 宏输出诊断信息。事件日志通过自身的环形缓冲区独立管理。

### 5.4 与共享定义的交互

- 使用 `shared.h` 中的 `HOOK_RULE`、`HOOK_EVENT`、`VMX_HOOK_INFO` 等结构体
- 通过 `IOCTL_VMX_INSTALL_HOOK` 等IOCTL代码处理用户态请求

---

## 6. 关键设计要点

### 6.1 EPT/NPT页分裂不可见性

通过EPT/NPT二级页表分裂实现函数钩子，Guest操作系统的代码页从未被修改。这意味着：
- **PatchGuard免疫**：Windows内核的代码完整性检查（PatchGuard）扫描代码页的哈希，由于代码页没有实际写入，PatchGuard检测不到
- **Anti-cheat免疫**：大多数反作弊系统依赖于检测代码页修改（inline hook、hotpatch等），EPT钩子完全绕过这些检测
- **CPU缓存一致性自动维护**：x86硬件保证可写内存的自修改代码icache一致性

### 6.2 动态Thunk分配与H-3桶复用

- 初始设计使用严格追加分配（类似表扩展），频繁的Hook/Unhook导致Thunk页泄漏
- H-3修复引入 `SlotBitmap` 位图，实现O(170)时间复杂度的槽位查找和O(1)的槽位释放
- 位图每页仅占用24字节（3个64位字），空间开销极小
- `UsedCount` 不作为分配依据（可能因竞争而不精确），仅用作统计和修复标记

### 6.3 L-5修复：用户态VA的正确处理

- **问题**：用户态虚拟地址在不同进程上下文中映射到不同的物理页。若IOCTL调用者不是目标进程，`MmGetPhysicalAddress`（在EPT安装函数内部调用）会返回错误的物理页
- **解决方案**：在调用 `HvHookFunction` 前，使用 `KeStackAttachProcess` 切换到目标进程上下文
- **启发式判断**：`TargetVa < 0x0000800000000000` 为用户态VA（x64规范地址下半部分）

### 6.4 HOOK_DECISION 共享内存布局约束

`HOOK_DECISION` 结构体由C代码填充，由汇编代码读取。其精确的字段偏移和填充约束是保证调度器正确工作的关键：
- 字段顺序与对齐必须与汇编代码中的偏移引用一致
- `Pad0` 和 `Pad1[7]` 确保64位整数字段自然对齐
- 总大小为40字节（0x28），设计为适合于缓存行

### 6.5 自旋锁分拆

框架使用两把自旋锁：
- `Lock`：保护Hook链表和Thunk位图
- `EventLock`：保护事件环形缓冲区

**设计原因**：事件日志记录发生在性能关键路径上（每次Hook命中都可能记录），与Hook链表操作隔离避免了锁竞争。

### 6.6 安全相关考虑

- **PID过滤器**：允许将Hook的作用范围限制在指定进程，避免对非目标进程造成影响
- **Thunk释放时序**：详细的注释分析了Thunk释放的安全性（TLB同步后、icache一致性、调度器执行路径）
- **池标签**：使用 `VMX_TAG`（'xmvD'）追踪内存分配，便于调试内存泄漏
- **位图竞争修复**：`UsedCount` 修复逻辑处理了数据竞争导致的位图/计数不一致问题
