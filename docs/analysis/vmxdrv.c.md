# vmxdrv.c -- 逻辑分析

## 1. 文件概述

### 角色与职责

`vmxdrv.c` 是 VMX Hypervisor Toolbox 驱动的主入口点和中枢调度模块。它承担以下核心职责：

- **驱动生命周期管理**：`DriverEntry` 和 `DriverUnload` 负责驱动加载与卸载的全流程初始化与清理。
- **IOCTL 请求调度**：集中接收所有来自用户态 `VMXToolbox.exe` 的 `DeviceIoControl` 调用，按功能码分发到对应处理函数。
- **VMCALL 关机认证**：实现 M-6 安全机制，确保只有经过认证的 Ring-0 Guest 代码能触发 VMCALL 关机序列。
- **进程内存访问引擎**：通过 CR3 页表遍历 + 物理内存直接映射（`MmMapIoSpace`），实现绕过所有操作系统保护的内存读写。
- **全局状态管理**：维护 `g_VmxState`、`g_DeviceObject`、`g_HvOps` 等核心全局变量。

### 依赖的其他模块

| 头文件 | 依赖模块 | 用途 |
|--------|----------|------|
| `vmx.h` | Intel VMX 后端 | VMX 状态结构、MSR 定义 |
| `svm.h` | AMD SVM 后端 | SVM 状态结构 |
| `hv_ops.h` | 虚拟化抽象层 | `g_HvOps` vtable 接口 |
| `hv_detect.h` | CPU 检测 | CPU 厂商检测函数 |
| `hv_mem.h` | 内存引擎 | 页表遍历辅助 |
| `hv_hook.h` | EPT/NPT Hook 框架 | 通用 Hook 安装/移除/查询 |
| `ssdt.h` | SSDT 监控框架 | SSDT 初始化/挂钩/转储 |
| `shadow_ssdt.h` | Shadow SSDT 框架 | Win32k 系统调用监控 |
| `log.h` | 日志系统 | 无锁环形缓冲区日志 |
| `process.h` | 进程跟踪 | 目标进程 CR3 跟踪 |
| `shared.h` | 用户-内核共享定义 | IOCTL 码、数据结构 |

---

## 2. 数据结构

### 全局变量

#### `VMX_STATE g_VmxState`
- **类型**：`VMX_STATE`（定义于 `vmx.h`）
- **作用**：Intel VMX 后端的全局状态，包含 `Initialized`、`CpuCount`、`CpuContexts[]` 等字段。
- **初始化**：在 `DriverEntry` 中零初始化（由于是全局变量，编译时已零初始化）。

#### `PDEVICE_OBJECT g_DeviceObject`
- **类型**：`PDEVICE_OBJECT`
- **作用**：指向创建的驱动设备对象的指针，用于 `IoDeleteDevice` 卸载清理。

#### `ULONG64 g_VmcallShutdownNonce`
- **类型**：`ULONG64`
- **作用**：每启动一次随机生成的 64 位 nonce，用于 VMCALL 关机认证。
- **生成**：混合 `KeQueryPerformanceCounter`、`__rdtsc`、`KeQueryInterruptTime`，经 Murmur 哈希风格混合器处理。
- **安全要求**：该值永不离开内核模块，仅在内核内存中保存。

#### `ULONG g_MaxProcessors`
- **类型**：`ULONG`
- **作用**：动态检测的活跃处理器数量，替代旧的 `MAX_PROCESSORS` 常量。用于所有 per-CPU 分配的容量计算。

#### `PFN_KeSetTargetProcessorDpcEx g_pfnKeSetTargetProcessorDpcEx`
- **类型**：函数指针
- **作用**：动态解析 `KeSetTargetProcessorDpcEx`，支持超过 128 CPU 的系统。在 Windows 7 以下系统为 NULL，回退到 `KeSetTargetProcessorDpc`（CCHAR 限制 0-127）。

