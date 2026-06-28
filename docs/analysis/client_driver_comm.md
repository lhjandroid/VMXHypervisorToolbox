# client/driver_comm.c — 逻辑分析

## 1. 文件概述

### 角色与职责
`driver_comm.c` 是 VMX Hypervisor Toolbox 的用户态驱动程序通信封装层。其核心职责是：

- 封装 `DeviceIoControl` 调用，提供类型安全的 IOCTL 请求/响应接口
- 管理驱动设备句柄的打开与关闭（`\\.\VMXToolbox`）
- 将用户态的高级操作（设置目标、安装Hook、读写内存等）转换为格式化的 IOCTL 请求
- 隐藏底层缓冲区管理、参数序列化等细节

### 依赖的其他模块
| 模块 | 头文件 | 用途 |
|------|--------|------|
| 共享定义 | `../common/shared.h` | IOCTL 码、共享结构体定义、常量 |
| Windows API | `<windows.h>` | `DeviceIoControl`、`CreateFileA`、`CloseHandle` |
| C标准库 | `<stdio.h>`, `<stdlib.h>`, `<string.h>` | `fprintf`、`malloc`、`free`、`memcpy`、`memset` |

### 对应头文件
`driver_comm.h` 声明了所有对外接口，包含详细的函数注释说明使用前提和语义。

## 2. 数据结构

### 核心常量

#### 驱动设备路径
```c
#define VMX_USERMODE_PATH   "\\\\.\\VMXToolbox"
```
用户态访问驱动的 Win32 设备路径。在内核中对应 `\Device\VMXToolbox`，通过 `\DosDevices\VMXToolbox` 符号链接暴露。

### 使用的共享结构体
文件本身不定义新结构体，全部从 `common/shared.h` 导入。使用的结构体包括：

| 结构体 | 用途 | 作为 | 方向 |
|--------|------|------|------|
| `VMX_TARGET_INFO` | 设置目标进程的输入参数 | InputBuffer | 用户->驱动 |
| `VMX_REMOVE_TARGET` | 移除目标的输入参数 | InputBuffer | 用户->驱动 |
| `VMX_CONFIG_INFO` | 更新配置的输入参数 | InputBuffer | 用户->驱动 |
| `VMX_STATUS` | 查询状态的输出参数 | OutputBuffer | 驱动->用户 |
| `VMX_LOG_BUFFER` | 获取日志的输出参数 | OutputBuffer | 驱动->用户 |
| `VMX_MEMORY_REQUEST` | 内存读写的请求头 | InputBuffer | 用户->驱动 |
| `VMX_HOOK_REQUEST` | 安装 Hook 的请求 | InputBuffer | 用户->驱动 |
| `VMX_HOOK_RESPONSE` | 安装 Hook 的响应 | OutputBuffer | 驱动->用户 |
| `VMX_UNHOOK_REQUEST` | 卸载 Hook 的请求 | InputBuffer | 用户->驱动 |
| `VMX_HOOK_LIST` | Hook 列表输出 | OutputBuffer | 驱动->用户 |
| `VMX_HOOK_EVENT_BUFFER` | Hook 事件输出 | OutputBuffer | 驱动->用户 |
| `VMX_SSDT_INIT_RESPONSE` | SSDT 初始化响应 | OutputBuffer | 驱动->用户 |
| `VMX_SSDT_DUMP_REQUEST` | SSDT 转储请求 | InputBuffer | 用户->驱动 |
| `VMX_SSDT_DUMP_RESPONSE` | SSDT 转储响应 | OutputBuffer | 驱动->用户 |
| `VMX_SSDT_HOOK_REQUEST` | SSDT Hook 请求 | InputBuffer | 用户->驱动 |
| `VMX_SSDT_HOOK_RESPONSE` | SSDT Hook 响应 | OutputBuffer | 驱动->用户 |
| `VMX_SSDT_UNHOOK_REQUEST` | SSDT Unhook 请求 | InputBuffer | 用户->驱动 |
| `VMX_SSDT_HOOK_LIST` | SSDT Hook 列表 | OutputBuffer | 驱动->用户 |
| `VMX_SSDT_MONITOR_REQUEST` | SSDT Monitor 请求 | InputBuffer | 用户->驱动 |
| Shadow SSDT 系列结构体 | 同上，前缀为 `VMX_SHADOW_SSDT_*` | 同上 | 同上 |

