# Windows Hypervisor Platform (WHP) API 技术详解

## 概述

**Windows Hypervisor Platform (WHP)** 是微软在 Windows 10 RS4 (1803, build 17134) 引入的一套**用户态 API**，允许第三方应用程序直接使用 Windows 内置的 Hyper-V 虚拟化能力，**无需开发内核驱动**。

对比传统虚拟化方案：

| 方式 | 运行层级 | 需要驱动 | 需要签名 | 复杂度 |
|------|---------|:---:|:---:|------|
| VMXToolbox 裸机 VMXON | Ring 0 (内核驱动) | 是 (`.sys`) | 是 (EV证书) | 高 |
| VBS/Windows 内置 | Ring -1 (Hyper-V) | 是 (系统内置) | N/A | — |
| **WHP API** | **Ring 3 (用户态)** | **否** | **否** | **较低** |

## 架构

```
┌──────────────────────────────────────────┐
│         你的应用程序 (Ring 3)              │
│         QEMU / VirtualBox / WinVisor / 自定义 │
├──────────────────────────────────────────┤
│     Windows Hypervisor Platform          │  ← WinHvPlatform.h
│          WinHvApi.dll                     │     WinHvEmulation.h
├──────────────────────────────────────────┤
│         Hyper-V Hypervisor               │  ← Ring -1
│    hvix64.exe (Intel) / hvax64.exe (AMD)  │
├──────────────────────────────────────────┤
│          物理硬件 (VT-x / AMD-V)           │
└──────────────────────────────────────────┘
```

## 启用 WHP

### 前提条件

- Windows 10 RS4 (1803) 或更新版本
- 支持 VT-x + EPT / AMD-V + NPT 的 CPU（Intel 需要 Unrestricted Guest 支持）
- 不能同时使用其他独占 VT-x 的 Hypervisor（如 VMware）

### 启用方式

```powershell
# 方式一：DISM
Dism /Online /Enable-Feature /FeatureName:HypervisorPlatform /All

# 方式二：图形界面
# 控制面板 → 程序和功能 → 启用或关闭 Windows 功能 → Windows Hypervisor Platform
```

重启后生效。

## 核心 API 概览

### 头文件

```c
#include <WinHvPlatform.h>   // 核心 API
#include <WinHvEmulation.h>  // 指令模拟辅助
#pragma comment(lib, "WinHvPlatform.lib")
```

### 平台能力检测

```c
HRESULT WHvGetCapability(
    WHV_CAPABILITY_CODE CapabilityCode,
    PVOID               CapabilityBuffer,
    UINT32              CapabilityBufferSizeInBytes,
    PUINT32             WrittenSizeInBytes
);
```

查询的 CapabilityCode 包括：

| Code | 用途 |
|------|------|
| `WHvCapabilityCodeHypervisorPresent` | Hyper-V 是否正在运行 |
| `WHvCapabilityCodeFeatures` | CPU 虚拟化支持特性 |
| `WHvCapabilityCodeExtendedVmExits` | 支持的 VM-Exit 类型（CPUID/MSR/RDTSC 等） |
| `WHvCapabilityCodeExceptionExitBitmap` | 支持拦截的异常向量位图 |

### 分区 (Partition) 生命周期

分区等于一个虚拟机实例：

```c
// 1. 创建分区对象（此时还没在 Hypervisor 中真正创建）
HRESULT WHvCreatePartition(WHV_PARTITION_HANDLE *Partition);

// 2. 配置分区属性（必须在 Setup 之前完成）
HRESULT WHvSetPartitionProperty(
    WHV_PARTITION_HANDLE Partition,
    WHV_PARTITION_PROPERTY_CODE PropertyCode,
    PVOID                  PropertyBuffer,
    UINT32                 PropertyBufferSizeInBytes
);

// 3. 在 Hypervisor 中正式创建分区（属性锁定，不可再改）
HRESULT WHvSetupPartition(WHV_PARTITION_HANDLE Partition);

// 4. 销毁分区
HRESULT WHvDeletePartition(WHV_PARTITION_HANDLE Partition);
```

### 分区属性配置

#### ProcessorCount — 定义 vCPU 数

```c
WHV_PARTITION_PROPERTY prop;
prop.ProcessorCount = 2;  // 2 个虚拟处理器

WHvSetPartitionProperty(hPartition,
    WHvPartitionPropertyCodeProcessorCount,
    &prop, sizeof(prop));
```

