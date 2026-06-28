# client/main.c — 逻辑分析

## 1. 文件概述

### 角色与职责
`main.c` 是 VMX Hypervisor Toolbox 的用户态 CLI 入口点。其核心职责包括：

- **命令行参数解析**：处理 40+ 个命令行选项，转换为内部命令标志和参数
- **命令执行派遣**：根据解析结果调用对应的 `Cmd*` 执行函数
- **用户交互**：向控制台输出格式化结果，包括表格、十六进制转储、状态信息
- **输出格式化**：将驱动返回的二进制数据（Hook 列表、SSDT 表项、日志、内存等）格式化为人类可读输出

### 依赖的其他模块
| 模块 | 头文件 | 用途 |
|------|--------|------|
| 驱动通信 | `driver_comm.h` | 调用 `DriverOpen`/`DriverClose` 及所有 IOCTL 封装函数 |
| 共享定义 | `../common/shared.h` | IOCTL 码、共享结构体、`AAD_HIDE_*` 标志、`HOOK_ACTION_*` 常量 |
| Windows API | `<windows.h>` | `HANDLE`, `DWORD`, `ULONG64` 等类型定义 |

### 文件规模
约 2258 行，包括：
- 前置文档（用法说明）63 行
- Banner 和用法打印函数 2 个
- 工具辅助函数 2 个（`HookActionToStr`, `LogLevelToStr`, `HexDump`, `ParseHexBytes`）
- 命令执行函数 22 个
- `main()` 入口函数 ~480 行（参数解析 + 命令派遣）

## 2. 数据结构

本文件使用栈上局部变量存储所有中间状态，不定义全局或持久化数据结构。

### 关键局部变量组

#### `main()` 中的状态变量
```
Pid               DWORD   -- 目标进程 ID（来自 --pid）
Flags             DWORD   -- AAD_HIDE_* 位掩码组合
DoRemove          BOOL    -- 是否执行移除目标
DoStatus          BOOL    -- 是否查询状态
DoStop            BOOL    -- 是否停止引擎
DoLog             BOOL    -- 是否显示日志
```

#### Hook 框架参数
```
HookFuncName[HOOK_MAX_NAME_LEN]  char[]  -- 要 Hook 的函数名
HookAddress                      ULONG64 -- 要 Hook 的地址
RemoveHookId                     DWORD   -- 要移除的 Hook ID
HookPid                          DWORD   -- Hook 的 PID 过滤器
HookRule                         HOOK_RULE -- Hook 行为规则
```

#### 内存读写参数
```
MemPid       DWORD    -- 目标进程 ID
MemAddress   ULONG64  -- 目标地址
MemSize      DWORD    -- 读取大小（默认 64 字节）
MemHexData[1024] char[] -- 要写入的十六进制字符串
```

#### SSDT/Shadow SSDT 参数
每组包含对应的：
- `Do*` BOOL 命令开关
- `*Target[SSDT_MAX_NAME_LEN]` char[] 目标函数名/索引
- `*DumpStart`, `*DumpCount` DWORD 转储范围
- `*MonitorMode` DWORD 监控模式
- `*FilterIndices[]` DWORD 过滤器索引数组
- `*FilterCount` DWORD 过滤器数量

### `HOOK_RULE` 默认值
```c
HookRule.Action = HOOK_ACTION_LOG_ONLY;  // 默认动作 = LOG_ONLY
```
日志记录默认关闭，需要显式指定 `--hook-log` 开启。

## 3. 核心函数详解

### 3.1 辅助工具函数

#### `PrintBanner`
```c
static void PrintBanner(void)
```
- **功能**：打印程序启动横幅
- **输出**：一个简单的 ASCII 艺术框架，显示 "VMX Hypervisor Toolbox" 和 "Intel VT-x / AMD SVM Platform"
- **调用位置**：`main()` 第一行

#### `PrintUsage`
```c
static void PrintUsage(const char *Argv0)
```
- **功能**：打印完整的使用说明
- **参数**：`Argv0` — 程序路径（用于示例中显示）
- **覆盖范围**：所有命令组（反反调试、Hook 框架、内存读写、SSDT、Shadow SSDT）
- **包含**：每个命令组的示例用法

