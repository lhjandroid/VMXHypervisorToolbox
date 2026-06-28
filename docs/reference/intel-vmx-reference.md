# Intel VT-x (VMX) 完整参考手册

> **版本**: 基于 Intel SDM Volume 3C (325384) 与 Volume 4 (335592)  
> **适用范围**: Windows x64 Type-2 Hypervisor 开发  
> **语言**: 中文  
> **更新日期**: 2026-06-28

---

## 目录

1. [VMX 架构概述](#1-vmx-架构概述)
2. [VMCS 数据结构](#2-vmcs-数据结构)
3. [VMX 基本控制 MSR](#3-vmx-基本控制-msr)
4. [VM-Execution 控制字段](#4-vm-execution-控制字段)
5. [VM-Exit 控制字段](#5-vm-exit-控制字段)
6. [VM-Entry 控制字段](#6-vm-entry-控制字段)
7. [VMCS Guest-State 区域](#7-vmcs-guest-state-区域)
8. [VMCS Host-State 区域](#8-vmcs-host-state-区域)
9. [VM-Exit 信息字段](#9-vm-exit-信息字段)
10. [VM-Exit 原因码完整表](#10-vm-exit-原因码完整表)
11. [EPT 与 VPID](#11-ept-与-vpid)
12. [VMX 指令参考](#12-vmx-指令参考)
13. [VM-Entry 校验](#13-vm-entry-校验)
14. [事件注入](#14-事件注入)
15. [VMCS 编码速查表](#15-vmcs-编码速查表)
16. [CR0/CR4 固定位算法](#16-cr0cr4-固定位算法)
17. [VMX-Preemption Timer](#17-vmx-preemption-timer)
18. [Posted-Interrupt 处理](#18-posted-interrupt-处理)

---

## 1. VMX 架构概述

### 1.1 VMX 根操作与 VMX 非根操作

Intel VT-x 引入两种操作模式：

| 模式 | 描述 | 特权级 |
|------|------|--------|
| **VMX Root Operation** | VMM 运行模式，完全特权级 | Ring 0 - Ring -1 |
| **VMX Non-Root Operation** | Guest 运行模式，部分敏感指令透明拦截 | Ring 0 - Ring 3 |

在 VMX 非根操作中，Guest 执行的**敏感指令**（如 CPUID、RDMSR、CR 访问等）会触发 **VM-Exit**，将控制权交还给根操作中的 VMM。

### 1.2 VMX 生命周期

```
VMCLEAR → VMPTRLD → VMCS 配置 → VMLAUNCH → (VM-Exit) → 处理 → VMRESUME → ...
                                     ↑                                |
                                     └────── VM-Exit 循环 ────────────┘
```

| 阶段 | 描述 | SDM 章节 |
|------|------|----------|
| **VMXON** | 分配 VMXON 区域，写入 Revision ID，执行 VMXON 指令进入 VMX 根操作 | §23.6 |
| **VMCLEAR** | 清除指定 VMCS 的活跃状态，将 VMCS 数据写回内存 | §30.2 |
| **VMPTRLD** | 将指定 VMCS 加载为当前 VMCS | §30.5 |
| **VMCS 配置** | 通过 VMWRITE 设置 Guest/Host 状态、控制字段等 | §24 |
| **VMLAUNCH** | 首次启动 VMCS 对应的 VM（VM-Entry） | §30.3 |
| **VMRESUME** | 在 VM-Exit 后恢复 VM 执行 | §30.6 |
| **VM-Exit** | 异常/中断/指令触发 VM-Exit，保存 Guest 状态，加载 Host 状态 | §27 |
| **VMXOFF** | 退出 VMX 根操作 | §30.8 |

### 1.3 VMCS 概念

**VMCS (Virtual Machine Control Structure)** 是每虚拟 CPU 的数据结构，包含：

- Guest 状态（寄存器、段、MSR 等）
- Host 状态（寄存器、段、基址等）
- VM-Execution 控制（决定哪些操作触发 VM-Exit）
- VM-Exit 控制（Exit 时的行为）
- VM-Entry 控制（Entry 时的行为）
- VM-Exit 信息（Exit 原因、详细信息）

> **注意**: VMCS 的内容**不允许**软件直接读写其内存区域，必须通过 VMREAD/VMWRITE 指令访问。

### 1.4 VMXON 区域与 VMCS 区域

| 属性 | VMXON 区域 | VMCS 区域 |
|------|-----------|-----------|
| **大小** | IA32_VMX_BASIC[44:32] 字节（最大 4096） | 同上 |
| **对齐** | 4KB 边界 | 4KB 边界 |
| **数量** | 每逻辑 CPU 一个 | 每 vCPU 一个 |
| **初始值** | 写入 Revision ID（IA32_VMX_BASIC[30:0]），bit 31=0 | 同左 |

**内存区域前 4 字节布局**:

| 偏移 | 大小 | 字段 | 描述 |
|------|------|------|------|
| 0 | 4 字节 | Revision Identifier | IA32_VMX_BASIC[30:0]；bit 31=0（VMXON）或 Shadow-VMCS 指示（VMCS） |
| 4 | 4 字节 | VMX-Abort Indicator | VMX Abort 时由处理器写入 |

---

## 2. VMCS 数据结构

### 2.1 VMCS 字段编码规则

每个 VMCS 字段由一个 32 位编码标识，编码格式如下：

| 位范围 | 字段 | 描述 |
|--------|------|------|
| 31:15 | Reserved | 保留，必须为 0 |
| 14:13 | **Width** | 字段宽度 |
| 12 | Reserved | 保留 |
| 11:10 | **Type** | 字段类型 |
| 9:1 | **Index** | 字段索引 |
| 0 | **Access Type** | 0=Full, 1=High（仅 64 位字段的高 32 位） |

**宽度编码 (Bits 14:13)**:

| 值 | 宽度 | 说明 |
|----|------|------|
| 00 | 16-bit | 16 位字段 |
| 01 | 64-bit | 64 位字段（需两个编码：Full 和 High） |
| 10 | 32-bit | 32 位字段 |
| 11 | Natural-width | 32 位（IA-32）或 64 位（x86-64） |

**类型编码 (Bits 11:10)**:

| 值 | 类型 | 说明 |
|----|------|------|
| 00 | Control | 控制字段 |
| 01 | Read-only Data | 只读数据（VM-Exit 时写入） |
| 10 | Guest-State | Guest 状态字段 |
| 11 | Host-State | Host 状态字段 |

### 2.2 VMCS 字段分类表

#### 16-Bit 字段

**控制字段 (Type=00)**:

| 编码 | 字段名 | 描述 |
|------|--------|------|
| `0000H` | VPID | 虚拟处理器 ID |
| `0002H` | Posted-interrupt notification vector | Posted-interrupt 通知向量 |
| `0004H` | EPTP index | EPTP 列表索引 |

**Guest-State 字段 (Type=10)**:

| 编码 | 字段名 |
|------|--------|
| `0800H` | Guest ES selector |
| `0802H` | Guest CS selector |
| `0804H` | Guest SS selector |
| `0806H` | Guest DS selector |
| `0808H` | Guest FS selector |
| `080AH` | Guest GS selector |
| `080CH` | Guest LDTR selector |
| `080EH` | Guest TR selector |
| `0810H` | Guest interrupt status |

**Host-State 字段 (Type=11)**:

| 编码 | 字段名 |
|------|--------|
| `0C00H` | Host ES selector |
| `0C02H` | Host CS selector |
| `0C04H` | Host SS selector |
| `0C06H` | Host DS selector |
| `0C08H` | Host FS selector |
| `0C0AH` | Host GS selector |
| `0C0CH` | Host TR selector |

---

#### 64-Bit 字段

**控制字段 (Type=00)** — 每个字段有两个编码（Full/High）：

| Full 编码 | High 编码 | 字段名 | 条件 |
|-----------|-----------|--------|------|
| `2000H` | `2001H` | Address of I/O bitmap A | |
| `2002H` | `2003H` | Address of I/O bitmap B | |
| `2004H` | `2005H` | Address of MSR bitmaps | 需启用 Use MSR bitmaps |
| `2006H` | `2007H` | VM-exit MSR-store address | |
| `2008H` | `2009H` | VM-exit MSR-load address | |
| `200AH` | `200BH` | VM-entry MSR-load address | |
| `200CH` | `200DH` | Executive-VMCS pointer | |
| `2010H` | `2011H` | TSC offset | |
| `2012H` | `2013H` | Virtual-APIC address | 需启用 Use TPR shadow |
| `2014H` | `2015H` | APIC-access address | 需启用 Virtualize APIC accesses |
| `2016H` | `2017H` | Posted-interrupt descriptor addr | 需启用 Process posted interrupts |
| `2018H` | `2019H` | VM-function controls | 需启用 Enable VM functions |
| `201AH` | `201BH` | EPT pointer (EPTP) | 需启用 Enable EPT |
| `201CH` | `201DH` | EOI-exit bitmap 0 | 需启用 Virtual-interrupt delivery |
| `201EH` | `201FH` | EOI-exit bitmap 1 | |
| `2020H` | `2021H` | EOI-exit bitmap 2 | |
| `2022H` | `2023H` | EOI-exit bitmap 3 | |
| `2024H` | `2025H` | EPTP-list address | 需启用 EPTP switching |
| `2026H` | `2027H` | VMREAD-bitmap address | 需启用 VMCS shadowing |
| `2028H` | `2029H` | VMWRITE-bitmap address | 需启用 VMCS shadowing |
| `202AH` | `202BH` | Virtualization-exception info addr | 需启用 EPT-violation #VE |
| `202CH` | `202DH` | XSS-exiting bitmap | 需启用 Enable XSAVES/XRSTORS |

**Read-Only Data (Type=01)**:

| Full | High | 字段名 |
|------|------|--------|
| `2400H` | `2401H` | Guest-physical address |

**Guest-State 字段 (Type=10)**:

| Full | High | 字段名 | 条件 |
|------|------|--------|------|
| `2800H` | `2801H` | VMCS link pointer | |
| `2802H` | `2803H` | Guest IA32_DEBUGCTL | |
| `2804H` | `2805H` | Guest IA32_PAT | 需启用 Load/Save IA32_PAT |
| `2806H` | `2807H` | Guest IA32_EFER | 需启用 Load/Save IA32_EFER |
| `2808H` | `2809H` | Guest IA32_PERF_GLOBAL_CTRL | 需启用 Load PERF_GLOBAL_CTRL |
| `280AH` | `280BH` | Guest PDPTE0 | 需禁用 EPT |
| `280CH` | `280DH` | Guest PDPTE1 | 同上 |
| `280EH` | `280FH` | Guest PDPTE2 | 同上 |
| `2810H` | `2811H` | Guest PDPTE3 | 同上 |

**Host-State 字段 (Type=11)**:

| Full | High | 字段名 | 条件 |
|------|------|--------|------|
| `2C00H` | `2C01H` | Host IA32_PAT | 需启用 Load IA32_PAT (VM-Exit) |
| `2C02H` | `2C03H` | Host IA32_EFER | 需启用 Load IA32_EFER (VM-Exit) |
| `2C04H` | `2C05H` | Host IA32_PERF_GLOBAL_CTRL | 需启用 Load PERF_GLOBAL_CTRL (VM-Exit) |

---

#### 32-Bit 字段

**控制字段 (Type=00)**:

| 编码 | 字段名 | 条件 |
|------|--------|------|
| `4000H` | Pin-based VM-execution controls | |
| `4002H` | Primary processor-based VM-execution controls | |
| `4004H` | Exception bitmap | |
| `4006H` | Page-fault error-code mask | |
| `4008H` | Page-fault error-code match | |
| `400AH` | CR3-target count | |
| `400CH` | VM-exit controls | |
| `400EH` | VM-exit MSR-store count | |
| `4010H` | VM-exit MSR-load count | |
| `4012H` | VM-entry controls | |
| `4014H` | VM-entry MSR-load count | |
| `4016H` | VM-entry interruption-information | |
| `4018H` | VM-entry exception error code | |
| `401AH` | VM-entry instruction length | |
| `401CH` | TPR threshold | 需启用 Use TPR shadow |
| `401EH` | Secondary processor-based VM-execution controls | 需启用 Activate secondary controls |
| `4020H` | PLE_Gap | 需启用 PAUSE-loop exiting |
| `4022H` | PLE_Window | 需启用 PAUSE-loop exiting |

**Read-Only Data (Type=01)**:

| 编码 | 字段名 |
|------|--------|
| `4400H` | VM-instruction error |
| `4402H` | Exit reason |
| `4404H` | VM-exit interruption information |
| `4406H` | VM-exit interruption error code |
| `4408H` | IDT-vectoring information |
| `440AH` | IDT-vectoring error code |
| `440CH` | VM-exit instruction length |
| `440EH` | VM-exit instruction information |

**Guest-State 字段 (Type=10)**:

| 编码 | 字段名 |
|------|--------|
| `4800H` | Guest ES limit |
| `4802H` | Guest CS limit |
| `4804H` | Guest SS limit |
| `4806H` | Guest DS limit |
| `4808H` | Guest FS limit |
| `480AH` | Guest GS limit |
| `480CH` | Guest LDTR limit |
| `480EH` | Guest TR limit |
| `4810H` | Guest GDTR limit |
| `4812H` | Guest IDTR limit |
| `4814H` | Guest ES access rights |
| `4816H` | Guest CS access rights |
| `4818H` | Guest SS access rights |
| `481AH` | Guest DS access rights |
| `481CH` | Guest FS access rights |
| `481EH` | Guest GS access rights |
| `4820H` | Guest LDTR access rights |
| `4822H` | Guest TR access rights |
| `4824H` | Guest interruptibility state |
| `4826H` | Guest activity state |
| `4828H` | Guest SMBASE |
| `482AH` | Guest SYSENTER_CS |
| `482EH` | VMX-preemption timer value |

**Host-State 字段 (Type=11)**:

| 编码 | 字段名 |
|------|--------|
| `4C00H` | Host IA32_SYSENTER_CS |

---

#### Natural-Width 字段

**控制字段 (Type=00)**:

| 编码 | 字段名 |
|------|--------|
| `6000H` | CR0 guest/host mask |
| `6002H` | CR4 guest/host mask |
| `6004H` | CR0 read shadow |
| `6006H` | CR4 read shadow |
| `6008H` | CR3-target value 0 |
| `600AH` | CR3-target value 1 |
| `600CH` | CR3-target value 2 |
| `600EH` | CR3-target value 3 |

**Read-Only Data (Type=01)**:

| 编码 | 字段名 |
|------|--------|
| `6400H` | Exit qualification |
| `6402H` | I/O RCX |
| `6404H` | I/O RSI |
| `6406H` | I/O RDI |
| `6408H` | I/O RIP |
| `640AH` | Guest-linear address |

**Guest-State 字段 (Type=10)**:

| 编码 | 字段名 |
|------|--------|
| `6800H` | Guest CR0 |
| `6802H` | Guest CR3 |
| `6804H` | Guest CR4 |
| `6806H` | Guest ES base |
| `6808H` | Guest CS base |
| `680AH` | Guest SS base |
| `680CH` | Guest DS base |
| `680EH` | Guest FS base |
| `6810H` | Guest GS base |
| `6812H` | Guest LDTR base |
| `6814H` | Guest TR base |
| `6816H` | Guest GDTR base |
| `6818H` | Guest IDTR base |
| `681AH` | Guest DR7 |
| `681CH` | Guest RSP |
| `681EH` | Guest RIP |
| `6820H` | Guest RFLAGS |
| `6822H` | Guest pending debug exceptions |
| `6824H` | Guest SYSENTER_ESP |
| `6826H` | Guest SYSENTER_EIP |

**Host-State 字段 (Type=11)**:

| 编码 | 字段名 |
|------|--------|
| `6C00H` | Host CR0 |
| `6C02H` | Host CR3 |
| `6C04H` | Host CR4 |
| `6C06H` | Host FS base |
| `6C08H` | Host GS base |
| `6C0AH` | Host TR base |
| `6C0CH` | Host GDTR base |
| `6C0EH` | Host IDTR base |
| `6C10H` | Host IA32_SYSENTER_ESP |
| `6C12H` | Host IA32_SYSENTER_EIP |
| `6C14H` | Host RSP |
| `6C16H` | Host RIP |

---

## 3. VMX 基本控制 MSR

### 3.1 MSR 地址总表

| MSR 名称 | 地址 | 描述 |
|-----------|------|------|
| `IA32_VMX_BASIC` | `0x480` | VMX 基本信息 |
| `IA32_VMX_PINBASED_CTLS` | `0x481` | Pin-Based VM-Execution 控制允许值 |
| `IA32_VMX_PROCBASED_CTLS` | `0x482` | Primary Processor-Based 控制允许值 |
| `IA32_VMX_EXIT_CTLS` | `0x483` | VM-Exit 控制允许值 |
| `IA32_VMX_ENTRY_CTLS` | `0x484` | VM-Entry 控制允许值 |
| `IA32_VMX_MISC` | `0x485` | VMX 杂项信息 |
| `IA32_VMX_CR0_FIXED0` | `0x486` | CR0 必须为 0 的位 |
| `IA32_VMX_CR0_FIXED1` | `0x487` | CR0 必须为 1 的位 |
| `IA32_VMX_CR4_FIXED0` | `0x488` | CR4 必须为 0 的位 |
| `IA32_VMX_CR4_FIXED1` | `0x489` | CR4 必须为 1 的位 |
| `IA32_VMX_VMCS_ENUM` | `0x48A` | VMCS 枚举（最高编码索引） |
| `IA32_VMX_PROCBASED_CTLS2` | `0x48B` | Secondary Processor-Based 控制允许值 |
| `IA32_VMX_EPT_VPID_CAP` | `0x48C` | EPT 和 VPID 能力 |
| `IA32_VMX_TRUE_PINBASED_CTLS` | `0x48D` | True Pin-Based 控制允许值 |
| `IA32_VMX_TRUE_PROCBASED_CTLS` | `0x48E` | True Primary 控制允许值 |
| `IA32_VMX_TRUE_EXIT_CTLS` | `0x48F` | True VM-Exit 控制允许值 |
| `IA32_VMX_TRUE_ENTRY_CTLS` | `0x490` | True VM-Entry 控制允许值 |
| `IA32_VMX_VMFUNC` | `0x491` | VM 函数控制 |
| `IA32_VMX_PROCBASED_CTLS3` | `0x492` | Tertiary 控制允许值 |
| `IA32_VMX_EXIT_CTLS2` | `0x493` | Secondary VM-Exit 控制允许值 |

### 3.2 IA32_VMX_BASIC (0x480) — 位布局

| 位 | 字段 | 描述 |
|----|------|------|
| 30:0 | **VMCS Revision ID** | VMCS 区域修订标识符 |
| 31 | 保留 | 必须为 0 |
| 44:32 | **VMCS Region Size** | VMXON/VMCS 区域大小（字节），最大 4096；bit 44=1 表示 4096 |
| 48 | **VMCS Address Width** | 0=物理地址宽度；1=32 位限制 |
| 49 | **Dual-Monitor Support** | 1=支持双显示器 SMM 处理 |
| 53:50 | **VMCS Memory Type** | VMCS 内存类型：6=Write-Back |
| 54 | **INS/OUTS Info** | 1=处理器报告 INS/OUTS 指令信息 |
| 55 | **TRUE Controls** | 1=支持 IA32_VMX_TRUE_*_CTLS MSR |
| 56 | **Any Err Code** | 1=任意异常可带或不带错误码 |

### 3.3 控制 MSR 的通用格式

所有 `IA32_VMX_*_CTLS` MSR 使用相同格式来报告允许的位设置：

| 位 | 字段 | 描述 |
|----|------|------|
| 31:0 | **Allowed 0-settings** | 某位=1 表示该位**必须为 1**（不能设为 0） |
| 63:32 | **Allowed 1-settings** | 某位=0 表示该位**必须为 0**（不能设为 1） |

**控制值计算算法**:

```c
// desired = 想要设置的位
// msr_val = RDMSR(IA32_VMX_*_CTLS)
uint32_t allowed_0 = (uint32_t)(msr_val);       // 必须为 1 的位
uint32_t allowed_1 = (uint32_t)(msr_val >> 32);  // 可以为 1 的位
uint32_t value = (allowed_0 | desired) & allowed_1;
```

### 3.4 IA32_VMX_PINBASED_CTLS (0x481) — 位定义

| 位 | 名称 | 描述 |
|----|------|------|
| 0 | **External-interrupt exiting** | 外部中断触发 VM-Exit |
| 3 | **NMI exiting** | NMI 触发 VM-Exit |
| 5 | **Virtual NMIs** | 启用虚拟 NMI |
| 6 | **Activate VMX-preemption timer** | 启用 VMX-preemption timer |
| 7 | **Process posted interrupts** | 启用 posted-interrupt 处理 |

### 3.5 IA32_VMX_PROCBASED_CTLS (0x482) — 位定义

| 位 | 名称 | 描述 |
|----|------|------|
| 2 | **Interrupt-window exiting** | RFLAGS.IF=1 时触发 VM-Exit |
| 3 | **Use TSC offsetting** | 启用 TSC 偏移 |
| 7 | **HLT exiting** | HLT 指令触发 VM-Exit |
| 9 | **INVLPG exiting** | INVLPG 指令触发 VM-Exit |
| 10 | **MWAIT exiting** | MWAIT 指令触发 VM-Exit |
| 11 | **RDPMC exiting** | RDPMC 指令触发 VM-Exit |
| 12 | **RDTSC exiting** | RDTSC 指令触发 VM-Exit |
| 15 | **CR3-load exiting** | MOV CR3 触发 VM-Exit |
| 16 | **CR3-store exiting** | 读取 CR3 触发 VM-Exit |
| 19 | **CR8-load exiting** | MOV CR8 写触发 VM-Exit |
| 20 | **CR8-store exiting** | 读 CR8 触发 VM-Exit |
| 21 | **Use TPR shadow** | 启用 TPR 影子 |
| 22 | **NMI-window exiting** | 无 NMI 阻塞时触发 VM-Exit |
| 23 | **MOV-DR exiting** | MOV DR 指令触发 VM-Exit |
| 24 | **Unconditional I/O exiting** | IN/OUT 无条件触发 VM-Exit |
| 25 | **Use I/O bitmaps** | 使用 I/O 位图 |
| 27 | **Monitor trap flag** | 启用 MTF（单步） |
| 28 | **Use MSR bitmaps** | 使用 MSR 位图 |
| 29 | **MONITOR exiting** | MONITOR 指令触发 VM-Exit |
| 30 | **PAUSE exiting** | PAUSE 指令触发 VM-Exit |
| 31 | **Activate secondary controls** | 启用 secondary 控制字段 |
| — | **Activate tertiary controls** (bit 17) | 启用 tertiary 控制字段 |

### 3.6 IA32_VMX_PROCBASED_CTLS2 (0x48B) — 位定义

| 位 | 名称 | 描述 | SDM § |
|----|------|------|-------|
| 0 | **Virtualize APIC accesses** | 虚拟化 APIC 访问 | 29.4 |
| 1 | **Enable EPT** | 启用扩展页表 | 28.3 |
| 2 | **Descriptor-table exiting** | LGDT/IDT/LLDT/LTR 等触发 VM-Exit | 25.1.2 |
| 3 | **Enable RDTSCP** | 启用 RDTSCP 指令 | — |
| 4 | **Virtualize x2APIC mode** | 虚拟化 x2APIC 模式 | 29.5 |
| 5 | **Enable VPID** | 启用 VPID | 28.1 |
| 6 | **WBINVD exiting** | WBINVD/WBNOINVD 触发 VM-Exit | 25.1.2 |
| 7 | **Unrestricted guest** | 允许无保护模式 Guest | 25.6 |
| 8 | **APIC-register virtualization** | APIC 寄存器虚拟化 | 29.4/29.5 |
| 9 | **Virtual-interrupt delivery** | 虚拟中断交付 | 29.4.2 |
| 10 | **PAUSE-loop exiting** | PAUSE 循环触发 VM-Exit | 24.6.13 |
| 11 | **RDRAND exiting** | RDRAND 触发 VM-Exit | 25.1.3 |
| 12 | **Enable INVPCID** | 启用 INVPCID 指令 | — |
| 13 | **Enable VM functions** | 启用 VMFUNC 指令 | 25.5.6 |
| 14 | **VMCS shadowing** | VMCS 影子 | 24.10/30.3 |
| 15 | **Enable ENCLS exiting** | ENCLS 指令触发 VM-Exit | 24.6.16 |
| 16 | **RDSEED exiting** | RDSEED 触发 VM-Exit | 25.1.3 |
| 17 | **Enable PML** | 页面修改日志 | 28.3.6 |
| 18 | **EPT-violation #VE** | EPT 违规触发 #VE 而非 VM-Exit | 25.5.7 |
| 19 | **Conceal VMX from PT** | 隐藏 VMX 于 Intel PT | — |
| 20 | **Enable XSAVES/XRSTORS** | 启用 XSAVES/XRSTORS 指令 | — |
| 22 | **Mode-based execute control for EPT** | EPT 用户/管理模式执行控制 | 28 |
| 23 | **Sub-page write permissions for EPT** | 子页写权限 | 28.3.4 |
| 25 | **Use TSC scaling** | TSC 缩放 | 24.6.5/25.3 |
| 26 | **Enable user wait and pause** | 启用 TPAUSE/UMONITOR/UMWAIT | — |
| 28 | **Enable ENCLV exiting** | ENCLV 指令触发 VM-Exit | 24.6.17 |

### 3.7 IA32_VMX_EXIT_CTLS (0x483) — 位定义

| 位 | 名称 | 描述 |
|----|------|------|
| 2 | **Save debug controls** | 保存 DR7 和 IA32_DEBUGCTL |
| 9 | **Host address-space size** | 1=Host 为 64 位模式 |
| 12 | **Load IA32_PERF_GLOBAL_CTRL** | VM-Exit 时加载 PERF_GLOBAL_CTRL |
| 15 | **Acknowledge interrupt on exit** | ACK 中断控制器 |
| 18 | **Save IA32_PAT** | 保存 PAT MSR |
| 19 | **Load IA32_PAT** | 加载 PAT MSR |
| 20 | **Save IA32_EFER** | 保存 EFER MSR |
| 21 | **Load IA32_EFER** | 加载 EFER MSR |
| 22 | **Save VMX-preemption timer value** | 保存 Preemption Timer 值 |
| 23 | **Clear IA32_BNDCFGS** | 清除 BNDCFGS（MPX） |
| 24 | **Conceal VMX from PT** | 隐藏 VMX 于 Intel PT |
| 25 | **Clear IA32_RTIT_CTL** | 清除处理器跟踪控制 |
| 26 | **Clear LBR_CTL** | 清除 LBR 控制 |
| 28 | **Load CET state** | 加载 CET 状态 |
| 29 | **Load PKRS** | 加载保护键 MSR |
| 30 | **Save IA32_PERF_GLOBAL_CTRL** | 保存 PERF_GLOBAL_CTRL |
| 31 | **Activate secondary VM-exit controls** | 启用 secondary VM-exit 控制 |

**Default1 位**（当 IA32_VMX_BASIC[55]=0 时必须为 1）:
`0x00036dff` — bits 0-8, 10, 11, 13, 14, 16, 17

### 3.8 IA32_VMX_ENTRY_CTLS (0x484) — 位定义

| 位 | 名称 | 描述 |
|----|------|------|
| 2 | **Load debug controls** | 加载 DR7 和 IA32_DEBUGCTL |
| 9 | **IA-32e mode guest** | Guest 进入 IA-32e 模式 |
| 10 | **Entry to SMM** | VM-Entry 进入 SMM |
| 11 | **Deactivate dual-monitor treatment** | 停用双监视器 SMM |
| 13 | **Load IA32_PERF_GLOBAL_CTRL** | VM-Entry 时加载 PERF_GLOBAL_CTRL |
| 14 | **Load IA32_PAT** | 加载 PAT MSR |
| 15 | **Load IA32_EFER** | 加载 EFER MSR |
| 16 | **Load IA32_BNDCFGS** | 加载 BNDCFGS（MPX） |
| 17 | **Conceal VMX from PT** | 隐藏 VMX 于 Intel PT |
| 18 | **Load IA32_RTIT_CTL** | 加载处理器跟踪控制 |
| 20 | **Load CET state** | 加载 CET 状态 |
| 21 | **Load IA32_LBR_CTL** | 加载 LBR 控制 |
| 22 | **Load PKRS** | 加载保护键 MSR |

**Default1 位**（当 IA32_VMX_BASIC[55]=0 时必须为 1）:
`0x000011ff` — bits 0-8, 12

### 3.9 IA32_VMX_MISC (0x485) — 位布局

| 位 | 字段 | 描述 |
|----|------|------|
| 4:0 | **Preemption timer TSC bit** | Preemption Timer 递减率 = TSC bit X 每变化一次 |
| 5 | **Save EFER.LMA** | VM-Exit 时保存 IA32_EFER.LMA 到 IA-32e mode guest entry control |
| 6 | **Activity state HLT** | 支持活动状态 1 (HLT) |
| 7 | **Activity state SHUTDOWN** | 支持活动状态 2 (Shutdown) |
| 8 | **Activity state WAIT-FOR-SIPI** | 支持活动状态 3 (Wait-for-SIPI) |
| 14 | **Processor Trace** | Intel PT 可在 VMX 操作中使用 |
| 15 | **SMBASE MSR readable** | RDMSR 可在 SMM 中读取 IA32_SMBASE |
| 24:16 | **CR3-target count** | 支持的 CR3-Target 数量（0-256，bit 24=1 时值为256） |
| 27:25 | **Max MSR list size** | MSR 列表最大条目数 = 512 * (N+1) |
| 28 | **Block SMI support** | IA32_SMM_MONITOR_CTL[2] 可设置 |
| 29 | **VMWRITE shadow RO** | VMWRITE 可以修改所有 VMCS 字段 |
| 30 | **Zero-length injection** | 支持指令长度为 0 的软件异常注入 |
| 63:32 | **MSEG revision ID** | MSEG 修订标识符 |

### 3.10 IA32_VMX_EPT_VPID_CAP (0x48C) — 位布局

| 位 | 描述 |
|----|------|
| 0 | **Execute-only translations** — 支持仅执行映射（无读权限） |
| 6 | **EPT accessed and dirty flags** — 支持访问位和脏位 |
| 8 | **Mode-based execute control for EPT** — 支持按模式区分执行权限 |
| 14 | **Reserved bit 6 for EPTP** — PML4 中的第 6 位（脏/访问） |
| 16 | **EPT page-walk length 4** — 支持 4 级页表遍历（5 级） |
| 20 | **EPT accessed/dirty flag for leaf entries only** |
| 21 | **EPT accessed/dirty flag for all entries** |
| 22 | **EPT paging-write (PW) accessed/dirty** |
| 23 | **Enable Execute-only for mode-based execute** |
| 24 | **Sub-page write permissions** — 支持子页写权限 |
| 25 | **EPT-violation #VE** — 支持 #VE |
| 26 | **AD1** — Accessed bit 更新抑制 |
| 27 | **HLAT** — Hypervisor-managed linear address translation |
| 32 | **INVVPID** — 支持 INVVPID 指令 |
| 33 | **INVVPID individual-address** — 支持 Individual-Address 类型 |
| 34 | **INVVPID single-context** — 支持 Single-Context 类型 |
| 35 | **INVVPID all-context** — 支持 All-Context 类型 |
| 36 | **INVVPID single-context-retaining-globals** — 支持保留全局的单上下文类型 |
| 40 | **INVEPT** — 支持 INVEPT 指令 |
| 41 | **INVEPT single-context** — 支持 Single-Context 类型 |
| 42 | **INVEPT all-context** — 支持 All-Context 类型 |
| 48 | **Enable VM functions** — 支持 VM 函数 |
| 49 | **EPTP switching** — 支持 EPTP 切换 |

### 3.11 True MSR 地址

| MSR 名称 | 地址 |
|----------|------|
| `IA32_VMX_TRUE_PINBASED_CTLS` | `0x48D` |
| `IA32_VMX_TRUE_PROCBASED_CTLS` | `0x48E` |
| `IA32_VMX_TRUE_EXIT_CTLS` | `0x48F` |
| `IA32_VMX_TRUE_ENTRY_CTLS` | `0x490` |
| `IA32_VMX_VMFUNC` | `0x491` |
| `IA32_VMX_PROCBASED_CTLS3` | `0x492` |
| `IA32_VMX_EXIT_CTLS2` | `0x493` |

**注意**: 当 `IA32_VMX_BASIC[55]=1` 时，应使用 True MSR 代替原始 MSR，
因为 True MSR 提供更准确的允许位设置信息（特别是 Default1 位在原始 MSR 中被错误报告为必须为 1 的情况）。

---

## 4. VM-Execution 控制字段

### 4.1 Pin-Based VM-Execution Controls (编码 `4000H`)

32 位字段，控制影响处理器固定引脚的中断和定时器事件。

| 位 | 名称 | 描述 |
|----|------|------|
| 0 | **External-interrupt exiting** | 1=外部中断触发 VM-Exit |
| 1-2 | 保留 | 必须为 0 |
| 3 | **NMI exiting** | 1=NMI 触发 VM-Exit |
| 4 | 保留 | 必须为 0 |
| 5 | **Virtual NMIs** | 1=启用虚拟 NMI，NMI 被虚拟化 |
| 6 | **Activate VMX-preemption timer** | 1=启用 VMX Preemption Timer |
| 7 | **Process posted interrupts** | 1=启用 Posted-Interrupt 处理 |
| 8-31 | 保留 | 必须为 0 |

### 4.2 Primary Processor-Based VM-Execution Controls (编码 `4002H`)

32 位字段，控制非根操作中的指令和事件拦截。

| 位 | 名称 | 描述 |
|----|------|------|
| 0-1 | 保留 | 必须为 0 |
| 2 | **Interrupt-window exiting** | RFLAGS.IF=1 且无 STI/MOV-SS 阻塞时触发 VM-Exit |
| 3 | **Use TSC offsetting** | 对 RDTSC/RDTSCP 应用 TSC 偏移 |
| 4-6 | 保留 | 必须为 0 |
| 7 | **HLT exiting** | HLT 指令触发 VM-Exit |
| 8 | 保留 | 必须为 0 |
| 9 | **INVLPG exiting** | INVLPG 指令触发 VM-Exit |
| 10 | **MWAIT exiting** | MWAIT 指令触发 VM-Exit |
| 11 | **RDPMC exiting** | RDPMC 指令触发 VM-Exit |
| 12 | **RDTSC exiting** | RDTSC/RDTSCP 指令触发 VM-Exit |
| 13-14 | 保留 | 必须为 0 |
| 15 | **CR3-load exiting** | MOV CR3 写触发 VM-Exit |
| 16 | **CR3-store exiting** | 读取 CR3 触发 VM-Exit |
| 17 | **Activate tertiary controls** | 启用 Tertiary Processor-Based 控制 |
| 18 | 保留 | 必须为 0 |
| 19 | **CR8-load exiting** | MOV CR8 写触发 VM-Exit |
| 20 | **CR8-store exiting** | 读 CR8 触发 VM-Exit |
| 21 | **Use TPR shadow** | 启用 TPR 影子机制 |
| 22 | **NMI-window exiting** | 无虚拟 NMI 阻塞时触发 VM-Exit |
| 23 | **MOV-DR exiting** | MOV DR 指令触发 VM-Exit |
| 24 | **Unconditional I/O exiting** | IN/OUT 无条件触发 VM-Exit |
| 25 | **Use I/O bitmaps** | 使用 I/O 位图（地址在两个位图指针中） |
| 26 | 保留 | 必须为 0 |
| 27 | **Monitor trap flag** | 启用 Monitor Trap Flag（单步跟踪） |
| 28 | **Use MSR bitmaps** | 使用 MSR 位图 |
| 29 | **MONITOR exiting** | MONITOR 指令触发 VM-Exit |
| 30 | **PAUSE exiting** | PAUSE 指令触发 VM-Exit |
| 31 | **Activate secondary controls** | 启用 Secondary Processor-Based 控制（编码 `401EH`） |

### 4.3 Secondary Processor-Based Controls (编码 `401EH`) — 完整清单

通过主控制字段 bit 31 启用。

| 位 | 名称 | 描述 |
|----|------|------|
| 0 | **Virtualize APIC accesses** | 虚拟化 APIC 访问页面的访问 |
| 1 | **Enable EPT** | 启用扩展页表（二级地址转换） |
| 2 | **Descriptor-table exiting** | LGDT/LIDT/SGDT/SIDT/LLDT/LTR/SLDT/STR 触发 VM-Exit |
| 3 | **Enable RDTSCP** | 启用 RDTSCP 指令（否则 #UD） |
| 4 | **Virtualize x2APIC mode** | 虚拟化 x2APIC MSR 访问 |
| 5 | **Enable VPID** | 启用虚拟处理器 ID 标记 TLB |
| 6 | **WBINVD exiting** | WBINVD/WBNOINVD 触发 VM-Exit |
| 7 | **Unrestricted guest** | Guest 可在无分页保护模式或实模式下运行 |
| 8 | **APIC-register virtualization** | 虚拟化 APIC 寄存器访问 |
| 9 | **Virtual-interrupt delivery** | 虚拟中断评估和交付 |
| 10 | **PAUSE-loop exiting** | PAUSE 循环超过阈值时触发 VM-Exit |
| 11 | **RDRAND exiting** | RDRAND 触发 VM-Exit |
| 12 | **Enable INVPCID** | 启用 INVPCID 指令（否则 #UD） |
| 13 | **Enable VM functions** | 启用 VMFUNC 指令 |
| 14 | **VMCS shadowing** | 非根操作中的 VMREAD/VMWRITE 访问影子 VMCS |
| 15 | **Enable ENCLS exiting** | ENCLS 指令按位图触发 VM-Exit |
| 16 | **RDSEED exiting** | RDSEED 触发 VM-Exit |
| 17 | **Enable PML** | 启用页面修改日志 |
| 18 | **EPT-violation #VE** | EPT 违规触发 #VE（虚拟化异常）而非 VM-Exit |
| 19 | **Conceal VMX from PT** | 隐藏 VMX 于 Intel Processor Trace |
| 20 | **Enable XSAVES/XRSTORS** | 启用 XSAVES/XRSTORS 指令（否则 #UD） |
| 21 | 保留 | 必须为 0 |
| 22 | **Mode-based execute control for EPT** | EPT 执行权限依赖线性地址的用户/管理模式 |
| 23 | **Sub-page write permissions for EPT** | 128 字节粒度 EPT 写权限 |
| 24 | **Intel PT uses guest physical addresses** | PT 输出地址经 EPT 转换 |
| 25 | **Use TSC scaling** | 启用 TSC 缩放（TSC 乘数） |
| 26 | **Enable user wait and pause** | 启用 TPAUSE/UMONITOR/UMWAIT 指令 |
| 27 | 保留 | 必须为 0 |
| 28 | **Enable ENCLV exiting** | ENCLV 指令按位图触发 VM-Exit |
| 29-31 | 保留 | 必须为 0 |

### 4.4 Tertiary Processor-Based Controls (编码待查)

通过主控制字段 bit 17 启用。64 位字段。

| 位 | 名称 | 描述 |
|----|------|------|
| 0 | **LOADIWKEY exiting** | LOADIWKEY 触发 VM-Exit |
| 4 | **Enable IPI virtualization** | 启用 IPI 虚拟化 |
| — | *(其余位保留)* | |

### 4.5 异常位图 (编码 `4004H`)

32 位字段，每位对应一个异常向量（bit 0=#DE, bit 1=#DB, ..., bit 31=#XF）。

| 位 | 异常 | 助记符 |
|----|------|--------|
| 0 | 除法错误 | #DE |
| 1 | 调试异常 | #DB |
| 2 | NMI 中断 | — |
| 3 | 断点 | #BP |
| 4 | 溢出 | #OF |
| 5 | 边界范围 | #BR |
| 6 | 非法操作码 | #UD |
| 7 | 设备不可用 | #NM |
| 8 | 双重错误 | #DF |
| 9 | 协处理器段越限 | — |
| 10 | 无效 TSS | #TS |
| 11 | 段不存在 | #NP |
| 12 | 堆栈错误 | #SS |
| 13 | 通用保护 | #GP |
| 14 | 页错误 | #PF |
| 16 | x87 FPU 错误 | #MF |
| 17 | 对齐检查 | #AC |
| 18 | 机器检查 | #MC |
| 19 | SIMD 浮点 | #XM/#XF |
| 20 | 虚拟化异常 | #VE |

**Page-Fault Error-Code Mask/Match**: 当异常位图 bit 14 置位时，
页错误触发 VM-Exit 还需要错误码满足 `(error_code & mask) == match`。

### 4.6 CR3-Target 控制

| 字段 | 编码 | 描述 |
|------|------|------|
| CR3-target count | `400AH` | 0-4，指定 CR3-target 值数量 |
| CR3-target value 0-3 | `6008H-600EH` | CR3-load 不触发 VM-Exit 的目标 CR3 值 |
| CR0 guest/host mask | `6000H` | 指定被 Host 控制的 CR0 位（Guest 修改会触发 VM-Exit） |
| CR4 guest/host mask | `6002H` | 指定被 Host 控制的 CR4 位 |
| CR0 read shadow | `6004H` | Guest 读取 CR0 时返回的值 |
| CR4 read shadow | `6006H` | Guest 读取 CR4 时返回的值 |

---

## 5. VM-Exit 控制字段

### 5.1 Primary VM-Exit Controls (编码 `400CH`)

32 位字段，控制 VM-Exit 时的处理器行为。

| 位 | 名称 | 描述 |
|----|------|------|
| 0-1 | 保留 | Default1 |
| 2 | **Save debug controls** | 保存 DR7 和 IA32_DEBUGCTL 到 Guest 状态区 |
| 3-8 | 保留 | Default1 |
| 9 | **Host address-space size** | 1=退出后 Host 在 64 位模式（CS.L=1, EFER.LMA=1） |
| 10-11 | 保留 | Default1 |
| 12 | **Load IA32_PERF_GLOBAL_CTRL** | 从 Host 状态区加载 PERF_GLOBAL_CTRL |
| 13-14 | 保留 | Default1 |
| 15 | **Acknowledge interrupt on exit** | 外部中断触发 VM-Exit 时 ACK 中断控制器，向量存入 VM-Exit interruption info |
| 16-17 | 保留 | Default1 |
| 18 | **Save IA32_PAT** | 保存 PAT MSR |
| 19 | **Load IA32_PAT** | 加载 Host PAT MSR |
| 20 | **Save IA32_EFER** | 保存 EFER MSR |
| 21 | **Load IA32_EFER** | 加载 Host EFER MSR |
| 22 | **Save VMX-preemption timer value** | 将当前 Preemption Timer 值保存到 VMCS 字段 |
| 23 | **Clear IA32_BNDCFGS** | 清除 BNDCFGS（MPX 边界配置） |
| 24 | **Conceal VMX from PT** | 隐藏 VMX 于 Intel PT |
| 25 | **Clear IA32_RTIT_CTL** | 清除处理器跟踪控制 MSR |
| 26 | **Clear LBR_CTL** | 清除 LBR 控制 MSR |
| 27 | 保留 | 必须为 0 |
| 28 | **Load CET state** | 加载 CET 状态 |
| 29 | **Load PKRS** | 加载保护键 MSR |
| 30 | **Save IA32_PERF_GLOBAL_CTRL** | 保存 PERF_GLOBAL_CTRL |
| 31 | **Activate secondary VM-exit controls** | 启用 Secondary VM-Exit 控制 |

### 5.2 Secondary VM-Exit Controls (编码 `400EH` 高阶字段)

通过主 VM-Exit 控制 bit 31 启用。

| 位 | 名称 | 描述 |
|----|------|------|
| 0-63 | *(定义中)* | 由 IA32_VMX_EXIT_CTLS2 (0x493) 报告 |

---

## 6. VM-Entry 控制字段

### 6.1 VM-Entry Controls (编码 `4012H`)

32 位字段，控制 VM-Entry 时的处理器行为。

| 位 | 名称 | 描述 |
|----|------|------|
| 0-1 | 保留 | Default1 |
| 2 | **Load debug controls** | 加载 DR7 和 IA32_DEBUGCTL |
| 3-8 | 保留 | Default1 |
| 9 | **IA-32e mode guest** | 1=Guest 进入 IA-32e 模式（64 位模式） |
| 10 | **Entry to SMM** | 1=VM-Entry 进入 SMM |
| 11 | **Deactivate dual-monitor treatment** | 1=停用双监视器 SMM 处理 |
| 12 | 保留 | Default1 |
| 13 | **Load IA32_PERF_GLOBAL_CTRL** | 加载 PERF_GLOBAL_CTRL |
| 14 | **Load IA32_PAT** | 加载 PAT MSR |
| 15 | **Load IA32_EFER** | 加载 EFER MSR（设置 LME/LMA） |
| 16 | **Load IA32_BNDCFGS** | 加载 BNDCFGS（MPX） |
| 17 | **Conceal VMX from PT** | 隐藏 VMX 于 Intel PT |
| 18 | **Load IA32_RTIT_CTL** | 加载处理器跟踪控制 |
| 19 | 保留 | 必须为 0 |
| 20 | **Load CET state** | 加载 CET 状态 |
| 21 | **Load IA32_LBR_CTL** | 加载 LBR 控制 |
| 22 | **Load PKRS** | 加载保护键 MSR |
| 23-31 | 保留 | 必须为 0 |

### 6.2 VM-Entry Interruption-Information Field (编码 `4016H`)

32 位字段，用于 VM-Entry 时注入事件。

| 位 | 字段 | 描述 |
|----|------|------|
| 7:0 | **Vector** | 中断或异常向量号（0-255） |
| 10:8 | **Interruption Type** | 事件类型（见下表） |
| 11 | **Deliver Error Code** | 1=将错误码注入到 Guest 堆栈 |
| 12-30 | 保留 | 必须为 0 |
| 31 | **Valid** | 1=VM-Entry 时注入该事件 |

**中断类型编码 (Bits 10:8)**:

| 值 | 类型 | 说明 |
|----|------|------|
| 0 | External Interrupt | 外部中断 |
| 1 | Reserved | 保留 |
| 2 | Non-Maskable Interrupt (NMI) | 不可屏蔽中断 |
| 3 | Hardware Exception | 硬件异常（如 #PF, #GP） |
| 4 | Software Interrupt | 软件中断（INT n 指令） |
| 5 | Privileged Software Exception | 特权软件异常（INT1/ICEBP） |
| 6 | Software Exception | 软件异常（INT3/INTO 指令） |
| 7 | Reserved | 保留（用于 Processor Extension） |

---

## 7. VMCS Guest-State 区域

### 7.1 Guest 段寄存器

| VMCS 字段 | 编码 | 宽度 | 描述 |
|-----------|------|------|------|
| Guest ES selector | `0800H` | 16-bit | ES 段选择子 |
| Guest CS selector | `0802H` | 16-bit | CS 段选择子 |
| Guest SS selector | `0804H` | 16-bit | SS 段选择子 |
| Guest DS selector | `0806H` | 16-bit | DS 段选择子 |
| Guest FS selector | `0808H` | 16-bit | FS 段选择子 |
| Guest GS selector | `080AH` | 16-bit | GS 段选择子 |
| Guest LDTR selector | `080CH` | 16-bit | LDTR 段选择子 |
| Guest TR selector | `080EH` | 16-bit | TR 段选择子 |
| Guest ES base | `6806H` | Natural | ES 段基地址 |
| Guest CS base | `6808H` | Natural | CS 段基地址 |
| Guest SS base | `680AH` | Natural | SS 段基地址 |
| Guest DS base | `680CH` | Natural | DS 段基地址 |
| Guest FS base | `680EH` | Natural | FS 段基地址 |
| Guest GS base | `6810H` | Natural | GS 段基地址 |
| Guest LDTR base | `6812H` | Natural | LDTR 段基地址 |
| Guest TR base | `6814H` | Natural | TR 段基地址 |
| Guest ES limit | `4800H` | 32-bit | ES 段限长 |
| Guest CS limit | `4802H` | 32-bit | CS 段限长 |
| Guest SS limit | `4804H` | 32-bit | SS 段限长 |
| Guest DS limit | `4806H` | 32-bit | DS 段限长 |
| Guest FS limit | `4808H` | 32-bit | FS 段限长 |
| Guest GS limit | `480AH` | 32-bit | GS 段限长 |
| Guest LDTR limit | `480CH` | 32-bit | LDTR 段限长 |
| Guest TR limit | `480EH` | 32-bit | TR 段限长 |
| Guest ES access rights | `4814H` | 32-bit | ES 访问权限（AR 字节） |
| Guest CS access rights | `4816H` | 32-bit | CS 访问权限 |
| Guest SS access rights | `4818H` | 32-bit | SS 访问权限 |
| Guest DS access rights | `481AH` | 32-bit | DS 访问权限 |
| Guest FS access rights | `481CH` | 32-bit | FS 访问权限 |
| Guest GS access rights | `481EH` | 32-bit | GS 访问权限 |
| Guest LDTR access rights | `4820H` | 32-bit | LDTR 访问权限 |
| Guest TR access rights | `4822H` | 32-bit | TR 访问权限 |

**段访问权限 (Access Rights) 字段格式** (32-bit，低 16 位有效):

| 位 | 字段 | 描述 |
|----|------|------|
| 3:0 | **Type** | 段类型（代码/数据/系统） |
| 4 | **S** | 0=系统段，1=代码/数据段 |
| 5 | **DPL** | 描述符特权级 |
| 6:7 | **P** | Segment Present |
| 8-11 | **Reserved** | 保留，必须为 0 |
| 12 | **AVL** | Available for software use |
| 13 | **L** | 64 位代码段（仅 CS） |
| 14 | **D/B** | Default operation size |
| 15 | **G** | Granularity（0=字节，1=4KB） |
| 16-31 | **Reserved** | 保留，必须为 0 |

**重要**: 对于 CS 段，当 L（bit 13）=1 时，D/B（bit 14）必须为 0。

### 7.2 GDTR/IDTR

| VMCS 字段 | 编码 | 宽度 |
|-----------|------|------|
| Guest GDTR base | `6816H` | Natural |
| Guest IDTR base | `6818H` | Natural |
| Guest GDTR limit | `4810H` | 32-bit |
| Guest IDTR limit | `4812H` | 32-bit |

### 7.3 Guest 控制寄存器和调试寄存器

| VMCS 字段 | 编码 | 宽度 | 描述 |
|-----------|------|------|------|
| Guest CR0 | `6800H` | Natural | 控制寄存器 0 |
| Guest CR3 | `6802H` | Natural | 页目录基址 |
| Guest CR4 | `6804H` | Natural | 控制寄存器 4 |
| Guest DR7 | `681AH` | Natural | 调试寄存器 7 |

### 7.4 Guest RIP、RSP、RFLAGS

| VMCS 字段 | 编码 | 宽度 | 描述 |
|-----------|------|------|------|
| Guest RSP | `681CH` | Natural | 堆栈指针 |
| Guest RIP | `681EH` | Natural | 指令指针（必须规范地址） |
| Guest RFLAGS | `6820H` | Natural | 标志寄存器（VM=0, IF 按需设置） |

### 7.5 Guest 非寄存器状态

#### Guest Interruptibility State (编码 `4824H`)

| 位 | 名称 | 描述 |
|----|------|------|
| 0 | **Blocking by STI** | STI 指令后的单指令中断阻塞 |
| 1 | **Blocking by MOV/POP SS** | MOV/POP SS 指令后的单指令中断阻塞 |
| 2 | **Blocking by SMI** | SMM 模式中，阻塞 SMI |
| 3 | **Blocking by NMI** | 虚拟 NMI 已注入，阻塞后续 NMI 直到 IRET |
| 4-31 | 保留 | 必须为 0 |

**注入阻塞规则**:
- **硬件中断（可屏蔽）**: 被 STI_BLOCKING 或 MOVSS_BLOCKING 阻塞
- **NMI 注入**: 被 NMI_BLOCKING、STI_BLOCKING、MOVSS_BLOCKING 阻塞
- **IRET 处理**: Virtual NMIs 启用时，IRET 触发异常会自动清除 NMI 阻塞

#### Guest Activity State (编码 `4826H`)

| 值 | 名称 | 描述 |
|----|------|------|
| 0 | **Active** | 正常执行指令 |
| 1 | **HLT** | 执行 HLT 指令后 |
| 2 | **Shutdown** | 三重故障等严重错误 |
| 3 | **Wait-for-SIPI** | 等待 SIPI（用于 TXT/ACM） |

#### Guest Pending Debug Exceptions (编码 `6822H`)

| 位 | 名称 | 描述 |
|----|------|------|
| 0 | **BS** | 待处理的单步调试异常 |
| 1 | 保留 | |
| 2 | 保留 | |
| 3 | **B3** | 断点条件 3 |
| ... | ... | ... |
| 11 | **Bac** |  |
| 12-13 | 保留 | |
| 14 | **Enabled** | 启用位 |
| 15-63 | 保留 | |

### 7.6 Guest MSR 状态

| VMCS 字段 | 编码 | 宽度 | 条件 |
|-----------|------|------|------|
| Guest SYSENTER_CS | `482AH` | 32-bit | |
| Guest SYSENTER_ESP | `6824H` | Natural | |
| Guest SYSENTER_EIP | `6826H` | Natural | |
| Guest IA32_DEBUGCTL | `2802H` | 64-bit | |
| Guest IA32_PAT | `2804H` | 64-bit | 需 Load/Save IA32_PAT 控制 |
| Guest IA32_EFER | `2806H` | 64-bit | 需 Load/Save IA32_EFER 控制 |
| Guest IA32_PERF_GLOBAL_CTRL | `2808H` | 64-bit | 需 Load PERF_GLOBAL_CTRL 控制 |
| Guest PDPTE0-3 | `280AH-2810H` | 64-bit | 仅当 EPT 未启用且 CR4.PAE=1 时使用 |
| Guest interrupt status | `0810H` | 16-bit | 用于虚拟中断交付 |
| VMCS link pointer | `2800H` | 64-bit | 保留的 VMCS 链指针（低 12 位=0） |
| VMX-preemption timer value | `482EH` | 32-bit | Preemption Timer 计数值 |

---

## 8. VMCS Host-State 区域

### 8.1 Host 段寄存器

| VMCS 字段 | 编码 | 宽度 | 注意 |
|-----------|------|------|------|
| Host ES selector | `0C00H` | 16-bit | 必须有效 |
| Host CS selector | `0C02H` | 16-bit | 必须有效，对应 VM-Exit 后模式 |
| Host SS selector | `0C04H` | 16-bit | 必须有效 |
| Host DS selector | `0C06H` | 16-bit | 必须有效 |
| Host FS selector | `0C08H` | 16-bit | 必须有效 |
| Host GS selector | `0C0AH` | 16-bit | 必须有效 |
| Host TR selector | `0C0CH` | 16-bit | 必须有效，TSS 段 |

### 8.2 Host 段基地址

| VMCS 字段 | 编码 | 宽度 | 注意 |
|-----------|------|------|------|
| Host FS base | `6C06H` | Natural | 必须规范地址 |
| Host GS base | `6C08H` | Natural | 必须规范地址 |
| Host TR base | `6C0AH` | Natural | 必须规范地址 |

### 8.3 Host GDTR/IDTR

| VMCS 字段 | 编码 | 宽度 |
|-----------|------|------|
| Host GDTR base | `6C0CH` | Natural |
| Host IDTR base | `6C0EH` | Natural |

### 8.4 Host 控制寄存器和 MSR

| VMCS 字段 | 编码 | 宽度 | 注意 |
|-----------|------|------|------|
| Host CR0 | `6C00H` | Natural | 必须符合 IA32_VMX_CR0_FIXED0/1 |
| Host CR3 | `6C02H` | Natural | 必须有效页表 |
| Host CR4 | `6C04H` | Natural | 必须符合 IA32_VMX_CR4_FIXED0/1 |
| Host RSP | `6C14H` | Natural | VM-Exit 后的 RSP |
| Host RIP | `6C16H` | Natural | VM-Exit 后的 RIP（VMM 入口） |
| Host IA32_SYSENTER_CS | `4C00H` | 32-bit | |
| Host IA32_SYSENTER_ESP | `6C10H` | Natural | |
| Host IA32_SYSENTER_EIP | `6C12H` | Natural | |
| Host IA32_PAT | `2C00H` | 64-bit | 需 Load IA32_PAT VM-Exit 控制 |
| Host IA32_EFER | `2C02H` | 64-bit | 需 Load IA32_EFER VM-Exit 控制 |
| Host IA32_PERF_GLOBAL_CTRL | `2C04H` | 64-bit | 需 Load PERF_GLOBAL_CTRL VM-Exit 控制 |

---

## 9. VM-Exit 信息字段

VM-Exit 时由处理器写入的只读信息。

### 9.1 Exit Reason (编码 `4402H`)

32 位字段：

| 位 | 字段 | 描述 |
|----|------|------|
| 15:0 | **Basic exit reason** | 基本退出原因码 |
| 26:16 | 保留 | |
| 27 | **Pending MTF VM-exit** | 挂起的 MTF VM-Exit（被 SMM VM-Exit 覆盖） |
| 28 | **VM-exit from VMX root** | 从 VMX 根操作退出（仅 SMM） |
| 29 | 保留 | |
| 30 | **VM-entry failure** | 1=VM-Entry 失败（非真正的 VM-Exit） |
| 31 | 保留 | |

### 9.2 Exit Qualification (编码 `6400H`)

Natural-width 字段，VM-Exit 时填充的详细信息，含义因退出原因而异。

各退出原因的 Exit Qualification 解释：

| 退出原因 | Exit Qualification 含义 |
|----------|------------------------|
| CR Access | CR 号、访问类型、GPR、LMSW 数据 |
| EPT Violation | 访问类型、权限位、GLA 有效性等 |
| I/O Instruction | I/O 端口号、方向、字符串/重复信息 |
| APIC Access | 偏移、访问类型等 |
| MOV DR | DR 号、访问类型、GPR |

### 9.3 Guest-Linear Address (编码 `640AH`)

Natural-width，某些 VM-Exit 填充的 Guest 线性地址。

### 9.4 Guest-Physical Address (编码 `2400H`)

64-bit，EPT 违规/配置错误时填充的 Guest 物理地址。

### 9.5 其他 Exit 信息字段

| VMCS 字段 | 编码 | 宽度 | 描述 |
|-----------|------|------|------|
| VM-exit instruction length | `440CH` | 32-bit | 导致 VM-Exit 的指令长度 |
| VM-exit instruction information | `440EH` | 32-bit | 指令解码信息 |
| VM-exit interruption information | `4404H` | 32-bit | 导致 VM-Exit 的中断/异常信息 |
| VM-exit interruption error code | `4406H` | 32-bit | 中断/异常错误码 |
| IDT-vectoring information | `4408H` | 32-bit | VM-Exit 发生时正在交付的事件 |
| IDT-vectoring error code | `440AH` | 32-bit | 对应错误码 |
| I/O RCX | `6402H` | Natural | I/O 指令的 RCX 值 |
| I/O RSI | `6404H` | Natural | I/O 指令的 RSI 值 |
| I/O RDI | `6406H` | Natural | I/O 指令的 RDI 值 |
| I/O RIP | `6408H` | Natural | I/O 指令的 RIP 值 |
| VM-instruction error | `4400H` | 32-bit | VMLAUNCH/VMRESUME 失败原因 |

### 9.6 VM-Exit Interruption Information (编码 `4404H`) 格式

| 位 | 字段 | 描述 |
|----|------|------|
| 7:0 | **Vector** | 导致 VM-Exit 的事件向量号 |
| 10:8 | **Type** | 类型（同 Entry Interruption Info） |
| 11 | **Error code valid** | 1=错误码有效 |
| 12 | **NMI unblocking** | 1=由于 IRET 导致 NMI 阻塞状态变化 |
| 12 | **VINTR valid** | 用于其他中断类型 |
| 30:13 | 保留 | |
| 31 | **Valid** | 1=信息有效 |

### 9.7 IDT-Vectoring Information (编码 `4408H`) 格式

| 位 | 字段 | 描述 |
|----|------|------|
| 7:0 | **Vector** | 正在交付的事件向量号 |
| 10:8 | **Type** | 类型（同 Entry Interruption Info） |
| 11 | **Error code valid** | 1=错误码有效 |
| 12 | 未定义 | 内容不可预测 |
| 30:13 | 保留 | 必须为 0 |
| 31 | **Valid** | 1=VM-Exit 发生在事件交付过程中 |

---

## 10. VM-Exit 原因码完整表

### 基本退出原因 (Basic Exit Reason)

| 码 | 十六进制 | 名称 | 描述 |
|----|----------|------|------|
| 0 | `00` | **Exception or NMI** | 异常位图命中或 NMI 到达时触发 |
| 1 | `01` | **External Interrupt** | 外部中断且 External-interrupt exiting=1 |
| 2 | `02` | **Triple Fault** | 三重故障（#DF 处理中的#PF） |
| 3 | `03` | **INIT Signal** | INIT 信号到达 VMX 非根模式 |
| 4 | `04` | **SIPI** | Startup IPI 到达 Wait-for-SIPI 状态 |
| 5 | `05` | **I/O SMI** | I/O 指令后的 SMI（SMM） |
| 6 | `06` | **Other SMI** | 其他原因 SMI（SMM） |
| 7 | `07` | **Interrupt Window** | RFLAGS.IF=1，无 STI/MOV SS 阻塞，Interrupt-window exiting=1 |
| 8 | `08` | **NMI Window** | 无 NMI 阻塞，NMI-window exiting=1 |
| 9 | `09` | **Task Switch** | Guest 尝试任务切换 |
| 10 | `0A` | **CPUID** | CPUID 指令执行 |
| 11 | `0B` | **GETSEC** | GETSEC 指令执行 |
| 12 | `0C` | **HLT** | HLT 指令且 HLT exiting=1 |
| 13 | `0D` | **INVD** | INVD 指令执行 |
| 14 | `0E` | **INVLPG** | INVLPG 指令且 INVLPG exiting=1 |
| 15 | `0F` | **RDPMC** | RDPMC 指令且 RDPMC exiting=1 |
| 16 | `10` | **RDTSC** | RDTSC 指令且 RDTSC exiting=1 |
| 17 | `11` | **RSM** | RSM 指令在 SMM 中执行 |
| 18 | `12` | **VMCALL** | VMCALL 指令执行 |
| 19 | `13` | **VMCLEAR** | VMCLEAR 指令执行 |
| 20 | `14` | **VMLAUNCH** | VMLAUNCH 指令执行 |
| 21 | `15` | **VMPTRLD** | VMPTRLD 指令执行 |
| 22 | `16` | **VMPTRST** | VMPTRST 指令执行 |
| 23 | `17` | **VMREAD** | VMREAD 指令执行 |
| 24 | `18` | **VMRESUME** | VMRESUME 指令执行 |
| 25 | `19` | **VMWRITE** | VMWRITE 指令执行 |
| 26 | `1A` | **VMXOFF** | VMXOFF 指令执行 |
| 27 | `1B` | **VMXON** | VMXON 指令执行 |
| 28 | `1C` | **CR Access** | CR0/CR3/CR4/CR8 访问（MOV CR, CLTS, LMSW） |
| 29 | `1D` | **MOV DR** | MOV DR 指令且 MOV-DR exiting=1 |
| 30 | `1E` | **I/O Instruction** | IN/OUT 指令触发 |
| 31 | `1F` | **RDMSR** | RDMSR 指令（未在 MSR 位图中过滤） |
| 32 | `20` | **WRMSR** | WRMSR 指令（未在 MSR 位图中过滤） |
| 33 | `21` | **Invalid Guest State** | VM-Entry 失败：Guest 状态无效 |
| 34 | `22` | **MSR Loading Failed** | VM-Entry 失败：MSR 加载失败 |
| 35 | `23` | 保留 | |
| 36 | `24` | **MWAIT** | MWAIT 指令且 MWAIT exiting=1 |
| 37 | `25` | **Monitor Trap Flag** | MTF 触发（单步跟踪） |
| 38 | `26` | 保留 | |
| 39 | `27` | **MONITOR** | MONITOR 指令且 MONITOR exiting=1 |
| 40 | `28` | **PAUSE** | PAUSE 指令且 PAUSE exiting=1 |
| 41 | `29` | **Machine Check at Entry** | VM-Entry 时机器检查异常 |
| 42 | `2A` | 保留 | |
| 43 | `2B` | **TPR Below Threshold** | TPR 影子值低于 TPR 阈值 |
| 44 | `2C` | **APIC Access** | 访问 APIC-access 页面中的物理地址 |
| 45 | `2D` | **Virtualized EOI** | EOI 虚拟化命中 EOI-exit 位图 |
| 46 | `2E` | **GDTR/IDTR Access** | LGDT/LIDT/SGDT/SIDT 且 Descriptor-table exiting=1 |
| 47 | `2F` | **LDTR/TR Access** | LLDT/LTR/SLDT/STR 且 Descriptor-table exiting=1 |
| 48 | `30` | **EPT Violation** | EPT 页表禁止内存访问 |
| 49 | `31` | **EPT Misconfiguration** | EPT 页表项配置错误 |
| 50 | `32` | **INVEPT** | INVEPT 指令执行 |
| 51 | `33` | **RDTSCP** | RDTSCP 指令且 RDTSC exiting=1 |
| 52 | `34` | **Preemption Timer Expired** | VMX Preemption Timer 计时到零 |
| 53 | `35` | **INVVPID** | INVVPID 指令执行 |
| 54 | `36` | **WBINVD** | WBINVD 指令且 WBINVD exiting=1 |
| 55 | `37` | **XSETBV** | XSETBV 指令执行 |
| 56 | `38` | **APIC Write** | 写入虚拟 APIC 页面需要 VMM 虚拟化 |
| 57 | `39` | **RDRAND** | RDRAND 指令且 RDRAND exiting=1 |
| 58 | `3A` | **INVPCID** | INVPCID 指令执行 |
| 59 | `3B` | **VMFUNC** | VMFUNC 指令触发（函数未启用或强制退出） |
| 60 | `3C` | **ENCLS** | ENCLS 指令执行（SGX） |
| 61 | `3D` | **RDSEED** | RDSEED 指令且 RDSEED exiting=1 |
| 62 | `3E` | **PML Full** | 页面修改日志已满 |
| 63 | `3F` | **XSAVES** | XSAVES 指令且未允许 |
| 64 | `40` | **XRSTORS** | XRSTORS 指令且未允许 |
| 65 | `41` | **SPP Event** | 子页保护未命中或配置错误 |
| 66 | `42` | 保留 | |
| 67 | `43` | **UMWAIT** | UMWAIT 指令触发 |
| 68 | `44` | **TPAUSE** | TPAUSE 指令触发 |
| 69-73 | `45-49` | 保留 | |
| 74 | `4A` | **BUS_LOCK** | 总线锁定检测 |
| 75 | `4B` | **NOTIFY** | 通知窗口 VM-Exit |

### VM-Entry 失败原因 (bit 30 置位)

当 Exit Reason 的 bit 30 为 1 时，原因码表示 VM-Entry 失败原因：

| 码 | 名称 | 描述 |
|----|------|------|
| 33 | VMX_INVALID_GUEST_STATE | Guest 状态校验失败 |
| 34 | VMX_MSR_LOAD_FAIL | MSR 加载失败 |
| 41 | VMX_MACHINE_CHECK | VM-Entry 时机器检查 |

---

## 11. EPT 与 VPID

### 11.1 EPT 页表结构

EPT 使用 4 级页表结构（与 x86-64 长模式类似）:

| 级别 | 表名 | 条目数 | 每项大小 | 覆盖范围 |
|------|------|--------|----------|----------|
| 4 | **PML4** (Page Map Level 4) | 512 | 64-bit | 512 GB/项 |
| 3 | **PDPT** (Page Directory Pointer Table) | 512 | 64-bit | 1 GB/项（大页）或指向 PD |
| 2 | **PD** (Page Directory) | 512 | 64-bit | 2 MB/项（大页）或指向 PT |
| 1 | **PT** (Page Table) | 512 | 64-bit | 4 KB/项 |

**GPA 位拆分**（4KB 页，4 级页表）:

| GPA 位 | 字段 |
|--------|------|
| 63:48 | 忽略 |
| 47:39 | PML4 索引 (bit 47:39) |
| 38:30 | PDPT 索引 (bit 38:30) |
| 29:21 | PD 索引 (bit 29:21) |
| 20:12 | PT 索引 (bit 20:12) |
| 11:0 | 页内偏移 |

### 11.2 EPT 页表项格式

**通用 EPT 项格式**:

| 位 | 字段 | 描述 |
|----|------|------|
| 0 | **Read** | 读权限 |
| 1 | **Write** | 写权限 |
| 2 | **Execute** | 执行权限（mode-based exec=0 时）；管理员模式执行（mode-based exec=1 时） |
| 3 | **Leaf** | 大页标志（PDPT/PD 中使用） |
| 4 | **Dirty** | 脏位（需 EPTP[6]=1 且硬件支持） |
| 5 | **Ignore PAT** | 忽略 PAT（仅叶节点） |
| 5:3 | **Memory Type** | 内存类型（叶节点，非叶节点保留） |
| 6 | **Suppress #VE** | 抑制虚拟化异常 |
| 7 | **Page Size** | 1=大页（2MB/1GB），0=非叶节点 |
| 8 | 保留 | |
| 9 | 保留 | |
| 10 | **User Execute** | 用户模式执行权限（mode-based exec=1 时） |
| 11 | 保留 | |
| N-1:12 | **Physical Address** | 物理地址（N=MAXPHYADDR） |
| 63:N | 保留 | 必须为 0 |

**注意**: 当 bit 2:0 全为 0 时，该 EPT 项为**不存在**。

**内存类型编码**（叶节点 bits 5:3）:

| 值 | 类型 |
|----|------|
| 0 | UC (Uncacheable) |
| 1 | WC (Write Combining) |
| 4 | WT (Write Through) |
| 5 | WP (Write Protected) |
| 6 | WB (Write Back) |
| 2,3,7 | 保留 |

### 11.3 EPT Violation Exit Qualification

64 位字段，EPT 违规时写入。

| 位 | 字段 | 描述 |
|----|------|------|
| 0 | **Data read** | 数据读取访问 |
| 1 | **Data write** | 数据写入访问 |
| 2 | **Instruction fetch** | 指令获取 |
| 3 | **Readable** | EPT 项 Read 权限（所有级 AND） |
| 4 | **Writable** | EPT 项 Write 权限（所有级 AND） |
| 5 | **Executable** | EPT 项 Execute 权限（mode-based=0；或 mode-based=1 时管理员执行） |
| 6 | **User executable** | EPT 项用户执行权限（mode-based=1 时） |
| 7 | **GLA valid** | Guest Linear Address 有效 |
| 8 | **GLA translated** | GPA 为线性地址的翻译结果（vs. 页表项） |
| 9 | **User/supervisor** | 0=管理员，1=用户（bit 7=1 且 bit 8=1 时有效） |
| 10 | **Read-only/RW** | 0=只读页，1=读写页 |
| 11 | **XD/NX** | 0=可执行，1=禁止执行 |
| 12 | **NMI unblocking** | 由于 IRET NMI 取消阻塞 |
| 13 | **Shadow-stack access** | 影子栈访问 |
| 14 | **Supervisor shadow-stack** | 管理员影子栈控制 |
| 15 | **Guest-paging verification** | Guest 分页验证 |
| 16 | **Asynchronous access** | 异步访问（PT/PEBS） |
| 63:17 | 保留 | |

### 11.4 EPT 配置错误条件

以下情况产生 EPT Misconfiguration：

1. **权限不一致**: bit 0=0 (Read) 且 bit 1=1 (Write)
2. **Execute-only 不支持**: 不支持时 bit 0=0 且 bit 2=1
3. **Mode-based execute 冲突**: user-execute (bit 10)=1 而 read (bit 0)=0
4. **保留位被设置**: 超出物理地址宽度的位被设置
5. **保留内存类型**: bits 5:3 为 2, 3, 或 7

### 11.5 EPTP (Extended Page Table Pointer) 格式

64 位 VMCS 字段 `201AH`:

| 位 | 字段 | 描述 |
|----|------|------|
| 2:0 | **EPT memory type** | EPT 页表内存类型（0=UC, 6=WB） |
| 5:3 | **Page-walk length** | 页表级数 - 1（4 级 = 3） |
| 6 | **Enable Acecss/Dirty** | 启用访问/脏位 |
| 11:7 | 保留 | 必须为 0 |
| N-1:12 | **PML4 address** | PML4 表物理地址（N=MAXPHYADDR） |
| 63:N | 保留 | 必须为 0 |

### 11.6 INVEPT 指令

| 属性 | 描述 |
|------|------|
| 操作码 | `66 0F 38 80 /r` |
| 格式 | `INVEPT r64, m128` |
| 类型寄存器 | Bits 63:32 保留=0 |
| 描述符 | 128 位内存：EP4TA (bits 63:0) |

**INVEPT 类型**:

| 类型 | 名称 | 行为 |
|------|------|------|
| 1 | **Single-context** | 使指定 EP4TA 的所有映射（所有 VPID/PCID） |
| 2 | **All-context** | 使所有 EP4TA 的所有映射 |

### 11.7 VPID

VPID 为每个虚拟 CPU 的 TLB 条目加上标签，避免 VM-Entry/VM-Exit 时 TLB 刷新。

**INVVPID 指令**:

| 属性 | 描述 |
|------|------|
| 操作码 | `66 0F 38 81 /r` |
| 格式 | `INVVPID r64, m128` |

**INVVPID 类型**:

| 类型 | 名称 | 行为 |
|------|------|------|
| 0 | **Individual-address** | 使特定 VPID+线性地址的映射 |
| 1 | **Single-context** | 使特定 VPID 的所有映射 |
| 2 | **All-context** | 使所有非零 VPID 的所有映射 |
| 3 | **Single-context retaining globals** | 使特定 VPID 的所有映射（保留全局页） |

---

## 12. VMX 指令参考

### 12.1 VMCALL

| 属性 | 描述 |
|------|------|
| 操作码 | `0F 01 C1` |
| 描述 | 调用 VMM（从非根操作或根操作） |
| 根操作 | 触发 VM-Exit（退出原因 18），如果在 SMM Dual-Monitor 处理中则不同 |
| 非根操作 | 始终触发 VM-Exit（原因 18） |
| 标志 | 不影响 EFLAGS |
| #UD 条件 | 不在 VMX 操作中 |
| #GP(0) | 在 VMX 根操作中且 CR0.PE=0 |

### 12.2 VMCLEAR

| 属性 | 描述 |
|------|------|
| 操作码 | `66 0F C7 /6` |
| 格式 | `VMCLEAR m64` |
| 描述 | 将 VMCS 从处理器缓存写回内存，清除 active/launched 状态 |

### 12.3 VMFUNC

| 属性 | 描述 |
|------|------|
| 操作码 | `0F 01 D4` |
| 格式 | `VMFUNC`（无操作数，EAX=函数号） |
| 描述 | 非根操作中调用 VMM 预定义的函数 |
| 支持的函数 | 0=EPTP 切换 |
| VM-Exit | 未定义函数或函数失败时触发 |

### 12.4 VMLAUNCH / VMRESUME

| 属性 | VMLAUNCH | VMRESUME |
|------|----------|----------|
| 操作码 | `0F 01 C2` | `0F 01 C3` |
| 描述 | 首次启动 VM | 恢复已启动的 VM |
| 校验 | 完整 Guest/Host 状态校验 | 仅部分校验 |
| VMCS 状态 | 必须 Clean | 必须 Launched |

### 12.5 VMPTRLD

| 属性 | 描述 |
|------|------|
| 操作码 | `0F C7 /6`（无 66 前缀） |
| 格式 | `VMPTRLD m64` |
| 描述 | 加载 VMCS 指针为 Current VMCS |

### 12.6 VMREAD / VMWRITE

| 属性 | VMREAD | VMWRITE |
|------|--------|---------|
| 操作码 | `0F 78 /r` | `0F 79 /r` |
| 格式 | `VMREAD r/m64, r64` | `VMWRITE r64, r/m64` |
| 描述 | 读取 VMCS 字段 | 写入 VMCS 字段 |

### 12.7 VMXOFF

| 属性 | 描述 |
|------|------|
| 操作码 | `0F 01 C4` |
| 描述 | 退出 VMX 根操作 |

### 12.8 VMXON

| 属性 | 描述 |
|------|------|
| 操作码 | `0F C7 /6`（无 66 前缀） |
| 格式 | `VMXON m64` |
| 描述 | 进入 VMX 根操作 |

**VMXON 前必须满足**:
1. CR4.VMXE=1
2. CR0 符合 IA32_VMX_CR0_FIXED0/1
3. CR4 符合 IA32_VMX_CR4_FIXED0/1
4. VMXON 区域已初始化（Revision ID 已写入）

---

## 13. VM-Entry 校验

VM-Entry 时处理器执行一系列校验。失败时将 VM-instruction error 字段置位，产生 VM-Entry 失败（Exit Reason bit 30=1）。

### 13.1 控制字段校验

- Pin-Based/Primary/Secondary 控制字段必须符合对应 MSR 报告的值
- 若位被保留或不允许设置则失败

### 13.2 Guest 状态校验（关键项）

**控制寄存器和 MSR**:
- CR0.PE=0 时 CR0.PG 必须为 0
- CR0.PG=1 时 CR0.PE 必须为 1
- IA-32e mode guest=1 时: CR0.PG=1, CR4.PAE=1
- IA-32e mode guest=0 时: CR4.PCIDE=0
- EFER.LMA = IA-32e mode guest（如果 Load IA32_EFER=1）
- EFER.LME = EFER.LMA 当 CR0.PG=1

**段寄存器校验**:
- CS: S=1, Type=8/10/11（代码段），Present=1
- SS: S=1（IA-32e 模式），DPL 与 CS.DPL 匹配
- DS/ES/FS/GS: 类型有效
- TR: Type=3（16 位）或 11（32 位，64 位）
- 64 位模式: CS.L=1, CS.D/B=0, TR.Type=11

**RIP/RFLAGS**:
- RIP 必须规范地址
- RFLAGS.VM=0 当 IA-32e mode guest=1 或 CR0.PE=0

### 13.3 Host 状态校验

- Host CS.S=1, Type=8/10/11, Present=1
- Host SS.S=1, DPL=0
- Host TR: Present=1, Type=11（64 位）或 3（32 位）
- Host RIP 必须规范
- Host CR0/CR4 符合 Fixed0/Fixed1 MSR

---

## 14. 事件注入

### 14.1 VM-Entry 事件注入格式

通过 `4016H` (VM-entry interruption-information field) 进行：

| 位 | 字段 | 描述 |
|----|------|------|
| 7:0 | Vector | 向量号 |
| 10:8 | Type | 类型码 |
| 11 | Error Code | 1=推送错误码 |
| 31 | Valid | 1=注入事件 |

### 14.2 不同类型注入要求

| 类型 | 值 | Vector 范围 | 说明 |
|------|----|-------------|------|
| External Interrupt | 0 | 0-255 | 硬件中断 |
| NMI | 2 | 2 (固定) | NMI |
| Hardware Exception | 3 | 0-31 | #DE 等 |
| Software Interrupt | 4 | 0-255 | INT n |
| Privileged Software Ex | 5 | 0-255 | INT1/ICEBP |
| Software Exception | 6 | 0-255 | INT3/INTO |

### 14.3 错误码注入

当 Interruption-Info bit 11=1 时，从 `4018H` (VM-entry exception error code) 读取错误码注入 Guest 堆栈。

### 14.4 指令长度要求

对于软件中断/异常注入（类型 4,5,6），必须在 `401AH` (VM-entry instruction length) 设置指令长度（1-15）。

### 14.5 事件重新注入决策

VM-Exit 发生时，需检查 IDT-Vectoring Information 是否有效。若有效则在处理 VM-Exit 后需重新注入原始事件。

**重新注入流程**:
1. 检查 IDT-Vectoring info bit 31 (Valid)
2. 若有效，将 IDT-Vectoring info 内容复制到 Entry Interruption Info
3. 将 Entry Interruption Info 写入 VMCS
4. 执行 VMRESUME

---

## 15. VMCS 编码速查表

### 按编码排序的完整字段表

| 编码 | 宽度 | 类型 | 字段名 |
|------|------|------|--------|
| `0000H` | 16 | Control | VPID |
| `0002H` | 16 | Control | Posted-interrupt notification vector |
| `0004H` | 16 | Control | EPTP index |
| `0800H` | 16 | Guest | Guest ES selector |
| `0802H` | 16 | Guest | Guest CS selector |
| `0804H` | 16 | Guest | Guest SS selector |
| `0806H` | 16 | Guest | Guest DS selector |
| `0808H` | 16 | Guest | Guest FS selector |
| `080AH` | 16 | Guest | Guest GS selector |
| `080CH` | 16 | Guest | Guest LDTR selector |
| `080EH` | 16 | Guest | Guest TR selector |
| `0810H` | 16 | Guest | Guest interrupt status |
| `0C00H` | 16 | Host | Host ES selector |
| `0C02H` | 16 | Host | Host CS selector |
| `0C04H` | 16 | Host | Host SS selector |
| `0C06H` | 16 | Host | Host DS selector |
| `0C08H` | 16 | Host | Host FS selector |
| `0C0AH` | 16 | Host | Host GS selector |
| `0C0CH` | 16 | Host | Host TR selector |
| `2000H/2001H` | 64 | Control | I/O bitmap A address |
| `2002H/2003H` | 64 | Control | I/O bitmap B address |
| `2004H/2005H` | 64 | Control | MSR bitmaps address |
| `2006H/2007H` | 64 | Control | VM-exit MSR-store address |
| `2008H/2009H` | 64 | Control | VM-exit MSR-load address |
| `200AH/200BH` | 64 | Control | VM-entry MSR-load address |
| `200CH/200DH` | 64 | Control | Executive-VMCS pointer |
| `2010H/2011H` | 64 | Control | TSC offset |
| `2012H/2013H` | 64 | Control | Virtual-APIC address |
| `2014H/2015H` | 64 | Control | APIC-access address |
| `2016H/2017H` | 64 | Control | Posted-interrupt descriptor addr |
| `2018H/2019H` | 64 | Control | VM-function controls |
| `201AH/201BH` | 64 | Control | EPT pointer (EPTP) |
| `201CH-2023H` | 64 | Control | EOI-exit bitmaps 0-3 |
| `2024H/2025H` | 64 | Control | EPTP-list address |
| `2026H/2027H` | 64 | Control | VMREAD-bitmap address |
| `2028H/2029H` | 64 | Control | VMWRITE-bitmap address |
| `202AH/202BH` | 64 | Control | Virtualization-exception info addr |
| `202CH/202DH` | 64 | Control | XSS-exiting bitmap |
| `2400H/2401H` | 64 | Read-Only | Guest-physical address |
| `2800H/2801H` | 64 | Guest | VMCS link pointer |
| `2802H/2803H` | 64 | Guest | Guest IA32_DEBUGCTL |
| `2804H/2805H` | 64 | Guest | Guest IA32_PAT |
| `2806H/2807H` | 64 | Guest | Guest IA32_EFER |
| `2808H/2809H` | 64 | Guest | Guest IA32_PERF_GLOBAL_CTRL |
| `280AH/280BH` | 64 | Guest | Guest PDPTE0 |
| `280CH/280DH` | 64 | Guest | Guest PDPTE1 |
| `280EH/280FH` | 64 | Guest | Guest PDPTE2 |
| `2810H/2811H` | 64 | Guest | Guest PDPTE3 |
| `2C00H/2C01H` | 64 | Host | Host IA32_PAT |
| `2C02H/2C03H` | 64 | Host | Host IA32_EFER |
| `2C04H/2C05H` | 64 | Host | Host IA32_PERF_GLOBAL_CTRL |
| `4000H` | 32 | Control | Pin-based VM-execution controls |
| `4002H` | 32 | Control | Primary proc-based controls |
| `4004H` | 32 | Control | Exception bitmap |
| `4006H` | 32 | Control | Page-fault error-code mask |
| `4008H` | 32 | Control | Page-fault error-code match |
| `400AH` | 32 | Control | CR3-target count |
| `400CH` | 32 | Control | VM-exit controls |
| `400EH` | 32 | Control | VM-exit MSR-store count |
| `4010H` | 32 | Control | VM-exit MSR-load count |
| `4012H` | 32 | Control | VM-entry controls |
| `4014H` | 32 | Control | VM-entry MSR-load count |
| `4016H` | 32 | Control | VM-entry interruption-information |
| `4018H` | 32 | Control | VM-entry exception error code |
| `401AH` | 32 | Control | VM-entry instruction length |
| `401CH` | 32 | Control | TPR threshold |
| `401EH` | 32 | Control | Secondary proc-based controls |
| `4020H` | 32 | Control | PLE_Gap |
| `4022H` | 32 | Control | PLE_Window |
| `4400H` | 32 | Read-Only | VM-instruction error |
| `4402H` | 32 | Read-Only | Exit reason |
| `4404H` | 32 | Read-Only | VM-exit interruption information |
| `4406H` | 32 | Read-Only | VM-exit interruption error code |
| `4408H` | 32 | Read-Only | IDT-vectoring information |
| `440AH` | 32 | Read-Only | IDT-vectoring error code |
| `440CH` | 32 | Read-Only | VM-exit instruction length |
| `440EH` | 32 | Read-Only | VM-exit instruction information |
| `4800H` | 32 | Guest | Guest ES limit |
| `4802H` | 32 | Guest | Guest CS limit |
| `4804H` | 32 | Guest | Guest SS limit |
| `4806H` | 32 | Guest | Guest DS limit |
| `4808H` | 32 | Guest | Guest FS limit |
| `480AH` | 32 | Guest | Guest GS limit |
| `480CH` | 32 | Guest | Guest LDTR limit |
| `480EH` | 32 | Guest | Guest TR limit |
| `4810H` | 32 | Guest | Guest GDTR limit |
| `4812H` | 32 | Guest | Guest IDTR limit |
| `4814H` | 32 | Guest | Guest ES access rights |
| `4816H` | 32 | Guest | Guest CS access rights |
| `4818H` | 32 | Guest | Guest SS access rights |
| `481AH` | 32 | Guest | Guest DS access rights |
| `481CH` | 32 | Guest | Guest FS access rights |
| `481EH` | 32 | Guest | Guest GS access rights |
| `4820H` | 32 | Guest | Guest LDTR access rights |
| `4822H` | 32 | Guest | Guest TR access rights |
| `4824H` | 32 | Guest | Guest interruptibility state |
| `4826H` | 32 | Guest | Guest activity state |
| `4828H` | 32 | Guest | Guest SMBASE |
| `482AH` | 32 | Guest | Guest SYSENTER_CS |
| `482EH` | 32 | Guest | VMX-preemption timer value |
| `4C00H` | 32 | Host | Host IA32_SYSENTER_CS |
| `6000H` | Natural | Control | CR0 guest/host mask |
| `6002H` | Natural | Control | CR4 guest/host mask |
| `6004H` | Natural | Control | CR0 read shadow |
| `6006H` | Natural | Control | CR4 read shadow |
| `6008H` | Natural | Control | CR3-target value 0 |
| `600AH` | Natural | Control | CR3-target value 1 |
| `600CH` | Natural | Control | CR3-target value 2 |
| `600EH` | Natural | Control | CR3-target value 3 |
| `6400H` | Natural | Read-Only | Exit qualification |
| `6402H` | Natural | Read-Only | I/O RCX |
| `6404H` | Natural | Read-Only | I/O RSI |
| `6406H` | Natural | Read-Only | I/O RDI |
| `6408H` | Natural | Read-Only | I/O RIP |
| `640AH` | Natural | Read-Only | Guest-linear address |
| `6800H` | Natural | Guest | Guest CR0 |
| `6802H` | Natural | Guest | Guest CR3 |
| `6804H` | Natural | Guest | Guest CR4 |
| `6806H` | Natural | Guest | Guest ES base |
| `6808H` | Natural | Guest | Guest CS base |
| `680AH` | Natural | Guest | Guest SS base |
| `680CH` | Natural | Guest | Guest DS base |
| `680EH` | Natural | Guest | Guest FS base |
| `6810H` | Natural | Guest | Guest GS base |
| `6812H` | Natural | Guest | Guest LDTR base |
| `6814H` | Natural | Guest | Guest TR base |
| `6816H` | Natural | Guest | Guest GDTR base |
| `6818H` | Natural | Guest | Guest IDTR base |
| `681AH` | Natural | Guest | Guest DR7 |
| `681CH` | Natural | Guest | Guest RSP |
| `681EH` | Natural | Guest | Guest RIP |
| `6820H` | Natural | Guest | Guest RFLAGS |
| `6822H` | Natural | Guest | Guest pending debug exceptions |
| `6824H` | Natural | Guest | Guest SYSENTER_ESP |
| `6826H` | Natural | Guest | Guest SYSENTER_EIP |
| `6C00H` | Natural | Host | Host CR0 |
| `6C02H` | Natural | Host | Host CR3 |
| `6C04H` | Natural | Host | Host CR4 |
| `6C06H` | Natural | Host | Host FS base |
| `6C08H` | Natural | Host | Host GS base |
| `6C0AH` | Natural | Host | Host TR base |
| `6C0CH` | Natural | Host | Host GDTR base |
| `6C0EH` | Natural | Host | Host IDTR base |
| `6C10H` | Natural | Host | Host IA32_SYSENTER_ESP |
| `6C12H` | Natural | Host | Host IA32_SYSENTER_EIP |
| `6C14H` | Natural | Host | Host RSP |
| `6C16H` | Natural | Host | Host RIP |

---

## 16. CR0/CR4 固定位算法

### 16.1 固定位 MSR

| MSR | 地址 | 含义 |
|-----|------|------|
| IA32_VMX_CR0_FIXED0 | `0x486` | 必须为 0 的 CR0 位（1=该位必须为 0） |
| IA32_VMX_CR0_FIXED1 | `0x487` | 必须为 1 的 CR0 位（0=该位必须为 1） |
| IA32_VMX_CR4_FIXED0 | `0x488` | 必须为 0 的 CR4 位 |
| IA32_VMX_CR4_FIXED1 | `0x489` | 必须为 1 的 CR4 位 |

### 16.2 校验算法

```c
static bool is_cr_value_valid(uint64_t cr_val, uint64_t fixed0, uint64_t fixed1)
{
    // fixed0: 位=1 表示 CR 中该位必须为 0
    // fixed1: 位=0 表示 CR 中该位必须为 1
    return (cr_val & fixed0) == 0            // 所有必须为 0 的位都清 0
        && (cr_val & ~fixed1) == 0;          // 所有必须为 1 的位都置 1
}
```

**简化形式:**

```
CR0_valid = ((CR0 & FIXED0) == 0) && ((CR0 | FIXED1) == FIXED1)
CR4_valid = ((CR4 & FIXED0) == 0) && ((CR4 | FIXED1) == FIXED1)
```

### 16.3 典型值

| MSR | 典型值（64 位模式） |
|-----|---------------------|
| IA32_VMX_CR0_FIXED0 | `0x80000021`（NE=1, PE=1, PG=1） |
| IA32_VMX_CR0_FIXED1 | `0xFFFFFFFF`（所有位均可为 1） |
| IA32_VMX_CR4_FIXED0 | `0x00002000`（VMXE=1） |
| IA32_VMX_CR4_FIXED1 | `0x00000FFF`（低 12 位可为 0 或 1） |

### 16.4 附加校验

除了 Fixed0/Fixed1 校验外，还有以下 CR 校验：
- CR0.PG=1 时 CR0.PE 必须为 1
- CR4.PCIDE=1 时 CR0.PG=1 且 CR4.PAE=1 且 IA32_EFER.LMA=1

---

## 17. VMX-Preemption Timer

### 17.1 启用

通过 Pin-Based VM-Execution 控制字段 bit 6 (Activate VMX-preemption timer) 启用。

### 17.2 定时值

VMCS 字段 `482EH` (VMX-preemption timer value)，32 位计数值。

### 17.3 递减率

Preemption Timer 的递减率由 IA32_VMX_MISC[4:0] 决定。

```
递减间隔 = 2^X TSC 增量
其中 X = IA32_VMX_MISC[4:0]
```

**示例**:
- X=5: 每 32 TSC 增量递减 1
- X=0: 每 1 TSC 增量递减 1

### 17.4 定时计算

```
定时器周期(TSC cycles) = timer_value × 2^X
定时器周期(秒) = (timer_value × 2^X) / TSC频率(Hz)
```

### 17.5 退出行为

- Preemption Timer 到零时触发 VM-Exit（退出原因 52）
- 如果启用了 `Save VMX-preemption timer value`（VM-Exit 控制位 22），则在 VM-Exit 时保存剩余计数值

---

## 18. Posted-Interrupt 处理

### 18.1 Posted-Interrupt Descriptor (PID) 格式

64 字节结构，缓存行对齐。

| 偏移 | 大小 | 字段 | 描述 |
|------|------|------|------|
| 0-31 | 32 字节 | **PIR** (Posted Interrupt Requests) | 256 位位图，每位对应一个中断向量 |
| 32-33 | 2 字节 | **ON** / **SN** | 控制位 |
| 34 | 1 字节 | **NV** (Notification Vector) | 通知向量 |
| 35 | 1 字节 | 保留 | |
| 36-39 | 4 字节 | **NDST** (Notification Destination) | 目标 CPU 的 APIC ID |
| 40-63 | 24 字节 | 保留 | 填充到 64 字节 |

**控制位 (偏移 32)**:

| 全局位 | 名称 | 描述 |
|--------|------|------|
| 256 | **ON** (Outstanding Notification) | 1=有待处理的 Posted Interrupt |
| 257 | **SN** (Suppress Notification) | 1=抑制通知 |
| 272-279 | **NV** (Notification Vector) | 通知向量号 |

### 18.2 启用条件

需要在 Pin-Based 控制中启用：
- Process posted interrupts (bit 7) = 1
- External-interrupt exiting (bit 0) = 1

### 18.3 处理流程

```
1. 外部设备通过 IOMMU (VT-d) 发送 Posted Interrupt
2. 硬件将向量写到 PID.PIR 对应位
3. 如果 SN=0，硬件发送 NV 向量的中断到 NDST CPU
4. CPU 收到中断后 VM-Exit（外部中断退出）
5. VMM 处理 PIR 中的待处理中断
6. VMM 清除 PID.ON，若 PIR 非空则不处理直接 VMRESUME
```

### 18.4 KVM 处理模式

KVM 利用 SN 位来优化：
- vCPU 运行时: SN=0，允许硬件直接通知
- vCPU 被抢占: SN=1，抑制通知
- vCPU 恢复运行前: 清除 SN，检查 PIR，必要时置 ON

---

## 26. PML (Page Modification Logging)

### 26.1 概念

PML 是 EPT 的一个扩展特性（Secondary 控制 bit 17），用于跟踪 Guest 物理页面的修改。

### 26.2 启用条件

- Secondary 控制 bit 17 (Enable PML) = 1
- Enable EPT (bit 1) = 1
- EPT accessed/dirty flags 必须支持（IA32_VMX_EPT_VPID_CAP[14]=1，EPTP[6]=1）

### 26.3 PML 工作原理

1. 当一页首次被写入时（EPT dirty bit 从 0→1），处理器将页面的 GPA 记录到 PML buffer
2. PML buffer 是一个物理上连续的页面数组
3. 每个 PML buffer 条目为 64 位，包含一个 GPA
4. 当 PML buffer 写满时，触发 PML Full VM-Exit（退出原因 62）

### 26.4 PML 相关地址

PML 地址通过 PML Address MSR（`IA32_PML_INDEX` / `IA32_PML_ADDRESS`）配置：
- IA32_PML_ADDRESS（MSR 0x80E）: PML buffer 的物理基地址
- IA32_PML_INDEX（MSR 0x80F）: 当前写入索引（从 511 递减到 0）

---

## 27. EPT 子页写权限 (SPP)

### 27.1 概念

SPP（Secondary 控制 bit 23）允许以 128 字节粒度控制 EPT 写权限。

### 27.2 启用条件

- Secondary 控制 bit 23 = 1
- Enable EPT (bit 1) = 1
- IA32_VMX_EPT_VPID_CAP[24] = 1（硬件支持）

### 27.3 SPP 页表结构

SPP 使用独立的 2 级页表：

| 级别 | 描述 | 条目数 |
|------|------|--------|
| Level 1 | SPP Table | 512 (4KB 页) |
| Level 2 | Sub-Page Permissions | 每个 512 位（每位控制 128 字节子页） |

### 27.4 SPP 条目格式

每项 64 位（128 字节子页权限）：

- 每个子页由 4 位控制：
  - Bit 0: 写权限（0=禁止写，1=允许写）
  - Bits 1-3: 保留

当 SPP 未命中或配置错误时触发 SPP Event VM-Exit（退出原因 65）。

---

## 28. VMFUNC 与 EPTP 切换

### 28.1 VMFUNC 概述

VMFUNC（Secondary 控制 bit 13）允许 Guest 在非根操作中调用预定义的 VM 函数。

### 28.2 VMFUNC 调用约定

```asm
mov     eax, 0          ; 函数号 (0 = EPTP switching)
vmfunc                  ; 0F 01 D4
```

- EAX = 函数号
- 如果函数成功：继续执行下一条指令
- 如果函数失败：触发 VM-Exit（退出原因 59）

### 28.3 函数 0: EPTP 切换

允许 Guest 在不触发 VM-Exit 的情况下切换到不同的 EPT 视图。

**参数**:
- EAX = 0
- ECX = EPTP 列表索引

**条件**:
- VM-function controls = 1（编码 `2018H`，bit 0=1）
- EPTP-list address（编码 `2024H`）指向 EpTP 列表
- 列表项必须有效（EPTP 格式正确）

### 28.4 VM-Function Controls (编码 `2018H`)

64 位字段：

| 位 | 名称 | 描述 |
|----|------|------|
| 0 | **EPTP switching** | 启用 EPTP 切换函数 |
| 1-63 | 保留 | 必须为 0 |

---

## 29. 二级地址转换 (EPT) 实现细节

### 29.1 EPT 兼容性与常规页表的差异

| 特性 | 常规 x64 页表 | EPT 页表 |
|------|-------------|----------|
| 权限位 | R/W, NX, User/Supervisor | Read, Write, Execute（独立） |
| Present 检查 | Bit 0 (Present) | Bits 2:0 任意一个=1 |
| 访问/脏位 | 始终支持 | 需 IA32_VMX_EPT_VPID_CAP[6,21] 且 EPTP[6]=1 |
| 物理地址 | Guest 物理地址 | Host 物理地址 |
| 内存类型 | MTRR/PAT 确定 | 每项 bits 5:3（叶节点） |
| 每表项数 | 512 | 512 |
| 地址宽度 | 最大 MAXPHYADDR | 最大 MAXPHYADDR |

### 29.2 EPT 遍历算法

```c
// 伪代码: EPT 地址转换
uint64_t ept_walk(uint64_t gpa, uint64_t eptp) {
    uint64_t pml4_base = eptp & ~0xFFF;           // EPTP 中的 PML4 地址
    int pml4_idx = (gpa >> 39) & 0x1FF;
    int pdpt_idx = (gpa >> 30) & 0x1FF;
    int pd_idx   = (gpa >> 21) & 0x1FF;
    int pt_idx   = (gpa >> 12) & 0x1FF;
    int offset   = (gpa >> 0)  & 0xFFF;
    
    // PML4
    ept_entry_t pml4e = read_phys(pml4_base + pml4_idx * 8);
    if (!is_ept_present(pml4e)) return EPT_VIOLATION;
    if (is_ept_misconfigured(pml4e)) return EPT_MISCONFIG;
    
    // PDPT
    uint64_t pdpt_base = pml4e & ~0xFFF;
    ept_entry_t pdpte = read_phys(pdpt_base + pdpt_idx * 8);
    if (!is_ept_present(pdpte)) return EPT_VIOLATION;
    
    // 检查 1GB 大页
    if (pdpte & PAGE_SIZE_BIT) {
        if (is_ept_misconfigured(pdpte)) return EPT_MISCONFIG;
        return (pdpte & ADDR_MASK) | (gpa & 0x3FFFFFFF);  // GPA bits 29:0
    }
    
    // PD
    uint64_t pd_base = pdpte & ~0xFFF;
    ept_entry_t pde = read_phys(pd_base + pd_idx * 8);
    if (!is_ept_present(pde)) return EPT_VIOLATION;
    
    // 检查 2MB 大页
    if (pde & PAGE_SIZE_BIT) {
        if (is_ept_misconfigured(pde)) return EPT_MISCONFIG;
        return (pde & ADDR_MASK) | (gpa & 0x1FFFFF);  // GPA bits 20:0
    }
    
    // PT (4KB)
    uint64_t pt_base = pde & ~0xFFF;
    ept_entry_t pte = read_phys(pt_base + pt_idx * 8);
    if (!is_ept_present(pte)) return EPT_VIOLATION;
    if (is_ept_misconfigured(pte)) return EPT_MISCONFIG;
    
    return (pte & ADDR_MASK) | offset;
}
```

### 29.3 EPT Violation 和 Misconfiguration 区别

| 特征 | EPT Violation | EPT Misconfiguration |
|------|--------------|---------------------|
| 定义 | EPT 权限不足或项不存在 | EPT 项格式错误 |
| 退出原因 | 48 (0x30) | 49 (0x31) |
| 典型原因 | Guest 试图访问无权限的内存 | 保留位被设置、无效内存类型 |
| Exit Qualification | 详细的访问信息 | 未定义 |
| Guest-Physical Address | 有效 | 有效 |

---

## 30. Virtualization Exception (#VE)

### 30.1 概念

#VE 允许 EPT 违规直接投递到 Guest 而无需 VM-Exit。

### 30.2 启用条件

- Secondary 控制 bit 18 (EPT-violation #VE) = 1
- Enable EPT (bit 1) = 1
- IA32_VMX_EPT_VPID_CAP[25] = 1（硬件支持）

### 30.3 Virtualization-Exception Info Address

VMCS 编码 `202AH`，指向一个 4KB 的物理页面，包含：

| 偏移 | 大小 | 描述 |
|------|------|------|
| 0 | 4 字节 | #VE 信息块 - 退出原因 |
| 4 | 4 字节 | 保留 |
| 8 | 8 字节 | Exit Qualification |
| 16 | 8 字节 | Guest-Linear Address |
| 24 | 8 字节 | Guest-Physical Address |
| 32 | 1 字节 | EPT 遍历级数 |
| 33-4095 | 剩余 | 保留 |

### 30.4 Suppress #VE（bit 6）

每个 EPT 项的 bit 6 可抑制该页的 #VE 投递。当 bit 6=1 且 EPT 违规发生时，仍然触发 VM-Exit 而非 #VE。

---

## 31. 部分 VMX 指令的详细操作

### 31.1 VMCALL (0F 01 C1)

```
IF VMX_ROOT (CPL=0):
    IF "SMM dual-monitor treatment" active:
        IF "VMX in SMM" flag set:
            保存 Guest 状态到 SMM-transfer VMCS
            加载 Host 状态从 VMCS
            VMX_OFF
            SMM 重新入口
        ELSE:
            保存 Guest 状态到 SMM-transfer VMCS
            加载 Host 状态从 SMM-transfer VMCS
            VMX_OFF
            SMM 重新入口
    ELSE:
        #GP(0) 或 #UD
    
IF VMX_NON_ROOT:
    触发 VM-Exit (退出原因 18)
```

### 31.2 VMLAUNCH (0F 01 C2)

```
检查 Current-VMCS 指针有效
检查 VMCS 状态为 "Clear"
执行 VMCS 校验:
    - 控制字段校验
    - Host 状态校验
    - Guest 状态校验
    - 如果全部通过: VMCS 状态 → "Launched", VM-Entry 发生
    - 如果校验失败: VM-instruction error 置位
```

### 31.3 VMRESUME (0F 01 C3)

```
检查 Current-VMCS 指针有效
检查 VMCS 状态为 "Launched"
执行有限校验:
    - 控制字段校验 (简略)
    - Host 状态基本校验
    - 如果通过: VM-Entry 发生
    - 如果失败: VM-instruction error 置位
```

### 31.4 VMREAD (0F 78 /r)

```
IF SRC IS VMCS 编码:
    检查编码是否支持
    从 Current-VMCS 读取字段
    写入 DST
ELSE:
    VMfailInvalid
```

**VMX 非根操作中的 VMREAD（需要 VMCS Shadowing）**:
```
IF VMCS shadowing 启用 AND VMREAD-bitmap 允许此编码:
    从 Shadow VMCS 读取字段
ELSE:
    触发 VM-Exit (退出原因 23)
```

### 31.5 VMWRITE (0F 79 /r)

```
IF DST IS VMCS 编码:
    检查编码是否支持
    IF 编码是只读字段 (Type=01):
        VMfailInvalid
    从 SRC 读取值
    写入 Current-VMCS 字段
ELSE:
    VMfailInvalid
```

### 31.6 VMXON (0F C7 /6)

```
检查 CR4.VMXE = 1
检查 CR0 满足 Fixed0/Fixed1
检查 CR4 满足 Fixed0/Fixed1
检查 A20M 模式已禁用 (系统地址必须一致)
检查 VMXON 区域:
    - 4KB 对齐
    - Revision ID 匹配
    - 不在系统内存范围外
进入 VMX 根操作
```

---

## 32. VMX 非根操作中的指令行为

### 32.1 敏感指令分类

| 类别 | 指令 | 控制位 |
|------|------|--------|
| **无条件退出** | CPUID, GETSEC, INVD, VMCALL, VMXOFF, RSM | — |
| **条件退出** | HLT, MWAIT, PAUSE, MONITOR, INVLPG, RDPMC, RDTSC | Primary controls |
| **I/O 指令** | IN, OUT, INS, OUTS | Unconditional I/O 或 I/O bitmaps |
| **CR 访问** | MOV CR, CLTS, LMSW | CR guest/host mask, CR3-target |
| **DR 访问** | MOV DR | MOV-DR exiting |
| **MSR 访问** | RDMSR, WRMSR | MSR bitmaps |
| **描述符表** | LGDT, LIDT, SGDT, SIDT, LLDT, LTR, SLDT, STR | Descriptor-table exiting |
| **APIC 访问** | APIC 内存/MSR 访问 | APIC virtualization 控制 |
| **EPT 相关** | INVEPT, INVVPID | Enable EPT/Enable VPID |

### 32.2 Guest 试图执行 VMX 指令

| 指令 | 非根操作行为 |
|------|-------------|
| VMCALL | VM-Exit（退出原因 18） |
| VMLAUNCH | VM-Exit（退出原因 20） |
| VMRESUME | VM-Exit（退出原因 24） |
| VMCLEAR | VM-Exit（退出原因 19） |
| VMPTRLD | VM-Exit（退出原因 21） |
| VMPTRST | VM-Exit（退出原因 22） |
| VMREAD | VM-Exit 或 Shadow VMCS 访问（退出原因 23） |
| VMWRITE | VM-Exit 或 Shadow VMCS 访问（退出原因 25） |
| VMXOFF | VM-Exit（退出原因 26） |
| VMXON | VM-Exit（退出原因 27） |
| VMFUNC | 执行 VM 函数或 VM-Exit（退出原因 59） |

---

## 33. VM-Entry 详细校验项

### 33.1 Guest 状态校验 (SDM §26.3.1)

**CR0 校验**:
| 条件 | 要求 |
|------|------|
| CR0.PE=0 | CR0.PG=0 |
| CR0.PG=1 | CR0.PE=1 |
| IA32_EFER.LMA=1 | CR0.PE=1, CR0.PG=1, CR4.PAE=1 |
| CR0.NW=1 | CR0.CD=1 |
| CR0.CD=0 | CR0.NW=0 |

**CR4 校验**:
| 条件 | 要求 |
|------|------|
| IA-32e mode guest=1 | CR4.PAE=1 |
| IA-32e mode guest=0 | CR4.PCIDE=0 |
| CR4.PCIDE=1 | CR0.PG=1, CR4.PAE=1, IA32_EFER.LMA=1 |
| CR4.SMEP=1 | CR0.PG=1 |
| CR4.SMAP=1 | CR0.PG=1 |

**EFLAGS 校验**:
| 条件 | 要求 |
|------|------|
| IA-32e mode guest=1 | RFLAGS.VM=0 |
| CR0.PE=0 | RFLAGS.VM=0 |
| Activity state=HLT | RFLAGS.IF=1 |
| RFLAGS.VM=1 | CR0.PE=1, CPL=3, CR0.PG=1, CR4.VMXE=0, IA-32e mode=0 |

**SS 段**:
| 条件 | 要求 |
|------|------|
| Guest 在保护模式 (CR0.PE=1) | SS.S=1, SS.DPL=CPL, SS 类型必须为可写数据段 |
| Activity state=HLT | SS.DPL=0 |
| CPL=0 | SS access rights 的 Type 字段必须包含 writable |

### 33.2 Host 状态校验 (SDM §26.2)

**CS 段选择子**:
- S=1 (代码或数据段)
- Type=8, 10, 或 11 (代码段)
- Present=1
- DPL=0
- 64 位模式: CS.L=1, CS.D/B=0

**SS 段选择子**:
- S=1
- DPL=0
- IA-32e 模式: SS 可为 null (selector=0)

**TR 段**:
- Present=1
- Type=3 (16 位忙碌 TSS) 或 11 (32/64 位忙碌 TSS)
- Base 必须规范

**GDTR/IDTR base**:
- 必须规范地址

**CR0/CR4**:
- 必须满足 IA32_VMX_CR0_FIXED0/1 和 IA32_VMX_CR4_FIXED0/1
- CR0.PG=1 时 CR0.PE=1
- CR4.PAE=1（64 位模式）

**RIP 和 RSP**:
- 必须规范地址
- 64 位模式时 RIP 必须为规范地址（高 16 位是符号扩展）

---

## 34. VM-Exit 操作步骤

### 34.1 VM-Exit 过程 (SDM §27.1)

1. **处理器暂停** Guest 指令执行
2. **保存基本信息到 VMCS**:
   - Exit reason (编码 `4402H`) — 退出原因和状态
   - Exit qualification (编码 `6400H`) — 退出详细信息
   - Guest-linear address (编码 `640AH`) — 如有效
   - Guest-physical address (编码 `2400H`) — 如有效
3. **中断/异常信息**:
   - VM-exit interruption information (编码 `4404H`)
   - VM-exit interruption error code (编码 `4406H`)
   - IDT-vectoring information (编码 `4408H`)
4. **指令信息**:
   - VM-exit instruction length (编码 `440CH`)
   - VM-exit instruction information (编码 `440EH`)
5. **保存 Guest 状态到 Guest-State 区域**:
   - 段寄存器: CS, SS, DS, ES, FS, GS, LDTR, TR (选择子/基址/限长/AR)
   - 控制寄存器: CR0, CR3, CR4
   - DR7
   - 桌面寄存器: GDTR, IDTR (基址和限长)
   - RIP, RSP, RFLAGS
   - 非寄存器状态: Activity state, Interruptibility state
   - Pending debug exceptions
6. **保存 MSR 到 Guest-State 区域**（根据控制设置）:
   - IA32_DEBUGCTL
   - IA32_PAT（如果 Save IA32_PAT VM-Exit 控制=1）
   - IA32_EFER（如果 Save IA32_EFER VM-Exit 控制=1）
   - IA32_PERF_GLOBAL_CTRL（如果 Save PERF_GLOBAL_CTRL=1）
7. **加载 Host 状态从 Host-State 区域**:
   - CR0, CR3, CR4
   - CS, SS, DS, ES, FS, GS, TR 选择子
   - FS, GS, TR, GDTR, IDTR 基址
   - RIP, RSP
   - SYSENTER_CS, SYSENTER_ESP, SYSENTER_EIP
8. **加载 Host MSR**（根据控制设置）:
   - IA32_PAT, IA32_EFER, IA32_PERF_GLOBAL_CTRL
   - 执行 MSR load 列表（VM-exit MSR-load address/count）
   - 清除 IA32_BNDCFGS, IA32_RTIT_CTL, LBR_CTL（如控制位设置）
9. **更新 RFLAGS**: 清除 IF=0（禁止中断），其他位由 Host RFLAGS 加载
10. **开始执行 Host 代码**: 在 Host RIP 处开始

---

## 35. TPR Shadow 与虚拟 APIC

### 35.1 TPR Shadow 概述

TPR Shadow（Primary 控制 bit 21）允许 Guest 直接读取/写入 APIC TPR 寄存器而无需 VM-Exit。

**相关 VMCS 字段**:
| 字段 | 编码 | 描述 |
|------|------|------|
| Virtual-APIC address | `2012H` | 虚拟 APIC 页面的物理地址 |
| TPR threshold | `401CH` | TPR 阈值 |

### 35.2 TPR Shadow 工作原理

```
Guest MOV CR8/访问 APIC TPR:
    │
    ├── 如果 Guest 写 TPR 且新值 ≥ TPR threshold:
    │       → 正常写入（不触发 VM-Exit）
    │       → VTPR 更新到虚拟 APIC 页面
    │
    └── 如果 Guest 写 TPR 且新值 < TPR threshold:
            → 触发 TPR Below Threshold VM-Exit（退出原因 43）
            → VMM 可以虚拟化中断
```

### 35.3 虚拟 APIC 页面

虚拟 APIC 页面是一个 4KB 物理页面，布局与 xAPIC 内存映射寄存器相同：

| 偏移 | 寄存器 | 描述 |
|------|--------|------|
| 080H | **TPR** (Task Priority) | 任务优先级寄存器 |
| 090H | **APR** (Arbitration Priority) | 仲裁优先级 |
| 0A0H | **PPR** (Processor Priority) | 处理器优先级 |
| 0B0H | **EOI** (End of Interrupt) | EOI 寄存器 |
| 0D0H | **IRR** (In-Service Register) | 中断请求寄存器（63:0） |
| 0E0H | **ISR** (In-Service Register) | 在服务寄存器（63:0） |
| 280H | **TIMER** LVTT | 本地向量表定时器 |
| 320H | **CMCI** LVTT | CMCI 本地向量表 |
| 350H | **LINT0** LVTT | LINT0 本地向量表 |
| 360H | **LINT1** LVTT | LINT1 本地向量表 |
| 370H | **ERROR** LVTT | 错误本地向量表 |
| 380H | **ICR** (Interrupt Command) | 中断命令寄存器（低 32 位） |
| 390H | **ICR** (Interrupt Command) | 中断命令寄存器（高 32 位） |
| 3E0H | **SVR** (Spurious Vector) | 伪中断向量寄存器 |

---

## 36. EPT Accessed/Dirty Flags

### 36.1 概念

EPT Access/Dirty 标志（EPTP[6]=1）允许 VMM 跟踪页面访问和修改。

### 36.2 位定义

每个 EPT 叶节点条目包含：

| 位 | 名称 | 描述 |
|----|------|------|
| 3 | **Accessed** | 1=页面被访问过（读或写） |
| 4 | **Dirty** | 1=页面被写入过 |

### 36.3 硬件行为

- **Accessed 位**: 页面首次被读/写/执行时，处理器设置 bit 3
  - 如果 bit 3 已为 1，不产生额外操作
  - 如果 bit 3 为 0，处理器设置 bit 3（如果可写）
  
- **Dirty 位**: 页面首次被写入时，处理器设置 bit 4
  - 需 Write 权限已授予
  - 如果 bit 4 已为 1，不产生额外操作

### 36.4 与 PML 的关系

当 Dirty 位从 0→1 时，如果 PML 启用，GPA 被记录到 PML buffer。

---

## 37. Mode-Based Execute Control for EPT

### 37.1 概念

MBEC（Secondary 控制 bit 22）将 EPT 执行权限分为管理员模式执行和用户模式执行。

### 37.2 启用条件

- Secondary 控制 bit 22 = 1
- Enable EPT (bit 1) = 1
- IA32_VMX_EPT_VPID_CAP[8] = 1（硬件支持）

### 37.3 EPT 权限位重新定义

当 MBEC 启用时：

| EPT bit | 权限 | 描述 |
|---------|------|------|
| 0 | Read | 所有模式的读取权限 |
| 1 | Write | 所有模式的写入权限 |
| 2 | **Supervisor Execute** | 管理员模式（CPL<3）执行权限 |
| 10 | **User Execute** | 用户模式（CPL=3）执行权限 |

### 37.4 Present 检查

当 MBEC 启用时，EPT 项存在条件为：
```
Present = Read(bit 0) | Write(bit 1) | Supervisor_Exec(bit 2) | User_Exec(bit 10)
```

所有四个位都为 0 才表示不存在。

---

## 38. EPTP Switching 与多 EPT 视图

### 38.1 概念

EPTP Switching 允许 Guest 在不触发 VM-Exit 的情况下切换到不同的 EPT 根，实现内存隔离（如 TDX 架构）。

### 38.2 配置

1. Secondary 控制 bit 13 (Enable VM functions) = 1
2. VM-function controls bit 0 (EPTP switching) = 1
3. EPTP-list address（编码 `2024H`）指向 EPTP 列表

### 38.3 EPTP 列表格式

EPTP 列表包含 512 个 64 位 EPTP 条目：

| 偏移 | 大小 | 描述 |
|------|------|------|
| 0 | 8 字节 | EPTP 值 0 |
| 8 | 8 字节 | EPTP 值 1 |
| ... | ... | ... |
| 0xFF8 | 8 字节 | EPTP 值 511 |

### 38.4 VMFUNC 调用

```asm
; Guest 代码切换到不同的 EPT 视图
mov     eax, 0          ; 函数 0 = EPTP switching
mov     ecx, 1          ; EPTP 列表索引 1
vmfunc                  ; 执行切换

; 如果成功: 继续执行，EPT 已切换到新视图
; 如果失败: VM-Exit (退出原因 59)
```

---

## 39. SMM Dual-Monitor Treatment

### 39.1 概念

VMX 支持双监视器模式（Dual-Monitor Treatment）处理 SMM，允许两个独立 VMX 监视器：Executive Monitor（处理正常 VM-Exit）和 SMM Monitor（处理 SMI）。

### 39.2 启用条件

- IA32_VMX_BASIC[49] = 1（硬件支持）
- 使用 IA32_SMM_MONITOR_CTL MSR 激活

### 39.3 SMM Transfer VMCS (STM)

SMM 使用两种特殊的 VMCS 类型：

| VMCS 类型 | 描述 |
|-----------|------|
| **Executive-VMCS pointer** (`200CH`) | Executive Monitor 的正常 VMCS |
| **SMM-transfer VMCS (STM)** | 用于 SMM 进入/退出的 VMCS |

### 39.4 SMM 进入/退出流程

```
正常操作 → SMI → SMM Transfer VMCS → SMM Monitor
                ↓
          Executive Monitor 保存状态到 Executive VMCS
                ↓
          Executive Monitor 加载 SMM Monitor 状态
                ↓
          VMRESUME 进入 SMM Monitor
                ↓
          RSM → SMM Transfer VMCS → Executive Monitor
```

### 39.5 VM-Entry 控制

| 控制位 | 结合 Dual Monitor |
|--------|------------------|
| Entry to SMM (bit 10) | VM-Entry 表示进入 SMM |
| Deactivate dual-monitor (bit 11) | VM-Entry 后退出 Dual Monitor |

---

## 40. VMCS Region Memory Type 与缓存一致性

### 40.1 内存类型要求

根据 IA32_VMX_BASIC[53:50]，VMCS 区域应使用：

| 值 | 类型 | 描述 |
|----|------|------|
| 0 | UC | 非缓存（较慢） |
| 6 | WB | Write-Back（推荐，性能最优） |

大多数现代 CPU 报告 WB（6）。

### 40.2 缓存一致性问题

在多核系统中，VMCS 访问需注意：
- 不同逻辑核访问同一 VMCS 前必须执行 VMCLEAR 确保写回
- VMCLEAR 确保 VMCS 数据在内存中是可见的
- 除非通过 VMPTRLD，否则不要假设 VMCS 数据是最新的

### 40.3 正确的跨核 VMCS 共享

```c
// CPU 0 上的操作
__vmx_vmwrite(field, value);
__vmx_vmclear(&vmcs_region);       // 写回到内存
_mm_sfence();                       // 保证顺序

// CPU 1 上的操作 (等待 CPU 0 完成后)
_mm_lfence();                       // 保证读取顺序
__vmx_vmptrld(&vmcs_region);        // 从内存加载
value = __vmx_vmread(field);        // 读取
```

---

## 附录 A: VMCS 编程注意事项

### A.1 VMCS 生命周期管理

```
分配 VMCS 区域 (4KB aligned, Revision ID)
    │
    ▼
VMCLEAR (重置为 Clear 状态)
    │
    ▼
VMPTRLD (加载为 Current VMCS)
    │
    ▼
VMWRITEs (配置所有字段)
    │
    ▼
VMLAUNCH (首次启动)
    │
    ▼
[VM-Exit → 处理 → VMRESUME] 循环
    │
    ▼
VMXOFF (退出 VMX 操作)
```

### A.2 VMWRITE/VMCLEAR 后可见性

```
CPU 0                          CPU 1
  │                              │
  ├─ VMWRITE                     │
  ├─ VMCLEAR → 写回内存          │
  ├─ (sfence)                    │
  │                              ├─ (lfence)
  │                              ├─ VMPTRLD → 从内存加载
  │                              ├─ VMREAD → 读取字段
```

### A.3 常见编程错误

| 错误 | 后果 |
|------|------|
| 忘记设置 IA-32e mode guest=1 | Guest 陷入 32 位模式，CS.L 错误 |
| Host RIP 指向无效地址 | VM-Exit 后 #GP |
| 未设置 Host CR4.PAE=1 | VM-Entry 失败 |
| 使用默认段选择子错误 | VM-Entry 段校验失败 |
| VMCS link pointer 未清 0 | VM-Entry 失败 |
| EPT 权限位设置不当 | EPT 违规模糊不清 |
| 忽略 True MSR | 不能清除 Default1 位的功能 |
| VMPTRLD 前未 VMCLEAR | 处理器行为不可预测 |

---

## 附录 B: CR Access Exit Qualification 详解

### CR 访问类型表

| Access Type 值 | 含义 | 触发条件 |
|----------------|------|----------|
| 0 | MOV to CR | 写入 CR（CR0/CR3/CR4/CR8） |
| 1 | MOV from CR | 读取 CR |
| 2 | CLTS | 清除 CR0.TS |
| 3 | LMSW | 加载 CR0 低 16 位 |

### CR Guest/Host Mask 工作原理

```c
// 如果 Guest 尝试写入 CR0:
new_guest_value = <Guest 试图写入的值>
mask = CR0_guest_host_mask
shadow = CR0_read_shadow

// 实际写入 CR0 的值:
actual = (guest_value & ~mask) | (shadow & mask)

// 如果任何 mask 位的值发生变化 → VM-Exit
if ((actual ^ CR0) & mask) != 0:
    // 条件可能触发 VM-Exit (取决于 CR 具体配置)
    ;
```

**注意**: CR3-load/CR3-store exiting 控制位覆盖 Guest/Host Mask 机制。

---

## 附录 B: 典型配置示例

### Windows x64 Guest 典型 VMCS 设置

```c
// 适用于 Windows 10/11 x64 的典型设置

// Pin-Based Controls
uint32_t pin_ctls = 
    PINBASED_EXTINT_EXITING |      // 外部中断退出
    PINBASED_NMI_EXITING;          // NMI 退出

// Primary Processor-Based Controls  
uint32_t primary_ctls =
    PROCBASED_HLT_EXITING |        // HLT 退出
    PROCBASED_MWAIT_EXITING |      // MWAIT 退出
    PROCBASED_RDPMC_EXITING |      // RDPMC 退出
    PROCBASED_RDTSC_EXITING |      // RDTSC 退出
    PROCBASED_CR3_LOAD_EXITING |   // CR3 加载退出
    PROCBASED_CR3_STORE_EXITING |  // CR3 保存退出
    PROCBASED_CR8_LOAD_EXITING |   // CR8 加载退出
    PROCBASED_CR8_STORE_EXITING |  // CR8 保存退出
    PROCBASED_TPR_SHADOW |         // TPR 影子
    PROCBASED_IO_BITMAPS |         // I/O 位图
    PROCBASED_MSR_BITMAPS |        // MSR 位图
    PROCBASED_MONITOR_EXITING |    // MONITOR 退出
    PROCBASED_PAUSE_EXITING |      // PAUSE 退出
    PROCBASED_SECONDARY_CTLS;      // 启用 Secondary 控制

// Secondary Processor-Based Controls
uint32_t secondary_ctls =
    SECPROCBASED_ENABLE_EPT |           // EPT
    SECPROCBASED_ENABLE_VPID |          // VPID
    SECPROCBASED_UNRESTRICTED_GUEST |   // 无限制 Guest
    SECPROCBASED_DESCRIPTOR_TABLE_EXITING; // 描述符表退出

// VM-Exit Controls
uint32_t exit_ctls =
    VMEXIT_HOST_64BIT_MODE |        // Host 64 位模式
    VMEXIT_ACK_INTERRUPT;           // ACK 中断

// VM-Entry Controls
uint32_t entry_ctls =
    VMENTRY_GUEST_64BIT_MODE;       // Guest 64 位模式

// Exception Bitmap (仅捕获关键异常)
uint32_t exc_bitmap =
    (1U << 1) |   // #DB - 调试异常
    (1U << 3) |   // #BP - 断点
    (1U << 6) |   // #UD - 非法操作码
    (1U << 8);    // #DF - 双重错误
```

---

## 附录 C: 术语对照表

| 英文 | 中文 | 说明 |
|------|------|------|
| VMX Root Operation | VMX 根操作 | VMM 运行模式 |
| VMX Non-Root Operation | VMX 非根操作 | Guest 运行模式 |
| VM-Entry | VM 进入 | 从根到非根操作 |
| VM-Exit | VM 退出 | 从非根到根操作 |
| VMCS | 虚拟机控制结构 | 每 vCPU 的状态/控制结构 |
| EPT | 扩展页表 | 二级地址转换 |
| VPID | 虚拟处理器 ID | TLB 标记 |
| Posted Interrupt | 发布中断 | 直接向 Guest 投递中断 |
| Preemption Timer | 抢占定时器 | VMX 计时器 |
| MTF | 监控陷阱标志 | 单步跟踪 |
| PID | Posted Interrupt Descriptor | 发布中断描述符 |

---

## 19. VM-Exit 详细信息 — Exit Qualification 编码

### 19.1 CR Access Exit Qualification

退出原因 28 (CR Access) 时的 Exit Qualification (编码 `6400H`) 格式:

| 位 | 字段 | 描述 |
|----|------|------|
| 3:0 | **CR Number** | 访问的控制寄存器号（0=CR0, 3=CR3, 4=CR4, 8=CR8） |
| 5:4 | **Access Type** | 0=MOV to CR (写), 1=MOV from CR (读), 2=CLTS, 3=LMSW |
| 6 | **LMSW Operand Type** | 0=寄存器操作数, 1=内存操作数（仅 LMSW） |
| 7 | 保留 | 0 |
| 11:8 | **GPR** | MOV CR 使用的通用寄存器编号（0=RAX, ..., 15=R15） |
| 15:12 | 保留 | 0 |
| 31:16 | **LMSW Source Data** | LMSW 源操作数数据 |
| 63:32 | 保留（64 位处理器） | |

**Access Type 详解**:

| 值 | 类型 | 说明 |
|----|------|------|
| 0 | MOV to CR | MOV CRx, r64 — 从 GPR 写入 CR |
| 1 | MOV from CR | MOV r64, CRx — 从 CR 读取到 GPR |
| 2 | CLTS | 清除 CR0.TS 位 |
| 3 | LMSW | 加载机器状态字到 CR0 |

### 19.2 DR Access Exit Qualification

退出原因 29 (MOV DR) 时的 Exit Qualification 格式:

| 位 | 字段 | 描述 |
|----|------|------|
| 2:0 | **DR Number** | 调试寄存器号（0=DR0, ..., 7=DR7） |
| 3 | 保留 | 0 |
| 4 | **Direction** | 0=MOV to DR (写), 1=MOV from DR (读) |
| 7:5 | 保留 | 0 |
| 11:8 | **GPR** | 通用寄存器编号 |

### 19.3 I/O Instruction Exit Qualification

退出原因 30 (I/O Instruction) 时的 Exit Qualification 格式:

| 位 | 字段 | 描述 |
|----|------|------|
| 3:0 | **Port number** | I/O 端口号（低 4 位） |
| 4 | 保留 | 0 |
| 5 | **String** | 1=字符串 I/O（INS/OUTS） |
| 6 | **REX** | 1=有 REX 前缀 |
| 7 | **Operand size** | 0=16 位, 1=32 位 |
| 9:8 | **Address size** | 0=16 位, 1=32 位, 2=64 位 |
| 10 | **Segment** | 段寄存器编码 |
| 11 | 保留 | 0 |
| 15:12 | **Port number (high)** | I/O 端口号（高 4 位） |
| 31:16 | **Reserved** | |
| 63:32 | **Port number (RIP relative)** | （64 位模式下） |

### 19.4 APIC Access Exit Qualification

退出原因 44 (APIC Access) 时的 Exit Qualification 格式:

| 位 | 字段 | 描述 |
|----|------|------|
| 11:0 | **Offset** | APIC-access 页内的偏移 |
| 15:12 | **Access Type** | 详见下表 |
| 63:16 | 保留 | |

**APIC Access Type**:

| 值 | 类型 | 描述 |
|----|------|------|
| 0 | Linear read | 线性地址读取 |
| 1 | Linear write | 线性地址写入 |
| 2 | Linear instruction fetch | 线性地址指令获取 |
| 3 | Linear read/write (event) | 事件交付时的读/写 |
| 10 | Physical read | 物理地址读取（EPT 转换前） |
| 11 | Physical write | 物理地址写入 |
| 15 | Physical access (event) | 事件交付时的物理访问 |

### 19.5 Task Switch Exit Qualification

退出原因 9 (Task Switch) 时的 Exit Qualification 格式:

| 位 | 字段 | 描述 |
|----|------|------|
| 15:0 | **TSS Selector** | 新任务的 TSS 段选择子 |
| 30:16 | 保留 | 0 |
| 31:31 | **Source** | 0=调用 CALL/JMP/INT, 1=IRET（NT 标志） |
| 63:32 | 保留 | |

---

## 20. VMX-Abort 码

当 VMX 发生不可恢复的内部错误时，处理器写入 VMX-Abort 码到 VMCS/VMXON 区域的偏移 4 处。

| 码 | 描述 |
|----|------|
| 0 | VMX-Abort 未发生 |
| 1 | VM-Entry 时检测到无效的 Host 状态 |
| 2 | VM-Entry 时检测到无效的 Guest 状态 |
| 3 | VM-Entry 时 MSR 加载失败 |
| 4 | VM-Exit 时无法保存 Guest 状态 |
| 5 | VM-Exit 时无法加载 Host 状态 |
| 6 | VM-Exit 时 MSR 存储失败 |
| 7 | VM-Exit 时 MSR 加载失败 |
| 8 | VM-Entry 时机器检查错误 |
| 9 | VM-Entry 时不可恢复的机器检查 |

---

## 21. VM-Instruction Error 码

当 VMLAUNCH/VMRESUME/VMREAD/VMWRITE 等指令失败时，`VM-instruction error` 字段（编码 `4400H`）指示失败原因。

| 码 | 描述 |
|----|------|
| 1 | VMCALL 在 VMX 根操作中执行 |
| 2 | VMCLEAR 使用了无效的物理地址 |
| 3 | VMCLEAR 使用的 VMXON 指针无效 |
| 4 | VMLAUNCH/VMRESUME 在非根操作中执行 |
| 5 | VMRESUME 对应的 VMCS 未被启动 |
| 6 | VMRESUME 后 VMCS 启动标志被清除 |
| 7 | VMPTRLD 使用了无效的物理地址 |
| 8 | VMPTRLD 使用的 VMCS Revision ID 不匹配 |
| 9 | VMPTRLD 使用的 VMXON 指针无效 |
| 10 | VMREAD/VMWRITE 使用了无效的 VMCS 编码 |
| 11 | VMWRITE 尝试写入只读字段 |
| 12 | VMXOFF 在 Dual-Monitor 处理中 |
| 13 | VMXOFF/VMCLEAR 使用的指针是 32 位地址 |
| 14 | VMLAUNCH/VMRESUME 的 Current VMCS 指针无效 |
| 15 | VMLAUNCH 的 VMCS 已启动 |
| 16 | VMWRITE 写入不支持的 VMCS 编码 |
| 17 | VMXON 在 VMX 根操作中执行 |
| 18 | VM-entry 控制字段校验失败 |
| 19 | VM-entry MSR-load 计数过大 |
| 20 | VM-entry 中断注入字段无效 |
| 21 | VM-entry 活动状态校验失败 |
| 22 | VM-entry 事件注入被阻塞 |
| 23 | VM-entry 针对不支持的硬件异常触发 |
| 24 | VM-entry 无效的软件中断类型 |
| 25 | VM-entry 段描述符校验失败 |
| 26 | VM-entry Guest 状态 CR/DR/EFLAGS/RIP 校验失败 |
| 27 | VM-entry 保留位不为 0 |

---

## 22. MSR Bitmaps 与 I/O Bitmaps

### 22.1 MSR Bitmaps 布局

MSR Bitmaps 地址指向一个 4KB 的位图区域。

| 偏移 | 大小 | 描述 |
|------|------|------|
| 0-1023 | 1024 字节 | RDMSR 位图（低位 MSR: 0x0000-0x1FFF） |
| 1024-2047 | 1024 字节 | RDMSR 位图（高位 MSR: 0xC0000000-0xC0001FFF） |
| 2048-3071 | 1024 字节 | WRMSR 位图（低位 MSR: 0x0000-0x1FFF） |
| 3072-4095 | 1024 字节 | WRMSR 位图（高位 MSR: 0xC0000000-0xC0001FFF） |

**位图编码**:
- 每个 MSR 映射到位图中的一位
- 低位 MSR: bit = MSR 地址（0x0000-0x1FFF），每位覆盖 8 个 MSR 地址
- 高位 MSR: bit = MSR 地址 & 0x1FFF

**注意**: 如果某位=1，对应的 MSR 访问会触发 VM-Exit（忽略 MSR 位图过滤）。

### 22.2 I/O Bitmaps 布局

I/O Bitmaps A 和 B 分别指向两个 4KB 位图区域。

| 位图 | 覆盖端口 | 大小 |
|------|----------|------|
| I/O Bitmap A | 0x0000-0x7FFF | 4096 字节（8192 端口，每位=1 触发 VM-Exit） |
| I/O Bitmap B | 0x8000-0xFFFF | 4096 字节（8192 端口，每位=1 触发 VM-Exit） |

**位图编码**:
- 位图中第 port 位控制端口 port
- 位=1: I/O 指令触发 VM-Exit
- 位=0: I/O 指令正常执行（不触发 VM-Exit）

---

## 23. 虚拟中断交付

### 23.1 概念

Virtual-Interrupt Delivery (secondary control bit 9) 允许处理器直接评估和交付虚拟中断到 Guest，无需 VM-Exit。

### 23.2 依赖条件

- Virtualize APIC accesses (secondary bit 0) = 1
- Virtual-interrupt delivery (secondary bit 9) = 1
- Use TPR shadow (primary bit 21) = 1
- 需要 APIC-register virtualization (secondary bit 8) = 1 **或** Virtualize x2APIC mode (secondary bit 4) = 1

### 23.3 虚拟中断评估条件

1. **RFLAGS.IF = 1**（中断启用）
2. **无 STI/MOV-SS 阻塞**（Interruptibility State bits 0,1 = 0）
3. **TPR 检查**: 虚拟中断的优先级 > VTPR（Task Priority Register）
4. **PPR 检查**: 虚拟中断的优先级 > Processor Priority Register
5. **EOI 退出位图**: 如果虚拟中断的向量在 EOI-exit bitmap 中，则 EOI 写触发 VM-Exit

### 23.4 Guest Interrupt Status (编码 `0810H`)

16 位字段，用于跟踪虚拟中断状态：

| 位 | 字段 | 描述 |
|----|------|------|
| 7:0 | **RVI** (Requested Vector) | 请求的最高优先级虚拟中断向量 |
| 15:8 | **SVI** (Served Vector) | 正在服务的最高优先级虚拟中断向量 |

---

## 24. TSC Offsetting 与 TSC Scaling

### 24.1 TSC Offsetting

由 Primary Processor-Based 控制 bit 3 控制。

**计算公式**:
```
Guest_TSC = Host_TSC + TSC_Offset
```

TSC Offset 是 64 位 VMCS 字段（编码 `2010H`）。

### 24.2 TSC Scaling

由 Secondary Processor-Based 控制 bit 25 控制。

**计算公式**:
```
Guest_TSC = Host_TSC × TSC_Multiplier
```

TSC Multiplier 不是直接的 VMCS 字段，而是通过 `IA32_TSC_MULTIPLIER` MSR（地址 `0x00002032`）设置。

**当 TSC Offsetting 和 TSC Scaling 同时启用**:
```
Guest_TSC = (Host_TSC × TSC_Multiplier) + TSC_Offset
```

### 24.3 TSC 相关退出条件

| 条件 | 行为 |
|------|------|
| RDTSC exiting=0, TSC offsetting=0 | Guest_RDTSC = Host_TSC |
| RDTSC exiting=0, TSC offsetting=1 | Guest_RDTSC = Host_TSC + Offset |
| RDTSC exiting=0, TSC scaling=1 | Guest_RDTSC = Host_TSC × Multiplier |
| RDTSC exiting=1 | RDTSC/RDTSCP 触发 VM-Exit |

---

## 25. VMware / VirtualBox 兼容性设置

### 25.1 VMCS Shadowing (Secondary Control Bit 14)

允许 Guest 通过 VMREAD/VMWRITE 访问影子 VMCS。

**启用条件**:
1. Secondary 控制 bit 14 = 1
2. VMREAD-bitmap 地址（编码 `2026H`）已设置
3. VMWRITE-bitmap 地址（编码 `2028H`）已设置

**位图格式**:
- VMREAD-bitmap: 4KB，每 8 个编码映射到一位，位=1 允许非根 VMREAD
- VMWRITE-bitmap: 4KB，每 8 个编码映射到一位，位=1 允许非根 VMWRITE

**两级 VMCS 指针**:
- Executive-VMCS pointer（编码 `200CH`）: 真正的 VMCS
- VMCS link pointer（编码 `2800H`）: 影子 VMCS（Guest 可访问）
- 非根 VMREAD/VMWRITE 通过影子 VMCS 执行，实际由处理器映射到 Executive VMCS

---

## 26. 参考代码模式

### 26.1 VMCS 区域分配

```c
// 分配 4KB 对齐的 VMCS 区域
void* vmx_alloc_vmcs_region() {
    void* region = _aligned_malloc(4096, 4096);
    if (!region) return NULL;
    RtlZeroMemory(region, 4096);
    
    // 写入 Revision ID
    uint32_t revision = (uint32_t)__readmsr(IA32_VMX_BASIC);
    *(uint32_t*)region = revision;  // bit 31 = 0
    
    return region;
}
```

### 26.2 VMXON 初始化

```c
NTSTATUS vmxon_init() {
    // 1. 检查 VMX 支持
    if (!cpuid_has_vmx()) return STATUS_NOT_SUPPORTED;
    
    // 2. 启用 VMXE
    __writemsr(IA32_VMX_CR4_FIXED0, 
              __readmsr(IA32_VMX_CR4_FIXED0) | (1ULL << 13)); // CR4.VMXE
    
    // 3. 验证 CR0/CR4
    uint64_t cr0 = __readcr0();
    uint64_t fixed0 = __readmsr(IA32_VMX_CR0_FIXED0);
    uint64_t fixed1 = __readmsr(IA32_VMX_CR0_FIXED1);
    if ((cr0 & fixed0) || (cr0 & ~fixed1)) return STATUS_INVALID_PARAMETER;
    
    // 4. 分配 VMXON 区域
    void* vmxon_region = vmx_alloc_vmcs_region();
    if (!vmxon_region) return STATUS_NO_MEMORY;
    
    // 5. 执行 VMXON
    _mm_prefetch(vmxon_region, _MM_HINT_NTA);
    int status = __vmx_on(&vmxon_region);
    if (status) return STATUS_UNSUCCESSFUL;
    
    return STATUS_SUCCESS;
}
```

### 26.3 控制字段安全编程

```c
uint32_t adjust_vmx_controls(uint32_t ctl_min, uint32_t ctl_opt, uint32_t msr) {
    uint64_t msr_val = __readmsr(msr);
    uint32_t allowed_0 = (uint32_t)(msr_val);        // must-be-1 bits
    uint32_t allowed_1 = (uint32_t)(msr_val >> 32);   // can-be-1 bits
    
    uint32_t desired = ctl_min | ctl_opt;
    uint32_t value = (allowed_0 | desired) & allowed_1;
    
    return value;
}

// 使用示例
void setup_vmcs_controls() {
    // 决定使用 True MSR 还是原始 MSR
    uint64_t basic = __readmsr(IA32_VMX_BASIC);
    uint32_t pin_msr = (basic & (1ULL << 55)) ? 
                       IA32_VMX_TRUE_PINBASED_CTLS : IA32_VMX_PINBASED_CTLS;
    
    uint32_t pin_ctls = adjust_vmx_controls(
        0,                               // 最小需求
        PINBASED_EXTINT_EXITING |        // 可选功能
        PINBASED_NMI_EXITING,
        pin_msr
    );
    __vmx_vmwrite(PIN_BASED_VM_EXEC_CTRL, pin_ctls);
}
```

---

## 27. 调试与故障排除

### 27.1 VM-Entry 失败调试流程

```
VMLAUNCH/VMRESUME 失败
         │
         ▼
  读取 VM-instruction error (4400H)
         │
         ▼
  读取 Exit reason (4402H)
  检查 bit 30 (VM-entry failure)
         │
         ▼
  根据错误码排查:
  ┌─ 无效 Guest 状态 (33) → 检查段寄存器/CR/RIP/EFLAGS
  ├─ MSR 加载失败   (34) → 检查 MSR 列表地址和内容
  └─ 机器检查       (41) → 严重硬件错误
```

### 27.2 常见错误

| 症状 | 可能原因 | 解决方案 |
|------|----------|----------|
| VMXON 失败 (#GP) | CR0/CR4 不满足 Fixed0/Fixed1 | 检查并设置 CR0.NE, PE, PG; CR4.VMXE |
| VMPTRLD 失败 | Revision ID 不匹配 | 从 IA32_VMX_BASIC 获取正确的 Revision ID |
| VMLAUNCH 失败 (错误 25) | 段描述符校验失败 | 检查 CS/SS/TR 的 access rights |
| VMLAUNCH 失败 (错误 26) | CR/EFLAGS/RIP 无效 | 检查 CR0.PG/PE, RFLAGS.VM, RIP 规范地址 |
| VM-Exit 后不能恢复 | Host RIP 无效 | 确认 Host RIP 指向 VMM 入口点 |
| EPT 违规 (退出 48) | EPT 项未正确配置 | 检查 EPT 权限位和物理地址 |

### 27.3 典型 Hypervisor 初始化序列

```c
// 每 CPU 初始化流程
void vcpu_vmx_init() {
    // 1. 硬件使能
    enable_vmx_operation();           // CR4.VMXE, CR0/CR4 fixed, VMXON
    
    // 2. 分配和初始化 VMCS
    void* vmcs = vmx_alloc_vmcs_region();
    __vmx_vmptrld(&vmcs);
    
    // 3. 配置 VM-Execution 控制
    set_pin_based_ctls();
    set_primary_proc_based_ctls();    // 含异常位图
    set_secondary_proc_based_ctls();  // EPT, VPID 等
    set_tertiary_proc_based_ctls();   // 如需要
    
    // 4. 配置 VM-Exit 控制
    set_vmexit_ctls();                // Host 地址大小, ACK 等
    
    // 5. 配置 VM-Entry 控制
    set_vmentry_ctls();               // IA-32e guest 等
    
    // 6. 配置 Host 状态
    set_host_state();                 // CS/SS/DS/ES, CR3, RIP, RSP
    
    // 7. 配置 Guest 状态
    set_guest_state();                // 段寄存器, CR0/CR3/CR4, RIP, RSP, RFLAGS
    
    // 8. 启动 VM
    int status = __vmx_vmlaunch();
    if (status) analyze_vmentry_failure();
}
```

---

## 附录 B: 参考文献

- Intel SDM Volume 3C: Chapter 23-33, Appendix A-C (Order #325384)
- Intel SDM Volume 4: MSR Reference (Order #335592)
- Intel 64 and IA-32 Architectures Software Developer Manuals: https://www.intel.com/sdm
- Linux KVM 源码: `arch/x86/kvm/vmx.c`, `arch/x86/include/asm/vmx.h`
- 《处理器虚拟化技术》, 邓志著, 电子工业出版社
- felixcloutier.com x86 Instruction Reference: https://www.felixcloutier.com/x86/

---

> **文档维护**: 本文件是 VMX Hypervisor Toolbox 项目的 Intel VT-x 参考手册。
> 如有更新或修正，请提交 PR 到项目仓库。
