# ssdt.c -- 逻辑分析

## 1. 文件概述

### 角色与职责
`ssdt.c` 是 VMX Hypervisor Toolbox 中负责 **SSDT (System Service Descriptor Table) 监控与 Hook 框架** 的核心模块。其职责包括：

- **SSDT 自动发现**：在不依赖任何硬编码内核地址的情况下，通过逆向工程动态定位 `nt!KiServiceTable` 的基址和大小。
- **系统调用地址解析**：将 SSDT 表中的相对偏移解码为实际函数虚拟地址。
- **名称缓存**：通过遍历 ntoskrnl.exe 的 PE 导出表，将 Nt\* 函数名与 SSDT 索引关联。
- **Hook 协调**：通过 `GenericHookInstall()` / `GenericHookRemove()` 框架，对任意系统调用号安装或移除 EPT/NPT 级别的不可见 Hook。
- **监控模式**：支持全量监控（ALL）和按系统调用索引过滤（FILTERED）两种监控模式，以 `HOOK_ACTION_LOG_ONLY` 方式记录目标进程的系统调用行为。

### 依赖的其他模块
| 被依赖模块 | 头文件 | 使用方式 |
|-----------|-------|---------|
| `hv_hook.h` | `driver/hv_hook.h` | 调用 `GenericHookInstall()` / `GenericHookRemove()` 部署 EPT/NPT Hook |
| `log.h` | `driver/log.h` | 日志记录（LOG_INFO / LOG_WARN / LOG_ERROR / LOG_DEBUG） |
| `shared.h` | `common/shared.h` | 共享数据结构（SSDT_ENTRY_INFO、HOOK_RULE、VMX_SSDT_MONITOR_REQUEST 等） |
| `ntddk.h` | WDK | 内核 API（MmGetSystemRoutineAddress、ExAllocatePoolWithTag 等） |
| `ntstrsafe.h` | WDK | 安全字符串操作（RtlStringCchCopyW） |
| `ntimage.h` | WDK | PE 结构体（IMAGE_DOS_HEADER、IMAGE_NT_HEADERS64、IMAGE_EXPORT_DIRECTORY） |

---

## 2. 数据结构

### 2.1 全局状态 -- `SSDT_STATE (ssdt.h:37-64)`

```c
typedef struct _SSDT_STATE {
    BOOLEAN     Initialized;
    ULONG64     KiSystemCall64Va;       /* IA32_LSTAR 值（信息性） */
    ULONG64     KiServiceTableVa;       /* nt!KiServiceTable 基地址（活动内存） */
    ULONG       ServiceCount;           /* 系统调用条目数 */

    ULONG64     ResolvedAddresses[SSDT_MAX_SERVICES];  /* 地址缓存：索引映射到函数VA */
    WCHAR       NameCache[SSDT_MAX_SERVICES][SSDT_MAX_NAME_LEN]; /* 名称缓存 */
    BOOLEAN     NamesPopulated;

    ULONG64     NtoskrnlBase;           /* ntoskrnl.exe 加载基址 */
    ULONG       NtoskrnlSize;           /* ntoskrnl.exe 镜像大小 */

    PSSDT_HOOK_MAPPING  HookListHead;   /* Hook 映射单向链表头 */
    ULONG               HookCount;
    KSPIN_LOCK          HookLock;        /* 保护 HookListHead/HookCount 的自旋锁 */

    ULONG       MonitorMode;            /* SSDT_MONITOR_OFF / ALL / FILTERED */
    ULONG       MonitorPid;
} SSDT_STATE, *PSSDT_STATE;
```

| 字段 | 说明 |
|------|------|
| `Initialized` | 标记模块是否已完成初始化 |
| `KiServiceTableVa` | 发现到的 `nt!KiServiceTable` 虚拟地址 |
| `ServiceCount` | SSDT 表中的系统调用条目总数（例如 Win10 x64 约 500+） |
| `ResolvedAddresses[]` | 预解码的每个索引对应的函数入口地址，索引即系统调用号 |
| `NameCache[][]` | 每个系统调用对应的 Nt\* 函数名（宽字符），例如 `NtOpenProcess` |
| `NamesPopulated` | 名称缓存是否已填充 |
| `NtoskrnlBase / NtoskrnlSize` | ntoskrnl.exe 加载基址和镜像大小，用于地址有效性验证 |
| `HookListHead` | 已安装 Hook 的单向链表（带自旋锁保护） |
| `MonitorMode` | 监控模式（关闭/全量/过滤） |
| `MonitorPid` | 监控策略绑定的目标进程 PID |

