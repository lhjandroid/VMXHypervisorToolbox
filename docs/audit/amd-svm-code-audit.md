# AMD SVM 代码合规性审计报告

> **审计依据**: AMD64 Architecture Programmer's Manual (APM) Volume 2, Chapter 15 — Secure Virtual Machine
> **审计日期**: 2026-06-28
> **审计范围**: `svm_init.c`, `svm_exit.c`, `npt.c`, `svm.h`, `hv_detect.c` (AMD 部分)
> **审计原则**: 每一项发现均附 AMD APM 具体章节号，不靠猜测和推理

---

## 审计摘要

| 严重级别 | 数量 | 说明 |
|----------|------|------|
| 🔴 **严重** | 2 | 可导致 VMEXIT 风暴 / 系统不可用 |
| 🟡 **警告** | 4 | 可能在某些条件下导致问题 |
| 🔵 **信息** | 4 | 不符合最佳实践但当前无害 |

---

## 🔴 严重发现

### 发现 1: INTR 拦截导致 VMEXIT 死循环

**文件**: `svm_init.c:555` + `svm_exit.c:212-214`
**APM 依据**: AMD APM Vol 2, §15.7 (Physical Interrupts), §15.5.1 (VMCB.Intercept layout)

**VMCB 配置**:
```c
// svm_init.c:555
Vmcb->Control.Intercept = ... |
    (1ULL << SVM_INTERCEPT_INTR) |      /* Intercept physical interrupts */
    (1ULL << SVM_INTERCEPT_NMI);
```

**VMEXIT Handler**:
```c
// svm_exit.c:212-214
case SVM_EXIT_INTR:
    /* Physical interrupt - just resume, interrupt will be delivered */
    break;
```

**问题**: AMD APM §15.7 明确指出：当 `INTR` 被拦截时，处理器执行 #VMEXIT 而不是通过 Guest IDT 投递中断。中断**不会被自动确认 (acknowledge)** — LAPIC 中的中断请求仍然挂起。VMRUN 恢复 Guest 后，挂起的中断立即再次触发 #VMEXIT → 无限循环。

APM §15.7: *"If the physical interrupt is intercepted, the processor performs a #VMEXIT rather than delivering the interrupt through the guest IDT. The hypervisor must then inject the interrupt into the guest using the V_INTR mechanism or handle it directly."*

当前处理器只 `break` 不做任何操作。后果：
- 硬件中断（时钟中断、磁盘 I/O、网卡等）全部触发 VMEXIT 风暴
- 系统 CPU 100% 消耗在 VMEXIT → break → VMRESUME → VMEXIT 循环中
- Guest 中的中断处理完全不工作 → 系统死锁

**对比 Intel VMX**: Intel 端**不设置** `PIN_BASED_EXTERNAL_INT_EXIT`，外部中断直接通过 Guest IDT 投递。AMD 端应做相同处理。

**修复建议**:
```c
// 方案 A (推荐): 不拦截 INTR，与 Intel 端对齐
// svm_init.c 第555行: 移除 (1ULL << SVM_INTERCEPT_INTR)
Vmcb->Control.Intercept =
    (1ULL << SVM_INTERCEPT_CPUID) |
    ... |
    (1ULL << SVM_INTERCEPT_NMI);   // 保留 NMI 拦截
    // 注意: 不设置 SVM_INTERCEPT_INTR

// 方案 B: 如果必须拦截，则正确注入到 Guest
case SVM_EXIT_INTR:
    Vmcb->Control.IntCtl |= V_IRQ_MASK;              // 设置虚拟中断 pending
    Vmcb->Control.IntVector = <actual interrupt vector from LAPIC>;
    break;
```

---

### 发现 2: NptInvalidateAll 未清零 VMCB Clean Bits 导致 TLB 刷新可能不生效