#### `HookActionToStr`
```c
static const char *HookActionToStr(ULONG Action)
```
- **功能**：将 `HOOK_ACTION_*` 数值常量转换为人类可读字符串
- **映射**：
  - `0` → `"PASSTHROUGH"`
  - `1` → `"LOG_ONLY"`
  - `2` → `"BLOCK"`
  - `3` → `"MODIFY_RETVAL"`
  - 其他 → `"UNKNOWN"`

#### `LogLevelToStr`
```c
static const char *LogLevelToStr(ULONG Level)
```
- **功能**：将日志级别数值转换为 3 字符缩写
- **映射**：
  - `0` → `"ERR"`
  - `1` → `"WRN"`
  - `2` → `"INF"`
  - `3` → `"DBG"`
  - 其他 → `"???"`

#### `HexDump`
```c
static void HexDump(ULONG64 BaseAddr, const BYTE *Data, DWORD Size)
```
- **功能**：经典十六进制 + ASCII 转储
- **格式**：每行 16 字节，含地址偏移、十六进制值（中间 8 字节后加空格分隔）、ASCII 显示
- **不可打印字符替换**：非 `0x20`-`0x7E` 范围显示为 `'.'`

#### `ParseHexBytes`
```c
static DWORD ParseHexBytes(const char *HexStr, BYTE *OutBuffer, DWORD MaxSize)
```
- **功能**：将十六进制字符串解析为字节数组
- **特性**：
  - 自动跳过 `0x`/`0X` 前缀
  - 输入必须是偶数长度
  - 每 2 个字符转换为 1 个字节
  - 支持最大 512 字节输出（`WriteBuffer[512]` 限制）
- **返回值**：成功解析的字节数，失败返回 0
- **错误条件**：空字符串、奇数长度、超过 MaxSize

### 3.2 反反调试命令函数

#### `CmdSetTarget`
```c
static int CmdSetTarget(HANDLE hDevice, DWORD Pid, DWORD Flags)
```
- **功能**：设置目标进程并应用反反调试特性
- **参数**：
  - `hDevice` — 驱动设备句柄
  - `Pid` — 目标进程 ID
  - `Flags` — `AAD_HIDE_*` 位掩码
- **核心流程**：
  1. 调用 `DriverInitVmx` 初始化 VMX（如果尚未初始化）
  2. 忽略 `ERROR_ALREADY_REGISTERED`（已初始化是正常情况）
  3. 打印当前启用的每个反反调试特性
  4. 调用 `DriverSetTarget` 发送 IOCTL
  5. 成功时提示用户可以附加调试器
- **返回值**：0=成功, 1=失败

#### `CmdRemoveTarget`
```c
static int CmdRemoveTarget(HANDLE hDevice, DWORD Pid)
```
- **功能**：移除对目标进程的保护
- **核心流程**：
  1. 调用 `DriverRemoveTarget`
  2. 成功时打印移除确认信息
- **返回值**：0=成功, 1=失败

#### `CmdQueryStatus`
```c
static int CmdQueryStatus(HANDLE hDevice)
```
- **功能**：查询 VMX 引擎运行状态并打印
- **输出内容**：
  - VMX Active（是否激活）
  - CPU Count（虚拟化的 CPU 数量）
  - Active Targets（活跃目标数）
  - Total VM-Exits（总 VM-Exit 次数）
- **返回值**：0=成功, 1=失败

#### `CmdStop`
```c
static int CmdStop(HANDLE hDevice)
```
- **功能**：停止 VMX 引擎
- **返回值**：0=成功, 1=失败

#### `CmdShowLog`
```c
static int CmdShowLog(HANDLE hDevice)
```
- **功能**：获取并显示驱动内部的环形缓冲区日志
- **缓冲区预分配**：`sizeof(VMX_LOG_BUFFER) + sizeof(VMX_LOG_ENTRY) * 100`
- **输出格式**：表格形式，列包括 `LEVEL`, `PID`, `MESSAGE`
- **空日志处理**：显示 `"(no log entries)"`
- **内存管理**：`malloc`/`free`，失败返回 1

