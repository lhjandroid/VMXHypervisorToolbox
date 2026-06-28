# hv_mem.c / hv_mem.h — 逻辑分析

## 1. 文件概述

### 角色与职责

`hv_mem.c` 和 `hv_mem.h` 实现了**Guest进程内存的读/写引擎**，通过手动遍历Guest的CR3页表实现虚拟地址到物理地址的转换。该模块的原本设计目标是：

- 绕过Guest操作系统所有内存访问保护机制
- 不触发 `ObRegisterCallbacks`（不需要打开句柄）
- 不涉及 `NtReadVirtualMemory` 钩子（不调用任何API）
- 不需要 `KeStackAttachProcess`（没有上下文切换）
- 绕过任何驱动级别的内存访问监控

**当前状态（M-7审查后）**：由于VMX根模式下直接通过物理地址强制转换访问内存存在严重安全问题（VMX根模式下 `(PVOID)PhysicalAddress` 是错误的——CPU仍使用Host CR3页表进行地址翻译，且SEH在根模式下不可靠），该模块的大部分功能已被禁用。

### 仍然保留的功能

| 函数 | 状态 | 说明 |
|------|------|------|
| `HvGuestVaToPa()` | **可用** | Guest虚拟地址→物理地址转换，带有IRQL上限检查（≤ DISPATCH_LEVEL） |
| `HvReadGuestMemory()` | **已禁用** | 返回 `STATUS_NOT_SUPPORTED` |
| `HvWriteGuestMemory()` | **已禁用** | 返回 `STATUS_NOT_SUPPORTED` |
| `HvHandleMemoryVmcall()` | **已禁用** | 空操作（仅推进Guest RIP） |

### 依赖的其他模块

| 模块 | 文件 | 依赖关系 |
|------|------|----------|
| 抽象层 | `hv_ops.h` | 通过 `HvAdvanceGuestRip()` 宏操作 |
| VMX定义 | `vmx.h` | - |
| 日志 | `log.h` | 使用 `LOG_WARN` 输出禁用路径的警告 |
| 进程追踪 | `process.h` | （预留，当前路径不使用） |

---

## 2. 数据结构

### 2.1 页表遍历常量

```c
#define PAGE_PRESENT        (1ULL << 0)     // 页表项存在位
#define PAGE_LARGE          (1ULL << 7)     // 大页标志（PS位：2MB或1GB页）
#define PAGE_ADDR_MASK_4K   0x000FFFFFFFFFF000ULL   // 4KB页物理地址掩码 (bits 51:12)
#define PAGE_ADDR_MASK_2M   0x000FFFFFFFE00000ULL   // 2MB页物理地址掩码 (bits 51:21)
#define PAGE_ADDR_MASK_1G   0x000FFFFFC0000000ULL   // 1GB页物理地址掩码 (bits 51:30)
```

页内偏移量计算：
```c
#define PAGE_OFFSET_4K(va)  ((va) & 0xFFF)         // 4KB页偏移 (12位)
#define PAGE_OFFSET_2M(va)  ((va) & 0x1FFFFF)      // 2MB页偏移 (21位)
#define PAGE_OFFSET_1G(va)  ((va) & 0x3FFFFFFF)    // 1GB页偏移 (30位)
```

4级页表索引计算：
```c
#define PML4_INDEX(va)      (((va) >> 39) & 0x1FF)  // PML4索引 (bits 47:39)
#define PDPT_INDEX(va)      (((va) >> 30) & 0x1FF)  // PDPT索引 (bits 38:30)
#define PD_INDEX(va)        (((va) >> 21) & 0x1FF)  // PD索引 (bits 29:21)
#define PT_INDEX(va)        (((va) >> 12) & 0x1FF)  // PT索引 (bits 20:12)
```

### 2.2 VMCALL参数块

```c
typedef struct _VMCALL_MEM_PARAMS {
    ULONG64     TargetCr3;          // 目标进程CR3 (DirectoryTableBase)
    ULONG64     TargetVa;           // 目标进程中的虚拟地址
    ULONG64     BufferVa;           // 调用进程中的缓冲区VA（内核）
    ULONG       Size;               // 字节数
    NTSTATUS    Status;             // [输出] 结果状态
} VMCALL_MEM_PARAMS;
```

### 2.3 VMCALL分发码

