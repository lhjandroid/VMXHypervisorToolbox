# hv_detect.c -- 逻辑分析

## 1. 文件概述

### 角色与职责

`hv_detect.c` 是 VMX Hypervisor Toolbox 的 CPU 检测模块，负责在驱动初始化的早期阶段完成以下任务：

- **CPU 厂商识别**：通过 CPUID 指令检测 CPU 是 Intel 还是 AMD
- **虚拟化能力验证**：确认 CPU 支持并已启用相应的虚拟化技术（Intel VT-x / AMD SVM）
- **AMD 特有功能查询**：检测 NPT（Nested Page Tables）支持、SVM 修订版本、最大 ASID 数量
- **Hypervisor 环境检测**：检测当前是否已在某 Hypervisor 下运行，以及是否正在运行 Windows Hyper-V

该模块是选择虚拟化后端（VMX vs SVM）的前提条件，其结果直接影响 `DriverEntry` 中 `g_HvOps` 的赋值决策。

### 依赖的其他模块

| 头文件 | 用途 |
|--------|------|
| `hv_detect.h` | 导出函数声明和依赖的 `hv_ops.h` 类型 |
| `log.h` | 日志记录，用于输出检测结果和错误信息 |

---

## 2. 数据结构

### 内部常量

#### AMD MSR 定义

```c
#define MSR_VM_CR      0xC0010114   // AMD SVM 控制寄存器
#define MSR_VM_HSAVE_PA 0xC0010117  // AMD SVM 主机保存区物理地址
```

- `MSR_VM_CR`：AMD SVM 功能控制 MSR。bit 4（SVMDIS）表示 SVM 是否被禁用，bit 3（LOCK）表示配置是否被锁定。
- `MSR_VM_HSAVE_PA`：保存主机状态的保存区域物理地址，仅在 SVM 激活时有效。

#### VM_CR 标志位

```c
#define VM_CR_SVMDIS  (1ULL << 4)   // SVM disabled
#define VM_CR_LOCK    (1ULL << 3)   // SVM lock
```

- `VM_CR_SVMDIS`：BIOS 或系统固件已禁用 SVM
- `VM_CR_LOCK`：VM_CR 寄存器已锁定，无法修改

#### SVM CPUID 函数号

```c
#define SVM_CPUID_FUNC  0x8000000A  // SVM 功能信息 CPUID 叶
```

此叶返回 SVM 的修订号、支持的 ASID 数量、以及各种 SVM 特性标志。

---

## 3. 核心函数详解

### 3.1 `HvDetectCpuVendor`

```c
CPU_VENDOR HvDetectCpuVendor(VOID)
```

**功能**：通过 CPUID 指令的 Leaf 0 检测 CPU 厂商。

**核心逻辑流程**：

1. 执行 `__cpuid(CpuInfo, 0)`，获取 CPUID Leaf 0 的返回
2. CPUID Leaf 0 的 `EBX:EDX:ECX` 包含 12 字节的厂商字符串：
   - **Intel**：`"GenuineIntel"` 编码为 `EBX=0x756E6547`（"Genu"）、`EDX=0x49656E69`（"ineI"）、`ECX=0x6C65746E`（"ntel"）
   - **AMD**：`"AuthenticAMD"` 编码为 `EBX=0x68747541`（"Auth"）、`EDX=0x69746E65`（"enti"）、`ECX=0x444D4163`（"cAMD"）

3. 分别与 Intel 和 AMD 的 magic 值比较
4. 匹配则返回 `CPU_VENDOR_INTEL` 或 `CPU_VENDOR_AMD`
5. 不匹配则输出警告日志并返回 `CPU_VENDOR_UNKNOWN`

**返回值**：`CPU_VENDOR` 枚举值。

**设计要点**：
- 使用硬编码的 magic 值进行字符串比较，避免了字符串库函数的依赖
- 不匹配时输出 EBX/EDX/ECX 实际值，便于调试未知 CPU

---

### 3.2 `HvCheckVmxSupport`

```c
BOOLEAN HvCheckVmxSupport(VOID)
```

**功能**：检测 Intel CPU 是否支持并启用了 VMX（Virtual Machine Extensions）。

**核心逻辑流程**：

1. **CPUID.1:ECX[5] 检查**
   - 执行 `__cpuid(CpuInfo, 1)`，检查 `ECX[5]`（bit 5）
   - 若为 0，表示 CPU 不支持 VMX，返回 `FALSE`

