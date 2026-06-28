# AMD SVM (Secure Virtual Machine) 完整参考手册

> **版本:** 1.0  
> **目标平台:** AMD64 (x86-64) Windows Type-2 Hypervisor (Blue Pill)  
> **参考文档:** AMD64 Architecture Programmer's Manual Volume 2, Document #24593  
> **最新版本:** Rev. 3.42, March 2024  
> **官方PDF:** <https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24593.pdf>

---

## 目录

1. [SVM 架构概述](#1-svm-架构概述)
2. [CPUID 0x8000000A — SVM 检测与能力发现](#2-cpuid-0x8000000a--svm-检测与能力发现)
3. [EFER MSR — 扩展功能启用寄存器](#3-efer-msr--扩展功能启用寄存器)
4. [SVM 相关 MSR](#4-svm-相关-msr)
5. [VMCB — 虚拟机控制块 完整布局](#5-vmcb--虚拟机控制块-完整布局)
6. [SVM #VMEXIT 退出码完整表](#6-svm-vmexit-退出码完整表)
7. [嵌套分页 (NPT)](#7-嵌套分页-npt)
8. [MSR 拦截 (MSRPM)](#8-msr-拦截-msrpm)
9. [I/O 拦截 (IOPM)](#9-io-拦截-iopm)
10. [事件注入 (EVENTINJ)](#10-事件注入-eventinj)
11. [VMCB Clean Bits](#11-vmcb-clean-bits)
12. [ASID — 地址空间标识符](#12-asid--地址空间标识符)
13. [SVM 锁与安全机制](#13-svm-锁与安全机制)
14. [VM_CR — 虚拟机控制寄存器](#14-vm_cr--虚拟机控制寄存器)
15. [SVM 指令集详解](#15-svm-指令集详解)
16. [VMCB 状态保存/恢复机制](#16-vmcb-状态保存恢复机制)
17. [AMD-V vs Intel VMX 架构对比](#17-amd-v-vs-intel-vmx-架构对比)
18. [AMD-V 功能演进历程](#18-amd-v-功能演进历程)
19. [AVIC — 高级虚拟中断控制器](#19-avic--高级虚拟中断控制器)
20. [附录](#20-附录)

---

## 1. SVM 架构概述

### 1.1 AMD-V 硬件虚拟化概念

AMD SVM (Secure Virtual Machine) 是 AMD 对 x86 硬件虚拟化的实现，通常称为 **AMD-V**。它引入了两种新的执行模式：

- **Host Mode（主机模式）** — Hypervisor 运行在此模式下，拥有完全控制权
- **Guest Mode（客户模式）** — 虚拟机运行在此模式下，某些敏感指令和操作会触发 #VMEXIT

### 1.2 VMRUN 指令与 VMCB

核心机制围绕 **VMRUN** 指令和 **VMCB (Virtual Machine Control Block)**：

- **VMRUN (0F 01 D8)** — 从主机模式切换到客户模式，开始执行客户机代码
- **VMCB** — 一个 **4KB 对齐的物理页**，包含控制区域 (0x000–0x3FF) 和保存状态区域 (0x400–0xFFF)
- VMRUN 的地址通过 **RAX 寄存器** 传递（物理地址）

### 1.3 SVM 生命周期

```
+--------------------+         VMRUN (RAX = VMCB物理地址)         +---------------------+
|                    | ------------------------------------------> |                     |
|   Host Mode        |                                             |   Guest Mode        |
|   (Hypervisor)     |         #VMEXIT (硬件自动保存Guest状态       |   (虚拟机)          |
|                    | <------------------------------------------ |                     |
+--------------------+                                             +---------------------+
        |                                                                  |
        | 处理退出原因                                                       |
        | 修改VMCB (如事件注入)                                                |
        | VMRUN (恢复客户运行)                                                |
        +------------------------------------------------------------------+
```

完整流程：
1. **准备阶段:** Hypervisor 分配并初始化 VMCB，设置控制区域（拦截位、NPT 等）和保存区域（客户 CPU 状态）
2. **VMLOAD:** 加载 FS/GS/TR/LDTR 等额外状态
3. **STGI:** 设置全局中断标志 (GIF=1)
4. **VMRUN:** 硬件自动保存主机状态到 VM_HSAVE_PA，加载客户状态，进入客户模式
5. **客户运行:** 客户机代码在 Ring 0/3 执行
6. **#VMEXIT:** 发生拦截事件时，硬件自动保存客户状态到 VMCB，从 VM_HSAVE_PA 恢复主机状态
7. **退出处理:** Hypervisor 读取 EXITCODE，处理退出原因
8. **恢复运行:** Hypervisor 修改 VMCB 后再次执行 VMRUN

### 1.4 SVM 修订号和能力发现

详见第 2 节 CPUID 0x8000000A。

---

## 2. CPUID 0x8000000A — SVM 检测与能力发现

### 2.1 CPUID 概述

使用 `CPUID` 指令，输入 EAX = 0x8000000A 可获得 SVM 的特性和能力信息。

> **注意:** 如果 `CPUID Fn8000_0001_ECX[SVM] = 0`，则所有以下寄存器均为保留（不可用）。

### 2.2 寄存器布局

| 寄存器 | 位域 | 描述 |
|--------|------|------|
| **EAX** | 31:0 | SVM 修订号 (Revision Number) |
| **EBX** | 31:0 | ASID 数量 (NASID) — 最大 ASID 数目 |
| **ECX** | 31:0 | 保留 |
| **EDX** | 31:0 | SVM 特性标志 (Feature Flags) |

### 2.3 EAX — SVM 修订号

| 位 | 描述 |
|:--:|------|
| 7:0 | SVM 修订号。典型值为 `01h` |
| 31:8 | 保留 |

### 2.4 EBX — ASID 数量

| 位 | 描述 |
|:--:|------|
| 31:0 | NASID — 支持的 ASID (地址空间标识符) 数量。典型值 `0x10000` (65536) |

### 2.5 EDX — SVM 特性标志 (Feature Flags)

| 位 | 名称 | 描述 | 引入 |
|:--:|------|------|:----:|
| **0** | **NP** | Nested Paging — 嵌套分页支持 | Rev 1 |
| **1** | **LbrVirt** | LBR Virtualization — Last Branch Record 虚拟化 | Rev 1 |
| **2** | **SVML** | SVM Lock — SVM 锁定支持 | Rev 1 |
| **3** | **NRIPS** | NRIP Save — #VMEXIT 时保存 Next RIP | Rev 1 |
| **4** | **TscRateMsr** | MSR-based TSC Rate Control — 基于 MSR 的 TSC 速率控制 | Rev 1 |
| **5** | **VmcbClean** | VMCB Clean Bits — VMCB Clean Bits 支持 | Rev 1 |
| **6** | **FlushByAsid** | Flush by ASID — 按 ASID 刷新 TLB | Rev 1 |
| **7** | **DecodeAssist** | Decode Assist — 硬件辅助指令解码 | Rev 1 |
| 8 | 保留 | | |
| 9 | 保留 | | |
| **10** | **PauseFilter** | PAUSE Intercept Filter — PAUSE 拦截过滤器 | Fam15h |
| 11 | 保留 | | |
| **12** | **PauseFilterThreshold** | PAUSE Filter Threshold — PAUSE 过滤阈值 | Fam15h |
| **13** | **AVIC** | Advanced Virtual Interrupt Controller — 高级虚拟中断控制器 | Fam15h Carrizo |
| 14 | 保留 | | |
| **15** | **V_VMSAVE_VMLOAD** | Virtualized VMSAVE/VMLOAD — 虚拟化 VMSAVE/VMLOAD | Fam15h Carrizo |
| **16** | **vGIF** | Virtual GIF — 虚拟化全局中断标志 | Fam15h Carrizo |
| **17** | **GMET** | Guest Mode Execute Trap — 客户模式执行陷阱 | Zen 3 |
| **18** | **x2AVIC** | x2APIC 模式 AVIC 支持 | Zen 2 |
| 19 | 保留 | | |
| 20 | 保留 | | |
| **21** | **SSSCheck** | SVM Supervisor Shadow Stack — SVM 监管者影子栈 | Zen 3 |
| **22** | **SPEC_CTRL** | Speculation Control — 预测控制虚拟化 | Zen 3 |
| **23** | **ROGPT** | Read-Only Guest Page Table — 只读客户页表 | Zen 3 |
| **24** | **VNMI** | Virtual NMI — 虚拟化 NMI | Zen 3 |
| 25 | 保留 | | |
| **26** | **IBSVirt** | IBS Virtualization — 基于指令的采样虚拟化 | Zen 4 |
| **27** | **VmcbPermissive** | VMCB Permissive Mode — VMCB 宽松模式 | Zen 4 |
| **28** | **SVME_ADDR_CHK** | SVME Address Check — SVM 指令地址检查 | Zen 4 |
| **29** | **NestedVirt** | Nested Virtualization (SEV-SNP) — 嵌套虚拟化 | Zen 4 |
| **30** | **SEV** | Secure Encrypted Virtualization — 安全加密虚拟化 | Zen 1 |
| **31** | **SEV_ES** | SEV Encrypted State — SEV 加密状态 | Zen 3 |

---

## 3. EFER MSR — 扩展功能启用寄存器

### 3.1 EFER 寄存器 (MSR 0xC0000080)

**地址:** `0xC0000080` (MSR)  
**名称:** Extended Feature Enable Register (EFER)

### 3.2 EFER 位布局

| 位 | 助记符 | 名称 | 类型 | 描述 |
|:--:|:------:|------|:----:|------|
| **0** | **SCE** | System-Call Extension | R/W | 启用 SYSCALL/SYSRET 指令 |
| 1–7 | — | 保留 | — | MBZ |
| **8** | **LME** | Long Mode Enable | R/W | 启用长模式 |
| 9 | — | 保留 | — | MBZ |
| **10** | **LMA** | Long Mode Active | **R/O** | 指示长模式已激活 (只读状态位) |
| **11** | **NXE** | No-Execute Enable | R/W | 启用 No-Execute 页保护 |
| **12** | **SVME** | Secure Virtual Machine Enable | **R/W** | **启用 SVM 扩展** |
| **13** | **LMSLE** | Long Mode Segment Limit Enable | R/W | 启用长模式段限制检查 |
| **14** | **FFXSR** | Fast FXSAVE/FXRSTOR | R/W | 启用快速 FXSAVE/FXRSTOR |
| **15** | **TCE** | Translation Cache Extension | R/W | 启用转换缓存扩展 |
| 63:16 | — | 保留 | — | MBZ |

### 3.3 EFER.SVME (位 12) 详解

- **重置值:** `0` (默认禁用)
- **功能:** 将此位设为 `1` 可启用 SVM (AMD-V) 虚拟化扩展。当该位为零时，所有 SVM 指令（`VMRUN`、`VMLOAD`、`VMSAVE`、`CLGI`、`STGI`、`INVLPGA`、`VMMCALL`）都会引发 `#UD` (非法指令) 异常
- **警告:** 在客户机运行时关闭 EFER.SVME 的行为是 **未定义的**。VMM 应始终阻止客户机写入 EFER

```c
// Linux 内核定义
#define MSR_EFER          0xC0000080
#define EFER_SVME         (1 << 12)   // = 0x1000
```

### 3.4 EFER.SVME 与 VM_CR 的交互

参见第 13 节 "SVM 锁与安全机制" 了解 VM_CR.SVMDIS 如何阻止写入 EFER.SVME。

---

## 4. SVM 相关 MSR

### 4.1 VM_CR — 虚拟机控制寄存器 (0xC0010114)

| 位 | 名称 | 类型 | 描述 |
|:--:|:----:|:----:|------|
| 0 | DPD | R/W | Debug Port Disable — 调试端口禁用 |
| **1** | **R_INIT** | R/W | Intercept INIT — 拦截 INIT 信号 |
| **2** | **DIS_A20M** | R/W | Disable A20 Masking — 禁用 A20 地址掩码 |
| **3** | **LOCK** | R/W | SVM Lock — SVM 锁定位 |
| **4** | **SVME_DISABLE** | R/W | SVM Disable — 禁用 SVM (通过 EFER.SVME) |
| 63:5 | — | R | 保留 |

> 完整详解请参见第 14 节。

### 4.2 VM_HSAVE_PA — 主机保存区物理地址 (0xC0010117)

| 位 | 描述 |
|:--:|------|
| 63:0 | 主机保存区的物理地址（必须 4KB 对齐） |

- **MSR 地址:** `0xC0010117`
- **大小:** 64 位物理地址
- **对齐:** 必须指向一个 4KB 对齐的物理页
- **功能:** 此 MSR 必须由 Hypervisor 在 VMRUN 之前设置。VMRUN 执行时，硬件会自动将关键主机状态 (RSP、RAX、RIP、RFLAGS、CS/SS/DS/ES 等) 保存到此页面；#VMEXIT 时自动恢复
- **必须设置:** 每个逻辑核心必须在首次 VMRUN 之前设置 VM_HSAVE_PA

### 4.3 VM_IGNNE — IGNNE 状态 (0xC0010115)

- **MSR 地址:** `0xC0010115`
- 保存 IGNNE# (Ignore Numeric Error) 引脚的锁定状态

### 4.4 TSC_RATIO — TSC 速率控制 (0xC0010204)

> **注意:** 这是特定于家族的 MSR，地址可能根据家族有所不同。对于 Family 10h 及更新版本：`0xC0010204`。某些旧文档可能引用 `0xC0010201`。

- **功能:** 控制呈现给客户机的 TSC 频率与真实 TSC 之间的比率
- **需要:** CPUID Fn8000_000A_EDX[TscRateMsr] = 1
- **公式:**
  - TSC 比率 = (TSC 乘数) / (TSC 除数)
  - 实际 MSR 编码视具体家族而定

### 4.5 SVM_KEY — SVM 解锁密钥 (0xC0010118)

- **MSR 地址:** `0xC0010118`
- **功能:** 写入正确的密钥可清除 VM_CR.LOCK 位
- **限制:** 只有具有适当平台权限的软件（通常是 BIOS/固件）才知道密钥值

### 4.6 SYSCFG — 系统配置 (0xC0010010)

- **MSR 地址:** `0xC0010010`
- 位 20 (MtrrVarDram): MTRR 可变 DRAM 启用
- 位 21 (MtrrFixDram): MTRR 固定 DRAM 启用
- 位 24 (Tom2ForceMemTypeWB): TOM2 强制 WB 内存类型

### 4.7 硬件配置寄存器 (0xC0010015)

- **MSR 地址:** `0xC0010015`
- 控制与 C1E (增强暂停) 状态相关的行为

### 4.8 MSR 地址总结

| MSR | 地址 | 用途 |
|-----|:----:|------|
| EFER | `0xC0000080` | 扩展功能启用 |
| VM_CR | `0xC0010114` | 虚拟机控制 |
| VM_IGNNE | `0xC0010115` | IGNNE 锁定状态 |
| VM_HSAVE_PA | `0xC0010117` | 主机保存区物理地址 |
| SVM_KEY | `0xC0010118` | SVM 解锁密钥 |
| SYSCFG | `0xC0010010` | 系统配置 |
| HWCR | `0xC0010015` | 硬件配置 |
| TSC_RATIO | `0xC0010204` | TSC 比率控制 |

---

## 5. VMCB — 虚拟机控制块 完整布局

### 5.1 VMCB 概况

- **总大小:** 4096 字节 (0x1000, 4KB) — 恰好一页
- **控制区域:** 偏移 0x000–0x3FF (1024 字节)
- **保存区域:** 偏移 0x400–0xFFF (3072 字节)
- **对齐:** 必须 4KB 对齐

> **重要说明:** 不同 SVM 修订版中，VMCB 的控制区域布局略有差异。下表详述了现代 AMD 处理器（Rev 1+，包括所有 Zen 系列）的布局。历史版本的偏移量在附录中注明。

### 5.2 VMCB 控制区域 (偏移 0x000–0x3FF)

控制区域占用 VMCB 的偏移 **0x000–0x3FF**。

| 绝对偏移 | 大小 | 字段 | 描述 |
|:--------:|:----:|------|------|
| **0x000** | 2B | CR_INTERCEPT_READ | CR0–CR15 读拦截位图 |
| **0x002** | 2B | CR_INTERCEPT_WRITE | CR0–CR15 写拦截位图 |
| **0x004** | 4B | DR_INTERCEPT | DR0–DR15 读/写拦截位图 (32位) |
| **0x008** | 4B | EXCEPTION_INTERCEPT | 异常向量 0–31 拦截位图 |
| **0x00C** | 4B | INTERCEPT_VECTOR_1 | 通用拦截控制 1 |
| **0x010** | 4B | INTERCEPT_VECTOR_2 | 通用拦截控制 2 |
| **0x014** | 4B | INTERCEPT_VECTOR_3 | 通用拦截控制 3 (SEV-SNP 扩展) |
| **0x018** | 24B | 保留 (SBZ) | |
| **0x030** | 8B | IOPM_BASE_PA | I/O 权限映射表物理地址 [51:12] |
| **0x038** | 8B | MSRPM_BASE_PA | MSR 权限映射表物理地址 [51:12] |
| **0x040** | 8B | TSC_OFFSET | TSC 偏移值 (64位) |
| **0x048** | 4B | EVENTINJ (低32位) | 事件注入 (参见第10节) |
| **0x04C** | 4B | EVENTINJ (高32位) | 事件注入 (续) |
| **0x050** | 12B | 保留 (SBZ) | |
| **0x05C** | 1B | TLB_CONTROL | TLB 控制 (参见第12节) |
| **0x05D** | 3B | 保留 | |
| **0x060** | 8B | V_INTR | 虚拟中断 (AVIC 相关) |
| **0x068** | 8B | V_INTR_STATE | 中断状态: INTR_SHADOW, GUEST_INTERRUPT_MASK |
| **0x070** | 8B | EXITCODE | #VMEXIT 退出码 |
| **0x078** | 8B | EXITINFO1 | 退出信息 1 |
| **0x080** | 8B | EXITINFO2 | 退出信息 2 |
| **0x088** | 8B | EXITINTINFO | 退出中断信息 |
| **0x090** | 8B | N_CR3 | 嵌套页表 CR3 (nCR3) |
| **0x098** | 8B | V_APIC_BAR | 虚拟 APIC 基地址 (AVIC) |
| **0x0A0** | 8B | 保留 (SBZ) | |
| **0x0A8** | 8B | EVENTINJ (完整64位) | 事件注入字段 (参见第10节) |
| **0x0B0** | 8B | 保留 (SBZ) | |
| **0x0B8** | 8B | VIRT_EXT | 虚拟化扩展控制: LBR_VIRT_ENABLE, V_VMSAVE_VMLOAD, V_IBS_ENABLE |
| **0x0C0** | 4B | CLEAN_BITS | VMCB Clean Bits (参见第11节) |
| **0x0C4** | 4B | 保留 (SBZ) | |
| **0x0C8** | 8B | NRIP | Next RIP — 下一条指令的 RIP |
| **0x0D0** | 16B | GUEST_INSN | 解码辅助: 字节计数 (1B) + 指令字节 (15B) |
| **0x0E0** | 8B | AVIC_APIC_BACKING_PAGE | AVIC APIC 后备页 [51:12] |
| **0x0E8** | 8B | 保留 | |
| **0x0F0** | 8B | AVIC_LOGICAL_TABLE | AVIC 逻辑 APIC ID 表 [51:12] |
| **0x0F8** | 8B | AVIC_PHYSICAL_TABLE | AVIC 物理 APIC ID 表 [51:12] + MAX_INDEX [7:0] |
| **0x100** | 8B | VMSA_PTR | VMSA 指针 (SEV-ES) [51:12] |
| **0x108** | 8B | VMGEXIT_RAX | VMGEXIT RAX (SEV-ES) |
| **0x110** | 1B | VMGEXIT_CPL | VMGEXIT CPL (SEV-ES) |
| **0x118–0x13F** | — | 保留 | |
| **0x138** | 8B | ALLOWED_SEV_FEATURES | 允许的 SEV 特性 |
| **0x140** | 8B | GUEST_SEV_FEATURES | 客户 SEV 特性 |
| **0x150–0x16F** | 32B | REQUESTED_IRR (4×64) | 请求的中断请求寄存器 (AVIC) |
| **0x170–0x3DF** | — | 保留 (SBZ) | |
| **0x3E0** | 32B | HOST_SAVE | 为 Hypervisor 保留的软件存储区 |

#### 5.2.0 VMCB 控制区域结构定义 (C 语言)

```c
struct vmcb_control_area {
    uint16_t intercept_cr_read;    // 0x000: CR0-CR15 读拦截
    uint16_t intercept_cr_write;   // 0x002: CR0-CR15 写拦截
    uint32_t intercept_dr;         // 0x004: DR0-DR15 读/写拦截
    uint32_t intercept_exceptions; // 0x008: 异常向量 0-31
    uint32_t intercept_vectors_1;  // 0x00C: 通用拦截向量 1
    uint32_t intercept_vectors_2;  // 0x010: 通用拦截向量 2
    uint32_t intercept_vectors_3;  // 0x014: 通用拦截向量 3 (SEV-SNP)
    uint32_t reserved_1[6];        // 0x018
    uint64_t iopm_base_pa;         // 0x030: IOPM 物理地址
    uint64_t msrpm_base_pa;        // 0x038: MSRPM 物理地址
    uint64_t tsc_offset;           // 0x040: TSC 偏移
    uint32_t eventinj_low;         // 0x048: 事件注入低 (旧格式)
    uint32_t eventinj_high;        // 0x04C: 事件注入高 (旧格式)
    uint8_t  reserved_3[15];       // 0x050
    uint8_t  tlb_control;          // 0x05C: TLB 控制
    uint8_t  reserved_4[3];        // 0x05D
    uint64_t v_intr;               // 0x060: 虚拟中断 (AVIC)
    uint64_t v_intr_state;         // 0x068: 中断状态
    uint64_t exit_code;            // 0x070: #VMEXIT 退出码
    uint64_t exit_info_1;          // 0x078: 退出信息 1
    uint64_t exit_info_2;          // 0x080: 退出信息 2
    uint64_t exit_int_info;        // 0x088: 退出中断信息
    uint64_t nested_cr3;           // 0x090: nCR3 (NPT 根)
    uint64_t v_apic_bar;           // 0x098: APIC 基址 (AVIC)
    uint64_t reserved_6;           // 0x0A0
    uint64_t event_inj;            // 0x0A8: 事件注入 (完整 64 位)
    uint64_t reserved_7;           // 0x0B0
    uint64_t virt_ext;             // 0x0B8: 虚拟化扩展
    uint32_t clean_bits;           // 0x0C0: VMCB Clean 位
    uint32_t reserved_8;           // 0x0C4
    uint64_t next_rip;             // 0x0C8: nRIP
    uint16_t insn_len;             // 0x0D0: 指令字节数 (DecodeAssist)
    uint8_t  insn_bytes[15];       // 0x0D2: 指令字节
    uint8_t  reserved_9;           // 0x0E1
    uint64_t avic_apic_backing_page; // 0x0E0: AVIC 后备页
    uint64_t reserved_10;          // 0x0E8
    uint64_t avic_logical_table;   // 0x0F0: AVIC 逻辑表
    uint64_t avic_physical_table;  // 0x0F8: AVIC 物理表
    uint64_t vmsa_ptr;             // 0x100: VMSA 指针 (SEV-ES)
    uint64_t vmgexit_rax;          // 0x108: VMGEXIT RAX
    uint8_t  vmgexit_cpl;          // 0x110: VMGEXIT CPL
    uint8_t  reserved_11[39];      // 0x111
    uint64_t allowed_sev_features; // 0x138: 允许的 SEV 特性
    uint64_t guest_sev_features;   // 0x140: 客户 SEV 特性
    uint64_t requested_irr[4];     // 0x150-0x16F: AVIC IRR
    uint8_t  reserved_12[0x3E0 - 0x170]; // 填充到 0x3E0
    uint8_t  host_reserved[32];    // 0x3E0: 软件预留
};
```

#### 5.2.0.1 VMCB 保存区域结构定义 (C 语言)

```c
struct vmcb_save_area {
    /* 段寄存器: 每个 16 字节 */
    struct { uint16_t sel, attrib; uint32_t limit; uint64_t base; } es;     // 0x000
    struct { uint16_t sel, attrib; uint32_t limit; uint64_t base; } cs;     // 0x010
    struct { uint16_t sel, attrib; uint32_t limit; uint64_t base; } ss;     // 0x020
    struct { uint16_t sel, attrib; uint32_t limit; uint64_t base; } ds;     // 0x030
    struct { uint16_t sel, attrib; uint32_t limit; uint64_t base; } fs;     // 0x040
    struct { uint16_t sel, attrib; uint32_t limit; uint64_t base; } gs;     // 0x050

    /* 描述符表 */
    struct { uint16_t reserved_sel, reserved_attrib; uint32_t limit; uint64_t base; } gdtr; // 0x060
    struct { uint16_t sel, attrib; uint32_t limit; uint64_t base; } ldtr;   // 0x070
    struct { uint16_t reserved_sel, reserved_attrib; uint32_t limit; uint64_t base; } idtr; // 0x080
    struct { uint16_t sel, attrib; uint32_t limit; uint64_t base; } tr;     // 0x090

    uint8_t  reserved_1[0x3A];    // 0x0A0
    uint8_t  vmpl;                 // 0x0CA: VMPL (SEV-SNP)
    uint8_t  cpl;                  // 0x0CB: 当前权限级
    uint8_t  reserved_2[4];        // 0x0CC
    uint64_t efer;                 // 0x0D0: EFER MSR
    /* ... 更多字段遵循第 5.3 节的布局 ... */
};
```

### 5.2.1 CR 拦截位 (偏移 0x000–0x002)

**CR 读拦截 (偏移 0x000, 16 位):**

| 偏移位 | CR | 拦截操作 |
|:-----:|:--:|:--------:|
| 0 | CR0 | MOV CR0, reg / SMSW |
| 1 | CR1 | MOV CR1, reg (CR1 不可用，此位通常无用) |
| 2 | CR2 | MOV CR2, reg |
| 3 | CR3 | MOV CR3, reg |
| 4 | CR4 | MOV CR4, reg |
| 5 | CR5 | MOV CR5, reg |
| 6 | CR6 | MOV CR6, reg |
| 7 | CR7 | MOV CR7, reg |
| 8 | CR8 | MOV CR8, reg |
| 9–15 | CR9–CR15 | MOV CRx, reg |

**CR 写拦截 (偏移 0x002, 16 位):**

| 偏移位 | CR | 拦截操作 |
|:-----:|:--:|:--------:|
| 0 | CR0 | MOV reg, CR0 / LMSW |
| 1 | CR1 | MOV reg, CR1 |
| 2 | CR2 | MOV reg, CR2 |
| 3 | CR3 | MOV reg, CR3 |
| 4 | CR4 | MOV reg, CR4 |
| 5 | CR5 | MOV reg, CR5 |
| 6 | CR6 | MOV reg, CR6 |
| 7 | CR7 | MOV reg, CR7 |
| 8 | CR8 | MOV reg, CR8 |
| 9–15 | CR9–CR15 | MOV reg, CRx |

#### 5.2.2 DR 拦截位 (偏移 0x004, 32 位)

| 位 | DR | 拦截操作 |
|:--:|:--:|:--------:|
| 0 | DR0 | MOV DR0, reg / MOV reg, DR0 |
| 1 | DR1 | MOV DR1, reg / MOV reg, DR1 |
| 2 | DR2 | MOV DR2, reg / MOV reg, DR2 |
| 3 | DR3 | MOV DR3, reg / MOV reg, DR3 |
| 4 | DR4 | MOV DR4, reg / MOV reg, DR4 |
| 5 | DR5 | MOV DR5, reg / MOV reg, DR5 |
| 6 | DR6 | MOV DR6, reg / MOV reg, DR6 |
| 7 | DR7 | MOV DR7, reg / MOV reg, DR7 |
| 8–15 | DR8–DR15 | MOV DRx, reg / MOV reg, DRx |

#### 5.2.3 异常拦截位 (偏移 0x008, 32 位)

| 位 | 向量 | 异常 | 描述 |
|:--:|:----:|:----:|------|
| 0 | 0 | #DE | 除法错误 |
| 1 | 1 | #DB | 调试异常 |
| 2 | 2 | — | 向量 2 (保留；用 NMI 拦截替代) |
| 3 | 3 | #BP | 断点 (INT3) |
| 4 | 4 | #OF | 溢出 (INTO) |
| 5 | 5 | #BR | BOUND 范围超出 |
| 6 | 6 | #UD | 非法操作码 |
| 7 | 7 | #NM | 设备不可用 (x87 FPU) |
| 8 | 8 | #DF | 双重故障 |
| 9 | 9 | — | 保留 |
| 10 | 10 | #TS | 无效 TSS |
| 11 | 11 | #NP | 段不存在 |
| 12 | 12 | #SS | 栈段故障 |
| 13 | 13 | #GP | 通用保护 |
| 14 | 14 | #PF | 页故障 |
| 15 | 15 | — | 保留 |
| 16 | 16 | #MF | x87 FPU 浮点错误 |
| 17 | 17 | #AC | 对齐检查 |
| 18 | 18 | #MC | 机器检查 |
| 19 | 19 | #XF | SIMD 浮点异常 |
| 20–31 | 20–31 | — | 保留 |

#### 5.2.4 通用拦截控制 1 — INTERCEPT_VECTOR_1 (偏移 0x00C, 32 位)

| 位 | 拦截名称 | 描述 |
|:--:|:---------:|------|
| **0** | INTR | 外部可屏蔽中断 |
| **1** | NMI | 不可屏蔽中断 |
| **2** | SMI | 系统管理中断 |
| **3** | INIT | INIT 信号 |
| **4** | VINTR | 虚拟中断 (AVIC) |
| **5** | CR0_SEL_WRITE | CR0 选择性写 (如 LMSW) |
| **6** | IDTR_READ | SIDT 指令 |
| **7** | IDTR_WRITE | LIDT 指令 |
| **8** | GDTR_READ | SGDT 指令 |
| **9** | GDTR_WRITE | LGDT 指令 |
| **10** | LDTR_READ | SLDT 指令 |
| **11** | LDTR_WRITE | LLDT 指令 |
| **12** | TR_READ | STR 指令 |
| **13** | TR_WRITE | LTR 指令 |
| **14** | RDTSC | RDTSC/RDTSCP 指令 |
| **15** | RDPMC | RDPMC 指令 |
| **16** | PUSHF | PUSHF/PUSHFD/PUSHFQ 指令 |
| **17** | POPF | POPF/POPFD/POPFQ 指令 |
| **18** | CPUID | CPUID 指令 |
| **19** | RSM | RSM 指令 (SMM 恢复) |
| **20** | IRET | IRET 指令 |
| **21** | INTn | 软件中断 (INT n) |
| **22** | INVD | INVD 指令 |
| **23** | PAUSE | PAUSE 指令 |
| **24** | HLT | HLT 指令 |
| **25** | INVLPG | INVLPG 指令 |
| **26** | INVLPGA | INVLPGA 指令 |
| **27** | IOIO_PROT | I/O 指令拦截 (IN/OUT) |
| **28** | MSR_PROT | MSR 拦截 (RDMSR/WRMSR) |
| **29** | TASK_SWITCH | 任务切换 |
| **30** | FERR_FREEZE | FERR 冻结 (x87 FPU) |
| **31** | SHUTDOWN | 关闭 (如三重故障) |

#### 5.2.5 通用拦截控制 2 — INTERCEPT_VECTOR_2 (偏移 0x010, 32 位)

| 位 | 拦截名称 | 描述 |
|:--:|:---------:|------|
| **0** | VMRUN | VMRUN 指令 |
| **1** | VMMCALL | VMMCALL 指令 |
| **2** | VMLOAD | VMLOAD 指令 |
| **3** | VMSAVE | VMSAVE 指令 |
| **4** | STGI | STGI 指令 |
| **5** | CLGI | CLGI 指令 |
| **6** | SKINIT | SKINIT 指令 |
| **7** | RDTSCP | RDTSCP 指令 |
| **8** | ICEBP | ICEBP (INT1) 指令 |
| **9** | WBINVD | WBINVD 指令 |
| **10** | MONITOR | MONITOR 指令 |
| **11** | MWAIT_UNCOND | MWAIT 指令 (无条件) |
| **12** | MWAIT_ARMED | MWAIT 指令 (已触发) |
| **13** | XSETBV | XSETBV 指令 |
| **14** | RDPRU | RDPRU 指令 |
| **15** | EFER_WRITE_TRAP | EFER 写陷阱 |
| **16** | CR0_WRITE_TRAP | CR0 写陷阱 |
| **17** | CR1_15_WRITE_TRAP | CR1..CR15 写陷阱 |
| **18** | CR2_15_WRITE_TRAP | CR2..CR15 写陷阱 |
| **19** | CR3_WRITE_TRAP | CR3 写陷阱 |
| **20** | CR4_WRITE_TRAP | CR4 写陷阱 |
| **21** | CR5_15_WRITE_TRAP | CR5..CR15 写陷阱 |
| **22** | INVLPGB | INVLPGB 指令 (广播 TLB 失效) |
| **23** | INVPCID | INVPCID 指令 |
| **24** | MCOMMIT | MCOMMIT 指令 |
| **25** | TLBSYNC | TLBSYNC 指令 |
| **26–31** | — | 保留 |

#### 5.2.6 通用拦截控制 3 — INTERCEPT_VECTOR_3 (偏移 0x014, 32 位)

SEV-SNP 和较新 AMD 处理器中增加的扩展拦截：

| 位 | 拦截名称 | 描述 |
|:--:|:---------:|------|
| 0 | VMGEXIT | VMGEXIT 指令 (SEV-ES) |
| 1–31 | — | 保留 (SBZ) |

### 5.3 VMCB 保存区域 (偏移 0x400–0xFFF)

保存区域从绝对偏移 **0x400** 开始。下表中的偏移是 **相对于保存区域起始 (0x400)** 的偏移。

#### 5.3.1 段寄存器 (每个 16 字节, 相对偏移 0x000–0x09F)

| 相对偏移 | 字段 | 字节 |
|:--------:|:----:|:----:|
| **0x000** | ES_SEL | 2B 选择子, 2B 属性, 4B 限长, 8B 基址 |
| **0x010** | CS_SEL | 2B 选择子, 2B 属性, 4B 限长, 8B 基址 |
| **0x020** | SS_SEL | 2B 选择子, 2B 属性, 4B 限长, 8B 基址 |
| **0x030** | DS_SEL | 2B 选择子, 2B 属性, 4B 限长, 8B 基址 |
| **0x040** | FS_SEL | 2B 选择子, 2B 属性, 4B 限长, 8B 基址 |
| **0x050** | GS_SEL | 2B 选择子, 2B 属性, 4B 限长, 8B 基址 |

每个段寄存器的内部布局 (16 字节):

| 偏移内偏移 | 大小 | 字段 |
|:----------:|:----:|------|
| 0 | 2B | 选择子 (Selector) |
| 2 | 2B | 属性 (Attributes) |
| 4 | 4B | 限长 (Limit) |
| 8 | 8B | 基址 (Base) — 低 32 位有效 |

#### 5.3.2 描述符表寄存器 (相对偏移 0x060–0x09F)

| 相对偏移 | 字段 | 大小 |
|:--------:|:----:|:----:|
| **0x060** | GDTR | 2B 选择子(保留), 2B 属性(保留), 4B 限长(低16位有效), 8B 基址 |
| **0x070** | LDTR | 2B 选择子, 2B 属性, 4B 限长, 8B 基址 |
| **0x080** | IDTR | 2B 选择子(保留), 2B 属性(保留), 4B 限长(低16位有效), 8B 基址 |
| **0x090** | TR | 2B 选择子, 2B 属性, 4B 限长, 8B 基址 |

#### 5.3.3 系统寄存器 (相对偏移 0x0A0–0x1FF)

| 相对偏移 | 大小 | 字段 | 交换类型 | 描述 |
|:--------:|:----:|:----:|:--------:|------|
| **0x0A0** | 42B | 保留 | | |
| **0x0CA** | 1B | VMPL | | VMPL 级别 (SEV-SNP) |
| **0x0CB** | 1B | CPL | | 当前权限级别 |
| **0x0CC** | 4B | 保留 | | |
| **0x0D0** | 8B | EFER | B | EFER MSR |
| **0x0D8** | 8B | 保留 | | |
| **0x0E0** | 8B | PERF_CTL0 | | 性能控制 0 |
| **0x0E8** | 8B | PERF_CTR0 | | 性能计数器 0 |
| **0x0F0** | 8B | PERF_CTL1 | | 性能控制 1 |
| **0x0F8** | 8B | PERF_CTR1 | | 性能计数器 1 |
| **0x100** | 8B | PERF_CTL2 | | 性能控制 2 |
| **0x108** | 8B | PERF_CTR2 | | 性能计数器 2 |
| **0x110** | 8B | PERF_CTL3 | | 性能控制 3 |
| **0x118** | 8B | PERF_CTR3 | | 性能计数器 3 |
| **0x120** | 8B | PERF_CTL4 | | 性能控制 4 |
| **0x128** | 8B | PERF_CTR4 | | 性能计数器 4 |
| **0x130** | 8B | PERF_CTL5 | | 性能控制 5 |
| **0x138** | 8B | PERF_CTR5 | | 性能计数器 5 |
| **0x140** | 8B | 保留 | | |
| **0x148** | 8B | CR4 | B | 控制寄存器 4 |
| **0x150** | 8B | CR3 | B | 控制寄存器 3 |
| **0x158** | 8B | CR0 | B | 控制寄存器 0 |
| **0x160** | 8B | DR7 | B | 调试寄存器 7 |
| **0x168** | 8B | DR6 | B | 调试寄存器 6 |
| **0x170** | 8B | RFLAGS | B | 标志寄存器 |
| **0x178** | 8B | RIP | B | 指令指针 |
| **0x180** | 64B | 保留 | | |
| **0x1C0** | 8B | INSTR_RETIRED_CTR | | 退休指令计数器 |
| **0x1C8** | 8B | PERF_CTR_GLOBAL_STS | | 全局性能计数器状态 |
| **0x1D0** | 8B | PERF_CTR_GLOBAL_CTL | | 全局性能计数器控制 |
| **0x1D8** | 8B | RSP | B | 栈指针 |
| **0x1E0** | 8B | S_CET | B | 监管者 CET |
| **0x1E8** | 8B | SSP | B | 监管者影子栈指针 |
| **0x1F0** | 8B | ISST_ADDR | B | 中断影子栈表地址 |
| **0x1F8** | 8B | RAX | B | 通用寄存器 RAX |

#### 5.3.4 MSR 寄存器 (相对偏移 0x200–0x307)

| 相对偏移 | 大小 | 字段 | 交换类型 | 描述 |
|:--------:|:----:|:----:|:--------:|------|
| **0x200** | 8B | STAR | B | STAR MSR |
| **0x208** | 8B | LSTAR | B | LSTAR MSR |
| **0x210** | 8B | CSTAR | B | CSTAR MSR |
| **0x218** | 8B | SFMASK | B | SF_MASK MSR |
| **0x220** | 8B | KERNEL_GS_BASE | B | KernelGSBase MSR |
| **0x228** | 8B | SYSENTER_CS | B | SYSENTER_CS MSR |
| **0x230** | 8B | SYSENTER_ESP | B | SYSENTER_ESP MSR |
| **0x238** | 8B | SYSENTER_EIP | B | SYSENTER_EIP MSR |
| **0x240** | 8B | CR2 | B | 控制寄存器 2 |
| **0x248** | 32B | 保留 | | |
| **0x268** | 8B | G_PAT | B | PAT MSR (仅当 NPT 启用时) |
| **0x270** | 8B | DBGCTL | C | 调试控制 MSR |
| **0x278** | 8B | BR_FROM | C | LastBranchFromIP |
| **0x280** | 8B | BR_TO | C | LastBranchToIP |
| **0x288** | 8B | LASTEXCPFROM | C | LastIntFromIP |
| **0x290** | 8B | LASTEXCPTO | C | LastIntToIP |
| **0x298** | 8B | DBGEXTNCTL | C | DebugExtnCtl |
| **0x2A0** | 64B | 保留 | | |
| **0x2E0** | 8B | SPEC_CTRL | | 预测控制 MSR |
| **0x2E8** | 16B | 保留 | | |
| **0x2F8** | 8B | GUEST_TSC_OFFSET | | 客户 TSC 偏移 |
| **0x300** | 8B | REG_PROT_NONCE | | 寄存器保护 Nonce (SEV-ES) |
| **0x308** | 8B | RCX | B | 通用寄存器 RCX |
| **0x310** | 8B | RDX | B | 通用寄存器 RDX |
| **0x318** | 8B | RBX | B | 通用寄存器 RBX |
| **0x320** | 8B | 保留 | | (RSP 在 0x1D8) |
| **0x328** | 8B | RBP | B | 通用寄存器 RBP |
| **0x330** | 8B | RSI | B | 通用寄存器 RSI |
| **0x338** | 8B | RDI | B | 通用寄存器 RDI |
| **0x340** | 8B | R8 | B | 通用寄存器 R8 |
| **0x348** | 8B | R9 | B | 通用寄存器 R9 |
| **0x350** | 8B | R10 | B | 通用寄存器 R10 |
| **0x358** | 8B | R11 | B | 通用寄存器 R11 |
| **0x360** | 8B | R12 | B | 通用寄存器 R12 |
| **0x368** | 8B | R13 | B | 通用寄存器 R13 |
| **0x370** | 8B | R14 | B | 通用寄存器 R14 |
| **0x378** | 8B | R15 | B | 通用寄存器 R15 |

> **交换类型说明:**
> - **B (Swap Type B):** 由 VMRUN/#VMEXIT 自动保存/恢复的寄存器
> - **C (Swap Type C):** 仅在 #VMEXIT 时有条件保存的寄存器

#### 5.3.5 SEV-ES / 自动退出额外字段 (相对偏移 0x380–0x3FF)

| 相对偏移 | 大小 | 字段 | 描述 |
|:--------:|:----:|:----:|------|
| **0x390** | 8B | GUEST_EXITINFO1 | 自动退出的 EXITINFO1 |
| **0x398** | 8B | GUEST_EXITINFO2 | 自动退出的 EXITINFO2 |
| **0x3A0** | 8B | GUEST_EXITINTINFO | 自动退出的 EXITINTINFO |
| **0x3A8** | 8B | GUEST_NRIP | 自动退出的 Next RIP |
| **0x3B0** | 8B | SEV_FEATURES | SEV 特性: SNPActive, vTOM, ReflectVC |
| **0x3B8** | 8B | VINTR_CTRL | 客户控制的中断注入: V_TPR, V_IRQ, VGIF |
| **0x3C0** | 8B | GUEST_EXITCODE | 自动退出的 EXITCODE |
| **0x3C8** | 8B | VIRTUAL_TOM | 虚拟 TOM (Top of Memory) |
| **0x3D0** | 8B | TLB_ID | TLB ID |
| **0x3D8** | 8B | PCPU_ID | 物理 CPU ID |
| **0x3E0** | 8B | EVENTINJ | 事件注入 (同 VMCB 0xA8) |
| **0x3E8** | 8B | XCR0 | B | 扩展控制寄存器 0 |
| **0x3F0** | 16B | 保留 | |

#### 5.3.6 FPU/XMM/YMM 状态 (Swap Type C, 相对偏移 0x400+)

| 相对偏移 | 大小 | 字段 |
|:--------:|:----:|:----:|
| **0x400** | 8B | X87_DP (x87 数据指针) |
| **0x408** | 4B | MXCSR |
| **0x40C** | 2B | X87_FTW (标记字) |
| **0x40E** | 2B | X87_FSW (状态字) |
| **0x410** | 2B | X87_FCW (控制字) |
| **0x412** | 2B | X87_FOP (操作码) |
| **0x414** | 2B | X87_DS |
| **0x416** | 2B | X87_CS |
| **0x418** | 8B | X87_RIP |
| **0x420–0x46F** | 80B | FPREG_X87 (x87 寄存器栈) |
| **0x470–0x56F** | 256B | FPREG_XMM (XMM0–XMM15) |
| **0x570–0x66F** | 256B | FPREG_YMM (YMM_HI0–YMM_HI15) |
| **0x670–0x76F** | 256B | LBR_STACK (FROM/TO 对) |
| **0x770** | 8B | LBR_SELECT |
| **0x778** | 8B | IBS_FETCH_CTL |
| **0x780** | 8B | IBS_FETCH_LINADDR |
| **0x788** | 8B | IBS_OP_CTL |
| **0x790** | 8B | IBS_OP_RIP |
| **0x798** | 8B | IBS_OP_DATA |
| **0x7A0** | 8B | IBS_OP_DATA2 |
| **0x7A8** | 8B | IBS_OP_DATA3 |
| **0x7B0** | 8B | IBS_DC_LINADDR |
| **0x7B8** | 8B | BP_IBSTGT_RIP |
| **0x7C0** | 8B | IC_IBS_EXTD_CTL |
| **0x7C8+** | — | 保留 |

---

## 6. SVM #VMEXIT 退出码完整表

### 6.1 退出码概述

VMCB 控制区域的 `EXITCODE` 字段 (偏移 0x070, 64 位) 包含 #VMEXIT 的原因。以下是所有硬件定义退出码的完整列表。

### 6.2 CR 读拦截 (0x00–0x0F)

| 退出码 | 名称 | 描述 |
|:-----:|:----:|------|
| **0x00** | VMEXIT_CR0_READ | CR0 读 |
| **0x01** | VMEXIT_CR1_READ | CR1 读 |
| **0x02** | VMEXIT_CR2_READ | CR2 读 |
| **0x03** | VMEXIT_CR3_READ | CR3 读 |
| **0x04** | VMEXIT_CR4_READ | CR4 读 |
| **0x05** | VMEXIT_CR5_READ | CR5 读 |
| **0x06** | VMEXIT_CR6_READ | CR6 读 |
| **0x07** | VMEXIT_CR7_READ | CR7 读 |
| **0x08** | VMEXIT_CR8_READ | CR8 读 |
| **0x09** | VMEXIT_CR9_READ | CR9 读 |
| **0x0A** | VMEXIT_CR10_READ | CR10 读 |
| **0x0B** | VMEXIT_CR11_READ | CR11 读 |
| **0x0C** | VMEXIT_CR12_READ | CR12 读 |
| **0x0D** | VMEXIT_CR13_READ | CR13 读 |
| **0x0E** | VMEXIT_CR14_READ | CR14 读 |
| **0x0F** | VMEXIT_CR15_READ | CR15 读 |

### 6.3 CR 写拦截 (0x10–0x1F)

| 退出码 | 名称 | 描述 |
|:-----:|:----:|------|
| **0x10** | VMEXIT_CR0_WRITE | CR0 写 |
| **0x11** | VMEXIT_CR1_WRITE | CR1 写 |
| **0x12** | VMEXIT_CR2_WRITE | CR2 写 |
| **0x13** | VMEXIT_CR3_WRITE | CR3 写 |
| **0x14** | VMEXIT_CR4_WRITE | CR4 写 |
| **0x15** | VMEXIT_CR5_WRITE | CR5 写 |
| **0x16** | VMEXIT_CR6_WRITE | CR6 写 |
| **0x17** | VMEXIT_CR7_WRITE | CR7 写 |
| **0x18** | VMEXIT_CR8_WRITE | CR8 写 |
| **0x19** | VMEXIT_CR9_WRITE | CR9 写 |
| **0x1A** | VMEXIT_CR10_WRITE | CR10 写 |
| **0x1B** | VMEXIT_CR11_WRITE | CR11 写 |
| **0x1C** | VMEXIT_CR12_WRITE | CR12 写 |
| **0x1D** | VMEXIT_CR13_WRITE | CR13 写 |
| **0x1E** | VMEXIT_CR14_WRITE | CR14 写 |
| **0x1F** | VMEXIT_CR15_WRITE | CR15 写 |

### 6.4 DR 读拦截 (0x20–0x2F)

| 退出码 | 名称 | 描述 |
|:-----:|:----:|------|
| **0x20** | VMEXIT_DR0_READ | DR0 读 |
| **0x21** | VMEXIT_DR1_READ | DR1 读 |
| **0x22** | VMEXIT_DR2_READ | DR2 读 |
| **0x23** | VMEXIT_DR3_READ | DR3 读 |
| **0x24** | VMEXIT_DR4_READ | DR4 读 |
| **0x25** | VMEXIT_DR5_READ | DR5 读 |
| **0x26** | VMEXIT_DR6_READ | DR6 读 |
| **0x27** | VMEXIT_DR7_READ | DR7 读 |
| **0x28–0x2F** | VMEXIT_DR8_READ – VMEXIT_DR15_READ | DR8–DR15 读 |

### 6.5 DR 写拦截 (0x30–0x3F)

| 退出码 | 名称 | 描述 |
|:-----:|:----:|------|
| **0x30** | VMEXIT_DR0_WRITE | DR0 写 |
| **0x31** | VMEXIT_DR1_WRITE | DR1 写 |
| **0x32** | VMEXIT_DR2_WRITE | DR2 写 |
| **0x33** | VMEXIT_DR3_WRITE | DR3 写 |
| **0x34–0x3F** | VMEXIT_DR4_WRITE – VMEXIT_DR15_WRITE | DR4–DR15 写 |

### 6.6 异常拦截 (0x40–0x5F)

| 退出码 | 位址 | 异常 | 描述 |
|:-----:|:----:|:----:|------|
| **0x40** | 0 | #DE | 除法错误 |
| **0x41** | 1 | #DB | 调试 |
| **0x42** | 2 | — | 向量 2 (保留; 应使用 NMI 拦截) |
| **0x43** | 3 | #BP | 断点 (INT3) |
| **0x44** | 4 | #OF | 溢出 (INTO) |
| **0x45** | 5 | #BR | BOUND Range Exceeded |
| **0x46** | 6 | #UD | 非法操作码 |
| **0x47** | 7 | #NM | 设备不可用 |
| **0x48** | 8 | #DF | 双重故障 |
| **0x49** | 9 | — | 保留 |
| **0x4A** | 10 | #TS | 无效 TSS |
| **0x4B** | 11 | #NP | 段不存在 |
| **0x4C** | 12 | #SS | 栈段故障 |
| **0x4D** | 13 | #GP | 通用保护 |
| **0x4E** | 14 | #PF | 页故障 |
| **0x4F** | 15 | — | 保留 |
| **0x50** | 16 | #MF | x87 FPU 浮点错误 |
| **0x51** | 17 | #AC | 对齐检查 |
| **0x52** | 18 | #MC | 机器检查 |
| **0x53** | 19 | #XF | SIMD 浮点异常 |
| **0x54** | 20 | — | 保留 (Intel 为 #VE) |
| **0x55** | 21 | #CP | 控制保护异常 (CET) |
| **0x56–0x5F** | 22–31 | — | 保留 |

### 6.7 通用拦截 (0x60–0x8F)

| 退出码 | 拦截名称 | EXITINFO1 | EXITINFO2 |
|:-----:|:---------:|:---------:|:---------:|
| **0x60** | INTR (外部中断) | 0 | 0 |
| **0x61** | NMI | 0 | 0 |
| **0x62** | SMI | 0 | 0 |
| **0x63** | INIT | 0 | 0 |
| **0x64** | VINTR (虚拟中断) | 0 | 0 |
| **0x65** | CR0_SEL_WRITE | 0 | 0 |
| **0x66** | IDTR_READ | 0 | 0 |
| **0x67** | IDTR_WRITE | 0 | 0 |
| **0x68** | GDTR_READ | 0 | 0 |
| **0x69** | GDTR_WRITE | 0 | 0 |
| **0x6A** | LDTR_READ | 0 | 0 |
| **0x6B** | LDTR_WRITE | 0 | 0 |
| **0x6C** | TR_READ | 0 | 0 |
| **0x6D** | TR_WRITE | 0 | 0 |
| **0x6E** | RDTSC | 0 | 0 (参见 DecodeAssist) |
| **0x6F** | RDPMC | 0 | 0 |
| **0x70** | PUSHF | 0 | 0 |
| **0x71** | POPF | 0 | 0 |
| **0x72** | CPUID | 0 | 0 |
| **0x73** | RSM | 0 | 0 |
| **0x74** | IRET | 0 | 0 |
| **0x75** | INTn (软件中断) | 中断向量 | 0 |
| **0x76** | INVD | 0 | 0 |
| **0x77** | PAUSE | 0 | 0 |
| **0x78** | HLT | 0 | 0 |
| **0x79** | INVLPG | 0 | 0 (参见 DecodeAssist) |
| **0x7A** | INVLPGA | 0 | 0 |
| **0x7B** | **IOIO_PROT** | **见下方详解** | 下条指令 RIP |
| **0x7C** | **MSR_PROT** | **0=RDMSR, 1=WRMSR** | **0** |
| **0x7D** | TASK_SWITCH | 0 | 0 |
| **0x7E** | FERR_FREEZE | 0 | 0 |
| **0x7F** | SHUTDOWN | 0 | 0 |
| **0x80** | VMRUN | 0 | 0 |
| **0x81** | VMMCALL | RFLAGS | 0 |
| **0x82** | VMLOAD | 0 | 0 |
| **0x83** | VMSAVE | 0 | 0 |
| **0x84** | STGI | 0 | 0 |
| **0x85** | CLGI | 0 | 0 |
| **0x86** | SKINIT | 0 | 0 |
| **0x87** | RDTSCP | 0 | 0 |
| **0x88** | ICEBP | 0 | 0 |
| **0x89** | WBINVD | 0 | 0 |
| **0x8A** | MONITOR | 0 | 0 |
| **0x8B** | MWAIT (无条件) | 0 | 0 |
| **0x8C** | MWAIT (已触发) | 0 | 0 |
| **0x8D** | XSETBV | XCR 号 | 0 |
| **0x8E** | RDPRU | ECX 输入 | 0 |
| **0x8F** | EFER_WRITE_TRAP | **见下方** | 0 |
| **0x90** | CR0_WRITE_TRAP | **见下方** | 0 |
| **0x91** | CR1_15_WRITE_TRAP | **见下方** | 0 |
| **0x92** | CR2_15_WRITE_TRAP | **见下方** | 0 |
| **0x93** | CR3_WRITE_TRAP | **见下方** | 0 |
| **0x94** | CR4_WRITE_TRAP | **见下方** | 0 |
| **0x95** | CR5_15_WRITE_TRAP | **见下方** | 0 |
| **0x96** | INVLPGB | 0 | 0 |
| **0x97** | INVPCID | 0 | 0 |
| **0x98** | MCOMMIT | 0 | 0 |
| **0x99** | TLBSYNC | 0 | 0 |

#### 6.7.1 IOIO_PROT (0x7B) EXITINFO1 格式

EXITINFO1 位布局 (64 位):

| 位域 | 大小 | 助记符 | 描述 |
|:----:|:----:|:------:|------|
| 63:48 | 16 | PORT | 拦截的 I/O 端口号 |
| 47:44 | 4 | BRP | I/O 断点匹配 |
| 43 | 1 | TF | EFLAGS.TF 值 |
| 42 | 1 | — | 保留 |
| 41 | 1 | A64 | 64 位地址大小 |
| 40 | 1 | A32 | 32 位地址大小 |
| 39 | 1 | A16 | 16 位地址大小 |
| 38 | 1 | SZ32 | 32 位操作数大小 |
| 37 | 1 | SZ16 | 16 位操作数大小 |
| 36 | 1 | SZ8 | 8 位操作数大小 |
| 35 | 1 | REP | REP 前缀 |
| 34 | 1 | STR | 字符串 I/O (INS/OUTS) |
| 33 | 1 | VAL | I/O 信息有效 |
| 32 | 1 | TYPE | 访问类型: 0=OUT, 1=IN |
| 31:13 | 19 | — | 保留 |
| 12:10 | 3 | SEG | 有效段号 (需 DecodeAssist) |
| 9:2 | 8 | — | 保留 |
| 1 | 1 | — | SMI 相关 |
| 0 | 1 | TYPE_L | 访问类型低: 0=OUT, 1=IN |

#### 6.7.2 写陷阱 (WRITE_TRAP) EXITINFO1 格式

| 位域 | 描述 |
|:----:|------|
| 63:0 | 写入寄存器的值 (对于 CR 写陷阱，包含写入的值) |

对于 EFER_WRITE_TRAP，EXITINFO1 包含尝试写入 EFER 的值。

### 6.8 扩展退出码 (0x400+)

| 退出码 | 名称 | 描述 | EXITINFO1 | EXITINFO2 |
|:-----:|:----:|------|:---------:|:---------:|
| **0x400** | **NPF** | 嵌套页故障 | **见 7.6 节** | 故障 GPA |
| **0x401** | AVIC_INCOMPLETE_IPI | AVIC 不完整 IPI | 子类型码 | — |
| **0x402** | AVIC_NOACCEL | AVIC 无法加速的访问 | VAPIC 偏移 | 0 |
| **0x403** | VMGEXIT | VMGEXIT (SEV-ES) | — | — |
| **0x404** | BUS_LOCK | 总线锁阈值 | 0 | 0 |
| **0x405** | V_INTR_INJECT | 虚拟中断注入 | — | — |

#### 6.8.1 AVIC_INCOMPLETE_IPI EXITINFO1 子码

| 值 | 描述 |
|:--:|------|
| 1 | 无效的中断类型 |
| 2 | 目标未在运行 |
| 3 | 无效的目标 |
| 4 | 无效的后备页 |
| 5 | 无效的 IPI 向量 (较新的处理器) |

### 6.9 VMRUN 故障退出码 (64 位负数)

当 VMRUN 因无效状态而失败时（不是真正的 #VMEXIT，而是 VMRUN 指令本身失败），EXITCODE 包含以下 64 位负数之一：

| 退出码 (64位) | 常量名 | 描述 |
|:------------:|:-------:|------|
| `0xFFFFFFFFFFFFFFFF` (–1) | VMEXIT_INVALID | VMCB 中无效的 Guest 状态 |
| `0xFFFFFFFFFFFFFFFE` (–2) | VMEXIT_BUSY | VMSA 的 BUSY 位已设置 |
| `0xFFFFFFFFFFFFFFFD` (–3) | VMEXIT_IDLE_REQUIRED | 兄弟线程未处于空闲状态 |
| `0xFFFFFFFFFFFFFFFC` (–4) | VMEXIT_INVALID_PMC | 无效的 PMC 状态 |

---

## 7. 嵌套分页 (NPT)

### 7.1 NPT 概述

Nested Page Tables (NPT) 是 AMD 的硬件辅助二级地址转换实现。它将 **客户物理地址 (GPA)** 转换为 **主机物理地址 (HPA)**。

- CPUID 标志: `Fn8000_000A_EDX[NP]` (位 0)
- 启用: VMCB 控制区域 NPT 控制位
- 根指针: VMCB 控制区域 `N_CR3` 字段 (偏移 0x090)

### 7.2 NPT 页表层级结构

NPT 使用与 AMD64 长模式页表相同的 4 级结构:

| 级别 | 名称 | 虚拟地址位 | 每项覆盖 | 每项大小 |
|:---:|:----:|:---------:|:--------:|:--------:|
| 4 | PML4 (nPML4E) | 47:39 | 512 GB | 512 项 × 8B |
| 3 | PDP (nPDPE) | 38:30 | 1 GB | 512 项 × 8B |
| 2 | PD (nPDE) | 29:21 | 2 MB | 512 项 × 8B |
| 1 | PT (nPTE) | 20:12 | 4 KB | 512 项 × 8B |

### 7.3 NPT 表项格式 (64 位)

```
63    62 61 60 59  52  51                         12 11  9  8  7  6  5  4  3  2  1  0
+------+--+--+--+------+-----------------------------+-----+--+--+--+--+--+--+--+--+--+
|  NX  | R | X | P | RSVD |   物理地址 [51:12]       | AVL |G |PS| D | A |CD|WT|U/S|W | P |
|      |SVD| D |K |      |   (下一级表或页的PFN)      |[11:9]|  |  |   |   |  |  |   |  |   |
+------+--+--+--+------+-----------------------------+-----+--+--+--+--+--+--+--+--+--+
```

### 7.4 NPT 位定义

| 位 | 名称 | 描述 |
|:-:|:----:|------|
| **0** | **P** (Present) | 1=有效, 0=不存在 (所有其他位被忽略) |
| **1** | **R/W** (Read/Write) | 1=允许写入, 0=只读 |
| **2** | **U/S** (User/Supervisor) | **NPT 必须设为 1** (所有 NPT 遍历被视为用户级访问) |
| **3** | **PWT** (Page-Level Writethrough) | 缓存控制提示 |
| **4** | **PCD** (Page-Level Cache Disable) | 缓存控制提示 |
| **5** | **A** (Accessed) | 由硬件在访问 (读/写) 时设置。硬件从不清除 |
| **6** | **D** (Dirty) | 由硬件在页面被写入时设置。仅在叶表项有效 |
| **7** | **PS** (Page Size) / PAT | PDE 中: 1=2MB 大页 (叶), 0=指向下一级表。PTE 中作 PAT |
| **8** | **G** (Global) | 全局页面提示 |
| **11:9** | **AVL** | 软件可用，硬件忽略 |
| **51:12** | **PhysAddr** | 物理页号 (PFN)，支持最多 52 位物理地址 |
| **59** | **PK** (Protection Key) | 保护键 (较新处理器) |
| **60** | **NX** (No-Execute) | 1=禁止取指，0=允许执行 |
| **61** | **PK_WD** (Protection Key Write Disable) | 保护键写禁用 (较新处理器) |
| **62** | — | 保留 |
| **63** | **NX** (No-Execute) | 1=禁止执行 (与位 60 冗余; 实际使用位 63) |

### 7.5 NPT 权限模型

NPT 权限与客户页表权限是 **与 (AND)** 关系。访问必须同时通过 **两者** 的检查：

| 客户页表 | NPT | 有效 |
|:-------:|:---:|:----:|
| R/W | R/W | R/W |
| R/W | R-O | R-O |
| R-O | R/W | R-O |
| R-O | R-O | R-O |

> **重要:**
> - NPT 的 U/S 位必须始终为 1 (用户级)
> - NPT 权限是"超级集"：NPT 限制访问，但不放宽客户页表的限制
> - 如果 NPT 表项不存在 (P=0)，则立即触发 NPF (#VMEXIT 0x400)

### 7.6 NPF (Nested Page Fault) — EXITINFO1/2 格式

#### EXITINFO1 (64 位)

**低 32 位 [31:0] — 错误码 (类似传统页故障):**

| 位 | 助记符 | 描述 |
|:-:|:-----:|------|
| 0 | **P** | 0=页面不存在, 1=违反保护 |
| 1 | **R/W** | 0=读访问, 1=写访问 |
| 2 | **U/S** | 0=监管者访问, 1=用户访问 |
| 3 | **RSV** | 1=保留位违反 |
| 4 | **ID** | 1=指令取指导致故障 |
| 5 | **PK** | 1=页保护键导致故障 |
| 6 | **SS** | 1=影子栈访问导致故障 |
| 7–31 | — | 保留 |

**高 32 位 [63:32] — NPT 限定符:**

| 位 | 助记符 | 描述 |
|:-:|:-----:|------|
| 32 | **FINAL_GPA** | 1=故障在翻译客户最终物理地址时发生 |
| 33 | **PT_WALK** | 1=故障在翻译客户页表时发生 |
| 34 | **ENC** | 1=客户 C 位为 1 (SEV-SNP) |
| 35 | **SIZEM** | 1=大小不匹配故障 (SEV-SNP RMP) |
| 36 | **VMPL** | 1=VMPL 权限检查失败 |
| 37–63 | — | 保留 |

> **注意:** 位 32 和 33 互斥 — 指示 NPT 遍历在何处发生故障。

#### EXITINFO2 (64 位)

| 字段 | 描述 |
|:----:|------|
| EXITINFO2 | 导致嵌套页故障的 **客户物理地址 (GPA)** |

### 7.7 NPT 遍历流程

NPT 遍历完整流程:

1. 使用 **gCR3** (客户 CR3) 作为 GPA → NPT 翻译 → 得到 HPA → 定位客户 PML4 表
2. 使用客户 VA [47:39] 索引客户 PML4 → 得到 gPML4E (这是一个 GPA)
3. NPT 翻译 gPML4E → HPA → 定位客户 PDP 表
4. 使用客户 VA [38:30] → 得到 gPDPE (GPA)
5. NPT 翻译 gPDPE → HPA → 定位客户 PD 表
6. 使用客户 VA [29:21] → 得到 gPDE (GPA)
7. NPT 翻译 gPDE → HPA → 定位客户 PT 表
8. 使用客户 VA [20:12] → 得到 gPTE (GPA)
9. NPT 翻译 gPTE → HPA (最终物理地址)
10. 使用客户 VA [11:0] 作为页内偏移

每个步骤中，如果 NPT 翻译失败（页面不存在或保留位违规），立即生成 #VMEXIT(NPF)。

### 7.8 大页支持

| 页面大小 | 级别 | PS 位 |
|:-------:|:----:|:-----:|
| 4 KB | PT (级别 1) | — |
| 2 MB | PD (级别 2) | PS=1 |
| 1 GB | PDP (级别 3) | PS=1 |

### 7.9 TLB 管理

- **INVLPGA** — 按虚拟地址和 ASID 失效 TLB 项
- **VMCB.TLB_CONTROL** — 控制 VMRUN 时的 TLB 刷新行为（参见第 12 节）
- **ASID** — 地址空间标识符，用于标记 TLB 项避免不必要的刷新

---

## 8. MSR 拦截 (MSRPM)

### 8.1 MSR 权限映射表结构

MSRPM (MSR Permission Map) 是一个 **8KB (2 页)** 的位图，用于控制对 MSR 的 `RDMSR` 和 `WRMSR` 拦截。

- **总大小:** 8192 字节 (8KB)
- **对齐要求:** 4KB 对齐
- **VMCB 字段:** `MSRPM_BASE_PA` (偏移 0x038)
- **内存类型:** 必须是 Writeback (WB)

### 8.2 三个 MSR 范围

MSRPM 将 MSR 地址空间分为三个非连续范围，每个范围映射到 2KB 子位图:

| 范围索引 | 基址 MSR | 名称 |
|:-------:|:--------:|:----:|
| 0 | `0x00000000` | 标准 MSR |
| 1 | `0xC0000000` | x2APIC/扩展 MSR |
| 2 | `0xC0010000` | AMD 特定 MSR |

每个范围覆盖 **8192 个 MSR 槽位**:
```
MSRS_IN_RANGE = (2048 字节 × 8 位/字节) / 2 位每 MSR = 8192
```

### 8.3 2KB 子位图布局

每个 2KB 子位图是一个扁平位数组:

```
字节 0:   MSR_base+0  (位 1:0 = 读, 写)
          MSR_base+1  (位 3:2)
          MSR_base+2  (位 5:4)
          MSR_base+3  (位 7:6)

字节 1:   MSR_base+4  (位 1:0)
          ...

字节 2047: MSR_base+8188 (位 1:0)
           MSR_base+8189 (位 3:2)
           ...
           MSR_base+8191 (位 15:14) — 最后一个 2 位对
```

### 8.4 位编码

每个 MSR 由 **2 位** 编码:

| 位位置 | 含义 |
|:-----:|:----:|
| 位 0 (LSB) | 1=拦截 RDMSR (读) |
| 位 1 (MSB) | 1=拦截 WRMSR (写) |

| 二进制值 | 行为 |
|:-------:|:----:|
| `00` | 无拦截 (直通) |
| `01` | 仅拦截读 |
| `10` | 仅拦截写 |
| `11` | 拦截读和写 (默认) |

### 8.5 偏移计算公式

对于给定的 MSR 地址，位位置计算如下:

```c
for each range i:
    if (msr >= msrpm_ranges[i] && msr < msrpm_ranges[i] + MSRS_IN_RANGE)
        msr_offset = (i * MSRS_IN_RANGE + (msr - msrpm_ranges[i])) * 2
        base       = msrpm + (msr_offset / 32)  // 32 位 dword 偏移
        shift      = msr_offset % 32             // dword 内的位位置
```

### 8.6 内存布局

```
偏移 0x000 – 0x7FF (2KB):  范围 0 – MSRs 0x0000 – 0x1FFF
偏移 0x800 – 0xFFF (2KB):  范围 1 – MSRs 0xC0000000 – 0xC0001FFF
偏移 0x1000 – 0x17FF (2KB): 范围 2 – MSRs 0xC0010000 – 0xC0011FFF
偏移 0x1800 – 0x1FFF (2KB): 保留 (实践中设为 0)
```

### 8.7 MSR 拦截 EXITINFO1

对于退出码 `VMEXIT_MSR` (0x7C):

- **EXITINFO1 = 0:** 拦截的指令是 RDMSR
- **EXITINFO1 = 1:** 拦截的指令是 WRMSR

---

## 9. I/O 拦截 (IOPM)

### 9.1 I/O 权限映射表结构

IOPM (I/O Permission Map) 是一个位图，用于控制对 I/O 端口 `IN`/`OUT` 指令的拦截。

- **总大小:** 12 KB (3 × 4KB 页)
- **对齐要求:** 4KB 对齐
- **VMCB 字段:** `IOPM_BASE_PA` (偏移 0x030)
- **覆盖范围:** I/O 端口 0–65538

### 9.2 位图布局

```
页 0 (0x0000 – 0x0FFF):  位  0 – 32767  (端口 0x0000 – 0x7FFF)
页 1 (0x1000 – 0x1FFF):  位 32768 – 65535 (端口 0x8000 – 0xFFFF)
页 2 (0x2000 – 0x2000):  位 65536 – 65538 (端口 0x10000 – 0x10002)
                          (仅前 3 字节，其余未使用)
```

### 9.3 端口到位映射

- **位 N 对应 I/O 端口 N**
- **位值 = 1:** 对该端口的访问被拦截 (#VMEXIT)
- **位值 = 0:** 对该端口的访问被允许 (直通)

### 9.4 IOPM 地址验证

如果 IOPM 表最后一个字节的地址 >= 处理器支持的最大物理地址，则 VMRUN 将产生 #VMEXIT(VMEXIT_INVALID)。

### 9.5 IOIO 拦截格式

参见第 6.7.1 节了解 IOIO_PROT 的 EXITINFO1 格式。

---

## 10. 事件注入 (EVENTINJ)

### 10.1 EVENTINJ 字段格式

VMCB 控制区域的 **EVENTINJ** 字段 (偏移 0x0A8, 64 位) 用于向客户机注入事件（中断、异常等）。

### 10.2 位布局

```
63 56 55  48 47   40 39  36 35   32 31    12 11 10   8 7    0
+------+-------+-------+------+--------+--------+------+-------+
| RSVD | RC    | IV    | RSVD | Error  | EV |TY | Vector |
|      |       |       |      | Code   |    |PE |        |
+------+-------+-------+------+--------+------+---+--------+
```

| 位域 | 大小 | 助记符 | 描述 |
|:----:|:----:|:------:|------|
| **7:0** | 8 | Vector | 向量号 (中断/异常号) |
| **10:8** | 3 | Type | 类型 (见下方) |
| **11** | 1 | EV | 错误码有效 |
| **31:12** | 20 | ErrorCode | 错误码 (如果 EV=1) |
| **39:32** | 8 | IVL | 注入向量低位 |
| **40** | 1 | Valid (V) | **必须为 1 以执行注入** |
| **63:41** | 23 | — | 保留 (MBZ) |

### 10.3 Type 字段编码 (位 10:8)

| 值 | 类型 | 描述 |
|:-:|:----:|------|
| **0** | EXTERNAL | 外部 (固定) 中断 |
| **1** | — | 保留 |
| **2** | NMI | 不可屏蔽中断 |
| **3** | EXCEPTION | 异常 (故障/陷阱) |
| **4** | SW_INT | 软件中断 (INT n) |
| 5 | — | 保留 |
| 6 | — | 保留 |
| 7 | — | 保留 |

### 10.4 注入约束

- **Valid (V) 位** 必须为 1。如果 V=0，硬件忽略 EVENTINJ
- **错误码:** 如果异常类型需要错误码 (#GP, #PF, #TS, #NP, #SS, #AC, #DF) 且 EV=1，则硬件推送错误码。如果 EV=0，硬件不推送错误码
- **注入时机:** 在 VMRUN 执行后、执行任何客户指令之前检查 EVENTINJ
- **不重新拦截:** 硬件注入的事件不会再次触发拦截（除非客户机在传递过程中发生新的异常）

### 10.5 使用示例

```c
// 注入 #GP(0) 到客户机:
VMCB->EVENTINJ = (0x0D << 0)   |    // Vector = #GP (13)
                 (3    << 8)   |    // Type = Exception
                 (1    << 11)  |    // EV = 1 (有效错误码)
                 (0    << 12)  |    // ErrorCode = 0
                 (1    << 40);      // Valid = 1
```

---

## 11. VMCB Clean Bits

### 11.1 Clean Bits 字段位置

| 字段 | 偏移 | 位 | 描述 |
|:----:|:----:|:-:|------|
| **VMCB Clean Bits** | 0x0C0 | 31:0 | 指示哪些 VMCB 字段是"干净的"（无需重写） |
| | 0x0C0 | 63:32 | **保留 (SBZ)** |

### 11.2 Clean Bits 定义

| 位 | 常量名 | 覆盖的字段 |
|:-:|:------:|:----------:|
| **0** | VMCB_INTERCEPTS | 拦截向量、TSC 偏移、暂停过滤器计数 |
| **1** | VMCB_PERM_MAP | IOPM 基址和 MSRPM 基址 |
| **2** | VMCB_ASID | ASID |
| **3** | VMCB_INTR | int_ctl, int_vector (中断相关状态, 0x60–0x67) |
| **4** | VMCB_NPT | npt_en, nCR3, gPAT (嵌套分页状态) |
| **5** | VMCB_CR | CR0, CR3, CR4, EFER |
| **6** | VMCB_DR | DR6, DR7 |
| **7** | VMCB_DT | GDT, IDT (基址和限制) |
| **8** | VMCB_SEG | CS, DS, SS, ES, CPL |
| **9** | VMCB_CR2 | CR2 |
| **10** | VMCB_LBR | DBGCTL, BR_FROM, BR_TO, LAST_EX_FROM, LAST_EX_TO |
| **11** | VMCB_AVIC | AVIC APIC_BAR, AVIC 后备页, AVIC 物理/逻辑表 |
| **12–30** | — | 保留 |
| **31** | VMCB_SW | 为 Hypervisor/软件保留 |

### 11.3 工作原理

- 当 Clean 位 = **1**: 指示对应的 VMCB 字段组自上次 VMRUN 以来未被修改。硬件可以跳过重新加载该状态，减少 VM 进入开销
- 当 Clean 位 = **0**: 指示对应的 VMCB 字段已被修改，硬件必须重新加载
- Hypervisor **修改了相应字段时必须清除** Clean 位

### 11.4 全 Clean 掩码

```c
#define VMCB_ALL_CLEAN_MASK (                \
    (1 << 0)  |  // VMCB_INTERCEPTS          \
    (1 << 1)  |  // VMCB_PERM_MAP            \
    (1 << 2)  |  // VMCB_ASID                \
    (1 << 3)  |  // VMCB_INTR                \
    (1 << 4)  |  // VMCB_NPT                 \
    (1 << 5)  |  // VMCB_CR                  \
    (1 << 6)  |  // VMCB_DR                  \
    (1 << 7)  |  // VMCB_DT                  \
    (1 << 8)  |  // VMCB_SEG                 \
    (1 << 9)  |  // VMCB_CR2                 \
    (1 << 10) |  // VMCB_LBR                 \
    (1 << 11) |  // VMCB_AVIC                \
    (1 << 31))   // VMCB_SW
```

该掩码的值为 `0x80000FFF`。

---

## 12. ASID — 地址空间标识符

### 12.1 ASID 概念

ASID (Address Space Identifier) 是 AMD SVM 用于标记 TLB 项以区分不同地址空间的机制。

- **ASID 0** — 通常为主机 (Hypervisor) 保留
- **ASID 1+** — 分配给客户 VM
- **最大 ASID 数量** — 通过 `CPUID Fn8000_000A_EBX` 获取
- **功能:** TLB 项带有 ASID 标签，避免在 VM 进入/退出时清空整个 TLB

### 12.2 TLB_CONTROL 字段 (VMCB 偏移 0x05C)

| 值 | 常量名 | 描述 |
|:-:|:------:|------|
| **0** | TLB_CONTROL_DO_NOTHING | 不执行 TLB 刷新 |
| **1** | TLB_CONTROL_FLUSH_ALL | 刷新 **整个** TLB (包括 ASID 0) |
| **3** | TLB_CONTROL_FLUSH_ASID | 仅刷新 **当前 ASID** 的 TLB 项 |
| **7** | TLB_CONTROL_FLUSH_ASID_LOCAL | 仅刷新本地 TLB 中当前 ASID 的项 |

### 12.3 Flush-by-ASID 特性

- **需要:** CPUID Fn8000_000A_EDX[FlushByAsid] = 1
- **引入:** AMD Family 15h (Bulldozer/Piledriver)
- **行为:** 允许按 ASID 精细刷新 TLB，无需清除包含主机 ASID 0 项在内的整个 TLB
- **性能优势:** 保留主机 TLB 项，减少 VM 进入时的 TLB 未命中

### 12.4 ASID 管理策略

```c
// 典型 ASID 分配策略:
if (cpu_has_svm_flushbyasid)
    vmcb->tlb_control = TLB_CTRL_FLUSH_ASID;  // 仅刷新当前 ASID
else
    vmcb->tlb_control = TLB_CTRL_FLUSH_ALL;    // 刷新整个 TLB

// ASID 耗尽时，执行完整刷新并重用 ASID
if (asid_generation_changed) {
    vmcb->tlb_control = TLB_CTRL_FLUSH_ALL;
    // 重新分配 ASID 生成号
}
```

---

## 13. SVM 锁与安全机制

### 13.1 SVM Lock 概述

AMD SVM 提供了硬件级锁定机制，防止未经授权的软件启用虚拟化（"Blue Pill" 攻击）。

### 13.2 检测算法

检测 SVM 可用性的完整算法 (AMD APM Vol. 2, Section 15.4):

```
if (CPUID 8000_0001.ECX[SVM] == 0)
    return SVM_NOT_AVAILABLE;

if (VM_CR.SVMDIS == 0)
    return SVM_AVAILABLE;

if (CPUID 8000_000A.EDX[SVM_LOCK] == 0)
    return SVM_DISABLED_AT_BIOS_CANNOT_UNLOCK;
else
    return SVM_DISABLED_WITH_KEY;
```

### 13.3 VM_CR.LOCK 位 (位 3)

- 设置 LOCK 后，对 LOCK 位和 SVME_DISABLE 位的写入被 **静默忽略**
- LOCK 只能通过向 **SVM_KEY MSR** (0xC0010118) 写入正确的密钥来清除
- INIT 或 SKINIT **不会** 影响 LOCK 位

### 13.4 VM_CR.SVME_DISABLE 位 (位 4)

- 设置后，写入 EFER MSR 将 **SVME 位 (位 12) 视为 MBZ (必须为零)**
- 在 EFER.SVME=1 时设置 SVMDIS 会导致 `#GP` (通用保护) 异常
- INIT 仅在 LOCK=0 时清除 SVMDIS

### 13.5 SVM_KEY MSR (0xC0010118)

- 写入正确的密钥可清除 VM_CR.LOCK
- 密钥值特定于平台，通常需要与 BIOS 或 TPM 交互才能获取

### 13.6 安全影响

- 现代系统 (带 Microsoft 安全级别或类似平台安全功能) 在启动时锁定 SVM
- CPUID **仍然报告 SVM 可用**，即使 SVMDIS 已设置
- 仅检查 CPUID 是不够的 — Hypervisor 必须检查 VM_CR
- **嵌套虚拟化** 可能在锁定系统上失败，因为 L1 Hypervisor 无法写入 EFER.SVME

---

## 14. VM_CR — 虚拟机控制寄存器

### 14.1 VM_CR MSR (0xC0010114)

**地址:** `0xC0010114`  
**名称:** Virtual Machine Control Register

### 14.2 位布局

| 位 | 名称 | 类型 | 重置值 | 描述 |
|:-:|:----:|:----:|:-----:|------|
| **0** | DPD | R/W | 0 | Debug Port Disable — 调试端口禁用 |
| **1** | R_INIT | R/W | 0 | Intercept INIT — 拦截 INIT 信号 |
| **2** | DIS_A20M | R/W | 0 | Disable A20 Masking — 禁用 A20 地址掩码 |
| **3** | **LOCK** | R/W | 0 | SVM Lock — 设置后锁定位 3 和 4 |
| **4** | **SVME_DISABLE** | R/W | 0 | SVM Disable — 禁止通过 EFER.SVME 启用 SVM |
| **63:5** | — | R | 0 | 保留 |

### 14.3 各位置详解

**DPD (位 0):**
- 调试端口禁用
- 当设置时，禁用 JTAG/调试端口

**R_INIT (位 1):**
- 拦截 INIT 信号
- 当设置时，INIT 信号在 SVM 启用时被拦截

**DIS_A20M (位 2):**
- 禁用 A20 地址掩码
- 当设置时，A20M 引脚被禁用，A20 掩码功能关闭
- 这防止了实模式地址回绕漏洞

**LOCK (位 3):**
- 参见第 13.3 节

**SVME_DISABLE (位 4):**
- 参见第 13.4 节

---

## 15. SVM 指令集详解

### 15.1 VMRUN (0F 01 D8)

- **操作码:** `0F 01 D8`
- **功能:** 从主机模式切换到客户模式，开始执行客户机代码
- **输入:** RAX = VMCB 物理地址
- **保护:** 需要 EFER.SVME=1，否则 #UD
- **行为:**
  1. 保存主机状态到 VM_HSAVE_PA (RSP, RAX, RIP, RFLAGS, CS, SS, DS, ES)
  2. 从 VMCB 加载客户状态 (CS, SS, DS, ES, RIP, RFLAGS, RSP, RAX, CR0, CR3, CR4, EFER, 段寄存器)
  3. 从 VMCB 检查 Clean Bits 以决定是否跳过加载
  4. 检查 EVENTINJ 以决定是否注入事件
  5. 切换到 Guest Mode 并开始执行
- **故障:** 如果 VMCB 状态无效，VMRUN 本身失败并设置 EXITCODE 为负数 (参见第 6.9 节)

### 15.2 VMLOAD (0F 01 DA)

- **操作码:** `0F 01 DA`
- **功能:** 从 VMCB 加载 FS、GS、TR、LDTR 及其隐藏描述符寄存器状态
- **输入:** RAX = VMCB 物理地址
- **使用时机:** 通常在 VMRUN **之前** 执行，加载额外客户状态
- **拦截:** 退出码 `0x82`

### 15.3 VMSAVE (0F 01 DB)

- **操作码:** `0F 01 DB`
- **功能:** 将 FS、GS、TR、LDTR 及其隐藏描述符寄存器保存到 VMCB
- **输入:** RAX = VMCB 物理地址
- **使用时机:** 通常在 #VMEXIT **之后** 执行，保存额外客户状态
- **拦截:** 退出码 `0x83`

### 15.4 STGI (0F 01 DC)

- **操作码:** `0F 01 DC`
- **功能:** 设置全局中断标志 (GIF=1)，启用中断传递
- **可用条件:** EFER.SVME=1 **或** CPUID 8000_0001.ECX[SKINIT]=1
- **拦截:** 退出码 `0x84`
- **异常顺序:** 先检查 #GP，再检查拦截

### 15.5 CLGI (0F 01 DD)

- **操作码:** `0F 01 DD`
- **功能:** 清除全局中断标志 (GIF=0)，阻止所有中断传递 (包括 NMI、SMI)
- **可用条件:** 需要 EFER.SVME=1
- **拦截:** 退出码 `0x85`
- **行为:** GIF=0 时中断挂起等待，CLGI 本身可被 NMI 中断（在 GIF 设置后服务）

### 15.6 SKINIT (0F 01 DE)

- **操作码:** `0F 01 DE`
- **功能:** 安全启动并跳转，带认证
- **输入:** EAX = SLB (安全加载器块) 物理地址
- **行为:**
  1. 将 CPU 初始化为已知安全状态
  2. 指定一个 64KB 内存区域为 SLB
  3. 向系统 TPM 提交该内存区域的副本进行验证
  4. 跳转到 SLB 开始执行
- **拦截:** 退出码 `0x86`

### 15.7 INVLPGA (0F 01 DF)

- **操作码:** `0F 01 DF`
- **功能:** 按虚拟地址和 ASID 失效 TLB 项
- **输入:** RAX = 虚拟地址, ECX = ASID
- **ASID=0:** 失效所有地址空间的项
- **拦截:** 退出码 `0x7A`
- **作用:** 比 INVLPG 更精确，允许按 ASID 选择性失效

### 15.8 VMMCALL (0F 01 D9)

- **操作码:** `0F 01 D9`
- **功能:** 向 Hypervisor 发起调用（功能上类似 Intel 的 VMX VMCALL）
- **使用:** 客户机通过 VMMCALL 请求 Hypervisor 服务
- **拦截:** 退出码 `0x81`
- **EXITINFO1:** RFLAGS 值

### 15.9 指令编码总结

| 指令 | 操作码 | 隐式寄存器 | 拦截退出码 |
|:----:|:-----:|:---------:|:---------:|
| VMRUN | `0F 01 D8` | RAX (VMCB 物理地址) | `0x80` |
| VMMCALL | `0F 01 D9` | — | `0x81` |
| VMLOAD | `0F 01 DA` | RAX (VMCB 物理地址) | `0x82` |
| VMSAVE | `0F 01 DB` | RAX (VMCB 物理地址) | `0x83` |
| STGI | `0F 01 DC` | — | `0x84` |
| CLGI | `0F 01 DD` | — | `0x85` |
| SKINIT | `0F 01 DE` | EAX (SLB 地址) | `0x86` |
| INVLPGA | `0F 01 DF` | RAX (VA), ECX (ASID) | `0x7A` |

---

## 16. VMCB 状态保存/恢复机制

### 16.1 Host Save Area (VM_HSAVE_PA)

**MSR:** `0xC0010117` — VM_HSAVE_PA

VMRUN 执行时，硬件自动保存以下 **主机状态** 到由 VM_HSAVE_PA 指向的 4KB 页面：

| 偏移 | 大小 | 字段 |
|:----:|:----:|:----:|
| 0x000 | 8B | RSP |
| 0x008 | 8B | RAX |
| 0x010 | 8B | RIP |
| 0x018 | 8B | RFLAGS |
| 0x020 | 8B | CS |
| 0x028 | 8B | (未指定) |
| 0x030 | 8B | (未指定) |
| 0x038 | 8B | (未指定) |
| 0x040 | 8B | (未指定) |
| 0x048 | 8B | (未指定) |
| 0x050 | 8B | (未指定) |
| 0x058 | 8B | (未指定) |

> **注意:** AMD APM 未完整记录主机保存区的完整布局。Hypervisor 必须确保 VM_HSAVE_PA 在 VMRUN 之前已设置，且指向一个有效、清零的物理页。

### 16.2 VMRUN 自动保存/加载的状态

| 操作 | 保存 | 加载 |
|:---:|:----:|:----:|
| **VMRUN** | 主机 → VM_HSAVE_PA (RSP, RAX, RIP, RFLAGS, CS, SS, DS, ES) | 客户 ← VMCB (CS, SS, DS, ES, RIP, RFLAGS, RSP, RAX, CR0, CR3, CR4, EFER, 段寄存器) |
| **#VMEXIT** | 客户 → VMCB | 主机 ← VM_HSAVE_PA (RSP, RAX, RIP, RFLAGS, CS, SS, DS, ES) |

### 16.3 Swap Type B 寄存器 (自动交换)

VMRUN/#VMEXIT 时自动交换的寄存器:

- CR0, CR3, CR4
- EFER
- RFLAGS, RIP, RSP, RAX
- RCX, RDX, RBX, RBP, RSI, RDI, R8–R15
- STAR, LSTAR, CSTAR, SFMASK, KernelGSBase
- SYSENTER_CS, SYSENTER_ESP, SYSENTER_EIP
- CR2
- G_PAT (如果 NPT 启用)
- S_CET, SSP, ISST_ADDR (CET 支持)
- XCR0 (SEV-ES)

### 16.4 Swap Type C 寄存器 (有条件的)

主要在 #VMEXIT 时有条件保存的寄存器:

- DBGCTL, BR_FROM, BR_TO, LASTEXCPFROM, LASTEXCPTO
- DBGEXTNCTL
- FPU/XMM/YMM 状态
- LBR/IBS 状态

---

## 17. AMD-V vs Intel VMX 架构对比

### 17.1 核心架构对比

| 对比项 | Intel VMX | AMD SVM |
|:------:|:---------:|:-------:|
| **控制结构** | VMCS — 通过 VMREAD/VMWRITE 访问 | VMCB — 通过普通内存读写访问 |
| **入口/退出指令** | VMLAUNCH/VMRESUME → VM-Exit | VMRUN → #VMEXIT |
| **根/非根模式** | VMX Root / VMX Non-Root | Host Mode / Guest Mode |
| **硬件辅助分页** | EPT (Extended Page Tables) | NPT (Nested Page Tables) |
| **TLB 标记** | VPID (Virtual Processor ID) | ASID (Address Space ID) |
| **中断虚拟化** | APICv / Posted Interrupts | AVIC (Advanced Virtual Interrupt Controller) |
| **安全加密** | TDX (Trust Domain Extensions) | SEV, SEV-ES, SEV-SNP |

### 17.2 对应关系表

| 功能 | Intel VMX | AMD SVM |
|:----:|:---------:|:-------:|
| 进入客户机 | VMLAUNCH / VMRESUME | VMRUN |
| 退出客户机 | VM-Exit (自动) | #VMEXIT (自动) |
| 读取控制结构 | VMREAD | 直接内存读取 |
| 写入控制结构 | VMWRITE | 直接内存写入 |
| 客户调用 | VMCALL | VMMCALL |
| 页表结构 | EPT (4 级) | NPT (4 级) |
| 二级地址故障 | EPT Violation / Misconfig | NPF (Nested Page Fault) |
| 退出码 | VM-Exit Reason (16 位) | EXITCODE (64 位) |
| MSR 拦截 | MSR 位图 | MSRPM (MSR Permission Map) |
| I/O 拦截 | I/O 位图 | IOPM (I/O Permission Map) |
| 调试寄存器切换 | VMCS debug 字段 | DR 拦截 (0x20–0x3F) |
| ASID/VPID | VPID (16 位) | ASID (32 位) |
| 事件注入 | VM-Entry Event Injection | EVENTINJ 字段 |

### 17.3 主要区别

**VMCB vs VMCS 访问方式:**
- SVM 使用普通内存访问，更简单直接
- VMX 需要特殊指令 (VMREAD/VMWRITE)，保护 VMCS 不被意外修改

**退出码格式:**
- VMX: 16 位退出原因码 + 限定符
- SVM: 64 位 EXITCODE，包括负数错误码

**嵌套虚拟化:**
- VMX: VMCS Shadowing 硬件支持
- SVM: 嵌套 VMCB 映射

**TLB 控制:**
- VMX: VPID + INVVPID
- SVM: ASID + INVLPGA

### 17.4 关键差异表

| 特性 | Intel VMX | AMD SVM |
|:----:|:---------:|:-------:|
| 控制结构访问 | VMREAD/VMWRITE 指令 | 直接内存读写 |
| VM 进入指令 | VMLAUNCH / VMRESUME | VMRUN |
| VM 退出机制 | VM-Exit | #VMEXIT |
| 退出原因宽度 | 16 位 | 64 位 |
| 二级地址转换 | EPT | NPT |
| TLB 标签 | VPID | ASID |
| 中断虚拟化 | APICv | AVIC |
| 安全扩展 | TDX | SEV/SEV-ES/SEV-SNP |
| 客户-Hyperv 调用 | VMCALL | VMMCALL |
| LBR 虚拟化 | VMCS 字段 | LbrVirt 特性 |
| PAUSE 过滤 | PLE (Pause Loop Exiting) | PauseFilter 特性 |

---

## 18. AMD-V 功能演进历程

### 18.1 功能引入时间线

| CPU 家族 | 微架构 | 年份 | 引入的 SVM 特性 |
|:--------:|:------:|:----:|:---------------:|
| **Rev F/G (Family 0Fh)** | K8 | 2006 | 基础 SVM: NP, LbrVirt, SVMLock, NRIPS, TscRateMsr, VmcbClean, FlushByAsid, DecodeAssist |
| **Family 10h** | K10 (Barcelona) | 2007 | NPT 改进，性能优化 |
| **Family 12h** | Llano | 2011 | — |
| **Family 14h** | Bobcat | 2011 | — |
| **Family 15h** | Bulldozer/Piledriver/Steamroller/Excavator | 2011–2015 | **PauseFilter** (位 10), **PauseFilterThreshold** (位 12), **AVIC** (位 13), **V_VMSAVE_VMLOAD** (位 15), **vGIF** (位 16) |
| **Family 16h** | Jaguar | 2013 | — |
| **Family 17h** | Zen/Zen+/Zen 2 | 2017–2019 | **x2AVIC** (位 18), **SEV** (位 30), **SEV-ES** (位 31) |
| **Family 19h** | Zen 3/Vermeer | 2020 | **GMET** (位 17), **SSSCheck** (位 21), **SPEC_CTRL** (位 22), **ROGPT** (位 23), **VNMI** (位 24) |
| **Family 19h (更新)** | Zen 4/Phoenix | 2022 | **IBSVirt** (位 26), **VmcbPermissive** (位 27), **SVME_ADDR_CHK** (位 28), **NestedVirt/SEV-SNP** (位 29) |
| **Family 1Ah** | Zen 5 | 2024–2025 | 进一步改进 SVM，FRED 支持 |

### 18.2 各版本 SVM 修订号

| CPU 家族 | SVM 修订号 (EAX) |
|:--------:|:----------------:|
| Family 0Fh (K8) | 01h |
| Family 10h (K10) | 01h |
| Family 11h (K8 改进) | 01h |
| Family 12h (Llano) | 01h |
| Family 14h (Bobcat) | 01h |
| Family 15h (Bulldozer+) | 01h |
| Family 16h (Jaguar) | 01h |
| Family 17h (Zen) | 01h |
| Family 19h (Zen 3/4) | 01h |

> **注意:** 尽管修订号保持为 01h，后续 CPU 通过 CPUID 8000_000A EDX 的扩展特性位增加新功能。

### 18.3 向后兼容性

SVM 保持高度向后兼容性：
- 旧版 Hypervisor 可在新 CPU 上运行，忽略新特性位
- 所有新特性由 CPUID EDX 位控制
- VMCB 控制区域的保留字段 (SBZ) 确保未来扩展兼容

---

## 19. AVIC — 高级虚拟中断控制器

### 19.1 AVIC 概述

AVIC (Advanced Virtual Interrupt Controller) 是 AMD SVM 的硬件虚拟化 APIC 实现。

- **CPUID:** Fn8000_000A_EDX[AVIC] (位 13)
- **x2AVIC:** Fn8000_000A_EDX[x2AVIC] (位 18)
- **引入:** Family 15h 6Xh (Carrizo) 及更新
- **两种模式:** xAVIC (MMIO 接口) 和 x2AVIC (MSR 接口)

### 19.2 数据结构

AVIC 定义了三个 4KB 数据结构：

**1. Virtual APIC Backing Page (vAPIC 后备页)**
- 每 vCPU 一个
- 保存客户虚拟 APIC 寄存器状态
- 客户对本地 APIC 寄存器 GPA 范围的访问被重定向至此

**2. Physical APIC ID Table (物理 APIC ID 表)**
- 每 VM 一个
- 按客户物理 APIC ID 索引 (xAVIC: 256 项, x2AVIC: 512 项)
- 每项 64 位包含: V (有效位), IR (运行位), 后备页指针, 主机物理 APIC ID

**3. Logical APIC ID Table (逻辑 APIC ID 表)**
- 每 VM 一个
- 映射逻辑 APIC ID 到客户物理 APIC ID

### 19.3 VMCB 字段

| 偏移 | 字段 | 描述 |
|:----:|:----:|:------|
| 0x098 | V_APIC_BAR | 客户 vAPIC 寄存器组的 GPA |
| 0x0E0 | AVIC_BACKING_PAGE | vAPIC 后备页的 HPA |
| 0x0F0 | AVIC_LOGICAL_TABLE | 逻辑 APIC ID 表的 HPA |
| 0x0F8 | AVIC_PHYSICAL_TABLE | 物理 APIC ID 表的 HPA + MAX_INDEX |

### 19.4 中断注入

AVIC 启用时，VMCB 中的 `V_IRQ`, `V_INTR_PRIO`, `V_IGN_TPR`, `V_INTR_VECTOR` 字段被 **忽略**。

### 19.5 退出码

| 退出码 | 名称 | 描述 |
|:-----:|:----:|------|
| **0x401** | AVIC_INCOMPLETE_IPI | IPI 传递失败 |
| **0x402** | AVIC_NOACCEL | 无法加速的 vAPIC 寄存器访问 |

### 19.6 门铃 (Doorbell) 机制

- **MSR C001_011Bh:** Doorbell 寄存器
- 写入目标核心的物理 APIC ID 发送门铃信号
- 门铃信号目标核心评估 vAPIC 状态并注入中断

---

## 20. Decode Assist 机制

### 20.1 Decode Assist 概述

Decode Assist 是 AMD SVM 的一项硬件特性，在 #VMEXIT 时提供额外的指令解码信息，帮助 Hypervisor 快速处理退出而不需要手动解析指令字节。

- **CPUID:** Fn8000_000A_EDX[DecodeAssist] = 1
- **引入:** Rev 1 (所有支持 SVM 的 AMD CPU)

### 20.2 提供的信息

启用 DecodeAssist 后，VMCB 控制区域在 #VMEXIT 时包含以下额外信息：

| VMCB 字段 | 描述 |
|:---------:|:----:|
| **NRIP (偏移 0x0C8)** | 导致 #VMEXIT 的下一条指令的 RIP |
| **GUEST_INSN (偏移 0x0D0)** | 字节计数 (1B) + 最多 15 字节的指令字节 |

### 20.3 NRIP 详细行为

NRIP (Next RIP) 字段在 VMCB 偏移 0x0C8 处，是一个 64 位值。

- **#VMEXIT 时:** 硬件将客户 RIP 之后的指令地址写入 NRIP
- **VMRUN 时:** 如果 NRIP 有效，硬件可以使用它来优化 #VMEXIT 处理
- **需求:** CPUID Fn8000_000A_EDX[NRIPS] (位 3) = 1

### 20.4 NRIP 对不同退出码的影响

| 退出码 | NRIP 行为 |
|:-----:|:---------:|
| **所有通用拦截 (0x60–0x99)** | NRIP 指向导致退出的下一条指令 |
| **IOIO_PROT (0x7B)** | EXITINFO2 = NRIP (下条指令地址) |
| **异常 (0x40–0x5F)** | 对于故障类异常: NRIP = RIP (故障指令重新执行) |
| **异常 (0x40–0x5F)** | 对于陷阱类异常: NRIP = RIP + 指令长度 |

### 20.5 DecodeAssist 用法的完整示例

```c
// DecodeAssist 辅助函数: 计算指令长度
uint32_t svm_insn_length(struct vmcb *vmcb)
{
    if (vmcb->control.nrip) {
        // 如果 NRIP 可用，仅仅需要减法
        return (uint32_t)(vmcb->control.nrip - vmcb->save.rip);
    }
    // 否则需要手动解码 (回退路径)
    uint8_t *insn_bytes = vmcb->control.insn_bytes;
    uint8_t  insn_len   = vmcb->control.insn_len & 0x7F;
    return insn_len;
}

// 使用 DecodeAssist 计算 MOV CR 指令
uint8_t decode_mov_cr(struct vmcb *vmcb, int *is_read, int *cr_num)
{
    uint8_t *insn = vmcb->control.insn_bytes;
    uint8_t  len  = vmcb->control.insn_len & 0x7F;
    
    if (len < 2) return 0;
    
    // MOV CR 指令格式:
    // 0F 20/r: MOV r32/r64, CR  (CR 读)
    // 0F 22/r: MOV CR, r32/r64  (CR 写)
    if (insn[0] == 0x0F) {
        if (insn[1] == 0x20) {
            *is_read = 1;
            *cr_num = insn[2] & 7;
            return 1;
        }
        if (insn[1] == 0x22) {
            *is_read = 0;
            *cr_num = insn[2] & 7;
            return 1;
        }
    }
    return 0;
}
```

## 21. Virtual GIF (vGIF) 机制

### 21.1 概述

Virtual GIF (vGIF) 是 AMD SVM 的一项特性，允许虚拟化全局中断标志 (GIF) 在 Guest 模式下使用。

- **CPUID:** Fn8000_000A_EDX[vGIF] (位 16) = 1
- **引入:** Family 15h (Carrizo/Excavator)
- **启用:** VMCB 控制区域 `VIRT_EXT` 字段中的 V_GIF_ENABLE 位

### 21.2 VMCB 配置

VMCB 控制区域偏移 0x0B8 (VIRT_EXT) 字段:

| 位 | 名称 | 描述 |
|:-:|:----:|------|
| 0 | LBR_VIRT_ENABLE | LBR 虚拟化启用 |
| 1 | V_VMSAVE_VMLOAD_ENABLE | VMSAVE/VMLOAD 虚拟化启用 |
| 2 | V_IBS_ENABLE | IBS 虚拟化启用 |
| **3** | **V_GIF_ENABLE** | **Virtual GIF 启用** |

### 21.3 vGIF 行为

启用 vGIF 时:
- **STGI** 和 **CLGI** 在 Guest 模式下执行成功（而不是 #VMEXIT）
- VMCB 中的虚拟 GIF 状态跟踪 GIF 值
- 物理 GIF 在 #VMEXIT 时自动恢复为主机值
- STGI/CLGI 的拦截仍然可能（如果设置了拦截位）

### 21.4 vGIF 退出码

如果 vGIF 未启用但 Guest 尝试执行 STGI/CLGI:
- 如果设置了拦截: `VMEXIT_STGI` (0x84) / `VMEXIT_CLGI` (0x85)
- 如果未设置拦截: `#UD` (非法操作码)

## 22. VNMI — 虚拟化 NMI

### 22.1 概述

VNMI (Virtual NMI) 允许 Hypervisor 向客户机注入虚拟 NMI，并跟踪 NMI 的屏蔽状态。

- **CPUID:** Fn8000_000A_EDX[VNMI] (位 24) = 1
- **引入:** Zen 3 (Family 19h)
- **启用:** VMCB 控制区域的 V_NMI_ENABLE 位

### 22.2 VMCB V_NMI 字段格式 (偏移 0x068)

V_INTR_STATE 字段 (偏移 0x068) 包含 VNMI 相关位:

| 位 | 名称 | 描述 |
|:-:|:----:|------|
| **0** | INTR_SHADOW | 中断影子 (STI/SS 后的中断屏蔽) |
| **1** | GUEST_INTERRUPT_MASK | Guest RFLAGS.IF 状态 (如果 V_GIF=0) |
| **11** | V_NMI_PENDING | 虚拟 NMI 挂起 |
| **12** | V_NMI_MASK | 虚拟 NMI 屏蔽 |
| **13** | V_NMI_ENABLE | 虚拟 NMI 启用 |
| **14** | V_NMI_MASK_V2 | 虚拟 NMI 屏蔽版本 2 |
| 63:15 | — | 保留 |

### 22.3 VNMI 行为

1. 设置 `V_NMI_ENABLE` 启用 VNMI
2. 设置 `V_NMI_PENDING` 注入虚拟 NMI
3. 硬件在传递 NMI 时自动设置 `V_NMI_MASK`
4. 客户 `IRET` 指令自动清除 `V_NMI_MASK` (如果拦截了 IRET)
5. V_NMI_MASK 防止多个 NMI 同时传递

### 22.4 VNMI 注入示例

```c
// 注入虚拟 NMI:
vmcb->control.v_intr_state |= (1 << 13);  // V_NMI_ENABLE
vmcb->control.v_intr_state |= (1 << 11);  // V_NMI_PENDING

// VMRUN 后，硬件自动传递 NMI 并设置屏蔽
// 如果 IRET 拦截，Hypervisor 应清除 V_NMI_MASK:
if (exit_code == VMEXIT_IRET && (vmcb->control.v_intr_state & (1 << 12)))
    vmcb->control.v_intr_state &= ~(1 << 12);  // 清除 V_NMI_MASK
```

## 23. PAUSE Intercept Filter (暂停拦截过滤器)

### 23.1 概述

PAUSE 拦截过滤器允许 Hypervisor 减少由 PAUSE 指令引起的 #VMEXIT 数量，提高虚拟化 CPU 效率。

- **CPUID 标志:** 
  - PauseFilter (位 10): 支持 PAUSE 拦截过滤
  - PauseFilterThreshold (位 12): 支持 PAUSE 过滤阈值
- **引入:** Family 15h (Bulldozer)

### 23.2 VMCB 字段

| 偏移 | 字段 | 大小 | 描述 |
|:----:|:----:|:----:|------|
| **0x048** | PAUSE_FILTER_COUNT | 2B | 在产生 #VMEXIT 之前允许的 PAUSE 数量 |
| **0x04A** | PAUSE_FILTER_THRESHOLD | 2B | PAUSE 循环的周期计数阈值 |

> **注意:** 在较早的 SVM 修订版中，这些字段位于偏移 0x016–0x019。当前 (Rev 1+) 位置如上所示。

### 23.3 工作原理

1. 设置 `INTERCEPT_VECTOR_1` 中的 PAUSE 位 (位 23) 以启用 PAUSE 拦截
2. **PAUSE_FILTER_COUNT** — 在生成 #VMEXIT 前，允许 PAUSE 指令通过的次数
3. **PAUSE_FILTER_THRESHOLD** — PAUSE 循环期间的最小周期数阈值（需要 PauseFilterThreshold 支持）
4. 当客户执行 PAUSE 循环时，硬件递增内部计数器
5. 计数器超过 PAUSE_FILTER_COUNT 时触发 #VMEXIT

```c
// 配置 PAUSE 过滤:
vmcb->control.pause_filter_count = 4096;      // 最多允许 4096 个 PAUSE
vmcb->control.pause_filter_thresh = 0x0FFF;   // 周期阈值
```

## 24. SPEC_CTRL 虚拟化

### 24.1 概述

SPEC_CTRL 虚拟化允许客户机访问 `MSR_IA32_SPEC_CTRL` (0x48)，用于控制推测执行缓解措施。

- **CPUID:** Fn8000_000A_EDX[SPEC_CTRL] (位 22) = 1
- **引入:** Zen 3

### 24.2 VMCB 保存区域中的 SPEC_CTRL

客户 SPEC_CTRL 值保存在 VMCB 保存区域:

| 相对偏移 | 大小 | 字段 | 类型 |
|:--------:|:----:|:----:|:----:|
| **0x2E0** | 8B | SPEC_CTRL | 客户 SPEC_CTRL MSR 值 |

### 24.3 处理

- 客户对 `MSR_IA32_SPEC_CTRL` (0x48) 的读写由 MSRPM 控制
- 如果允许直通，客户可以直接操作 SPEC_CTRL
- 如果拦截，Hypervisor 模拟访问并更新保存区域

## 25. Nested Page Table (NPT) Walk 详细代码示例

### 25.1 手动 NPT 地址翻译

以下代码演示了如何在 Hypervisor 中手动执行 NPT 地址翻译 (GPA → HPA):

```c
// NPT 表项类型
typedef union {
    uint64_t raw;
    struct {
        uint64_t present    : 1;   // 位 0
        uint64_t rw         : 1;   // 位 1
        uint64_t us         : 1;   // 位 2
        uint64_t pwt        : 1;   // 位 3
        uint64_t pcd        : 1;   // 位 4
        uint64_t accessed   : 1;   // 位 5
        uint64_t dirty      : 1;   // 位 6
        uint64_t ps         : 1;   // 位 7
        uint64_t global     : 1;   // 位 8
        uint64_t avail_low  : 3;   // 位 11:9
        uint64_t pfn        : 40;  // 位 51:12
        uint64_t avail_high : 7;   // 位 58:52
        uint64_t pkey       : 1;   // 位 59
        uint64_t nx         : 2;   // 位 63:60 (位 60=NX, 61=PK_WD)
    };
} npt_entry_t;

// 从 GPA 翻译到 HPA
uint64_t npt_translate(uint64_t ncr3_pa, uint64_t gpa)
{
    npt_entry_t *table;
    uint64_t    paddr;
    int         level;
    
    paddr = ncr3_pa;  // 从 nCR3 开始
    table = (npt_entry_t *)phys_to_virt(paddr);
    
    // 4 级页表遍历
    for (level = 4; level >= 1; level--) {
        int idx;
        
        // 提取当前级别的索引
        switch (level) {
            case 4: idx = (gpa >> 39) & 0x1FF; break;  // PML4
            case 3: idx = (gpa >> 30) & 0x1FF; break;  // PDP
            case 2: idx = (gpa >> 21) & 0x1FF; break;  // PD
            case 1: idx = (gpa >> 12) & 0x1FF; break;  // PT
        }
        
        if (!table[idx].present)
            return (uint64_t)-1;  // 页不存在 -> NPF
        
        // 检查大页
        if (table[idx].ps && (level == 2 || level == 3)) {
            // 大页处理
            uint64_t page_offset;
            uint64_t page_base;
            
            if (level == 2) {  // 2MB 页面
                page_base   = table[idx].pfn << 12;
                page_offset = gpa & 0x1FFFFF;
            } else {           // 1GB 页面
                page_base   = table[idx].pfn << 12;
                page_offset = gpa & 0x3FFFFFFF;
            }
            return page_base | page_offset;
        }
        
        if (level == 1) {
            // 4KB 叶页面
            uint64_t page_base = table[idx].pfn << 12;
            return page_base | (gpa & 0xFFF);
        }
        
        // 进入下一级
        paddr = table[idx].pfn << 12;
        table = (npt_entry_t *)phys_to_virt(paddr);
    }
    
    return (uint64_t)-1;  // 不应该到达这里
}

// 分配并设置 NPT 层级
uint64_t npt_create_mapping(uint64_t root_pa, uint64_t gpa, 
                             uint64_t hpa, uint64_t size, uint64_t flags)
{
    // 为简单起见，仅演示 4KB 或 2MB 映射
    // 实际实现需要更复杂的分页逻辑
    
    npt_entry_t *pml4, *pdpt, *pd, *pt;
    int pml4_idx = (gpa >> 39) & 0x1FF;
    int pdpt_idx = (gpa >> 30) & 0x1FF;
    int pd_idx   = (gpa >> 21) & 0x1FF;
    int pt_idx   = (gpa >> 12) & 0x1FF;
    
    pml4 = (npt_entry_t *)phys_to_virt(root_pa);
    
    // PML4 表项
    if (!pml4[pml4_idx].present) {
        uint64_t pdpt_pa = alloc_page();
        memset(phys_to_virt(pdpt_pa), 0, 0x1000);
        pml4[pml4_idx].raw = (pdpt_pa & 0xFFFFFF000) | NPT_PRESENT | NPT_RW | NPT_US;
    }
    
    pdpt = (npt_entry_t *)phys_to_virt(pml4[pml4_idx].pfn << 12);
    
    // 检查是否使用 1GB 页
    if ((size >= 0x40000000ULL) && (gpa & 0x3FFFFFFF) == 0 && (hpa & 0x3FFFFFFF) == 0) {
        pdpt[pdpt_idx].raw = (hpa & 0xFFFFFF000) | flags | NPT_PS;
        return root_pa;
    }
    
    // 否则继续 PDP → PD → PT
    if (!pdpt[pdpt_idx].present) {
        uint64_t pd_pa = alloc_page();
        memset(phys_to_virt(pd_pa), 0, 0x1000);
        pdpt[pdpt_idx].raw = (pd_pa & 0xFFFFFF000) | NPT_PRESENT | NPT_RW | NPT_US;
    }
    
    pd = (npt_entry_t *)phys_to_virt(pdpt[pdpt_idx].pfn << 12);
    
    // 检查是否使用 2MB 页
    if ((size >= 0x200000ULL) && (gpa & 0x1FFFFF) == 0 && (hpa & 0x1FFFFF) == 0) {
        pd[pd_idx].raw = (hpa & 0xFFFFFF000) | flags | NPT_PS;
        return root_pa;
    }
    
    // 4KB 映射
    if (!pd[pd_idx].present) {
        uint64_t pt_pa = alloc_page();
        memset(phys_to_virt(pt_pa), 0, 0x1000);
        pd[pd_idx].raw = (pt_pa & 0xFFFFFF000) | NPT_PRESENT | NPT_RW | NPT_US;
    }
    
    pt = (npt_entry_t *)phys_to_virt(pd[pd_idx].pfn << 12);
    pt[pt_idx].raw = (hpa & 0xFFFFFF000) | flags;
    
    return root_pa;
}
```

## 26. VMCB 初始化完整流程

### 26.1 VMCB 初始化清单

以下是创建和初始化 VMCB 以运行 64 位长模式客户机的完整清单:

```c
// 初始化客户 VMCB
void vmcb_init_guest(struct vmcb *vmcb, uint64_t entry_rip, 
                      uint64_t entry_rsp, uint64_t cr3)
{
    // 1. 清零整个 VMCB
    memset(vmcb, 0, sizeof(struct vmcb));
    
    // 2. 设置控制区域
    // 2a. 设置基本拦截 (拦截敏感指令)
    vmcb->control.intercept_cr_read  = 0;       // 不拦截 CR 读
    vmcb->control.intercept_cr_write = 
        (1 << 0) |  // CR0 写拦截 (CR0.PG, CR0.PE)
        (1 << 3) |  // CR3 写拦截
        (1 << 4);   // CR4 写拦截
    
    // 2b. 异常拦截
    vmcb->control.intercept_exceptions = 
        (1 << 6) |  // #UD — 非法操作码
        (1 << 13);  // #GP — 通用保护
    
    // 2c. 通用拦截
    vmcb->control.intercept_vectors_1 = 
        (1 << 0)  |  // INTR — 外部中断
        (1 << 1)  |  // NMI
        (1 << 18) |  // CPUID
        (1 << 24) |  // HLT
        (1 << 28);   // MSR_PROT
    
    vmcb->control.intercept_vectors_2 = 
        (1 << 1);    // VMMCALL
    
    // 3. 设置 NPT (如果启用)
    vmcb->control.nested_cr3 = npt_root_pa;    // nCR3
    // 启用 NPT (在 nested_ctl 字段或通过其他机制)
    
    // 4. 设置 TLB 控制
    vmcb->control.tlb_control = TLB_CTRL_FLUSH_ALL;
    
    // 5. 清除 CLEAN_BITS (首次运行需要全部加载)
    vmcb->control.clean_bits = 0;
    
    // 6. 设置保存区域 — 段寄存器
    // CS: 64 位代码段
    vmcb->save.cs.sel     = 0x10 | 0;  // Ring 0
    vmcb->save.cs.attrib  = 0xAB9;       // Present, Code, L=1, D=0, G=1
    vmcb->save.cs.limit   = 0xFFFFFFFF;
    vmcb->save.cs.base    = 0;
    
    // SS, DS, ES: 64 位数据段
    uint16_t data_attrib = 0xA93;  // Present, Data, W=1, G=1
    vmcb->save.ss.sel    = 0x18 | 0;
    vmcb->save.ss.attrib = data_attrib;
    vmcb->save.ss.limit  = 0xFFFFFFFF;
    vmcb->save.ss.base   = 0;
    
    vmcb->save.ds.sel    = 0x20 | 0;
    vmcb->save.ds.attrib = data_attrib;
    vmcb->save.ds.limit  = 0xFFFFFFFF;
    vmcb->save.ds.base   = 0;
    
    vmcb->save.es.sel    = 0x28 | 0;
    vmcb->save.es.attrib = data_attrib;
    vmcb->save.es.limit  = 0xFFFFFFFF;
    vmcb->save.es.base   = 0;
    
    vmcb->save.fs.sel    = 0;
    vmcb->save.fs.attrib = 0x88;    // Present
    vmcb->save.fs.limit  = 0;
    vmcb->save.fs.base   = 0;
    
    vmcb->save.gs.sel    = 0;
    vmcb->save.gs.attrib = 0x88;
    vmcb->save.gs.limit  = 0;
    vmcb->save.gs.base   = 0;
    
    // 7. GDTR 和 IDTR
    vmcb->save.gdtr.limit = 0xFF;
    vmcb->save.gdtr.base  = 0;
    vmcb->save.idtr.limit = 0xFFFF;
    vmcb->save.idtr.base  = 0;
    
    // 8. 控制寄存器
    vmcb->save.cr0 = 0x80000001;     // PG=1, PE=1
    vmcb->save.cr3 = cr3;            // 客户 CR3
    vmcb->save.cr4 = 0x00000660;     // PAE=1, PSE=1, OSFXSR=1
    vmcb->save.cr2 = 0;
    
    // 9. EFER
    vmcb->save.efer = 0x1001;        // SVME=1, LME=1 (长模式启用)
    
    // 10. 调试寄存器
    vmcb->save.dr6 = 0xFFFF0FF0;
    vmcb->save.dr7 = 0x00000400;
    
    // 11. 标志和指令指针
    vmcb->save.rflags = 0x00000002;  // 位 1 始终为 1
    vmcb->save.rip = entry_rip;
    vmcb->save.rsp = entry_rsp;
    vmcb->save.rax = 0;
    
    // 12. MSR 寄存器
    vmcb->save.star         = 0;
    vmcb->save.lstar        = 0;
    vmcb->save.cstar        = 0;
    vmcb->save.sfmask       = 0;
    vmcb->save.kernel_gs_base = 0;
    vmcb->save.sysenter_cs   = 0;
    vmcb->save.sysenter_esp  = 0;
    vmcb->save.sysenter_eip  = 0;
    
    // 13. CPL
    vmcb->save.cpl = 0;              // Ring 0
}
```

### 26.2 段属性 (Attribute) 编码

SVM VMCB 中的段属性字段使用与 Intel VMCS 不同的编码。16 位属性编码如下:

| 位 | 名称 | 描述 |
|:-:|:----:|------|
| 3:0 | Type | 段类型 |
| 4 | S | 描述符类型 (0=系统, 1=代码/数据) |
| 5 | DPL | 描述符权限级别 (2 位, 位 6:5) |
| 7:6 | P | 段存在 (位 7) |
| 11:8 | — | 保留 (可使用位 12 作为 AVL) |
| 12 | AVL | 软件可用 |
| 13 | L | 长模式 (仅 CS) |
| 14 | D/B | 默认操作数大小 (0=16位, 1=32位) |
| 15 | G | 粒度 (0=字节, 1=4KB) |

**常用段类型:**
- 数据段: Type=2 (读/写), S=1, DPL=0 → 属性 [15:8]=0xC0 | [7:0]=0x93 = 0xA93
- 64 位代码段: Type=0xB (执行/读/访问), S=1, DPL=0, L=1, D=0, G=1 → 0xAB9
- TSS: Type=0x9 (64 位 TSS 可用), S=0 → 0x89

## 27. #VMEXIT 处理框架

### 27.1 #VMEXIT 分发器

以下是一个完整的 #VMEXIT 分发器框架:

```c
// #VMEXIT 处理函数类型
typedef void (*exit_handler_t)(struct vmcb *vmcb);

// 退出码处理表
exit_handler_t svm_exit_handlers[256] = {
    [0x00 ... 0x0F] = handle_cr_read,    // CR0-CR15 读
    [0x10 ... 0x1F] = handle_cr_write,   // CR0-CR15 写
    [0x40] = handle_exception_de,
    [0x41] = handle_exception_db,
    [0x43] = handle_exception_bp,
    [0x46] = handle_exception_ud,
    [0x4D] = handle_exception_gp,
    [0x4E] = handle_exception_pf,
    [0x60] = handle_external_intr,
    [0x61] = handle_nmi,
    [0x64] = handle_vintr,
    [0x72] = handle_cpuid,
    [0x77] = handle_pause,
    [0x78] = handle_hlt,
    [0x7B] = handle_io,
    [0x7C] = handle_msr,
    [0x81] = handle_vmmcall,
};

// 主 #VMEXIT 分发器
void svm_vmexit_handler(struct vmcb *vmcb, struct percpu_data *cpu)
{
    uint64_t exit_code = vmcb->control.exit_code;
    
    // 读取退出码信息
    uint64_t exit_info_1 = vmcb->control.exit_info_1;
    uint64_t exit_info_2 = vmcb->control.exit_info_2;
    uint64_t exit_int_info = vmcb->control.exit_int_info;
    uint64_t nrip = vmcb->control.next_rip;
    
    // 检查 NPF (特殊处理)
    if (exit_code == 0x400) {
        handle_nested_page_fault(vmcb);
        goto resume;
    }
    
    // 检查 AVIC 退出
    if (exit_code >= 0x401 && exit_code <= 0x402) {
        handle_avic_exit(vmcb, exit_code, exit_info_1);
        goto resume;
    }
    
    // 检查 VMRUN 故障 (负数退出码)
    if ((int64_t)exit_code < 0) {
        handle_vmrun_failure(vmcb, exit_code);
        return; // 无法恢复
    }
    
    // 分发到处理函数 (低 8 位作为索引)
    if (exit_code <= 0xFF && svm_exit_handlers[exit_code]) {
        svm_exit_handlers[exit_code](vmcb);
    } else {
        handle_unhandled_exit(vmcb, exit_code);
    }
    
resume:
    // 清理 Clean Bits (如果需要修改 VMCB)
    // vmcb->control.clean_bits = 0;
    
    // 恢复客户运行
    VMRUN(vmcb);  // 执行 VMRUN 指令
}
```

### 27.2 CPUID 处理示例

```c
void handle_cpuid(struct vmcb *vmcb)
{
    uint32_t eax, ebx, ecx, edx;
    uint32_t input = (uint32_t)vmcb->save.rax;
    
    // 执行 CPUID
    __cpuid(&eax, &ebx, &ecx, &edx, input);
    
    // 可选: 篡改 CPUID 返回 (如隐藏 SVM 支持)
    if (input == 0x80000001) {
        ecx &= ~(1 << 2);  // 清除 SVM 位
    }
    
    // 如果要隐藏特性的数量
    if (input == 0x8000000A) {
        edx &= ~(1 << 0);  // 清除 NP 位 (隐藏 NPT 支持)
    }
    
    // 设置返回值
    vmcb->save.rax = eax;
    vmcb->save.rbx = ebx;
    vmcb->save.rcx = ecx;
    vmcb->save.rdx = edx;
    
    // 推进 RIP
    vmcb->save.rip = vmcb->control.next_rip;
}
```

### 27.3 MSR 读写处理示例

```c
void handle_msr(struct vmcb *vmcb)
{
    int is_write = (vmcb->control.exit_info_1 & 1);
    uint32_t msr = (uint32_t)vmcb->save.rcx;
    
    if (is_write) {
        // WRMSR
        uint64_t value = (vmcb->save.rdx << 32) | (vmcb->save.rax & 0xFFFFFFFF);
        
        switch (msr) {
        case MSR_IA32_STAR:
            vmcb->save.star = value;
            break;
        case MSR_IA32_LSTAR:
            vmcb->save.lstar = value;
            break;
        case MSR_IA32_SYSENTER_EIP:
            vmcb->save.sysenter_eip = value;
            break;
        default:
            // 模拟或忽略
            break;
        }
    } else {
        // RDMSR
        uint64_t value = 0;
        
        switch (msr) {
        case MSR_IA32_STAR:
            value = vmcb->save.star;
            break;
        case MSR_IA32_LSTAR:
            value = vmcb->save.lstar;
            break;
        case MSR_IA32_SYSENTER_EIP:
            value = vmcb->save.sysenter_eip;
            break;
        case MSR_IA32_TSC:
            value = rdtsc() + vmcb->control.tsc_offset;
            break;
        default:
            // 实际读取硬件 MSR
            value = rdmsr(msr);
            break;
        }
        
        vmcb->save.rax = value & 0xFFFFFFFF;
        vmcb->save.rdx = (value >> 32) & 0xFFFFFFFF;
    }
    
    // 推进 RIP
    vmcb->save.rip = vmcb->control.next_rip;
}
```

## 28. SEV/SEV-ES/SEV-SNP 概述

### 28.1 SEV (安全加密虚拟化)

- **CPUID:** Fn8000_000A_EDX[SEV] (位 30) = 1
- **引入:** Zen 1 (Family 17h)
- **功能:** 使用 AES 加密引擎透明加密客户机内存，Hypervisor 无法访问客户数据
- **密钥管理:** 每个客户机有不同的加密密钥，由 AMD 安全处理器 (SP) 管理
- **C 位:** 使用页表项的第 47 位 (C-bit) 标记加密页面

### 28.2 SEV-ES (加密状态)

- **CPUID:** Fn8000_000A_EDX[SEV_ES] (位 31) = 1
- **引入:** Zen 3
- **功能:** 扩展 SEV 以加密客户机寄存器状态。处理器自动加密 VMSA (VMCB 保存区域)
- **VMGEXIT:** 客户机执行 VMGEXIT 而不是 VMMCALL 与 Hypervisor 通信
- **自动退出 (AE):** 对于选定的安全退出原因，处理器可以直接处理而不暴露寄存器状态
- **VMSA:** 单独的加密 VMSA 页面取代标准的 VMCB 保存区域

### 28.3 SEV-SNP (安全嵌套分页)

- **CPUID:** Fn8000_000A_EDX[NestedVirt] (位 29) = 1
- **引入:** Zen 4
- **功能:** 添加嵌套分页的完整性保护，防止 Hypervisor 篡改客户机页表
- **RMP:** 反向映射表 (Reverse Map Table) — 保护物理页面所有权
- **RMPADJUST:** 调整 RMP 项的指令
- **PSP:** 平台安全处理器验证客户机启动度量

### 28.4 SEV VMSA 布局

在 SEV-ES 中，VMSA (VM Save Area) 是加密的 VMCB 保存区域，遵循第 5.3 节的布局，但有以下附加字段:

| 相对偏移 | 大小 | 字段 | 描述 |
|:--------:|:----:|:----:|------|
| 0x3B0 | 8B | SEV_FEATURES | SNPActive、vTOM、ReflectVC 等 |
| 0x3B8 | 8B | VINTR_CTRL | 客户控制的中断注入 |
| 0x3D0 | 8B | TLB_ID | TLB ID |
| 0x3D8 | 8B | PCPU_ID | 物理 CPU ID |

## 29. 高级 SVM 调试和故障排除

### 29.1 常见 VMRUN 故障原因

**VMEXIT_INVALID (-1) 的常见原因:**

| 症状 | 可能原因 | 解决方案 |
|:----|:---------|:---------|
| VMRUN 立即失败 | VMCB 未 4KB 对齐 | 确保 VMCB 物理地址低 12 位为零 |
| VMRUN 返回 -1 | CR0.PG=1 且 CR0.PE=0 | 保护模式要求 PE=1 |
| VMRUN 返回 -1 | EFER.LME=1 且 CR0.PG=0 | 长模式要求分页启用 |
| VMRUN 返回 -1 | 段属性无效 | 检查 CS 属性，确保 L=1 时 D=0 |
| VMRUN 返回 -1 | CR3 高 32 位非零 (PAE 模式) | 清除 CR3 高 32 位 |
| VMRUN 返回 -1 | MBZ 保留位被设置 | 清除 VMCB 中所有保留位 |
| VMRUN 返回 -2 | VMSA BUSY 位 | 等待或清除 VMSA BUSY |

**VMRUN 故障调试清单:**

```c
// VMRUN 之前的状态验证
int vmcb_validate(struct vmcb *vmcb)
{
    struct vmcb_save_area *s = &vmcb->save;
    
    // 1. CR0 和 CR0 保护位检查
    if (s->cr0 & (1 << 31)) {  // PG = 1
        if (!(s->cr0 & 1))     // PE 必须为 1
            return -1;
        if (s->cr3 & 0xFFF)    // CR3 低 12 位必须为零
            return -1;
    }
    
    // 2. EFER 检查
    if (s->efer & (1 << 8)) {  // LME = 1
        if (!(s->cr0 & (1 << 31)))  // PG 必须为 1
            return -1;
        if (!(s->cr4 & (1 << 5)))   // PAE 必须为 1
            return -1;
    }
    
    // 3. CS 段检查 (64 位模式)
    if ((s->cs.attrib & (1 << 13)) &&  // L = 1
        (s->cs.attrib & (1 << 14))) {  // D = 1 (非法组合)
        return -1;
    }
    
    // 4. VMCB 清除保证
    // 检查控制区域中的保留 (SBZ) 字段
    uint32_t *ctl = (uint32_t *)&vmcb->control;
    for (int i = 0; i < 0x100; i++) {
        if (is_reserved_vmcb_field(i) && ctl[i] != 0)
            return -1;
    }
    
    return 0;
}
```

### 29.2 #VMEXIT 处理期间常见陷阱

**陷阱 1: 忘记推进 RIP**
- 大多数退出处理 (如 CPUID、MSR、RDTSC) 后，必须将 RIP 推进到 NRIP
- 否则客户机会重新执行相同的指令，导致无限循环

**陷阱 2: #VMEXIT 期间访问 VMCB 的缓存一致性**
- VMCB 必须是 WB (Writeback) 内存类型
- 在多核系统中，确保 VMCB 修改后发出适当的缓存刷写指令

**陷阱 3: 嵌套中断处理**
- 在 STGI 之前（GIF=0），中断被阻塞
- 处理 #VMEXIT 后要尽快调用 STGI 以启用中断

**陷阱 4: Clean Bits 优化**
- 修改 VMCB 后必须清除相应的 Clean 位
- 否则硬件可能使用过时的状态

### 29.3 调试工具和方法

```c
// 打印 VMCB 转储
void vmcb_dump(struct vmcb *vmcb)
{
    LogInfo("=== VMCB Dump ===\n");
    LogInfo("Exit Code:     0x%llx\n", vmcb->control.exit_code);
    LogInfo("Exit Info 1:   0x%llx\n", vmcb->control.exit_info_1);
    LogInfo("Exit Info 2:   0x%llx\n", vmcb->control.exit_info_2);
    LogInfo("Exit Int Info: 0x%llx\n", vmcb->control.exit_int_info);
    LogInfo("Next RIP:      0x%llx\n", vmcb->control.next_rip);
    
    LogInfo("--- Guest State ---\n");
    LogInfo("CR0: 0x%llx  CR2: 0x%llx  CR3: 0x%llx  CR4: 0x%llx\n",
            vmcb->save.cr0, vmcb->save.cr2, vmcb->save.cr3, vmcb->save.cr4);
    LogInfo("RIP: 0x%llx  RSP: 0x%llx  RFLAGS: 0x%llx\n",
            vmcb->save.rip, vmcb->save.rsp, vmcb->save.rflags);
    LogInfo("RAX: 0x%llx  RCX: 0x%llx  RDX: 0x%llx  RBX: 0x%llx\n",
            vmcb->save.rax, vmcb->save.rcx, vmcb->save.rdx, vmcb->save.rbx);
    LogInfo("CS: sel=0x%x  base=0x%llx  limit=0x%x  attrib=0x%x\n",
            vmcb->save.cs.sel, vmcb->save.cs.base,
            vmcb->save.cs.limit, vmcb->save.cs.attrib);
    LogInfo("EFER: 0x%llx\n", vmcb->save.efer);
    
    // NPT 调试
    LogInfo("nCR3: 0x%llx\n", vmcb->control.nested_cr3);
    LogInfo("Clean Bits: 0x%x\n", vmcb->control.clean_bits);
}
```

## 30. 特定退出码详细处理指南

### 30.1 CR0 写拦截处理

当客户机尝试修改 CR0 时 (如修改 PG/PE 位、启用/禁用分页):

```c
void handle_cr0_write(struct vmcb *vmcb)
{
    uint64_t old_cr0 = vmcb->save.cr0;
    uint64_t new_cr0 = vmcb->control.exit_info_1;  // 写入的值
    
    // CR0 中可修改的位:
    // 位 0 (PE): 保护模式启用
    // 位 16 (WP): 只读页写保护
    // 位 29 (NW): 非写直达
    // 位 30 (CD): 缓存禁用
    // 位 31 (PG): 分页启用
    
    // 某些位不应随意更改:
    new_cr0 = (old_cr0 & ~0xE0000001) | (new_cr0 & 0xE0000001);
    
    // 处理 PG/PE 转换
    uint64_t old_pg_pe = old_cr0 & ((1 << 31) | 1);
    uint64_t new_pg_pe = new_cr0 & ((1 << 31) | 1);
    
    if (old_pg_pe != new_pg_pe) {
        // 模式转换 (实模式 ↔ 保护模式 ↔ 分页模式)
        if ((new_cr0 >> 31) & 1) {  // PG 从 0→1
            // 需要更新 EFER.LME 和 CR4.PAE
        }
    }
    
    vmcb->save.cr0 = new_cr0;
    vmcb->save.rip = vmcb->control.next_rip;
}
```

### 30.2 CR3 写拦截处理

当客户机修改 CR3 (页表基址) 时:

```c
void handle_cr3_write(struct vmcb *vmcb)
{
    uint64_t new_cr3 = vmcb->control.exit_info_1;
    
    // 验证 CR3 值
    if (new_cr3 & 0xFFF) {
        // CR3 必须页对齐
        // 某些 Hypervisor 允许位 3:0 用于 PCID
        new_cr3 &= ~0xFFFULL;
    }
    
    vmcb->save.cr3 = new_cr3;
    
    // 如果使用 NPT:
    // - 客户 CR3 是 GPA，由 NPT 翻译
    // - 不需要额外的 TLB 刷新 (NPT 自动处理)
    
    // 如果使用影子页表:
    // - 需要更新影子页表
    // - 需要刷新 TLB
    
    vmcb->save.rip = vmcb->control.next_rip;
}
```

### 30.3 嵌套页故障 (NPF) 处理

```c
void handle_nested_page_fault(struct vmcb *vmcb)
{
    uint64_t error_code = vmcb->control.exit_info_1;
    uint64_t fault_gpa  = vmcb->control.exit_info_2;
    
    int present   = (error_code >> 0) & 1;
    int is_write  = (error_code >> 1) & 1;
    int is_user   = (error_code >> 2) & 1;
    int is_rsvd   = (error_code >> 3) & 1;
    int is_fetch  = (error_code >> 4) & 1;
    int is_gpa    = (error_code >> 32) & 1;
    int is_ptwalk = (error_code >> 33) & 1;
    
    if (is_rsvd) {
        // NPT 表项中存在保留位违规
        LogError("NPF: Reserved bit violation at GPA 0x%llx\n", fault_gpa);
        // 通常应引起客户 #GP
        inject_exception(vmcb, 13, 0, 0);  // #GP(0)
        return;
    }
    
    if (!present) {
        // NPT 页面不存在 — 需要添加映射
        uint64_t hpa = allocate_physical_page();
        uint64_t flags = NPT_PRESENT | NPT_RW | NPT_US;
        
        npt_add_mapping(vmcb->control.nested_cr3, 
                        fault_gpa & ~0xFFFULL,  // 页基址
                        hpa,                     // 主机物理地址
                        0x1000,                  // 4KB 页
                        flags);
        
        // 页面已映射，客户机将重新执行
    } else {
        // 权限违规
        LogError("NPF: Permission violation at GPA 0x%llx\n", fault_gpa);
        // 如果来自客户页表遍历 (is_ptwalk=1)，应引起客户 #PF
        if (is_ptwalk) {
            inject_exception(vmcb, 14, error_code & 0xFFFF, 
                           vmcb->save.cr2);  // #PF(error_code, CR2)
        }
    }
    
    // 对于 NPF，不推进 RIP — 重新执行故障指令
}
```

## 31. SVM 实现性能优化指南

### 31.1 VMCB Clean Bits 优化

最大限度地减少 VMRUN 时的 VMCB 重新加载:

```c
// 首次运行时，清除所有 Clean Bits
vmcb->control.clean_bits = 0;

// 后续运行，如果只修改了特定字段:
vmcb->control.event_inj = new_event;
vmcb->control.clean_bits = VMCB_CLEAN_INTERCEPTS | VMCB_CLEAN_PERM_MAP 
                          | VMCB_CLEAN_ASID | VMCB_CLEAN_CR 
                          | VMCB_CLEAN_DR | VMCB_CLEAN_DT 
                          | VMCB_CLEAN_SEG;

// 如果修改了中断相关状态:
// 清除 VMCB_CLEAN_INTR 位
vmcb->control.clean_bits &= ~VMCB_CLEAN_INTR;

// 如果修改了 NPT 相关状态:
vmcb->control.clean_bits &= ~VMCB_CLEAN_NPT;
```

### 31.2 ASID 优化

```c
// ASID 分配策略
struct asid_manager {
    uint64_t generation;     // 当前 ASID 世代
    uint32_t next_asid;      // 下一个可用 ASID
    uint32_t max_asid;       // 最大 ASID (来自 CPUID)
};

uint32_t asid_allocate(struct asid_manager *mgr, uint32_t cpu_id)
{
    if (mgr->next_asid > mgr->max_asid) {
        // ASID 耗尽 — 刷新所有 ASID
        // 在所有 CPU 上递增世代计数器
        atomic_increment(&mgr->generation);
        mgr->next_asid = 1;  // 跳过 ASID 0 (主机)
    }
    
    return mgr->next_asid++;
}

// 在 VMRUN 之前
void asid_apply(struct vmcb *vmcb, uint32_t asid, uint64_t generation)
{
    // ASID 存储在 VMCB 字段 (偏移 0x068)
    // (在较早的 VMCB 布局中，ASID 是一个单独的字段)
    // 注意: 当 FlushByAsid 可用时使用 ASID 特定的 TLB 控制
    
    vmcb->control.asid = asid;
    
    if (asid_generation_changed)
        vmcb->control.tlb_control = TLB_CTRL_FLUSH_ASID;
    else
        vmcb->control.tlb_control = TLB_CTRL_DO_NOTHING;
}
```

### 31.3 MSRPM 预计算优化

```c
// MSR 权限访问的优化查找
#define MSRPM_PAGES 2

// 预计算 MSRPM 偏移
int msrpm_get_offset(uint32_t msr, int is_write)
{
    int range, offset;
    
    // 确定范围
    if (msr < 0x2000)
        range = 0;
    else if (msr >= 0xC0000000 && msr < 0xC0002000)
        range = 1;
    else if (msr >= 0xC0010000 && msr < 0xC0012000)
        range = 2;
    else
        return -1;  // 不支持
    
    // 计算位偏移
    switch (range) {
    case 0:
        offset = msr * 2;
        break;
    case 1:
        offset = (msr - 0xC0000000 + 0x2000) * 2;
        break;
    case 2:
        offset = (msr - 0xC0010000 + 0x4000) * 2;
        break;
    }
    
    int byte_offset = offset / 8;
    int bit_offset  = offset % 8;
    
    // 翻转位 (0 = 允许, 1 = 拦截)
    return byte_offset + (is_write ? 1 : 0);
}

// 设置 MSR 允许
void msrpm_set_pass(uint8_t *msrpm, uint32_t msr)
{
    int off_r = msrpm_get_offset(msr, 0);  // 读位
    int off_w = msrpm_get_offset(msr, 1);  // 写位
    
    if (off_r >= 0) msrpm[off_r] &= ~(1 << ((off_r * 8) & 7));
    if (off_w >= 0) msrpm[off_w] &= ~(1 << ((off_w * 8) & 7));
}
```

### 31.4 快速路径优化

对于高频 #VMEXIT 类型，可以内联处理减少开销:

```c
// #VMEXIT 快速路径 — 在汇编中直接处理
// 减少不必要的 C 函数调用

__attribute__((naked))
void svm_vmexit_fastpath(void)
{
    __asm volatile (
        "pushfq\n"
        "push %%rbp\n"
        "movq %%rsp, %%rbp\n"
        // ... 保存更多寄存器
        
        // 读取 VMCB 退出码
        // 如果是 CPUID/HLT/MSR 等简单退出，直接处理
        // 避免完整的 C 函数调用开销
        
        // 如果是复杂退出，跳转到完整处理程序
        "call svm_vmexit_handler\n"
        
        // 恢复寄存器
        "pop %%rbp\n"
        "popfq\n"
        "ret\n"
    );
}
```

---

## 32. 附录

### 20.1 错误码参考: VMRUN 失败条件

| 故障类型 | 错误码 | 可能原因 |
|:--------:|:-----:|:---------|
| VMEXIT_INVALID | -1 (0xFF...FF) | VMCB 中保留位非零、无效 CR3、无效客户状态、SEV-SNP 页面检查失败 |
| VMEXIT_BUSY | -2 (0xFF...FE) | VMSA 的 BUSY 位已设置 (可选注入) |
| VMEXIT_IDLE_REQUIRED | -3 (0xFF...FD) | SMT 兄弟线程未处于空闲状态 |
| VMEXIT_INVALID_PMC | -4 (0xFF...FC) | 无效的 PMC (性能计数器) 状态 |

### 20.2 SVM 检测快速参考

```c
// 检测 SVM 可用性
int svm_detect(void)
{
    int eax, ebx, ecx, edx;

    // 1. CPUID Fn8000_0001: 检查 SVM 支持
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 2)))          // ECX[2] = SVM
        return SVM_NOT_SUPPORTED;

    // 2. CPUID Fn8000_000A: 获取 SVM 特性
    cpuid(0x8000000A, &eax, &ebx, &ecx, &edx);
    // EAX = SVM 修订号
    // EBX = 最大 ASID 数
    // EDX = 特性标志

    // 3. 检查 VM_CR.SVMDIS
    uint64_t vm_cr = rdmsr(MSR_VM_CR);    // 0xC0010114
    if (vm_cr & (1 << 4))                  // SVMDIS = 位 4
        return SVM_DISABLED_BY_BIOS;

    // 4. 检查 VM_CR.LOCK
    if (vm_cr & (1 << 3))                  // LOCK = 位 3
        return SVM_LOCKED;

    // 5. 启用 EFER.SVME
    uint64_t efer = rdmsr(MSR_EFER);       // 0xC0000080
    wrmsr(MSR_EFER, efer | (1 << 12));     // SVME = 位 12

    // 6. 设置 VM_HSAVE_PA (每个逻辑核心)
    wrmsr(MSR_VM_HSAVE_PA, host_save_pa); // 0xC0010117

    return SVM_AVAILABLE;
}
```

### 20.3 常用常量定义 (C 语言)

```c
/* MSR 地址 */
#define MSR_EFER          0xC0000080
#define MSR_VM_CR         0xC0010114
#define MSR_VM_IGNNE      0xC0010115
#define MSR_VM_HSAVE_PA   0xC0010117
#define MSR_SVM_KEY       0xC0010118
#define MSR_AVIC_DOORBELL 0xC001011B
#define MSR_SYSCFG        0xC0010010

/* VMCB 控制区域字段偏移 */
#define VMCB_CR_READ       0x000
#define VMCB_CR_WRITE      0x002
#define VMCB_DR_INTERCEPT  0x004
#define VMCB_EXC_INTERCEPT 0x008
#define VMCB_INT_VECTOR_1  0x00C
#define VMCB_INT_VECTOR_2  0x010
#define VMCB_IOPM_PA       0x030
#define VMCB_MSRPM_PA      0x038
#define VMCB_TSC_OFFSET    0x040
#define VMCB_TLB_CTRL      0x05C
#define VMCB_V_INTR        0x060
#define VMCB_EXITCODE      0x070
#define VMCB_EXITINFO1     0x078
#define VMCB_EXITINFO2     0x080
#define VMCB_EXITINTINFO   0x088
#define VMCB_N_CR3         0x090
#define VMCB_EVENTINJ      0x0A8
#define VMCB_CLEAN_BITS    0x0C0
#define VMCB_NRIP          0x0C8

/* VMCB Clean Bits */
#define VMCB_CLEAN_INTERCEPTS   (1 << 0)
#define VMCB_CLEAN_PERM_MAP     (1 << 1)
#define VMCB_CLEAN_ASID         (1 << 2)
#define VMCB_CLEAN_INTR         (1 << 3)
#define VMCB_CLEAN_NPT          (1 << 4)
#define VMCB_CLEAN_CR           (1 << 5)
#define VMCB_CLEAN_DR           (1 << 6)
#define VMCB_CLEAN_DT           (1 << 7)
#define VMCB_CLEAN_SEG          (1 << 8)
#define VMCB_CLEAN_CR2          (1 << 9)
#define VMCB_CLEAN_LBR          (1 << 10)
#define VMCB_CLEAN_AVIC         (1 << 11)

/* TLB Control */
#define TLB_CTRL_DO_NOTHING     0
#define TLB_CTRL_FLUSH_ALL      1
#define TLB_CTRL_FLUSH_ASID     3
#define TLB_CTRL_FLUSH_ASID_LOCAL 7

/* EVENTINJ 位 */
#define EVENTINJ_VALID      (1ULL << 40)
#define EVENTINJ_EV         (1ULL << 11)
#define EVENTINJ_TYPE_SHIFT 8
#define EVENTINJ_TYPE_EXTERNAL 0
#define EVENTINJ_TYPE_NMI      2
#define EVENTINJ_TYPE_EXCEPTION 3
#define EVENTINJ_TYPE_SWINT    4

/* CPUID 8000_0001 ECX SVM 位 */
#define CPUID_SVM_BIT      2

/* CPUID 8000_000A EDX SVM 特性位 */
#define SVM_FEAT_NP            (1 << 0)
#define SVM_FEAT_LBRV          (1 << 1)
#define SVM_FEAT_SVMLOCK       (1 << 2)
#define SVM_FEAT_NRIPS         (1 << 3)
#define SVM_FEAT_TSCRATEMSR    (1 << 4)
#define SVM_FEAT_VMCBCLEAN     (1 << 5)
#define SVM_FEAT_FLUSHBYASID   (1 << 6)
#define SVM_FEAT_DECODEASSIST  (1 << 7)
#define SVM_FEAT_PAUSEFILTER   (1 << 10)
#define SVM_FEAT_PAUSETHRESH   (1 << 12)
#define SVM_FEAT_AVIC          (1 << 13)
#define SVM_FEAT_V_VMSAVE_VMLOAD (1 << 15)
#define SVM_FEAT_VGIF          (1 << 16)
#define SVM_FEAT_GMET          (1 << 17)
#define SVM_FEAT_X2AVIC        (1 << 18)
#define SVM_FEAT_SSSCHK        (1 << 21)
#define SVM_FEAT_SPEC_CTRL     (1 << 22)
#define SVM_FEAT_ROGPT         (1 << 23)
#define SVM_FEAT_VNMI          (1 << 24)
#define SVM_FEAT_IBSVIRT       (1 << 26)
#define SVM_FEAT_VMCBPERMISSIVE (1 << 27)
#define SVM_FEAT_SVME_ADDR_CHK (1 << 28)
#define SVM_FEAT_NESTEDVIRT    (1 << 29)
#define SVM_FEAT_SEV           (1 << 30)
#define SVM_FEAT_SEV_ES        (1 << 31)
```

### 20.4 参考资料来源

| 来源 | 描述 |
|:----|:----|
| **AMD APM Vol. 2** (#24593) | AMD64 Architecture Programmer's Manual Volume 2: System Programming — 主要参考 |
| **AMD APM Vol. 3** (#24594) | AMD64 Architecture Programmer's Manual Volume 3: General-Purpose and System Instructions |
| **Linux KVM** (`arch/x86/kvm/svm/`) | KVM AMD SVM 后端 — 规范参考实现 |
| **Linux 内核** (`arch/x86/include/asm/svm.h`) | SVM 常量和 VMCB 结构定义 |
| **Xen Hypervisor** | Xen AMD SVM 后端 — 替代实现参考 |
| **FreeBSD bhyve** | FreeBSD AMD SVM 后端 — 附加参考 |
| **VirtualBox** (`VBox/hm_svm.h`) | VirtualBox SVM 硬件抽象层 — 已验证的偏移量 |

### 20.5 文件修订历史

| 版本 | 日期 | 变更 |
|:----:|:----:|:-----|
| 1.0 | 2026-06-28 | 初始版本 — 完整 SVM 参考手册 |

---

> **本文档是为 VMXHypervisorToolbox 项目的 AMD SVM 后端开发编制的完整参考手册。**  
> 如有疑问或不一致，请以 AMD 官方文档 (#24593) 为准。
