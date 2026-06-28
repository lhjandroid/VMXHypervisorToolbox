# anti_anti_debug.c / anti_anti_debug.h — 逻辑分析

## 1. 文件概述

### 角色与职责

`anti_anti_debug.c` 和 `anti_anti_debug.h` 实现了VMXToolbox超管系统的**反反调试引擎**。该引擎通过多种技术手段隐藏调试器在目标进程面前的存在，使得被保护的进程无法通过常见的反调试技术检测到调试器。

与传统的用户态反反调试（如 `ScyllaHide`、`TitanHide`）不同，本引擎运行在**Ring -1（超管模式）**，通过EPT/NPT不可见钩子和VM-Exit事件拦截来实现反反调试，具有更高级别的不可见性。

### 10种反反调试技术

| 位掩码标志 | 技术 | 实现方式 |
|-----------|------|----------|
| `AAD_HIDE_DEBUGGER` (bit 0) | 隐藏调试器存在 | EPT钩子 `NtQueryInformationProcess`，篡改 `ProcessDebugPort`、`ProcessDebugObjectHandle`、`ProcessDebugFlags` |
| `AAD_HIDE_HWBP` (bit 1) | 隐藏硬件断点 | MOV-DR VM-Exit处理，伪造DR0-DR7返回值 |
| `AAD_HIDE_TIMING` (bit 2) | TSC时间补偿 | 硬件TSC偏移（VMCS/VMCB TSC Offset字段），累加调试暂停期间的时间增量 |
| `AAD_HIDE_CPUID` (bit 3) | 隐藏CPUID中的超管信息 | CPUID VM-Exit处理，篡改超管相关叶子节点 |
| `AAD_HIDE_SYSINFO` (bit 4) | 隐藏系统调试信息 | EPT钩子 `NtQuerySystemInformation`，伪造 `SystemKernelDebuggerInformation` |
| `AAD_HIDE_EXCEPTIONS` (bit 5) | 异常行为规范化 | #DB和#BP异常拦截，重新注入到Guest SEH处理 |
| `AAD_HIDE_NTCLOSE` (bit 6) | 抑制NtClose异常 | EPT钩子 `NtClose`，用SEH包装调用抑制异常 |
| `AAD_HIDE_THREADINFO` (bit 7) | 阻止线程隐藏 | EPT钩子 `NtSetInformationThread`，拦截 `ThreadHideFromDebugger` |
| `AAD_HIDE_HEAP` (bit 8) | 堆标志隐藏 | 标志已定义，当前源代码中无实现（可能在 `process.c` 或其他模块） |
| `AAD_HIDE_PARENT` (bit 9) | 父进程伪造 | 标志已定义，当前源代码中无实现（可能在 `process.c` 或其他模块） |

### 依赖的其他模块

| 模块 | 文件 | 依赖关系 |
|------|------|----------|
| EPT底层 | `ept.h` | EPT钩子安装/卸载 |
| 超管抽象层 | `hv_ops.h` | 通过宏调用 `HvHookFunction()`、`HvUnhookFunction()`、`HvReadGuestCr3()`、`HvWriteTscOffset()` 等 |
| CPU检测 | `hv_detect.h` | CPU厂商检测 |
| 进程追踪 | `process.h` | `ProcessFindByCr3()`、`IsFeatureEnabled()` |
| 日志 | `log.h` | `LOG_INFO`、`LOG_WARN`、`LOG_ERROR`、`LOG_DEBUG`、`LOG_DEBUG_PID` |
| 共享定义 | `common/shared.h` | `AAD_HIDE_*` 标志位宏定义 |
| VMCS/VMCB | `vmx.h` | `GUEST_CONTEXT`、`DR_ACCESS_*` 常量、`CPUID_BACKDOOR_*`、`INTERRUPT_INFO_*` |

---

## 2. 数据结构

### 2.1 AAD_STATE — 全局反反调试状态

```c
typedef struct _AAD_STATE {
    BOOLEAN Initialized;

    /* 原始函数指针（来自EPT钩子的蹦床） */
    PFN_NtQueryInformationProcess   OrigNtQueryInformationProcess;
    PFN_NtQuerySystemInformation    OrigNtQuerySystemInformation;
    PFN_NtSetInformationThread      OrigNtSetInformationThread;
    PFN_NtClose                     OrigNtClose;

    /* 已解析的内核函数地址 */
    ULONG64     NtQueryInformationProcessAddr;
    ULONG64     NtQuerySystemInformationAddr;
    ULONG64     NtSetInformationThreadAddr;
    ULONG64     NtCloseAddr;
} AAD_STATE;
```