### 3.3 Hook 框架命令函数

#### `CmdInstallHook`
```c
static int CmdInstallHook(HANDLE hDevice, const char *FuncName,
                          DWORD HookPid, const HOOK_RULE *Rule)
```
- **功能**：通过内核导出函数名安装函数 Hook
- **参数**：
  - `FuncName` — 内核导出函数名（如 `"NtOpenProcess"`）
  - `HookPid` — PID 过滤器（0=全局）
  - `Rule` — 钩子行为规则
- **核心流程**：
  1. 调用 `DriverInitVmx` 初始化 VMX
  2. 将 ANSI 函数名逐字符转换为宽字符（WideChar）
  3. 打印 Hook 参数总览（Action、PID Filter、RetVal、Logging）
  4. 调用 `DriverInstallHook(hDevice, TRUE, WideName, 0, HookPid, Rule, &Response)`
  5. 打印返回的 HookId 和解析的地址
- **参数验证**：`FuncName` 必须非空（由调用方保证）

#### `CmdInstallHookAddr`
```c
static int CmdInstallHookAddr(HANDLE hDevice, ULONG64 Address,
                              DWORD HookPid, const HOOK_RULE *Rule)
```
- **功能**：通过直接虚拟地址安装 Hook
- **参数**：
  - `Address` — 内核空间中的目标函数地址（十六进制）
- **与 CmdInstallHook 区别**：
  - 调用 `DriverInstallHook(hDevice, FALSE, NULL, Address, HookPid, Rule, &Response)`
  - 即 `ByName=FALSE`，直接使用传入地址
  - 无需函数名 -> 宽字符转换
- **适用场景**：Hook 非导出函数或动态计算地址

#### `CmdRemoveHook`
```c
static int CmdRemoveHook(HANDLE hDevice, DWORD HookId)
```
- **功能**：通过 HookId 移除 Hook
- **错误处理**：失败时提示用户使用 `--list-hooks` 检查 Hook ID

#### `CmdListHooks`
```c
static int CmdListHooks(HANDLE hDevice)
```
- **功能**：列出所有活跃 Hook 的详细信息
- **缓冲区预分配**：`sizeof(VMX_HOOK_LIST) + sizeof(VMX_HOOK_INFO) * 1024`
- **输出表格列**：ID, Active, Address, Action, PID, HitCount, Function
- **每行扩展信息**：
  - 如果 `ProcessId != 0`，额外显示 `PID Filter: <pid>`
  - 如果 Action == BLOCK，显示 `Block RetVal`
  - 如果 Action == MODIFY_RETVAL，显示 `New RetVal`
  - 如果 LogEnabled，显示 `Logging: ON`
- **宽字符转换**：`FunctionName[WCHAR]` 逐字符转换为 `char[]`

#### `CmdHookEvents`
```c
static int CmdHookEvents(HANDLE hDevice)
```
- **功能**：读取并显示 Hook 事件环形缓冲区内容
- **缓冲区**：预分配包含 `HOOK_EVENT_RING_SIZE = 512` 条事件的空间
- **重要语义**："事件被消耗性读取" —— 显示后从环形缓冲区移除
- **输出表格列**：HookID, PID, Timestamp, CallerAddr, RetVal, Action

### 3.4 内存读写命令函数

#### `CmdReadMem`
```c
static int CmdReadMem(HANDLE hDevice, DWORD Pid, ULONG64 Address, DWORD Size)
```
- **功能**：通过 Hypervisor 物理内存访问读取目标进程内存
- **参数校验**：
  - `Size == 0` 或 `Size > VMX_MEM_MAX_SIZE (65536)` 返回错误
- **核心流程**：
  1. 初始化 VMX（如未初始化）
  2. 分配 Size 字节的读取缓冲区
  3. 调用 `DriverReadMemory`
  4. 成功时调用 `HexDump` 显示十六进制转储
- **错误提示**：失败时列出可能原因（PID 不存在、地址未映射、VMX 未初始化）