```c
#define VMCALL_MAGIC            0xCAFE0000ULL    // RAX高位的幻数前缀
#define VMCALL_MAGIC_MASK       0xFFFF0000ULL
#define VMCALL_SUBCMD_SHUTDOWN      0x0000       // 关闭VMX/SVM
#define VMCALL_SUBCMD_READ_MEMORY   0x0001       // 读目标进程内存
#define VMCALL_SUBCMD_WRITE_MEMORY  0x0002       // 写目标进程内存
```

### 2.4 用户态内存请求结构（来自 shared.h）

```c
typedef struct _VMX_MEMORY_REQUEST {
    ULONG       Pid;                // 目标进程ID
    ULONG       Size;               // 读写字节数（最大64KB）
    ULONG64     VirtualAddress;     // 目标进程中的虚拟地址
} VMX_MEMORY_REQUEST;
```

---

## 3. 核心函数详解

### 3.1 `SafeReadPhysU64` — 安全的物理内存读取

```c
static BOOLEAN SafeReadPhysU64(ULONG64 PhysAddr, PULONG64 Value)
```

**功能**：从指定的物理地址读取一个ULONG64（8字节）值。

**参数**：
- `PhysAddr`：源物理地址
- `Value`：[输出] 读取的8字节值

**返回值**：成功返回TRUE，失败返回FALSE。

**核心流程**：
1. **零地址检查**：`PhysAddr == 0` 直接返回FALSE
2. **OS映射**：调用 `MmGetVirtualForPhysical(Pa)` 获取物理地址对应的内核虚拟地址
3. **检查有效性**：若返回NULL，说明OS无法为该物理地址建立映射
4. **读取值**：通过返回的VA指针读取8字节
5. **返回**：读取成功返回TRUE

**设计要点**：
- 安全范围：`IRQL ≤ DISPATCH_LEVEL`（调用者需保证）
- **不在VMX根模式下使用**：`MmGetVirtualForPhysical` 可能访问可分页结构
- 不使用SEH（结构化异常处理）：因为 `MmGetVirtualForPhysical` 的内部验证已经排除了非法地址的可能性

### 3.2 `HvGuestVaToPa` — Guest虚拟地址转物理地址

```c
ULONG64 HvGuestVaToPa(ULONG64 GuestCr3, ULONG64 VirtualAddress)
```

**功能**：通过手动遍历Guest的4级页表，将Guest虚拟地址转换为Guest物理地址。

**参数**：
- `GuestCr3`：目标进程的CR3值（DirectoryTableBase）
- `VirtualAddress`：需要翻译的虚拟地址

**返回值**：翻译成功返回物理地址，失败返回0。

**核心流程**（x64 4级页表遍历）：

```
GuestCr3 (& PAGE_ADDR_MASK_4K) → PML4基地址
    │
    ├── PML4[PML4_INDEX(va)] → Pml4e
    │   └── 检查 Present 位
    │
    ├── PDPT[(Pml4e & mask) + PDPT_INDEX(va)*8] → Pdpte
    │   ├── 检查 Present 位
    │   ├── 检查 Large 位 → 1GB页面: (Pdpte & mask_1G) | offset_1G(va)
    │   └── 继续下一级
    │
    ├── PD[(Pdpte & mask) + PD_INDEX(va)*8] → Pde
    │   ├── 检查 Present 位
    │   ├── 检查 Large 位 → 2MB页面: (Pde & mask_2M) | offset_2M(va)
    │   └── 继续下一级
    │
    └── PT[(Pde & mask) + PT_INDEX(va)*8] → Pte
        ├── 检查 Present 位
        └── 4KB页面: (Pte & mask_4K) | offset_4K(va)
```

**安全约束（M-7修订）**：
- 函数入口检查 `KeGetCurrentIrql() > DISPATCH_LEVEL`，若违反立即返回0
- 此检查防止在VMX根模式或高于DISPATCH_LEVEL的IRQL下调用
- 原因：`SafeReadPhysU64` 使用 `MmGetVirtualForPhysical`，该函数在高于DISPATCH_LEVEL时不可用

**逐级错误处理**：
- 任意一级页表项 `Present` 位为0 → 返回0
- 任意一级物理内存读取失败 → 返回0
- 支持1GB大页（PDPT级的 `PAGE_LARGE` 位）和2MB大页（PD级的 `PAGE_LARGE` 位）

### 3.3 `HvReadGuestMemory` — 读Guest内存（已禁用）

