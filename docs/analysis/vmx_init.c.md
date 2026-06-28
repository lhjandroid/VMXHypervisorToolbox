# vmx_init.c — 逻辑分析

## 1. 文件概述

### 角色与职责

`vmx_init.c` 是 Intel VT-x 后端的初始化与终止模块，是整个 VMX 子系统的入口。它负责：

1. **VMX 支持检测**：通过 CPUID 和 MSR 检查 CPU 是否支持 VMX，IA32_FEATURE_CONTROL 是否已锁定并启用 VMXON。
2. **能力校验**：读取所有 VMX 能力 MSR（包括真实控制位），验证必需的硬件特性（EPT、RDTSCP、VPID）是否可用。
3. **内存分配**：为每个 CPU 分配 VMXON 区域、VMCS 区域、MSR 位图、I/O 位图以及 Host 栈（32KB）。
4. **VMCS 配置**：完整配置 VM-Execution 控制字段、VM-Exit 控制字段、VM-Entry 控制字段、Guest 状态区、Host 状态区。
5. **Per-CPU 虚拟化启用**：通过 DPC 在每个 CPU 上执行 VMXON → VMPTRLD → VMLAUNCH 序列。
6. **HV_OPS vtable 注册**：将 VMX 后端的函数指针填充到 `g_VmxOps` 中，供上层模块通过 `g_HvOps` 统一调用。
7. **动态异常位图同步**：AAD-BP 机制，通过全局"期望值"加 per-CPU"已应用代数"实现跨 CPU 的 Exception Bitmap 延迟同步。

### 依赖的其他模块

| 模块 | 用途 |
|------|------|
| `vmx.h` | VMCS 字段编码、常量、数据结构、VmxRead/VmxWrite 内联函数 |
| `ept.c/h` | EPT 身份映射初始化 (`EptSetupIdentityMap`)、全局初始化 (`EptInitialize`) |
| `log.h` | 日志输出宏 (`LOG_INFO`, `LOG_ERROR`, `LOG_WARN`) |
| `hv_ops.h` | HV_OPS vtable 定义、g_MaxProcessors 全局变量 |
| `hv_detect.h` | `HvSetTargetProcessorDpc` 动态决议（>127 CPU 支持） |
| `process.h` | `ProcessRegisterExceptionHideToggle` 注册异常拦截回调 |
| `msr.c` | MSR 预探测 (`MsrProbeInvalidMsrs`) 和清理 (`MsrCleanupInvalidBitmap`) |

---

## 2. 数据结构

### `VMX_DPC_CONTEXT` (局部)

用于在 DPC 例程和主线程之间传递状态。

```c
typedef struct _VMX_DPC_CONTEXT {
    PVMX_STATE  State;      // 全局 VMX 状态指针
    NTSTATUS    Status;     // DPC 执行结果
    KEVENT      Event;      // 同步事件，主线程等待此事件
} VMX_DPC_CONTEXT;
```

### `VMX_CPU_CONTEXT` (定义于 vmx.h)

每个 CPU 的私有上下文：

| 字段 | 含义 |
|------|------|
| `VmxonRegionVa/Pa` | VMXON 区域的虚拟地址/物理地址（4KB 对齐） |
| `VmcsRegionVa/Pa` | VMCS 区域的虚拟地址/物理地址（4KB 对齐） |
| `MsrBitmapVa/Pa` | MSR 位图的虚拟地址/物理地址（4KB） |
| `IoBitmapAVa/Pa`, `IoBitmapBVa/Pa` | I/O 位图 A/B 的虚拟地址/物理地址（各4KB） |
| `HostStackBase/Size` | VM-Exit 处理器的 Host 栈（32KB） |
| `VmxEnabled` | VMXON 是否已成功执行 |
| `VmcsLaunched` | VMLAUNCH 是否已成功执行 |
| `ProcessorNumber` | CPU 编号 |
| `OriginalCr4` | VMXON 前原始的 CR4 值（用于恢复） |
| `TscOffset` | 累积的 TSC 偏移量（反时序检测） |
| `LastDebugPauseTsc` | 调试暂停开始时的 TSC |
| `InDebugPause` | 当前是否处于调试暂停状态 |
| `ExitCount` | VM-Exit 计数器（`volatile LONG64`） |

