# npt.c / npt.h — 逻辑分析

## 1. 文件概述

- **角色与职责**: `npt.c` 是 AMD Nested Page Tables（NPT）的实现，是 Intel EPT 在 AMD 平台的等价物。它负责 NPT 身份映射页表的建立、2MB 大页分裂、NPT 钩子引擎以及 NPT 缺页（#NPF）的处理。
- **npt.h 的角色**: 定义 NPT 状态结构体、钩子状态结构体，以及所有 NPT 相关函数的声明。与 `ept.h` 共享 `EPT_PML4E`/`EPT_PDPTE`/`EPT_PDE`/`EPT_PTE`/`EPT_HOOK_ENTRY` 等结构体定义。
- **依赖的其他模块**:
  - `ept.h` — 复用了 EPT 页表条目结构体和 `EPT_HOOK_ENTRY` 钩子条目结构体
  - `svm.h` / `svm.c` — 访问 `g_SvmState`（用于 VMCB TLB 控制）
  - `log.h` / `log.c` — 日志系统
  - EPT 模块的指令解码函数：`EptGetInstructionLength()`、`EptIsRipRelativeInstruction()`、`EptRelocateRipRelativeInstruction()`

---

## 2. 数据结构

### NPT 页表结构（已复用的 EPT 类型）

NPT 使用与 x86-64 相同的 4 级页表结构（PML4 → PDPT → PD → PT），与 Intel EPT 的页表条目格式**兼容**，因此 `npt.c` 直接复用 `ept.h` 中的联合体定义：

| 类型 | 层级 | 页面粒度 | 关键位 |
|------|------|----------|--------|
| `EPT_PML4E` | PML4 | — | Read/Write/Execute, PhysAddr[51:12] |
| `EPT_PDPTE` | PDPT | 1GB 大页 (LargePage=1) | Read/Write/Execute, LargePage, PhysAddr |
| `EPT_PDE` | PD | 2MB 大页 (LargePage=1) | Read/Write/Execute, LargePage, PhysAddr |
| `EPT_PTE` | PT | 4KB | Read/Write/Execute, MemoryType, Accessed, Dirty, PhysAddr, SuppressVe |

### EPT→NPT 位语义映射表（npt.h）

虽然 NPT 复用了 EPT 的位域结构体定义，但同一位的硬件语义在 EPT 和 NPT 之间**不同**。`npt.h` 中的注释块和 `C_ASSERT` 编译期断言明确了以下映射：

