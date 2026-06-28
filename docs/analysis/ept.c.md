# ept.c — 逻辑分析

## 1. 文件概述

### 角色与职责

`ept.c` 是 Intel EPT（Extended Page Tables）子系统的核心实现，负责第二个层面地址转换（GPA → HPA）的管理。EPT 是 Intel VT-x 的硬件辅助内存虚拟化技术，在 CPU 完成 Guest 虚拟地址 → Guest 物理地址（通过 Guest 页表）后，EPT 再将 GPA 转换为 Host 物理地址。

本文件的职责范围：

1. **EPT 身份映射初始化**：建立 GPA == HPA 的身份映射，覆盖全部物理内存（含 2GB headroom），使用 2MB 大页
2. **2MB → 4KB 大页拆分**：当安装 hook 时需要 4KB 粒度的权限控制，将 2MB 大页拆分为 512 个 4KB 页表
3. **EPT Hook 引擎**：通过页级拆分和权限操纵实现不可见函数挂钩（Execute-Only 页技术）
4. **EPT 违例处理**：在 EPT violation 发生时动态切换页面权限，配合 MTF 实现单步执行后恢复
5. **Per-CPU EPT 隔离**：每个 CPU 拥有独立的 EPT 页表链，消除多核竞争
6. **INVEPT 缓存管理**：EPT TLB 刷新策略，支持全局刷新和代际计数器惰性刷新

### 依赖的其他模块

| 模块 | 用途 |
|------|------|
| `ept.h` | EPT 数据结构定义、常量、函数声明 |
| `vmx.h` | VmxRead/VmxWrite 函数、VMCS 字段编码（特别用于 HandleEptViolation 和 EptSetupIdentityMap） |
| `log.h` | 日志输出（支持 VMX root 模式下的 VMXROOT_LOG_* 变体） |

---

## 2. 数据结构

### EPT 页表条目（定义于 ept.h）

**EPT_PML4E** — PML4 表项：
| 位域 | 含义 |
|------|------|
| Read/Write/Execute | 访问权限（所有级别均需要） |
| Accessed | 访问标志 |
| UserModeExecute | 用户模式执行（bit 10，仅 EPT 有） |
| PhysAddr | 下一级页表物理地址 [51:12] |

**EPT_PDPTE** — Page Directory Pointer 表项：
| 位域 | 含义 |
|------|------|
| Read/Write/Execute | 访问权限 |
| LargePage | 1GB 大页标志 |
| PhysAddr | PD 页物理地址或 1GB 页基址 |

**EPT_PDE** — Page Directory 表项：
| 位域 | 含义 |
|------|------|
| Read/Write/Execute | 访问权限 |
| LargePage | 2MB 大页标志 |
| PhysAddr | PT 页物理地址或 2MB 页基址 |

**EPT_PTE** — Page Table 表项（4KB 页）：
| 位域 | 含义 |
|------|------|
| Read/Write/Execute | 访问权限 |
| MemoryType | 内存类型 (0=UC, 6=WB) |
| IgnorePat | 忽略 PAT |
| Accessed/Dirty | 访问/脏标志 |
| UserModeExecute | 用户模式执行 |
| PhysAddr | 4KB 页物理地址 [51:12] |
| SuppressVe | 抑制虚拟化异常 (bit 63) |

**EPT_POINTER** — EPTP（VMCS 中的 EPT 指针字段）：
| 位域 | 含义 |
|------|------|
| MemoryType | 内存类型（推荐 WB=6） |
| PageWalkLength | 页表级数-1（4级=3） |
| DirtyAccess | 脏/访问标志启用 |
| Pml4PhysAddr | PML4 表物理地址 [51:12] |

### `EPT_STATE` — 全局 EPT 状态

| 字段 | 含义 |
|------|------|
| `Pml4[512]` | PML4 表（页对齐嵌入） |
| `Pdpt[512]` | PDPT 表（覆盖前 512GB，页对齐嵌入） |
| `Eptp` | EPTP 模板值 |
| `Pml4Pa` | PML4 物理地址 |
| `Initialized` | 初始化标志 |

### `EPT_CPU_STATE` — Per-CPU EPT 根结构

| 字段 | 含义 |
|------|------|
| `Pml4[512]` | 本 CPU 的 PML4 表 |
| `Pdpt[512]` | 本 CPU 的 PDPT 表 |
| `Eptp` | 本 CPU 的 EPTP 值 |
| `Pml4Pa` | 本 CPU 的 PML4 物理地址 |

### `EPT_HOOK_ENTRY` — EPT Hook 条目