#### `PHV_OPS g_HvOps`
- **类型**：`PHV_OPS`
- **作用**：指向当前激活的虚拟化后端的 vtable 指针。在 `DriverEntry` 中根据 CPU 厂商设定为 `g_VmxOps` 或 `g_SvmOps`。

#### `CPU_VENDOR g_CpuVendor`
- **类型**：`CPU_VENDOR` 枚举
- **作用**：记录检测到的 CPU 厂商（`CPU_VENDOR_UNKNOWN` / `CPU_VENDOR_INTEL` / `CPU_VENDOR_AMD`）。

---

## 3. 核心函数详解

### 3.1 `HvIsAuthenticShutdownCaller`

```c
BOOLEAN HvIsAuthenticShutdownCaller(
    ULONG64 GuestRcx,
    ULONG64 GuestRip,
    ULONG   GuestCpl,
    ULONG64 GuestEfer,
    BOOLEAN GuestCsL
)
```

**功能**：M-6 关机 VMCALL 的多因素认证检查，确保只有合法、经过认证的 Ring-0 代码能触发关机。

**参数说明**：

| 参数 | 来源 | 意义 |
|------|------|------|
| `GuestRcx` | VMCALL 参数 | 调用方提供的 nonce，须匹配 `g_VmcallShutdownNonce` |
| `GuestRip` | VMCS/VMCB | 调用方的 RIP，须在内核地址空间 |
| `GuestCpl` | VMCS/VMCB | 当前特权级，须为 0 |
| `GuestEfer` | VMCS/VMCB | EFER 寄存器，须设置 LMA（长模式激活） |
| `GuestCsL` | VMCS/VMCB | 代码段长模式标志，须为 1（64 位代码段） |

**返回值**：`TRUE` = 认证通过，`FALSE` = 拒绝。

**核心逻辑流程**：
1. 检查 `GuestRcx == g_VmcallShutdownNonce`（nonce 匹配）
2. 检查 `EFER.LMA` 位已设置（长模式）
3. 检查 `CS.L` 位已设置（64 位代码段，非兼容模式）
4. 检查 `CPL == 0`（内核 Ring）
5. 检查 `RIP >= 0xFFFF800000000000`（内核态地址空间上半部）
6. 所有条件均通过返回 `TRUE`，任一不通过返回 `FALSE`

**设计要点**：
- 五重检查防止各类绕过攻击：32 位兼容模式攻击者、Ring-0 ROP gadget（用户页具有 Ring 0 权限）、恶意驱动加载等。
- 每个检查独立返回 `FALSE`，无副作用，无信息泄露。

---

### 3.2 `DriverEntry`

```c
NTSTATUS DriverEntry(
    PDRIVER_OBJECT     DriverObject,
    PUNICODE_STRING    RegistryPath
)
```

**功能**：驱动入口点，按严格顺序完成所有初始化。

**参数说明**：

| 参数 | 意义 |
|------|------|
| `DriverObject` | WDK 提供的驱动对象，用于注册分发函数 |
| `RegistryPath` | 注册表路径（本驱动未使用） |

**返回值**：`STATUS_SUCCESS` 或错误码。

**核心逻辑流程（严格的初始化顺序）**：

1. **日志系统初始化**
   - `LogInitialize()` -- 初始化无锁环形缓冲区
   - `LogFlushThreadStart()` -- 启动系统线程，将环形缓冲区内容刷出到 WinDbg

2. **处理器计数**
   - `KeQueryActiveProcessorCount(NULL)` -- 获取活跃处理器数，存入 `g_MaxProcessors`

3. **VMCALL 关机 Nonce 生成**
   - 三源熵池混合：`KeQueryPerformanceCounter` + `__rdtsc` + `KeQueryInterruptTime`
   - Murmur 风格 finalizer：两次移位异或 + 两次乘常数
   - 零值保护：若结果为零则设为固定值 `0xA5A5A5A5A5A5A5A5`

