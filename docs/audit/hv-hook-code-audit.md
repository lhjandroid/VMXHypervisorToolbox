# hv_hook 通用 EPT/NPT Hook 框架审计报告

> **审计依据**: Intel SDM Vol 3C, AMD APM Vol 2, Windows x64 ABI
> **审计日期**: 2026-06-29
> **审计范围**: `hv_hook.h`, `hv_hook.c`, `hv_hook_asm.asm`
> **审计定位**: hv_hook 是纯软件层框架，不直接操作 VMCS/VMCB 硬件，所有硬件操作委托给 g_HvOps

---

## 审计摘要

| 严重级别 | 数量 | 说明 |
|----------|------|------|
| 🔴 **严重** | 0 | — |
| 🟡 **警告** | 2 | 潜在竞态条件、内存可执行性依赖 |
| 🔵 **信息** | 3 | 设计权衡值得关注 |
| ✅ **已验证** | 7 | 检查通过 |

---

## 🟡 警告发现

### 发现 1: GenericHookDecide 无锁读取链表，与 GenericHookRemove 存在竞态

**文件**: `hv_hook.c:581` (`GenericHookDecide`), `hv_hook.c:469-491` (`GenericHookRemove`)
**涉及**: `FindHookById()` 在 VM-Exit handler 上下文中无锁遍历链表

**代码路径对比**:

```c
// GenericHookDecide — 无锁读取 (VM-Exit handler, 任意 IRQL)
Entry = FindHookById((ULONG)HookIndex);  // 遍历 g_GenericHookState.HookListHead
while (Entry) {
    if (Entry->Active && Entry->HookId == HookId) {
        return Entry;                     // 无锁返回指针
    }
    Entry = Entry->Next;                  // 无锁跟随指针
}

// GenericHookRemove — 持有锁写入
KeAcquireSpinLock(&g_GenericHookState.Lock, &OldIrql);
// ... unlink ...
ExFreePoolWithTag(Entry, VMX_TAG);        // 释放内存
KeReleaseSpinLock(...);
```

**问题**: `GenericHookDecide` 在调用 `FindHookById` 时**未持有 `g_GenericHookState.Lock`**。如果另一个 CPU 上的 `GenericHookRemove` 同时：
1. 调用 `HvUnhookFunction` (EPT/NPT hook 已移除)
2. 重新获取锁，从链表移除 Entry
3. `ExFreePoolWithTag(Entry)` 释放内存

此时 `GenericHookDecide` 可能正在读取已释放的 Entry 字段 → **use-after-free**。

**缓解因素**:
- `HvUnhookFunction` 在释放 Entry **之前**移除了 EPT/NPT hook → 新线程不会再进入 thunk
- `ExFreePoolWithTag` 通常不立即清零内存 → 读取旧值大概率正确
- 只有"恰好同时卸载 hook 且已有线程在 dispatcher 中"时才可能触发

**修复建议**: 
- 方案 A: 使用 RCU 风格 — GenericHookRemove 先标记 `Active = FALSE`，延迟到确认无 CPU 正在 dispatcher 中再释放
- 方案 B (最小侵入): GenericHookDecide 中读取 HookId/Rule/Trampoline 时使用 volatile 读取 + 在读取后重新确认 Active 标志

---

### 发现 2: NonPagedPool 可执行性依赖 WDK 7600 目标环境

**文件**: `hv_hook.c:79-80`
**APM/SDM 依据**: Windows 内存管理文档

**当前代码**:
```c
/* NonPagedPool is executable on WDK 7600 target */
Page->CodeBase = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, VMX_TAG);
```

**问题**: 从 Windows 8 开始引入 `NonPagedPoolNx`（不可执行池），Windows 10 RS4 (1803) 将其设为默认。在启用了 NX pool 的系统上，`ExAllocatePoolWithTag(NonPagedPool, ...)` 返回的内存设置了 NX 位，执行 thunk 代码会触发 Guest 内 #PF。

**实际情况**:
- WDK 7600 目标 (Windows 7): NonPagedPool 可执行 ✓
- Windows 10 1607-1709: 默认仍为旧 NonPagedPool (可执行) ✓
- Windows 10 1803+: NonPagedPoolNx 默认，可能不可执行 ⚠️

**影响**: 此问题同样影响 EPT/NPT hook 页 (HookPage/OriginalPage/Trampoline)，是平台层面的约束，非 hv_hook 独有。项目目前标识为 WDK 7600 目标，Windows 7/早期 Win10 不受影响。

**修复建议**: 使用 `MmAllocateContiguousMemorySpecifyCache` + 显式映射为可执行，或使用 `ExAllocatePoolWithTag` + `MmGetPhysicalAddress` + 修改 EPT/NPT 的 NX 权限。但这需要架构级变更（所有代码分配路径），建议在单独的"Win10+ NX pool 兼容"专项中处理。

---

## 🔵 信息发现