| 字段 | 含义 |
|------|------|
| `Active` | 是否激活 |
| `TargetVirtualAddr` | 目标函数虚拟地址 |
| `TargetPhysicalAddr` | 目标页物理地址（4KB 对齐） |
| `TargetPageOffset` | 目标函数在页内的偏移 |
| `OriginalPageVa` | 原始页内容副本（虚拟地址） |
| `HookPageVa` | Hook 页（含 JMP 指令）虚拟地址 |
| `HookPagePa` | Hook 页物理地址 |
| `OwnsPages` | 是否拥有页的所有权（共享页时仅一个拥有者） |
| `TrampolineVa` | Trampoline（原始指令 + JMP 返回） |
| `OriginalBytes[32]` | 原始指令字节备份 |
| `OriginalBytesSize` | 原始指令字节数 |
| `HookFunction` | 替换函数指针 |
| `TargetPte` | 指向该页的 EPT PTE 指针 |

### `EPT_HOOK_STATE` — Hook 管理器

| 字段 | 含义 |
|------|------|
| `Hooks[1024]` | Hook 条目池 |
| `HookCount` | 当前激活 Hook 数 |
| `Lock` | 自旋锁保护 Hook 状态 |
| `ExecuteOnlySupported` | CPU 是否支持 Execute-Only 页 |
| `HookHashTable[2048]` | 物理地址 → Hook 索引的哈希表（开放寻址） |

### EPT 拆分相关数据结构

**`EPT_SPLIT_PAGE`** — 2MB→4KB 拆分页表：
- `Pte[512]` — 512 个 4KB 页表项
- `PhysicalAddress` — 该 PTE 数组的物理地址
- `BasePhysAddr2MB` — 覆盖的 2MB 区域基址
- `InUse` — 是否在使用

**`EPT_PER_CPU_SPLIT`** — Per-CPU 拆分页表副本：
- `Pte[512]` — 本 CPU 的 PTE 数组
- `PhysicalAddress` — 物理地址
- `Allocated` — 是否已分配

**`EPT_SPLIT_HASH_ENTRY[256]`** — 拆分页哈希表：
- `Base2MB` — 键（2MB 基地址）
- `SplitIdx` — 值（`g_SplitPages[]` 索引）

### 全局变量

| 变量 | 类型 | 含义 |
|------|------|------|
| `g_EptState` | `EPT_STATE` | 全局 EPT 状态 |
| `g_EptHookState` | `EPT_HOOK_STATE` | Hook 管理器 |
| `g_EptCpuStates` | `PEPT_CPU_STATE` | Per-CPU EPT 根数组 |
| `g_EptPdptTotal` | `ULONG` | 实际 PDPT 条目数（默认为 512） |
| `g_EptPml4Count` | `ULONG` | PML4 条目数（默认为 1） |
| `g_PdPages` | `EPT_PD_PAGE*` | PD 页数组 |
| `g_SplitPages` | `EPT_SPLIT_PAGE*` | 拆分页表池（128 个） |
| `g_EptInveptGeneration` | `volatile LONG` | INVEPT 代际计数器 |
| `g_MtfRelaxedPagePa` | `volatile ULONG64*` | Per-CPU 松弛页面跟踪 |
| `g_EptExtPdptPages` | `EPT_PDPT_PAGE*` | 扩展 PDPT 页（>512GB 支持） |
| `g_PerCpuSplitPages` | `PEPT_PER_CPU_SPLIT*` | Per-CPU 拆分页数组 |
| `g_PerCpuPdPages` | `EPT_PER_CPU_PD_PAGE**` | Per-CPU PD 页数组 |
| `g_PerCpuPdAllocated` | `PBOOLEAN` | PDPT 条目 per-CPU 分配标志 |

---

## 3. 核心函数详解

### EPT 身份映射初始化

#### `EptInitialize()`

- **签名**: `NTSTATUS EptInitialize(VOID)`
- **功能**: 构建完整的 EPT 身份映射页表
- **详细流程**:

