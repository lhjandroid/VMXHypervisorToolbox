# vmx_exit.c — 逻辑分析

## 1. 文件概述

### 角色与职责

`vmx_exit.c` 是 Intel VT-x VM-Exit 主分发器和各个退出原因的具体处理模块。当 Guest 执行敏感指令或发生特定事件时，CPU 从 VMX non-root 模式退出到 VMX root 模式，`AsmVmxExitHandler`（汇编入口）保存 Guest 寄存器上下文后调用 `VmxExitHandler`，后者根据退出原因分发到对应处理函数。

职责范围：
1. **主分发器**：`VmxExitHandler` 读取 `VMCS_EXIT_REASON`，通过 switch 语句分发到 20+ 个特定 Handler
2. **各个退出原因的处理**：CPUID、RDMSR/WRMSR、CR/DR 访问、异常/NMI、EPT 违例/误配置、VMCALL 超调用、I/O 指令、HLT、MTF 等
3. **IDT-vectoring 事件重注入**：因 EPT 违例等中断事件传递过程中断时，在 VM-Exit 尾部重注入
4. **VMRESUME 失败处理**：当 VMRESUME 执行失败时的诊断路径
5. **GP 寄存器读写辅助**：通过 GUEST_CONTEXT 结构体按索引读写 GP 寄存器

### 依赖的其他模块

| 模块 | 用途 |
|------|------|
| `vmx.h` | VmxRead/VmxWrite 内联、VMCS 字段编码、常量、GUEST_CONTEXT |
| `ept.c/h` | `HandleEptViolation`、EPT 相关操作和 MTF 跟踪 |
| `hv_ops.h` | 宏定义间接访问，但本文件主要直接使用 VmxRead/VmxWrite |
| `hv_mem.h` | 超调用内存操作（已废弃，仅保留告警） |
| `log.h` | 日志输出 |
| `process.h` | 进程跟踪相关 |
| `anti_anti_debug.h` | 反反调试引擎（`AadHandleCpuid`, `AadHandleDrAccess`, `AadHandleException`） |
| `msr.c` | MSR 读写处理 (`HandleRdmsrImpl`, `HandleWrmsrImpl`) |
| `shared.h` | 共享常量和结构体 |

---

## 2. 数据结构

### `GUEST_CONTEXT` (定义于 vmx.h)

VM-Exit 时保存的 Guest 通用寄存器上下文，与 `vmx_asm.asm` 中的布局严格一致：

```c
typedef struct _GUEST_CONTEXT {
    ULONG64 Rax;    // 0x00
    ULONG64 Rcx;    // 0x08
    ULONG64 Rdx;    // 0x10
    ULONG64 Rbx;    // 0x18
    ULONG64 Rsp;    // 0x20 — 占位符，实际值从 VMCS_GUEST_RSP 读取
    ULONG64 Rbp;    // 0x28
    ULONG64 Rsi;    // 0x30
    ULONG64 Rdi;    // 0x38
    ULONG64 R8;     // 0x40
    ULONG64 R9;     // 0x48
    ULONG64 R10;    // 0x50
    ULONG64 R11;    // 0x58
    ULONG64 R12;    // 0x60
    ULONG64 R13;    // 0x68
    ULONG64 R14;    // 0x70
    ULONG64 R15;    // 0x78
} GUEST_CONTEXT;
```

布局优化：RSP 现在直接在 ASM 存根中通过 `vmread VMCS_GUEST_RSP` 设置，无需在 C handler 中额外 VmxRead。Handler 返回前将可能修改的 RSP 写回 VMCS。

---

## 3. 核心函数详解

### `VmxExitHandler()` — 主分发器

- **签名**: `BOOLEAN VmxExitHandler(PGUEST_CONTEXT GuestContext)`
- **功能**: VM-Exit 主处理函数，从 ASM 存根调用
- **参数**: `GuestContext` — 保存的 Guest 寄存器指针
- **返回值**: `TRUE` = 继续运行 Guest (VMRESUME)，`FALSE` = 关闭 VMX

#### 核心流程

