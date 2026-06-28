# msr.c -- 逻辑分析

## 1. 文件概述

### 角色与职责

`msr.c` 是 VMX Hypervisor Toolbox 的 MSR（Model-Specific Register）管理模块，承担以下核心职责：

- **MSR 位图初始化**：配置 VMX/SVM MSR 位图，选择性拦截需要监控的 MSR 读写操作
- **MSR 读取处理**（`HandleRdmsrImpl`）：拦截和处理 Guest 的 RDMSR 指令，实现反调试欺骗和 VMX/SVM 能力隐藏
- **MSR 写入处理**（`HandleWrmsrImpl`）：拦截和处理 Guest 的 WRMSR 指令，阻止 Guest 启用虚拟化功能
- **无效 MSR 预探测**：在进入 VMX/SVM Root 模式前，预先探测哪些 MSR 会导致 #GP，避免在 Root 模式下不可靠的 SEH 处理

### 依赖的其他模块

| 头文件 | 用途 |
|--------|------|
| `vmx.h` | VMX 状态结构、MSR 常量（如 `MSR_IA32_DEBUGCTL`） |
| `hv_ops.h` | 通过 `HvReadGuestCr3()`、`HvAdvanceGuestRip()` 等宏访问 Guest 状态 |
| `log.h` | 日志输出 |
| `process.h` | `IsTargetProcess()`、`IsFeatureEnabled()` 目标进程判断 |
| `shared.h` | 共享定义 |

---

## 2. 数据结构

### 2.1 无效 MSR 位图（Invalid MSR Bitmap）

#### 设计目标

VMX/SVM Root 模式下 SEH（Structured Exception Handling）不可靠，因为 Host 栈位于 `ExAllocatePoolWithTag` 分配的非分页内存上，不在正常的线程内核栈上。当 `__readmsr` 对不存在的 MSR 执行时引发 #GP，SEH 会沿损坏的异常处理链跳转到零填充的栈内存，导致 BSOD 0x0A（IRQL_NOT_LESS_OR_EQUAL）。

解决方案：在进入 Root 模式前（此时 SEH 仍然可靠），预先探测 MSR，将导致 #GP 的 MSR 标记在位图中。在 Root 模式的 MSR 处理函数中，仅通过查位图来决定注入 #GP 还是执行真实 MSR 操作。

#### 位图布局

```
位图总大小：2048 字节（2KB）
[0x000..0x3FF] MSR 0x00000000 - 0x00001FFF（低范围，1024 字节 = 8192 位）
[0x400..0x7FF] MSR 0xC0000000 - 0xC0001FFF（高范围，1024 字节 = 8192 位）
```

每个 MSR 对应 1 个 bit，1 = 无效（会导致 #GP），0 = 有效或未知。

#### 偏移量常量

```c
#define INVALID_MSR_BITMAP_SIZE  2048
#define INVALID_MSR_LOW_OFFSET   0
#define INVALID_MSR_HIGH_OFFSET  0x400
```

#### 全局位图指针

```c
static PUCHAR g_InvalidMsrBitmap = NULL;
```

NULL 表示尚未完成预探测，处理函数将直接执行 MSR 操作。

### 2.2 MSR 位图（VMCS MSR Bitmap）

标准 VMX MSR 位图布局（4KB）：

```
[0x000..0x3FF] 低 MSR 范围（0x0-0x1FFF）的读取拦截位图
[0x400..0x7FF] 高 MSR 范围（0xC0000000-0xC0001FFF）的读取拦截位图
[0x800..0xBFF] 低 MSR 范围（0x0-0x1FFF）的写入拦截位图
[0xC00..0xFFF] 高 MSR 范围（0xC0000000-0xC0001FFF）的写入拦截位图
```

---

## 3. 核心函数详解

### 3.1 `MsrIsKnownInvalid`

```c
static BOOLEAN MsrIsKnownInvalid(ULONG Msr)
```

**功能**：查询指定 MSR 是否在预探测位图中被标记为无效。

**参数**：

| 参数 | 范围 | 说明 |
|------|------|------|
| `Msr` | 任意 | 要查询的 MSR 编号 |

