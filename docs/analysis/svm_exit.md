# svm_exit.c — 逻辑分析

## 1. 文件概述

- **角色与职责**: `svm_exit.c` 是 AMD SVM 后端的 #VMEXIT 分发器，负责根据 VMCB 中的退出码路由到对应的处理函数。它是 AMD 虚拟化后端的运行时核心。
- **依赖的其他模块**:
  - `svm.h` — SVM 数据结构和常量定义
  - `npt.h` / `npt.c` — NPT 缺页处理
  - `anti_anti_debug.h` / `anti_anti_debug.c` — 反反调试处理（CPUID、异常、DR 等）
  - `hv_ops.h` — 通过 `HvAdvanceGuestRip()` 等宏间接调用
  - `hv_mem.h` — VMCALL 子命令常量
  - `msr.c` — `HandleRdmsrImpl` / `HandleWrmsrImpl` MSR 处理
  - `process.h` / `process.c` — 目标进程判断
  - `vmx.h` — `VMCALL_MAGIC_SHUTDOWN` 等常量
  - `common/shared.h` — AAD_HIDE_HWBP 等功能标志

---

## 2. 数据结构

本文件主要使用已在 `svm_init.md` 中详细分析的 `VMCB`、`GUEST_CONTEXT`、`SVM_CPU_CONTEXT` 等结构体。

### 使用的外部函数/变量
- `AadHandleCpuid()` — CPUID 处理（反反调试）
- `AadHandleException()` — 异常处理（反反调试）
- `HandleRdmsrImpl()` / `HandleWrmsrImpl()` — MSR 读写处理（来自 `msr.c`）
- `NptHandlePageFault()` — NPT 缺页处理（来自 `npt.c`）
- `NptDbMatchesRelaxedRip()` — 判断 #DB 是否由 NPT 单步引起
- `NptDbGetAndClearRelaxedPage()` — 获取并清除松弛的页面记录
- `NptFindHookByPhysicalAddress()` / `NptGetPerCpuPte()` — NPT 钩子查询
- `IsTargetProcess()` — 检查是否目标进程
- `IsFeatureEnabled()` — 检查进程功能标志
- `AadUpdateHwTscOffset()` — 更新硬件 TSC Offset
- `HvIsAuthenticShutdownCaller()` — 关机 VMCALL 认证

---

## 3. 核心函数详解

### `SvmExitHandler()` — 主 #VMEXIT 分发器