4. **动态函数解析**
   - `MmGetSystemRoutineAddress` 解析 `KeSetTargetProcessorDpcEx`
   - 支持 >128 CPU 系统，不可用时静默回退

5. **CPU 厂商检测与后端选择**
   - `HvDetectCpuVendor()` 检测 CPU 厂商
   - Intel：`HvCheckVmxSupport()` 确认 VMX 支持，设 `g_HvOps = &g_VmxOps`
   - AMD：`HvCheckSvmSupport()` 确认 SVM 支持，设 `g_HvOps = &g_SvmOps`
   - 未知：返回 `STATUS_NOT_SUPPORTED`，驱动加载失败

6. **设备对象创建**
   - `IoCreateDevice` 创建设备 `\Device\VMXToolbox`
   - `IoCreateSymbolicLink` 创建符号链接 `\DosDevices\VMXToolbox`

7. **分发函数注册**
   - `IRP_MJ_CREATE` / `IRP_MJ_CLOSE` -> `DispatchCreateClose`
   - `IRP_MJ_DEVICE_CONTROL` -> `DispatchDeviceControl`
   - `DriverUnload` -> `DriverUnload`

8. **子系统初始化**
   - `ProcessTrackingInit()` -- 进程跟踪子系统
   - `GenericHookInit()` -- 通用 Hook 框架

**错误处理路径**：
- 任一检查失败时，必须回滚已成功的步骤
- CPU 不支持时：调用 `LogTerminate()`，返回 `STATUS_NOT_SUPPORTED`
- `IoCreateDevice` 失败：`LogTerminate()`，返回错误码
- `IoCreateSymbolicLink` 失败：删除设备 + `LogTerminate()`，返回错误码

---

### 3.3 `DriverUnload`

```c
VOID DriverUnload(PDRIVER_OBJECT DriverObject)
```

**功能**：驱动卸载的清理入口，按初始化的逆序执行。

**核心逻辑流程**：

1. **日志记录** `"Driver unloading..."`
2. **Shadow SSDT 清理**：`ShadowSsdtCleanup()` -- 先于 SSDT 清理
3. **SSDT 清理**：`SsdtCleanup()` -- 先于通用 Hook 清理
4. **通用 Hook 清理**：`GenericHookCleanup()`
5. **虚拟化终止**：`g_HvOps->Terminate()` -- 在所有处理器上退出 VMX/SVM
6. **进程跟踪清理**：`ProcessTrackingCleanup()`
7. **符号链接与设备删除**：`IoDeleteSymbolicLink` + `IoDeleteDevice`
8. **日志线程终止**：`LogFlushThreadStop()` + `LogTerminate()`

**设计要点**：
- 严格按照依赖关系的逆序清理（Shadow SSDT -> SSDT -> Generic Hook -> Hypervisor -> ...）
- `Terminate()` 仅在对应后端已初始化时调用

---

### 3.4 `DispatchCreateClose`

```c
NTSTATUS DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
```

**功能**：处理 IRP_MJ_CREATE 和 IRP_MJ_CLOSE，简单返回成功。

**逻辑**：设 `IoStatus.Status = STATUS_SUCCESS`，`IoStatus.Information = 0`，完成 IRP。

---

### 3.5 `DispatchDeviceControl`

```c
NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
```

**功能**：所有 IOCTL 请求的主调度入口。根据 `IoControlCode` 分发到对应的 `HandleIoctl*` 函数。

**核心逻辑流程**：

1. 通过 `IoGetCurrentIrpStackLocation` 获取 IO 栈位置
2. 提取 `IoControlCode`
3. **BUG FIX**：将 `Irp->IoStatus.Information` 初始化为 0，防止失败路径向用户态返回垃圾数据
4. 根据 `IoControlCode` 进入 `switch` 语句分发
5. 调用对应处理函数
6. 设置 `Irp->IoStatus.Status`
7. `IoCompleteRequest(Irp, IO_NO_INCREMENT)` 完成 IRP

**IOCTL 代码映射表**：