**核心逻辑**：
1. `g_InvalidMsrBitmap == NULL` -> 返回 `FALSE`（未知状态按有效处理）
2. `Msr <= 0x1FFF`：低范围，偏移量 `INVALID_MSR_LOW_OFFSET + Msr/8`
3. `Msr >= 0xC0000000 && Msr <= 0xC0001FFF`：高范围，偏移量 `INVALID_MSR_HIGH_OFFSET + Index/8`
4. 其他范围 -> 返回 `FALSE`（位图不覆盖）

**返回值**：`TRUE` = 已知无效，`FALSE` = 有效或未知。

### 3.2 `MsrSetInvalid`

```c
static VOID MsrSetInvalid(ULONG Msr)
```

**功能**：在位图中将指定 MSR 标记为无效。

**逻辑**：与 `MsrIsKnownInvalid` 镜像的偏移量计算，将对应 bit 置 1。

### 3.3 `MsrProbeInvalidMsrs`

```c
NTSTATUS MsrProbeInvalidMsrs(VOID)
```

**功能**：在进入 VMX/SVM Root 模式前，预探测 MSR 的可用性。

**核心逻辑流程**：

1. **位图分配**：若 `g_InvalidMsrBitmap == NULL`，分配 2KB 非分页池，标签 `'rsmI'`
2. **低范围探测**（`0x0000 - 0x1FFF`）：
   - 对每个 MSR 执行 `__readmsr(Msr)`
   - SEH 捕获 #GP 异常：`_except(EXCEPTION_EXECUTE_HANDLER)`
   - 捕获到异常时调用 `MsrSetInvalid(Msr)` 标记
3. **高范围探测**（`0xC0000000 - 0xC0001FFF`）：
   - 同样方式遍历 8192 个 MSR
4. **日志输出**：报告每个范围的无效 MSR 数量

**设计要点**：
- 仅在 VMXON/VMRUN 前调用一次，此时 SEH 可靠
- 只探测 RDMSR（读取），不探测 WRMSR，避免写探测可能改变系统状态
- 覆盖的 MSR 范围对应 Intel/AMD MSR 位图的两个标准范围

### 3.4 `MsrCleanupInvalidBitmap`

```c
VOID MsrCleanupInvalidBitmap(VOID)
```

**功能**：释放无效 MSR 位图。

**逻辑**：检查 `g_InvalidMsrBitmap` 非空，调用 `ExFreePoolWithTag` 释放，将指针置 NULL。

### 3.5 `MsrBitmapSetBit`

```c
static VOID MsrBitmapSetBit(PVOID MsrBitmap, ULONG Msr, BOOLEAN Read, BOOLEAN Write)
```

**功能**：在 VMCS MSR 位图中设置拦截位。

**参数**：

| 参数 | 说明 |
|------|------|
| `MsrBitmap` | 4KB 位图缓冲区 |
| `Msr` | 要拦截的 MSR |
| `Read` | 是否拦截 RDMSR |
| `Write` | 是否拦截 WRMSR |

**核心逻辑**：
- 低 MSR（`<= 0x1FFF`）：读位图在 `[ByteOffset]`，写位图在 `[0x800 + ByteOffset]`
- 高 MSR（`0xC0000000 - 0xC0001FFF`）：读位图在 `[0x400 + ByteOffset]`，写位图在 `[0xC00 + ByteOffset]`

### 3.6 `MsrBitmapInitialize`

```c
VOID MsrBitmapInitialize(PVOID MsrBitmap)
```

**功能**：初始化每个 CPU 的 MSR 位图。

**核心逻辑**：

1. `RtlZeroMemory` 清空 4KB 位图（默认全部放行）
2. 配置拦截的 MSR：

| MSR | 地址 | 拦截原因 |
|-----|------|----------|
| `IA32_DEBUGCTL` | `0x1D9` | 反调试：监控调试控制寄存器的修改 |
| `IA32_FEATURE_CONTROL` | `0x03A` | VMX/SVM 能力隐藏：隐藏 VMXON 使能状态 |
| `IA32_VMX_BASIC` ~ `IA32_VMX_VMFUNC` | `0x480` - `0x491` | VMX/SVM 能力隐藏：隐藏 VMX 能力 |