**关键设计**：
- 存储4个EPT钩子函数的原始（蹦床）指针，用于调用原始内核API
- 存储4个内核函数的解析地址，用于EPT钩子的定位和卸载
- 所有函数通过 `MmGetSystemRoutineAddress`（`ResolveKernelExport`）动态解析

### 2.2 反反调试标志位（来自 `shared.h`）

```c
#define AAD_HIDE_DEBUGGER       (1 << 0)    // 隐藏PEB.BeingDebugged, NtQueryInformationProcess
#define AAD_HIDE_HWBP           (1 << 1)    // DR0-DR7寄存器隐藏
#define AAD_HIDE_TIMING         (1 << 2)    // RDTSC/RDTSCP偏移补偿
#define AAD_HIDE_CPUID          (1 << 3)    // 从CPUID隐藏超管
#define AAD_HIDE_SYSINFO        (1 << 4)    // NtQuerySystemInformation伪造
#define AAD_HIDE_EXCEPTIONS     (1 << 5)    // INT 2D / INT 3行为规范化
#define AAD_HIDE_NTCLOSE        (1 << 6)    // NtClose无效句柄异常抑制
#define AAD_HIDE_THREADINFO     (1 << 7)    // NtSetInformationThread HideFromDebugger
#define AAD_HIDE_HEAP           (1 << 8)    // 堆标志隐藏
#define AAD_HIDE_PARENT         (1 << 9)    // 父进程伪造
#define AAD_HIDE_ALL            (0xFFFFFFFF)
```

### 2.3 调试寄存器伪造常量

```c
#define DR7_DEFAULT_VALUE       0x400ULL     // DR7默认值：无断点启用
#define DR6_DEFAULT_VALUE       0xFFFF0FF0ULL // DR6默认值：干净的调试状态
```

- `DR7 = 0x400` 对应标准的"无断点活动"状态（只有 `L0` 位可能已设置，但调试相关的GD（General Detect）、GE（Global Exact）、LE（Local Exact）位均为0）
- `DR6 = 0xFFFF0FF0` 表示没有挂起的调试异常（所有标志位被清除）

### 2.4 NT API函数指针类型

```c
typedef NTSTATUS (*PFN_NtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS (*PFN_NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS (*PFN_NtSetInformationThread)(HANDLE, ULONG, PVOID, ULONG);
typedef NTSTATUS (*PFN_NtClose)(HANDLE);
```

### 2.5 调试寄存器访问退出条件码（来自 `vmx.h`）

```c
#define DR_ACCESS_REG_MASK                  0x07     // 位2:0 — DR编号
#define DR_ACCESS_DIRECTION_BIT             4        // 位4: 0=写DR, 1=读DR
#define DR_ACCESS_DIRECTION_WRITE           0
#define DR_ACCESS_DIRECTION_READ            1
#define DR_ACCESS_GP_REG_SHIFT              8        // 位11:8 — 通用寄存器编号
#define DR_ACCESS_GP_REG_MASK               0x0F
```

### 2.6 目标进程结构（来自 `process.h`）

```c
typedef struct _TARGET_PROCESS {
    ULONG64     Cr3;            // DirectoryTableBase, 用于CR3匹配
    ULONG       Pid;            // 进程ID
    ULONG       Flags;          // AAD_HIDE_* 位掩码
    BOOLEAN     Active;         // 槽位是否在使用中
} TARGET_PROCESS;
```

---

## 3. 核心函数详解

### 3.1 内核地址解析

#### `ResolveKernelExport`

```c
static ULONG64 ResolveKernelExport(const WCHAR *FunctionName)
```

- **功能**：通过函数名解析ntoskrnl.exe中的导出地址
- **实现**：使用 `MmGetSystemRoutineAddress(&name)` — WDK标准的导出函数查询API
- **返回值**：函数虚拟地址，失败则为0

### 3.2 EPT钩子处理函数

#### `HookNtQueryInformationProcess` — 隐藏调试器存在

```c
static NTSTATUS NTAPI HookNtQueryInformationProcess(
    HANDLE  ProcessHandle,
    ULONG   ProcessInformationClass,
    PVOID   ProcessInformation,
    ULONG   ProcessInformationLength,
    PULONG  ReturnLength
)
```

**功能**：EPT钩子拦截 `NtQueryInformationProcess`，根据目标进程的标志位篡改返回信息。