### 2.2 Hook 映射节点 -- `SSDT_HOOK_MAPPING (ssdt.h:27-32)`

```c
typedef struct _SSDT_HOOK_MAPPING {
    struct _SSDT_HOOK_MAPPING  *Next;
    ULONG       SyscallIndex;       /* SSDT 索引 (0..ServiceCount-1) */
    ULONG       GenericHookId;      /* GenericHookInstall 返回的 ID */
    BOOLEAN     IsMonitorHook;      /* TRUE 表示由监控模式创建的 Hook */
} SSDT_HOOK_MAPPING, *PSSDT_HOOK_MAPPING;
```

这是一个链表节点，维护系统调用号与通用 Hook 框架 ID 的映射关系。`IsMonitorHook` 标记使 `SsdtStopMonitoring()` 能够快速识别哪些 Hook 是监控模式创建的，从而在停止监控时只移除这些 Hook，不影响用户手动安装的 Hook。

### 2.2b 内部辅助结构 -- `ZW_NT_PAIR (ssdt.c:132-135)`

```c
typedef struct _ZW_NT_PAIR {
    const WCHAR *ZwName;
    const WCHAR *NtName;
} ZW_NT_PAIR;
```

用于 SSDT 发现的 Zw/Nt 函数名对。共 8 对，包括 `ZwClose/NtClose`、`ZwOpenProcess/NtOpenProcess` 等每个 x64 Windows 版本均导出的函数。

### 2.3 共享的数据结构（来自 `shared.h`）

| 结构体 | 用途 |
|--------|------|
| `SSDT_ENTRY_INFO` | 单个 SSDT 条目的信息（索引、参数数、原始偏移、函数 VA、函数名） |
| `HOOK_RULE` | Hook 行为控制（动作类型、目标 PID、阻断返回值等） |
| `VMX_SSDT_MONITOR_REQUEST` | 监控模式配置（模式、目标 PID、过滤器索引列表） |

### 2.4 SSDT 相关常量（来自 `shared.h`）

| 常量 | 值 | 说明 |
|------|-----|------|
| `SSDT_MAX_SERVICES` | 512 | 最大系统调用条目数 |
| `SSDT_MAX_NAME_LEN` | 128 | 函数名最大长度 |
| `SSDT_MONITOR_OFF` | 0 | 停止监控 |
| `SSDT_MONITOR_ALL` | 1 | 全量监控模式 |
| `SSDT_MONITOR_FILTERED` | 2 | 过滤监控模式 |
| `SSDT_MONITOR_MAX_FILTER` | 64 | 过滤模式下最多可指定的索引数 |

---

## 3. 核心函数详解

### 生命周期函数

#### `SsdtInitialize() (line 1056-1088)`

**签名**：`NTSTATUS SsdtInitialize(VOID)`

**功能**：SSDT 模块的完整初始化入口。

**核心流程**：
1. 检查 `g_SsdtState.Initialized`，如果已初始化则直接返回成功（幂等）
2. 清零全局状态并初始化自旋锁
3. 调用 `SsdtGetNtoskrnlBase()` 获取 ntoskrnl.exe 基址和大小
4. 调用 `SsdtDiscoverByZwStubReverse()` 通过 Zw 存根逆向发现 KiServiceTable
5. 调用 `SsdtPopulateNames()` 填充名称缓存（尽力而为，失败不阻塞）
6. 设置 `Initialized = TRUE`

**返回值**：STATUS_SUCCESS 或底层发现函数的错误码。

#### `SsdtCleanup() (line 1090-1104)`

**功能**：停止所有监控并移除所有 SSDT Hook。

---

### ntoskrnl 基地址发现

#### `SsdtGetNtoskrnlBase() (line 84-120)`