### `VMX_STATE` (定义于 vmx.h)

全局 VMX 状态：

| 字段 | 含义 |
|------|------|
| `CpuContexts` | 动态分配的 `VMX_CPU_CONTEXT` 数组 `[g_MaxProcessors]` |
| `CpuCount` | 活动 CPU 数量 |
| `Initialized` | VMX 是否已初始化完成 |
| `VmcsRevisionId` | 从 `IA32_VMX_BASIC` MSR 读取的 VMCS 修订号 |
| `VmxBasic` | `IA32_VMX_BASIC` MSR 原始值 |
| `PinBasedCap`, `ProcBasedCap` 等 | 各控制字段的能力 MSR 值 |
| `TrueControlsSupported` | 是否支持真实控制位 MSR |
| `TruePinBasedCap` 等 | 真实控制位能力 MSR（如支持） |

### Exception Bitmap 同步相关全局变量

| 变量 | 含义 |
|------|------|
| `g_VmxDesiredExceptionBitmap` | 全局期望的 Exception Bitmap 值 |
| `g_VmxExcBmpGeneration` | 全局代数计数器，每次期望值变化时递增 |
| `g_VmxExcBmpCpuGen` | Per-CPU 已应用代数数组 |
| `g_VmxExcBmpLock` | 保护期望值的自旋锁 |
| `g_VmxExcBmpInited` | 初始化标志 |

---

## 3. 核心函数详解

### `VmxIsSupported()`

- **签名**: `BOOLEAN VmxIsSupported(VOID)`
- **功能**: 检查当前 CPU 是否支持 VMX 且 BIOS 已正确配置
- **核心逻辑**:
  1. 执行 `CPUID.1`，检查 `ECX[5]`（VMX 位）
  2. 读取 `IA32_FEATURE_CONTROL` MSR（0x3A）
  3. 如果 MSR 已锁定 (`bit 0`)，检查 VMXON 是否启用 (`bit 2`)
  4. 如果 MSR 未锁定，发出警告但不拒绝——说明 BIOS 未正确配置 VMX
- **返回值**: TRUE=支持，FALSE=不支持或 BIOS 锁定

### `VmxCheckCapabilities()`

- **签名**: `static BOOLEAN VmxCheckCapabilities(PVMX_STATE State)`
- **功能**: 读取所有 VMX 能力 MSR，验证必需特性
- **核心逻辑**:
  1. 读取 `IA32_VMX_BASIC` 获取 VMCS 修订号和真实控制位支持
  2. 如支持真实控制位，读取 `TRUE_PINBASED_CTLS` 等四个 MSR
  3. 始终读取标准控制位 MSR
  4. 检查二级控制支持，如支持则读取 `PROCBASED_CTLS2`
  5. 读取 `EPT_VPID_CAP`
  6. 验证 EPT 必须可用，否则返回 FALSE
- **返回值**: TRUE=能力满足要求

### `VmxAdjustControls()`

- **签名**: `static ULONG VmxAdjustControls(ULONG RequestedControls, ULONG64 Capability)`
- **功能**: 按 Intel SDM Vol.3C 第 24.6.1 节调整控制字段
- **逻辑**: 低位32位是必须设为1的位（`RequestedControls |= Low`），高位32位是可设为1的位（`RequestedControls &= High`）
- **返回值**: 调整后的控制值

### `VmxAllocateAlignedMemory()`

- **签名**: `static PVOID VmxAllocateAlignedMemory(SIZE_T Size, ULONG64 *PhysicalAddress)`
- **功能**: 分配连续的、页对齐的物理内存（通过 `MmAllocateContiguousMemorySpecifyCache`）
- **返回值**: 虚拟地址，通过 `PhysicalAddress` 输出物理地址

### `VmxAllocateCpuContext()`

