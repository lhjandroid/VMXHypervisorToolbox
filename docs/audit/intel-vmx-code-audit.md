# Intel VMX 代码合规性审计报告

> **审计依据**: Intel SDM Vol 3C (325384) VMX 章节 (Ch 23-33)
> **审计日期**: 2026-06-28
> **审计范围**: `vmx_init.c`, `vmx_exit.c`, `ept.c`, `vmx.h`, `msr.c`, `hv_detect.c`
> **审计原则**: 每一项发现均附 Intel SDM 具体章节号，不靠猜测和推理

---

## 审计摘要

| 严重级别 | 数量 | 说明 |
|----------|------|------|
| 🔴 **严重** | 2 | 可导致 VM-Entry 失败 / BSOD |
| 🟡 **警告** | 4 | 可能在某些条件下导致问题 |
| 🔵 **信息** | 5 | 不符合最佳实践但当前无害 |

---

## 🔴 严重发现

### 发现 1: CR0 Guest/Host Mask 未覆盖所有 VMX 约束位

**文件**: `vmx_init.c:567-570`
**SDM 依据**: Intel SDM Vol 3C, §26.3.1.1 (Checks on Guest CR0)

**当前代码**:
```c
ULONG64 Cr0Fixed0 = __readmsr(MSR_IA32_VMX_CR0_FIXED0);
VmxWrite(VMCS_CTRL_CR0_GUEST_HOST_MASK, Cr0Fixed0);
VmxWrite(VMCS_CTRL_CR0_READ_SHADOW, Cr0 & Cr0Fixed0);
```

**问题**: CR0 Guest/Host Mask 只设置了 `IA32_VMX_CR0_FIXED0` 的值（即必须为 0 的位）。对于 **必须为 1 的位**（`IA32_VMX_CR0_FIXED1` 中置位但 `FIXED0` 中未置位的位），Guest 可以自由写入而不触发 VM-Exit。

Intel SDM §26.3.1.1 明确规定 VM-Entry 时 对 Guest CR0 的检查：
- `(CR0 & FIXED0) == 0` — 必须为 0 的位必须为 0
- `(CR0 | FIXED1) == FIXED1` — 必须为 1 的位必须为 1

如果 Guest 清除了必须为 1 的位（例如 CR0.PE=0, CR0.PG=0），由于 Mask 不拦截此写入，Guest CR0 将违反约束，下一次 VM-Entry 会因为 VM-Entry 检查失败而导致 **VM-Entry Failure**（Exit Reason bit 31 = 1），触发 VMX Shutdown。

**实际硬件示例**:
- CR0 中 NE (bit 5), PE (bit 0), PG (bit 31) 通常属于 `FIXED1` 中"必须为 1"的位
- 这些位的 `FIXED0` 值可能为 0（意味着不为"必须为 0"）
- 因此 Mask 中这些位 = 0，Guest 可以自由写入

**修复建议**:
```c
// 正确的 Mask: 拦截所有受 VMX 约束的 CR0 位
ULONG64 Cr0Fixed0 = __readmsr(MSR_IA32_VMX_CR0_FIXED0);
ULONG64 Cr0Fixed1 = __readmsr(MSR_IA32_VMX_CR0_FIXED1);
ULONG64 Cr0Mask = Cr0Fixed0 ^ Cr0Fixed1;  // 异或给出所有受约束的位
VmxWrite(VMCS_CTRL_CR0_GUEST_HOST_MASK, Cr0Mask);
// ReadShadow: Guest 视角的真实值
VmxWrite(VMCS_CTRL_CR0_READ_SHADOW, Cr0);
```

**为什么 XOR 是正确的**:
| FIXED0 bit | FIXED1 bit | XOR | 含义 |
|-----------|-----------|-----|------|
| 0 | 0 | 0 | 灵活位，无需拦截 ✓ |
| 1 | 0 | 1 | 必须为 0，拦截 ✓ |
| 0 | 1 | 1 | 必须为 1，拦截 ✓ |
| 1 | 1 | 0 | 无效组合（硬件不应出现） |