| 步骤 | 操作 |
|------|------|
| 1 | 计算所需覆盖的物理地址范围：通过 `MmGetPhysicalMemoryRanges()` 获取最大物理地址，加 2GB headroom，向上取整到 1GB 边界，再向上取整到 512 的倍数（整个 PDPT 页） |
| 2 | 设置 `g_EptPdptTotal` 和 `g_EptPml4Count` |
| 3 | 初始化哈希表：Hook 哈希表（2048 槽位填 EMPTY）、Split 哈希表（256 槽位填 EMPTY） |
| 4 | 分配 per-CPU 跟踪数组：`g_EptInveptCpuGen` 和 `g_MtfRelaxedPagePa` |
| 5 | **检测 Execute-Only 支持**：读 `IA32_VMX_EPT_VPID_CAP[0]`。若支持则记录日志继续；若**不支持**，记录详细错误日志（含 MSR 值），释放已分配资源，返回 `STATUS_NOT_SUPPORTED`（Audit #1 修复） |
| 6 | 分配 PD 页数组：`g_EptPdptTotal` 个 PD 页 |
| 7 | 分配 `g_PerCpuPdAllocated` 位图 |
| 8 | 如果 `g_EptPml4Count > 1`，分配扩展 PDPT 页 |
| 9 | 分配拆分页表池（128 个 `EPT_SPLIT_PAGE`） |
| 10 | **构建页表结构**: |
|    | - 每个 PDPT 条目指向一个 PD 页（RWX） |
|    | - 每个 PD 条目 = 2MB 大页映射（RWX） |
|    | - PML4[0] 指向嵌入的 PDPT；PML4[1..] 指向扩展 PDPT 页 |
| 11 | 计算并保存 `Pml4Pa` 和 `Eptp`（MemoryType=WB, PageWalkLength=3(4级), DirtyAccess=0） |
| 12 | 设置 `Initialized = TRUE` |

**Audit #1 修复（Execute-Only 拒绝策略）**: 原始代码在检测到 `ExecuteOnlySupported=FALSE` 时仅记录日志并继续执行，期望在 EPT Hook 安装时退化为 R=0,W=0,X=0（全零）PTE 模式。然而 Intel SDM Vol.3C §29.3.3 明确规定：当 mode-based execute control 为 0 时（本驱动的配置），EPT PTE 位模式 000（R=0,W=0,X=0）会触发 EPT Misconfiguration，导致 VM-Exit → 无法恢复 → BSOD。换言之旧 fallback 路径是死路。由于每颗裸金属 Intel CPU 自 Westmere 微架构（2010 年）起均支持 Execute-Only EPT，在初始化阶段直接拒绝是最安全的策略——唯一不支持的情况是嵌套虚拟化环境（VMware、Hyper-V 等），而本驱动已在 `DriverEntry` 层拒绝了嵌套环境。

#### `EptComputeRequiredPdPages()`

- **功能**: 通过 `MmGetPhysicalMemoryRanges()` 查询物理内存上限，加 2GB headroom，向上取整到 1GB 和 512 对齐
- **返回值**: 所需的 PDPT 条目数

#### `EptSetupIdentityMap()`

- **签名**: `NTSTATUS EptSetupIdentityMap(struct _VMX_CPU_CONTEXT *CpuCtx, struct _VMX_STATE *State)`
- **功能**: 为指定 CPU 的 VMCS 写入 EPT Pointer
- **逻辑**: 如果 per-CPU EPT 可用，使用该 CPU 的 EPTP；否则使用共享 EPTP
- 在 `VmxSetupVmcs` 中被调用

#### `EptCleanup()`

- 先调用 `EptUnhookAll()` 移除所有 hook
- 释放：拆分页表池、PD 页、扩展 PDPT 页、per-CPU 分配位图、per-CPU 跟踪数组
- 重置哈希表

### 大页拆分

#### `EptSplitLargePage()`

- **签名**: `VOID EptSplitLargePage(ULONG64 PhysicalAddress)`
- **功能**: 将 2MB 大页拆分为 512 个 4KB 标准页
- **核心逻辑**:
  1. 计算 2MB 基址和 flat PDPT 索引
  2. 检查是否已拆分（`PDE.LargePage == 0`）→ 直接返回
  3. 从 `g_SplitPages[]` 池中找空闲条目
  4. 初始化 512 个 PTE：每个指向对应 4KB 物理页，RWX 权限
  5. 计算 PTE 数组物理地址
  6. 插入拆分页哈希表（O(1) 后续查找）
  7. 修改 PDE：清除 LargePage 位，指向拆分页表
- **限制**: 最多 128 个拆分页 (`MAX_SPLIT_PAGES`)

### 页表项查找

#### `EptGetPteForPhysicalAddress()`

- **签名**: `PEPT_PTE EptGetPteForPhysicalAddress(ULONG64 PhysicalAddress)`
- **功能**: 根据物理地址查找 EPT PTE
- **返回**: PTE 指针，或 NULL（需要先拆分）
- **逻辑**: 通过哈希表 O(1) 查找拆分页索引，如果仍是 2MB 大页返回 NULL（调用者需先拆分）