- **签名**: `static NTSTATUS VmxAllocateCpuContext(PVMX_CPU_CONTEXT CpuCtx, ULONG VmcsRevision)`
- **功能**: 为单个 CPU 分配所有必需的上下文内存
- **分配的内存区域**:
  1. VMXON 区域（4KB）— 写入 VMCS 修订号
  2. VMCS 区域（4KB）— 写入 VMCS 修订号
  3. MSR 位图（4KB）— 全部清零
  4. I/O 位图 A（4KB）— 全部清零 = 无 I/O 退出
  5. I/O 位图 B（4KB）— 全部清零
  6. Host 栈（32KB = 8×4KB）— 通过 `ExAllocatePoolWithTag(NonPagedPool)`
- **返回值**: `STATUS_SUCCESS` 或 `STATUS_INSUFFICIENT_RESOURCES`

### `VmxFreeCpuContext()`

- **签名**: `static VOID VmxFreeCpuContext(PVMX_CPU_CONTEXT CpuCtx)`
- **功能**: 释放 `VmxAllocateCpuContext` 分配的所有内存
- **注意**: 使用 `MmFreeContiguousMemory` 释放连续内存，`ExFreePoolWithTag` 释放 Host 栈

### `VmxGetSegmentBase()`, `VmxGetSegmentAccessRights()`, `VmxGetSegmentLimit()`

- **签名**: `static ULONG64 VmxGetSegmentBase/GdtBase, Selector)` 等
- **功能**: 从 GDT 条目解析段描述符的基址、访问权限、界限
- **核心逻辑**:
  - 使用 Selector 索引 GDT 条目（`Selector >> 3`）
  - `VmxGetSegmentBase`: 合成 BaseLow、BaseMid、BaseHigh；对系统段（S=0）在64位模式下还要读取 BaseUpper（16字节描述符）
  - `VmxGetSegmentAccessRights`: 按 VMCS 访问权限格式（SDM Table 24-2）从 Access 和 LimitHighAndFlags 合成
  - `VmxGetSegmentLimit`: 合成 LimitLow 和 LimitHighAndFlags，如果 G 位（粒度）置位则左移12位并补充0xFFF
  - 对 Selector=0 或空选择子，Base 返回 0，AccessRights 返回 0x10000 (Unusable)，Limit 返回 0

### `VmxSetupVmcs()`

- **签名**: `NTSTATUS VmxSetupVmcs(PVMX_CPU_CONTEXT CpuCtx, PVMX_STATE State)`
- **功能**: 配置完整的 VMCS（Intel SDM 定义的全部六个区域）
- **核心流程**:

#### 阶段 1: VMCLEAR + VMPTRLD
- 执行 `__vmx_vmclear()` 清除 VMCS 状态
- 执行 `__vmx_vmptrld()` 加载 VMCS 为当前

#### 阶段 2: VM-Execution 控制字段

**Pin-Based Controls:**
- 只请求 `PIN_BASED_NMI_EXIT`（NMI 退出，支持 WinDbg Ctrl+Break）
- 不请求 `PIN_BASED_EXTERNAL_INT_EXIT`（Blue Pill 设计：外部中断直接通过 Guest IDT 传递）

**Primary Processor-Based Controls:**
```
USE_MSR_BITMAPS | USE_IO_BITMAPS | SECONDARY_CONTROLS |
CR3_LOAD_EXIT | MOV_DR_EXIT | USE_TSC_OFFSETTING
```
- 日志诊断：输出 must-be-1 位强制启用的额外位

**Secondary Processor-Based Controls:**
```
ENABLE_EPT | ENABLE_RDTSCP | ENABLE_VPID |
ENABLE_INVPCID | ENABLE_XSAVES
```
- 日志诊断：输出强制位（如 DESC_TABLE_EXIT、WBINVD_EXIT）

**其他控制字段:**
- Exception Bitmap: 0（初始不拦截任何异常）
- MSR Bitmap: 写入物理地址，MSR 拦截位通过 `MsrBitmapInitialize` 配置
- I/O Bitmaps: 全部清零 = 无 I/O 端口触发 VM-Exit
- VPID: `ProcessorNumber + 1`（VPID 0 保留给 Host）
- XSS-Exiting Bitmap: 0（不拦截 XSAVES/XRSTORS）
- EPT Pointer: 通过 `EptSetupIdentityMap` 设置身份映射 EPT

#### 阶段 3: CR0/CR4 Guest-Host Masks