| 步骤 | 操作 |
|------|------|
| 1 | 读取 `VMCS_EXIT_REASON` 和当前 CPU 编号 |
| 2 | 递增 per-CPU 退出计数器（release 模式用普通自增，diagnostic 模式用 InterlockedIncrement64） |
| 3 | `EptCheckPendingInvept()` — 检查并执行待处理的 EPT TLB 刷新 |
| 4 | `VmxSyncExceptionBitmap()` — 惰性同步 Exception Bitmap |
| 5 | 检查 VM-Entry 失败位 (bit 31)，如是则返回 FALSE 关闭 VMX |
| 6 | switch 分发处理 |
| 7 | **IDT-vectoring 重注入**：检查 `VMCS_IDT_VECTORING_INFO`，若有效且当前未注入事件，重注入被中断的事件。**Audit #6**：跳过外部中断 (IntType == INTERRUPT_TYPE_EXTERNAL) 的重注入——外部中断已被 LAPIC 确认消费，重新注入类型 0 (External Interrupt) 会导致 VM-Entry 一致性检查失败 (SDM §27.2.4) |
| 8 | 写回 RSP 到 VMCS |
| 9 | 返回 TRUE/FALSE |

#### 快速路径

- `VMX_EXIT_DIAGNOSTICS` 条件编译控制诊断日志
- Release 模式：仅执行必要的 VMREAD 和递增计数器
- Diagnostic 模式：心率检测（每100/10000次退出）、前30次详细日志、快速退出风暴检测（>10000次/秒关闭）

#### 退出原因分发表

| 退出原因 | 处理函数 | 关键行为 |
|----------|----------|----------|
| CPUID (10) | `HandleCpuid` | 委托给 `AadHandleCpuid`（隐藏 Hypervisor、返回真实 CPU 信息） |
| RDMSR (31) | `HandleRdmsr` | 委托给 `HandleRdmsrImpl`（MSR 白名单/黑名单过滤） |
| WRMSR (32) | `HandleWrmsr` | 委托给 `HandleWrmsrImpl` |
| CR Access (28) | `HandleCrAccess` | CR3 写入监测进程切换、CR0/CR4 固定位调整（含 Audit #1/#2 FIXED0/FIXED1 修正）、CR 读取隐藏 VMXE、CLTS 同步 ReadShadow（Audit #3）、LMSW 固定位调整 |
| DR Access (29) | `HandleDrAccess` | 委托给 `AadHandleDrAccess`（隐藏硬件断点） |
| Exception/NMI (0) | `HandleException` | NMI 立即重注入或通过 NMI-window 延迟注入；#BP/#DB 委托给 `AadHandleException` |
| EPT Violation (48) | `HandleEptViolation` | 委托给 `HandleEptViolation` (ept.c) |
| EPT Misconfig (49) | `HandleEptMisconfig` | 记录错误并返回 FALSE 关闭 VMX |
| MTF (37) | `HandleMtf` | 禁用 MTF，恢复 per-CPU 的 EPT hook 页面权限 |
| VMCALL (18) | `HandleVmcall` | 超调用分发：关闭、内存操作（已废弃）、未知调用注入 #UD |
| XSETBV (55) | `HandleXsetbv` | 验证 XCR0 值后执行 XSETBV，非法值注入 #GP(0)。含 Audit #4 保留位检查 |
| INVD (13) | `HandleInvd` | 转化为 WBINVD（写回+失效）执行 |
| INVLPG (14) | `HandleInvlpg` | 执行实际 INVLPG |
| WBINVD (54) | `HandleWbinvd` | 执行实际 WBINVD |
| Triple Fault (2) | `HandleTripleFault` | 记录完整上下文后关闭 VMX |
| HLT (12) | 内联 | 推进 RIP，条件允许时设置 Activity State = HLT |
| INVPCID (58) | 内联 | 执行 INVVPID ALL_CONTEXTS，推进 RIP |
| I/O Instruction (30) | 内联 | 模拟 IN/OUT 指令（直接执行，非 string 时） |
| VMX 指令系列 (19-27,50,53) | 内联 | 注入 #UD（隐藏 Hypervisor 存在） |
| 其他 | 内联 | 循环检测，3次重复同一退出 → 关闭 VMX |

### `VmxResumeFailedHandler()` — VMRESUME 失败处理

- **签名**: `VOID VmxResumeFailedHandler(ULONG64 VmInstructionError)`
- **功能**: 当 VMRESUME 失败时从 ASM 路径调用
- **逻辑**: 读取 Guest RIP/RSP 和 Exit Reason，通过 VMXROOT_LOG_ERROR 记录诊断信息
- **重要**: 不使用 `__try/__except`（SEH 在 VMX root 模式不可靠）