### IOCTL 码速查

所有 IOCTL 都使用 `METHOD_BUFFERED` + `FILE_ANY_ACCESS` 模式：

| IOCTL 码 | 宏定义 | 功能 |
|----------|--------|------|
| `0x800` | `IOCTL_VMX_INIT` | 初始化 VMX 引擎 |
| `0x801` | `IOCTL_VMX_SET_TARGET` | 设置目标进程 |
| `0x802` | `IOCTL_VMX_REMOVE_TARGET` | 移除目标进程 |
| `0x803` | `IOCTL_VMX_SET_CONFIG` | 设置配置 |
| `0x804` | `IOCTL_VMX_GET_LOG` | 获取日志 |
| `0x805` | `IOCTL_VMX_STOP` | 停止 VMX 引擎 |
| `0x806` | `IOCTL_VMX_QUERY_STATUS` | 查询状态 |
| `0x807` | `IOCTL_VMX_READ_MEMORY` | 读取内存 |
| `0x808` | `IOCTL_VMX_WRITE_MEMORY` | 写入内存 |
| `0x809` | `IOCTL_VMX_INSTALL_HOOK` | 安装 Hook |
| `0x80A` | `IOCTL_VMX_REMOVE_HOOK` | 卸载 Hook |
| `0x80B` | `IOCTL_VMX_LIST_HOOKS` | 列出 Hook |
| `0x80C` | `IOCTL_VMX_GET_HOOK_EVENTS` | 获取 Hook 事件 |
| `0x80D` | `IOCTL_VMX_SSDT_INIT` | 初始化 SSDT |
| `0x80E` | `IOCTL_VMX_SSDT_DUMP` | 转储 SSDT |
| `0x80F` | `IOCTL_VMX_SSDT_HOOK` | Hook SSDT 表项 |
| `0x810` | `IOCTL_VMX_SSDT_UNHOOK` | 卸载 SSDT Hook |
| `0x811` | `IOCTL_VMX_SSDT_UNHOOK_ALL` | 卸载所有 SSDT Hook |
| `0x812` | `IOCTL_VMX_SSDT_LIST_HOOKS` | 列出 SSDT Hook |
| `0x813` | `IOCTL_VMX_SSDT_MONITOR` | 设置 SSDT 监控 |
| `0x814` | `IOCTL_VMX_SHADOW_SSDT_INIT` | 初始化 Shadow SSDT |
| `0x815` | `IOCTL_VMX_SHADOW_SSDT_DUMP` | 转储 Shadow SSDT |
| `0x816` | `IOCTL_VMX_SHADOW_SSDT_HOOK` | Hook Shadow SSDT |
| `0x817` | `IOCTL_VMX_SHADOW_SSDT_UNHOOK` | 卸载 Shadow SSDT Hook |
| `0x818` | `IOCTL_VMX_SHADOW_SSDT_UNHOOK_ALL` | 卸载所有 Shadow SSDT Hook |
| `0x819` | `IOCTL_VMX_SHADOW_SSDT_LIST_HOOKS` | 列出 Shadow SSDT Hook |
| `0x81A` | `IOCTL_VMX_SHADOW_SSDT_MONITOR` | 设置 Shadow SSDT 监控 |

共 27 个 IOCTL 码，范围 `0x800` - `0x81A`。

## 3. 核心函数详解

### 3.1 驱动句柄管理