**CR0 Guest-Host Mask（Audit #1 修正）:**

原实现仅使用 `MSR_IA32_VMX_CR0_FIXED0` 作为掩码，只拦截了 must-be-0 位的写操作，遗漏了 must-be-1 位（如 PE、PG、NE）。Guest 清除了 must-be-1 位时不会触发 VM-Exit，导致下一次 VM-Entry 失败。

修正后读取 `Cr0Fixed0` 和 `Cr0Fixed1` 两个 MSR，使用 `Cr0Fixed0 ^ Cr0Fixed1` 作为掩码。XOR 公式捕获所有在任一方向上被固定的位（必须为 0 或必须为 1 的位），符合 SDM §26.3.1.1。

Read Shadow 改为直接存储原始 `Cr0`（不再做 `Cr0 & Fixed0`），使 Guest 读取 CR0 时看到原始值。

- CR0 Guest-Host Mask = `Cr0Fixed0 ^ Cr0Fixed1`
- CR0 Read Shadow = `Cr0`（原始值）

**CR4 Guest-Host Mask（Audit #2 修正）:**

原实现仅以 `CR4_VMXE` 作为掩码，只隐藏 VMXE 位，忽略了其他被 VMX 约束的 CR4 位。修正后同样读取 `Cr4Fixed0` 和 `Cr4Fixed1`，使用 `(Cr4Fixed0 ^ Cr4Fixed1) | CR4_VMXE` 作为掩码，覆盖 VMX 约束的所有位加上 VMXE 隐藏需求。

- CR4 Guest-Host Mask = `(Cr4Fixed0 ^ Cr4Fixed1) | CR4_VMXE`
- CR4 Read Shadow = `Cr4 & ~CR4_VMXE`

#### 阶段 4: VM-Exit Controls
```
HOST_ADDR_SPACE_SIZE | SAVE_IA32_EFER | LOAD_IA32_EFER
```
- MSR store/load counts = 0

#### 阶段 5: VM-Entry Controls
```
IA32E_MODE_GUEST | LOAD_IA32_EFER
```
- MSR load counts = 0
- Entry interrupt info = 0

#### 阶段 6: Guest State

完整保存所有段寄存器（CS/SS/DS/ES/FS/GS/TR/LDTR）的选择子、基址、界限、访问权限。其中 FS/GS 基址通过 MSR 读取。

控制寄存器（CR0/CR3/CR4）、描述符表（GDTR/IDTR）、调试寄存器（DR7）、RFLAGS 全部保存。

MSRs：DEBUGCTL、EFER、SYSENTER_CS/ESP/EIP、XSS（如果 XSAVES 启用）。

Activity State = 0（Active），Interruptibility = 0，Pending Debug Exceptions = 0，VMCS Link Pointer = 0xFFFFFFFFFFFFFFFF。

#### 阶段 7: Host State

段选择子：RPL 清零（`& 0xFFF8`），确保 CPL=0。

Host RSP：栈顶 16 字节对齐后减 8（模拟压入了返回地址，使 handler 中 sub rsp,N 后对齐）。

Host RIP：指向 `AsmVmxExitHandler`（汇编入口）。

### `VmxEnableOnCpu()`

- **签名**: `static NTSTATUS VmxEnableOnCpu(PVMX_CPU_CONTEXT CpuCtx, PVMX_STATE State)`
- **功能**: 在单个 CPU 上启用 VMX
- **流程**:
  1. 保存原始 CR4
  2. 设置 CR4.VMXE
  3. 按 CR0 Fixed0/Fixed1 调整 CR0
  4. 执行 `__vmx_on()` VMXON
- **返回值**: STATUS_SUCCESS 或 STATUS_UNSUCCESSFUL

### `VmxDisableOnCpu()`

- **签名**: `static VOID VmxDisableOnCpu(PVMX_CPU_CONTEXT CpuCtx)`
- **功能**: 在单个 CPU 上禁用 VMX
- **注意**: 仅在 `VmcsLaunched == FALSE` 时执行 `__vmx_off()`，否则假定 ASM 关闭路径已执行

### `VmxInitDpcRoutine()`

