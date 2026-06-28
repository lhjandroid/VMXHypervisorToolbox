# log.c -- 逻辑分析

## 1. 文件概述

### 角色与职责

`log.c` 是 VMX Hypervisor Toolbox 的统一日志子系统，提供一套完全无锁的环形缓冲区日志框架，是整个驱动中最重要的基础设施模块之一。其核心职责包括：

- **无锁日志写入**：在任意 IRQL（包括 VMX Root 模式、DPC、IPI）下安全写入日志
- **日志刷出**：通过系统线程将环形缓冲区中的日志条目定期输出到 WinDbg（`DbgPrintEx`）
- **用户态日志读取**：通过 `IOCTL_VMX_GET_LOG` 支持用户态工具读取日志（如 `VMXToolbox.exe --log`）

### 设计哲学

```
统一架构（Unified Architecture）：

LogWrite（任意 IRQL，安全）：
  - 格式化消息到栈局部缓冲区
  - 仅使用 InterlockedIncrement 写入环形缓冲区（无锁）
  - 不做 DbgPrintEx -- 不触发 INT 3，无死锁风险
  - 被所有 LOG_* 和 VMXROOT_LOG_* 宏使用

刷出线程（PASSIVE_LEVEL）：
  - 系统线程，每 5ms 轮询环形缓冲区
  - 通过 DbgPrintEx 输出新条目到 WinDbg
  - 提供约 5ms 最大延迟的实时输出

读取路径（IOCTL 上下文，PASSIVE_LEVEL）：
  - LogRead 从环形缓冲区读取条目
  - 使用 InterlockedCompareExchange 进行安全的计数递减
```

### 依赖的其他模块

| 头文件 | 用途 |
|--------|------|
| `log.h` | 日志结构体定义、函数声明、便利宏 |
| `shared.h` | `VMX_LOG_ENTRY`、`VMX_LOG_BUFFER` 结构体和日志级别常量 |
| `ntstrsafe.h` | `RtlStringCbVPrintfA` 安全字符串格式化 |

---

## 2. 数据结构

### 2.1 `LOG_RING_BUFFER`（log.h 定义）

```c
typedef struct _LOG_RING_BUFFER {
    VMX_LOG_ENTRY   Entries[LOG_RING_BUFFER_ENTRIES];  // 8192 条日志条目
    volatile LONG   Ready[LOG_RING_BUFFER_ENTRIES];     // 8192 个就绪标志
    volatile LONG   WriteIndex;      // 原子递增的写位置
    volatile LONG   Count;           // 当前条目计数（饱和至 ENTRIES）
    volatile LONG   ReadIndex;       // 读位置（仅 LogRead 使用）
    volatile LONG   FlushIndex;      // 刷出线程的当前位置
    BOOLEAN         Initialized;     // 缓冲区是否已初始化

    // 刷出线程状态
    HANDLE          FlushThreadHandle;
    PETHREAD        FlushThreadObject;
    KEVENT          FlushStopEvent;  // 通知刷出线程退出的事件
    BOOLEAN         FlushThreadRunning;
} LOG_RING_BUFFER, *PLOG_RING_BUFFER;
```

#### 字段说明

| 字段 | 用途 | 访问者 |
|------|------|--------|
| `Entries[]` | 环形日志条目数组，8192 条 | 写：LogWrite；读：刷出线程、LogRead |
| `Ready[]` | 每槽就绪标志，1=数据已写入可读，0=空/写入中 | 写：LogWrite（`InterlockedExchange` 设为1）；读：刷出线程、LogRead（检查后清0） |
| `WriteIndex` | 下一个可写入的位置，原子递增 | `InterlockedIncrement` 在 LogWrite 中独占写入 |
| `Count` | 当前有效条目数，饱和到 8192 | 写：LogWrite（CAS 递增）；读：LogRead（CAS 递减）；刷出线程不影响 Count |
| `ReadIndex` | IOCTL 读取位置，独立于刷出线程 | 仅 LogRead 使用 |
| `FlushIndex` | 刷出线程最后处理的条目位置 | 仅刷出线程写入 |
| `Initialized` | 缓冲区是否就绪 | LogWrite 和 LogRead 检查此标志 |

### 2.2 `VMX_LOG_ENTRY`（shared.h 定义）