### 3.7 `HandleRdmsrImpl`

```c
BOOLEAN HandleRdmsrImpl(PGUEST_CONTEXT GuestContext)
```

**功能**：处理 Guest 发起的 RDMSR 指令的 VM-Exit。

**核心逻辑流程**：

1. **读取 MSR 编号**：`Msr = (ULONG)GuestContext->Rcx`

2. **VMX/SVM 能力隐藏**：
   - `0x480 - 0x491`（VMX 能力 MSR）：返回 `RAX=0, RDX=0`，假装 VMX 不可用
   - `0x003A`（IA32_FEATURE_CONTROL）：返回 `RAX=1`（Lock=1, VMXON=0）
   - `0xC0010114`（MSR_VM_CR，AMD）：设置 SVMDIS=1 和 LOCK=1
   - `0xC0010117`（MSR_VM_HSAVE_PA，AMD）：返回 0

3. **Hyper-V 合成 MSR**（`0x40000000 - 0x400000FF`）：
   - 在裸机上，hypervisor-present 位（CPUID.1:ECX[31]）为 0，Windows 不会查询这些 MSR
   - 这些 MSR 位于标准 MSR 位图范围之外，总是引发 VM-Exit
   - 如果到达此处理程序（第三方工具探测），注入 #GP(0) — 它们在裸机上不存在，Guest 应看到与真实硬件相同的异常

4. **无效 MSR 预探测检查**：
   - 调用 `MsrIsKnownInvalid(Msr)` 检查
   - 若为已知无效 MSR，注入 #GP(0) 到 Guest，不执行真实 RDMSR

5. **未知 MSR 安全网**（`!((Msr <= 0x1FFF) || (Msr >= 0xC0000000 ...))`）：
   - 不在位图覆盖范围内的 MSR，注入 #GP
   - 带频率限制的日志记录（最多输出 20 次，使用 `InterlockedIncrement`）

6. **执行真实 RDMSR**：
   - `Value = __readmsr(Msr)`（无 SEH 保护）
   - 只有已知有效的 MSR 能到达此路径

7. **反调试欺骗**：
   - 检查当前 Guest CR3 是否属于目标进程
   - `IA32_DEBUGCTL`：清除 bit 0（LBR）、bit 1（BTF）、bit 6（TR）

8. **返回值设置**：`RAX = Value & 0xFFFFFFFF`，`RDX = Value >> 32`

9. **RIP 前进**：`HvAdvanceGuestRip()`

**返回值**：始终返回 `TRUE`（表示 MSR 读取已在 Hypervisor 层处理完毕）。

### 3.8 `HandleWrmsrImpl`

```c
BOOLEAN HandleWrmsrImpl(PGUEST_CONTEXT GuestContext)
```

**功能**：处理 Guest 发起的 WRMSR 指令的 VM-Exit。

**核心逻辑流程**：

1. **提取参数**：`Msr = RCX`，`Value = (RAX & 0xFFFFFFFF) | ((RDX & 0xFFFFFFFF) << 32)`

2. **VMX/SVM 能力保护**（拦截 VMX/SVM 使能 MSR 的写入）：
   - `0x480 - 0x491`（VMX 能力 MSR）：注入 #GP(0)，这些 MSR 是只读的
   - `0x003A`（IA32_FEATURE_CONTROL）：注入 #GP(0)，阻止 VMXON 使能
   - `0xC0010114`（MSR_VM_CR）：注入 #GP(0)，阻止 SVM 使能
   - `0xC0010117`（MSR_VM_HSAVE_PA）：注入 #GP(0)

3. **Hyper-V 合成 MSR**（`0x40000000 - 0x400000FF`）：
   - 在裸机上，这些 MSR 不存在，写入它们会在真实硬件上 #GP
   - 注入 #GP(0) 以匹配裸机行为
   - Windows 不会访问这些 MSR（CPUID.1:ECX[31]=0），仅捕获第三方探测