**核心逻辑**：
1. **调用原始函数**：通过蹦床 `g_AadState.OrigNtQueryInformationProcess` 调用真正的 `NtQueryInformationProcess`
2. **快速失败**：若原始函数失败，直接返回
3. **检查目标进程**：通过 `ProcessFindByCr3(__readcr3())` 判断当前进程是否为受保护的目标
4. **标志位检查**：检查 `AAD_HIDE_DEBUGGER` 标志
5. **SEH保护**：用 `__try/__except` 包装用户缓冲区写入（防止用户态无效指针导致崩溃）

**处理的三种信息类**：

| 信息类 | 宏值 | 含义 | 伪造方法 |
|--------|------|------|----------|
| `ProcessDebugPort` | 7 | 调试端口（非0表示被调试） | 置为0 |
| `ProcessDebugObjectHandle` | 0x1E | 调试对象句柄（存在=被调试） | 置NULL，状态码改为 `STATUS_PORT_NOT_SET` (0xC0000353) |
| `ProcessDebugFlags` | 0x1F | 非调试继承标志（0=被调试） | 置为1（表示未被调试） |

#### `HookNtQuerySystemInformation` — 隐藏系统调试信息

```c
static NTSTATUS NTAPI HookNtQuerySystemInformation(
    ULONG   SystemInformationClass,
    PVOID   SystemInformation,
    ULONG   SystemInformationLength,
    PULONG  ReturnLength
)
```

**功能**：EPT钩子拦截 `NtQuerySystemInformation`，伪造内核调试器信息。

**核心逻辑**：
1. 调用原始函数获取真实信息
2. 通过 `IsFeatureEnabled(CurrentCr3, AAD_HIDE_SYSINFO)` 检查当前进程是否启用此功能
3. 当 `SystemInformationClass == SystemKernelDebuggerInformation`（0x23）时：
   - 设置 `KernelDebuggerEnabled = FALSE`
   - 设置 `KernelDebuggerNotPresent = TRUE`
4. SEH保护用户缓冲区写入

#### `HookNtSetInformationThread` — 阻止线程反调试

```c
static NTSTATUS NTAPI HookNtSetInformationThread(
    HANDLE  ThreadHandle,
    ULONG   ThreadInformationClass,
    PVOID   ThreadInformation,
    ULONG   ThreadInformationLength
)
```

**功能**：拦截并阻止目标进程调用 `ThreadHideFromDebugger`。

**核心逻辑**：
- 当 `ThreadInformationClass == 0x11`（`ThreadHideFromDebugger`）且目标进程启用 `AAD_HIDE_THREADINFO` 时：
  - **直接返回 `STATUS_SUCCESS`**，不调用原始函数
  - 这使目标进程认为调用"成功"，但实际上线程没有被隐藏
- 否则正常调用原始函数

#### `HookNtClose` — 抑制无效句柄异常

```c
static NTSTATUS NTAPI HookNtClose(HANDLE Handle)
```

**功能**：防止反调试技通过传递无效句柄给 `NtClose` 触发异常（某些调试器会捕获此类异常）。

**反调试原理**：某些程序故意调用 `NtClose(INVALID_HANDLE_VALUE)` 或 `NtClose(0)`，在非调试环境下会引发异常（并通常被VEH/SEH处理），而在调试环境下异常会被调试器捕获。通过这种方式程序可以检测调试器是否存在。

**核心逻辑**：
- 当目标进程启用 `AAD_HIDE_NTCLOSE`：
  - 用 `__try/__except` 包装原始 `NtClose` 调用
  - 如果原始调用引发异常（`STATUS_HANDLE_NOT_CLOSABLE` 或 `STATUS_INVALID_HANDLE`），用 `GetExceptionCode()` 获取异常码作为返回值
  - 记录抑制的异常日志
- 否则正常调用原始函数

### 3.3 初始化/清理

#### `AadInitialize`

```c
NTSTATUS AadInitialize(VOID)
```

**功能**：解析4个内核函数的地址并初始化全局状态。

**核心流程**：
1. 清零 `g_AadState`
2. 依次解析 `NtQueryInformationProcess`、`NtQuerySystemInformation`、`NtSetInformationThread`、`NtClose`
3. 至少 `NtQueryInformationProcess` 必须成功解析
4. 设置 `Initialized = TRUE`

#### `AadCleanup`

```c
VOID AadCleanup(VOID)
```

- 调用 `AadRemoveHooks()` 卸载所有EPT钩子
- 设置 `Initialized = FALSE`

### 3.4 EPT钩子安装/卸载

#### `AadInstallHooks`

```c
NTSTATUS AadInstallHooks(VOID)
```

**功能**：安装4个EPT不可见钩子到内核API。