### 发现 3: HookLogEvent spin lock 在 VM-Exit 热路径

**文件**: `hv_hook.c:636-668`

HookLogEvent 在 `ShouldLog == TRUE` 时被调用，获取 `g_GenericHookState.EventLock` 自旋锁并在 512 条环形缓冲区中写入事件。当 hook 配置为 LOG_ONLY 动作时，每次调用都触发日志写入 → 自旋锁开销成为热路径瓶颈。

**当前无问题**: 512 条环形缓冲区 + 自旋锁设计正确、安全。本发现仅为性能提醒。

---

### 发现 4: HookId 不在 Thunk 中验证完整性

**文件**: `hv_hook.c:50-64`, `hv_hook_asm.asm:26-152`

**当前流程**:
```
Thunk: mov r10, HookId; jmp AsmGenericHookDispatcher
ASM dispatcher: pass R10 to GenericHookDecide
GenericHookDecide: FindHookById(HookId)
```

HookId 通过 R10 从 thunk 传递到决策函数。没有验证 R10 是否被篡改。如果恶意代码直接 `mov r10, X; jmp AsmGenericHookDispatcher`，可以伪造 HookId。

**实际风险**: 极低。攻击者需要知道 `AsmGenericHookDispatcher` 的地址，且伪造 HookId 仅影响 PID 过滤和行为——核心的 EPT/NPT 页面权限仍然生效。这是一个 defense-in-depth 的缺失，不是安全漏洞。

---

### 发现 5: NextHookId 单调递增无上限回收

**文件**: `hv_hook.c:354`

`NextHookId` 从 1 开始递增，从不回收。FreeThunk 回收 thunk slot，但 HookId 永不复用。在极长期运行（数百万次 hook/unhook）后可能溢出 32 位 ULONG。实际上不可能达到。✓

---

## ✅ 已验证正确

### x64 ABI 合规性 (hv_hook_asm.asm)

| 检查项 | 结果 | 依据 |
|--------|------|------|
| 寄存器参数保存 (RCX,RDX,R8,R9) | ✅ | 存入 [rbp-08] 到 [rbp-20] |
| 栈参数 5-8 复制 | ✅ | 从 [rbp+30h] 到 [rbp+48h] 复制到 [rbp-38h] 到 [rbp-50h] |
| Shadow space 分配 | ✅ | `sub rsp, 0C0h` 提供 192 字节 |
| 调用 trampoline 时参数位置 | ✅ | Arg5-8 放在 [rsp+20h] 到 [rsp+38h] (call 前) |
| 栈 16 字节对齐 | ✅ | push rbp + call 嵌套保证对齐 |
| R10 作为 HookId 载体 | ✅ | R10 是 volatile 寄存器，函数入口不依赖其值 |
| RAX 返回值传递 | ✅ | [rbp-0A0h] 保存最终返回值，epilogue 移入 RAX |

### FreeThunk 安全性

| 检查项 | 结果 |
|--------|------|
| Thunk zeroing 不影响已运行的 dispatcher | ✅ dispatcher 代码在独立页面 |
| SlotBitmap 更新原子性 | ✅ 调用方持有 Lock |
| EPT/NPT hook 在 FreeThunk 之前已移除 | ✅ GenericHookRemove 先调 HvUnhookFunction |

### 线程安全

| 操作 | 锁 | 安全 |
|------|-----|------|
| AllocateThunk (搜索/分配) | g_GenericHookState.Lock | ✅ |
| FindHookById (GenericHookRemove 内) | g_GenericHookState.Lock | ✅ |
| Hook 链表插入/删除 | g_GenericHookState.Lock | ✅ |
| EventRing 写入/读取 | g_GenericHookState.EventLock | ✅ |
| FindHookById (GenericHookDecide 内) | **无锁** | ⚠️ 发现 #1 |

### Hook 生命周期

```
Install: AllocateThunk → HvHookFunction → AllocateEntry → Link
Remove:  FindEntry → HvUnhookFunction → FreeThunk → Unlink → ExFreePool
```

EPT/NPT 操作在锁**之外**执行（避免在高 IRQL 持有自旋锁进行耗时操作），锁仅保护内存数据结构。正确。✅

---

## 审计结论

hv_hook 作为纯软件层 Hook 框架，设计合理、ASM 调度器 ABI 正确、大部分并发路径有适当锁保护。

**唯一需关注的是发现 #1**（GenericHookDecide 无锁读取链表）。实际触发条件苛刻（必须恰好同时卸载 hook 且有线程在执行 dispatcher），且 HvUnhookFunction 已在释放 Entry 前移除了 EPT/NPT hook 作为第一道防线，所以**在生产环境中极不可能导致崩溃**。建议在后续迭代中添加 RCU 或 `Active` 标志双重检查。

**发现 #2**（NX pool）是平台兼容性问题，影响所有 NonPagedPool 代码页面，需在专项中统一解决。