#### `CmdWriteMem`
```c
static int CmdWriteMem(HANDLE hDevice, DWORD Pid, ULONG64 Address, const char *HexData)
```
- **功能**：通过 Hypervisor 写入目标进程内存，并回读验证
- **核心流程**：
  1. 初始化 VMX
  2. 调用 `ParseHexBytes` 将十六进制字符串解析为字节数组（最大 512 字节）
  3. 写入前显示将要写入的数据（`HexDump`）
  4. 调用 `DriverWriteMemory` 写入
  5. 写入后**自动回读验证**：
     - 如果回读成功且数据匹配：`Verification PASSED`
     - 如果回读成功但数据不匹配：显示实际读回的数据
     - 如果回读失败：提示错误但不影响写入结果判定
- **关键设计**：写入 + 验证是防弹实践，确保 Hypervisor 物理写入确实生效

#### `CmdDumpMem`
```c
static int CmdDumpMem(HANDLE hDevice, DWORD Pid, ULONG64 Address, DWORD Size)
```
- **功能**：大范围内存区域的全十六进制 + ASCII 转储
- **分块读取**：当 `Size > VMX_MEM_MAX_SIZE` (64KB) 时分块读取
- **块循环逻辑**：
  ```
  TotalRead = 0
  while (TotalRead < Size):
      ReadSize = min(Size - TotalRead, VMX_MEM_MAX_SIZE)
      DriverReadMemory(hDevice, Pid, CurrentAddr, Buffer, ReadSize, &BytesReturned)
      if (失败 && TotalRead == 0) -> 致命错误，退出
      if (失败 && TotalRead > 0)  -> 显示 partial read，退出
      HexDump(CurrentAddr, Buffer, BytesReturned)
      TotalRead += BytesReturned
      CurrentAddr += BytesReturned
      if (BytesReturned < ReadSize) -> 显示 partial read，退出
  ```
- **优雅降级**：如果大区域中间有未映射页面，显示已成功读取的部分并提示中断原因

### 3.5 SSDT 框架命令函数

#### `CmdSsdtInit`
```c
static int CmdSsdtInit(HANDLE hDevice)
```
- **功能**：初始化 SSDT 发现扫描
- **输出信息**：
  - `KiSystemCall64` 地址（系统调用入口）
  - `KiServiceTable` 地址（服务表基址）
  - `ServiceCount`（发现的服务数）
- **双重检查**：检查 `DriverSsdtInit` 返回值 + `Response.Success` 字段

#### `CmdSsdtDump`
```c
static int CmdSsdtDump(HANDLE hDevice, DWORD StartIndex, DWORD Count)
```
- **功能**：转储 SSDT 表项
- **参数边界**：`Count` 限制在 `[0, SSDT_MAX_SERVICES=512]` 范围内，0 表示全部
- **缓冲区计算**：`sizeof(VMX_SSDT_DUMP_RESPONSE) + sizeof(SSDT_ENTRY_INFO) * Count`
- **输出表格列**：Index, Address, Args, RawOffset, Name
- **空名称处理**：`FunctionName` 为空时显示 `"(unknown)"`

#### `CmdSsdtHook`
```c
static int CmdSsdtHook(HANDLE hDevice, const char *Target,
                        DWORD HookPid, const HOOK_RULE *Rule)
```
- **功能**：Hook SSDT 表项
- **参数`Target`解析**：智能判断目标类型
  - 如果包含字母 → 视为函数名（`ByName=TRUE`）
  - 如果仅数字 → 视为系统调用索引（`ByName=FALSE`）
- **宽字符转换**：函数名逐字符转换为 WCHAR
- **输出信息**：HookId, SyscallIndex, FunctionVa, FunctionName

#### `CmdSsdtUnhook`
```c
static int CmdSsdtUnhook(HANDLE hDevice, const char *Target)
```
- **功能**：卸载 SSDT Hook
- **`Target`格式**：
  - `"hookid:N"` → 按 HookId 卸载（`ByHookId=TRUE`）
  - `"N"`（纯数字）→ 按系统调用索引卸载（`ByHookId=FALSE`）
- **解析逻辑**：使用 `strncmp(Target, "hookid:", 7)` 区分两种模式