- **签名**: `BOOLEAN SvmExitHandler(PGUEST_CONTEXT GuestContext)`
- **功能**: 从 ASM VMRUN 循环中调用，根据 VMCB.Control.ExitCode 分发到对应处理函数
- **参数**: `GuestContext` — 通用寄存器上下文指针（来自 ASM 保存）
- **返回值**: `TRUE` = 继续运行客户机，`FALSE` = 退出 VMRUN 循环（关机）
- **核心流程**:
  1. 获取当前 CPU 的 `SVM_CPU_CONTEXT` 和 `VMCB`
  2. 同步 RAX（从 VMCB.Save.Rax → GuestContext->Rax）
  3. 递增退出计数器（每 CPU，无需原子操作）
  4. 重置 TLB 控制为 "不做任何操作"（首次进入后不再需要冲刷）
  5. 根据 `ExitCode` 进行 switch 分发：

  | 退出码 | 处理函数 | 说明 |
  |--------|----------|------|
  | `SVM_EXIT_CPUID` (0x072) | `AadHandleCpuid()` | CPUID 指令拦截 |
  | `SVM_EXIT_MSR` (0x07C) | `SvmHandleMsr()` | RDMSR/WRMSR 拦截 |
  | `SVM_EXIT_VMMCALL` (0x081) | `SvmHandleVmmcall()` | VMMCALL 拦截 |
  | `SVM_EXIT_HLT` (0x078) | 推进 RIP | 防御性处理（HLT 默认不拦截） |
  | `SVM_EXIT_INVD` (0x076) | `__wbinvd()` + 推进 RIP | 缓存无效化 |
  | `SVM_EXIT_WBINVD` (0x089) | `__wbinvd()` + 推进 RIP | 缓存回写+无效化 |
  | `SVM_EXIT_XSETBV` (0x08D) | 验证后执行 XSETBV | XCR0 写验证（含 Audit #4 保留位检查） |
  | `SVM_EXIT_WRITE_CR0/3/4` | `SvmHandleCrAccess()` | CR 写拦截（Audit #5: CR0 PG=1→PE=1 一致性检查） |
  | `SVM_EXIT_READ_CR0/3/4` | `SvmHandleCrAccess()` | CR 读拦截 |
  | `SVM_EXIT_READ/WRITE_DR*` (0x020-0x037) | `SvmHandleDrAccess()` | DR 读写拦截（含 Audit #3 DR4/DR5 修复） |
  | `SVM_EXIT_EXCP_DB` (0x041) | `SvmHandleDbException()` | #DB 异常 |
  | `SVM_EXIT_EXCP_BP` (0x043) | `AadHandleException()` | #BP 异常 |
  | `SVM_EXIT_NPF` (0x400) | `NptHandlePageFault()` | NPT 缺页异常 |
  | `SVM_EXIT_INTR` (0x060) | 防御性空操作 | 物理中断（Audit #1: INTR 不再被拦截，保留 case 防止意外重启用） |
  | `SVM_EXIT_NMI` (0x061) | 延迟注入 | NMI 处理 |
  | `SVM_EXIT_IRET` (0x074) | 注入延迟的 NMI | IRET 处理 |
  | SVM 指令（VMRUN/VMLOAD/VMSAVE/STGI/CLGI/SKINIT） | 注入 #UD | 隐藏嵌套虚拟化 |
  | `SVM_EXIT_SHUTDOWN` (0x07F) | 返回 FALSE | 客户机关机事件 |
  | 默认 | 记录警告，不推进 RIP | 未处理退出码 |

  6. 同步 RAX 和 RSP 回 VMCB

### `SvmHandleMsr()` — MSR 读写处理

- **签名**: `static BOOLEAN SvmHandleMsr(PGUEST_CONTEXT Ctx)`
- **功能**: 根据 ExitInfo1 判断是 RDMSR（0）还是 WRMSR（1），委托给共享的 `HandleRdmsrImpl()` / `HandleWrmsrImpl()`
- **核心逻辑**: SVM 使用 ExitInfo1 区分 MSR 访问方向，而不是像 VMX 那样使用不同的退出码

### `SvmHandleCrAccess()` — CR 访问处理

- **签名**: `static BOOLEAN SvmHandleCrAccess(PGUEST_CONTEXT Ctx, ULONG ExitCode)`
- **功能**: 处理 CR0/CR3/CR4 的读写拦截
- **核心流程**:
  - 从 ExitInfo1[3:0] 提取通用寄存器编号
  - **CR3 写**: 更新 VMCB.Save.Cr3，调用 `AadUpdateHwTscOffset(Value)` 更新 TSC Offset 以进行进程切换检测
  - **CR0 写（Audit #5 修复）**: 应用基本一致性检查——若 PG=1（分页启用）但 PE=0（保护模式未启用），强制设置 PE=1。AMD SVM 没有 Intel 的 CR0 Fixed-Bit MSRs，但 x86-64 架构仍要求 PG=1 时 PE 必须为 1，否则客户机将立即三 fault。
  - **CR4 写**: 直接更新对应的 VMCB 保存区
  - **CR 读**: 从 VMCB 读取并向通用寄存器写入
  - 调用 `HvAdvanceGuestRip()` 推进 RIP

### `SvmHandleDrAccess()` — DR 访问处理