- **签名**: `static VOID VmxInitDpcRoutine(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)`
- **功能**: DPC 例程，在目标 CPU 上执行完整的初始化序列
- **流程**:
  1. 调用 `VmxEnableOnCpu` → VMXON
  2. 调用 `VmxSetupVmcs` → VMCS 配置
  3. 调用 `AsmVmxLaunch()` → VMLAUNCH
  4. 成功后设置 `VmcsLaunched = TRUE`，设置 DPC 事件
- **返回值**: 通过 DpcCtx.Status 传递

### `VmxTerminateDpcRoutine()`

- **签名**: `static VOID VmxTerminateDpcRoutine(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)`
- **功能**: DPC 例程，在目标 CPU 上执行 VMX 关闭
- **流程**:
  1. 如果 `VmcsLaunched`，执行 `AsmVmxVmcall2(VMCALL_MAGIC_SHUTDOWN, g_VmcallShutdownNonce)`
  2. ASM 路径执行 vmxoff 后返回，这里恢复 CR4
  3. 如果从未 Launch，直接调用 `VmxDisableOnCpu`

### `VmxInitialize()`

- **签名**: `NTSTATUS VmxInitialize(PVMX_STATE State)`
- **功能**: 全局 VMX 初始化的主入口
- **详细流程**:

| 步骤 | 操作 |
|------|------|
| 1 | 检查重复初始化 |
| 2 | 执行 `KeQueryActiveProcessorCount`，设置 `g_MaxProcessors` |
| 3 | 动态分配 `CpuContexts` 数组 |
| 4 | 调用 `VmxCheckCapabilities` 检测能力 |
| 5 | 为每个 CPU 调用 `VmxAllocateCpuContext` 分配内存 |
| 6 | **MSR 预探测**: 在 VMXON 之前探测无效 MSR（VMX root 模式下 SEH 不可靠） |
| 7 | **EPT 全局初始化**: `EptInitialize()` |
| 8 | **预分配 HV_CPU_CONTEXT 数组**: 修复多 CPU 竞争条件（Bug #1） |
| 9 | **Per-CPU EPT 初始化**: `EptInitPerCpu()`（非致命，失败则回退到共享 EPT） |
| 10 | **通过 DPC 逐个 CPU 启用 VMX**: 使用 `KeInsertQueueDpc` + `KeWaitForSingleObject` 串行化 |
| 11 | DPC 等待使用 1 秒轮询 + 60 秒超时，超时时安全撤销 |
| 12 | 注册 AAD-BP 异常拦截回调 `ProcessRegisterExceptionHideToggle` |

- **错误路径**: `InitFailed` 标签，有序撤销：已 Launch 的 CPU 发送 VMCALL 关闭 → 释放上下文 → EPT 清理 → MSR 位图清理

### Exception Bitmap 同步函数组

这个子系统允许在 Guest 运行时动态修改 Exception Bitmap，无需跨 CPU IPI：

- `VmxExcBmpEnsureInit()`: 单次初始化自旋锁和 per-CPU 代数数组
- `VmxExcBmpSet(BitMask, Enable)`: 修改全局期望值，递增代数计数器
- `VmxSetExceptionInterceptDb(BOOLEAN)`: 设置 #DB 拦截（值为 EXCEPTION_BITMAP_DB）
- `VmxSetExceptionInterceptBp(BOOLEAN)`: 设置 #BP 拦截（值为 EXCEPTION_BITMAP_BP）
- `VmxSyncExceptionBitmap()`: 在 VM-Exit handler 入口调用，比较当前 CPU 代数，落后则执行 VMWRITE

### `VmxTerminate()`

- **签名**: `VOID VmxTerminate(PVMX_STATE State)`
- **功能**: 全局 VMX 终止
- **流程**: 通过 DPC 逐 CPU 发送 VMCALL 关闭 → EPT 清理 → 释放所有内存 → MSR 位图清理 → Exception Bitmap 状态重置

### HV_OPS vtable 实现

`g_VmxOps` 结构体包含所有 VMX 后端函数指针：