#### `EptGuestVaToPa()`

- **签名**: `static ULONG64 EptGuestVaToPa(ULONG64 GuestCr3, ULONG64 GuestVa)`
- **功能**: 通过手动遍历 Guest 页表（4 级）将 Guest VA 转换为 Guest PA
- **安全修复**: 在 VMX root 模式不用 `MmGetPhysicalAddress()`（Windows API 在 VMX root 不安全），而是用 `MmGetVirtualForPhysical` 逐级遍历 PML4→PDPT→PD→PT
- **支持**: 1GB/2MB 大页和 4KB 标准页
- **返回值**: GPA 或 0（转换失败）

### 指令长度解码和 RIP-relative 指令处理

这三个函数是 Trampoline 构建的关键工具：

#### `EptGetInstructionLength()`

- **签名**: `ULONG EptGetInstructionLength(PUCHAR Code)`
- **功能**: 确定一条 x86-64 指令的长度
- **支持的指令集**: 常见函数序言指令（MOV, PUSH, SUB, LEA, CMP, TEST, JMP, CALL, NOP, RET, INT3, SYSCALL, CMOVcc, SETcc, IMUL, ALU 操作等）
- **返回**: 指令字节数，0 = 无法解码
- **限制**: 最多 15 字节（x86-64 最大指令长度）
- **不支持的指令**: 返回 0，调用者应拒绝 hook

#### `EptIsRipRelativeInstruction()`

- **签名**: `BOOLEAN EptIsRipRelativeInstruction(PUCHAR Code, ULONG InsnLen, PULONG pDispOffset)`
- **功能**: 检查指令是否使用 RIP-relative 寻址 (ModRM Mod=00, RM=101)
- **RIP-relative 检测**: 解析前缀 → 操作码 → ModRM → 检查 Mod=00 且 RM=101
- **输出**: `pDispOffset` = disp32 字段在指令内的偏移

#### `EptRelocateRipRelativeInstruction()`

- **签名**: `BOOLEAN EptRelocateRipRelativeInstruction(PUCHAR TrampolineInsn, ULONG InsnLen, ULONG DispOffset, ULONG64 OriginalVA, ULONG64 TrampolineVA)`
- **功能**: 修复 RIP-relative 指令的 disp32 字段，使其在 Trampoline 新位置仍指向原目标地址
- **公式**: `NewDisp = TargetAddr - (TrampolineVA + InsnLen)`
- **返回**: FALSE 如果新位移超出 32 位有符号范围（目标太远）

### EPT Hook 引擎

#### `EptHookFunction()`

- **签名**: `NTSTATUS EptHookFunction(ULONG64 TargetVa, PVOID HookFunction, PVOID *OriginalFunction)`
- **功能**: 通过 EPT 页面拆分在目标函数处安装不可见 Hook
- **完整流程**:

| 步骤 | 操作 |
|------|------|
| 1 | 将 TargetVa 转换为物理地址 |
| 2 | 检查 Hook 点距离页末尾 >= 12 字节（JMP 需要 12 字节） |
| 3 | 获取自旋锁 |
| 4 | 检查重复 Hook（同一 VA 不可重复安装） |
| 5 | 检查同页其他 Hook（共享页优化） |
| 6 | 找空闲 Hook 槽位 |
| 7 | `EptSplitLargePage(TargetPa)` — 确保 4KB 粒度 |
| 8 | `EptInvalidateFromGuest()` — 立即刷新 EPT TLB（防 EPT Misconfig） |
| 9 | 获取该页的 PTE |
| 10 | **页分配**: 首次 Hook 该页时分配 OriginalPage 和 HookPage（各 4KB）并复制原始内容；后续 Hook 共享 |
| 11 | **Trampoline 分配**: 每个 Hook 独占 64 字节 Trampoline |
| 12 | **指令解码**: 通过 `EptGetInstructionLength` 找到覆盖 >= 12 字节的完整指令序列 |
| 13 | **Hook 页打补丁**: 在 `HookPage + PageOffset` 写入 `48 B8 [imm64] FF E0`（MOV RAX, imm64; JMP RAX） |
| 14 | **Trampoline 构建**: 复制原始指令 → RIP-relative 修正 → 尾部写入 FF 25 [RIP+0] [8-byte target] |
| 15 | **PTE 权限设置** (关键步骤): |
|    | - Execute-Only 支持: R=0, W=0, X=1（PhysAddr = HookPage） |
|    | - 不支持: 不可达路径。`EptInitialize()` 在初始化阶段已拒绝不支持 Execute-Only 的 CPU。旧代码的 else 分支（R=0,W=0,X=0）保留仅作编译兼容，实际永不被执行 |
| 16 | **Per-CPU 隔离**: 为所有 CPU 克隆 PD 和 PT，复制相同 PTE 权限 |
| 17 | 插入 Hook 哈希表 |
| 18 | 释放自旋锁，执行 `EptInvalidateFromGuest()` |
| 19 | 返回 Trampoline 地址作为 `OriginalFunction` |