### `HandleCpuid()` — CPUID 处理

- **签名**: `static BOOLEAN HandleCpuid(PGUEST_CONTEXT Ctx)`
- **功能**: 委托给 `AadHandleCpuid`，后者根据 AAD_HIDE_CPUID 标志决定是否隐藏 Hypervisor

### `HandleCrAccess()` — CR 寄存器访问

- **签名**: `static BOOLEAN HandleCrAccess(PGUEST_CONTEXT Ctx)`
- **功能**: 处理 CR 寄存器访问导致的 VM-Exit
- **核心逻辑**:

**MOV to CR3** (进程切换):
- 写入 `VMCS_GUEST_CR3`
- 调用 `AadUpdateHwTscOffset` 更新 TSC 偏移（进程切换后重置时序补偿）
- 记录前 5 次 CR3 切换日志

**MOV to CR0**:
- 保存原始值到 CR0 Read Shadow（使 Guest 读取 CR0 时看到自己写入的值）
- 应用 CR0 Fixed0/Fixed1 调整后写入 `VMCS_GUEST_CR0`

**MOV to CR4（Audit #2b 修正）**:
- 读取 `MSR_IA32_VMX_CR4_FIXED0/FIXED1` 调整写入值，确保满足 VM-Entry 约束
- 额外保持 VMXE 位设置（`ActualCr4 = NewValue | CR4_VMXE`）
- Read Shadow 写入 Guest 原始值（不带 VMXE，对 Guest 隐藏 VMX 存在）
- 依据 SDM §26.3.1.1（同 CR0 修正模式）

**MOV from CR3/CR0/CR4**: 从 VMCS 读取相应字段返回 Guest，CR4 返回隐藏 VMXE 的值

**CLTS（Audit #3 修正）**:
- 清除 CR0.TS 位后同步写入 `VMCS_CTRL_CR0_READ_SHADOW`，确保 Guest 读取 CR0 时看到正确的 TS 位状态
- 原实现仅更新 `VMCS_GUEST_CR0`，未同步 ReadShadow，导致 Guest 读到过期的 TS=1 值

**LMSW**: 加载机器状态字（低16位 CR0），保持 PE 位，应用 Fixed0/Fixed1

### `HandleDrAccess()` — DR 寄存器访问

- **签名**: `static BOOLEAN HandleDrAccess(PGUEST_CONTEXT Ctx)`
- **功能**: 委托给 `AadHandleDrAccess` 处理调试寄存器隐藏

### `HandleException()` — 异常处理

- **签名**: `static BOOLEAN HandleException(PGUEST_CONTEXT Ctx)`
- **功能**: 处理异常和 NMI VM-Exit
- **NMI 处理**:
  1. 读取中断信息和 NMI 阻塞状态
  2. 如未被 NMI 阻塞 → 立即通过 `VMENTRY_INT_INFO` 重注入
  3. 如被 NMI 阻塞 → 设置 `PROC_BASED_NMI_WINDOW_EXIT`，NMI 窗口开启时自动注入
  4. 如 Guest 处于 HLT 状态 → 唤醒 (Activity State = 0)
- **其他异常**: 委托给 `AadHandleException`

### `HandleEptViol()` / `HandleEptMisconfig()` — EPT 处理

- `HandleEptViol`: 委托给 `HandleEptViolation` (ept.c)
- `HandleEptMisconfig`: 记录错误后返回 FALSE（致命）

### `HandleMtf()` — Monitor Trap Flag 处理

- **签名**: `static BOOLEAN HandleMtf(PGUEST_CONTEXT Ctx)`
- **功能**: EPT hook 单步完成后的恢复处理
- **核心逻辑**:
  1. 禁用 MTF（清除 `PROC_BASED_MONITOR_TRAP_FLAG`）
  2. 获取当前 CPU 松弛的页面物理地址 (`EptMtfGetAndClearRelaxedPage`)
  3. **O(1) 路径**: 通过 `EptFindHookByPhysicalAddress` 哈希表查找
  4. 恢复 PTE：清除 R/W 位，设置执行位，指向 Hook 页
  5. **回退路径**: RelaxedPa=0 时执行 O(n) 线性扫描全部 hooks
  6. 使用 `INVEPT SINGLE_CONTEXT`（per-CPU EPT 时）或 `ALL_CONTEXTS`