#### `DriverOpen`
```c
BOOL DriverOpen(HANDLE *OutHandle)
```
- **功能**：打开到 VMX 驱动的用户态设备句柄
- **参数**：`OutHandle` — 输出参数，成功时接收设备句柄
- **返回值**：成功返回 `TRUE`，失败返回 `FALSE`（并打印错误信息）
- **核心逻辑**：
  1. 调用 `CreateFileA` 打开 `\\.\VMXToolbox`，请求 `GENERIC_READ | GENERIC_WRITE`
  2. 失败时打印详细的用户指引（如何加载驱动）
  3. 成功时将句柄写入 `OutHandle`
- **错误处理**：失败时输出驱动加载步骤给用户参考

#### `DriverClose`
```c
VOID DriverClose(HANDLE DeviceHandle)
```
- **功能**：关闭驱动句柄
- **参数**：`DeviceHandle` — 要关闭的设备句柄
- **核心逻辑**：检查句柄有效后调用 `CloseHandle`
- **安全考虑**：对 `NULL` 和 `INVALID_HANDLE_VALUE` 都进行了防护

### 3.2 基本操作 IOCTL 封装

#### `DriverInitVmx`
```c
BOOL DriverInitVmx(HANDLE DeviceHandle)
```
- **功能**：发送初始化 VMX 引擎的 IOCTL
- **IOCTL**：`IOCTL_VMX_INIT` (0x800)
- **缓冲区**：无输入，无输出
- **返回值**：`DeviceIoControl` 的结果
- **注意**：驱动对重复初始化返回 `ERROR_ALREADY_REGISTERED`，调用方需处理此情况

#### `DriverStopVmx`
```c
BOOL DriverStopVmx(HANDLE DeviceHandle)
```
- **功能**：停止 VMX 引擎
- **IOCTL**：`IOCTL_VMX_STOP` (0x805)
- **缓冲区**：无输入，无输出

#### `DriverSetTarget`
```c
BOOL DriverSetTarget(HANDLE DeviceHandle, DWORD Pid, DWORD Flags)
```
- **功能**：设置要保护的目标进程及反反调试特性
- **IOCTL**：`IOCTL_VMX_SET_TARGET` (0x801)
- **参数**：
  - `Pid` — 目标进程 ID
  - `Flags` — `AAD_HIDE_*` 位掩码，指定启用的反反调试特性
- **输入结构**：`VMX_TARGET_INFO`（Pid + Flags）
- **缓冲区**：输入大小 `sizeof(VMX_TARGET_INFO)`，无输出

#### `DriverRemoveTarget`
```c
BOOL DriverRemoveTarget(HANDLE DeviceHandle, DWORD Pid)
```
- **功能**：从保护列表中移除目标进程
- **IOCTL**：`IOCTL_VMX_REMOVE_TARGET` (0x802)
- **输入结构**：`VMX_REMOVE_TARGET`（仅 Pid）

#### `DriverSetConfig`
```c
BOOL DriverSetConfig(HANDLE DeviceHandle, DWORD Pid, DWORD Flags)
```
- **功能**：更新已跟踪进程的反反调试标志
- **IOCTL**：`IOCTL_VMX_SET_CONFIG` (0x803)
- **输入结构**：`VMX_CONFIG_INFO`（Pid + Flags）

#### `DriverQueryStatus`
```c
BOOL DriverQueryStatus(HANDLE DeviceHandle, VMX_STATUS *OutStatus)
```
- **功能**：查询 VMX 引擎运行状态
- **IOCTL**：`IOCTL_VMX_QUERY_STATUS` (0x806)
- **输出结构**：`VMX_STATUS` — 包含 VmxActive、ActiveTargets、TotalExits、CpuCount
- **缓冲区**：输出大小 `sizeof(VMX_STATUS)`

#### `DriverGetLog`
```c
BOOL DriverGetLog(HANDLE DeviceHandle, VMX_LOG_BUFFER *Buffer, DWORD BufferSize, DWORD *BytesReturned)
```
- **功能**：获取驱动内部日志条目的环形缓冲区内容
- **IOCTL**：`IOCTL_VMX_GET_LOG` (0x804)
- **参数**：
  - `Buffer` — 预分配的输出缓冲区（`VMX_LOG_BUFFER` + 可变长 `VMX_LOG_ENTRY[]`）
  - `BufferSize` — 缓冲区总大小
  - `BytesReturned` — 实际返回的字节数