```c
typedef struct _VMX_LOG_ENTRY {
    ULONG           Level;       // 0=Error, 1=Warn, 2=Info, 3=Debug
    ULONG           Pid;         // 源进程 ID（0 = 系统）
    LARGE_INTEGER   Timestamp;   // 系统时间戳
    CHAR            Message[VMX_LOG_MAX_MSG];  // 256 字节消息
} VMX_LOG_ENTRY, *PVMX_LOG_ENTRY;
```

### 2.3 日志级别常量（shared.h）

| 常量 | 值 | 字符串 |
|------|-----|--------|
| `VMX_LOG_ERROR` | 0 | "ERR" |
| `VMX_LOG_WARN` | 1 | "WRN" |
| `VMX_LOG_INFO` | 2 | "INF" |
| `VMX_LOG_DEBUG` | 3 | "DBG" |

### 2.4 全局日志缓冲区

```c
LOG_RING_BUFFER g_LogBuffer = { 0 };
```

全局实例，在 `log.c` 中定义，通过 `log.h` 的 `extern` 声明暴露给其他模块。

### 2.5 日志级别字符串表

```c
static const CHAR *LogLevelStr[] = { "ERR", "WRN", "INF", "DBG" };
```

---

## 3. 核心函数详解

### 3.1 `LogInitialize`

```c
NTSTATUS LogInitialize(VOID)
```

**功能**：初始化日志环形缓冲区。

**核心流程**：
1. `RtlZeroMemory` 清零整个 `g_LogBuffer`
2. 设置 `WriteIndex = 0`、`ReadIndex = 0`、`Count = 0`
3. 设置 `Initialized = TRUE`
4. 输出初始化成功的 INFO 日志

**设计要点**：
- 必须在所有其他日志使用前调用
- 零初始化确保 `Ready[]` 数组均为 0（所有槽位为空）
- 此时刷出线程尚未启动，初始化日志通过 `LogWrite` 写入缓冲区（因为 `Initialized` 已为 TRUE）

### 3.2 `LogTerminate`

```c
VOID LogTerminate(VOID)
```

**功能**：终止日志系统。

**核心流程**：
1. 调用 `LogFlushThreadStop()` 确保刷出线程退出
2. 设置 `Initialized = FALSE`，阻止后续所有 LogWrite 操作

### 3.3 `LogWrite`

```c
VOID LogWrite(ULONG Level, ULONG Pid, const CHAR *Format, ...)
```

**功能**：统一的日志写入函数，在任意 IRQL（包括 VMX Root 模式）下安全。

**核心流程（锁无关发布协议的三步）**：

1. **快速检查**：`if (!g_LogBuffer.Initialized) return;`

2. **消息格式化**：
   - `va_start/va_end` 提取变参
   - `RtlStringCbVPrintfA` 格式化消息到栈局部缓冲区 `TempBuffer[256]`
   - 仅 `STATUS_BUFFER_OVERFLOW` 不被视为错误（截断可接受）

3. **锁无关写操作**：
   - **Step 1**（Claim Slot）：`InterlockedIncrement(&WriteIndex)` 原子递增写索引，返回新值
   - 计算有效索引：`(NewIndex - 1) % LOG_RING_BUFFER_ENTRIES`，处理负数取模
   - 获取对应 `Entry` 指针（此刻写入者独享此槽）
   - **Step 2**（Fill Data）：写入 `Level`、`Pid`、`Timestamp`（`KeQuerySystemTime`）、`Message`
   - **Step 3**（Publish）：`InterlockedExchange(&Ready[Idx], 1)` -- 全内存屏障（RELEASE 语义），确保 Step 2 的所有写操作在 Ready 标志置 1 前全局可见

4. **计数更新**：使用 `InterlockedCompareExchange` CAS 循环递增 `Count`，饱和至 `LOG_RING_BUFFER_ENTRIES`

**并发安全性**：
- 多写入者安全：每个写入者通过 `InterlockedIncrement` 获取唯一槽位，互不干扰
- 写入者与读取者同步：通过 `Ready[]` 标志的 ACQUIRE/RELEASE 语义保证
- 无锁、无 IRQL 操作、无死锁可能

### 3.4 `LogFlushThreadRoutine`