```c
NTSTATUS HvReadGuestMemory(ULONG64 GuestCr3, ULONG64 SourceVa, PVOID Destination, ULONG Size)
```

- **当前行为**：记录警告日志（"disabled path, use IOCTL KernelCopyProcessMemory instead"），返回 `STATUS_NOT_SUPPORTED`
- 所有参数被 `UNREFERENCED_PARAMETER` 抑制

### 3.4 `HvWriteGuestMemory` — 写Guest内存（已禁用）

```c
NTSTATUS HvWriteGuestMemory(ULONG64 GuestCr3, ULONG64 DestVa, PVOID Source, ULONG Size)
```

- 同上，返回 `STATUS_NOT_SUPPORTED`

### 3.5 `HvHandleMemoryVmcall` — VMCALL内存操作处理器（已禁用）

```c
BOOLEAN HvHandleMemoryVmcall(PVOID GuestContext, ULONG SubCommand)
```

**功能**：处理Guest发起的VMCALL/VMMCALL内存读写请求。

**参数**：
- `GuestContext`：保存的Guest寄存器上下文
- `SubCommand`：`VMCALL_SUBCMD_READ_MEMORY` 或 `VMCALL_SUBCMD_WRITE_MEMORY`

**当前行为**：
- 不读取参数结构体（避免有问题的物理地址直接解引用）
- 调用 `HvAdvanceGuestRip()` 推进RIP，防止Guest陷入无限循环
- 返回TRUE（表示继续执行Guest）
- **重要**：不修改 `VMCALL_MEM_PARAMS.Status` 字段，调用方应预初始化状态为哨兵值

**M-7安全考虑**：
- 原始实现将 `VMCALL_MEM_PARAMS` 的物理地址直接转换为虚拟地址指针解引用
- 在VMX根模式下，CPU使用Host CR3而非EPT标识映射，`PA != HVA`
- SEH（结构化异常处理）在根模式下不可靠，无法安全捕获非法内存访问
- 因此完全禁用此路径是最安全的选择

---

## 4. 控制流与逻辑流程

### 4.1 页表遍历控制流

```
HvGuestVaToPa(Cr3, VA)
    │
    ├── IRQL检查: KeGetCurrentIrql() > DISPATCH_LEVEL? → 返回0
    │
    ├── CR3零检查: (Cr3 & mask) == 0? → 返回0
    │
    ├── PML4级:
    │   ├── SafeReadPhysU64(PML4基址 + 索引*8) → Pml4e
    │   ├── 读取失败? → 返回0
    │   ├── Present位? → 返回0
    │   └── 通过
    │
    ├── PDPT级:
    │   ├── SafeReadPhysU64(PDPT基址 + 索引*8) → Pdpte
    │   ├── 读取失败? → 返回0
    │   ├── Present位? → 返回0
    │   ├── Large位? → 返回 (Pdpte & mask_1G) | offset_1G(VA)
    │   └── 通过
    │
    ├── PD级:
    │   ├── SafeReadPhysU64(PD基址 + 索引*8) → Pde
    │   ├── 读取失败? → 返回0
    │   ├── Present位? → 返回0
    │   ├── Large位? → 返回 (Pde & mask_2M) | offset_2M(VA)
    │   └── 通过
    │
    └── PT级:
        ├── SafeReadPhysU64(PT基址 + 索引*8) → Pte
        ├── 读取失败? → 返回0
        ├── Present位? → 返回0
        └── 返回 (Pte & mask_4K) | offset_4K(VA)
```

### 4.2 推荐的IOCTL内存访问路径（替代方案）

```
用户态 (VMXToolbox.exe)
    │
    │ DeviceIoControl(IOCTL_VMX_READ_MEMORY / IOCTL_VMX_WRITE_MEMORY)
    ▼
内核态 (vmxdrv.c: IOCTL分发器)
    │
    │ KernelCopyProcessMemory()
    │   ├── PsLookupProcessByProcessId(pid) → EPROCESS
    │   ├── KeStackAttachProcess(Eprocess)  // 切换到目标进程上下文
    │   ├── MmCopyVirtualMemory()           // OS管理的安全内存拷贝
    │   │   (或 MmMapIoSpace() 用于物理内存映射)
    │   └── KeUnstackDetachProcess()
    │
    ▼
返回数据给用户态
```