- **输出结构**：`VMX_LOG_BUFFER`（Count + Entries[Count]）

### 3.3 内存读写 IOCTL 封装

#### `DriverReadMemory`
```c
BOOL DriverReadMemory(HANDLE DeviceHandle, DWORD Pid, ULONG64 VirtualAddress,
                      PVOID OutBuffer, DWORD Size, DWORD *BytesReturned)
```
- **功能**：通过 Hypervisor 物理内存访问（CR3 页表遍历）读取目标进程内存
- **IOCTL**：`IOCTL_VMX_READ_MEMORY` (0x807)
- **参数**：
  - `Pid` — 目标进程 ID
  - `VirtualAddress` — 目标虚拟地址
  - `OutBuffer` — 接收数据的缓冲区
  - `Size` — 读取字节数（最大 `VMX_MEM_MAX_SIZE` = 64KB）
  - `BytesReturned` — 实际读取字节数
- **输入结构**：`VMX_MEMORY_REQUEST`（Pid, Size, VirtualAddress）
- **关键特性**：绕过所有 Windows API 级钩子（无 `OpenProcess`、`NtReadVirtualMemory`、`KeStackAttachProcess`）

#### `DriverWriteMemory`
```c
BOOL DriverWriteMemory(HANDLE DeviceHandle, DWORD Pid, ULONG64 VirtualAddress,
                       const VOID *InBuffer, DWORD Size)
```
- **功能**：通过 Hypervisor 物理内存访问写入目标进程内存
- **IOCTL**：`IOCTL_VMX_WRITE_MEMORY` (0x808)
- **参数**：
  - `Pid` — 目标进程 ID
  - `VirtualAddress` — 目标虚拟地址
  - `InBuffer` — 要写入的数据
  - `Size` — 写入字节数
- **缓冲区布局**（特殊设计）：
  ```
  [VMX_MEMORY_REQUEST 头部][负载数据...]
  总大小 = sizeof(VMX_MEMORY_REQUEST) + Size
  ```
- **实现细节**：
  1. 分配 `sizeof(VMX_MEMORY_REQUEST) + Size` 大小的发送缓冲区
  2. 填充 `VMX_MEMORY_REQUEST` 头部（Pid, VirtualAddress, Size）
  3. 将负载数据 `memcpy` 到头部之后
  4. 发送组合缓冲区到驱动
  5. `free` 临时缓冲区
- **返回值**：BOOL 类型的 DeviceIoControl 结果
- **内存管理**：使用 `malloc`/`free` 管理临时缓冲区，失败时设置 `ERROR_NOT_ENOUGH_MEMORY`

### 3.4 Hook 框架 IOCTL 封装

#### `DriverInstallHook`
```c
BOOL DriverInstallHook(HANDLE DeviceHandle, BOOL ByName, const WCHAR *FunctionName,
                       ULONG64 TargetAddress, DWORD ProcessId,
                       const HOOK_RULE *Rule, VMX_HOOK_RESPONSE *OutResponse)
```
- **功能**：安装一个内核函数 Hook
- **IOCTL**：`IOCTL_VMX_INSTALL_HOOK` (0x809)
- **两种模式**：
  - `ByName=TRUE` — 通过 `FunctionName`（内核导出名，如 `"NtOpenProcess"`）解析地址
  - `ByName=FALSE` — 直接使用 `TargetAddress`（十六进制地址）
- **输入结构**：`VMX_HOOK_REQUEST`（ByName, FunctionName[128], TargetAddress, ProcessId, Rule）
- **输出结构**：`VMX_HOOK_RESPONSE`（HookId, ResolvedAddress）
- **关键注意**：ByName 时宽字符复制使用 `wcsncpy`，长度限制 `HOOK_MAX_NAME_LEN - 1`