| 位 | EPT_PTE 字段 | EPT 含义 | NPT 含义 | 关键说明 |
|---|-------------|----------|----------|---------|
| 0 | Read | 读访问权限 | Present（P） | 巧合一致：Read=1 在 NPT 中标志页有效 |
| 1 | Write | 写访问权限 | Read/Write（R/W） | 巧合一致：Write=0 在 NPT 中为只读 |
| 2 | Execute | 执行访问权限 | User/Supervisor（U/S） | **关键差异**: EPT 用此位控制执行，NPT 用此位控制权限级别 |
| 5:3 | MemoryType | EPT 内存类型 | PWT, PCD, PAT | NPT 中的缓存控制位 |
| 63 | SuppressVe | 抑制虚拟化异常 (#VE) | No Execute（NX） | **关键差异**: NPT 的执行控制位是 bit 63，而非 bit 2 |

**NPT 钩子引擎能正确工作的原因**:
1. NX 位（bit 63）默认为 0 → 执行**始终允许**（不是因为 `Execute=1`，因为那在 NPT 中设置的是 U/S=1，是一个不同的位！）
2. `Read=1` 巧合地映射到 NPT `Present=1` → 页表项有效
3. `Write=0` 巧合地映射到 NPT `R/W=0` → 写触发 NPF（正确的权限控制）

**编译期验证（npt.h 中的 C_ASSERT）**:
```c
C_ASSERT((1ULL << 0)  == 1);   /* Bit 0: EPT Read = NPT Present (始终位 0) */
C_ASSERT((1ULL << 1)  == 2);   /* Bit 1: EPT Write = NPT R/W (始终位 1) */
C_ASSERT((1ULL << 51) != 0);   /* PhysAddr 高位在 ULONG64 内 */
```

### `EPT_HOOK_ENTRY` — 钩子条目（来自 ept.h）

```c
typedef struct _EPT_HOOK_ENTRY {
    BOOLEAN     Active;                  // 是否激活
    ULONG64     TargetVirtualAddr;       // 目标函数 VA
    ULONG64     TargetPhysicalAddr;      // 目标页的物理地址（4KB 对齐）
    ULONG64     TargetPageOffset;        // 在页内的偏移
    PVOID       OriginalPageVa;          // 原始页面内容副本
    PVOID       HookPageVa;              // 修改后的页面（含 JMP 指令）
    ULONG64     HookPagePa;              // 钩子页面物理地址
    BOOLEAN     OwnsPages;              // 是否拥有页面资源
    PVOID       TrampolineVa;            // 跳板（原始字节 + JMP 返回）
    ULONG       OriginalBytesSize;       // 保存的原始字节大小
    UCHAR       OriginalBytes[32];       // 原始指令字节备份
    PVOID       HookFunction;            // 替换函数指针
    PEPT_PTE    TargetPte;               // 指向此页的 NPT PTE
} EPT_HOOK_ENTRY;
```

### `NPT_STATE` — NPT 全局状态

```c
typedef struct _NPT_STATE {
    DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML4E Pml4[EPT_PML4E_COUNT];   // PML4 表
    DECLSPEC_ALIGN(PAGE_SIZE) EPT_PDPTE Pdpt[EPT_PDPTE_COUNT];   // PDPT（覆盖首 512GB）
    ULONG64 Pml4Pa;                          // PML4 物理地址
    BOOLEAN Initialized;
} NPT_STATE;
```

### `NPT_CPU_STATE` — 每 CPU NPT 根（钩子页面隔离）

```c
typedef struct _NPT_CPU_STATE {
    DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML4E Pml4[EPT_PML4E_COUNT];
    DECLSPEC_ALIGN(PAGE_SIZE) EPT_PDPTE Pdpt[EPT_PDPTE_COUNT];
    ULONG64     Pml4Pa;
} NPT_CPU_STATE;
```

每个 CPU 拥有自己完整的 PML4→PDPT 链，使得每 CPU 可以独立切换 NPT PTE 权限（解决多核竞争）。

### `NPT_HOOK_STATE` — 钩子管理器状态

```c
typedef struct _NPT_HOOK_STATE {
    EPT_HOOK_ENTRY  Hooks[NPT_MAX_HOOKS];   // 钩子数组（1024 条）
    ULONG           HookCount;               // 当前钩子数量
    KSPIN_LOCK      Lock;                    // 保护锁
    BOOLEAN         Initialized;
    ULONG           HookHashTable[EPT_HOOK_HASH_SIZE];  // O(1) 哈希表
} NPT_HOOK_STATE;
```

### 内部辅助结构体

#### `NPT_SPLIT_PAGE` — 大页分裂后的 4KB 页表
```c
typedef struct _NPT_SPLIT_PAGE {
    EPT_PTE   Pte[EPT_PTE_COUNT];   // 512 个 PTE（覆盖 2MB 区域）
    ULONG64   PhysicalAddress;       // PT 页表的物理地址
    ULONG64   BasePhysAddr2MB;       // 对应的 2MB 基地址
    BOOLEAN   InUse;
} NPT_SPLIT_PAGE;
```

#### `NPT_PD_PAGE` — 页目录页
```c
typedef struct _NPT_PD_PAGE {
    EPT_PDE   Entries[EPT_PDE_COUNT];  // 512 个 PDE
} NPT_PD_PAGE;
```

#### `NPT_SPLIT_HASH_ENTRY` — 分裂页哈希表条目
```c
typedef struct _NPT_SPLIT_HASH_ENTRY {
    ULONG64 Base2MB;     // 2MB 基地址
    ULONG   SplitIdx;    // 在 g_NptSplitPages 中的索引
} NPT_SPLIT_HASH_ENTRY;
```

### 每 CPU #DB 跟踪器

三个每 CPU 数组（动态分配，大小为 `g_MaxProcessors`）：
- `g_NptDbRelaxedPagePa[CpuIndex]` — 松弛页面的物理地址（非零 = 跟踪器启用）
- `g_NptDbRelaxedRip[CpuIndex]` — 松弛时的 RIP
- `g_NptDbRelaxedCr3[CpuIndex]` — 松弛时的 CR3（M-4 修订，用于跨进程鉴别）

### 每 CPU NPT 分裂页面管理

- `g_NptPerCpuSplitPages[cpu][splitIdx].Pte[]` — 每 CPU 独立的 512 个 PTE
- `g_NptPerCpuPdPages[cpu][FlatPdptIdx].Entries[]` — 每 CPU 独立的 PD 页面
- `g_NptPerCpuPdAllocated[FlatPdptIdx]` — PD 区域是否已克隆到所有 CPU

---

## 3. 核心函数详解

### NPT 标识映射设置

#### `NptComputeRequiredPdPages()` — 计算需要的 PDPT 条目数
- **签名**: `static ULONG NptComputeRequiredPdPages(VOID)`
- **功能**: 通过 `MmGetPhysicalMemoryRanges()` 获取物理内存范围，计算需要的 1GB PDPT 条目数
- **核心逻辑**:
  1. 遍历物理内存范围，找到最大物理地址（`MaxPa`）
  2. 加上 2GB MMIO 余量
  3. 向上取整到 1GB 对齐
  4. 最小值为 `NPT_DEFAULT_PD_PAGES`（512 = 512GB）
  5. 最大值为 `NPT_MAX_PD_PAGES_CAP`（512 * 512 = 256TB）
  6. 对齐到 512 的倍数（整个 PDPT 页）

#### `NptInitialize()` — NPT 初始化
- **签名**: `NTSTATUS NptInitialize(VOID)`
- **功能**: 分配并建立完整的 NPT 身份映射页表
- **核心流程**:
  1. 检查重复初始化
  2. 计算需要的 PDPT 数量（`g_NptPdptTotal`）和 PML4 数量（`g_NptPml4Count`）
  3. 初始化哈希表（钩子和分裂页）
  4. 分配每 CPU 跟踪数组（`g_NptDbRelaxedPagePa/Rip/Cr3`）
  5. 分配 PD 页面数组（`g_NptPdPages`，大小为 `g_NptPdptTotal`）
  6. 分配每 CPU PD 分配位图（`g_NptPerCpuPdAllocated`）
  7. 扩展 PDPT 页面（当需要 > 512GB 时）
  8. 分配分裂页面池（128 个）
  9. **建立身份映射**:
     - 遍历所有 PDPT 条目
     - 每个 PDPT → 对应的 PD 页面（读写执行全开）
     - 每个 PD 条目 → 2MB 大页（读写执行全开）
     - PML4[0] → 嵌入的 Pdpt[]
     - PML4[1..N-1] → 扩展的 PDPT 页面
  10. 记录 PML4 物理地址

- **返回值**: `STATUS_SUCCESS` 或内存分配错误

#### `NptCleanup()` — NPT 清理
- **签名**: `VOID NptCleanup(VOID)`
- **功能**: 释放所有 NPT 资源
- **核心流程**:
  1. 先解除所有钩子（`NptUnhookAll()`）
  2. 释放分裂页面池，并重置哈希表
  3. 释放 PD 页面、扩展 PDPT、每 CPU 分配位图
  4. 释放每 CPU 跟踪数组
  5. 标志位清零

### 2MB 大页分裂

#### `NptSplitLargePage()` — 分裂 2MB 大页为 4KB 页面
- **签名**: `VOID NptSplitLargePage(ULONG64 PhysicalAddress)`
- **功能**: 将指定物理地址所在的 2MB NPT 大页分裂为 512 个 4KB 页面
- **核心流程**:
  1. 计算 2MB 基地址
  2. 检查 PDE 是否已经是分裂状态（`LargePage == 0` → 已分裂）
  3. 在分裂页池中找空闲条目
  4. 初始化 512 个 PTE：每个映射 4KB 页面，读写执行全开，WB 缓存类型
  5. 记录分裂页的物理地址和基地址
  6. 插入分裂页哈希表（O(1) 查询）
  7. 修改 PDE：清除 LargePage 标志，设置 PhysAddr 指向 PTE 页表
  8. 此后，此 2MB 区域通过 4KB 粒度管理

### NPT PTE 查找

#### `NptGetPteForPhysicalAddress()` — 获取指定物理地址的 PTE
- **签名**: `PEPT_PTE NptGetPteForPhysicalAddress(ULONG64 PhysicalAddress)`
- **功能**: 查找管理指定物理地址的 NPT PTE
- **核心流程**:
  1. 计算 2MB 基地址、PDPT 索引、PD 索引、PT 索引
  2. 检查 PDE 是否是大页（`LargePage == 1` → 需要先分裂，返回 NULL）
  3. 使用哈希表 O(1) 查询分裂页表（取代原来的 O(n) 线性扫描）
  4. 返回对应 PTE 指针

### NPT 钩子引擎

#### `NptHookFunction()` — 安装 NPT 钩子
- **签名**: `NTSTATUS NptHookFunction(ULONG64 TargetVa, PVOID HookFunction, PVOID *OriginalFunction)`
- **功能**: 通过 NPT 页表分裂实现透明的函数钩子
- **核心流程**:

  1. **地址解析**: 将目标 VA 转换为 PA，获取页基址和页内偏移
  2. **边界检查**: 验证目标点在页面末尾前至少 12 字节（JMP 指令长度）
  3. **重复检查**: 确保同一 VA 未被重复钩住
  4. **页面所有者检查**: 查找同物理页上是否已有其他钩子（页面共享）
  5. **分配钩子槽位**: 在 `g_NptHookState.Hooks[]` 中找空闲条目
  6. **分裂大页**: 确保目标物理地址所在的 2MB 大页已分裂为 4KB 页面
  7. **冲刷 TLB**: 分裂后立即冲刷，防止其他 CPU 的陈旧 TLB 指向旧结构
  8. **获取 PTE**: 获取指向目标物理地址的 NPT PTE
  9. **页面分配**:
     - 首次钩住此物理页：分配 OriginalPage + HookPage，复制原始页面内容，建立 HookPagePa
     - 同页已有钩子：共享页面（不重复分配）
  10. **分配跳板**: 为目标函数分配跳板内存
  11. **指令解码与保存**:
      - 使用 `EptGetInstructionLength()` 解码至少 12 字节的完整指令
      - 保存到 `Hook->OriginalBytes`
  12. **构建钩子页面**:
      - 在 HookPage 的目标偏移处写入 `MOV RAX, imm64; JMP RAX`（12 字节）
  13. **构建跳板**:
      - 复制原始指令字节
      - **RIP-相对地址重定位**: 扫描每条指令，如果存在 RIP-相对寻址（ModRM Mod=00 RM=101），修正 disp32 使其在跳板的 VA 上仍指向原始目标
      - 追加 `JMP [RIP+0]` 跳回 `TargetVa + OriginalBytesSize`
  14. **配置 PTE（NPT 语义详解）**:
      - NPT 钩子策略（AMD 不支持 Execute-Only）:
        - **`Pte->Read = 1`** → NPT Present=1（页有效）
        - **`Pte->Write = 0`** → NPT R/W=0（只读，写触发 NPF）
        - **`Pte->Execute = 1`** → NPT U/S=1（用户+管理员权限，**并非执行控制**）
        - **默认 NX=0**（bit 63 未设置）→ 执行始终允许
        - PhysAddr → HookPage（指向含 JMP 的钩子页）
      - **关键洞察**: 尽管 PTE 视觉上有 `Execute=1`，但 NPT 的执行控制位是 bit 63（NX）而非 bit 2。`Execute=1` 在 NPT 中设置的是 U/S=1（允许用户模式访问），执行权限由 NX 默认 0 保证。详情见 npt.h 的完整映射表。
  15. **每 CPU 钩子隔离**:
      - 克隆 PD 到所有 CPU
      - 克隆分裂页表到所有 CPU
      - 在每 CPU PTE 上应用相同权限
  16. **启用 #DB 拦截**: 安装钩子后启用 #DB 中断（C-3），用于单步恢复机制
  17. 返回跳板地址给调用者

#### `NptUnhookFunction()` — 卸载钩子
- **签名**: `NTSTATUS NptUnhookFunction(ULONG64 TargetVa)`
- **核心流程**:
  1. 定位钩子条目
  2. 在共享 HookPage 上恢复原始字节
  3. 检查是否还有其他钩子在同一物理页上
  4. **两遍卸载**（Review Issue #2 修复）:
     - 第一遍：恢复 NPT PTE 到原始物理地址（RWX）
     - 冲刷 TLB（包括每 CPU）
     - **然后** 释放页面（防止 UAF）
  5. 更新哈希表（`NptHookHashRebuild()`）
  6. 如果钩子计数归零，禁用 #DB 拦截

#### `NptUnhookAll()` — 卸载全部钩子
- **签名**: `VOID NptUnhookAll(VOID)`
- **核心流程**:
  1. **第一遍**: 恢复所有 NPT PTE 到原始物理地址（RWX）
  2. 冲刷 TLB（同步 IPI，`NptInvalidateAllCpusSync()`）
  3. **第二遍**: 释放所有钩子页面和跳板内存
  4. 清空哈希表
  5. 禁用 #DB 拦截

#### `NptFindHookByPhysicalAddress()` — 按物理地址查找钩子
- **签名**: `PEPT_HOOK_ENTRY NptFindHookByPhysicalAddress(ULONG64 PhysicalAddress)`
- **功能**: 使用哈希表 O(1) 查找指定物理地址页上的钩子
- **核心逻辑**: 哈希函数 `NptHookHashFn(PagePa >> 12)`，开放寻址+线性探测

### NPT 缺页处理

#### `NptHandlePageFault()` — #NPF 处理
- **签名**: `BOOLEAN NptHandlePageFault(PVOID GuestContext)`
- **功能**: 处理 NPT 缺页异常（SVM_EXIT_NPF）
- **核心流程**:
  1. 获取当前 CPU 的 VMCB
  2. 读取 ExitInfo2（缺页 GPA）和 ExitInfo1（错误码）
  3. **查找钩子**: 检查是否在钩子页面上发生 NPF
  4. **非钩子页面 NPF**:
     - 如果 GPA 超出身份映射范围 → 返回 FALSE（关机）
     - 否则，设置 RWX 权限（修复未知 NPF）
  5. **钩子页面 NPF**:
     - 使用每 CPU PTE（如果可用）
     - 统一设置 `R=1, W=1, X=1`，物理地址改为**原始页面**（写穿透）
     - 调用 `NptDbTrackRelaxedPage()` 记录松弛的页面
     - 设置 RFLAGS.TF（位8）启用单步
     - **不推进 RIP**（重新执行触发 NPF 的指令）
  6. 返回 TRUE（继续客户机）

### 每 CPU NPT 管理

#### `NptInitPerCpu()` — 每 CPU NPT 初始化
- **签名**: `NTSTATUS NptInitPerCpu(VOID)`
- **功能**: 为每个 CPU 创建独立的 NPT 根页表
- **核心流程**:
  1. 分配 `g_NptCpuStates[]`（每 CPU PML4+PDPT）
  2. 分配 `g_NptPerCpuSplitPages[]`（每 CPU 分裂页表）
  3. 分配 `g_NptPerCpuPdPages[]`（每 CPU PD 页面）
  4. 扩展 PDPT 页面（>512GB 时）
  5. 从共享模板复制 PML4 和 PDPT
  6. 更新 PML4[0] 的 PhysAddr 指向本 CPU 的 PDPT
  7. 计算和记录每 CPU Pml4Pa

#### `NptGetPerCpuPte()` — 获取每 CPU PTE
- **签名**: `PEPT_PTE NptGetPerCpuPte(ULONG CpuIndex, ULONG64 PhysicalAddress)`
- **功能**: 获取指定 CPU 私有的 PTE（用于钩子隔离）
- **核心逻辑**: 通过哈希表查找分裂页，然后访问 `g_NptPerCpuSplitPages[CpuIndex][splitIdx].Pte[PtIndex]`

#### `NptGetPerCpuRootPa()` — 获取每 CPU 根页表物理地址
- **签名**: `ULONG64 NptGetPerCpuRootPa(ULONG CpuIndex)`
- **功能**: 返回指定 CPU 的 PML4 物理地址（用于 VMCB.NestedCr3）

### TLB 无效化

#### `NptInvalidateAll()` — 设置 TLB 冲刷
- **签名**: `VOID NptInvalidateAll(VOID)`
- **功能**: 在所有 CPU 的 VMCB 中设置 `TlbCtl = FLUSH_ALL_ASID`，同时清除 Clean Bits 位 0，确保 TLB 冲刷在下一次 VMRUN 时能生效

**Audit #2 Clean Bits 修复**: 新增清除 `Vmcb->Control.CleanBits &= ~(1UL << 0)` 的逻辑。
- **问题**: AMD APM Vol.2 §15.5.1 规定 Clean Bits 位 0 控制 Intercept vectors、TSC Offset、Pause Filter **以及 TlbCtl** 的重新读取。当位 0 为 1 时，CPU 在下次 VMRUN 时会跳过重新读取 TlbCtl。
- **场景**: `NptInvalidateAll()` 经常从 IOCTL 上下文（PASSIVE_LEVEL）调用。此时 VMCB 的 Clean Bits 可能已被之前的 VMRUN 设置为 1。如果不清除位 0，TlbCtl 的写入会被忽略，导致 TLB 冲刷不生效。
- **影响**: 没有此修复，在钩子安装/卸载后可能出现陈旧的 NPT 地址转换残留。

#### `NptInvalidateAllCpusSync()` — 同步 TLB 冲刷（H-5 修复）
- **签名**: `VOID NptInvalidateAllCpusSync(VOID)`
- **功能**: 通过 IPI 在所有 CPU 上强制执行一次 VMEXIT→VMRUN 循环，使待处理的 TlbCtl 冲刷生效
- **核心流程**:
  1. 调用 `NptInvalidateAll()` 设置 TlbCtl
  2. 通过 `KeIpiGenericCall()` 在所有 CPU 上执行 `NptInveptIpiCallback()`：
     - 执行 CPUID（总是被拦截）
     - 导致 VMEXIT → VMRUN 循环，应用 TlbCtl 冲刷
  3. `KeIpiGenericCall()` 同步返回，确保所有 CPU 已完成

### 每 CPU #DB 跟踪器（M-4 修复）

#### `NptDbTrackRelaxedPage()` — 记录松弛页面
- **签名**: `VOID NptDbTrackRelaxedPage(ULONG64 PagePhysicalAddr)`
- **功能**: 在 NPF 处理中记录被松弛的页面地址和当时的 RIP/CR3
- **内存顺序**: 先写 Rip/Cr3，`_WriteBarrier()`，再写 PagePa（启用跟踪）

#### `NptDbMatchesRelaxedRip()` — 判断 #DB 归属
- **签名**: `BOOLEAN NptDbMatchesRelaxedRip(ULONG64 CurrentRip)`
- **功能**: 判断当前 #DB 是否由本系统 NPT 单步引起
- **判定条件**:
  1. `PagePa != 0`（跟踪器已启用）
  2. `CurrentCr3 == Snap.Cr3`（相同进程，掩去 PCID）
  3. `CurrentRip - Snap.Rip` 在 0-15 字节范围内（RIP 未远距离偏离）

#### `NptDbGetAndClearRelaxedPage()` — 获取并清除松弛页面
- **签名**: `ULONG64 NptDbGetAndClearRelaxedPage(VOID)`
- **功能**: 获取曾被松弛的页面物理地址并清除跟踪器

---

## 4. 控制流与逻辑流程

### 钩子安装流程
```
NptHookFunction()
  ├── 地址转换和验证（VA→PA, 页面边界检查）
  ├── 加锁
  ├── 检查重复和页面所有者
  ├── NptSplitLargePage()    ← 分裂 2MB 大页
  ├── NptInvalidateAll()     ← 立即刷新 TLB
  ├── NptGetPteForPhysicalAddress()  ← 获取目标 PTE
  ├── 分配 OriginalPage + HookPage
  ├── 指令解码（EptGetInstructionLength）→ 保存 ≥12 字节
  ├── 构建 HookPage: MOV RAX, hook_fn; JMP RAX
  ├── 构建 Trampoline: 原始指令 + RIP-相对重定位 + JMP 返回
  ├── 配置 PTE:
  │   ├── Pte->Read = 1    → NPT Present=1  (页有效)
  │   ├── Pte->Write = 0   → NPT R/W=0      (只读, 写触发NPF)
  │   ├── Pte->Execute = 1 → NPT U/S=1      (用户+管理员权限)
  │   ├── NX=0 (默认)      → 执行始终允许   (非Execute位控制!)
  │   └── PhysAddr = HookPagePa
  ├── 每 CPU 隔离设置（克隆 PD/PT 到所有 CPU）
  ├── 哈希表插入
  ├── 解锁
  ├── NptInvalidateAll()
  ├── SvmSetExceptionInterceptDb(TRUE)   ← 启用 #DB
  └── 返回跳板地址
```

### NPF → 单步 → #DB 恢复流程（核心陷阱机制）
```
客户机执行写入被钩函数
  → NPF (NPT 缺页)，因为 PTE 是 R+X (W=0)
  → NptHandlePageFault()
       ├── 设置 PTE: R=1, W=1, X=1, PhysAddr=原始页面
       ├── NptDbTrackRelaxedPage(PagePa)  ← 记录松弛信息
       ├── 设置 RFLAGS.TF = 1
       └── 不推进 RIP → 重新执行指令
  → VMRUN
  → 同一指令重执行（这次成功写入原始页面）
  → 指令执行完毕，TF 触发 #DB
  → SvmHandleDbException()
       ├── NptDbMatchesRelaxedRip() = TRUE  ← 确认是本系统事件
       ├── NptDbGetAndClearRelaxedPage()    ← 获取松弛页面
       ├── NptFindHookByPhysicalAddress()   ← 查找钩子
       ├── 恢复 PTE: R=1, W=0, X=1, PhysAddr=HookPagePa
       ├── 清除 RFLAGS.TF
       └── NptInvalidateAll()  ← 冲刷 TLB
  → VMRUN，客户机从下一条指令继续执行
```

### 非钩子 NPF 处理
```
NPF 在非钩子页面
  → 检查 GPA 是否超出身份映射范围
     ├── 超出 → 返回 FALSE（关机）
     └── 范围内 → 设置 RWX，冲刷 TLB，继续
```

### 每 CPU 钩子隔离（多核竞争修复）

问题：两个 CPU 同时处理不同页面的 NPF，但共享同一个 PTE 数组。

修复：每 CPU 拥有独立的 PD 和 PT 页面副本。
```
NptHookFunction() 安装钩子时:
  → NptEnsurePerCpuPdForRegion(PdptIndex)
       ├── 克隆共享 PD 到所有 CPU 的 g_NptPerCpuPdPages
       └── 更新每 CPU PDPTE 指向各自的 PD
  → NptEnsurePerCpuSplitPage(splitIdx, ...)
       ├── 克隆分裂页表到所有 CPU 的 g_NptPerCpuSplitPages
       └── 更新每 CPU PDE 指向各自的 PT
  → 对每个 CPU: 设置 PTE 权限

NPF 处理时:
  → Pte = NptGetPerCpuPte(CpuIdx, TargetPa)
  → 如果没有每 CPU PTE，回退到 Hook->TargetPte（共享）

#DB 恢复时:
  → Pte = NptGetPerCpuPte(CpuNum, RelaxedPa)
  → 独立修改本 CPU 的 PTE，不影响其他 CPU
```

### 同步 TLB 冲刷（H-5 修复）

```
NptUnhookAll()
  → Pass 1: 恢复所有 PTE 到 RWX + 原始物理地址
  → NptInvalidateAllCpusSync()
       ├── NptInvalidateAll()   设置所有 VMCB.TlbCtl + 清除 Clean Bits 位0
       └── KeIpiGenericCall()
            └── 每 CPU 执行 CPUID → VMEXIT → VMRUN → TlbCtl 生效
  → Pass 2: 安全释放页面内存
```

---

## 5. 与其他模块的交互

### SVM 模块（svm.c / svm.h）

- `NptInitialize()` 在 `SvmInitialize()` 中被调用
- `NptInitPerCpu()` 同样在 `SvmInitialize()` 中被调用（失败时回退到共享 NPT）
- `SvmInitVmcb()` 调用 `NptGetPerCpuRootPa()` / `NptGetRootPageTablePa()` 获取 NPT 根，填入 `VMCB.Control.NestedCr3`
- NPF 处理后，`SvmInitDbException()` 中的 `#DB` 恢复使用 `NptDbMatchesRelaxedRip()` 判断归属
- TLB 冲刷通过写入 `g_SvmState.CpuContexts[i].VmcbVa->Control.TlbCtl` 实现

### SVM 退出处理（svm_exit.c）

- `SVM_EXIT_NPF` → `NptHandlePageFault()`
- `SVM_EXIT_EXCP_DB` 的 #DB 处理中广泛调用 NPT 函数：
  - `NptDbMatchesRelaxedRip()` — 判断 #DB 归属
  - `NptDbGetAndClearRelaxedPage()` — 获取松弛页面
  - `NptFindHookByPhysicalAddress()` — 查找钩子
  - `NptGetPerCpuPte()` — 获取每 CPU PTE
  - `NptInvalidateAll()` — 冲刷 TLB

### HV_OPS vtable

NPT 函数包装为 `HV_OPS` 接口：
- `SetupPageTables` → `NptInitialize()`
- `CleanupPageTables` → `NptCleanup()`
- `InvalidatePageTables` → `NptInvalidateAll()`
- `HookFunction` → `NptHookFunction()`
- `UnhookFunction` → `NptUnhookFunction()`
- `UnhookAll` → `NptUnhookAll()`

### EPT 模块（ept.h / ept.c）

- 复用了 EPT 的全部页表条目结构体定义（`EPT_PML4E`, `EPT_PDPTE`, `EPT_PDE`, `EPT_PTE`）
- 复用了 `EPT_HOOK_ENTRY` 结构体
- 复用了哈希表常量（`EPT_HOOK_HASH_SIZE`, `EPT_SPLIT_HASH_SIZE`）
- **共享指令解码器**：`EptGetInstructionLength()`, `EptIsRipRelativeInstruction()`, `EptRelocateRipRelativeInstruction()`
  - 这些是纯算法函数（不依赖硬件），NPT 可以安全复用

### 反反调试模块（anti_anti_debug.c）

- 通过 `SvmSetExceptionInterceptDb(TRUE/FALSE)` 控制 #DB 拦截的启用/禁用
- 钩子安装/卸载通过 `g_HvOps->HookFunction/UnhookFunction/UnhookAll` 间接调用 NPT

---

## 6. 关键设计要点

### NPT 钩子策略（与 EPT 的核心差异）

由于 AMD NPT **不支持 Execute-Only 页面**，采用了不同的钩子映射策略：

| 特性 | Intel EPT | AMD NPT |
|------|-----------|---------|
| 钩子页权限 | Execute-Only (R=0,W=0,X=1) | Read+Execute (R=1,W=0,X=1) |
| 执行行为 | 通过（指令页映射在钩子页） | 通过（同 EPT） |
| 读行为 | 缺页 → 从原始页读取 → 恢复 | 直接看到钩子页（含 JMP） |
| 写行为 | 缺页 → 临时写权限 → 恢复 | 缺页 → 临时写权限 → 恢复 |
| 隐秘性 | 高（读不出 JMP） | 中（读取代码区域可发现 JMP） |

读行为差异意味着在 AMD 上，如果目标程序对自己的代码区域进行完整性检查（读取并校验），可能发现 NPT 钩子的存在。但对反反调试场景，目标程序主要执行代码而非读取，因此影响有限。

### EPT→NPT 位语义映射（npt.h 关键文档）

NPT 直接复用 EPT 的 `EPT_PTE` 位域结构体（`Read`, `Write`, `Execute`, `SuppressVe`），但同一位的硬件语义完全不同。`npt.h` 中的映射表是理解 NPT 钩子行为的关键文档：

- **Bit 0 (Read → Present)**: 写入 `Read=1` 巧合地设置了 NPT 的 `Present=1`，使页表项有效
- **Bit 1 (Write → R/W)**: 写入 `Write=0` 巧合地设置了 NPT 的 `R/W=0`，写触发 NPF
- **Bit 2 (Execute → U/S)**: **最易误解的位**。`Execute=1` 在 NPT 中设置的是 `U/S=1`（用户+管理员都可以访问），**而不是**执行权限。执行权限由 `NX` 位（bit 63）控制
- **Bit 63 (SuppressVe → NX)**: NX 默认为 0（执行允许），所以执行**总是允许的**，但这不是因为 `Execute=1`

**核心结论**: 钩子 PTE `R=1,W=0,X=1` 能工作的真正原因是 NX=0（默认），而不是 `Execute=1`。`Execute=1` 的视觉效果是一种巧合的副作用。

`C_ASSERT` 编译期断言验证了关键位（bit 0、bit 1）在两种架构中的物理位置相同，确保共享的位域访问代码正确。

### 钩子 PTE 设置中的逐字段注释（npt.c）

`NptHookFunction()` 中的 PTE 配置块（1134-1151 行）包含每条字段的 NPT 语义注释：

```c
Pte->Read = 1;           /* NPT: Present */
Pte->Write = 0;          /* NPT: read-only (write → NPF) */
Pte->Execute = 1;        /* NPT: U/S=1 (NOT execute control!) */
Pte->PhysAddr = Hook->HookPagePa >> 12;
```

这些注释明确区分了 EPT 字段名和 NPT 实际语义，防止后续开发者误以为 `Execute=1` 控制了执行权限。该设计文档与 `npt.h` 中的映射表形成完整的文档链。

### 两遍卸载防止 UAF（Issue #7+10 镜像）

NPT 卸载使用两遍协议：
1. **第一遍**: 恢复所有 PTE 指向原始物理地址并设置 RWX
2. **冲刷 TLB**（同步 IPI 确保所有 CPU 的陈旧 TLB 条目无效）
3. **第二遍**: 释放原始页面和钩子页面

如果不这样做，某 CPU 可能在进入 HLT 状态时持有指向已释放页面的 TLB 条目，导致 UAF 崩溃。

### 哈希表优化（Issue #3+5+6）

采用与 EPT 相同设计的哈希表实现 O(1) 查找：
- **分裂页哈希表**（`g_NptSplitHashTable`）：256 个桶，支持最多 128 个分裂页（负载因子 ≤ 0.5）
- **钩子哈希表**（`g_NptHookState.HookHashTable`）：2048 个桶，支持最多 1024 个钩子（负载因子 ≤ 0.5）
- 使用 Open-addressing + 线性探测，空槽用 `-1`（`EPT_SPLIT_HASH_EMPTY`）填充
- 哈希函数使用黄金比例乘法（2654435761）除以 2^32

### M-4 修复：#DB 归属的三要素判定

修复引入 CR3 追踪和 RIP 窗口检查：
- **CR3 匹配**: 确保当前进程与松弛时的进程相同（掩去 PCID 位）
- **RIP 窗口**: 确保当前 RIP 在记录的 RIP 的 0-15 字节内（指令最大长度）
- **顺序一致性**: 通过 `_WriteBarrier` / `_ReadBarrier` 保证写入/读取顺序

之前在 #DB 分类上的错误会引起：
1. 未分类为本系统事件 → 页面保持松弛（钩子绕过）
2. 误将客户机 #DB 分类为本系统事件 → 吞噬客户机调试事件（调试器损坏）
3. 跨进程 RIP 巧合 → 误判

### H-2 修复：动态身份映射大小

NPT 身份映射大小从静态 512GB 改为根据 `MmGetPhysicalMemoryRanges()` 动态计算：
- 计算最大物理地址 + 2GB MMIO 余量
- 取整到 1GB 边界
- 最小 512GB（默认），最大 256TB
- 支持扩展到 PML4[1..N-1]（超过 512GB 的地址空间）

### 每 CPU 钩子页面隔离

每 CPU 拥有独立的 NPT 根页表，克隆流程：
1. 首次钩住某 2MB 区域时，克隆 PD 页面到所有 CPU
2. 克隆分裂页表到所有 CPU
3. 在每 CPU 的 PTE 上应用相同的钩子权限
4. NPF 和 #DB 处理都使用每 CPU PTE（`NptGetPerCpuPte()`）

优势：
- 消除多核竞争：CPU 0 修改其 PTE 不会影响 CPU 1 的 PTE
- 减少 TLB 冲刷需求：每 CPU 可以独立管理权限

### 与 Intel EPT 的对应关系

| 概念 | EPT (Intel) | NPT (AMD) |
|------|-------------|-----------|
| 页表结构 | 专用 EPT 格式 | x86-64 页表格式（复用） |
| 根地址 | VMCS EPTP 字段 | VMCB.NestedCr3 |
| 页表条目 | EPT_PTE/EPT_PDE/EPT_PDPTE/EPT_PML4E | 同上（复用，但位语义不同） |
| 大页粒度 | 2MB (PD) / 1GB (PDPT) | 同 EPT |
| Execute-Only | 支持（可达化） | **不支持** |
| 缺页异常 | EPT violation (VMEXIT) | SVM_EXIT_NPF (0x400) |
| TLB 冲刷 | INVEPT 指令 | VMCB.TlbCtl + VMRUN |
| 同步冲刷 | INVEPT + IPI | TlbCtl + CPUID(IPI) |
| 身份映射 | `ept.c` | `npt.c` |
| 钩子结构 | `EPT_HOOK_ENTRY` | 同上（复用） |
| 单步恢复 | MTF (Monitor Trap Flag) | RFLAGS.TF + #DB |
| 位语义映射 | EPT 原生语义 | npt.h 中有显式映射表 |