- **JMP 编码**: 使用 `MOV RAX, imm64; JMP RAX`（12 字节绝对跳转）而非 `FF 25` RIP-relative 编码（避免在只执行页上因读取内存导致 EPT 违例循环）

#### `EptUnhookFunction()`

- **签名**: `NTSTATUS EptUnhookFunction(ULONG64 TargetVa)`
- **功能**: 移除指定 VA 的 Hook
- **逻辑**:
  1. 在 Hook 页还原原始指令字节
  2. 检查是否还有其他 Hook 在同一页面
  3. **无其他 Hook → 两遍卸载**:
     - Pass 1: 恢复 EPT PTE（RWX） + 刷新 TLB
     - Pass 2: 释放页面
  4. **有其他 Hook → 仅释放 Trampoline，转移页面所有权**
  5. 重建 Hook 哈希表

#### `EptUnhookAll()`

- **签名**: `VOID EptUnhookAll(VOID)`
- **功能**: 移除所有 EPT Hook
- **两遍卸载**:
  1. Pass 1: 恢复所有 EPT PTE 指向原始物理页（RWX），释放自旋锁
  2. `EptInvalidateAllCpusSync()` — 同步 IPI 刷新所有 CPU 的 EPT TLB
  3. Pass 2: 重新获取锁，释放所有页面和 Trampoline

### EPT 违例处理

#### `HandleEptViolation()`

- **签名**: `BOOLEAN HandleEptViolation(PVOID GuestContext)`
- **功能**: 处理 EPT 违例 VM-Exit（EPT violation 和 EPT misconfig 的主要处理函数）
- **核心逻辑**:

**Step 1: 读取信息**
- `VMCS_GUEST_PHYSICAL_ADDRESS` — 引发违例的 GPA
- `VMCS_EXIT_QUALIFICATION` — 违例详情（读/写/执行、当前权限）

**Step 2: 查找 Hook**
- 通过 `EptFindHookByPhysicalAddress`（哈希表 O(1)）检查违例地址是否在 Hook 页上

**Step 3: 非 Hook 页处理**
- 超出身份映射范围 → 返回 FALSE（无法恢复，关闭 VMX）
- 在范围内但权限受限 → 设置 RWX 权限 + INVEPT（自身立即）+ 代际通知（其他 CPU）

**Step 4: Hook 页处理（Mode A — Execute-Only 支持）**

| 访问类型 | 操作 |
|----------|------|
| 读/写 (IsRead \|\| IsWrite) | 临时切换到原始页 (R=1,W=1,X=0) + MTF + INVEPT SINGLE |
| 执行 (IsExec) | 切换回 Hook 页 (R=0,W=0,X=1) + INVEPT SINGLE |

**Step 5: Hook 页处理（Mode B — 无 Execute-Only）**

此路径当前不可达（`EptInitialize` 在初始化阶段拒绝）。保留代码仅作编译兼容。

**Step 6: Per-CPU PTE 隔离**
- 优先使用当前 CPU 的 per-CPU PTE（通过 `EptGetPerCpuPte`）
- 记录松弛页面 → MTF handler 恢复

### MTF 跟踪

#### `EptMtfTrackRelaxedPage()`
- 记录当前 CPU 松弛的物理页到 `g_MtfRelaxedPagePa[CpuIndex]`

#### `EptMtfGetAndClearRelaxedPage()`
- 读取并清除当前 CPU 的松弛页面记录

### Per-CPU EPT 管理

#### `EptInitPerCpu()`

- 分配 `g_EptCpuStates[g_MaxProcessors]` = 每个 CPU 的 PML4 + PDPT + EPTP
- 分配 `g_PerCpuSplitPages[g_MaxProcessors]` = per-CPU 拆分页表指针
- 分配 `g_PerCpuPdPages[g_MaxProcessors]` = per-CPU PD 页指针
- 如果 `g_EptPml4Count > 1`：分配 per-CPU 扩展 PDPT 页
- 克隆 PML4、PDPT 和扩展 PDPT 到每个 CPU，调整 PML4 指向各自的 PDPT