4. **无效 MSR 预探测检查**：
   - `MsrIsKnownInvalid(Msr)` -> 注入 #GP(0)

5. **未知 MSR 安全网**：
   - 同 `HandleRdmsrImpl` 的逻辑，注入 #GP(0)
   - 频率限制的日志记录

6. **执行真实 WRMSR**：
   - `__writemsr(Msr, Value)`（无 SEH 保护）

7. **RIP 前进**：`HvAdvanceGuestRip()`

**返回值**：始终返回 `TRUE`。

---

## 4. 控制流与逻辑流程

### 4.1 RDMSR 处理流程

```
HandleRdmsrImpl(GuestContext)
 |
 +-- Msr = GuestContext->Rcx
 |
 +-- Msr 在 0x480-0x491 范围？
 |    +-- YES -> RAX=0, RDX=0, AdvanceRip -> 返回 TRUE（隐藏 VMX）
 |
 +-- Msr == 0x3A？
 |    +-- YES -> RAX=1(Locked, VMXON=0), RDX=0, AdvanceRip -> 返回 TRUE
 |
 +-- Msr == 0xC0010114？
 |    +-- YES -> __readmsr(0xC0010114), 设置 SVMDIS|LOCK, AdvanceRip -> 返回 TRUE
 |
 +-- Msr == 0xC0010117？
 |    +-- YES -> RAX=0, RDX=0, AdvanceRip -> 返回 TRUE
 |
 +-- Msr 在 0x40000000-0x400000FF 范围？
 |    +-- YES -> 注入 #GP(0), 设置指令长度 -> 返回 TRUE（裸机兼容）
 |
 +-- MsrIsKnownInvalid(Msr) == TRUE？
 |    +-- YES -> 注入 #GP(0), 设置指令长度 -> 返回 TRUE
 |
 +-- Msr 不在 (<=0x1FFF || >=0xC0000000 && <= C0001FFF) 范围？
 |    +-- YES -> 日志(限频), 注入 #GP(0) -> 返回 TRUE
 |
 +-- Value = __readmsr(Msr)  // 仅已知有效 MSR 到达此路径
 |
 +-- IsTargetProcess(GuestCr3) && Msr == IA32_DEBUGCTL && AAD_HIDE_DEBUGGER ?
 |    +-- YES -> Value &= ~0x43（清除 LBR/BTF/TR 位）
 |
 +-- RAX = Value & 0xFFFFFFFF, RDX = Value >> 32
 +-- HvAdvanceGuestRip()
 +-- 返回 TRUE
```

### 4.2 WRMSR 处理流程

```
HandleWrmsrImpl(GuestContext)
 |
 +-- Msr = RCX, Value = (RAX|RDX)
 |
 +-- Msr 在 0x480-0x491 或 Msr==0x3A 或 Msr==0xC0010114 或 Msr==0xC0010117？
 |    +-- YES -> 注入 #GP(0), 设置指令长度 -> 返回 TRUE
 |
 +-- Msr 在 0x40000000-0x400000FF 范围？
 |    +-- YES -> 注入 #GP(0), 设置指令长度 -> 返回 TRUE（裸机兼容）
 |
 +-- MsrIsKnownInvalid(Msr) == TRUE？
 |    +-- YES -> 注入 #GP(0) -> 返回 TRUE
 |
 +-- Msr 不在已知范围？
 |    +-- YES -> 日志(限频), 注入 #GP(0) -> 返回 TRUE
 |
 +-- __writemsr(Msr, Value)  // 仅已知有效 MSR 到达此路径
 +-- HvAdvanceGuestRip()
 +-- 返回 TRUE
```

### 4.3 MSR 预探测流程

```
MsrProbeInvalidMsrs()
 |
 +-- g_InvalidMsrBitmap == NULL ?
 |    +-- YES -> ExAllocatePoolWithTag(2KB, 'rsmI'), RtlZeroMemory
 |
 +-- for Msr = 0 to 0x1FFF:
 |    +-- __try { __readmsr(Msr) }
 |    +-- __except(EXCEPTION_EXECUTE_HANDLER) { MsrSetInvalid(Msr) }
 |
 +-- for Msr = 0xC0000000 to 0xC0001FFF:
 |    +-- __try { __readmsr(Msr) }
 |    +-- __except(EXCEPTION_EXECUTE_HANDLER) { MsrSetInvalid(Msr) }
 |
 +-- 返回 STATUS_SUCCESS
```

