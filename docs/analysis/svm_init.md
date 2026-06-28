# svm_init.c / svm.h — 逻辑分析

## 1. 文件概述

- **角色与职责**: `svm_init.c` 是 AMD SVM 后端的初始化模块，负责 VMCB（Virtual Machine Control Block）的配置、SVM 的每个 CPU 启用/禁用，以及将 SVM 后端注册到全局的 `HV_OPS` vtable 中。它是整个 AMD 虚拟化后端的入口点。
- **svm.h 的角色**: 定义了所有 SVM 相关的常量（退出码、拦截位、MSR 地址）、数据结构（VMCB 控制区/保存区、CPU上下文、全局状态）以及函数声明。是 AMD SVM 后端的核心头文件。
- **依赖的其他模块**:
  - `hv_ops.h` — 超管理器抽象层，`g_SvmOps` 实现了 `HV_OPS` vtable
  - `hv_detect.h` / `hv_detect.c` — CPU 厂商检测和 SVM 能力探测
  - `npt.h` / `npt.c` — Nested Page Tables (AMD 版 EPT)
  - `log.h` / `log.c` — 日志系统
  - `process.h` / `process.c` — 进程跟踪（用于 AAD_HIDE_EXCEPTIONS 回调）
  - `vmx.h` — 借用了段寄存器读取的 ASM 辅助函数声明
  - `ept.h` — 复用了 EPT 页表结构体定义（EPT_PTE, EPT_PDE 等）

---

## 2. 数据结构

### VMCB 相关结构体（svm.h）

#### `VMCB_SEG` — VMCB 段寄存器格式
```c
typedef struct _VMCB_SEG {
    USHORT  Selector;    // 段选择子
    USHORT  Attrib;      // 16位 SVM 段属性（Type/S/DPL/P/AVL/L/D_B/G）
    ULONG   Limit;       // 段限长
    ULONG64 Base;        // 段基址
} VMCB_SEG;
```
与 Intel VMCS 的段格式不同，SVM 使用更紧凑的 16 位属性（见 `SvmGetSegmentAttrib()` 的实现）。

#### `VMCB_CONTROL_AREA` — VMCB 控制区（偏移 0x000-0x3FF）
关键字段：
- `InterceptCr`（偏移 0x000）: CR 读写拦截位图
- `InterceptDr`（偏移 0x004）: DR 读写拦截位图
- `InterceptExceptions`（偏移 0x008）: 异常拦截位图
- `Intercept`（偏移 0x00C）: 指令拦截位图（64位），涵盖 CPUID/MSR/VMMCALL/VMRUN/NMI 等
- `IopmBasePa`（偏移 0x040）: I/O 权限映射表物理地址
- `MsrpmBasePa`（偏移 0x048）: MSR 权限映射表物理地址
- `TscOffset`（偏移 0x050）: TSC 偏移量
- `Asid`（偏移 0x058）: Address Space ID（类似 Intel VPID）
- `TlbCtl`（偏移 0x05C）: TLB 控制
- `IntCtl`（偏移 0x060）: 中断控制
- `IntState`（偏移 0x068）: 中断状态（NMI 屏蔽等）
- `ExitCode`（偏移 0x070）: #VMEXIT 退出码
- `ExitInfo1/2`（偏移 0x078/0x080）: 退出信息
- `ExitIntInfo` / `ExitIntInfoErr`（偏移 0x088/0x08C）: 退出中断信息
- `NestedCtl`（偏移 0x090）: 嵌套分页控制
- `EventInj` / `EventInjErr`（偏移 0x0A8/0x0AC）: 事件注入
- `NestedCr3`（偏移 0x0B0）: NPT 根页表物理地址
- `CleanBits`（偏移 0x0C0）: VMCB 干净位优化
- `NextRip`（偏移 0x0C8）: 下一条 RIP（NRIPS）
- `InsnLen` / `InsnBytes`（偏移 0x0D0）: 指令长度和字节