#### ExtendedVmExits — 控制哪些操作触发 VM-Exit

```c
WHV_EXTENDED_VM_EXITS exits = {0};

// 启用 CPUID 拦截
exits.X64CpuidExit  = TRUE;
// 启用 RDMSR/WRMSR 拦截
exits.X64MsrExit    = TRUE;
// 启用 RDTSC/RDTSCP 拦截
exits.X64RdtscExit  = TRUE;
// 启用 MOV-DR 拦截（调试寄存器）
exits.X64DrExit     = TRUE;
// 启用异常拦截（需要配合 ExceptionExitBitmap）
exits.ExceptionExit = TRUE;

WHvSetPartitionProperty(hPartition,
    WHvPartitionPropertyCodeExtendedVmExits,
    &exits, sizeof(exits));
```

#### ExceptionExitBitmap — 选择拦截哪些异常向量

```c
WHV_EXCEPTION_EXIT_BITMAP bitmap;
bitmap.Bitmap = (1 << 1) | (1 << 3);  // 拦截 #DB (向量1) 和 #BP (向量3)

WHvSetPartitionProperty(hPartition,
    WHvPartitionPropertyCodeExceptionExitBitmap,
    &bitmap, sizeof(bitmap));
```

### 内存管理

WHP 使用进程虚拟内存作为 Guest 物理地址空间的存储：

```c
// 映射 GPA 范围：将 Host 用户态内存映射到 Guest 物理地址空间
HRESULT WHvMapGpaRange(
    WHV_PARTITION_HANDLE Partition,
    VOID                 *SourceAddress,      // Host 进程地址
    WHV_GUEST_PHYSICAL_ADDRESS GuestAddress,  // Guest 物理地址
    UINT64               SizeInBytes,         // 映射大小（必须 4KB 对齐）
    WHV_MAP_GPA_RANGE_FLAGS Flags            // 访问控制
);

// 解除映射（后续访问 → Memory Access Exit）
HRESULT WHvUnmapGpaRange(
    WHV_PARTITION_HANDLE Partition,
    WHV_GUEST_PHYSICAL_ADDRESS GuestAddress,
    UINT64               SizeInBytes
);

// GVA → GPA 地址翻译（软件页表遍历）
HRESULT WHvTranslateGva(
    WHV_PARTITION_HANDLE Partition,
    UINT32               VpIndex,
    WHV_GUEST_VIRTUAL_ADDRESS  GuestVa,
    WHV_TRANSLATE_GVA_FLAGS    TranslateFlags,
    WHV_TRANSLATE_GVA_RESULT   *TranslationResult,
    WHV_GUEST_PHYSICAL_ADDRESS *Gpa
);

// GPU range 2 — 映射到指定宿主进程的内存
HRESULT WHvMapGpaRange2(
    WHV_PARTITION_HANDLE Partition,
    HANDLE               HostProcess,         // 目标进程句柄
    VOID                 *SourceAddress,
    WHV_GUEST_PHYSICAL_ADDRESS GuestAddress,
    UINT64               SizeInBytes,
    WHV_MAP_GPA_RANGE_FLAGS Flags
);
```

Map 标志位：

| 标志 | 含义 |
|------|------|
| `WHvMapGpaRangeFlagNone` | 无特殊标志 |
| `WHvMapGpaRangeFlagRead` | 允许 Guest 读 |
| `WHvMapGpaRangeFlagWrite` | 允许 Guest 写 |
| `WHvMapGpaRangeFlagExecute` | 允许 Guest 执行 |
| `WHvMapGpaRangeFlagTrackDirtyPages` | 启用脏页追踪（用于实时迁移） |

### 虚拟处理器 (Virtual Processor) 生命周期

```c
// 创建 vCPU
HRESULT WHvCreateVirtualProcessor(
    WHV_PARTITION_HANDLE Partition,
    UINT32               VpIndex       // vCPU 索引（也是 APIC ID）
);

// 运行 vCPU — 阻塞调用，直到发生 VM-Exit
HRESULT WHvRunVirtualProcessor(
    WHV_PARTITION_HANDLE Partition,
    UINT32               VpIndex,
    VOID                 *ExitContext,         // [out] 退出上下文
    UINT32               ExitContextSizeInBytes
);

// 取消运行（从其他线程调用以中断阻塞的 Run）
HRESULT WHvCancelRunVirtualProcessor(
    WHV_PARTITION_HANDLE Partition,
    UINT32               VpIndex,
    UINT32               Flags
);

// 删除 vCPU
HRESULT WHvDeleteVirtualProcessor(
    WHV_PARTITION_HANDLE Partition,
    UINT32               VpIndex
);
```