| IOCTL 码 | Handler 函数 | 功能组 |
|----------|-------------|--------|
| `0x800` | `HandleIoctlInit` | 生命周期 |
| `0x801` | `HandleIoctlSetTarget` | 目标进程 |
| `0x802` | `HandleIoctlRemoveTarget` | 目标进程 |
| `0x803` | `HandleIoctlSetConfig` | 目标进程 |
| `0x804` | `HandleIoctlGetLog` | 诊断 |
| `0x805` | `HandleIoctlStop` | 生命周期 |
| `0x806` | `HandleIoctlQueryStatus` | 诊断 |
| `0x807` | `HandleIoctlReadMemory` | 内存访问 |
| `0x808` | `HandleIoctlWriteMemory` | 内存访问 |
| `0x809` | `HandleIoctlInstallHook` | Hook 框架 |
| `0x80A` | `HandleIoctlRemoveHook` | Hook 框架 |
| `0x80B` | `HandleIoctlListHooks` | Hook 框架 |
| `0x80C` | `HandleIoctlGetHookEvents` | Hook 框架 |
| `0x80D` | `HandleIoctlSsdtInit` | SSDT |
| `0x80E` | `HandleIoctlSsdtDump` | SSDT |
| `0x80F` | `HandleIoctlSsdtHook` | SSDT |
| `0x810` | `HandleIoctlSsdtUnhook` | SSDT |
| `0x811` | `HandleIoctlSsdtUnhookAll` | SSDT |
| `0x812` | `HandleIoctlSsdtListHooks` | SSDT |
| `0x813` | `HandleIoctlSsdtMonitor` | SSDT |
| `0x814` | `HandleIoctlShadowSsdtInit` | Shadow SSDT |
| `0x815` | `HandleIoctlShadowSsdtDump` | Shadow SSDT |
| `0x816` | `HandleIoctlShadowSsdtHook` | Shadow SSDT |
| `0x817` | `HandleIoctlShadowSsdtUnhook` | Shadow SSDT |
| `0x818` | `HandleIoctlShadowSsdtUnhookAll` | Shadow SSDT |
| `0x819` | `HandleIoctlShadowSsdtListHooks` | Shadow SSDT |
| `0x81A` | `HandleIoctlShadowSsdtMonitor` | Shadow SSDT |

---

### 3.6 `HandleIoctlInit`

**功能**：处理 IOCTL_VMX_INIT，初始化虚拟化后端。

**核心逻辑**：
1. 检查 `g_HvOps` 非空
2. 检查是否已初始化（避免重复初始化）
3. 调用 `g_HvOps->Initialize()` 在所有处理器上启动虚拟化

---

### 3.7 `HandleIoctlSetTarget` / `HandleIoctlRemoveTarget` / `HandleIoctlSetConfig`

**功能**：管理受保护的目标进程。

- `HandleIoctlSetTarget`：调用 `ProcessAddTarget(Pid, Flags)` 添加目标进程
- `HandleIoctlRemoveTarget`：调用 `ProcessRemoveTarget(Pid)` 移除目标进程
- `HandleIoctlSetConfig`：调用 `ProcessUpdateConfig(Pid, Flags)` 更新目标进程的 AAD 标志

**输入验证**：检查 `InputBufferLength >= sizeof(XXX)`，`SystemBuffer` 非空。

---

### 3.8 `HandleIoctlGetLog`

**功能**：从无锁环形缓冲区读取日志条目。

**流程**：
1. 计算输出缓冲区能容纳的最大条目数：`(OutputSize - sizeof(VMX_LOG_BUFFER 头)) / sizeof(VMX_LOG_ENTRY)`
2. 调用 `LogRead()` 从环形缓冲区读取
3. 设置 `Count` 字段和 `Information`（实际数据大小）

---

### 3.9 `HandleIoctlStop`

**功能**：停止虚拟化。

**流程**：检查后端初始化状态，调用 `g_HvOps->Terminate()`。

---

### 3.10 `HandleIoctlQueryStatus`