#### `VMCB_SAVE_AREA` — VMCB 保存区（偏移 0x400-0xFFF）
包含全部段寄存器（Es/Cs/Ss/Ds/Fs/Gs/Gdtr/Ldtr/Idtr/Tr）、控制寄存器（Cr0/Cr2/Cr3/Cr4）、调试寄存器（Dr6/Dr7）、Rflags、Rip/Rsp/Rax、以及 SYSCALL/SYSRET MSR（Star/Lstar/Cstar/Sfmask）、SYSENTER_* MSR、PAT、DBGCTL 等。

#### `VMCB` — 完整 VMCB
```c
typedef struct _VMCB {
    VMCB_CONTROL_AREA   Control;    // 0x000 - 0x3FF
    VMCB_SAVE_AREA      Save;       // 0x400 - 0xFFF
} VMCB;
```
`C_ASSERT(sizeof(VMCB) <= 0x1000)` 确保 4KB 页面容纳。

### SVM 上下文结构体

#### `SVM_CPU_CONTEXT` — 每 CPU 的 SVM 上下文
```c
typedef struct _SVM_CPU_CONTEXT {
    HV_CPU_CONTEXT  Common;          // 通用超管理器上下文（必须是第一个成员，用于类型转换）
    PVMCB           VmcbVa;          // VMCB 虚拟地址
    ULONG64         VmcbPa;          // VMCB 物理地址
    PVOID           HostSaveAreaVa;  // 硬件管理的 Host Save Area（MSR_VM_HSAVE_PA 指向）
    ULONG64         HostSaveAreaPa;  // Host Save Area 物理地址
    PVMCB           HostVmcbVa;      // 软件管理的 Host VMCB（用于 VMSAVE/VMLOAD）
    ULONG64         HostVmcbPa;      // Host VMCB 物理地址
    PVOID           MsrpmVa;         // MSR 权限映射表虚拟地址（8KB）
    ULONG64         MsrpmPa;         // MSRPM 物理地址
    PVOID           HostStackBase;   // 主机栈基址
    SIZE_T          HostStackSize;   // 主机栈大小
    ULONG           Asid;            // 此 vCPU 的 ASID
    ULONG64         OriginalEfer;    // 原始 EFER（用于恢复）
} SVM_CPU_CONTEXT;
```

**关键设计说明**:
- `HostSaveAreaVa/Pa` 是 CPU 硬件自动保存/恢复的（由 MSR_VM_HSAVE_PA 指向），但只保存 CR3/RFLAGS/RAX/RSP/RIP 和 CS/SS/DS/ES
- `HostVmcbVa/Pa` 是软件显式通过 VMSAVE/VMLOAD 管理的，保存 FS/GS/TR/LDTR 基址和系统调用 MSR
- 两者必须分开（不能共用同一页），否则 VMRUN 的硬件自动保存会覆盖 VMSAVE 保存的内容

#### `SVM_STATE` — 全局 SVM 状态
```c
typedef struct _SVM_STATE {
    PSVM_CPU_CONTEXT CpuContexts;    // 动态分配的每 CPU 上下文数组 [g_MaxProcessors]
    ULONG           CpuCount;        // CPU 数量
    BOOLEAN         Initialized;     // 初始化完成标志
    PVOID           IopmVa;          // 全局 IOPM（12KB = 3 页）
    ULONG64         IopmPa;          // IOPM 物理地址
    ULONG           SvmRevision;     // SVM 修订版
    ULONG           MaxAsid;         // 最大 ASID
    BOOLEAN         NptSupported;           // NPT 支持
    BOOLEAN         NripSaveSupported;      // Next RIP Save 支持
    BOOLEAN         VmcbCleanSupported;     // VMCB Clean Bits 支持
    BOOLEAN         FlushByAsidSupported;   // Flush by ASID 支持
    BOOLEAN         DecodeAssistSupported;  // Decode Assists 支持
} SVM_STATE;
```

### `GUEST_CONTEXT`（vmx.h 中定义）
```c
typedef struct _GUEST_CONTEXT {
    ULONG64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    ULONG64 R8, R9, R10, R11, R12, R13, R14, R15;
} GUEST_CONTEXT;
```
这是在 ASM VMRUN 循环中保存/恢复的通用寄存器上下文，与 Intel VMX 侧共用同一结构体。

---

## 3. 核心函数详解