### 虚拟处理器寄存器操作

```c
// 读取寄存器（可一次读多个）
HRESULT WHvGetVirtualProcessorRegisters(
    WHV_PARTITION_HANDLE Partition,
    UINT32               VpIndex,
    const WHV_REGISTER_NAME *RegisterNames,
    UINT32               RegisterCount,
    WHV_REGISTER_VALUE   *RegisterValues
);

// 写入寄存器
HRESULT WHvSetVirtualProcessorRegisters(
    WHV_PARTITION_HANDLE Partition,
    UINT32               VpIndex,
    const WHV_REGISTER_NAME *RegisterNames,
    UINT32               RegisterCount,
    const WHV_REGISTER_VALUE *RegisterValues
);
```

常用寄存器名称（x64）：

| 寄存器名 | 含义 |
|---------|------|
| `WHvX64RegisterRax` ~ `WHvX64RegisterR15` | 通用寄存器 |
| `WHvX64RegisterRip` | 指令指针 |
| `WHvX64RegisterRflags` | 标志寄存器 |
| `WHvX64RegisterCr0` / `Cr2` / `Cr3` / `Cr4` | 控制寄存器 |
| `WHvX64RegisterCs` / `SS` / `DS` / `ES` / `FS` / `GS` | 段寄存器 |
| `WHvX64RegisterIdtr` / `Gdtr` | 描述符表寄存器 |
| `WHvX64RegisterMsrEfer` | EFER (long mode 等) |
| `WHvX64RegisterMsrStar` / `Lstar` / `Cstar` / `Sfmask` | SYSCALL MSR |
| `WHvX64RegisterDr0` ~ `Dr7` | 调试寄存器 |
| `WHvX64RegisterXCr0` | XCR0 (XSAVE 特性) |
| `WHvX64RegisterTsc` | 时间戳计数器 |
| `WHvX64RegisterApicId` | APIC ID |

## VM-Exit 处理 — 核心运行循环

`WHvRunVirtualProcessor` 是**同步阻塞**调用。每次返回时携带一个退出原因，虚拟化栈必须处理并决定是否继续运行。

### Exit Reason 类型

```c
typedef enum WHV_RUN_VP_EXIT_REASON {
    WHvRunVpExitReasonNone              = 0,
    WHvRunVpExitReasonMemoryAccess      = 1,  // 访问了未映射/无权限的 GPA
    WHvRunVpExitReasonX64IoPortAccess   = 2,  // IN/OUT/INS/OUTS
    WHvRunVpExitReasonX64MsrAccess      = 3,  // RDMSR/WRMSR
    WHvRunVpExitReasonX64Cpuid          = 4,  // CPUID
    WHvRunVpExitReasonX64Halt           = 5,  // HLT
    WHvRunVpExitReasonException         = 6,  // 异常 (#PF/#GP 等)
    WHvRunVpExitReasonUnrecoverableException = 7,  // 三重错误
    WHvRunVpExitReasonX64InterruptWindow = 8, // 处理器可接收中断
    WHvRunVpExitReasonX64Rdtsc          = 9,  // RDTSC/RDTSCP
    WHvRunVpExitReasonUnsupportedFeature = 10, // 不支持的特性
    WHvRunVpExitReasonCanceled          = 11, // 被 WHvCancelRunVirtualProcessor 取消
} WHV_RUN_VP_EXIT_REASON;
```

### 退出上下文结构

#### MemoryAccess — 最核心的退出类型

```c
typedef struct WHV_MEMORY_ACCESS_CONTEXT {
    WHV_MEMORY_ACCESS_TYPE AccessType;     // Read / Write / Execute
    WHV_GUEST_PHYSICAL_ADDRESS Gpa;        // 触发的 Guest 物理地址
    UINT64                 Gva;            // 对应的 Guest 虚拟地址（如果有）
    UINT64                 InstructionLength;
    // 指令字节 (x64 最大 16 字节)：
    UINT8                  InstructionBytes[16];
    UINT8                  InstructionByteCount;
} WHV_MEMORY_ACCESS_CONTEXT;
```