- **多核竞争修复**: 每个 CPU 只恢复自己松弛的页面的 hook，不触及其他 CPU 的状态

### `HandleVmcall()` — VMCALL 超调用

- **签名**: `static BOOLEAN HandleVmcall(PGUEST_CONTEXT Ctx)`
- **功能**: 处理 VMCALL 指令，实现 Hypervisor 与 Guest 之间的通信
- **安全认证 (M-6)**:
  1. 检查 CPL（非 Ring 0 → 注入 #UD）
  2. 关闭请求需通过 `HvIsAuthenticShutdownCaller` 验证：
     - RCX = `g_VmcallShutdownNonce`（per-boot 随机数）
     - CS.L = 1（64位模式）
     - EFER.LMA = 1
     - RIP 在用户地址空间外
     - CPL = 0
- **多命令格式**:
  - 旧格式：`RAX == VMCALL_MAGIC_SHUTDOWN` (0xDEADCAFE)
  - 新格式：`RAX 高16位 == VMCALL_MAGIC`，低16位为子命令
    - `VMCALL_SUBCMD_SHUTDOWN`: 关闭 VMX（需认证）
    - `VMCALL_SUBCMD_READ_MEMORY` / `WRITE_MEMORY`: 已废弃，注入 #UD
    - 默认: 未知命令 → 注入 #UD

### `HandleXsetbv()` — XSETBV 处理

- **签名**: `static BOOLEAN HandleXsetbv(PGUEST_CONTEXT Ctx)`
- **功能**: 处理 XSETBV 指令（XCR 寄存器写入）
- **验证（含 Audit #4 修正）**:
  1. ECX 必须为 0（只允许写 XCR0）
  2. XCR0 bit 0 (x87) 必须为 1
  3. 如果 bit 2 (AVX) 置位，bit 1 (SSE) 必须同时置位
  4. **保留位检查**：使用保守掩码（bits 0-7 + bit 9）检查保留位，非法位 → 注入 #GP(0)。
     SDM Vol.1 §13.3 要求写入 XCR0 的保留位将触发 #GP。原实现在 Host 上下文执行 XSETBV，
     若 Guest 写入非法值则导致 Hypervisor 自身 #GP 崩溃。
- 非法值 → 注入 #GP(0)
- 合法值 → 通过 `AsmXsetbv` 执行实际写入

### `HandleInvd()` / `HandleInvlpg()` / `HandleWbinvd()`

- **INVD**: 转换为 WBINVD（写回缓存再失效，比单纯 INVD 更安全）
- **INVLPG**: 从 Exit Qualification 读取线性地址，执行 `__invlpg`
- **WBINVD**: 直接执行 `__wbinvd`

### `HandleTripleFault()` — 三重故障

- **签名**: `static BOOLEAN HandleTripleFault(PGUEST_CONTEXT Ctx)`
- **功能**: 记录完整的故障上下文后返回 FALSE 关闭 VMX
- **记录信息**: RIP, RSP, CR3, CS, RFLAGS, Activity State, Interruptibility, IDT-vectoring, Entry/Exit Interruption Info

### `GetGpRegValue()` / `SetGpRegValue()` — GP 寄存器辅助

- **签名**: `static ULONG64 GetGpRegValue(PGUEST_CONTEXT Ctx, ULONG RegIndex)` / `SetGpRegValue(PGUEST_CONTEXT Ctx, ULONG RegIndex, ULONG64 Value)`
- **功能**: 按索引（0-15 对应 RAX-R15）读写 GUEST_CONTEXT 中的寄存器值
- **实现**: 将 GUEST_CONTEXT 指针视为 ULONG64 数组，索引直接对应寄存器偏移
- **RSP (索引4)**: 现在可以正常工作，因为 RSP 在 VM-Exit 时已从 VMCS 同步到 GuestContext

---

## 4. 控制流与逻辑流程

### VM-Exit 处理完整流程