- **签名**: `static BOOLEAN SvmHandleDrAccess(PGUEST_CONTEXT Ctx, ULONG ExitCode)`
- **功能**: 处理 DR0-DR7 的读写拦截，根据目标进程的 AAD_HIDE_HWBP 标志决定是否返回伪造值
- **核心流程**:
  1. 从退出码计算 DR 编号和方向（读/写）
  2. 从 ExitInfo1[3:0] 提取通用寄存器编号
  3. 非目标进程：真实执行 DR 读/写（通过 `__readdr`/`__writedr`）
  4. 目标进程：
     - DR 读：返回伪造值（DR0-DR3 = 0，DR6 = DR6_DEFAULT_VALUE 即 0xFFFF0FF0，DR7 = DR7_DEFAULT_VALUE）
     - DR 写：允许真实写入（因为还需要保留其他调试器的设置），但后续读取会隐藏
  5. 推进 RIP
- **Audit #3 修复**: 添加了 DR4/DR5 的退出码支持。某些 AMD CPU 会为 DR4/DR5 生成独立的退出码（即使它们与 DR6/DR7 在硅片级别别名），缺少这些 case 标签会导致 fall-through 到 default 处理程序，不推进 RIP 造成死循环。

### `SvmHandleDbException()` — #DB 异常处理

- **签名**: `static BOOLEAN SvmHandleDbException(PGUEST_CONTEXT Ctx)`
- **功能**: 处理 #DB 异常，服务于两个目的：
  1. NPT 钩子单步完成（TF 被设置以完成写穿透）
  2. 反反调试异常处理

- **核心流程（M-4 修复版）**:
  1. 调用 `NptDbMatchesRelaxedRip(CurrentRip)` 判断当前 #DB 是否由 NPT 钩子单步引起
  2. **非本系统 #DB**（即客户机自己的调试事件）：
     - 清理松弛页面跟踪器
     - 如果是目标进程 → 委托给 `AadHandleException()`
     - 否则，设置 DR6（= 0xFFFF0FF0 | 位14 BS）然后重新注入 #DB 到客户机
     - **关键修复**: 不接管/吞噬客户机的真实调试事件
  3. **本系统 #DB**（由 NPT 页面松弛触发）：
     - 调用 `NptDbGetAndClearRelaxedPage()` 获取松弛的页面物理地址
     - 使用 O(1) 哈希表查找对应的钩子条目
     - 还原页面权限为 R+X（读+执行，不写），物理地址改回钩子页面
     - 清除 TF（RFLAGS.位8）
     - 调用 `NptInvalidateAll()` 冲刷 TLB
  4. 未找到松弛页面时的处理：
     - 目标进程 → 委托给 `AadHandleException()`
     - 非目标进程 → 重新注入 #DB（设 DR6.BS + 保留位）

- **多核竞争修复**: 每 CPU 只恢复自己松弛的页面，使用 `g_NptDbRelaxedPagePa[CpuIndex]` 的每 CPU 追踪。这避免了老代码中全局恢复所有钩子导致的多核竞争条件。

### `SvmHandleVmmcall()` — VMMCALL 处理

- **签名**: `static BOOLEAN SvmHandleVmmcall(PGUEST_CONTEXT Ctx)`
- **功能**: 处理客户机的 VMMCALL 指令
- **核心流程**:

  **安全前置检查**:
  1. 检查 CPL（必须是 0，否则注入 #UD）
  2. 检查 CS.L（必须是 64 位模式）

  **旧版关机路径**（RAX = VMCALL_MAGIC_SHUTDOWN 即 0xDEADCAFE）:
  1. 调用 `HvIsAuthenticShutdownCaller()` 验证（核对 RCX 中的 nonce、RIP 地址范围、CPL、EFER.LMA、CS.L）
  2. 认证通过 → 记录日志，推进 RIP，返回 FALSE（退出 VMRUN 循环）
  3. 认证失败 → 注入 #UD

  **新版 VMCALL 分发**（RAX 高 16 位 = VMCALL_MAGIC 即 0xCAFE0000）:
  - `VMCALL_SUBCMD_SHUTDOWN`(0x0000): 同旧版关机路径
  - `VMCALL_SUBCMD_READ_MEMORY`(0x0001) / `VMCALL_SUBCMD_WRITE_MEMORY`(0x0002): **永久禁用**（M-7 修复）。注入 #UD 并记录警告（最多 10 次）
  - 默认: 注入 #UD，记录未知 VMMCALL（最多 10 次）