### `SvmIsSupported()` — SVM 能力检测
- **签名**: `BOOLEAN SvmIsSupported(VOID)`
- **功能**: 检查当前 CPU 是否支持 AMD SVM
- **核心逻辑**: 委托给 `HvCheckSvmSupport()`（在 `hv_detect.c` 中实现，通过 CPUID 0x80000001:ECX[2] 检测）

### `SvmCheckCapabilities()` — 内部能力验证
- **签名**: `static BOOLEAN SvmCheckCapabilities(VOID)`
- **功能**: 通过 CPUID 0x8000000A 读取 SVM 能力和特性标志
- **核心流程**:
  1. 执行 `__cpuid(CpuInfo, SVM_CPUID_FUNC)` 即 0x8000000A
  2. EAX[7:0] = SVM 修订版 → `g_SvmState.SvmRevision`
  3. EBX = 最大 ASID → `g_SvmState.MaxAsid`
  4. EDX 位域解析特性：
     - 位 0 = NPT（嵌套分页）
     - 位 3 = NRIP Save（下一 RIP 保存）
     - 位 5 = VMCB Clean Bits（干净位优化）
     - 位 6 = Flush by ASID（ASID 冲刷）
     - 位 7 = Decode Assists（解码辅助）
  5. 验证：MaxAsid > 0，NPT 必须支持
- **返回值**: TRUE 表示能力满足要求

### `SvmAllocateContiguous()` — 连续物理内存分配
- **签名**: `static PVOID SvmAllocateContiguous(SIZE_T Size, ULONG64 *PhysicalAddress)`
- **功能**: 分配物理连续、页对齐的缓存，并返回虚拟地址和物理地址
- **核心流程**:
  1. 调用 `MmAllocateContiguousMemorySpecifyCache()` 分配
  2. 使用 `MmGetPhysicalAddress()` 获取物理地址
  3. 清零分配的内存
- **关键点**: 使用页边界（BoundaryAddr=0x1000）对齐，确保 VMCB/IOPM/MSRPM 需要页对齐的硬件要求

### `SvmAllocateCpuContext()` — 每 CPU 资源分配
- **签名**: `static NTSTATUS SvmAllocateCpuContext(PSVM_CPU_CONTEXT CpuCtx)`
- **功能**: 为单个 CPU 分配 VMCB、HostSaveArea、HostVmcb、MSRPM、主机栈
- **核心流程**:
  1. 分配 4KB 页对齐的 VMCB（`SvmAllocateContiguous(PAGE_SIZE, ...)`）
  2. 分配 4KB HostSaveArea（由 MSR_VM_HSAVE_PA 指向）
  3. 分配 4KB HostVmcb（用于 VMSAVE/VMLOAD，与 HostSaveArea **分开**）
  4. 分配 8KB MSRPM 并初始化（`SvmInitMsrpm()`）
  5. 分配 16KB 主机栈（NonPagedPool）
  6. 计算 ASID = ProcessorNumber + 1（ASID 0 保留给宿主机）

### `SvmFreeCpuContext()` — 每 CPU 资源释放
- **功能**: 逆序释放 VMCB、HostSaveArea、HostVmcb、MSRPM、主机栈

### `SvmAllocateGlobalResources()` — 全局资源分配
- **签名**: `static NTSTATUS SvmAllocateGlobalResources(VOID)`
- **功能**: 分配 12KB I/O Permission Map（全零 = 不拦截任何 I/O 端口）

### `SvmMsrpmSetBit()` — MSRPM 位设置
- **签名**: `static VOID SvmMsrpmSetBit(PVOID Msrpm, ULONG Msr, BOOLEAN Read, BOOLEAN Write)`
- **功能**: 在 8KB MSRPM 中设置指定 MSR 的读/写拦截位
- **MSRPM 布局**:
  - 偏移 0x0000: MSR 0x00000000-0x00001FFF（4KB = 8192 个 MSR 的 2 位）
  - 偏移 0x0800: MSR 0xC0000000-0xC0001FFF
  - 偏移 0x1000: MSR 0xC0010000-0xC0011FFF
- 每个 MSR 使用 2 位：位 0 = 读拦截，位 1 = 写拦截