**文件**: `npt.c:1815-1829` (`NptInvalidateAll`)
**APM 依据**: AMD APM Vol 2, §15.5.1 (VMCB Clean Bits), §15.12.1 (TLB Control)

**当前代码**:
```c
VOID NptInvalidateAll(VOID)
{
    ...
    for (i = 0; i < g_SvmState.CpuCount; i++) {
        if (g_SvmState.CpuContexts[i].VmcbVa) {
            g_SvmState.CpuContexts[i].VmcbVa->Control.TlbCtl = TLB_CONTROL_FLUSH_ALL_ASID;
        }
    }
}
```

**问题**: AMD APM §15.5.1 定义了 VMCB Clean Bits 机制。Bit 0 控制 Intercept vectors / TSC Offset / **TLB_CONTROL** 字段的重载。当 Clean Bits bit 0 = 1 时，CPU 在 VMRUN 时**不会重新从内存读取 TlbCtl**，而是使用缓存值。

当 `NptInvalidateAll` 从 IOCTL 上下文（PASSIVE_LEVEL，非 VMEXIT handler 内）被调用时：
1. 上次 VMRUN 后，各 CPU 的 VMCB Clean Bits bit 0 = 1（CPU 自动设置）
2. `NptInvalidateAll` 写 `TlbCtl = FLUSH_ALL_ASID`
3. 但 Clean Bits bit 0 仍为 1
4. 下次 VMRUN 时 CPU 不重读 TlbCtl → TLB 刷新不执行
5. NPT 页表修改对旧 TLB 条目不可见 → 陈旧翻译

**触发条件**: IOCTL 安装/卸载 Hook 时（`NptHookFunction` / `NptUnhookFunction` / `NptUnhookAll` 在 PASSIVE_LEVEL 调用 `NptInvalidateAll`）。

**与 Intel 端对比**: Intel 端 `EptInvalidateFromGuest` 通过增加计数器来实现，每个 CPU 在自己 VM-Exit handler 中执行 INVEPT — 不依赖 Clean Bits。AMD 端通过 VMCB 直接写入 TlbCtl 的方式依赖 Clean Bits 的正确性。

**修复建议**:
```c
VOID NptInvalidateAll(VOID)
{
    ULONG i;
    if (!g_SvmState.CpuContexts) return;
    for (i = 0; i < g_SvmState.CpuCount; i++) {
        if (g_SvmState.CpuContexts[i].VmcbVa) {
            PVMCB Vmcb = g_SvmState.CpuContexts[i].VmcbVa;
            Vmcb->Control.TlbCtl = TLB_CONTROL_FLUSH_ALL_ASID;
            // 清零 Clean Bits bit 0 (Intercepts/TLB_Control/TSC)
            Vmcb->Control.CleanBits &= ~(1UL << 0);
        }
    }
}
```

---

## 🟡 警告发现

### 发现 3: DR4/DR5 退出码未处理

**文件**: `svm_exit.c:185-191`
**APM 依据**: AMD APM Vol 2, §15.10 (SVM Exit Codes)

**当前 switch 覆盖**:
```c
case SVM_EXIT_READ_DR0: ... case SVM_EXIT_READ_DR3:
case SVM_EXIT_READ_DR6: case SVM_EXIT_READ_DR7:
case SVM_EXIT_WRITE_DR0: ... case SVM_EXIT_WRITE_DR3:
case SVM_EXIT_WRITE_DR6: case SVM_EXIT_WRITE_DR7:
    Result = SvmHandleDrAccess(GuestContext, ExitCode);
    break;
```

**缺失**: `SVM_EXIT_READ_DR4` (0x024), `SVM_EXIT_READ_DR5` (0x025), `SVM_EXIT_WRITE_DR4` (0x034), `SVM_EXIT_WRITE_DR5` (0x035)