```
Guest 执行 → VM-Exit 发生
  ↓
AsmVmxExitHandler (汇编)
  ├── 保存 Guest GP 寄存器到堆栈 (push/pushfq)
  ├── vmread VMCS_GUEST_RSP → [RSP slot]
  ├── 切换 RSP 到 Host 栈
  └── call VmxExitHandler
       ↓
VmxExitHandler (C)
  ├── ExitCount++
  ├── EptCheckPendingInvept()
  ├── VmxSyncExceptionBitmap()
  ├── VM-Entry failure? → return FALSE
  ├── switch(ExitReason):
  │   ├── CPUID → HandleCpuid → AadHandleCpuid
  │   ├── CR_ACCESS → HandleCrAccess → CR3/CR0/CR4/CLTS/LMSW
  │   ├── EPT_VIOLATION → HandleEptViol → HandleEptViolation (ept.c)
  │   ├── MTF → HandleMtf → restore hook permissions
  │   ├── VMCALL → HandleVmcall → auth + dispatch
  │   ├── ... (20+ handlers)
  │   └── default → loop detection
  ├── IDT-vectoring reinjection check (含 Audit #6 外部中断过滤)
  ├── Write back Guest RSP
  └── return result
       ↓
AsmVmxExitHandler (汇编)
  ├── Result == TRUE → VMRESUME
  ├── Result == FALSE → VmxShutdown (vmxoff → restore → ret)
  └── VMRESUME failed → VmxResumeFailedHandler → vmxoff → hlt
```

### 关键条件分支

**VM-Entry 失败检测** (bit 31 of Exit Reason):
- 设置 `VmcsLaunched = FALSE`, `VmxEnabled = FALSE`
- 返回 FALSE → ASM 路径执行 vmxoff

**IDT-vectoring 重注入**:
- 仅当 `VMENTRY_INT_INFO` 的 Valid 位未设置时才重注入
- 包含错误码和指令长度的重注入（对软件中断/异常必需）
- **Audit #6**: 跳过外部中断类型的 IDT-vectoring 事件。外部中断已被 LAPIC 确认消费，不能作为有效事件重注入（中断类型 0 不是 VM-Entry 允许的注入类型，SDM §26.6.1）

**HLT 活动状态转换**:
- 需要符合 Intel SDM §26.3.1.5 的所有约束：
  - RFLAGS.IF = 1
  - 无 STI/MOV-SS 阻塞
  - 无待处理调试异常

**无限循环检测**:
- 同一退出原因 + 同一 RIP 连续出现 3 次 → 判定为无限循环 → 返回 FALSE 关闭 VMX

**NMI 延迟注入**:
- 被 NMI 阻塞时通过 NMI-window 退出机制延迟注入

### I/O 指令模拟

非字符串 IN/OUT 指令直接在 VMX root 模式执行：
- `__inbyte/__inword/__indword` 对应 8/16/32 位读取
- `__outbyte/__outword/__outdword` 对应写入
- 字符串 I/O (INS/OUTS) 暂不实现完整模拟（仅推进 RIP）

**重要**: 不使用 `__try/__except`。在 VMX root 模式，SEH 不可靠（Host 栈不是线程内核栈，异常注册链无效）。

---

## 5. 与其他模块的交互

### 反反调试引擎 (anti_anti_debug.c/h)

通过以下三个委派调用紧密协作：
- `HandleCpuid` → `AadHandleCpuid()` — CPUID 结果过滤
- `HandleDrAccess` → `AadHandleDrAccess()` — 调试寄存器隐藏
- `HandleException` → `AadHandleException()` — #BP/#DB 异常规范化

### EPT 模块 (ept.c)

- `HandleEptViol` → `HandleEptViolation()` — EPT 违例处理
- `HandleMtf` 中大量使用 ept.c 的 API：
  - `EptMtfGetAndClearRelaxedPage()` — 获取本 CPU 松弛的页面
  - `EptFindHookByPhysicalAddress()` — O(1) 哈希查找
  - `EptGetPerCpuPte()` — 获取 per-CPU PTE
  - `EptGetPerCpuEptp()` — 获取 per-CPU EPTP 用于 INVEPT SINGLE
  - `EptInvalidateSingleContext()` / `EptInvalidateAllContexts()`

### MSR 模块 (msr.c)

- `HandleRdmsr` → `HandleRdmsrImpl()` — MSR 读取过滤
- `HandleWrmsr` → `HandleWrmsrImpl()` — MSR 写入过滤