```c
static VOID LogFlushThreadRoutine(PVOID Context)
```

**功能**：系统线程主函数，定期将环形缓冲区中的日志输出到 WinDbg。

**核心流程**：

1. **初始化**：
   - 设置轮询间隔为 5ms（`-50000LL` 以 100ns 为单位）
   - 输出线程启动日志

2. **主循环**：
   - `KeWaitForSingleObject` 等待 `FlushStopEvent` 信号或 5ms 超时
   - **批量刷出**（最多 256 条/批）：
     - 读取 `WriteIndex` 和 `FlushIndex`，计算未刷出的条目数
     - 对每个待刷出条目：
       - **ACQUIRE 屏障**：`InterlockedCompareExchange(&Ready[Idx], 0, 0)` 检查就绪标志
         - 若为 0：写入者还在写入中，停止刷出（保证顺序）
         - 若为 1：所有条目字段可安全读取
       - **DbgPrintEx 输出**：仅 `Level <= VMX_LOG_INFO`（ERROR/WARN/INFO）输出到 WinDbg，DEBUG 级别仅在缓冲区保留
       - **RELEASE 屏障**：`InterlockedExchange(&Ready[Idx], 0)` 释放槽位
       - 递增 `FlushIdx` 和 `BatchCount`
   - 更新 `FlushIndex`（`InterlockedExchange`）

3. **退出处理**（收到 `FlushStopEvent` 信号时）：
   - 清空所有未刷出的条目（循环等待写入者完成）
   - 输出线程退出日志
   - `PsTerminateSystemThread(STATUS_SUCCESS)`

**设计要点**：
- 运行在 PASSIVE_LEVEL 的 System Thread 上，DbgPrintEx 安全
- 5ms 轮询间隔兼顾实时性和 CPU 开销
- DEBUG 级别日志不输出到 WinDbg 以减少噪音，但保留在缓冲区中供 IOCTL 读取

### 3.5 `LogFlushThreadStart`

```c
NTSTATUS LogFlushThreadStart(VOID)
```

**功能**：启动日志刷出系统线程。

**核心流程**：
1. 检查 `Initialized`（未初始化则返回 `STATUS_NOT_INITIALIZED`）
2. 检查 `FlushThreadRunning`（防重复启动）
3. `KeInitializeEvent(&FlushStopEvent, NotificationEvent, FALSE)` 初始化停止事件
4. `InterlockedExchange(&FlushIndex, WriteIndex)` 设置 FlushIndex 为当前 WriteIndex，避免刷出旧条目
5. `PsCreateSystemThread` 创建系统线程
6. `ObReferenceObjectByHandle` 获取线程对象引用（用于后续等待终止）
7. 保存线程句柄和引用，设置 `FlushThreadRunning = TRUE`

**返回值**：`STATUS_SUCCESS` 或 `PsCreateSystemThread` 的错误码。

### 3.6 `LogFlushThreadStop`

```c
VOID LogFlushThreadStop(VOID)
```

**功能**：停止日志刷出线程。

**核心流程**：
1. 检查 `FlushThreadRunning` 和 `FlushThreadObject`
2. `KeSetEvent(&FlushStopEvent, ...)` 发信号通知线程退出
3. `KeWaitForSingleObject(FlushThreadObject, ..., 5 秒超时)` 等待线程终止
4. `ObDereferenceObject(FlushThreadObject)` 释放引用
5. 相关字段置空/置零

**设计要点**：
- 5 秒超时防止死锁（线程可能卡在 `DbgPrintEx` 中）
- 刷出线程在退出前会清空所有残留条目

### 3.7 `LogRead`

```c
ULONG LogRead(PVMX_LOG_ENTRY OutputBuffer, ULONG MaxEntries)
```

**功能**：从环形缓冲区读取日志条目到用户态输出缓冲区（通过 IOCTL 调用）。

**核心流程**：

1. **快速检查**：`Initialized`、`OutputBuffer`、`MaxEntries` 任一无效返回 0