**核心流程**：
1. 若未初始化，先调用 `AadInitialize()` 解析函数地址
2. 对每个函数调用 `HvHookFunction(TargetAddr, HookFunc, &OrigFunc)`：
   - `TargetAddr`：内核API的地址
   - `HookFunc`：替换处理函数的地址（如 `HookNtQueryInformationProcess`）
   - `OrigFunc`：输出参数，接收蹦床地址（用于调用原始函数）
3. 即使某个钩子安装失败，也继续安装其他钩子（逐个报告日志）

#### `AadRemoveHooks`

```c
VOID AadRemoveHooks(VOID)
```

- 对每个已解析地址调用 `HvUnhookFunction(Addr)` 恢复原始EPT映射

### 3.5 调试寄存器访问处理器

#### `AadHandleDrAccess`

```c
BOOLEAN AadHandleDrAccess(PGUEST_CONTEXT GuestContext)
```

**功能**：处理MOV-DR VM-Exit（当Guest执行 `mov drN, reg` 或 `mov reg, drN` 指令时触发），为受保护进程伪造调试寄存器值。

**退出条件解析**（从 `ExitQualification` VMCS字段提取）：
- `DrNumber` (位2:0) — 访问哪个调试寄存器（DR0-DR7）
- `Direction` (位4) — 读（MOV FROM DR）还是写（MOV TO DR）
- `GpReg` (位11:8) — 涉及的通用寄存器编号

**核心流程**：

```
AadHandleDrAccess(GuestContext)
    │
    ├── 解析 ExitQualification → DrNumber, Direction, GpReg
    ├── 获取 GuestCr3 = HvReadGuestCr3()
    ├── GpReg 范围检查: >15 → 跳过
    │
    ├── RegPtr = &GpRegs[GpReg] — 指向GUEST_CONTEXT中的对应寄存器
    │
    ├── [非目标进程] IsFeatureEnabled(Cr3, AAD_HIDE_HWBP) == FALSE?
    │   ├── [读] 真实DR值 → *RegPtr (写入Guest寄存器)
    │   ├── [写] *RegPtr (从Guest寄存器读取) → 写入真实DR
    │   └── 推进RIP, 返回
    │
    └── [目标进程] AAD_HIDE_HWBP 启用?
        ├── [读] 伪造DR值:
        │   ├── DR0-DR3: 返回 0 (隐藏硬件断点地址)
        │   ├── DR6: 返回 DR6_DEFAULT_VALUE (0xFFFF0FF0)
        │   └── DR7: 返回 DR7_DEFAULT_VALUE (0x400)
        └── [写] 仍然真实写入DR（保持硬件断点功能有效，但读取时隐藏）
            └── 推进RIP, 返回
```

**设计要点**：
- **统一RSP同步**：VM-Exit入口处已将VMCS/VMCB中的Guest RSP读入 `GuestContext->Rsp`，因此 `GpRegs[4]`（对应RSP）始终有效，无需特殊处理
- **写DR0-3时仍然真实写入**：硬件断点仍然在CPU层面生效（调试器可以触发断点），但受保护进程读取DR时看到的永远是干净的伪造值
- **对比HyperDbg**：注释中指出HyperDbg存在一个bug——MOV FROM DR读到的值仅存在局部变量而未写回Guest寄存器。VMXToolbox的 `RegPtr` 直接指向 `GUEST_CONTEXT`，没有这个bug

### 3.6 TSC偏移管理

#### `AadUpdateHwTscOffset`

```c
VOID AadUpdateHwTscOffset(ULONG64 NewCr3)
```

**功能**：在进程上下文切换（CR3加载）时，根据新进程是否为受保护目标来设置或清除硬件TSC偏移。

**核心逻辑**：
- 当进程切换的目标进程启用了 `AAD_HIDE_TIMING`：
  - 写入 `HvCtx->TscOffset` 到VMCS/VMCB的TSC Offset字段（负偏移）
  - Guest执行RDTSC/RDTSCP时自动获得补偿后的值，无需VM-Exit
- 对于非目标进程：将TSC Offset设置为0（无补偿）

#### `AadNotifyDebugPause`

```c
VOID AadNotifyDebugPause(ULONG CpuIndex)
```

**功能**：通知引擎调试器已暂停执行（如断点命中）。

**核心逻辑**：
- 记录当前TSC值到 `HvCtx->LastDebugPauseTsc`
- 设置 `HvCtx->InDebugPause = TRUE`
- 仅在首次暂停时记录（`!InDebugPause` 守卫）

#### `AadNotifyDebugResume`