#### `CmdSsdtUnhookAll`
```c
static int CmdSsdtUnhookAll(HANDLE hDevice)
```
- **功能**：卸载所有 SSDT Hook
- **IOCTL**：`IOCTL_VMX_SSDT_UNHOOK_ALL` (0x811)，无输入/输出缓冲区

#### `CmdSsdtList`
```c
static int CmdSsdtList(HANDLE hDevice)
```
- **功能**：列出所有活跃的 SSDT Hook
- **缓冲区**：预分配 64 条 `SSDT_HOOK_INFO` 空间
- **输出表格列**：HookID, Index, Address, Action, PID, HitCount, Function
- **与 CmdListHooks 类似的扩展信息格式**

#### `CmdSsdtMonitor`
```c
static int CmdSsdtMonitor(HANDLE hDevice, DWORD Mode, DWORD MonitorPid,
                          const DWORD *FilterIndices, DWORD FilterCount)
```
- **功能**：设置 SSDT 监控模式
- **三种模式**：
  - `SSDT_MONITOR_OFF=0` — 关闭监控
  - `SSDT_MONITOR_ALL=1` — 监控所有系统调用
  - `SSDT_MONITOR_FILTERED=2` — 仅监控指定索引
- **FILTERED 模式**：接收 `FilterIndices` 数组 + `FilterCount`，最多 `SSDT_MONITOR_MAX_FILTER` (64) 个
- **输出**：打印监控状态变化

### 3.6 Shadow SSDT（Win32k）框架命令函数

Shadow SSDT 命令函数与 SSDT 版本在逻辑上完全对应，差异点：

| 函数 | 对应 SSDT 版本 | 关键差异 |
|------|---------------|---------|
| `CmdShadowSsdtInit` | `CmdSsdtInit` | 输出 `VMX_SHADOW_SSDT_INIT_RESPONSE`（含 W32pServiceTableVa, Win32kBase），提示需要先调用 `--ssdt-init` |
| `CmdShadowSsdtDump` | `CmdSsdtDump` | `Count` 上限为 `SHADOW_SSDT_MAX_SERVICES=2048` |
| `CmdShadowSsdtHook` | `CmdSsdtHook` | 调用 `DriverShadowSsdtHook`，输出 `VMX_SHADOW_SSDT_HOOK_RESPONSE` |
| `CmdShadowSsdtUnhook` | `CmdSsdtUnhook` | 调用 `DriverShadowSsdtUnhook` |
| `CmdShadowSsdtUnhookAll` | `CmdSsdtUnhookAll` | 调用 `DriverShadowSsdtUnhookAll` |
| `CmdShadowSsdtList` | `CmdSsdtList` | 使用 `VMX_SHADOW_SSDT_HOOK_LIST` / `SHADOW_SSDT_HOOK_INFO` |
| `CmdShadowSsdtMonitor` | `CmdSsdtMonitor` | 使用 `VMX_SHADOW_SSDT_MONITOR_REQUEST`，过滤器最大 64 条目 |

## 4. 控制流与逻辑流程

### `main()` 完整执行流程