#### IoPortAccess

```c
typedef struct WHV_X64_IO_PORT_ACCESS_CONTEXT {
    BYTE   Direction;        // 0=IN, 1=OUT
    BYTE   StringOp;         // 0=单次操作, 1=REP 前缀
    BYTE   OperandSize;      // 1/2/4 字节
    BYTE   AccessSize;       // 实际访问大小
    UINT16 PortNumber;       // I/O 端口号
    UINT64 Rax;              // 返回/写入的 RAX (bits [AccessSize*8-1:0])
    UINT64 Rcx;              // REP 计数器 (仅 StringOp=1)
    UINT64 Rsi;              // DS:RSI (仅 IN with REP)
    UINT64 Rdi;              // ES:RDI (仅 OUT with REP)
    UINT8  DsSegment;        // DS 段选择子
    UINT8  EsSegment;        // ES 段选择子
} WHV_X64_IO_PORT_ACCESS_CONTEXT;
```

#### MsrAccess

```c
typedef struct WHV_X64_MSR_ACCESS_CONTEXT {
    WHV_X64_MSR_ACCESS_TYPE AccessType;  // 0=RDMSR, 1=WRMSR
    UINT32 Msr;                           // MSR 索引
    UINT64 Rax;                           // RAX (WRMSR 的低 32 位, RDMSR 的返回值低 32)
    UINT64 Rdx;                           // RDX (WRMSR 的高 32 位, RDMSR 的返回值高 32)
} WHV_X64_MSR_ACCESS_CONTEXT;
```

#### CpuidAccess

```c
typedef struct WHV_X64_CPUID_ACCESS_CONTEXT {
    UINT32 Leaf;
    UINT32 SubLeaf;
    // 真实 CPUID 返回值：
    UINT32 DefaultResultEax;
    UINT32 DefaultResultEbx;
    UINT32 DefaultResultEcx;
    UINT32 DefaultResultEdx;
} WHV_X64_CPUID_ACCESS_CONTEXT;
```

> 注意：`DefaultResult*` 是 Hyper-V 计算的默认返回值（已在 L0 层面隐藏了 Hypervisor 位）。你可以在此基础上进一步修改再写入寄存器返回给 Guest。

#### VpException

```c
typedef struct WHV_VP_EXCEPTION_CONTEXT {
    BYTE  ExceptionVector;  // 异常向量号
    BYTE  ExceptionType;    // 0=Hardware exception, 1=Software exception
    BYTE  ErrorCodeValid;   // 是否有 Error Code
    UINT32 ErrorCode;
    BYTE  NestedException;  // 是否嵌套异常
    UINT64 ExceptionParameter;  // 额外参数 (如 #PF 的 CR2)
} WHV_VP_EXCEPTION_CONTEXT;
```

### 退出上下文包装结构

```c
typedef struct WHV_RUN_VP_EXIT_CONTEXT {
    WHV_RUN_VP_EXIT_REASON ExitReason;  // 退出原因枚举
    UINT32 Reserved;
    UINT64 InstructionLength;           // 导致退出的指令长度（用于 RIP 前进）
    union {
        WHV_MEMORY_ACCESS_CONTEXT    MemoryAccess;
        WHV_X64_IO_PORT_ACCESS_CONTEXT IoPortAccess;
        WHV_X64_MSR_ACCESS_CONTEXT   MsrAccess;
        WHV_X64_CPUID_ACCESS_CONTEXT CpuidAccess;
        WHV_VP_EXCEPTION_CONTEXT     VpException;
        WHV_X64_INTERRUPTION_DELIVERABLE_CONTEXT InterruptWindow;
        WHV_RUN_VP_CANCELLED_CONTEXT Canceled;
        WHV_X64_RDTSC_CONTEXT        RDTSC;
    };
} WHV_RUN_VP_EXIT_CONTEXT;
```

## 典型 vCPU 运行循环