#### `DriverRemoveHook`
```c
BOOL DriverRemoveHook(HANDLE DeviceHandle, DWORD HookId)
```
- **功能**：通过 HookId 卸载已安装的 Hook
- **IOCTL**：`IOCTL_VMX_REMOVE_HOOK` (0x80A)
- **输入结构**：`VMX_UNHOOK_REQUEST`（仅 HookId）

#### `DriverListHooks`
```c
BOOL DriverListHooks(HANDLE DeviceHandle, VMX_HOOK_LIST *Buffer,
                     DWORD BufferSize, DWORD *BytesReturned)
```
- **功能**：列出所有活跃的 Hook
- **IOCTL**：`IOCTL_VMX_LIST_HOOKS` (0x80B)
- **输出结构**：`VMX_HOOK_LIST`（Count + `VMX_HOOK_INFO[]` 可变长数组）
- **调用方需预分配**：缓冲区大小应足够容纳预期的 Hook 数量

#### `DriverGetHookEvents`
```c
BOOL DriverGetHookEvents(HANDLE DeviceHandle, VMX_HOOK_EVENT_BUFFER *Buffer,
                         DWORD BufferSize, DWORD *BytesReturned)
```
- **功能**：从环形缓冲区读取 Hook 事件日志
- **IOCTL**：`IOCTL_VMX_GET_HOOK_EVENTS` (0x80C)
- **关键语义**：事件是"消耗性读取"（drain）—— 读取后从环形缓冲区移除，每次调用仅返回自上次读取后的新事件
- **环形缓冲区大小**：`HOOK_EVENT_RING_SIZE` = 512 条
- **输出结构**：`VMX_HOOK_EVENT_BUFFER`（Count + `HOOK_EVENT[]` 可变长数组）

### 3.5 SSDT 框架 IOCTL 封装

#### `DriverSsdtInit`
```c
BOOL DriverSsdtInit(HANDLE DeviceHandle, VMX_SSDT_INIT_RESPONSE *OutResponse)
```
- **功能**：初始化 SSDT 发现（扫描 `KiServiceTable`，解析地址和名称）
- **IOCTL**：`IOCTL_VMX_SSDT_INIT` (0x80D)
- **输出结构**：`VMX_SSDT_INIT_RESPONSE`（Success, ServiceCount, KiServiceTableVa, KiSystemCall64Va）

#### `DriverSsdtDump`
```c
BOOL DriverSsdtDump(HANDLE DeviceHandle, DWORD StartIndex, DWORD Count,
                    VMX_SSDT_DUMP_RESPONSE *Buffer, DWORD BufferSize, DWORD *BytesReturned)
```
- **功能**：转储 SSDT 表项
- **IOCTL**：`IOCTL_VMX_SSDT_DUMP` (0x80E)
- **输入结构**：`VMX_SSDT_DUMP_REQUEST`（StartIndex, Count）
- **输出结构**：`VMX_SSDT_DUMP_RESPONSE`（TotalServices, ReturnedCount + `SSDT_ENTRY_INFO[]`）

#### `DriverSsdtHook`
```c
BOOL DriverSsdtHook(HANDLE DeviceHandle, BOOL ByName, DWORD SyscallIndex,
                    const WCHAR *FunctionName, const HOOK_RULE *Rule,
                    VMX_SSDT_HOOK_RESPONSE *OutResponse)
```
- **功能**：Hook 指定 SSDT 表项
- **IOCTL**：`IOCTL_VMX_SSDT_HOOK` (0x80F)
- **两种模式**：通过系统调用索引或函数名称
- **输入结构**：`VMX_SSDT_HOOK_REQUEST`（ByName, SyscallIndex, FunctionName, Rule）
- **输出结构**：`VMX_SSDT_HOOK_RESPONSE`（HookId, SyscallIndex, FunctionVa, FunctionName）