#### `EptEnsurePerCpuPdForRegion()`

- 当 Hook 安装到某 2MB 区域时，为所有 CPU 克隆该区域的 PD 页
- 更新每个 CPU 的 PDPT 条目指向其私有的 PD 页

#### `EptEnsurePerCpuSplitPage()`

- 当 Hook 安装时，为所有 CPU 克隆拆分页表的 PT 页
- 更新每个 CPU 的 PD 条目指向其私有的 PT 页

#### `EptGetPerCpuPte()`

- 通过拆分页哈希表 O(1) 查找，返回指定 CPU 的 per-CPU PTE

#### `EptGetPerCpuEptp()`

- 返回指定 CPU 的 EPTP 值（用于 INVEPT SINGLE_CONTEXT）

### INVEPT 封装

#### `EptInvalidateAllContexts()`
- 执行 `INVEPT ALL_CONTEXTS`（需在 VMX root 模式调用）

#### `EptInvalidateSingleContext()`
- 执行 `INVEPT SINGLE_CONTEXT`（只刷新指定 EPTP 的 TLB）

#### `EptInvalidateFromGuest()`
- **递增** `g_EptInveptGeneration`（代际计数器）
- 每个 CPU 在下次 VM-Exit 时检测到落后 → 执行 INVEPT
- 不需 VMCALL（VMware 嵌套虚拟化会拦截 VMCALL）

#### `EptCheckPendingInvept()`
- 在 VM-Exit handler 入口调用
- 比较 `g_EptInveptCpuGen[CpuIndex]` 和 `g_EptInveptGeneration`
- 落后则执行 `EptInvalidateAllContexts()`

#### `EptInvalidateAllCpusSync()` (H-5)
- **同步 IPI 刷新**：通过 `KeIpiGenericCall` 在所有 CPU 上执行 `EptInveptIpiCallback`
- 回调函数直接在 IPI 处理上下文中执行 INVEPT
- 配合代际计数器（先递增计数器，再发 IPI），双重保障
- 解决 HLT 状态下 CPU 长期无 VM-Exit 导致的 UAF 问题

---

## 4. 控制流与逻辑流程

### EPT 初始化流程

```
VmxInitialize (vmx_init.c)
  ├── EptInitialize() — 全局 EPT 身份映射构建
  │   ├── EptComputeRequiredPdPages — 物理内存范围查询 + 2GB headroom
  │   ├── 检测 Execute-Only 支持 —— 不支持则拒绝启动（Audit #1）
  │   ├── 分配 PD 页数组 / 拆分页池 / 哈希表
  │   ├── 构建 PML4[0..N-1] → PDPT → PD(2MB大页) 结构
  │   └── 计算 EPTP
  └── EptInitPerCpu() — Per-CPU EPT 隔离
       ├── 分配 g_EptCpuStates[g_MaxProcessors]
       ├── 分配 per-CPU 拆分页 / PD 页指针数组
       ├── 分配 per-CPU 扩展 PDPT（>512GB时）
       └── 克隆 PML4/PDPT 到每个 CPU
```

### EPT Hook 安装流程

```
EptHookFunction(TargetVa, HookFunction, &OrigFunc)
  ├── MmGetPhysicalAddress → TargetPa
  ├── EptSplitLargePage(TargetPa) — 确保4KB粒度
  │   ├── 从 g_SplitPages[] 分配拆分页
  │   ├── 初始化512个PTE指向4KB页
  │   └── 修改PDE指向拆分页表
  ├── EptInvalidateFromGuest() — 立即刷新（防Misconfig）
  ├── EptGetPteForPhysicalAddress — 获取PTE
  ├── 分配 OriginalPage/HookPage + 复制原始代码
  ├── 分配 Trampoline
  ├── 指令解码 + RIP-relative修正
  ├── Hook页打JMP补丁 (48 B8 imm64 FF E0)
  ├── 设置PTE: Execute-Only (R=0,W=0,X=1) — 不支持情况已在初始化拒绝
  ├── (可选) EptEnsurePerCpuPdForRegion + EptEnsurePerCpuSplitPage
  ├── 插入哈希表
  └── EptInvalidateFromGuest()
```

### EPT Violation → MTF 循环