```c
VOID VpRunLoop(WHV_PARTITION_HANDLE Partition, UINT32 VpIndex)
{
    HRESULT hr;
    WHV_RUN_VP_EXIT_CONTEXT ExitContext;
    WHV_REGISTER_NAME RegNames[1];
    WHV_REGISTER_VALUE RegValues[1];

    while (g_Running) {
        // 1. 执行 Guest 代码（阻塞）
        hr = WHvRunVirtualProcessor(Partition, VpIndex,
            &ExitContext, sizeof(ExitContext));
        if (FAILED(hr)) break;

        // 2. 根据退出原因分派处理
        switch (ExitContext.ExitReason) {

        case WHvRunVpExitReasonMemoryAccess:
            // 缺页 / 无权限访问 → 模拟 MMIO / 按需分配页面
            HandleMemoryAccess(Partition, VpIndex,
                &ExitContext.MemoryAccess);
            break;

        case WHvRunVpExitReasonX64IoPortAccess:
            // I/O 端口 → 模拟设备 I/O
            HandleIoPortAccess(Partition, VpIndex,
                &ExitContext.IoPortAccess,
                ExitContext.InstructionLength);
            break;

        case WHvRunVpExitReasonX64MsrAccess:
            // MSR R/W → 模拟或透传
            HandleMsrAccess(Partition, VpIndex,
                &ExitContext.MsrAccess,
                ExitContext.InstructionLength);
            break;

        case WHvRunVpExitReasonX64Cpuid:
            // CPUID → 修改返回值后写入寄存器
            HandleCpuidAccess(Partition, VpIndex,
                &ExitContext.CpuidAccess,
                ExitContext.InstructionLength);
            break;

        case WHvRunVpExitReasonX64Halt:
            // HLT → 等中断到达或直接返回
            break;

        case WHvRunVpExitReasonException:
            // 异常 → 注入到 Guest 或 handle triple-fault
            if (ExitContext.VpException.ExceptionVector == 14) {
                // #PF → 按需换页
            }
            break;

        case WHvRunVpExitReasonX64InterruptWindow:
            // 可以注入中断 → 注入挂起的中断
            InjectPendingInterrupts(Partition, VpIndex);
            break;

        case WHvRunVpExitReasonCanceled:
            // 正常的取消指令，继续下一循环
            break;

        case WHvRunVpExitReasonUnsupportedFeature:
        case WHvRunVpExitReasonUnrecoverableException:
        default:
            // 致命错误 → 终止 Guest
            g_Running = FALSE;
            break;
        }
    }
}
```

## 现实项目中的应用

### 1. QEMU — WHPX Accelerator

微软在 2017-2018 年向 QEMU 贡献了 WHPX 后端（`target/i386/whpx/`），使得 QEMU 在 Windows 上能获得接近原生性能：

```bash
qemu-system-x86_64 -accel whpx -m 4G disk.img
```

内部使用 `WHvCreatePartition` → `WHvSetupPartition` → 完整模拟 Guest x86 启动（从 16-bit real mode 到 64-bit long mode）。

### 2. WinVisor — 用户态进程沙箱