**功能**：查询驱动状态。

**输出字段**：
- `VmxActive`：虚拟化是否激活
- `ActiveTargets`：活跃目标进程数（`ProcessGetActiveCount()`）
- `CpuCount`：虚拟化的 CPU 数
- `TotalExits`：所有 CPU 的 VM-Exit 总数累积

---

### 3.11 `KernelGuestVaToPa`

```c
static ULONG64 KernelGuestVaToPa(ULONG64 GuestCr3, ULONG64 VirtualAddress)
```

**功能**：在内核模式下手动遍历目标进程的四级页表，将虚拟地址转换为物理地址。

**流程**（四级页表遍历）：
1. **PML4**（Level 4）：`GuestCr3 & 0x000FFFFFFFFFF000` 为 PML4 基址，按 VA[47:39] 索引
2. **PDPT**（Level 3）：按 VA[38:30] 索引，检查 1GB 大页位（bit 7）
3. **PD**（Level 2）：按 VA[29:21] 索引，检查 2MB 大页位（bit 7）
4. **PT**（Level 1）：按 VA[20:12] 索引
5. 最终物理地址 = 页帧号（`Entry & 0x000FFFFFFFFFF000`）+ 页内偏移（`VA & 0xFFF`）

**关键设计**：
- 使用 `MmMapIoSpace`/`MmUnmapIoSpace` 映射物理内存页，而非常规虚拟地址解引用
- 每级页表项都检查 Present 位（bit 0）
- 支持 1GB 和 2MB 大页的直接处理
- 映射使用 `MmNonCached` 缓存类型

---

### 3.12 `KernelCopyProcessMemory`

```c
static NTSTATUS KernelCopyProcessMemory(
    ULONG64     TargetCr3,
    ULONG64     TargetVa,
    PVOID       KernelBuffer,
    ULONG       Size,
    BOOLEAN     IsRead
)
```

**功能**：在目标进程物理内存和内核缓冲区之间复制数据。

**流程**：
1. 循环处理所有字节，逐页复制
2. 对每个地址调用 `KernelGuestVaToPa` 获取物理地址
3. 处理跨页边界：每次复制不超过页剩余大小（`0x1000 - PageOffset`）
4. `MmMapIoSpace` 映射物理页
5. `RtlCopyMemory` 在 `__try/__except` 保护下复制数据
6. `MmUnmapIoSpace` 解除映射

**参数**：
- `IsRead = TRUE`：从目标进程读入内核缓冲区
- `IsRead = FALSE`：从内核缓冲区写入目标进程

**错误处理**：
- 页表遍历失败（`TargetPa == 0`）返回 `STATUS_INVALID_ADDRESS`
- `MmMapIoSpace` 失败返回 `STATUS_INSUFFICIENT_RESOURCES`
- SEH 异常捕获返回 `STATUS_ACCESS_VIOLATION`

---

### 3.13 `HandleIoctlReadMemory` / `HandleIoctlWriteMemory`

**功能**：实现不可检测的目标进程内存读写。

**读内存流程**：
1. 输入验证：检查 `VMX_MEMORY_REQUEST` 头完整性、`Size`（<= 64KB）、`VirtualAddress` 非零
2. `ResolvePidToCr3` 通过 `PsLookupProcessByProcessId` + EPROCESS 偏移量获取目标 CR3
3. `KernelCopyProcessMemory(..., IsRead=TRUE)` 执行实际读取

**写内存流程**：
1. 输入验证与读类似
2. 数据载荷紧跟在 `VMX_MEMORY_REQUEST` 头之后
3. `KernelCopyProcessMemory(..., IsRead=FALSE)` 执行实际写入

**不可检测性设计**：
- 不调用 `OpenProcess` / `NtReadVirtualMemory`
- 不调用 `KeStackAttachProcess`
- 不调用 `MmCopyVirtualMemory`
- 直接通过 `MmMapIoSpace` 访问物理内存

---