### Exception Bitmap 同步 (vmx_init.c)

`VmxSyncExceptionBitmap()` 在 VM-Exit handler 入口被调用，此函数定义于 `vmx_init.c`。

### EPT TLB 刷新协作

- `EptCheckPendingInvept()` 在每个 VM-Exit 入口执行
- 当 `EptInvalidateFromGuest()` 在 Guest 中被调用时，递增全局代数计数器
- 每个 CPU 在下次 VM-Exit 时检测到代数落后 → 执行 INVEPT

---

## 6. 关键设计要点

### 无条件退出风暴防护

通过 I/O 位图（全零）覆盖 PROC_BASED_UNCONDITIONAL_IO_EXIT 的强制位，避免 I/O 退出风暴。诊断模式下有退出速率检测（10000次/秒快速检测）。

### 执行 - 写 切换模型（Mode A/B）

EPT hook 的两种模式在 `vmx_exit.c` 的 `HandleMtf` 和 `HandleEptViol` 中协作：

**Mode A** (Execute-Only 支持):
- 执行页：R=0, W=0, X=1 → 执行触发 hook
- 数据访问：临时切换为 R=1, W=1, X=0（原始页）→ MTF 恢复

**Mode B** (无 Execute-Only 支持):
- 所有页：R=0, W=0, X=0 → 所有访问都违例
- 通过 Guest RIP 判断意图：
  - RIP 在 hook 页 → 执行 → 展示 hook 页 + MTF
  - RIP 不在 hook 页 → 数据访问 → 展示原始页 + MTF

### IDT-vectoring 事件重注入

当 VM-Exit 发生在中断/异常通过 IDT 传递过程中时（例如 EPT 违例发生在 #PF 传递中途），VMCS 的 IDT-vectoring 信息字段记录被中断的事件。Handler 必须在尾部重注入此事件，否则 Guest 丢失该中断/异常，导致内存损坏或设备停滞。

重注入条件：
1. `VMCS_IDT_VECTORING_INFO.Vaild = 1`
2. `VMCS_CTRL_VMENTRY_INT_INFO.Valid = 0`（handler 未设置其他注入）
3. **Audit #6**: 中断类型不为外部中断 (`IntType != INTERRUPT_TYPE_EXTERNAL`)。外部中断已被 LAPIC 确认消费，重新注入类型 0 会导致 VM-Entry 一致性检查失败 (SDM §27.2.4)

### 不加 SEH 的设计决策

VMX root 模式下的两个关键位置明确不使用 SEH：
- `HandleIoAccess`(IN/OUT 模拟)
- `VmxResumeFailedHandler`

原因：Host 栈是 `ExAllocatePoolWithTag` 分配的独立缓冲区，不是 Windows 线程内核栈。SEH 通过遍历线程栈上的 `_EXCEPTION_REGISTRATION_RECORD` 链工作，但在栈外缓冲区上，此链不存在或被零填充，触发 SEH 会导致 double fault。

### VMCALL 超调用认证

多层防御：
1. CPL 检查（非 Ring 0 → #UD）
2. Magic 值检查（RAX）
3. Per-boot nonce 检查（RCX）
4. 执行模式检查（CS.L, EFER.LMA）
5. 地址范围检查（RIP 在内核空间）

### 性能优化

- **RSP 直接同步**: Guest RSP 在 ASM 存根中通过 VMREAD 直接写入 GUEST_CONTEXT，省去 C 代码中的额外 VmxRead
- **条件编译诊断**: `VMX_EXIT_DIAGNOSTICS` 宏控制详细日志，生产版本完全消除开销
- **Per-CPU 计数器**: 非诊断模式下使用简单自增（单写入者），不使用原子操作
- **MTF O(1) 恢复**: 哈希表查找代替线性扫描
- **INVEPT SINGLE_CONTEXT**: per-CPU EPT 启用时只刷新本 CPU 的 TLB

### HLT 处理

关键决策：不直接在 VMX root 模式下执行 `HLT`（会导致中断在 Host 栈上处理，引发栈溢出和状态损坏），而是通过设置 `VMCS_GUEST_ACTIVITY_STATE = 1 (HLT)` 让 CPU 在 Guest 模式下进入 HLT。外部中断到达时自动 VM-Exit，handler 恢复 Activity State 后 VMRESUME。