---

### 发现 2: CR4 Guest/Host Mask 仅覆盖 VMXE 位

**文件**: `vmx_init.c:572-573`
**SDM 依据**: Intel SDM Vol 3C, §26.3.1.1 (Checks on Guest CR4)

**当前代码**:
```c
VmxWrite(VMCS_CTRL_CR4_GUEST_HOST_MASK, CR4_VMXE);  /* 仅拦截 VMXE */
VmxWrite(VMCS_CTRL_CR4_READ_SHADOW, Cr4 & ~CR4_VMXE);
```

**问题**: CR4 Guest/Host Mask 只设置为 `CR4_VMXE`（bit 13），其他受 VMX 约束的 CR4 位（如 PAE bit 5, 以及其他可能在 `IA32_VMX_CR4_FIXED0/FIXED1` 中约束的位）不被拦截。

Intel SDM §26.3.1.1 对 Guest CR4 有与 CR0 同样的约束检查：
- `(CR4 & FIXED0) == 0`
- `(CR4 | FIXED1) == FIXED1`

如果 Guest 修改了受约束的 CR4 位而未触发 VM-Exit，下次 VM-Entry 将失败。

**修复建议**:
```c
ULONG64 Cr4Fixed0 = __readmsr(MSR_IA32_VMX_CR4_FIXED0);
ULONG64 Cr4Fixed1 = __readmsr(MSR_IA32_VMX_CR4_FIXED1);
ULONG64 Cr4Mask = (Cr4Fixed0 ^ Cr4Fixed1) | CR4_VMXE;  // 所有固定位 + VMXE
VmxWrite(VMCS_CTRL_CR4_GUEST_HOST_MASK, Cr4Mask);
VmxWrite(VMCS_CTRL_CR4_READ_SHADOW, Cr4 & ~CR4Mask);
```

注意：当前 HandleCrAccess 中对 CR4 的处理也需要更新，使其对受约束的 CR4 位应用 `FIXED0/FIXED1` 调整（类似 CR0 的处理）。

---

## 🟡 警告发现

### 发现 3: HandleCrAccess 中 CLTS 处理后未更新 ReadShadow

**文件**: `vmx_exit.c:878-885`
**SDM 依据**: Intel SDM Vol 3C, §25.1.3 (CR0 Guest/Host Mask and Read Shadow)

**当前代码**:
```c
case CR_ACCESS_TYPE_CLTS:
    {
        ULONG64 Cr0 = VmxRead(VMCS_GUEST_CR0);
        Cr0 &= ~(1ULL << 3);  /* Clear TS bit */
        VmxWrite(VMCS_GUEST_CR0, Cr0);
    }
    break;
```

**问题**: CLTS 清除了 Guest CR0 的 TS 位（bit 3），但没有同步更新 `VMCS_CTRL_CR0_READ_SHADOW`。如果 TS 位在 Mask 中被拦截（取决于 CR0 Fixed 位），Guest 后续读取 CR0 时会从 ReadShadow 获取过时的值（其中 TS 位仍为旧值）。

**修复建议**: CLTS 处理后也更新 ReadShadow：
```c
VmxWrite(VMCS_CTRL_CR0_READ_SHADOW, Cr0);
```

---

### 发现 4: XSETBV Handler 未检查保留位

**文件**: `vmx_exit.c:1221-1269`
**SDM 依据**: Intel SDM Vol 1, §13.3 (XCR0 Extended Control Register)

**当前代码** 检查了:
- ECX == 0 (必须是 XCR0)
- Bit 0 == 1 (x87 FPU 必须开启)
- AVX 依赖 SSE

**缺失**: Intel SDM Vol 1 §13.3 明确说明 XCR0 **保留位必须为 0**。当前代码未检查 `Value` 中超出当前架构定义的位（bits 63:10 以及各 CPU 世代中未定义的位）。