### 3.14 IOCTL 处理函数组

以下函数均为 IOCTL 的薄封装，主要执行输入验证后调用对应模块的函数：

**通用 Hook 框架**：
- `HandleIoctlInstallHook`：按名称或地址解析目标，调用 `GenericHookInstall`
- `HandleIoctlRemoveHook`：按 HookId 调用 `GenericHookRemove`
- `HandleIoctlListHooks`：遍历 Hook 链表，填充输出缓冲区
- `HandleIoctlGetHookEvents`：调用 `HookLogRead` 读取 Hook 事件日志

**SSDT 框架**：
- `HandleIoctlSsdtInit`：调用 `SsdtInitialize`，返回 `KiServiceTableVa`、`KiSystemCall64Va`
- `HandleIoctlSsdtDump`：调用 `SsdtDumpTable`，带分页支持（`StartIndex`/`Count`）
- `HandleIoctlSsdtHook`：按名称或索引调用 `SsdtHookByName`/`SsdtHookByIndex`
- `HandleIoctlSsdtUnhook`：按 HookId 或索引调用 `SsdtUnhookByHookId`/`SsdtUnhookByIndex`
- `HandleIoctlSsdtListHooks`：`KeAcquireSpinLock` 保护下遍历 `g_SsdtState.HookListHead`

**Shadow SSDT 框架**：与 SSDT 框架完全对称，但操作 `g_ShadowSsdtState` 和对应的 Shadow SSDT 函数。

---

## 4. 控制流与逻辑流程

### 4.1 驱动初始化流程

```
DriverEntry
 |
 +-- LogInitialize()
 +-- LogFlushThreadStart()
 +-- KeQueryActiveProcessorCount()
 +-- [生成 g_VmcallShutdownNonce]
 +-- [解析 KeSetTargetProcessorDpcEx]
 +-- HvDetectCpuVendor()
 |    |
 |    +-- CPU_VENDOR_INTEL
 |    |    +-- HvCheckVmxSupport()
 |    |    +-- g_HvOps = &g_VmxOps
 |    |
 |    +-- CPU_VENDOR_AMD
 |    |    +-- HvCheckSvmSupport()
 |    |    +-- g_HvOps = &g_SvmOps
 |    |
 |    +-- CPU_VENDOR_UNKNOWN
 |         +-- 返回 STATUS_NOT_SUPPORTED
 |
 +-- IoCreateDevice()
 +-- IoCreateSymbolicLink()
 +-- [注册 MajorFunction]
 +-- ProcessTrackingInit()
 +-- GenericHookInit()
 +-- 返回 STATUS_SUCCESS
```

### 4.2 IOCTL 请求处理流程

```
用户态 DeviceIoControl()
  |
  v
DispatchDeviceControl()
  |
  +-- 初始化 Irp->IoStatus.Information = 0
  +-- switch(IoControlCode) {
  |       0x800 -> HandleIoctlInit()
  |       0x801 -> HandleIoctlSetTarget()
  |       0x807 -> HandleIoctlReadMemory()
  |       ...   -> ...
  |       default -> STATUS_INVALID_DEVICE_REQUEST
  +-- }
  +-- Irp->IoStatus.Status = Status
  +-- IoCompleteRequest()
```

### 4.3 内存读取流程

```
HandleIoctlReadMemory
 |
 +-- 验证输入 VMX_MEMORY_REQUEST
 +-- ResolvePidToCr3(Pid)
 |    +-- PsLookupProcessByProcessId
 |    +-- *(EPROCESS + DirectoryTableBase 偏移)
 |    +-- ObDereferenceObject
 |
 +-- KernelCopyProcessMemory(TargetCr3, Va, Buffer, Size, TRUE)
      |
      +-- 循环（逐页处理）
           +-- KernelGuestVaToPa(Cr3, Va + BytesDone)
           |    +-- PML4 -> PDPT -> PD -> PT 四级遍历
           |    +-- MmMapIoSpace / MmUnmapIoSpace
           |
           +-- MmMapIoSpace(物理页)
           +-- RtlCopyMemory（SEH 保护）
           +-- MmUnmapIoSpace
```