**签名**：`static NTSTATUS SsdtGetNtoskrnlBase(VOID)`

**功能**：获取 ntoskrnl.exe 的加载基址和镜像大小。

**算法**：
1. 调用 `ZwQuerySystemInformation(SystemModuleInformation)` 两次——第一次查询所需缓冲区大小，第二次获取数据
2. 系统模块列表的第一个条目始终是 ntoskrnl.exe
3. 提取 `Modules[0].ImageBase` 和 `ImageSize` 保存到 `g_SsdtState`

**注意事项**：使用 `ExAllocatePoolWithTag` 分配非分页内存，池标签为 `'TDSS'`（SSDT 倒写）。

---

### KiServiceTable 发现（关键技术）

#### `SsdtExtractIndexFromZwStub() (line 164-191)`

**签名**：`static BOOLEAN SsdtExtractIndexFromZwStub(ULONG64 ZwFuncVa, PULONG OutIndex)`

**功能**：从 Zw\* 内核存根的前 30 字节中提取系统调用号。

**核心逻辑**：
- x64 中的 Zw\* 存根包含指令 `mov eax, imm32`（操作码 0xB8），将系统调用号加载到 EAX
- 扫描 0-29 字节寻找模式：`B8 xx xx 00 00`（16 位索引）或 `B8 xx xx xx 00`（24 位索引）
- 验证 `Index < SSDT_MAX_SERVICES (512)` 防止越界
- 每次读取前调用 `MmIsAddressValid()` 确保内存可访问

**为什么这个方案有效**：x64 上每个 Zw\* 函数的第一条指令几乎总是 `mov eax, SSDT_INDEX`，且索引值永远小于 0x10000。

#### `SsdtFindRdataSection() (line 201-255)`

**签名**：`static BOOLEAN SsdtFindRdataSection(PUCHAR NtBase, ULONG NtSize, PULONG64 OutStart, PULONG64 OutEnd)`

**功能**：定位 ntoskrnl 中包含 KiServiceTable 的只读数据节区。

**算法**：
1. 验证 PE 头合法性（DOS 签名、NT 签名）
2. 遍历所有节区，寻找 `IMAGE_SCN_CNT_INITIALIZED_DATA | !IMAGE_SCN_MEM_WRITE` 的节
3. 选取最大的符合条件的节区（KiServiceTable 通常位于此类节区中）

#### `SsdtDiscoverByZwStubReverse() (line 269-479)`

**签名**：`static NTSTATUS SsdtDiscoverByZwStubReverse(VOID)`

**这是整个 SSDT 发现的核心算法**，分为 5 个阶段：

**Phase 1 -- 提取 (Index, NtFuncVa) 对**：
1. 遍历 8 个已知 Zw/Nt 函数名对
2. 通过 `MmGetSystemRoutineAddress()` 解析每个函数的地址
3. 通过 `SsdtExtractIndexFromZwStub()` 从 Zw 存根中提取系统调用索引
4. 验证 Nt 函数地址在 ntoskrnl 范围内
5. 收集至少 3 对有效数据才能进入下一阶段

**Phase 2 -- 确定扫描范围**：
1. 使用 `SsdtFindRdataSection()` 找到只读数据节区
2. 如果找不到，回退到扫描整个 ntoskrnl 范围

**Phase 3 -- 暴力扫描 KiServiceTable**：
1. 从扫描范围起点开始，按 4 字节对齐逐一尝试
2. 对每个候选地址，验证所有已知 (Index, NtVa) 对是否解码正确：
   - `Candidate + (Table[Index] >> 4) == NtVa`
3. 每页边界检查 `MmIsAddressValid`，跳过未映射的页
4. 全部匹配即认为找到 KiServiceTable

**Phase 4 -- 确定 ServiceCount**：
1. 从 KiServiceTable 向后遍历条目
2. 当解码后的 VA 超出 ntoskrnl 范围时停止
3. 要求至少 100 个有效条目，否则认为发现失败

**Phase 5 -- 解码所有地址**：
1. 遍历所有条目，解码每个函数的 VA
2. 验证 VA 在 ntoskrnl 范围内，否则标记为 0