```c
VOID AadNotifyDebugResume(ULONG CpuIndex)
```

**功能**：通知引擎调试器已恢复执行，累加暂停期间的时间差。

**核心逻辑**：
1. 读取当前TSC `Now`
2. 计算暂停持续时间 `PauseDuration = Now - LastDebugPauseTsc`
3. 累加到 `HvCtx->TscOffset += PauseDuration`
4. 设置 `InDebugPause = FALSE`
5. 若当前进程启用了 `AAD_HIDE_TIMING`，立即更新硬件TSC Offset

**时序补偿原理**：
```
Guest应用程序:  ┌──────────────────────┐     ┌──────────┐
                │  正常执行             │     │  继续      │
TSC值:          ▲                      ▲     ▲          ▲
                │                      │     │          │
                └── TSC递增 ──────────┘  ... └─ TSC递增 ─┘
                                      ▲─────▲
                                      调试暂停期间
                                      TSC仍在递增
                                      暂停结束累加差值到Offset
                                      Guest读到的TSC会减去此差值
                                      感觉不到暂停的发生
```

### 3.7 CPUID处理器

#### `AadHandleCpuid`

```c
BOOLEAN AadHandleCpuid(PGUEST_CONTEXT GuestContext)
```

**功能**：处理CPUID VM-Exit，隐藏超管存在并处理各种CPUID叶子节点。

**特殊后门**：
```c
if (Leaf == CPUID_BACKDOOR_LEAF) {  // 0x4CAFE000
    GuestContext->Rax = CPUID_BACKDOOR_MAGIC;  // 0x564D5854 = "VMXT"
    ...
}
```
这是一个**私有检测机制**：通过查询特殊的CPUID叶子（`0x4CAFE000`），任何用户态或内核态代码可以确认VMXToolbox超管是否激活，返回 `EAX=0x564D5854`（"VMXT"）。这是内部诊断和状态检测的手段，不会暴露给未授权的代码。

**各叶子节点处理**：

| 叶子 | 处理方式 | 目的 |
|------|----------|------|
| `0x4CAFE000` | 返回验证幻数 "VMXT" | 私有超管检测后门 |
| Leaf 1 | 清除bit 5（隐藏VMX），清除bit 31（Hypervisor存在位） | 裸机策略：bit 31=0，Windows不会查询超管CPUID叶子 |
| Leaf 0x80000001 | 清除bit 2（隐藏SVM） | 防止嵌套超管检测到AMD-V支持 |
| Leaf 0x8000000A | 全部清零 | 返回0阻止嵌套SVM检测 |
| Leaf 0x40000000-0x400000FF | 透传原始硬件值（裸机返回全零） | 不模拟任何超管接口 |

**裸机策略（原嵌套虚拟化设计已被取代）**：
- 在裸机上（无底层 Hyper-V），`CPUID.1:ECX[31]` 已经为 0
- **现策略**：直接清除 bit 31（防御性深度防御），使 Windows 不查询超管 CPUID 叶子
- Windows 引导时如果检测到 `CPUID.1:ECX[31]=0`，不会缓存"运行在超管之上"的决策
- 超管 CPUID 叶子（0x40000000+）不做任何模拟，透传原始硬件值（裸机返回全零）
- 此策略更简洁：不模拟 Hyper-V 接口，不维护 VMCALL 的兼容性
- 旧设计（保持 bit 31=1 并模拟 "Microsoft Hv"/"Hv#0"）已被移除，因为裸机场景不需要嵌套兼容性

### 3.8 异常处理器

#### `AadHandleException`

```c
BOOLEAN AadHandleException(PGUEST_CONTEXT GuestContext)
```

**功能**：处理被拦截的异常（#DB、#BP）用于反反调试的异常行为规范化。

**反调试异常原理**：
- **INT 2D**：调试器会跳过INT 2D后的字节，非调试程序不会
- **INT 3**（#BP）：调试器会捕获，非调试程序的SEH/VEH处理它
- **单步执行**（#DB）：调试器会拦截，行为差异可被检测

**核心逻辑**：
```
AadHandleException(GuestContext)
    │
    ├── 读取 ExitInterruptionInfo
    ├── 检查 Valid 位 → 无效则返回 FALSE
    ├── 提取 Vector (异常向量号), IntType (类型)
    │
    ├── 检查 GuestCr3 → IsFeatureEnabled(Cr3, AAD_HIDE_EXCEPTIONS)
    │   └── 是目标进程:
    │       ├── Vector 1 (#DB): 清除 RFLAGS 中的单步标志, 重新注入到Guest SEH
    │       └── Vector 3 (#BP): 重新注入到Guest SEH
    │
    └── 构造 VM-Entry 中断注入信息:
        ├── 设置中断有效位 (VALID | Vector | Type)
        ├── 若有错误码 → 通过 HvSetEntryExceptionErrorCode 设置
        ├── 若为软件异常/中断 → 设置指令长度
        └── 不推进RIP（异常处理程序会处理）
```