### `SvmInitMsrpm()` — MSRPM 初始化
- **签名**: `static VOID SvmInitMsrpm(PVOID Msrpm)`
- **功能**: 初始化 MSR 权限映射表，拦截特定 MSR 用于虚拟化隐藏
- **拦截的 MSR**:
  - `IA32_DEBUGCTL` (0x1D9) — 调试控制
  - `IA32_FEATURE_CONTROL` (0x3A) — VMXON 控制
  - Intel VMX MSRs (0x480-0x491) — 嵌套虚拟化探测
  - `MSR_VM_CR` (0xC0010114) — SVM 配置
  - `MSR_VM_HSAVE_PA` (0xC0010117) — 嵌套 SVM 探测

### `SvmGetSegmentAttrib()` — 段属性转换
- **签名**: `static USHORT SvmGetSegmentAttrib(ULONG64 GdtBase, USHORT Selector)`
- **功能**: 将 GDT 描述符转换为 SVM 16 位段属性格式
- **SVM 属性格式**: `[3:0]Type [4]S [6:5]DPL [7]P [8]AVL [9]L [10]D/B [11]G`
- **核心逻辑**: 从 GDT 中读取 `Access` 字节和 `LimitHighAndFlags` 的高半部分，合并为 16 位属性

### `SvmInitVmcb()` — VMCB 配置
- **签名**: `static VOID SvmInitVmcb(PSVM_CPU_CONTEXT CpuCtx)`
- **功能**: 初始化 VMCB 的全部控制区和保存区字段
- **核心流程**:

  **控制区配置**:
  1. 清零整个 VMCB 页面
  2. **CR 拦截**: 拦截 CR0 写、CR3 写（进程切换检测）、CR4 写
  3. **DR 拦截**: 拦截所有 DR0-DR7 读写（用于反反调试 DR 欺骗）
  4. **异常拦截**: 默认不拦截（C-3 修复）。通过 `SvmSetExceptionInterceptDb/Bp` 动态启用
  5. **指令拦截**: CPUID、MSR_PROT(MSR)、VMMCALL、INVD、WBINVD、XSETBV、VMRUN、VMLOAD、VMSAVE、STGI、CLGI、SKINIT、NMI
     - **不拦截 HLT**（C-2 修复，允许 CPU 进入 C1/C2 空闲状态）
     - **不拦截 INTR**（Audit #1 修复：被拦截的物理中断在 AMD SVM 上不会自动确认，会导致 VMEXIT 风暴。详见下方关键设计要点）
     - **不拦截 RDTSC/RDTSCP**（使用硬件 TSC Offset 机制）
  6. 设置 IOPM/MSRPM 基地址
  7. TSC Offset = 0
  8. ASID = 每 CPU 分配的 ASID
  9. TLB 控制 = 首次进入时冲刷全部 ASID
  10. 中断控制 = 启用虚拟中断屏蔽（`V_INTR_MASKING_MASK`）
  11. **NPT 配置**: 如支持 NPT，设置 `NestedCtl` 启用位，设置 `NestedCr3` 为 NPT 根页表地址（优先使用每 CPU 独立页表）

  **保存区配置**:
  1. 读取当前宿主机的 GDT/IDT 基址和限长
  2. 读取当前宿主机的段选择子（CS/SS/DS/ES/FS/GS/TR/LDTR）
  3. 填充所有段寄存器（`VMCB_SEG` 格式）：选择子、属性（从 GDT 转换）、限长（0xFFFFFFFF）、基址（FS/GS 从 MSR 获取）
  4. 填充 GDTR/IDTR（未使用 Segment 格式的 Attrib 和 Limit，仅 Base/Limit）
  5. 解析 TR 的 64 位 GDT 条目（包括 BaseUpper 字段）
  6. 控制寄存器：Cr0/Cr3/Cr4/Cr2
  7. 调试寄存器：Dr7/Dr6
  8. Rflags
  9. EFER（强制加入 `EFER_SVME` 位）
  10. 系统 MSR：STAR/LSTAR/CSTAR/SFMASK/KernelGsBase/SYSENTER_CS/ESP/EIP
  11. PAT、DEBUGCTL
  12. Cpl = 0（Ring 0）