| 函数 | 对应操作 |
|------|----------|
| `VmxOpsReadGuestRip` | `VmxRead(VMCS_GUEST_RIP)` |
| `VmxOpsWriteGuestRip` | `VmxWrite(VMCS_GUEST_RIP, V)` |
| `VmxOpsReadGuestRsp` | `VmxRead(VMCS_GUEST_RSP)` |
| `VmxOpsReadGuestCr3` | `VmxRead(VMCS_GUEST_CR3)` |
| `VmxOpsReadExitReason` | `VmxRead(VMCS_EXIT_REASON)` |
| `VmxOpsAdvanceGuestRip` | VmxAdvanceGuestRip() 内联函数 |
| `VmxOpsInjectException` | 组装中断信息字段写入 `VMENTRY_INT_INFO` |
| `VmxOpsSetupPageTables` | `EptInitialize()` |
| `VmxOpsHookFunction` | `EptHookFunction()` |
| `VmxOpsEnableSingleStep` | 设置 `PROC_BASED_MONITOR_TRAP_FLAG` |
| `VmxOpsWriteTscOffset` | 写入 `VMCS_CTRL_TSC_OFFSET`，注意负值处理 |
| `VmxOpsGetCurrentCpuContext` | 从 `g_VmxHvCtx` 数组获取当前 CPU 上下文 |

---

## 4. 控制流与逻辑流程

### 初始化序列

```
DriverEntry
  └── VmxInitialize (via g_HvOps->Initialize)
       ├── VmxCheckCapabilities
       ├── VmxAllocateCpuContext (per CPU)
       ├── MsrProbeInvalidMsrs (预探测，必须在 VMXON 之前)
       ├── EptInitialize
       ├── EptInitPerCpu (per-CPU EPT 隔离，可选)
       └── For each CPU:
            ├── KeInitializeDpc(VmxInitDpcRoutine)
            ├── HvSetTargetProcessorDpc
            ├── KeInsertQueueDpc
            └── KeWaitForSingleObject (1s polling loop, 60s timeout)
                 └── VmxInitDpcRoutine:
                      ├── VmxEnableOnCpu (VMXON)
                      ├── VmxSetupVmcs
                      └── AsmVmxLaunch
```

### 终止序列

```
VmxTerminate (via g_HvOps->Terminate)
  └── For each CPU with VmcsLaunched:
       ├── DPC: VmxTerminateDpcRoutine
       │    └── AsmVmxVmcall2(VMCALL_MAGIC_SHUTDOWN, nonce)
       │         └── ASM handler: vmxoff → return
       └── Restore CR4
  ├── EptCleanupPerCpu / EptCleanup
  ├── VmxFreeCpuContext (per CPU)
  └── MsrCleanupInvalidBitmap
```

### 关键条件分支

- **VM-Entry 失败**: VmLanchResult != 0 → VmxDisableOnCpu → InitFailed
- **DPC 超时 60 秒**: 尝试 `KeRemoveQueueDpc`，如失败则无限等待（防止栈释放后 DPC 仍执行）
- **CR0 Fixed0/Fixed1 调整**: 每次 Guest 写 CR0 时应用，确保 VM-Entry 不失败
- **Exception Bitmap 同步**: 通过代数比较实现每个 CPU 独立的延迟写入

### 错误处理路径

`InitFailed` 标签的撤销顺序：
1. 已 Launch 的 CPU：DPC 发送 VMCALL_SHUTDOWN → ASM vmxoff
2. 已 VMXON 但未 Launch 的 CPU：直接 `VmxDisableOnCpu` (__vmx_off)
3. 从未 VMXON 的 CPU：仅释放内存
4. EPT 全局清理
5. MSR 位图清理
6. HV_CPU_CONTEXT 数组释放

---

## 5. 与其他模块的交互

### 通过 hv_ops vtable 的交互

`vmx_init.c` 定义了 `g_VmxOps` 结构体并填充所有函数指针。`g_HvOps` 全局指针被设置为 `&g_VmxOps`（在 `vmxdrv.c` 中基于 CPU vendor 决定）。

所有其他模块（`vmx_exit.c`、`hv_mem.c`、`process.c` 等）均通过 `HvReadGuestRip()` 等宏或 `g_HvOps->HookFunction()` 调用 VMX 功能。

### 与 EPT 模块的交互