2. **IA32_FEATURE_CONTROL MSR（0x3A）检查**
   - 读取 MSR `0x003A`，检查 Lock 位（bit 0）：
     - **已锁定**（bit 0 = 1）：检查 VMXON 使能位（bit 2），若未设置则 VMX 被 BIOS 锁定，返回 `FALSE`
     - **未锁定**（bit 0 = 0）：输出警告（BIOS 未正确配置 VMX），但继续返回 `TRUE`

**返回值**：
- `TRUE`：VMX 可用
- `FALSE`：VMX 不可用或被禁用

**设计要点**：
- 区分"CPU 不支持"和"BIOS 禁用了"两种场景
- IA32_FEATURE_CONTROL 未锁定是 BIOS 配置不当，但不阻止驱动继续使用 VMX
- `__readmsr(0x003A)` 是 MSR 读取的编译器内建函数

---

### 3.3 `HvCheckSvmSupport`

```c
BOOLEAN HvCheckSvmSupport(VOID)
```

**功能**：检测 AMD CPU 是否支持并启用了 SVM（Secure Virtual Machine）。

**核心逻辑流程**：

1. **扩展 CPUID 可用性检查**
   - 执行 `__cpuid(CpuInfo, 0x80000000)`，检查 EAX >= 0x80000001
   - 若不支持扩展 CPUID，返回 `FALSE`

2. **CPUID 0x80000001:ECX[2] 检查**
   - 检查 SVM 功能位（bit 2）
   - 若为 0，CPU 不支持 SVM，返回 `FALSE`

3. **MSR_VM_CR（0xC0010114）检查**
   - 读取 MSR_VM_CR
   - 检查 `VM_CR_SVMDIS`（bit 4）：
     - **SVMDIS = 0**：SVM 未被禁用，支持确认，返回 `TRUE`
     - **SVMDIS = 1**：SVM 被禁用

4. **SVM Lock 检查（仅当 SVMDIS = 1）**
   - 执行 `__cpuid(CpuInfo, 0x8000000A)`，检查 `EDX[2]`（SVM Lock 位）
   - 若 SVM Lock = 1：SVM 被 BIOS 锁定且无法启用，返回 `FALSE`
   - 若 SVM Lock = 0：SVM 被禁用但未锁定（可能需要密钥），返回 `FALSE`

**返回值**：
- `TRUE`：SVM 可用
- `FALSE`：SVM 不可用或被禁用

**设计要点**：
- 比 VMX 检测更复杂，因为 AMD 的 SVM 禁用机制有两种：SVMDIS（可能可解除）和 SVM Lock（不可逆）
- SVM 被禁用但未锁定（SVMDIS=1, SVM Lock=0）时，理论上可以通过写入 MSR_VM_CR 启用，但当前实现选择返回 FALSE

---

### 3.4 `HvCheckNptSupport`

```c
BOOLEAN HvCheckNptSupport(VOID)
```

**功能**：检测 AMD CPU 是否支持 NPT（Nested Page Tables），即 AMD 版的硬件辅助内存虚拟化。

**逻辑**：
1. 执行 `__cpuid(CpuInfo, 0x8000000A)`
2. 检查 `EDX[0]`（NPT 位）
3. 支持时输出 INFO 日志，不支持时输出 WARN 日志

**返回值**：`TRUE` = NPT 支持，`FALSE` = 不支持。

---

### 3.5 `HvGetSvmRevision`

```c
ULONG HvGetSvmRevision(VOID)
```

**功能**：获取 AMD SVM 的修订版本号。

**逻辑**：
1. 执行 `__cpuid(CpuInfo, 0x8000000A)`
2. 返回 `EAX[7:0]`（SVM 修订版）

**用途**：SVM 不同修订版支持的 feature 可能不同，驱动可能需要根据修订版选择不同的 VMCB 布局。

---

### 3.6 `HvGetMaxAsid`

```c
ULONG HvGetMaxAsid(VOID)
```

**功能**：获取 CPU 支持的最大 ASID（Address Space Identifier）数量。

**逻辑**：
1. 执行 `__cpuid(CpuInfo, 0x8000000A)`
2. 返回 `EBX`（ASID 数量）

**用途**：ASID 用于 SVM 的 TLB 标记，避免在 VM 切换时刷新 TLB。驱动需要知道 ASID 数量以分配和管理 ASID 池。

---

### 3.7 `HvIsRunningUnderHypervisor`

```c
BOOLEAN HvIsRunningUnderHypervisor(VOID)
```

**功能**：检测当前系统是否已在某 Hypervisor 下运行。

**核心逻辑流程**：