如果 Guest 向 XCR0 写入保留位 = 1，真实 XSETBV 指令会 #GP。但当前代码的验证未检测此情况，可能导致：
- 如果 CPU 本身不支持这些位 → XSETBV #GP 在 VMX root mode 中 → Host crash
- 或者 XSETBV 成功但设置了 Host 不支持的状态 → 不可预知行为

**修复建议**:
```c
// 检查保留位
ULONG64 Xcr0Mask = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) |  // x87, SSE, AVX
                    (1ULL << 3) | (1ULL << 4) |                  // BNDREG, BNDCSR
                    (1ULL << 5) | (1ULL << 6) | (1ULL << 7);    // OPMASK, ZMM, HI_ZMM

// 根据 CPUID 动态确定支持的位
// 但作为保守做法，至少拒绝所有非标准位
if (Value & ~Xcr0Mask) {
    goto InjectGp;
}
```

---

### 发现 5: INVPCID Handler 未验证 INVPCID 类型

**文件**: `vmx_exit.c:353-368`
**SDM 依据**: Intel SDM Vol 3C, §30.10 (INVVPID) and §30.8 (INVEPT)

**当前代码**:
```c
case EXIT_REASON_INVPCID:
    {
        INVVPID_DESCRIPTOR VpidDesc;
        RtlZeroMemory(&VpidDesc, sizeof(VpidDesc));
        AsmVmxInvvpid(INVVPID_ALL_CONTEXTS, &VpidDesc);
    }
    VmxAdvanceGuestRip();
    break;
```

**问题**: Guest 执行的 INVPCID 指令可能指定了具体的线性地址和 VPID (`INVVPID_INDIVIDUAL_ADDR` 类型) 或单个上下文 (`INVVPID_SINGLE_CONTEXT` 类型)。当前代码一律用 `INVVPID_ALL_CONTEXTS` 替代，导致过度刷新 TLB。

**影响**: 性能问题（过度 TLB 刷新），非正确性 Bug。Intel SDM §28.3.3.4 定义了 INVPCID 的四种类型。正确的实现应从 Exit Qualification 中解析出 INVPCID 描述符信息并精确执行。

**修复建议**: 从 INVPCID Descriptor（通过 Exit Qualification 中的寄存器操作数获取）读取类型和参数，按 Guest 请求精确执行 INVVPID。

---

### 发现 6: HandleEptMisconfig 不尝试恢复

**文件**: `vmx_exit.c:966-976`
**SDM 依据**: Intel SDM Vol 3C, §29.3.3 (EPT Misconfiguration)

**当前代码**:
```c
static BOOLEAN HandleEptMisconfig(PGUEST_CONTEXT Ctx)
{
    ...
    return FALSE;  /* 直接 Shutdown */
}
```

**问题**: EPT Misconfiguration 直接返回 FALSE 导致 VMX Shutdown（整个 Hypervisor 退出）。Intel SDM §29.3.3 列出 EPT Misconfig 的可能原因包括：
- EPT Entry 中保留位被设置
- 多个 EPT Memory Type 冲突
- Execute-Only 页面上同时设置了 Read 或 Write

这些情况理论上不应该发生（因为我们控制 EPT 页表），但一旦发生就立即 Shutdown 意味着没有诊断和恢复的机会。在调试场景中，至少应该尝试：
1. 记录完整的 EPT 页表状态
2. 尝试通过重置 EPT 条目来恢复
3. 如果无法恢复，注入 #MC（Machine Check）给 Guest 而非直接 Shutdown

---

## 🔵 信息发现

### 发现 7: VmxAdjustControls 的 SDM 章节引用错误

**文件**: `vmx_init.c:136`
**SDM 依据**: Intel SDM Vol 3C, §24.6.1 (VM-Execution Control Fields)

**当前注释**:
```c
/* Adjust control fields per Intel SDM Vol. 3C, Section 31.5.1 */
```

**问题**: 章节号 `31.5.1` 错误。正确的章节是 **§24.6.1**（VM-Execution Control Fields / Determining the Allowed Settings）。第 31 章是关于 VMX 与系统管理模式 (SMM) 的内容。