虽然某些 AMD CPU 将 DR4/DR5 别名为 DR6/DR7，但 Exit Code 仍可能是独立的值。VMCB 中 `InterceptDr` 也设置了 DR4/DR5 的读写拦截位（svm_init.c 第 500-502 行），但 exit handler 未处理对应的退出码。

如果 CPU 生成 DR4/DR5 退出码，它们落入 `default:` 分支。默认分支**不推进 RIP**（Review Issue #1 修复），导致 Guest 在 DR 访问指令上无限循环。

**修复建议**: 在 switch 中添加 DR4/DR5 case：
```c
case SVM_EXIT_READ_DR4: case SVM_EXIT_READ_DR5:
case SVM_EXIT_WRITE_DR4: case SVM_EXIT_WRITE_DR5:
    Result = SvmHandleDrAccess(GuestContext, ExitCode);
    break;
```

---

### 发现 4: SVM XSETBV Handler 未检查保留位

**文件**: `svm_exit.c:123-169`
**APM 依据**: AMD APM Vol 1, §13.3 (XCR0 Extended Control Register — 与 Intel 一致)

与 Intel 端审计发现 #4 相同的问题。AMD SVM 的 XSETBV handler 缺少保留位检查：
- 未检查 `Value` 中超出当前架构定义的位
- 未检查保留位必须为 0

参见 `vmx_exit.c` 中已经修复的 XSETBV handler 作为参考。

---

### 发现 5: SvmHandleCrAccess CR0/CR4 写入缺乏 VMX 约束式保护

**文件**: `svm_exit.c:326-341`
**APM 依据**: AMD APM Vol 2, §15.5 (Guest CRn register state in VMCB Save Area)

**当前 CR0 写处理**:
```c
case SVM_EXIT_WRITE_CR0:
    Value = SvmGetGpReg(Ctx, GpReg);
    Vmcb->Save.Cr0 = Value;
    break;
```

**问题**: AMD SVM **没有** Intel VMX 那样的 CR0/CR4 Fixed-Bit MSR 约束。但是，Guest CR0 写入仍然可能设置不兼容的组合（例如清除 PG 位但未清除 PE 位），这不会导致 VMRUN 失败（AMD 的 VMRUN 检查比 Intel VM-Entry 宽松），但可能导致 Guest 内的不可预期行为。

**与 Intel 端比较**: Intel 的 `HandleCrAccess` 应用了 `MSR_IA32_VMX_CR0_FIXED0/FIXED1` 和 `MSR_IA32_VMX_CR4_FIXED0/FIXED1` 调整。AMD 没有这些 MSR，但可以考虑应用基本的 x86-64 CR0/CR4 一致性检查。

**当前影响**: 低。在 Blue Pill 场景中，Guest 是原始 OS，不太可能向 CR0/CR4 写入非法值。但如果恶意软件或 buggy 驱动修改 CR0，可能导致 Guest 不稳定。

---

### 发现 6: VMCB Clean Bits 完全依赖隐式行为

**文件**: `svm_init.c:468` (`SvmInitVmcb`), `npt.c` 各处
**APM 依据**: AMD APM Vol 2, §15.5.1 (VMCB Clean Bits)

**问题**: 
1. `SvmInitVmcb` 用 `RtlZeroMemory` 清零整个 VMCB → CleanBits = 0（干净）
2. 首次 VMRUN 后 CPU 设置各种 Clean Bits = 1
3. `SvmApplyExceptionIntercepts` 正确清零 bit 0
4. 但 NPT 页表修改（`NptSplitLargePage`, `NptHookFunction`, `NptHandlePageFault` 中的 PTE 修改）不操作 Clean Bits

AMD APM §15.5.1 明确说明：*"If the VMCB Clean Bits feature is enabled (CPUID 0x8000000A EDX[5]=1), software should set the bits corresponding to unmodified sections to improve performance."*