2. **循环读取**：
   - **CAS 计数递减**：`InterlockedCompareExchange(&Count, OldCount - 1, OldCount)` 尝试递减
     - 若 `Count <= 0`：无条目可读，跳转到 `done`
   - **获取槽位**：`InterlockedIncrement(&ReadIndex) - 1` 后取模
   - **就绪检查**：检查 `Ready[ReadIdx]` 标志
     - 若为 0：条目尚未完成写入，递增 Count 归还，跳转 `done`
   - **数据复制**：`RtlCopyMemory` 复制条目到输出缓冲区
   - 递增 `Copied` 计数

3. **返回实际复制的条目数**

**设计要点**：
- 仅在 PASSIVE_LEVEL 的 IOCTL 上下文中调用
- 读取者与写入者的竞争通过 `Ready[]` 标志协调
- Count 同时被写入者递增和 LogRead 递减，用 CAS 保证原子性
- 刷出线程和 LogRead 的读位置独立（FlushIndex vs ReadIndex），互不干扰

---

## 4. 控制流与逻辑流程

### 4.1 写入流程（LogWrite）

```
LogWrite(Level, Pid, Format, ...)
 |
 +-- !Initialized? -> return
 +-- va_start/va_end
 +-- RtlStringCbVPrintfA -> 格式化消息到 TempBuffer
 +-- Status 失败（非 BUFFER_OVERFLOW）? -> return
 |
 +-- [Step 1] InterlockedIncrement(&WriteIndex) -> NewIndex
 |    Index = (NewIndex - 1) % ENTRIES
 |    Entry = &Entries[Index]
 |
 +-- [Step 2] 填充 Entry:
 |    Entry->Level = Level
 |    Entry->Pid = Pid
 |    Entry->Timestamp = KeQuerySystemTime()
 |    RtlCopyMemory(Entry->Message, TempBuffer)
 |
 +-- [Step 3] InterlockedExchange(&Ready[Index], 1) // RELEASE 屏障
 |
 +-- [Count 更新] CAS 循环递增 Count，饱和至 ENTRIES
 +-- return
```

### 4.2 刷出线程流程

```
LogFlushThreadRoutine
 |
 +-- [初始化] PollInterval = 5ms
 |
 +-- while (TRUE):
 |    +-- KeWaitForSingleObject(FlushStopEvent, 5ms)
 |    |
 |    +-- [批量刷出] while (FlushIdx < WriteIdx && BatchCount < 256):
 |    |    +-- Idx = FlushIdx % ENTRIES
 |    |    +-- InterlockedCompareExchange(&Ready[Idx], 0, 0) == 0?
 |    |    |    +-- YES -> break（写入者未完成，保证顺序）
 |    |    |    +-- NO  -> 继续
 |    |    |
 |    |    +-- Entry->Level <= VMX_LOG_INFO?
 |    |    |    +-- YES -> DbgPrintEx 输出（带级别标识和 PID）
 |    |    |    +-- NO  -> 跳过（DEBUG 级别仅缓冲区保留）
 |    |    |
 |    |    +-- InterlockedExchange(&Ready[Idx], 0) // 释放槽位
 |    |    +-- FlushIdx++, BatchCount++
 |    |
 |    +-- InterlockedExchange(&FlushIndex, FlushIdx)
 |    |
 |    +-- WaitStatus == STATUS_SUCCESS?（收到停止信号）
 |         +-- [清空剩余] 循环等待所有条目刷出
 |         +-- PsTerminateSystemThread(STATUS_SUCCESS)
```

### 4.3 IOCTL 读取流程（LogRead）

```
LogRead(OutputBuffer, MaxEntries)
 |
 +-- !Initialized || !OutputBuffer || MaxEntries==0? -> return 0
 |
 +-- while Copied < MaxEntries:
 |    +-- CAS 递减 Count:
 |    |    OldCount = Count
 |    |    OldCount <= 0? -> goto done
 |    |    CAS(&Count, OldCount-1, OldCount) 失败? -> 重试
 |    |
 |    +-- ReadIdx = InterlockedIncrement(&ReadIndex) - 1
 |    +-- ReadIdx %= ENTRIES
 |    |
 |    +-- Ready[ReadIdx] == 0?（写入未完成）
 |    |    +-- YES -> InterlockedIncrement(&Count), goto done
 |    |
 |    +-- RtlCopyMemory(OutputBuffer[Copied], &Entries[ReadIdx])
 |    +-- Copied++
 |
 +-- done: return Copied
```

---

## 5. 与其他模块的交互