#### `DriverSsdtUnhook`
```c
BOOL DriverSsdtUnhook(HANDLE DeviceHandle, BOOL ByHookId, DWORD HookId, DWORD SyscallIndex)
```
- **功能**：卸载指定 SSDT Hook
- **IOCTL**：`IOCTL_VMX_SSDT_UNHOOK` (0x810)
- **两种模式**：通过 HookId 或系统调用索引
- **输入结构**：`VMX_SSDT_UNHOOK_REQUEST`（ByHookId, HookId, SyscallIndex）

#### `DriverSsdtUnhookAll`
```c
BOOL DriverSsdtUnhookAll(HANDLE DeviceHandle)
```
- **功能**：卸载所有 SSDT Hook
- **IOCTL**：`IOCTL_VMX_SSDT_UNHOOK_ALL` (0x811)

#### `DriverSsdtListHooks`
```c
BOOL DriverSsdtListHooks(HANDLE DeviceHandle, VMX_SSDT_HOOK_LIST *Buffer,
                         DWORD BufferSize, DWORD *BytesReturned)
```
- **功能**：列出所有活跃的 SSDT Hook
- **IOCTL**：`IOCTL_VMX_SSDT_LIST_HOOKS` (0x812)
- **输出结构**：`VMX_SSDT_HOOK_LIST`（Count + `SSDT_HOOK_INFO[]` 可变长）

#### `DriverSsdtMonitor`
```c
BOOL DriverSsdtMonitor(HANDLE DeviceHandle, const VMX_SSDT_MONITOR_REQUEST *Request)
```
- **功能**：设置 SSDT 监控模式
- **IOCTL**：`IOCTL_VMX_SSDT_MONITOR` (0x813)
- **输入结构**：`VMX_SSDT_MONITOR_REQUEST`（Mode, TargetPid, FilterCount + FilterIndices[64]）
- **三种模式**：`SSDT_MONITOR_OFF=0` / `SSDT_MONITOR_ALL=1` / `SSDT_MONITOR_FILTERED=2`

### 3.6 Shadow SSDT 框架 IOCTL 封装

Shadow SSDT 封装函数与 SSDT 版本完全对应，差别仅在于：
- 所有函数名前缀为 `DriverShadowSsdt` 而非 `DriverSsdt`
- IOCTL 码范围为 `0x814` - `0x81A`
- 使用 `VMX_SHADOW_SSDT_*` 系列结构体
- 最大服务数常量 `SHADOW_SSDT_MAX_SERVICES = 2048`
- 过滤器最大数量 `SHADOW_SSDT_MONITOR_MAX_FILTER = 64`

具体函数包括：
- `DriverShadowSsdtInit` — IOCTL `0x814`，输出 `VMX_SHADOW_SSDT_INIT_RESPONSE`
- `DriverShadowSsdtDump` — IOCTL `0x815`，输出 `VMX_SHADOW_SSDT_DUMP_RESPONSE`
- `DriverShadowSsdtHook` — IOCTL `0x816`，输入/输出对应请求/响应结构体
- `DriverShadowSsdtUnhook` — IOCTL `0x817`
- `DriverShadowSsdtUnhookAll` — IOCTL `0x818`
- `DriverShadowSsdtListHooks` — IOCTL `0x819`，输出 `VMX_SHADOW_SSDT_HOOK_LIST`
- `DriverShadowSsdtMonitor` — IOCTL `0x81A`，输入 `VMX_SHADOW_SSDT_MONITOR_REQUEST`

这些函数的实现模式、参数逻辑与 SSDT 版本完全一致。

## 4. 控制流与逻辑流程

### 通用 IOCTL 调用模式
所有 IOCTL 封装函数遵循统一的模板：

```
1. 声明并零初始化输入/输出结构体（局部变量）
2. 填充结构体字段（从函数参数转换）
3. 调用 DeviceIoControl(..., &Input, sizeof(Input), OutBuffer, OutSize, &BytesReturned, NULL)
4. 返回 DeviceIoControl 的 BOOL 结果
```