### `SvmEnableOnCpu()` — 启用 SVM
- **签名**: `static NTSTATUS SvmEnableOnCpu(PSVM_CPU_CONTEXT CpuCtx)`
- **功能**: 在当前 CPU 上启用 SVM 功能
- **核心流程**:
  1. 保存原始 EFER
  2. 设置 `EFER.SVME`（位 12）
  3. 设置 `MSR_VM_HSAVE_PA` 指向 HostSaveArea
  4. 标记 `HvEnabled = TRUE`

### `SvmDisableOnCpu()` — 禁用 SVM
- **签名**: `static VOID SvmDisableOnCpu(PSVM_CPU_CONTEXT CpuCtx)`
- **功能**: 禁用 SVM，恢复原始 EFER 并清除 HSAVE_PA

### `SvmInitDpcRoutine()` — 初始化 DPC 例程
- **签名**: `static VOID SvmInitDpcRoutine(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)`
- **功能**: 在指定 CPU 上通过 DPC 执行 SVM 初始化和 VMRUN
- **核心流程**:
  1. 获取当前 CPU 的 SVM 上下文
  2. 调用 `SvmEnableOnCpu()` 启用 SVM
  3. 调用 `AsmSvmLaunch(VmcbPa, VmcbVa, HostVmcbPa)` 发起 VMRUN
  4. `AsmSvmLaunch` 的行为：
     - 首次：VMSAVE HostVmcbPa（捕获真实主机状态）
     - 设置 VMCB.Save.Rsp/Rip → 设置着陆垫
     - VMRUN → 客户机运行
     - 客户机通过 `_SvmLaunchGuest` 返回
     - 后续 VMEXIT：VMLOAD HostVmcbPa（恢复主机）→ 调用 SvmExitHandler → VMLOAD VmcbPa（加载客户机额外状态）→ VMRUN
  5. 设置事件通知调用者完成
- **返回值由事件传递**: `DpcCtx->Status`

### `SvmTerminateDpcRoutine()` — 终止 DPC 例程
- **签名**: `static VOID SvmTerminateDpcRoutine(...)`
- **功能**: 在指定 CPU 上通过 VMMCALL 通知退出循环
- **核心流程**:
  1. 发送 `VMMCALL(VMCALL_MAGIC_SHUTDOWN, g_VmcallShutdownNonce)`
  2. VMEXIT 处理函数识别关机 magic，返回 FALSE 退出 VMRUN 循环
  3. ASM 代码执行 STGI 禁用 SVM
  4. 调用 `SvmDisableOnCpu()`

### `SvmInitialize()` — SVM 全局初始化
- **签名**: `NTSTATUS SvmInitialize(VOID)`
- **功能**: 完整的 SVM 初始化入口
- **核心流程**:
  1. 检查重复初始化 → `STATUS_ALREADY_REGISTERED`
  2. 清零全局状态，读取 CPU 数量
  3. 初始化异常拦截自旋锁
  4. 检查 SVM 能力
  5. 分配全局资源（IOPM）
  6. 初始化 NPT（`NptInitialize()`）
  7. 初始化每 CPU NPT 结构（非致命，失败则回退到共享 NPT）
  8. 分配 `CpuContexts` 数组
  9. **预探测无效 MSR**（`MsrProbeInvalidMsrs()`），在 VMRUN 前 SEH 仍然可靠
  10. 为每个 CPU 分配上下文并初始化 VMCB
  11. **通过 DPC 逐 CPU 初始化**（串行化，等待每个完成）：
      - 使用 `KeSetTargetProcessorDpcEx` 支持 >127 CPU
      - 每 CPU 带超时等待（最多 60 秒，每秒检查）
      - 超时后尝试 `KeRemoveQueueDpc` 清理
  12. 标记初始化完成
  13. 注册异常拦截回调（`ProcessRegisterExceptionHideToggle`）
- **错误路径**:
  - 使用 `goto SvmInitFailed` 统一清理
  - 清理顺序：先终止已启动的 CPU 的客户机模式 → 释放上下文 → NPT 清理 → MSR 位图清理 → 全局资源