1. **CPUID.1:ECX[31] 检查**
   - 执行 `__cpuid(CpuInfo, 1)`，检查 ECX[31]（hypervisor present 位）
   - 若为 1，表示已有 Hypervisor 在运行，返回 `TRUE`
   - Intel SDM Vol 3, §2.2：所有主流 Hypervisor 都会设置此位

2. **CPUID 0x40000000:EAX 检查**
   - 执行 `__cpuid(CpuInfo, 0x40000000)`，检查 EAX
   - 在裸机上，此叶子返回 EAX=0（最大 Hypervisor CPUID 叶号为 0，表示无 Hypervisor）
   - 若 EAX > 0，表示存在 Hypervisor，返回 `TRUE`

3. 两个检查均通过（均未检测到 Hypervisor），返回 `FALSE`

**设计要点**：
- 使用两个独立信号交叉验证，确保检测可靠性
- 在 `DriverEntry` 早期调用，用于决策是否继续加载 Hypervisor
- 若检测到已存在 Hypervisor，驱动应中止初始化，避免嵌套虚拟化

---

### 3.8 `HvIsHyperVEnabled`

```c
BOOLEAN HvIsHyperVEnabled(VOID)
```

**功能**：检测 Windows Hyper-V 是否已启用。

**核心逻辑流程**：

1. **Hypervisor CPUID 叶存在性检查**
   - 执行 `__cpuid(CpuInfo, 0x40000000)`，检查 `EAX >= 0x40000001`
   - 若不存在 Hypervisor CPUID 叶或最大叶小于 1，返回 `FALSE`

2. **Hyper-V 接口签名检查**
   - 执行 `__cpuid(CpuInfo, 0x40000001)`，检查 `EAX == 0x31237648`（"Hv#1"）
   - 若匹配，表示 Hyper-V 活动且接口符合规范
   - 输出警告日志并返回 `TRUE`

3. 不匹配则返回 `FALSE`

**设计要点**：
- 与 `HvIsRunningUnderHypervisor` 分离，因为 Hyper-V 可能在内核级别和硬件级别同时启用
- 专门检测 "Hv#1" 签名（符合规范的 Hyper-V 接口），忽略非规范的 "Hv#0"
- 返回值用于决定驱动是否应尝试加载（若 Hyper-V 已启用，嵌套虚拟化不可行）

---

## 4. 控制流与逻辑流程

### 4.1 CPU 检测流程

```
HvDetectCpuVendor()
 |
 +-- __cpuid(CpuInfo, 0)  // CPUID Leaf 0
 +-- CpuInfo[1] == 0x756E6547 AND
 |    CpuInfo[3] == 0x49656E69 AND
 |    CpuInfo[2] == 0x6C65746E ?
 |    +-- YES -> 返回 CPU_VENDOR_INTEL
 |    +-- NO  -> 继续
 |
 +-- CpuInfo[1] == 0x68747541 AND
 |    CpuInfo[3] == 0x69746E65 AND
 |    CpuInfo[2] == 0x444D4163 ?
 |    +-- YES -> 返回 CPU_VENDOR_AMD
 |    +-- NO  -> 返回 CPU_VENDOR_UNKNOWN
```

### 4.2 VMX 支持检测流程

```
HvCheckVmxSupport()
 |
 +-- CPUID.1:ECX[5] == 1 ?
 |    +-- NO  -> 返回 FALSE（CPU 不支持 VMX）
 |    +-- YES -> 继续
 |
 +-- IA32_FEATURE_CONTROL.Lock (bit 0) ?
      +-- YES -> IA32_FEATURE_CONTROL.VMXON (bit 2) 已设置？
      |    +-- YES -> 返回 TRUE
      |    +-- NO  -> 返回 FALSE（VMX 被 BIOS 锁定）
      +-- NO  -> 输出 WARN，返回 TRUE（未锁定但继续）
```

### 4.3 SVM 支持检测流程

```
HvCheckSvmSupport()
 |
 +-- CPUID 0x80000000:EAX >= 0x80000001 ?
 |    +-- NO  -> 返回 FALSE
 |    +-- YES -> 继续
 |
 +-- CPUID 0x80000001:ECX[2] == 1 ?
 |    +-- NO  -> 返回 FALSE（CPU 不支持 SVM）
 |    +-- YES -> 继续
 |
 +-- MSR_VM_CR.SVMDIS (bit 4) == 0 ?
      +-- YES -> 返回 TRUE（SVM 可用）
      +-- NO  -> 继续
      |
      +-- CPUID 0x8000000A:EDX[2] == 1 ?
           +-- YES -> 返回 FALSE（SVM 被 BIOS 锁定）
           +-- NO  -> 返回 FALSE（SVM 被禁用但未锁定）
```