**设计亮点**：这种发现方法不依赖 `IA32_LSTAR` 或 `KiSystemCall64` 的代码布局，在 KPTI、VBS、Hyper-V 嵌套虚拟化环境下均能可靠工作，兼容 Vista 到 Win11 的所有 x64 Windows 版本。

---

### 地址解析

#### `SsdtResolveAddress() (line 485-511)`

**签名**：`ULONG64 SsdtResolveAddress(ULONG Index)`

**功能**：按需解析单个系统调用的地址。如果地址已在缓存中则直接返回，否则从活动内存中读取 SSDT 条目并解码，使用 `__try/__except` 处理可能的页面错误。

#### `SsdtPopulateNames() (line 521-601)`

**签名**：`NTSTATUS SsdtPopulateNames(VOID)`

**功能**：遍历 ntoskrnl PE 导出表，将 Nt\* 导出函数的地址与已解析的 SSDT 地址匹配，从而确定每个系统调用的名称。

**算法**：
1. 验证 PE 头并定位导出目录
2. 遍历所有导出函数名，只关心以 "Nt" 开头的函数（跳过 "Zw" 前缀）
3. 对每个 Nt\* 函数，在所有已解析的 SSDT 地址中查找匹配
4. 匹配成功则转换为宽字符存入 `NameCache`
5. 使用 `__try/__except` 保护整个 PE 遍历过程

#### `SsdtFindIndexByName() (line 607-653)`

**签名**：`NTSTATUS SsdtFindIndexByName(const WCHAR *Name, PULONG OutIndex)`

**功能**：通过函数名查找系统调用索引。先查名称缓存，如果找不到则降级为通过 `MmGetSystemRoutineAddress` 解析地址并反向匹配。

---

### 表查询 API

#### `SsdtGetEntryInfo() (line 659-688)`

**签名**：`NTSTATUS SsdtGetEntryInfo(ULONG Index, PSSDT_ENTRY_INFO Out)`

**功能**：获取单个 SSDT 条目的完整信息（原始偏移、参数数、函数 VA、函数名）。

**关键细节**：参数数通过 `RawOffset & 0xF` 提取——SSDT 条目的低 4 位编码了参数个数。

#### `SsdtDumpTable() (line 690-717)`

**功能**：批量转储 SSDT 表。支持指定起始索引和数量（0 表示全部），返回 flex-array 格式的 `SSDT_ENTRY_INFO` 数组。

---

### Hook 操作

#### `SsdtHookByIndex() (line 747-810)`

**签名**：`NTSTATUS SsdtHookByIndex(ULONG Index, PHOOK_RULE Rule, PULONG OutHookId)`

**核心流程**：
1. 检查模块是否已初始化，索引是否有效
2. 获取自旋锁检查是否已存在相同索引的 Hook（防重入）
3. 从 `ResolvedAddresses` 获取目标函数地址
4. 委托 `GenericHookInstall()` 安装 EPT/NPT 级别的 Hook
5. 分配 `SSDT_HOOK_MAPPING` 节点并插入链表头部
6. 返回通过 `GenericHookInstall` 分配的 `HookId`

#### `SsdtHookByName() (line 812-827)`

**功能**：通过 Nt\* 函数名安装 Hook。先调用 `SsdtFindIndexByName()` 解析名称到索引，再调用 `SsdtHookByIndex()`。

---

### 移除 Hook

#### `SsdtUnhookByIndex() (line 833-865)`

**功能**：通过系统调用索引移除 Hook。在自旋锁保护下从链表中找到节点、摘除，然后调用 `GenericHookRemove()` 并释放内存。

#### `SsdtUnhookByHookId() (line 867-898)`

**功能**：通过 GenericHookId 移除 Hook。流程同上，但按 HookId 查找。

#### `SsdtUnhookAll() (line 900-922)`

**功能**：移除所有 SSDT Hook。先在自旋锁保护下摘除整个链表，然后在锁外逐个调用 `GenericHookRemove()` 和释放内存（避免在持有自旋锁时调用可能引起页错误的函数）。

---

### 监控模式

#### `SsdtSetMonitorMode() (line 928-1005)`

**签名**：`NTSTATUS SsdtSetMonitorMode(PVMX_SSDT_MONITOR_REQUEST Req)`