### 特殊模式：DriverWriteMemory
```
1. 计算总缓冲区大小: sizeof(VMX_MEMORY_REQUEST) + Size
2. malloc 分配总缓冲区
3. 填充 VMX_MEMORY_REQUEST 头部
4. memcpy 负载数据到头部之后
5. DeviceIoControl（输入 = 总缓冲区，输出 = NULL）
6. free 总缓冲区
7. 返回结果
```

### 错误处理策略
- 所有函数直接返回 `DeviceIoControl` 的 BOOL 结果
- 调用方（`main.c` 中的 `Cmd*` 函数）检查返回值并调用 `GetLastError()` 获取详细错误码
- `DriverOpen` 在失败时提供用户友好的驱动加载指引信息

## 5. 与其他模块的交互

### 与 `main.c` 的协作
```
main.c (CLI 入口)
  |-- 解析命令行参数
  |-- DriverOpen() -> 获取设备句柄
  |-- Cmd*() 函数
  |     |-- 调用 driver_comm.c 函数
  |     |     |-- DeviceIoControl() -> 发送 IOCTL 到驱动
  |     |-- 打印结果/错误
  |-- DriverClose()
```

### 与 `vmxdrv.c`（驱动入口）的交互
`driver_comm.c` 不直接与 vmxdrv.c 通信，而是通过 `DeviceIoControl` 间接交互：
- IOCTL 码在 `shared.h` 中定义，驱动力图 (`vmxdrv.c`) 中的 `DispatchXxx` 例程解析
- 双方的协议完全由 `common/shared.h` 中的结构体定义约束

### 与 `common/shared.h` 的关系
`shared.h` 是整个用户态/内核态通信的合同文档：
- 设备路径常量
- 27 个 IOCTL 码
- 所有请求/响应结构体
- 所有选项标志、常量定义

## 6. 关键设计要点

### 设计模式：门面（Facade）模式
`driver_comm.c` 充当 `DeviceIoControl` 底层 API 的门面：

- **原始 API 问题**：`DeviceIoControl` 需要 8 个参数，且需要手动计算缓冲区大小、处理指针类型、序列化复杂参数
- **门面封装**：每个 IOCTL 对应一个类型安全的函数，隐藏了：
  - IOCTL 码的选择
  - 结构体的填充和零初始化
  - 参数到结构体字段的映射
  - `wcsncpy` 等字符串复制细节

### 统一的内存管理
- 简单 IOCTL（如 `DriverSetTarget`）使用栈上变量
- 需要组装数据的 IOCTL（`DriverWriteMemory`）使用 `malloc`/`free`
- 输出缓冲区由调用方预分配（`DriverGetLog`, `DriverListHooks` 等），调用方负责释放

### 安全考虑
- `METHOD_BUFFERED` IOCTL 模式：所有数据通过系统缓冲区拷贝，防止内核态直接访问用户态指针
- `wcsncpy` 带长度限制（`HOOK_MAX_NAME_LEN - 1`），防止缓冲区溢出
- `WriteMemory` 使用分配的总缓冲区包含头部和负载，驱动只需做一次拷贝
- 所有结构体在使用前 `memset` 零初始化，防止信息泄露或未初始化字段问题

### 性能设计
- 内存读取最大 64KB（`VMX_MEM_MAX_SIZE`），平衡单次 IOCTL 传输量和系统缓冲区大小限制
- 写入操作的"写后读验证"由调用方决定是否执行（`main.c` 的 `CmdWriteMem` 实现），底层不强制
- 事件日志使用消耗性读取，避免重复传输

### 局限性
- 所有 IOCTL 使用同步 `DeviceIoControl`，不提供异步 I/O（`OVERLAPPED`）
- `MethodBuffered` 模式限制了单次传输的最大数据量（取决于内核非分页池大小）
- 没有重试机制或连接状态跟踪 —— 每次调用都是独立事务