- **注入 #UD 时不推进 RIP**: #UD 是故障（fault）语义，重新进入客户机时 RIP 必须仍指向 VMMCALL 指令，使客户机的 IDT 处理程序能看到故障地址

### `SvmGetGpReg()` / `SvmSetGpReg()` — 通用寄存器访问

- **签名**: `static ULONG64 SvmGetGpReg(PGUEST_CONTEXT Ctx, ULONG RegIndex)` / `static VOID SvmSetGpReg(...)`
- **功能**: 从 `GUEST_CONTEXT` 结构中按索引读取/写入通用寄存器
- **实现**: 将 `GUEST_CONTEXT` 指针视为 `ULONG64` 数组，根据索引（0-15）访问
- **注意**: RSP（索引4）现在也是有效的，因为 `SvmExitHandler()` 在入口处同步 VMCB.Save.Rsp → GuestContext->Rsp

---

## 4. 控制流与逻辑流程

### #VMEXIT 主循环（ASM + C 协同）

```
AsmSvmLaunch (svm_asm.asm)
  ├── [首次] VMSAVE HostVmcbPa
  ├── 设置 VMCB.Save.Rsp, VMCB.Save.Rip = 着陆垫
  ├── VMRUN
  ├── [客户机运行中...]
  ├── #VMEXIT 发生
  │    └── CPU 自动保存到 VMCB.Save.*
  ├── VMLOAD HostVmcbPa          (恢复宿主机)
  ├── 保存 Guest RAX/RSP 到 GuestContext
  ├── SvmExitHandler(GuestContext)  ← C 函数
  │    └── switch(ExitCode) 分发处理
  ├── 检查返回值:
  │    ├── TRUE  → VMLOAD VmcbPa, 跳转 VMRUN
  │    └── FALSE → STGI, 返回调用者
```

### #DB 判定流程（M-4 修复）
```
#DB 异常退出
  → SvmHandleDbException()
     → NptDbMatchesRelaxedRip(CurrentRip)?
        ├── FALSE (非本系统 #DB):
        │    ├── 清理跟踪器 (NptDbGetAndClearRelaxedPage)
        │    ├── 目标进程? → AadHandleException()
        │    └── 非目标 → 设 DR6=0xFFFF0FF0|BS, 重新注入 #DB
        └── TRUE (本系统 NPT 单步):
             → NptDbGetAndClearRelaxedPage()
             → O(1) 哈希查找钩子
             → 还原页面权限 R+X, 物理地址改回钩子页面
             → 清除 TF
             → NptInvalidateAll()
```

### NMI 延迟注入（Review Issue #3 修复）
```
NMI 退出
  → 检查 IntState.NmiMask (位2)
     ├── 未屏蔽: 直接注入 NMI (EventInj = VALID | TYPE_NMI | 2)
     └── 屏蔽中:
          └── 启用 IRET 拦截 (Intercept |= SVM_INTERCEPT_IRET)

稍后客户机执行 IRET:
  → SVM_EXIT_IRET 退出
  → 清除 IRET 拦截
  → 注入 NMI
```

### XSETBV 验证（Audit #4 修复）
```
XSETBV 退出
  → 验证 XCR 索引 (ECX) == 0
  → 验证位 0 (x87 FPU) 为 1
  → 验证位 2 (AVX) 要求位 1 (SSE) 也为 1
  → 验证保留位：仅允许位 0-7 和位 9（PKRU），其他位设置则注入 #GP
  → 通过 → 执行 AsmXsetbv(), 推进 RIP
  → 失败 → 注入 #GP(0)
```