**功能**：配置 SSDT 监控模式。

**三种模式**：
1. **OFF**：停止监控（调用 `SsdtStopMonitoring()`）
2. **ALL**：对所有系统调用安装 `HOOK_ACTION_LOG_ONLY` 类型的 Hook
3. **FILTERED**：只对指定的系统调用索引子集安装 Hook

安装后标记每个 Hook 节点 `IsMonitorHook = TRUE`，以便后续区分用户手动 Hook 和监控 Hook。

#### `SsdtStopMonitoring() (line 1007-1050)`

**功能**：停止监控。遍历 Hook 链表，将所有 `IsMonitorHook == TRUE` 的节点摘除到一个临时列表，然后在锁外统一移除和释放。

**设计考量**：这种分离移除机制确保用户手动安装的 Hook 在监控关闭后保持有效。

---

## 4. 控制流与逻辑流程

### 4.1 初始化调用链

```
SsdtInitialize()
  |
  +-> SsdtGetNtoskrnlBase()           // 获取 ntoskrnl 基址
  |     +-> ZwQuerySystemInformation(SystemModuleInformation)
  |
  +-> SsdtDiscoverByZwStubReverse()    // 发现 KiServiceTable
  |     +-> 对8对 Zw/Nt 函数提取索引
  |     +-> SsdtFindRdataSection()     // 定位只读数据节区
  |     +-> 暴力扫描匹配 (Index, NtVa) 对
  |     +-> 遍历确定 ServiceCount
  |     +-> 解码所有 SSDT 条目
  |
  +-> SsdtPopulateNames()              // 填充名称缓存（尽力而为）
```

### 4.2 Hook 安装控制流

```
SsdtHookByIndex(Index, Rule, &HookId)
  |
  +-> 检查 Initialized && Index < ServiceCount
  +-> 自旋锁 -> 检查重复 -> 释放自旋锁
  +-> ResolvedAddresses[Index]  -> FuncVa
  +-> GenericHookInstall(FuncVa, ...) -> HookId
  |     +-> EPT/NPT 页面拆分
  |     +-> 生成跳转 thunk
  |     +-> 注册决策回调
  |
  +-> 分配 SSDT_HOOK_MAPPING 节点
  +-> 自旋锁 -> 插入链表头 -> 释放自旋锁
```

### 4.3 错误处理路径

| 错误情况 | 处理方式 | 返回值 |
|----------|---------|--------|
| ntoskrnl 基址获取失败 | 记录错误日志 | STATUS_UNSUCCESSFUL |
| 提取不到足够的 Zw/Nt 对 (<3) | 记录警告日志 | STATUS_UNSUCCESSFUL |
| KiServiceTable 扫描不到 | 记录警告日志 | STATUS_NOT_FOUND |
| ServiceCount < 100 | 记录警告日志 | STATUS_UNSUCCESSFUL |
| 重复 Hook | 记录警告日志 | STATUS_ALREADY_REGISTERED |
| GenericHookInstall 失败 | 记录警告日志 | 传递下层错误码 |
| 内存分配失败（节点） | 回滚 GenericHookRemove | STATUS_INSUFFICIENT_RESOURCES |
| PE 遍历异常 | SEH 捕获 | STATUS_ACCESS_VIOLATION |
| 名称未找到 | 返回 NOT_FOUND | STATUS_NOT_FOUND |

---

## 5. 与其他模块的交互

### 5.1 与 `hv_hook.c/hv_hook.h` 的交互

这是最关键的交互关系。`ssdt.c` 本身不实现任何 Hook 机制，所有实际的 Hook 功能全部委托给 `hv_hook.c` 中的通用 Hook 框架：

- `GenericHookInstall(TargetVa, ProcessId, Name, Rule, &HookId)`：安装 EPT/NPT 级别的不可见 Hook
- `GenericHookRemove(HookId)`：移除指定 ID 的 Hook

这种设计实现了关注点分离（Separation of Concerns）——SSDT 模块负责定位和解析系统调用表，通用 Hook 框架负责底层的页面拆分和拦截机制。

### 5.2 与 `ssdt.h` 的交互