**异常重新注入**：
- 不在超管层面处理异常，而是重新注入到Guest OS
- Guest OS的异常分发机制会像在未调试环境下一样处理异常
- 这防止了调试器捕获异常并暴露其存在

---

## 4. 控制流与逻辑流程

### 4.1 完整事件流

```
Guest程序执行
    │
    ├── 调用 NtQueryInformationProcess → EPT缺页 → VM-Exit → EPT处理
    │   → HookNtQueryInformationProcess (伪造调试信息)
    │
    ├── 调用 NtQuerySystemInformation → EPT缺页 → VM-Exit → EPT处理
    │   → HookNtQuerySystemInformation (隐藏内核调试器)
    │
    ├── 调用 NtSetInformationThread(ThreadHideFromDebugger) → EPT缺页 → VM-Exit
    │   → HookNtSetInformationThread (拦截并返回成功)
    │
    ├── 调用 NtClose(无效句柄) → EPT缺页 → VM-Exit
    │   → HookNtClose (SEH包装抑制异常)
    │
    ├── 执行 MOV reg, DR0-DR7 / MOV DR0-DR7, reg → MOV-DR VM-Exit
    │   → AadHandleDrAccess (伪造/真实DR值返回)
    │
    ├── 执行 RDTSC / RDTSCP → 硬件TSC偏移自动计算 (无需VM-Exit)
    │
    ├── 执行 CPUID → CPUID VM-Exit
    │   → AadHandleCpuid (伪造超管相关叶子)
    │
    └── 触发 #DB / #BP 异常 → 异常 VM-Exit
        → AadHandleException (重新注入到Guest)
```

### 4.2 特征检查流程

`IsFeatureEnabled(CurrentCr3, FeatureFlag)` 的调用链：
```
AadHandleDrAccess / HookNtQuerySystemInformation / HookNtSetInformationThread / HookNtClose / AadHandleException / AadHandleCpuid
    │
    └── IsFeatureEnabled(cr3, AAD_HIDE_*)
        │
        └── ProcessFindByCr3(cr3)
            │
            └── 遍历 g_ProcessTracking.Targets[]
                ├── 匹配 CR3 → 返回 TARGET_PROCESS
                └── 不匹配 → 返回 NULL
```

### 4.3 TSC偏移生命周期

```
CR3加载 (进程切换)
    │
    └── AadUpdateHwTscOffset(NewCr3)
        ├── 目标进程: HvWriteTscOffset(HvCtx->TscOffset) — 应用负偏移
        └── 非目标: HvWriteTscOffset(0) — 无补偿

调试暂停
    │
    └── AadNotifyDebugPause(CpuIndex)
        └── LastDebugPauseTsc = rdtsc(), InDebugPause = TRUE

调试恢复
    │
    └── AadNotifyDebugResume(CpuIndex)
        ├── PauseDuration = rdtsc() - LastDebugPauseTsc
        ├── TscOffset += PauseDuration
        └── 若目标进程: HvWriteTscOffset(HvCtx->TscOffset)
```

### 4.4 条件分支总结

| 条件 | 分支路径 |
|------|----------|
| `ProcessFindByCr3(Cr3)` 返回 NULL | 非目标进程→不执行任何反反调试操作 |
| `Target->Flags & AAD_HIDE_*` 为0 | 特定功能禁用→该功能不生效 |
| `Target->Flags & AAD_HIDE_*` 非0 | 功能启用→执行对应的伪造/拦截 |
| `AadHandleDrAccess`: 非目标 | 真实DR值读写 |
| `AadHandleDrAccess`: 目标+读 | 伪造DR值（全0/默认值） |
| `AadHandleDrAccess`: 目标+写 | 真实写入DR（断点功能有效） |
| `CPUID Leaf` 分派 | 根据叶子类型执行不同篡改策略 |

---

## 5. 与其他模块的交互

### 5.1 通过 hv_ops vtable 的交互