```
main(argc, argv)
  |
  ├── PrintBanner()             -- 显示启动横幅
  |
  ├── 初始化所有局部变量为零    -- Pid, Flags, 所有 Do* 开关, HookRule 等
  ├── HookRule.Action = HOOK_ACTION_LOG_ONLY  -- 设置默认动作
  |
  ├── if (argc < 2) → PrintUsage + return 1
  |
  ├── 第一遍遍历: 参数解析循环 (for i = 1 to argc-1)
  |   ├── --pid N           → Pid = N
  |   ├── --hide-*          → Flags |= AAD_HIDE_*
  |   ├── --remove          → DoRemove = TRUE
  |   ├── --status          → DoStatus = TRUE
  |   ├── --stop            → DoStop = TRUE
  |   ├── --log             → DoLog = TRUE
  |   ├── --install-hook    → DoInstallHook = TRUE, 保存 FuncName
  |   ├── --install-hook-addr → DoInstallHookAddr = TRUE, 保存 Address
  |   ├── --remove-hook N   → DoRemoveHook = TRUE, 保存 RemoveHookId
  |   ├── --list-hooks      → DoListHooks = TRUE
  |   ├── --hook-events     → DoHookEvents = TRUE
  |   ├── --action N/name   → 设置 HookRule.Action (支持数字和命名)
  |   ├── --hook-pid N      → HookPid = N, HookRule.TargetPid = N
  |   ├── --block-retval X  → HookRule.BlockReturnValue = X
  |   ├── --new-retval X    → HookRule.NewReturnValue = X
  |   ├── --hook-log        → HookRule.LogEnabled = TRUE
  |   ├── --read-mem ...    → DoReadMem = TRUE, 保存参数
  |   ├── --write-mem ...   → DoWriteMem = TRUE, 保存参数
  |   ├── --dump-mem ...    → DoDumpMem = TRUE, 保存参数
  |   ├── --ssdt-*          → 设置对应 SSDT Do* 开关和参数
  |   ├── --shadow-ssdt-*   → 设置对应 Shadow SSDT Do* 开关和参数
  |   ├── --help/-h         → PrintUsage + return 0
  |   └── 未知选项          → 打印错误 + return 1
  |
  ├── 命令存在性判定
  |   ├── IsHookCmd = DoInstallHook || DoInstallHookAddr || DoRemoveHook || DoListHooks || DoHookEvents
  |   ├── IsMemCmd = DoReadMem || DoWriteMem || DoDumpMem
  |   ├── IsSsdtCmd = DoSsdt* (7 种)
  |   └── IsShadowSsdtCmd = DoShadowSsdt* (7 种)
  |
  ├── 参数验证阶段
  |   ├── 无命令 + Pid==0    → 错误: --pid required
  |   ├── Pid!=0 + 无操作    → 错误: no hide options
  |   ├── --install-hook 无名称 → 错误
  |   ├── --install-hook-addr 零地址 → 错误
  |   ├── IsMemCmd && MemPid==0 → 错误: need PID
  |   ├── IsMemCmd && MemAddress==0 → 错误: need address
  |   └── DoWriteMem && MemHexData 为空 → 错误
  |
  ├── DriverOpen(&hDevice)   -- 打开驱动设备
  |   └── 失败 → return 1
  |
  ├── 命令执行派遣 (if-else 链, 按优先级)
  |   ├── DoInstallHook       → CmdInstallHook(...)
  |   ├── DoInstallHookAddr   → CmdInstallHookAddr(...)
  |   ├── DoRemoveHook        → CmdRemoveHook(...)
  |   ├── DoListHooks         → CmdListHooks(...)
  |   ├── DoHookEvents        → CmdHookEvents(...)
  |   ├── DoReadMem           → CmdReadMem(...)
  |   ├── DoWriteMem          → CmdWriteMem(...)
  |   ├── DoDumpMem           → CmdDumpMem(...)
  |   ├── DoSsdtInit          → CmdSsdtInit(...)
  |   ├── DoSsdtDump          → CmdSsdtDump(...)
  |   ├── DoSsdtHook          → CmdSsdtHook(...)
  |   ├── DoSsdtUnhook        → CmdSsdtUnhook(...)
  |   ├── DoSsdtUnhookAll     → CmdSsdtUnhookAll(...)
  |   ├── DoSsdtList          → CmdSsdtList(...)
  |   ├── DoSsdtMonitor       → CmdSsdtMonitor(...)
  |   ├── DoShadowSsdtInit    → CmdShadowSsdtInit(...)
  |   ├── DoShadowSsdtDump    → CmdShadowSsdtDump(...)
  |   ├── DoShadowSsdtHook    → CmdShadowSsdtHook(...)
  |   ├── DoShadowSsdtUnhook  → CmdShadowSsdtUnhook(...)
  |   ├── DoShadowSsdtUnhookAll → CmdShadowSsdtUnhookAll(...)
  |   ├── DoShadowSsdtList    → CmdShadowSsdtList(...)
  |   ├── DoShadowSsdtMonitor → CmdShadowSsdtMonitor(...)
  |   ├── DoStatus            → CmdQueryStatus(...)
  |   ├── DoStop              → CmdStop(...)
  |   ├── DoLog               → CmdShowLog(...)
  |   ├── DoRemove            → CmdRemoveTarget(...)
  |   └── (默认)              → CmdSetTarget(...)
  |
  └── DriverClose(hDevice)   -- 关闭驱动句柄
  └── return Result
```