**修复**: 修改注释为 `Section 24.6.1`。

---

### 发现 8: HLT Handler 未设置 Interrupt-Window Exiting

**文件**: `vmx_exit.c:335-351`
**SDM 依据**: Intel SDM Vol 3C, §26.3.1.5 (Guest Activity State Checks)

**当前代码** 正确检查了 SDM §26.3.1.5 的 HLT Activity State 前置条件：
- RFLAGS.IF = 1 ✓
- Blocking by STI 清除 ✓
- Blocking by MOV-SS 清除 ✓
- Pending Debug Exceptions = 0 ✓

**信息**: 当前实现在 Guest 进入 HLT Activity State 后依赖外部中断来唤醒 Guest。由于 `PIN_BASED_EXTERNAL_INT_EXIT` 未设置，中断会直接投递到 Guest IDT 并自动唤醒 HLT 状态。这是正确的设计，但需要注意：如果中断被 Guest IDT 屏蔽（如 CLI 期间），Guest 将一直处于 HLT 状态。

可以考虑在未通过 HLT 条件检查时（当前代码的 else 分支），设置 Interrupt-Window Exiting 以便在中断使能后立即让 Guest 重新进入 HLT 状态。

---

### 发现 9: EptGuestVaToPa 使用 MmGetVirtualForPhysical 在 VMX Root Mode

**文件**: `ept.c:437-466`
**SDM 依据**: Windows Driver Kit 文档 — `MmGetVirtualForPhysical` IRQL 约束

**问题**: `MmGetVirtualForPhysical` 的文档说明调用者必须在 IRQL ≤ DISPATCH_LEVEL 下运行。VM-Exit Handler 在 DIRQL 下执行。虽然这在大多数 Windows 版本上可行（因为 `MmGetVirtualForPhysical` 的实现通常不严格要求 IRQL），但这不是官方保证的行为。

**影响**: 低风险。此函数在 `HandleEptViolation` 的 Mode B 代码路径中被调用（不存在 Execute-Only 支持时的回退路径）。当 `ExecuteOnlySupported == TRUE`（大多数现代 CPU）时，此代码路径不会被触发。

---

### 发现 10: IDT-Vectoring 事件重注入时 External Interrupt 类型处理

**文件**: `vmx_exit.c:674-726`
**SDM 依据**: Intel SDM Vol 3C, §27.2.4 (Information for VM Exits Due to Vectored Events)

**当前代码** 正确地重注入了 IDT-Vectoring 事件。但对于 `INTERRUPT_TYPE_EXTERNAL` (类型 0) 的事件，Intel SDM §27.2.4 特别说明：外部中断**不应**通过 VM-Entry 中断信息字段重注入，因为外部中断的向量被硬件确认 (acknowledged) 后已清除。当前代码对所有类型一视同仁地重注入。

**实际情况**: 由于 `PIN_BASED_EXTERNAL_INT_EXIT` 未设置，外部中断直接通过 Guest IDT 投递，不应出现在 IDT-Vectoring 中。加上 VMENTRY_INT_INFO 的 type=0 会导致 VM-Entry 失败（SDM 要求 type 2/3/4/5/6，不包括 0），所以当前代码虽然"重注入"了外部中断类型，但 VM-Entry 会因此失败 → 触发 HandleVmEntryFailure → VMX Shutdown。

**修复建议**: 在重注入前检查类型，对于 `INTERRUPT_TYPE_EXTERNAL` (0) 跳过重注入。

---

### 发现 11: VMCS_GUEST_IA32_PAT 未设置

**文件**: `vmx_init.c`
**SDM 依据**: Intel SDM Vol 3C, §26.3.1.5 (VM-Entry Checks on Guest State)

**当前代码** 未向 `VMCS_GUEST_IA32_PAT (0x00002804)` 写入值。