```c
// EPT钩子安装/卸载
HvHookFunction(target, hook, &orig)   // hv_ops.HookFunction → ept.c / npt.c
HvUnhookFunction(target)               // hv_ops.UnhookFunction → ept.c / npt.c

// Guest状态读取
HvReadGuestCr3()                       // hv_ops.ReadGuestCr3 → VMCS/VMCB
HvReadExitQualification()              // hv_ops.ReadExitQualification → VMCS/VMCB
HvReadExitInterruptionInfo()           // hv_ops.ReadExitInterruptionInfo → VMCS/VMCB

// TSC偏移
HvWriteTscOffset(offset)               // hv_ops.WriteTscOffset → VMCS TSC Offset

// 虚拟化控制
HvAdvanceGuestRip()                    // hv_ops.AdvanceGuestRip
HvSetEntryInterruptionInfo(info)       // hv_ops.SetEntryInterruptionInfo
HvSetEntryExceptionErrorCode(code)     // hv_ops.SetEntryExceptionErrorCode
HvSetEntryInstructionLength(len)       // hv_ops.SetEntryInstructionLength
```

### 5.2 与进程追踪模块的交互

`anti_anti_debug.c` 大量使用 `process.h` 提供的函数进行目标进程识别：

- **`ProcessFindByCr3(CurrentCr3)`**：在 `HookNtQueryInformationProcess` 中用于确认当前进程是否为受保护目标
- **`IsFeatureEnabled(Cr3, Flag)`**：在 `HookNtQuerySystemInformation`、`HookNtSetInformationThread`、`HookNtClose`、`AadHandleDrAccess` 和 `AadHandleException` 中检查特定功能是否启用
- **CR3匹配机制**：每次VM-Exit时读取Guest CR3，与目标进程的CR3比较（比PID更可靠，因为在进程上下文切换时CR3会立即变化）

### 5.3 与EPT底层的关系

EPT钩子的安装和卸载通过 `HvHookFunction` / `HvUnhookFunction` 宏调用底层EPT/NPT实现：
- 安装时：EPT页表分裂，目标函数页面被映射到Thunk/替换函数页面
- 卸载时：EPT映射恢复原始地址，TLB跨CPU同步

### 5.4 与日志模块的交互

- `LOG_DEBUG_PID(pid, "Spoofed ProcessDebugPort = 0")` — 添加了PID上下文的反反调试专用日志
- `VMXROOT_LOG_DEBUG("DR%u read spoofed: returned 0x%llX")` — 调试寄存器伪造日志（仅在VMX根模式活跃时）
- 条件编译策略：日志通过条件编译控制，生产构建中可剥离

### 5.5 与VM-Exit调度器的交互

`anti_anti_debug.c` 中的函数被VM-Exit调度器（`vmx_exit.c` / `svm_exit.c`）调用：

| VM-Exit原因 | 调度器调用 |
|-------------|-----------|
| `EXIT_REASON_DR_ACCESS` (29) | `AadHandleDrAccess(GuestContext)` |
| `EXIT_REASON_CPUID` (10) | `AadHandleCpuid(GuestContext)` |
| `EXIT_REASON_EXCEPTION_NMI` (0) | `AadHandleException(GuestContext)` |
| CR3加载/存储 | `AadUpdateHwTscOffset(NewCr3)` |

EPT钩子触发的VM-Exit（`EXIT_REASON_EPT_VIOLATION` 48）由EPT处理程序处理，它检查GPA是否匹配已Hook的函数地址，若命中则重定向到替换函数。

---

## 6. 关键设计要点

### 6.1 EPT不可见钩子 vs 传统Inline Hook

传统钩子方法（修改代码页）存在以下问题：
- **PatchGuard检测**：Windows内核代码完整性检查会检测到代码修改并触发BSOD
- **反作弊检测**：反作弊系统扫描内存中的代码页修改
- **多核同步**：ACPI规范要求修改代码时关闭中断且跨CPU同步

EPT钩子的优势：
- **不修改代码页**：Guest看到的代码页内容始终是原始的
- **完全透明**：在EPT（第二级地址翻译）层面拦截，Guest无法检测
- **PatchGuard免疫**：因为代码页哈希没有变化
- **进程粒度的控制**：可以针对特定进程启用/禁用钩子

### 6.2 调试寄存器隐藏策略（DR Spoofing）

**读-写不对称设计**：
- **读操作**：返回伪造值（硬件断点地址被隐藏为0，状态寄存器被重置为默认值）
- **写操作**：仍然真实写入（保持硬件断点功能在CPU层面有效）

这意味着调试器仍然可以设置和触发硬件断点（调试功能正常），但受保护的进程通过读取DR寄存器无法检测到断点的存在。

### 6.3 TSC时间补偿的硬件加速