### 命令执行优先级

`main()` 的 if-else 链定义了命令优先级：

1. **Hook 相关命令**（最高优先级）：`--install-hook`, `--install-hook-addr`, `--remove-hook`, `--list-hooks`, `--hook-events`
2. **内存读写**: `--read-mem`, `--write-mem`, `--dump-mem`
3. **SSDT 命令**: `--ssdt-*`
4. **Shadow SSDT 命令**: `--shadow-ssdt-*`
5. **状态/控制命令**: `--status`, `--stop`, `--log`
6. **目标管理命令**: `--remove` (移除目标)
7. **默认**: `CmdSetTarget` (设置目标 + 反反调试)

注意：在同一命令组内同时指定多个命令时，只有第一个匹配的会执行。

### 参数解析设计细节

#### Hook 动作的灵活解析
```c
if (_stricmp(ActionStr, "pass") == 0 || _stricmp(ActionStr, "passthrough") == 0)
    HookRule.Action = HOOK_ACTION_PASSTHROUGH;
else if (_stricmp(ActionStr, "log") == 0) ...
else if (_stricmp(ActionStr, "block") == 0) ...
else if (_stricmp(ActionStr, "modify") == 0) ...
else HookRule.Action = (ULONG)atoi(ActionStr);
```
用户可以使用 **数字** 或 **人类可读名称** 指定动作。

#### SSDT 目标智能识别
```c
/* 检测字符串是否含字母 -> 判断为函数名 */
while (*p) {
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        ByName = TRUE; break;
    } p++;
}
```
简单但有效的启发式判断：含字母视为函数名，纯数字视为系统调用索引。

#### 可选参数处理
```
--ssdt-dump [start] [count]
--read-mem <PID> <addr> [size]
```
通过检查 `argv[i+1][0] != '-'` 判断下一个 token 是否是可选参数：
```c
if (i + 1 < argc && argv[i + 1][0] != '-') {
    // 这是可选参数
}
```

#### 逗号分隔列表解析
```c
// --ssdt-filter 35,38,55
Token = strtok(FilterBuf, ",");
while (Token && SsdtFilterCount < SSDT_MONITOR_MAX_FILTER) {
    SsdtFilterIndices[SsdtFilterCount++] = (DWORD)atoi(Token);
    Token = strtok(NULL, ",");
}
```
使用 `strtok` 分割逗号分隔的系统调用索引列表。

## 5. 与其他模块的交互

### 与 `driver_comm.c` 的完整调用映射

```
main.c 中的 Cmd* 函数          driver_comm.c 的函数
----------------------         ---------------------
CmdSetTarget()          ──>    DriverInitVmx() + DriverSetTarget()
CmdRemoveTarget()       ──>    DriverInitVmx() + DriverRemoveTarget()
CmdQueryStatus()        ──>    DriverQueryStatus()
CmdStop()               ──>    DriverStopVmx()
CmdShowLog()            ──>    DriverGetLog()
CmdInstallHook()        ──>    DriverInitVmx() + DriverInstallHook()
CmdInstallHookAddr()    ──>    DriverInitVmx() + DriverInstallHook()
CmdRemoveHook()         ──>    DriverRemoveHook()
CmdListHooks()          ──>    DriverListHooks()
CmdHookEvents()         ──>    DriverGetHookEvents()
CmdReadMem()            ──>    DriverInitVmx() + DriverReadMemory()
CmdWriteMem()           ──>    DriverInitVmx() + DriverWriteMemory()
CmdDumpMem()            ──>    DriverInitVmx() + DriverReadMemory() (循环)
CmdSsdtInit()           ──>    DriverInitVmx() + DriverSsdtInit()
CmdSsdtDump()           ──>    DriverSsdtDump()
CmdSsdtHook()           ──>    DriverInitVmx() + DriverSsdtHook()
CmdSsdtUnhook()         ──>    DriverSsdtUnhook()
CmdSsdtUnhookAll()      ──>    DriverSsdtUnhookAll()
CmdSsdtList()           ──>    DriverSsdtListHooks()
CmdSsdtMonitor()        ──>    DriverInitVmx() + DriverSsdtMonitor()
CmdShadowSsdt*()        ──>    DriverInitVmx() + DriverShadowSsdt*()
```