### `SvmTerminate()` — SVM 终止
- **签名**: `VOID SvmTerminate(VOID)`
- **功能**: 完整的 SVM 终止处理
- **核心流程**:
  1. 通过 DPC 逐 CPU 发送关机 VMMCALL
  2. NPT 清理（`NptCleanupPerCpu()` → `NptCleanup()`）
  3. 释放所有 CPU 上下文
  4. MSR 位图清理
  5. 释放全局资源（IOPM）

### HV_OPS 实现函数（`svm_init.c` 后半部分）

所有 `SvmOps*` 函数构成了 `g_SvmOps` vtable 的实现。这些函数通过全局 `g_SvmState` 访问当前 CPU 的 VMCB。

| 函数 | 功能 | 对应 HV_OPS 字段 |
|------|------|------------------|
| `SvmOpsReadGuestRip()` | 读取客户机 RIP（来自 VMCB.Save.Rip） | `ReadGuestRip` |
| `SvmOpsWriteGuestRip()` | 写入客户机 RIP | `WriteGuestRip` |
| `SvmOpsReadGuestRsp()` | 读取客户机 RSP | `ReadGuestRsp` |
| `SvmOpsReadGuestCr3()` | 读取客户机 CR3 | `ReadGuestCr3` |
| `SvmOpsReadGuestRflags()` | 读取客户机 RFLAGS | `ReadGuestRflags` |
| `SvmOpsReadExitReason()` | 读取退出码（VMCB.Control.ExitCode） | `ReadExitReason` |
| `SvmOpsReadExitQualification()` | 读取退出限定（使用 ExitInfo1） | `ReadExitQualification` |
| `SvmOpsReadExitInstructionLength()` | 读取指令长度（NRIP Save 或 DecodeAssist） | `ReadExitInstructionLength` |
| `SvmOpsReadGuestPhysicalAddress()` | 读取客户机物理地址（NPF 时用 ExitInfo2） | `ReadGuestPhysicalAddress` |
| `SvmOpsAdvanceGuestRip()` | 推进 RIP（使用 NextRip 或指令长度） | `AdvanceGuestRip` |
| `SvmOpsInjectException()` | 注入异常/中断到客户机（配置 EventInj） | `InjectException` |
| `SvmOpsWriteTscOffset()` | 写硬件 TSC Offset | `WriteTscOffset` |

**关键差异**:
- SVM 没有像 VMX 那样的单独"退出限定"字段，使用 `ExitInfo1` 作为主退出限定
- SVM 使用 `EventInj` 字段注入事件（不是 VM-entry interruption info field）
- SVM 的单步通过设置 RFLAGS.TF + #DB 拦截模拟，没有 VMX 的 Monitor Trap Flag

### 异常拦截动态管理

#### `SvmInterceptLockInitialize()`
- 初始化保护 g_SvmInterceptDbRequested / g_SvmInterceptBpRequested 的自旋锁

#### `SvmApplyExceptionIntercepts()`
- 读取两个请求标志，合并 #DB（位 1）和 #BP（位 3）
- 对所有 CPU 的 VMCB 原子写入 `InterceptExceptions`
- 清除 `CleanBits` 的位 0（INTERCEPTS），强制下次 VMRUN 重载

#### `SvmSetExceptionInterceptDb()` / `SvmSetExceptionInterceptBp()`
- 由 NPT 钩子引擎（#DB）和反反调试子系统（#BP）调用
- 支持独立启用/禁用

---

## 4. 控制流与逻辑流程

### 初始化流程
```
DriverEntry (vmxdrv.c)
  → HvDetectCpuVendor() 检测 CPU 厂商
  → 如为 AMD: g_HvOps = &g_SvmOps
  → g_HvOps->Initialize()  →  SvmInitialize()
       ├─ SvmCheckCapabilities()    检查 CPU 能力和特性
       ├─ SvmAllocateGlobalResources()  分配 IOPM
       ├─ NptInitialize()              初始化 NPT 页表
       ├─ NptInitPerCpu()              初始化每 CPU NPT
       ├─ MsrProbeInvalidMsrs()        预探测无效 MSR
       └─ [对每个 CPU] 循环:
            ├─ SvmAllocateCpuContext()   分配 VMCB/MSRPM/栈
            ├─ SvmInitVmcb()             配置 VMCB
            └─ DPC → SvmInitDpcRoutine()
                 ├─ SvmEnableOnCpu()         设置 EFER.SVME
                 └─ AsmSvmLaunch()           执行 VMRUN 进入客户机
```