```
Guest执行 → EPT Violation VM-Exit
  ↓
HandleEptViolation
  ├── 查找Hook（哈希表O(1)）
  ├── 确定模式A/B
  ├── 临时修改PTE: 展示原始页/Hook页
  ├── EptMtfTrackRelaxedPage
  ├── 设置MTF标志
  └── return TRUE → VMRESUME
       ↓
Guest重新执行该指令（一条）
  → 退出到 VMX root → MTF VM-Exit
       ↓
HandleMtf
  ├── 清除MTF标志
  ├── EptMtfGetAndClearRelaxedPage
  ├── 恢复PTE (R=0,W=0,X=1)
  ├── INVEPT SINGLE_CONTEXT
  └── return TRUE → VMRESUME
```

### Hook 卸载流程

```
EptUnhookFunction(TargetVa)
  ├── 还原Hook页上的原始指令
  ├── 检查同页其他Hook
  ├── 无其他Hook:
  │   ├── Pass 1: 恢复PTE (RWX + 原始物理地址)
  │   ├── EptInvalidateAllCpusSync() — 同步IPI刷新
  │   └── Pass 2: 释放页面
  └── 有其他Hook:
       ├── 转移页面所有权
       ├── 释放Trampoline
       └── EptInvalidateFromGuest()
```

---

## 5. 与其他模块的交互

### 与 VMX 初始化的交互

- `EptInitialize()` 在 `VmxInitialize()` 中被调用，在分配 CPU 上下文之后、VMXON 之前
- `EptSetupIdentityMap()` 在每个 CPU 的 `VmxSetupVmcs()` 中被调用来设置 EPTP
- `EptInitPerCpu()` 在 VMXON 之前调用，每个 CPU 克隆自己的 EPT 根结构

### 与 VM-Exit handler 的交互

- **`EptCheckPendingInvept()`** 在每个 VM-Exit 入口被 `VmxExitHandler` 调用
- **`HandleEptViolation()`** 是 `HandleEptViol` 的完全委托函数
- **`HandleMtf()`** 在 `vmx_exit.c` 中直接调用 `EptMtfGetAndClearRelaxedPage()`、`EptFindHookByPhysicalAddress()`、`EptGetPerCpuPte()`、`EptGetPerCpuEptp()`、`EptInvalidateSingleContext()`

### 与 HV_OPS vtable 的交互

ept.c 的函数通过 `vmx_init.c` 中的包装函数注册到 `g_VmxOps`：

| HV_OPS 函数 | EPT 后端 |
|-------------|----------|
| `SetupPageTables` | `EptInitialize()` |
| `CleanupPageTables` | `EptCleanup()` |
| `InvalidatePageTables` | `EptInvalidateFromGuest()` |
| `HookFunction` | `EptHookFunction(TargetVa, HookFunc, OrigFunc)` |
| `UnhookFunction` | `EptUnhookFunction(TargetVa)` |
| `UnhookAll` | `EptUnhookAll()` |

---

## 6. 关键设计要点

### Execute-Only 页技术

Execute-Only 页（R=0, W=0, X=1）是 EPT Hook 的核心隐身机制：

- **执行触发**: 当 Guest 执行该页上的指令时，CPU 允许 execute-only 访问
- **数据读取触发 EPT Violation**: 当 PatchGuard 等完整性扫描器读取该页时，触发违例 → Handler 展示原始未修改的页内容
- **PatchGuard 绕过**: 扫描器读取的是原始（未 Hook）代码，Hook 的存在完全不可见

**Audit #1 修复（SDM §29.3.3 依据）**: 旧代码在 `ExecuteOnlySupported=FALSE` 时尝试退化为 `R=0,W=0,X=0`（全零）PTE 模式，期望通过 EPT Violation handler 动态切换权限来维持功能。然而 Intel SDM Vol.3C §29.3.3 规定 EPT PTE 位模式 000（R=0,W=0,X=0）在 mode-based execute control 为 0 时触发 EPT Misconfiguration（而非 EPT violation），导致 VM-Exit → 无法恢复 → BSOD。这意味着旧的 fallback 路径从硬件层面就是不可用的死路。

新行为（Audit #1）：`EptInitialize()` 在检测到 `ExecuteOnlySupported=FALSE` 时立即返回 `STATUS_NOT_SUPPORTED`，并记录三条详细错误日志（含 IA32_VMX_EPT_VPID_CAP MSR 值）。该拒绝策略的安全性基于以下事实：
- 每颗裸金属 Intel CPU 自 Westmere 微架构（2010）起均支持 Execute-Only EPT
- 唯一不支持的情况是嵌套虚拟化环境（VMware、Hyper-V 等），而本驱动在 `DriverEntry` 阶段已拒绝了嵌套环境
- `EptHookFunction()` 的 else 分支（Pte->Execute = 0）保留仅作编译兼容，实际永不被执行