Intel SDM §26.3.1.5 说明：当 VM-Exit Controls 中 `Save IA32_PAT` 和 VM-Entry Controls 中 `Load IA32_PAT` 被设置时，PAT 的 Guest 值从 VMCS Guest-State Area 自动加载。当前代码中：
- `VMEXIT_SAVE_IA32_PAT` 未设置 (VM-Exit Controls 中只有 `HOST_ADDR_SPACE | SAVE_EFER | LOAD_EFER`)
- `VMENTRY_LOAD_IA32_PAT` 未设置

因此 PAT 不会被自动保存/加载，Guest 的 PAT 未受影响。但 VM-Entry 检查 §26.3.1.5 要求 PAT 的 bit 0-2 满足特定约束（PA0=WB, PA1/PA2=UC 等）。如果 Guest PAT 碰巧不符合约束，VM-Entry 会失败。

**当前安全**: 因为 PAT 保存/加载控制位均未设置，处理器不会检查 PAT。✓

---

## 审计结论

### 必须修复（未修复则可导致 BSOD）

| # | 文件 | 行号 | 问题 | SDM 章节 |
|---|------|------|------|----------|
| 1 | vmx_init.c | 569 | CR0 Guest/Host Mask 未覆盖必须为 1 的位 | §26.3.1.1 |
| 2 | vmx_init.c | 572 | CR4 Guest/Host Mask 仅覆盖 VMXE 位 | §26.3.1.1 |

### 建议修复

| # | 文件 | 行号 | 问题 | SDM 章节 |
|---|------|------|------|----------|
| 3 | vmx_exit.c | 885 | CLTS 后未更新 ReadShadow | §25.1.3 |
| 4 | vmx_exit.c | 1239-1252 | XSETBV 未检查保留位 | Vol 1 §13.3 |
| 5 | vmx_exit.c | 362-367 | INVPCID 一律全刷新 | §28.3.3.4 |
| 6 | vmx_exit.c | 1092 | IDT-Vectoring 可能重注入外部中断类型 | §27.2.4 |

### 信息级别

| # | 文件 | 行号 | 问题 |
|---|------|------|------|
| 7 | vmx_init.c | 136 | 注释引用错误章节号 |
| 8 | vmx_exit.c | 351 | HLT 拒绝后未设 interrupt-window |
| 9 | ept.c | 437 | MmGetVirtualForPhysical IRQL 约束 |
| 10 | vmx_init.c | — | MSR_PAT 未设置（当前安全） |

---

## 已验证正确的设计决策

以下设计经 SDM 交叉验证，确认正确：

1. ✅ **VmxAdjustControls 算法** — `(Req | Low) & High` 是正确的控制字段调整算法 (SDM §24.6.1)
2. ✅ **VMCLEAR → VMPTRLD 顺序** — 正确遵循 VMCS 生命周期 (SDM §24.1)
3. ✅ **VmxEnableOnCpu 中 VMXON 前 CR0/CR4 调整** — 正确应用 Fixed 位 (SDM §23.7)
4. ✅ **EFER SAVE/LOAD 控制位 + MSR Count=0** — 正确（EFER 通过 VMCS 字段保存，不通过 MSR Store/Load 区域）
5. ✅ **HLT Activity State 前置条件检查** — 完整检查了 IF/STI/MOV-SS/Pending-Debug (SDM §26.3.1.5)
6. ✅ **IDT-Vectoring 重注入逻辑** — 正确检查 `VMENTRY_INT_INFO.Valid` 避免双重注入
7. ✅ **EPT EPTP 配置** — Memory Type, Page Walk Length, PML4 物理地址均正确 (SDM §29.2.2)
8. ✅ **VMX Abort 处理** — VM-Entry Failure (bit 31) 正确检测并清除标志
9. ✅ **VMCALL 认证** — M-6 多层认证 (nonce + CPL + long-mode + kernel RIP) 符合防御深度原则
10. ✅ **I/O Bitmap 处理** — 全部置零配合 USE_IO_BITMAPS 正确中和 UNCONDITIONAL_IO_EXIT