[x86matthew/WinVisor](https://github.com/x86matthew/WinVisor) 是一个基于 WHP 的 Windows x64 用户态**模拟器/沙箱**。其核心架构：

- 创建一个暂停的子进程，将整个地址空间通过 `WHvMapGpaRange` 映射到 Guest VM
- vCPU 以 CPL3（用户态）运行
- 通过将 `MSR_LSTAR` 设为保留地址（`0xFFFF800000000000`），使得 `SYSCALL` 时触发 MemoryAccess Exit → 模拟执行系统调用
- 按需分页——只映射固定数量页面，旧的 LRU 换出

### 3. Simpleator — 轻量级指令模拟器

Alex Ionescu 的 Simpleator 是 WHP 的另一个应用方向：单指令级别精确控制的 **指令追踪与模拟**。

### 4. Xenia (Xbox 360 模拟器)

Xenia 使用 WHP 作为后端的 CPU 虚拟化加速器，提升 Xbox 360 PPC 模拟的性能。

### 5. libwhp (Rust) / pywinhv (Python)

社区封装：

```
Rust:   https://github.com/alexpilotti/libwhp
Python: https://github.com/0vercl0k/pywinhv
```

## 与 VMXToolbox 的对比

| 维度 | VMXToolbox (内核驱动) | WHP API |
|------|---------------------|---------|
| **运行层级** | Ring 0 (内核) + Ring -1 (VMX root) | Ring 3 (用户态) |
| **需要内核驱动** | 是 | 否 |
| **需要代码签名** | 是 (EV 证书) | 否 |
| **VT-x 直接操控** | 是 (VMREAD/VMWRITE/VMCS) | 否 (Hyper-V 代理) |
| **EPT/NPT 直接操控** | 是 (手工构建页表) | 部分 (WHvMapGpaRange) |
| **Execute-Only 页面** | 是 (EPT R=0 W=0 X=1) | 否 (WHP 不支持) |
| **MSR Bitmap 精确控制** | 是 (逐 bit) | 有限 (全开/全关) |
| **VM-Exit 控制粒度** | 完整控制所有 VMCS/VMCB 字段 | 受限 (WHV_EXTENDED_VM_EXITS) |
| **BSOD 风险** | 高 (内核态) | 低 (用户态, 进程隔离) |
| **调试便利性** | 难 (需 WinDbg 双机) | 易 (VS 本地调试) |
| **反反调试能力** | 全 (Ring -1 无任何 OS 感知) | 受限 |
| **性能** | 近原生 | 略有开销 (额外系统调用) |

## WHP 的限制

### 1. 无法直接访问 VMCS/VMCB

WHP 的哲学是 Hyper-V 管理所有硬件细节，用户只通过 API 间接操控。这意味：
- 不能自定义 EPT 页表结构（如 Execute-Only page split）
- 不能使用 MTF 单步
- 不能自定义 VMCS 控制字段调整
- MSR 拦截粒度受限

### 2. 无 Execute-Only 页面

WHP 不支持 `R=0, W=0, X=1` 的 EPT 权限组合。最大粒度的 Hook 只能用 R+X 或 R+W 组合。

### 3. 不支持 16-bit Real Mode

vCPU 只能在 long mode (64-bit) / protected mode (32-bit) 下启动。无法模拟传统 BIOS POST 过程。

### 4. 内存限制

`WHvMapGpaRange` 只能映射调用进程的虚拟地址空间。跨进程访问需要通过 `MapGpaRange2` 单独解决。

### 5. 无法与内核态代码深度集成

如果想在 VM-Exit handler 中访问内核数据结构（如 EPROCESS、PEB），需要通过 IOCTL 与内核驱动通信，增加延迟。

## 典型使用场景

| 场景 | WHP 适用性 |
|------|-----------|
| 加速 QEMU/VirtualBox 等 Type-2 VMM | ✅ 最佳 |
| 构建用户态沙箱 (类似 WinVisor) | ✅ 适合理想 |
| CPU 指令级追踪/调试 | ✅ 适合理想 |
| 游戏机模拟器 (Xenia/RPCS3) | ✅ 适用于 CPU 加速 |
| 反反调试/反作弊 | ⚠️ 部分能力 (Execute-Only 不支持) |
| 内核 Hook 绕过 PatchGuard | ❌ 无法 (受 Hyper-V 限制) |
| 透明进程内存读写 | ⚠️ 可通过映射目标进程地址空间实现 |
| SSDT/Shadow SSDT 监控 | ❌ 无法直接访问内核内存 |
| Type-1 Hypervisor (独立 VMM) | ❌ 无法 — WHP 需要运行在 Hyper-V 之上 |

## 总结

WHP API 是微软对"虚拟化能力民主化"的回应——允许第三方**在用户态开发**自己的 Hypervisor，无需内核驱动开发、签名、调试的复杂度。

但它是一个**经过抽象和限制**的 API。Hyper-V 是真正占有硬件 VMX root 模式的实体，WHP 只是提供了一个受控的、简化的接口。对于需要完全控制 VT-x/SVM 的应用场景（如 VMXToolbox 的反反调试、EPT Execute-Only Hook），**内核驱动仍然无法替代**。

---

> **参考来源**
>
> - [Microsoft Docs: Windows Hypervisor Platform API Definitions](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/hypervisor-platform)
> - [WinVisor — WHP-based x64 user-mode emulator](https://github.com/x86matthew/WinVisor)
> - [QEMU WHPX Accelerator](https://www.qemu.org/docs/master/system/whpx.html)
> - [libwhp — Rust bindings for WHP](https://github.com/alexpilotti/libwhp)
> - [pywinhv — Python bindings for WHP](https://github.com/0vercl0k/pywinhv)
> - [Intel SDM Vol.3C — VMX operation / nested VMCS / VMCS shadowing](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
> - [Broadcom KB: Nested VT-x/EPT support policy](https://knowledge.broadcom.com/external/article/389469)