### 2MB → 4KB 大页拆分

身份映射默认使用 2MB 大页以最小化页表内存占用和 TLB 压力。但 EPT Hook 需要 4KB 粒度的权限控制，故按需拆分：

- 拆分池大小：128 个拆分页表（覆盖 128 × 2MB = 256MB 地址空间）
- 哈希表维护拆分索引的 O(1) 查找
- 拆分后立即 INVEPT（否则其他 CPU 的陈旧 TLB 项指向旧的大页 PDE 格式，导致 EPT Misconfiguration）

### Per-CPU EPT 隔离

**核心动机**: 解决多核 EPT 权限切换的竞争条件。

在没有 per-CPU 隔离的情况下：
1. CPU 0 触发 EPT violation → 修改共享 PTE（R+W+X 展示原始页）→ 设置 MTF
2. CPU 1 触发同一页的 EPT violation → 修改共享 PTE（R+W+X 展示 Hook 页）
3. CPU 0 的 MTF 触发 → 恢复 PTE 到 `Execute-Only` → CPU 1 的 Hook 被撤销 → CPU 1 下次执行时重复违例 → 无限循环

**解决方案**: 每个 CPU 拥有完整的 EPT 页表链副本：
- 共享部分：PD 页在无 Hook 时共享
- 私有部分：有 Hook 的 2MB 区域的 PD 和 PT 页每 CPU 独立
- 切换和恢复只影响本 CPU 的 PTE

### O(1) 哈希表优化

**挂钩哈希表**: 物理页地址 → Hook 索引
- 2048 槽位，开放寻址 + 线性探测
- 场景: 每次 EPT violation 都需要查找 Hook
- 优化: 从 O(1024) 线性扫描降为 O(1) 哈希查找

**拆分页哈希表**: 2MB 基址 → 拆分页索引
- 256 槽位，开放寻址 + 线性探测
- 场景: `EptGetPteForPhysicalAddress` 和 `EptGetPerCpuPte` 频繁调用

### 两遍卸载（Pass-1/Pass-2）

**安全问题**: 直接释放 HookPage 内存时，其他 CPU 的陈旧的 EPT TLB 仍可能指向该物理页 → UAF → BSOD

**解决方案**:
1. Pass 1: 恢复 PTE 指向原始物理页（RWX 权限）
2. 刷新所有 CPU 的 EPT TLB（`EptInvalidateAllCpusSync` 通过 IPI 同步刷新）
3. Pass 2: 安全释放页面

### 同步 TLB 刷新 (H-5)

`EptInvalidateAllCpusSync()` 使用 `KeIpiGenericCall` 在所有 CPU 上同步执行 INVEPT：

- 对于在 HLT 状态的 CPU：IPI 导致外部中断 VM-Exit → 中断处理在 VMX root 执行 → 在其上下文中执行 INVEPT
- 比代际计数器（惰性）更可靠：计数器机制需要 CPU 发生 VM-Exit 才能检查，而 HLT 的 CPU 可能无限期不退出

### 身份映射大小自动调整

传统实现在 512GB 物理内存的系统中浪费内存（分配超过需求），在超过 512GB 的系统中失败（映射不完整）。

**H-2 修复**: 动态查询物理内存范围 (`MmGetPhysicalMemoryRanges`)，加 2GB headroom，支持自动扩展到全 256TB 地址空间。额外 PDPT 页通过 `g_EptExtPdptPages` 动态分配，per-CPU 版本通过 `g_EptCpuExtPdpt` 分配。

### Trampoline RIP-relative 修正

x64 代码常用 RIP-relative 寻址（`LEA REG, [RIP+disp32]`、`MOV REG, [RIP+disp32]`）。将指令复制到 Trampoline（新 VA）后，disp32 不再正确指向原目标。

修复策略：计算 `原始目标地址 = 原始VA + InsnLen + OrigDisp`，然后写出 `新Disp = 原始目标地址 - (TrampolineVA + InsnLen)`。如果新位移超出 32 位范围，拒绝 Hook。

### JMP 编码选择

使用 `MOV RAX, imm64; JMP RAX`（12 字节）而非传统的 `FF 25 [RIP+0] [8-byte addr]`（14 字节 RIP-relative 间接跳转）。

原因：RIP-relative 编码在执行时会从目标地址**读取** 8 字节目标指针。在 Execute-Only 页上（R=0, W=0, X=1），读取操作触发 EPT violation，导致无限违例循环。`MOV RAX, imm64` 将目标地址编码为立即数（在指令流中），不产生数据读取。