### 与 `common/shared.h` 的数据流

```
命令行参数 (字符串)
  │
  ▼
main.c 解析为结构化参数 (Pid, Flags, HookRule, MemPid, ...)
  │
  ▼
driver_comm.c 填充 shared.h 结构体 (VMX_TARGET_INFO, VMX_MEMORY_REQUEST, ...)
  │
  ▼
DeviceIoControl 发送到内核驱动 vmxdrv.c 的 Dispatch 例程
  │
  ▼
共享结构体作为协议契约
```

## 6. 关键设计要点

### 设计模式：命令模式（Command Pattern）
`main.c` 实现了命令模式的变体：

- **命令封装**：每个 `Do*` BOOL 变量 + 相关参数变量 构成一个命令的描述
- **解析器**：参数解析循环将命令行字符串 -> 命令描述
- **派遣器**：if-else 链根据命令描述调用对应的 `Cmd*` 函数
- **执行器**：`Cmd*` 函数调用 `driver_comm.c` 完成实际操作

### 容错性设计

#### `ERROR_ALREADY_REGISTERED` 处理
所有需要 `DriverInitVmx` 的命令都正确处理了驱动已初始化的情况：
```c
if (!DriverInitVmx(hDevice)) {
    DWORD Err = GetLastError();
    if (Err != ERROR_ALREADY_REGISTERED && Err != 0) {
        // 真正错误，上报
        return 1;
    }
    // 已初始化是正常情况，继续
}
```

#### 内存读写的大区域处理
`CmdDumpMem` 实现了大区域（>64KB）的分块读取，具备：
- 优雅降级：区域中间遇到未映射页面不整体失败
- 逐块转储：每次读取成功立刻显示，用户无需等待全部完成
- 详细中断报告：显示中断位置和原因

#### 写入验证
`CmdWriteMem` 在写入后自动回读验证，确保 EPT/NPT 物理写入确实生效。

### 输出格式化

#### 统一的信息层次
所有输出遵循三个前缀约定：
- `[*]` — 操作信息/状态
- `[+]` — 成功确认
- `[!]` — 错误/警告

#### 表格对齐
列表命令（`CmdListHooks`, `CmdSsdtList`, `CmdSsdtDump` 等）使用固定宽度格式化字符串实现列对齐，同时包含表头分隔行。

### 安全性考量

#### 缓冲区边界
- `HOOK_MAX_NAME_LEN` (128) 和 `SSDT_MAX_NAME_LEN` (128) 提供了函数名的硬限制
- `strncpy` 用于所有字符串复制，指定最大长度
- `WriteBuffer[512]` 限制单次写入大小

#### 参数校验
- 内存命令检查 Pid 和 Address 非零
- Hook 地址命令检查地址非零
- 数字解析使用 `atoi` 和 `_strtoui64`（带基数指定）

### 局限性

#### 无命令组合
`main()` 的 if-else 链只允许执行单个命令。例如 `--pid 1234 --hide-all --status` 会忽略 `--status`（因为 `--pid` 触发的 `CmdSetTarget` 不是最后的 else if）。

#### 宽字符串转换
所有宽字符转换使用逐字符转换而非 `MultiByteToWideChar`，不支持 UTF-8 输入，但这对内核函数名（纯 ASCII）不是问题。

#### 无循环/持续监控
- `--hook-events` 是单次快照读取，不提供持续的 `tail -f` 风格监控
- 需要用户在脚本中循环调用实现实时监控

#### 错误恢复
- 没有自动重试机制
- 失败时直接返回退出码 1，没有备选操作路径