### 4.4 Hypervisor 环境检测流程

```
HvIsRunningUnderHypervisor()
 |
 +-- CPUID.1:ECX[31] == 1 ?
 |    +-- YES -> 返回 TRUE（已存在 Hypervisor）
 |    +-- NO  -> 继续
 |
 +-- CPUID 0x40000000:EAX > 0 ?
      +-- YES -> 返回 TRUE（已存在 Hypervisor）
      +-- NO  -> 返回 FALSE（裸机）
```

### 4.5 Hyper-V 启用检测流程

```
HvIsHyperVEnabled()
 |
 +-- CPUID 0x40000000:EAX >= 0x40000001 ?
 |    +-- NO  -> 返回 FALSE（无 Hypervisor CPUID 叶）
 |    +-- YES -> 继续
 |
 +-- CPUID 0x40000001:EAX == 0x31237648 ("Hv#1") ?
      +-- YES -> 返回 TRUE（Hyper-V 已启用）
      +-- NO  -> 返回 FALSE（其他 Hypervisor 或无）
```

---

## 5. 与其他模块的交互

| 模块 | 交互方式 | 详细说明 |
|------|----------|----------|
| `hv_ops.h` | 类型依赖 | 依赖 `CPU_VENDOR` 枚举类型 |
| `vmxdrv.c` | 调用者 | `DriverEntry` 调用 `HvDetectCpuVendor` + `HvCheckVmxSupport`/`HvCheckSvmSupport` 检测 CPU 能力；同时调用 `HvIsRunningUnderHypervisor` 和 `HvIsHyperVEnabled` 进行裸机环境验证 |
| `vmx_init.c` | 隐式依赖 | VMX 后端初始化前假设检测已完成且结果为 Intel |
| `svm_init.c` | 隐式依赖 | SVM 后端初始化前假设检测已完成且结果为 AMD |
| `log.h` | 日志 | 使用 `LOG_INFO`、`LOG_WARN`、`LOG_ERROR` 输出检测结果 |

---

## 6. 关键设计要点

### 6.1 Magic Number 字符串比较

对比 `EBX:EDX:ECX` 组合值而不是使用 `memcmp` 或字符串操作：
- 无外部函数依赖，适用于驱动早期初始化阶段
- 编译时常量，效率最高
- 每个 magic 值对应 4 字节的 ASCII 表示（小端序）

### 6.2 区分"不支持"和"被禁用"

- Intel VMX：通过 IA32_FEATURE_CONTROL 的 Lock 和 VMXON 位区分
- AMD SVM：通过 VM_CR.SVMDIS 和 SVM Lock 位组合区分
- 错误日志可帮助用户快速定位问题（是否需要在 BIOS 中启用虚拟化）

### 6.3 AMD SVM Lock 机制的特殊处理

AMD 的 SVM 锁定机制比 Intel 更复杂：
- SVMDIS = 1 但 SVM Lock = 0 的场合：理论上可通过特定密钥写入 MSR_VM_CR 解除，但本实现未处理此场景
- SVMDIS = 1 且 SVM Lock = 1 的场合：无法绕过，必须 BIOS 启用

### 6.4 Windows 驱动框架注意点

- 使用编译器内建函数 `__cpuid`、`__readmsr`，无需内联汇编
- 所有函数可在 `DriverEntry` 的 `PASSIVE_LEVEL` 安全调用
- 没有使用任何 WDK 特定的 API，仅依赖编译器内置指令
- CPUID 指令在所有 x64 CPU 上可用，无需条件检查

### 6.5 裸机环境验证

在驱动初始化的最早阶段，必须确认当前运行在裸机（Ring 0）而非已有 Hypervisor 之上：

- **双重检测策略**：结合 CPUID.1:ECX[31]（hypervisor present 位，遵循 Intel SDM Vol 3, §2.2）和 CPUID.0x40000000（Hypervisor CPUID 叶存在性）两个独立信号交叉验证
- **Hyper-V 专门检测**：通过检查 CPUID.0x40000001 是否返回 "Hv#1" 接口签名（0x31237648）来区分 Windows Hyper-V 与其他 Hypervisor
- **嵌套虚拟化保护**：检测到 Hypervisor 存在时，驱动应中止初始化，避免因嵌套虚拟化导致的性能问题或功能异常
- **调用时机**：在 `DriverEntry` 中，于虚拟化后端选择之前调用，确保裸机环境