---

## 5. 与其他模块的交互

| 模块 | 交互方式 | 详细说明 |
|------|----------|----------|
| `vmx.h` | 常量依赖 | 使用 `MSR_IA32_DEBUGCTL` 等 MSR 常量 |
| `hv_ops.h` | 宏调用 | `HvReadGuestCr3()`、`HvAdvanceGuestRip()`、`HvInjectException()` 等 |
| `process.h` | 函数调用 | `IsTargetProcess()` 和 `IsFeatureEnabled()` 判断是否需要欺骗 MSR 值 |
| `vmx_init.c` / `svm_init.c` | 调用方 | 后端初始化时调用 `MsrBitmapInitialize()` 和 `MsrProbeInvalidMsrs()` |
| `vmx_exit.c` / `svm_exit.c` | 调用方 | VM-Exit/#VMEXIT 分发器调用 `HandleRdmsrImpl()` / `HandleWrmsrImpl()` |
| `vmxdrv.c` | 间接 | 卸载时调用 `MsrCleanupInvalidBitmap()` |

---

## 6. 关键设计要点

### 6.1 无效 MSR 预探测机制

这是本文件最重要的设计模式：

- **问题**：VMX Root 模式下 SEH 不可靠，但不能用 SEH 保护 MSR 访问
- **解决方案**：在 Root 模式前用 SEH 安全地预探测所有 MSR
- **关键洞察**：预探测只做一次（DriverEntry 阶段），代价可接受（2 × 8192 次 RDMSR）
- **安全前提**：RDMSR 对不存在 MSR 的 #GP 是确定性的（同一硬件上每次结果相同）

### 6.2 VMX/SVM 能力隐藏

通过拦截特定 MSR 实现虚拟化不可见性：

- **VMX 能力 MSR**（0x480-0x491）：全部返回 0
- **IA32_FEATURE_CONTROL**：返回 Lock=1, VMXON=0
- **AMD SVM 相关 MSR**：返回 SVMDIS=1, LOCK=1
- 配合 CPUID 隐藏策略，使 Guest 完全无法检测到 Hypervisor 的存在

### 6.3 Hyper-V 合成 MSR 拦截

裸机行为模拟：

- 范围 `0x40000000-0x400000FF` 的 MSR 在真实硬件上不存在
- RDMSR 和 WRMSR 均注入 #GP(0)，匹配裸机行为
- 在裸机上 CPUID.1:ECX[31]=0，Windows 不会查询这些 MSR
- 此处理程序仅捕获第三方探测工具对 Hyper-V 合成 MSR 的意外访问

### 6.4 反调试 MSR 欺骗

针对目标进程的 IA32_DEBUGCTL 读取：
- 清除 LBR（Last Branch Record, bit 0）、BTF（Branch Single-Step, bit 1）、TR（Trace Message, bit 6）
- 反调试软件通过这些位检测调试器存在时被欺骗

### 6.5 未知 MSR 的安全网

- 泄漏保护：最多只记录 20 次未知 MSR 访问，防止日志洪泛
- 注入 #GP 给 Guest，而不是执行可能导致 Triple Fault 的未知 `__readmsr`
- 使用 `static volatile LONG` 计数器配合 `InterlockedIncrement` 实现免锁频率限制

### 6.6 Windows 驱动框架注意点

- `__readmsr` 和 `__writemsr` 是 MSVC/x64 编译器内建，在 WDK 7600 中可用
- VMX Root 模式下 SEH 不可靠的原因：Host 栈不在线程内核栈上，`KiDispatchException` 遍历异常处理链时会读取零填充内存
- MSR 位图是 VMCS 的一部分，每个逻辑 CPU 独立，因此 `MsrBitmapInitialize` 需为每个 CPU 调用
- 位图默认全零（全部放行），仅拦截需要的 MSR 以最小化 VM-Exit 频率