### 异常拦截动态启用流程
```
NptHookFunction() 安装钩子
  → SvmSetExceptionInterceptDb(TRUE)
     → 加锁 g_SvmInterceptLock
     → 设置 g_SvmInterceptDbRequested = TRUE
     → SvmApplyExceptionIntercepts()
         → 遍历所有 CPU，设置 VMCB.InterceptExceptions |= 位1 (#DB)
         → 清除 CleanBits 位0
     → 解锁

AAD 启用 AAD_HIDE_EXCEPTIONS
  → ProcessRegisterExceptionHideToggle 回调
  → SvmSetExceptionInterceptBp(TRUE)
     → 同理设置位3 (#BP)
```

### 终止流程
```
g_HvOps->Terminate() → SvmTerminate()
  → [对每个 CPU] DPC → SvmTerminateDpcRoutine()
       ├─ VMMCALL(VMCALL_MAGIC_SHUTDOWN, nonce)
       └─ SvmDisableOnCpu()  清除 EFER.SVME
  → NptCleanupPerCpu() + NptCleanup()
  → 释放所有 CPU 上下文
  → SvmFreeGlobalResources()
```

---

## 5. 与其他模块的交互

### 通过 HV_OPS vtable 的交互

`g_SvmOps` 在 `svm_init.c` 末尾定义并初始化，在 `DriverEntry` 中赋值给 `g_HvOps`。之后所有其他模块（如 `anti_anti_debug.c`, `hv_hook.c`, `hv_mem.c`）通过 `HvReadGuestRip()` 等宏间接调用，解析为 `g_HvOps->ReadGuestRip()` 即 `SvmOpsReadGuestRip()`。

### 与 NPT 模块的交互

- `SvmInitialize()` 调用 `NptInitialize()` 和 `NptInitPerCpu()` 初始化 NPT
- `SvmInitVmcb()` 调用 `NptGetPerCpuRootPa()` / `NptGetRootPageTablePa()` 获取 NPT 根地址，填入 `VMCB.Control.NestedCr3`
- `SvmTerminate()` 调用 `NptCleanupPerCpu()` / `NptCleanup()`

### 与反反调试模块的交互

- 注册 `SvmSetExceptionInterceptBp` 回调到进程跟踪子系统
- 在 CR3 写拦截中调用 `AadUpdateHwTscOffset()` 更新 TSC Offset

### 与进程跟踪模块的交互

- 通过 `ProcessRegisterExceptionHideToggle()` 注册回调，在添加/移除 AAD_HIDE_EXCEPTIONS 目标时自动启用/禁用 #BP 拦截

---

## 6. 关键设计要点

### ASID 与 TLB 管理

- ASID（Address Space ID）类似 Intel 的 VPID，允许不同地址空间的 TLB 条目共存
- ASID 0 保留给宿主机，客户机 ASID 从 1 开始（ProcessorNumber + 1）
- 首次 VMRUN 使用 `TLB_CONTROL_FLUSH_ALL_ASID`，后续使用 `TLB_CONTROL_DO_NOTHING`
- NPT TLB 冲刷通过设置 `TlbCtl` 字段，在下次 VMRUN 时生效

### VMCB Clean Bits 优化

- AMD VMCB Clean Bits 是一个优化机制：VMRUN 可以跳过重新加载标记为"干净"的 VMCB 区域
- 代码精细地只清除 `CleanBits` 的位 0（INTERCEPTS 区域），而不是清零整个 `CleanBits`
- 这避免了在其他区域（寻呼、段、ASID、TPR 等）上强制不必要的重载

### Audit #1 修复：不拦截 INTR（物理中断）

INTR（物理中断）不再被拦截。此修复基于 AMD APM Vol.2 第 15.7 节的硬件行为差异：