---

## 5. 与其他模块的交互

### 反反调试模块（anti_anti_debug.c）

通过调用 `AadHandleCpuid()` 和 `AadHandleException()` 直接交互。SVM 退出处理与 Intel VMX 共享这些 AAD 函数，因为它们的接口使用 `GUEST_CONTEXT` 和 `HvReadGuestRip()` 等宏（由 `g_HvOps` 路由）。

### MSR 模块（msr.c）

委派 MSR 处理到共享的 `HandleRdmsrImpl` / `HandleWrmsrImpl`，它们通过 `g_HvOps` 检查客户机状态。MSRPM 的配置在 `svm_init.c` 中完成。

### NPT 模块（npt.c）

- NPF（NPT 缺页）直接委派给 `NptHandlePageFault()`
- #DB 处理中调用 `NptDbMatchesRelaxedRip()`、`NptDbGetAndClearRelaxedPage()`、`NptFindHookByPhysicalAddress()` 和 `NptGetPerCpuPte()` 用于 NPT 钩子的单步恢复
- `NptInvalidateAll()` 在 #DB 处理后恢复页面权限后冲刷 TLB

### 进程跟踪模块（process.c）

使用 `IsTargetProcess()` 和 `IsFeatureEnabled()` 检查当前客户机 CR3 对应的进程是否为受保护进程，并检查设置了哪些反反调试功能。

### HV_OPS 抽象层

- 使用 `HvAdvanceGuestRip()` 推进 RIP
- 使用 `HvInjectException()` 注入异常
- 在 VMMCALL 认证中使用 `HvIsAuthenticShutdownCaller()`

---

## 6. 关键设计要点

### C-2 修复：不拦截 HLT

以前拦截了 HLT 并简单推进 RIP，导致 HLT 对客户机成为空操作，闲置核心上的空闲线程每秒数千次调用 HLT 造成 100% CPU 使用。现在不拦截 HLT，让 CPU 进入原生 C1/C2 空闲状态。如果未来需要 HLT 拦截，必须实现 AMD APM Vol.2 第 15.9 节的等待中断协议（设置 VMCB.Control.IntState 并重新 VMRUN）。

### C-3 修复：按需启用异常拦截

#DB 和 #BP 拦截不再永久启用，改为按需启用：
- #DB：由 NPT 钩子引擎在安装钩子时启用（钩子数量 > 0），卸载后禁用
- #BP：由反反调试子系统在目标进程设置 AAD_HIDE_EXCEPTIONS 时启用
- 两个位独立 OR 进 VMCB.Control.InterceptExceptions

### M-4 修复：#DB 归属判定

在 NPT 钩子单步过程中，需要仔细区分本系统的 #DB 和客户机自己的 #DB。修复使用三要素跟踪：
- **PagePa**（非零 = 跟踪器启用）
- **Rip**（当前 RIP 必须在记录的 RIP 的 0-15 字节范围内）
- **Cr3**（必须匹配，防止进程切换后的意外匹配）

写入顺序：先写 Rip/Cr3，再写 PagePa（编译器屏障保证顺序）
读取顺序：先读 PagePa，再读 Cr3/Rip（读取屏障保证顺序）

### M-6 修复：VMMCALL 认证

关机 VMMCALL 需要认证，使用 `g_VmcallShutdownNonce`（每次驱动加载随机生成的新 nonce）。认证检查：
- CPL == 0
- RCX == nonce
- RIP 在内核地址空间
- EFER.LMA 已设置
- CS.L == 1（64 位模式）

这防止了客户机内的恶意 Ring-0 代码卸载超管理器。

### M-7 修复：废弃的内存取 VMMCALL