| 模块 | 交互方式 | 详细说明 |
|------|----------|----------|
| **所有模块** | 宏调用 | 通过 `LOG_ERROR/WARN/INFO/DEBUG` 和 `VMXROOT_LOG_*` 宏调用 `LogWrite` |
| `vmxdrv.c` | 生命周期管理 | `DriverEntry` 中 `LogInitialize()` + `LogFlushThreadStart()`；`DriverUnload` 中 `LogFlushThreadStop()` + `LogTerminate()` |
| `vmxdrv.c` | IOCTL 处理 | `HandleIoctlGetLog` 调用 `LogRead` 读取日志给用户态 |
| `common/shared.h` | 数据结构 | `VMX_LOG_ENTRY`、`VMX_LOG_BUFFER` 和日志级别常量 |

---

## 6. 关键设计要点

### 6.1 锁无关发布协议（Lock-Free Publish Protocol）

这是本文件最核心的设计模式，保证无锁环境下的数据一致性：

```
写入者（LogWrite）：
  1. InterlockedIncrement(&WriteIndex)    -- 申请槽位
  2. 填充 Entry 字段                      -- 写数据
  3. InterlockedExchange(&Ready[Idx], 1)  -- RELEASE 屏障，发布

读取者（刷出线程 / LogRead）：
  1. InterlockedCompareExchange(&Ready[Idx], 0, 0)  -- ACQUIRE 屏障，检查就绪
  2. 读取 Entry 字段                       -- 确保数据完整
  3. InterlockedExchange(&Ready[Idx], 0)  -- RELEASE 屏障，释放槽位
```

**关键保证**：
- 写入者必在 Ready 置 1 前完成所有数据写入（x86/x64 强序 + InterlockedExchange 的全屏障）
- 读取者必在确认 Ready == 1 后才读取数据
- 多个写入者不会冲突，因为每个槽位被 InterlockedIncrement 唯一分配
- 日志顺序在某些竞态下可能非严格有序，但写入者之间的相对顺序基本保持

### 6.2 VMX Root 模式安全性

- `LogWrite` **不做** `DbgPrintEx` -- 避免 INT 3 中断和内核调试器死锁
- `LogWrite` **不使用** 自旋锁 -- 避免 IRQL 提升和死锁
- `LogWrite` **仅使用** `Interlocked*` 操作 -- 在任何 IRQL 下均安全
- 所有 `VMXROOT_LOG_*` 宏均完全等价于 `LOG_*` 宏

### 6.3 双消费者模型

环形缓冲区有两个独立的读取者：
- **刷出线程**：使用 `FlushIndex` 跟踪已刷出位置，用于 DbgPrintEx 实时输出
- **LogRead（IOCTL）**：使用 `ReadIndex` 跟踪已读取位置，用于用户态工具查询

两者相互独立，不影响彼此的位置指针。`Count` 字段反映缓冲区中尚未被 LogRead 读取的条目数。

### 6.4 刷出线程的调试级别过滤

- `VMX_LOG_DEBUG`（Level 3）仅写入环形缓冲区
- 刷出线程只输出 `Level <= VMX_LOG_INFO`（即 ERROR=0, WARN=1, INFO=2）
- 减少 WinDbg 输出噪音，同时保留完整日志供 IOCTL 读取

### 6.5 频率有限的未知 MSR 日志保护

虽然此逻辑不在 log.c 中，但 msr.c 中的频率限制（最多 20 次）是对日志系统的重要补充，防止日志缓冲区被重复的未知 MSR 告警填满。

### 6.6 Windows 驱动框架注意点

- `DbgPrintEx` 只能在 `PASSIVE_LEVEL` 且在线程上下文中安全调用
- `PsCreateSystemThread` 创建的系统线程有正常的线程栈，可以安全使用 SEH 和 DbgPrintEx
- `KeWaitForSingleObject` 在系统线程中可使用 `KernelMode` 参数（不 APC）
- `KeSetEvent` 的第二参数 `IO_NO_INCREMENT` 表示不提升等待线程的优先级
- `InterlockedCompareExchange` 在 WDK 7600 中作为编译器内建函数可用
- 非分页池分配（`NonPagedPool`）确保日志缓冲区在任何 IRQL 下均可访问