- `VmxSetupVmcs` 调用 `EptSetupIdentityMap` 配置 EPT Pointer
- `VmxInitialize` 调用 `EptInitialize()` 建立全局身份映射
- `VmxInitialize` 调用 `EptInitPerCpu()` 建立 per-CPU EPT 隔离
- `VmxTerminate` 调用 `EptCleanupPerCpu()` + `EptCleanup()`

### 与 MSR 模块的交互

- 调用 `MsrProbeInvalidMsrs()` 在 VMXON 之前预探测无效 MSR
- 调用 `MsrCleanupInvalidBitmap()` 在终止时清理

### 与 Process 模块的交互

- 初始化完成后注册 `VmxSetExceptionInterceptBp` 作为异常隐藏切换回调

---

## 6. 关键设计要点

### Blue Pill 架构

- **不请求** `PIN_BASED_EXTERNAL_INT_EXIT`：外部中断直接通过 Guest IDT 传递，Hypervisor 完全透明
- 只请求 `NMI_EXIT`：支持 WinDbg Ctrl+Break 和 NMI 阻塞语义
- VMX 信息通过 CPUID 隐藏（由 `AadHandleCpuid` 处理）

### Host 栈设计

- 32KB 每 CPU 的 `NonPagedPool` 分配，远大于典型 VM-Exit 处理栈使用量（2-4KB）
- 16 字节对齐确保 x64 ABI 兼容
- 栈顶减 8 模拟"已压入返回地址"，使 handler 中 `sub rsp, N` 后满足 `RSP % 16 == 0`

### CR0/CR4 Guest-Host Mask 策略

- **CR0 Mask = Fixed0 ^ Fixed1（Audit #1 修正）**: 原使用 Fixed0 仅覆盖 must-be-0 位，但 must-be-1 位（如 PE、PG、NE）的写入不经拦截会导致下一次 VM-Entry 失败。修正后 XOR 捕获所有 VMX 约束位。Read Shadow 直接存储原始 Cr0，不做调整。
- **CR4 Mask = (Fixed0 ^ Fixed1) | VMXE（Audit #2 修正）**: 原仅使用 VMXE 隐藏位，忽略其他 CR4 约束位。修正后覆盖所有 CR4 约束位加上 VMXE 隐藏需求。
- **SDM 依据**: 两项修正均依据 Intel SDM Vol. 3C §26.3.1.1，该节要求 Guest CR0/CR4 满足 FIXED0（must-be-0）和 FIXED1（must-be-1）双重约束。

### MSR 预探测 (Pre-Probe)

**关键设计决策**: 在 VMXON 之前（此时 SEH 正常工作）探测所有 MSR，建立无效 MSR 位图。VMXON 之后，SEH 在 VMX root 模式下不可靠（Host 栈不是线程内核栈）。预探测位图避免了 RDMSR/WRMSR handler 中需要 SEH。

### Exception Bitmap 延迟同步 (AAD-BP)

**设计模式**: 全局期望值 + per-CPU 代数计数器 + 在 VM-Exit handler 入口惰性同步。

优点：
- 无需跨 CPU IPI
- 无 VMCS 所有权问题（每个 CPU 只写自己的 VMCS）
- 快速路径仅为一个代数比较（无 VMWRITE）

### 竞争条件修复

- **Bug #1**: HV_CPU_CONTEXT 数组预分配而非惰性分配——消除了多 CPU 在第一次 VM-Exit 时同时分配导致的竞态
- **Bug #8**: 使用 `KeSetTargetProcessorDpcEx`（支持 >127 CPU）替代 `KeSetTargetProcessorDpc`（CCHAR 限制）
- **DPC 超时处理**: 60 秒超时时安全撤销 + 防止栈释放后 DPC 仍执行的保护

### VMCALL 关闭认证 (M-6)

- 使用 per-boot 随机 nonce（`g_VmcallShutdownNonce`）
- 关闭请求必须同时在 RAX 携带 magic 值、在 RCX 携带 nonce
- 验证：CPL=0, CS.L=1 (64位模式), EFER.LMA=1, RIP 在内核地址空间
- 防止 Guest 内恶意驱动卸载 Hypervisor