**设计**：使用VMCS/VMCB的硬件TSC Offset字段，而非在每次RDTSC/RDTSCP指令时通过VM-Exit进行模拟。

**优点**：
- **零VM-Exit开销**：RDTSC/RDTSCP直接在硬件层面完成TSC偏移计算
- **完全透明**：Guest无法检测到TSC被篡改（因为没有VM-Exit发生）
- **调试暂停时间隐藏**：累计调试暂停期间的所有TSC增量，作为负偏移注入

**局限性**：
- 需要硬件支持（Intel VT-x的TSC Offsetting功能或AMD-V的TSC Ratio功能）
- 偏移量有范围限制（VMCS TSC Offset是64位有符号整数）

### 6.4 CPUID裸机策略（原嵌套虚拟化兼容性）

**之前的设计（已移除）**：
- 旧的实现模拟了 Hyper-V 超管接口：`CPUID.0x40000000` 报告 "Microsoft Hv"，`CPUID.0x40000001` 返回 "Hv#0"
- 保持 `CPUID.1:ECX[31]=1`（Hypervisor 存在位）以匹配 Windows 的引导时缓存决策
- 此设计是为在 VMware (L0) 内部运行嵌套虚拟化而设计的

**当前策略**：
- `CPUID.1:ECX[31]` 被**清除为 0**（不再保留 Hypervisor 存在位）
- 超管 CPUID 叶子（0x40000000-0x400000FF）不做模拟，透传原始硬件值
- 在裸机上，这些叶子返回全零——没有 "Microsoft Hv"，没有 "Hv#0"，没有任何超管接口信息

**理由**：
- 在裸机上（无底层 Hyper-V），Windows 引导时 `CPUID.1:ECX[31]` 为 0。清除 bit 31 与硬件状态一致
- Windows 不会查询超管 CPUID 叶子，因为 `CPUID.1:ECX[31]=0` 意味着"无超管"
- 移除了维护 Hyper-V 兼容接口的复杂性——没有 VMCALL 处理，没有 enlightenment，没有 'Hv#0' 非符合接口伪装
- 第三方检测工具查询这些叶子会看到原始硬件值，这更真实

**在 VMXToolbox 作为嵌套超管运行时的行为**：
- 如果 L0 超管（如 VMware、Hyper-V）设置了 `CPUID.1:ECX[31]=1`，则 bit 31 仍可用——L0 会报告它，而 VMXToolbox 在此之后运行
- 通过清除 bit 31，嵌套 guest 认为没有超管存在
- 如果 L0 也模拟了 0x40000000+ 叶子，这些值会被嵌套 guest 看到（VMXToolbox 不再清除或修改它们）

### 6.5 异常重新注入 vs 直接处理

反反调试的异常处理采用**重新注入策略**而非直接在超管层面处理异常：
- 将异常原样注入回Guest OS
- Guest OS的异常分发机制（IDT、VEH、SEH）正常运行
- 调试器不会收到异常通知（因为异常在VM-Entry时注入，不经过调试器）

**INT 2D反调试对抗**：
- INT 2D（0x2D，`IceBP`）是Windows调试器使用的特殊断点
- 非调试环境下，执行INT 2D会触发异常，但下一条指令地址的跳过行为不同
- `AadHandleException` 不做特殊处理，而是重新注入，让Guest SEH自然处理

### 6.6 安全性考量

| 攻击面 | 防护措施 |
|--------|----------|
| 用户缓冲区写入 | 所有EPT钩子处理函数使用 `__try/__except` 保护 |
| 寄存器索引越界 | `GpReg > 15` 检查 |
| 非目标进程保护 | `IsFeatureEnabled` 确保只对受保护进程生效 |
| CR3匹配混淆 | 使用CR3作为进程标识（比PID更精确，不会重复使用） |
| TSC溢出 | 64位有符号TSC偏移，暂停累积采用加法累加而非乘法 |
| DR写入影响调试 | 写操作真实执行，不阻止调试器设置断点 |
| `NtClose`异常传播 | SEH包装确保异常不会传递到调试器 |

### 6.7 未实现的技术（标志已定义）

- **`AAD_HIDE_HEAP` (bit 8)**：堆标志隐藏。在选定目标进程时此标志可被设置和存储，但当前 `anti_anti_debug.c` 中没有对应的实现代码。可能是在用户态的PEB操作中完成，或由其他组件实现。
- **`AAD_HIDE_PARENT` (bit 9)**：父进程伪造。同上，标志已定义但反反调试引擎的C代码中没有对应的实现。

这两个功能的实现可能在 `process.c` 或其他模块中，或计划在后续版本中加入。