### 4.4 驱动卸载流程

```
DriverUnload
 |
 +-- ShadowSsdtCleanup()
 +-- SsdtCleanup()
 +-- GenericHookCleanup()
 +-- g_HvOps->Terminate()
 +-- ProcessTrackingCleanup()
 +-- IoDeleteSymbolicLink()
 +-- IoDeleteDevice()
 +-- LogFlushThreadStop()
 +-- LogTerminate()
```

---

## 5. 与其他模块的交互

| 模块 | 交互方式 | 详细说明 |
|------|----------|----------|
| `hv_ops.h` | vtable 调用 | 通过 `g_HvOps->Initialize()`、`g_HvOps->Terminate()` 调用后端 |
| `process.c/h` | 函数调用 | `ProcessAddTarget/RemoveTarget/UpdateConfig/GetActiveCount` |
| `hv_hook.c/h` | 函数调用 | `GenericHookInit/Cleanup/Install/Remove/GetInfo` |
| `ssdt.c/h` | 函数调用 | `SsdtInitialize/DumpTable/HookByName/HookByIndex/UnhookAll/SetMonitorMode` |
| `shadow_ssdt.c/h` | 函数调用 | 与 SSDT 对称的 Shadow 操作 |
| `log.c/h` | 函数调用 | `LogInitialize/Terminate/Write/Read` + 刷出线程控制 |
| `hv_detect.c/h` | 函数调用 | `HvDetectCpuVendor/HvCheckVmxSupport/HvCheckSvmSupport` |
| `common/shared.h` | 数据结构 | IOCTL 码、`VMX_TARGET_INFO`、`VMX_MEMORY_REQUEST` 等共享结构 |

---

## 6. 关键设计要点

### 6.1 M-6 VMCALL 关机安全认证

- 五重检查机制防止各类绕过
- 使用每启动一次的随机 nonce（三源熵池 + Murmur finalizer）
- 代码段长度检查（CS.L）防止 32 位兼容模式攻击
- 内核地址空间检查（RIP >= 0xFFFF800000000000）防止 Ring-0 ROP gadget 攻击

### 6.2 不可检测内存访问

- 页表遍历 + 物理内存直接映射（`MmMapIoSpace`）
- 完全不调用标准 Windows 内存访问 API
- 对所有反作弊/保护软件完全透明
- 最大单次传输 64KB（`VMX_MEM_MAX_SIZE`）

### 6.3 IOCTL Information 初始化 BUG FIX

- 修复前失败路径可能返回未初始化的 `Information` 值
- `METHOD_BUFFERED` 模式下 `Information` 决定复制到用户态的字节数
- 未初始化值会导致向用户态泄露内核内存内容

### 6.4 KeSetTargetProcessorDpcEx 动态解析

- 支持超过 128 CPU 的大型系统
- Windows 7 以下版本不可用时透明回退
- 封装在 `HvSetTargetProcessorDpc()` 内联函数中

### 6.5 严格初始化顺序

```
Log -> CPU检测 -> 设备创建 -> 分发注册 -> 子系统初始化
```

每个步骤都可能失败，失败时需回滚已成功的步骤。卸载按严格逆序执行。

### 6.6 Windows 驱动框架注意点

- `DRIVER_INITIALIZE` / `DRIVER_UNLOAD` 宏标记标准入口点
- `IoGetCurrentIrpStackLocation` 获取 I/O 栈位置
- `METHOD_BUFFERED` 使用 `Irp->AssociatedIrp.SystemBuffer` 访问缓冲区
- `IoCompleteRequest` 必须在每个 IRP 处理路径中调用
- `PsLookupProcessByProcessId` 使用后必须 `ObDereferenceObject`
- `PAGE_SIZE_4KB` 常量用于 `RtlZeroMemory` 初始化 MSR 位图