但对于**已修改**的 VMCB 区域，如果 Clean Bits 被 CPU 设置为 1 且 Hypervisor 未清零，CPU 将不会从内存重新加载该区域。当前实现依赖 TlbCtl 强制刷新（发现 #2 修复后），但不操作 NPT 相关的 Clean Bits（bit 2 = NPT control area）。

**修复建议**: 在所有 NPT 页表修改后，确保对应的 VMCB Clean Bits 被正确清零。最低限度，在 `NptInvalidateAll` 中也清零 bit 0（如发现 #2 修复）。

---

## 🔵 信息发现

### 发现 7: NptInvalidateAll flags 所有 CPU TLB 刷新，但缺少每 CPU 精细化刷新

**文件**: `npt.c:1815`
**APM 依据**: AMD APM Vol 2, §15.12.1 (TLB management)

**问题**: `NptInvalidateAll` 总是设置 `TLB_CONTROL_FLUSH_ALL_ASID`，刷新所有 ASID 的所有 TLB 条目。与 Intel 端的 `EptInvalidateSingleContext`（仅刷新当前 CPU 的 EPTP 上下文）相比，AMD 端没有等价优化。

AMD SVM 有 `INVLPGA` 指令（使特定 ASID 和虚拟地址的 TLB 失效）和 `TLB_CONTROL_FLUSH_ASID`（仅刷新特定 ASID）。当前实现未利用这些细粒度刷新机制。

**影响**: 性能优化，非正确性 bug。

---

### 发现 8: VMCB Save Area — EFER.SVME 对 Guest 可见

**文件**: `svm_init.c:698`
**APM 依据**: AMD APM Vol 2, §15.5 (VMCB Save Area EFER)

**当前代码**:
```c
Vmcb->Save.Efer = __readmsr(MSR_EFER) | EFER_SVME;
```

**说明**: VMCB Save Area 中的 EFER 是 Guest 看到的值。设置 SVME = 1 告诉 Guest "SVM 已启用"。对于 Blue Pill，这是正确的 — 因为 EFER.SVME 在 VMRUN 之前已经被宿主设置。Guest 读取 EFER 时会看到 SVME=1（通过 CPUID 我们已经隐藏了 SVM 功能位，所以 Guest 不会尝试使用 SVM）。✓

但这里有一个细微点：EFER.LMA (Long Mode Active, bit 10) 应该是只读的，由 CPU 自动管理。代码从当前 MSR 读取，应包含正确的 LMA 值。✓

---

### 发现 9: SvmHandleCrAccess 中 CR4 写未保持 CR4.VMXE

**文件**: `svm_exit.c:338-341`
**APM 依据**: 通用 x86-64 架构

**说明**: 与 Intel 端不同：
- Intel: CR4.VMXE (bit 13) 必须在 Guest CR4 中保持置位
- AMD: 使用 EFER.SVME (bit 12)，CR4.VMXE 不是 SVM 的要求

所以 AMD 端不需要拦截并保持 CR4.VMXE。当前代码是正确的。✓

---

### 发现 10: SvmExitHandler 中 EFER 检查缺失