`ssdt.h` 提供了：
- 数据结构定义（SSDT_STATE、SSDT_HOOK_MAPPING）
- 公共 API 声明
- 全局状态导出（`g_SsdtState`）

其中 `SSDT_HOOK_MAPPING` 和部分常量也被 `shadow_ssdt.c` 复用——Shadow SSDT 模块使用完全相同的链表节点结构和 API 模式。

### 5.3 与 `shared.h` 的交互

`shared.h` 定义了所有与用户态客户端共享的数据结构：
- IOCTL 代码（0x80D-0x813）
- `SSDT_ENTRY_INFO`、`HOOK_RULE`、`VMX_SSDT_MONITOR_REQUEST` 等结构
- 常量定义（SSDT_MAX_SERVICES、SSDT_MONITOR_*）

### 5.4 与 `log.h` 的交互

使用 `LOG_ERROR`、`LOG_WARN`、`LOG_INFO`、`LOG_DEBUG` 四种级别的日志宏进行诊断输出。

---

## 6. 关键设计要点

### 6.1 SSDT 条目编码格式

x64 Windows 中的 KiServiceTable 条目不是直接存储函数指针，而是存储 **相对偏移**：
```
SSDT_Entry[i] = (FunctionVa - KiServiceTableBase) << 4 | ArgumentCount
```

所以完整的解码过程是：
```c
FunctionVa = KiServiceTableBase + (SSDT_Entry[i] >> 4);
ArgumentCount = SSDT_Entry[i] & 0xF;
```

SsdtDiscoverByZwStubReverse 的 Phase 3 暴力扫描正是基于这个编码公式进行匹配。

### 6.2 非侵入式发现（不触发 PatchGuard）

传统的 SSDT Hook 方式（修改 KiServiceTable 条目）会：
1. 被 Windows PatchGuard (Kernel Patch Protection) 检测到
2. 触发 Bug Check 0x109 (CRITICAL_STRUCTURE_CORRUPTION)

本项目的 SSDT 模块 **不修改 KiServiceTable 本身**，而是使用 `hv_hook.c` 提供的 EPT/NPT 页面级别拦截。这种方法的优势：
- 不对 KiServiceTable 做任何写入，不触发 PatchGuard
- Hook 在内核内存模型之外（Ring -1 级别）
- 对目标系统调用函数本身做 EPT 页面拆分，而非替换 SSDT 表项

### 6.3 Staple 式名称解析

名称解析算法（SsdtPopulateNames）不是通过硬编码的序数或表偏移来匹配名称，而是：
1. 遍历 ntoskrnl 导出表中所有 Nt\* 开头的函数
2. 将函数地址与 `ResolvedAddresses[]` 中的地址逐一比对
3. 创建名称到索引的映射

这种方法不需要知道函数名到系统调用号的硬编码映射表，自动适应不同 Windows 版本中系统调用号的重新排列。

### 6.4 缓存与按需解析相结合

`ResolvedAddresses` 在初始化时一次性加载所有条目，但 `SsdtResolveAddress()` 也支持按需解析（如果缓存项为 0 则从内存实时读取）。这种双模式确保：
- 初始化时已经缓存尽可能多的地址
- 如果某些地址在初始化时解码失败（超出 ntoskrnl 范围），后续可以通过按需解析获得
- 减少对 SSDT 完整性的假设

### 6.5 监控与手动 Hook 共存

通过 `IsMonitorHook` 标记区分监控 Hook 和用户手动 Hook：
- `SsdtStopMonitoring()` 只移除标记为监控的 Hook
- `SsdtUnhookAll()` 移除所有 Hook（包括手动和监控）
- `SsdtUnhookByIndex()` / `SsdtUnhookByHookId()` 不区分类型，按条件移除

### 6.6 版本兼容性

- 支持 Vista 到 Win11 的所有 x64 Windows 版本
- 兼容 KPTI (Kernel Page Table Isolation) / Meltdown 补丁系统
- 兼容 VBS (Virtualization-Based Security) / Hyper-V 嵌套虚拟化
- Win10+ 中 KiServiceTable 位于只读内存中的策略被 `SsdtFindRdataSection` 处理