- **AMD SVM 的行为**：当 `Intercept` 位图中设置了 `SVM_INTERCEPT_INTR` 位时，所有物理中断在递交给客户机之前都会触发 VMEXIT。然而，与 Intel VMX 不同的是，AMD SVM **不会自动确认**被拦截的中断——中断在 LAPIC 中仍保持挂起状态。结果是，每次 VMRUN 后中断立即再次触发 VMEXIT，形成 `VMRUN → INTR 待处理 → 立即 VMEXIT → VMRUN → ...` 的死循环，导致系统完全无法使用（100% CPU 消耗在 VMEXIT 处理上）。

- **与 Intel VMX 对齐**：Intel VMX 侧同样未设置 `PIN_BASED_EXTERNAL_INT_EXIT`。外部中断直接通过客户机 IDT 传递，无需超管理器参与（Blue Pill 设计理念）。

- **影响**：超管理器不再接收物理中断的 VMEXIT。这意味着所有中断处理（时钟中断、设备中断等）完全由客户机直接处理，不影响被虚拟化的系统。这种设计对于 Type-2（Blue Pill）超管理器是合理的，因为超管理器本身不提供独立的设备虚拟化。

### Host Save Area 的双重设计

这是 AMD SVM 特有的复杂性：
1. **硬件自动保存**（HostSaveArea）：CPU 在 VMRUN 时自动保存 CR3/RFLAGS/RAX/RSP/RIP/CS/SS/DS/ES 到 HSAVE 区域。由 `MSR_VM_HSAVE_PA` 指向，完全由 CPU 管理。
2. **软件显式保存**（HostVmcb）：通过 VMSAVE 指令保存 FS/GS/TR/LDTR 基址和系统调用 MSR（STAR/LSTAR/...），通过 VMLOAD 恢复。

**VMRUN 循环协议**:
- 首次：VMSAVE HostVmcbPa（捕获真实宿主机状态）
- 每次 VMEXIT：VMLOAD HostVmcbPa（恢复宿主机额外状态）
- 每次 VMRUN（非首次）：VMLOAD VmcbPa（加载客户机额外状态），然后 VMRUN

没有 HostVmcb 的分割设计会导致宿主机 FS/GS（KPCR）/TR（TSS）/LSTAR（系统调用入口）被客户机值污染，必然 BSOD。

### 与 Intel VMX 的设计对比

| 特性 | Intel VMX | AMD SVM |
|------|-----------|---------|
| 控制结构 | VMCS（4KB，通过 VMREAD/VMWRITE 访问） | VMCB（4KB，固定偏移直接内存访问） |
| 退出信息 | VM_EXIT_REASON + EXIT_QUALIFICATION | ExitCode + ExitInfo1/ExitInfo2 |
| 指令长度 | VM_EXIT_INSTRUCTION_LENGTH | NextRip − Rip（NRIP Save）或 InsnLen |
| 事件注入 | VM-Entry interruption-information field | EventInj 控制字段 |
| TLB 管理 | INVEPT + VPID | TlbCtl（VMRUN 时冲刷）+ ASID |
| 单步 | Monitor Trap Flag（VM-execution control） | RFLAGS.TF + #DB 拦截 |
| 页表根 | EPTP（EPT pointer in VMCS） | NestedCr3（VMCB 控制区） |
| 页表结构 | 专用 EPT 格式（支持 Execute-Only） | 使用 x86-64 相同页表格式（不支持 Execute-Only） |
| 段属性 | VMCS 自有格式（32位访问权限） | 16 位紧凑格式（从 GDT 解析） |
| 宿主机状态保存 | 硬件自动保存全部宿主机状态 | 硬件仅保存部分，需软件补全（VMSAVE/VMLOAD） |
| CAP 探测 | CPUID.1:ECX[5] + IA32_FEATURE_CONTROL | CPUID 0x80000001:ECX[2] + CPUID 0x8000000A |

### MSR 预探测修复（与 VMX 共享）

在 VMRUN 之前执行 `MsrProbeInvalidMsrs()`，此时 `__try/__except` SEH 仍然可靠。VMRUN 之后宿主机运行在 SVM root 模式下 SEH 行为不可靠。预探测的位图用于 RDMSR/WRMSR 客户机退出处理中注入 #GP，而无需执行真实指令。