**文件**: `svm_exit.c:51-85`
**APM 依据**: AMD APM Vol 2, §15.9 (Handling #VMEXIT)

**说明**: 主出口 handler 在每一轮 VMRUN 时只重置了 `TlbCtl`，但没有检查或验证其他关键 VMCB 状态。特别是：
- EFER 的 LMA / LME 在 VMRUN 时由 CPU 自动验证
- 但如果在 #VMEXIT 处理过程中修改了 EFER 相关字段，可能导致 VM-Entry 失败（AMD 称为 "VMRUN failure"）

当前代码未执行 EFER 的验证或修正。在 Blue Pill 场景中，由于我们就是原始 OS，EFER 不会出现不一致。✓（当前安全）

---

## 审计结论

### 必须修复（未修复可导致系统死锁/不可用）

| # | 文件 | 行号 | 问题 | APM 章节 |
|---|------|------|------|----------|
| 1 | svm_init.c | 556 | INTR 拦截导致 VMEXIT 死循环 | §15.7 |
| 2 | npt.c | 1827 | NptInvalidateAll 未清零 Clean Bits bit 0 | §15.5.1 |

### 建议修复

| # | 文件 | 行号 | 问题 | APM 章节 |
|---|------|------|------|----------|
| 3 | svm_exit.c | 185-191 | DR4/DR5 退出码未处理 | §15.10 |
| 4 | svm_exit.c | 123-169 | XSETBV 未检查保留位 | Vol 1 §13.3 |
| 5 | svm_exit.c | 326-329 | CR0 写无一致性检查 | §15.5 |
| 6 | npt.c | 1827 | 通用 Clean Bits 管理缺失 | §15.5.1 |

### 信息级别

| # | 文件 | 行号 | 问题 |
|---|------|------|------|
| 7 | npt.c | 1827 | 无每 CPU 精细化 TLB 刷新 |
| 8 | svm_init.c | 698 | EFER.SVME 对 Guest 可见（当前安全） |
| 9 | svm_exit.c | 338 | CR4.VMXE 未保持（AMD 不需要） |
| 10 | svm_exit.c | 51 | EFER 验证缺失（当前安全） |

### 关键代码质量分歧 (Intel vs AMD)

| 方面 | Intel VMX | AMD SVM | 评估 |
|------|-----------|---------|------|
| 外部中断 | **不拦截** (Blue Pill) | **拦截** (当前 Bug) | AMD 需修复 |
| TLB 刷新机制 | INVEPT 指令 + 计数器 | VMCB.TlbCtl 写入 | AMD 需补 Clean Bits |
| CR0/CR4 固定位 | MSR 约束 + Mask 拦截 | 无等价机制 | AMD 正确(不需要) |
| Execute-Only | 支持 (EPT bit) | **不支持** (NPT 无此位) | AMD 的 R+X 回退正确 |
| 单步机制 | MTF (Monitor Trap Flag) | RFLAGS.TF + #DB 拦截 | AMD 实现正确 |
| HLT 处理 | 不拦截 (Blue Pill) | 不拦截 (C-2 Fix) | 一致 ✓ |
| VMCALL 认证 | M-6 nonce + CPL + CS.L + kernel RIP + EFER.LMA | 同样 (通过 `HvIsAuthenticShutdownCaller`) | 一致 ✓ |

---

## 已验证正确的设计决策

1. ✅ **VMCB 控制区布局** — `InterceptCr`, `InterceptDr`, `InterceptExceptions`, `Intercept` 均正确配置 (§15.5)
2. ✅ **CLGI/VMLOAD/VMRUN/VMSAVE/STGI 循环** — ASM 实现正确遵循 SVM 生命周期 (§15.8)
3. ✅ **Host Save Area 双重设计** — `HostSaveAreaPa` (硬件) + `HostVmcbPa` (软件 VMSAVE/VMLOAD) 分离正确 (§15.5.1)
4. ✅ **NPT 恒等映射** — PML4→PDPT→PD 结构与 EPT 完全镜像，GPA=HPA (§15.24)
5. ✅ **ASID 分配** — CPU 编号 + 1，0 保留给宿主 (§15.12)
6. ✅ **NMI 阻塞检测** — 使用 `VMCB.IntState.NmiMask` + IRET 拦截实现延迟注入 (Review Issue #3 修复, §15.7)
7. ✅ **#BP 按需拦截** — 通过 `SvmSetExceptionInterceptBp` 动态启用，避免无关调试的 VMEXIT 风暴 (C-3 修复)
8. ✅ **#DB 三要素判定** — {PagePa, Rip, Cr3} 三重匹配 + 内存屏障协议 (M-4 修复)
9. ✅ **Per-CPU NPT 隔离** — 独立 PML4/PDPT/PD/PT 页表副本消除多核竞态 (§15.24)