**优点**：
- 在 `PASSIVE_LEVEL` 的非根模式下运行
- SEH安全（可以使用 `__try/__except` 捕获异常）
- 通过OS内存管理器正确翻译Guest VA → Host VA
- 无需手动遍历页表

---

## 5. 与其他模块的交互

### 5.1 通过 hv_ops 的调用

`hv_mem.c` 通过 `hv_ops.h` 的宏 `HvAdvanceGuestRip()` 推进Guest RIP。
所有原有VMCALL路径的内存读写操作在M-7审查后已被禁用。

### 5.2 与日志模块的交互

使用 `LOG_WARN` 输出已禁用路径的警告信息，指导开发者使用新的IOCTL路径。

### 5.3 与IOCTL分发器的关系

文件头部注释明确建议用户态IOCTL消费者使用以下路径：
```
IOCTL → KernelCopyProcessMemory (vmxdrv.c)
    → PsLookupProcessByProcessId + KeStackAttachProcess
    → MmCopyVirtualMemory / MmMapIoSpace
```

### 5.4 与EPT/NPT的关系

`HvGuestVaToPa` 函数可以在EPT/NPT上下文中发挥作用——当驱动需要确认一个Guest虚拟地址是否映射到物理内存时（用于过滤或诊断），可以使用该函数进行地址翻译而不需要触发EPT缺页。

---

## 6. 关键设计要点

### 6.1 从VMX根模式直接访问物理内存的陷阱（M-7安全审查）

**原始设计的错误假设**：
- 原来的实现假设EPT将所有物理内存标识映射（Identity Map），因此 `Guest Physical Address == Host Virtual Address`
- 但这是**错误**的——在VMX根模式下，CPU使用**Host CR3页表**进行地址翻译，而非EPT
- Host CR3映射的内核虚拟地址并不等于物理地址

**后果**：
- 直接解引用 `(PVOID)PhysicalAddress` 导致访问了错误的虚拟地址
- SEH（结构化异常处理）在VMX根模式下不可靠（根据Intel SDM，某些异常行为在根模式下不同）
- 结果：BSOD而不是优雅的错误返回

**修复方案**：完全禁用有问题的路径，使用OS提供的安全API（`MmGetVirtualForPhysical`、`MmCopyVirtualMemory`）

### 6.2 4级页表遍历的完整性

`HvGuestVaToPa` 支持x64的所有三种页面大小：
- **4KB页面**：标准4级遍历（PML4 → PDPT → PD → PT）
- **2MB大页**：在PD级识别 `PAGE_LARGE` 位，提前返回
- **1GB大页**：在PDPT级识别 `PAGE_LARGE` 位，提前返回

每一级的错误处理都独立进行，任何一级的失败都返回0（翻译失败）。

### 6.3 SafeReadPhysU64 的安全性

使用 `MmGetVirtualForPhysical` 而非直接物理地址解引用：
- 这是WDK文档允许的在 `IRQL ≤ DISPATCH_LEVEL` 下使用物理地址→虚拟地址映射的标准API
- 物理地址为0时立即返回FALSE（空指针保护）
- 不滥用SEH——因为API本身已经进行了地址验证

### 6.4 Guest CR3中PCID/标志位的屏蔽

```c
Pml4Base = GuestCr3 & PAGE_ADDR_MASK_4K;  // 屏蔽低12位
```

x64的CR3寄存器低12位包含PCID（Process Context Identifier）和标志位（如 `CR3_PCID_ENABLE`、`CR3_PCID_NO_FLUSH`）。页表遍历时需屏蔽这些位以获取正确的PML4基地址。

### 6.5 禁用时代的遗留代码策略

- 保留禁用函数的编译和链接，避免破坏尚未修改的外部引用
- 函数体使用 `UNREFERENCED_PARAMETER` 宏抑制编译器警告
- 日志输出清晰指导开发者迁移到新路径
- `HvHandleMemoryVmcall` 保留最小操作（推进RIP），防止Guest死循环

### 6.6 安全边界

| 检查点 | 作用 |
|--------|------|
| IRQL检查 (`> DISPATCH_LEVEL`) | 防止在VMX根模式或DPC上下文中使用 |
| PhysAddr == 0 检查 | 防止null指针解引用 |
| `MmGetVirtualForPhysical` 返回值检查 | 确保OS层面映射有效 |
| 所有页表项的Present位检查 | 防止访问不存在的物理页 |
| 每级页表物理读取失败检查 | 防止未映射的物理地址访问 |