之前存在的 VMCALL 读/写内存路径已被永久禁用，因为它将客户机 PA 当作宿主 VA 解引用导致 BSOD。注入 #UD 强制调用者迁移到 IOCTL `KernelCopyProcessMemory` 路径。

### Review Issue #1 修复：默认处理不推进 RIP

对于未识别的退出码，不再盲目推进 RIP。非指令退出（中断、抢占定时器等）的指令长度未定义，盲目添加到 RIP 会导致客户机跳转到垃圾地址。

### Audit #1 修复：INTR 处理程序防御性注释

INTR（物理中断）的 VMEXIT 处理程序仅包含一个空操作（break），并带有防御性注释解释该退出本不应发生。这是因为 `svm_init.c` 中的 VMCB 配置不再设置 `SVM_INTERCEPT_INTR` 位（详见 svm_init.md 分析文档中的 Audit #1 说明）。保留此 case 标签是为了避免意外重新启用 INTR 拦截时导致未定义行为（fall-through 到 default 分支，同样不做任何有用处理，但不会破坏状态）。

### Audit #3 修复：添加 DR4/DR5 退出码支持

某些 AMD CPU 型号会为 DR4/DR5 生成独立的退出码（`SVM_EXIT_READ_DR4` = 0x024, `SVM_EXIT_READ_DR5` = 0x025, `SVM_EXIT_WRITE_DR4` = 0x034, `SVM_EXIT_WRITE_DR5` = 0x035），尽管这些寄存器在硅片级别与 DR6/DR7 别名。以前缺少这 4 个退出码的 case 标签，导致它们 fall-through 到 default 处理程序——而 default 不推进 RIP，造成客户机在无限循环中反复触发同一 VMEXIT。现已将这些退出码加入 DR 访问处理的分发中，与已有的 DR0-DR3/DR6-DR7 退出码一同路由到 `SvmHandleDrAccess()`。

### Audit #4 修复：XSETBV 保留位检查

重构了 XSETBV 验证逻辑，使用 goto 统一跳转到 `SvmXsetbvInjectGp` 标签注入 #GP，避免了重复的注入代码。新增保留位掩码检查：定义 `Xcr0DefinedMask` 仅包含位 0-7 和位 9（PKRU），任何在此范围外的位设置均注入 #GP(0)。此掩码与 Intel VMX 侧保持一致。验证失败时记录 `VMXROOT_LOG_WARN` 级别的日志以辅助诊断。

### Audit #5 修复：CR0 写一致性检查

在 `SVM_EXIT_WRITE_CR0` 处理程序中新增了基本一致性检查：如果客户机尝试设置 PG=1（分页启用）而 PE=0（保护模式禁用），则强制设置 PE=1。AMD SVM 没有 Intel 的 CR0 Fixed-Bit MSRs 机制，但 x86-64 架构仍要求 PG 时必须同时启用 PE，否则 VMRUN 可能成功但客户机立即三重 fault。

### 与 VMX 退出处理的差异

| 特性 | VMX (vmx_exit.c) | SVM (svm_exit.c) |
|------|-------------------|------------------|
| MSR 方向判断 | 不同退出码（RDMSR/WRMSR） | 同一退出码（SVM_EXIT_MSR），ExitInfo1=0 为读/1 为写 |
| DR 访问 | 统一退出（MOV_DR） | 独立的 16 个退出码（每 DR 读/写各一个，含 DR4/DR5） |
| 指令长度 | VMCS VM_EXIT_INSTRUCTION_LENGTH | NRIP Save 或 DecodeAssist |
| 单步机制 | Monitor Trap Flag（硬件） | RFLAGS.TF + #DB（软件模拟） |
| NMI 阻塞检测 | VM-entry interruption-info | IntState.NmiMask |
| 退出限定 | VMCS EXIT_QUALIFICATION | ExitInfo1 |
| XSETBV 地址 | Guest RIP 和指令长度 | InsnBytes 和 InsnLen |
| 事件注入字段 | VM-entry interruption-information | EventInj |
