# AMD SVM 参考手册 — 补充章节

> **说明:** 本文档是对 `amd-svm-reference.md` 的补充，涵盖 SEV/SEV-ES/SEV-SNP、#VC 异常处理、VMSA 结构、GHCB 协议、硬件勘误、罕见 #VMEXIT 场景、嵌套虚拟化、性能调优及新兴 SVM 特性。
> **参考文档:** AMD APM Vol.2 (#24593), SEV-ES GHCB Spec (#56421), SEV-SNP Firmware ABI Spec
> **语言:** 中文

---

## 目录（补充章节）

32. [SEV 安全加密虚拟化 完整技术详解](#32-sev-安全加密虚拟化-完整技术详解)
33. [#VC 异常处理详解 (SEV-ES)](#33-vc-异常处理详解-sev-es)
34. [VMSA 结构详解](#34-vmsa-结构详解)
35. [GHCB 通信协议详解](#35-ghcb-通信协议详解)
36. [AMD 硬件勘误与 Workaround](#36-amd-硬件勘误与-workaround)
37. [罕见的 #VMEXIT 场景处理](#37-罕见的-vmexit-场景处理)
38. [AMD-V 嵌套虚拟化 (Nested SVM)](#38-amd-v-嵌套虚拟化-nested-svm)
39. [SEV-SNP 远程证明 (Attestation)](#39-sev-snp-远程证明-attestation)
40. [AMD-V 性能调优指南](#40-amd-v-性能调优指南)
41. [VMCB Permissive Mode (Family 19h+)](#41-vmcb-permissive-mode-family-19h)
42. [ROGPT (Read-Only Guest Page Table)](#42-rogpt-read-only-guest-page-table)
43. [GMET (Guest Mode Execute Trap)](#43-gmet-guest-mode-execute-trap)

---

## 32. SEV 安全加密虚拟化 完整技术详解

### 32.1 SEV 概念

SEV (Secure Encrypted Virtualization) 是 AMD 的内存加密虚拟化技术，使用 AES-128-XTS 加密引擎对每个客户机的内存进行透明加密。

- **引入:** Zen 1 (Family 17h)
- **CPUID 检测:** `Fn8000_000A_EDX[SEV]` (位 30) = 1
- **CPUID 详细能力:** `Fn8000_001F` — SEV 特性叶子
- **加密引擎:** 集成在内存控制器中，AES-128-XTS 模式
- **性能影响:** 约 3-5% 的延迟开销

SEV 的核心思想是每个客户机 VM 拥有唯一的 AES 加密密钥，由 AMD 安全处理器 (PSP/SP) 管理。内存控制器在写入 DRAM 时自动加密，在读取时自动解密 — CPU 流水线完全不感知加密过程。

### 32.2 SEV vs SEV-ES vs SEV-SNP

| 特性 | SEV | SEV-ES | SEV-SNP |
|:----:|:---:|:------:|:-------:|
| 内存加密 (AES-128-XTS) | 是 | 是 | 是 |
| 寄存器状态加密 | 否 | 是 (VMSA 加密) | 是 |
| #VC 异常 | 否 | 是 (向量 29) | 是 |
| GHCB 通信 | 否 | 是 | 是 |
| 内存完整性保护 | 否 | 否 | 是 (RMP) |
| 重放攻击防护 | 否 | 否 | 是 |
| 重映射攻击防护 | 否 | 否 | 是 |
| VMPL 支持 | 否 | 否 | 可选 |
| vTOM 支持 | 否 | 否 | 可选 |
| 引入世代 | Zen 1 | Zen 3 | Zen 4 |

### 32.3 C-bit (加密位)

C-bit 是页表项中用于标记页面是否加密的物理地址位。

**C-bit 位置:**
- 通常位于物理地址的 **位 47** (对于 48 位物理地址空间的系统)
- 某些配置中可以是 **位 51**
- 通过 `CPUID Fn8000_001F_EBX` 的低 5 位获取 C-bit 位置
- C-bit 消耗一个物理地址位，因此实际可用物理地址减少 (PhysAddrReduction)

**C-bit 编码 (页表项):**
- **C-bit = 1:** 页面使用客户机唯一密钥加密 (Private)
- **C-bit = 0:** 页面不加密 (Shared) — 用于 DMA 和与 Hypervisor 通信

> **重要:** 客户页表本身和代码页面总是被加密的，无论 C-bit 如何设置。

### 32.4 SEV 加密密钥管理

**密钥层次结构:**

```
CEK (Chip Endorsement Key) ───→ PEK (Platform Endorsement Key) ───→ PDH (Platform Diffie-Hellman)
       (工厂烧录)                        (平台唯一)                        (会话临时)
```

| 密钥 | 描述 |
|:----|------|
| **CEK** | 芯片背书密钥 — 工厂烧录在 PSP 中的私钥。仅用于签名 PEK |
| **PEK** | 平台背书密钥 — 通过 `PEK_CSR` 命令生成的平台唯一密钥对 |
| **PDH** | 平台 Diffie-Hellman 密钥 — 每个客户机启动时生成的临时会话密钥 |
| **会话密钥** | 在客户机启动过程中由客户所有者生成，用于建立信任 |
| **ASID 派生密钥** | 每个客户机唯一的 AES 加密密钥，从 ASID 衍生 |

**密钥协商流程:**
1. PSP 通过 `PDH` 命令导出平台 PDH 证书链 (CEK → PEK → PDH)
2. 客户所有者获取 PDH 证书链并验证 (AMD Root CA 签名)
3. 客户所有者生成会话密钥，使用 PDH 公钥加密后发送给 PSP
4. PSP 使用 PDH 私钥解密会话密钥
5. 会话密钥用于后续的 LAUNCH_UPDATE_DATA 加密传输

### 32.5 CPUID 0x8000001F — SEV 特性检测

| 寄存器 | 位域 | 描述 |
|--------|:----:|------|
| **EAX** | 31:0 | 支持的加密类型位图: 位 0 = SME, 位 1 = SEV, 位 2 = SEV_ES, 位 3 = SEV_SNP |
| **EBX** | 7:0 | C-bit 位置 (物理地址中的位号) |
| **EBX** | 31:8 | PhysAddrReduction — 因 C-bit 减少的物理地址位数 |
| **ECX** | 31:0 | 保留 |
| **EDX** | 31:0 | 每 ASID 的加密页面数 (最小数量) |

**EAX 加密类型位图详解:**

| 位 | 名称 | 描述 |
|:--:|:----:|------|
| **0** | SME | Secure Memory Encryption — 全内存加密 (非虚拟化) |
| **1** | SEV | Secure Encrypted Virtualization |
| **2** | SEV-ES | SEV Encrypted State |
| **3** | SEV-SNP | SEV Secure Nested Paging |
| 4-31 | — | 保留 |

### 32.6 SEV 客户机启动流程 (6 阶段)

SEV 客户机启动涉及 Hypervisor、AMD SP (安全处理器) 和客户所有者三方的交互。

```
客户所有者                  Hypervisor                      AMD SP
    |                           |                              |
    |  1. 请求 PDH 证书链        |                              |
    |  ←────────────────────── |                              |
    |  2. 验证 PDH 链           |                              |
    |  3. 生成会话密钥          |                              |
    |  4. LAUNCH_START         |                              |
    |  ──────────────────────→ |  ──── LAUNCH_START ────────→ |
    |                          |  ←─── HANDLE (句柄) ──────── |
    |  5. LAUNCH_UPDATE_DATA   |                              |
    |  ──────────────────────→ |  ──── LAUNCH_UPDATE_DATA ──→ |
    |                          |      (内核, initrd, etc.)    |
    |  6. LAUNCH_MEASURE       |                              |
    |  ──────────────────────→ |  ──── LAUNCH_MEASURE ──────→ |
    |  ←── 度量值 (SHA-256) ── |  ←─── 度量值 ────────────── |
    |  7. 验证度量值           |                              |
    |  8. LAUNCH_FINISH        |                              |
    |  ──────────────────────→ |  ──── LAUNCH_FINISH ───────→ |
    |                          |  ←─── OK ──────────────────  |
```

**阶段 1 — 平台所有权 (PDH 证书链导出):**
- Hypervisor 调用 `PSP_PDH_CERT_EXPORT` 固件命令
- 返回 PDH 证书链: CEK 证书 → PEK 证书 → PDH 证书
- 每个证书是 DER 格式的 ECDSA-384 签名证书

**阶段 2 — 客户所有者验证:**
- 客户所有者从 AMD KDS (Key Distribution Service) 获取 AMD Root CA 证书
- 验证证书链签名 (ARK → ASK → CEK → PEK → PDH)
- 验证平台身份

**阶段 3 — LAUNCH_START:**
- 客户所有者生成一次性会话密钥 (AES-128 + 完整性密钥)
- 使用 PDH 公钥加密会话密钥
- 调用 `LAUNCH_START` 命令，传入加密的会话密钥和客户机策略 (Policy)
- SP 解密会话密钥，为客户机分配加密上下文
- 返回客户机句柄 (Handle)，用于后续操作

**阶段 4 — LAUNCH_UPDATE_DATA:**
- Hypervisor 为客户机注入加密数据 (内核、initrd、cmdline 等)
- 数据使用会话密钥进行加密传输
- SP 解密数据，使用客户机唯一密钥重新加密后写入客户机内存
- 每次 UPDATE 更新启动度量值

**阶段 5 — LAUNCH_MEASURE:**
- Hypervisor 调用命令获取客户机启动度量
- SP 返回所有已注入数据的 SHA-256 哈希值
- 客户所有者验证度量值是否匹配预期值
- 度量值确保客户机环境未被篡改

**阶段 6 — LAUNCH_FINISH:**
- 客户所有者确认度量值正确
- 调用 `LAUNCH_FINISH` 命令固化客户机状态
- SP 锁定客户机配置，完成启动流程
- Hypervisor 可以开始执行客户机代码

### 32.7 SEV 固件命令集

| 命令码 | 名称 | 描述 |
|:-----:|:----|------|
| **0x000** | PSP_INIT | 初始化 PSP |
| **0x001** | PSP_EXIT | 关闭 PSP |
| **0x002** | GET_ID | 获取芯片 ID (64 位) |
| **0x004** | GET_FW_VERSION | 获取固件版本 |
| **0x006** | PDH_CERT_EXPORT | 导出 PDH 证书链 |
| **0x007** | PEK_CSR | 生成 PEK 证书签名请求 |
| **0x008** | PEK_CERT_IMPORT | 导入 PEK 证书 |
| **0x009** | GET_CERT_ID | 获取证书扩展 ID |
| **0x00B** | PDH_GEN | 生成新的 PDH 密钥对 |
| **0x00C** | SET_EXT_OWNER | 设置扩展所有者 |
| **0x00D** | EXT_OWNED_STATUS | 查询扩展所有者状态 |
| **0x010** | LAUNCH_START | 客户机启动 — 创建加密上下文 |
| **0x011** | LAUNCH_UPDATE_DATA | 客户机启动 — 注入加密数据 |
| **0x012** | LAUNCH_UPDATE_VMSA | 客户机启动 — 注入加密 VMSA (SEV-ES) |
| **0x013** | LAUNCH_SECRET | 客户机启动 — 注入机密数据 |
| **0x014** | LAUNCH_MEASURE | 客户机启动 — 获取度量值 |
| **0x015** | LAUNCH_FINISH | 客户机启动 — 完成启动 |
| **0x020** | ACTIVATE | 激活已启动的客户机 |
| **0x021** | DEACTIVATE | 停用客户机 |
| **0x022** | DEACTIVATE_ALL | 停用所有客户机 |
| **0x024** | GET_POLICY | 获取客户机策略 |
| **0x025** | SET_POLICY | 设置客户机策略 |
| **0x030** | GUEST_STATUS | 查询客户机状态 |
| **0x031** | PLATFORM_STATUS | 查询平台状态 |
| **0x040** | COMMIT | 提交 TCB 版本 |
| **0x041** | GET_TCB_STATUS | 获取 TCB 状态 |
| **0x050** | GET_RMP_PAGE | 获取 RMP 表项值 |
| **0x051** | SET_RMP_PAGE | 设置 RMP 表项 |
| **0x060** | SNP_PLATFORM_STATUS | SNP 平台状态 |
| **0x061** | SNP_INIT | SNP 初始化 |
| **0x062** | SNP_SET_CONFIG | SNP 配置设置 |
| **0x063** | SNP_LAUNCH_START | SNP 启动开始 |
| **0x064** | SNP_LAUNCH_UPDATE | SNP 启动更新 |
| **0x065** | SNP_LAUNCH_FINISH | SNP 启动完成 |
| **0x066** | SNP_GUEST_STATUS | SNP 客户机状态 |
| **0x067** | SNP_DF_FLUSH | SNP 数据填充刷新 |
| **0x068** | SNP_SHUTDOWN | SNP 关闭 |
| **0x069** | SNP_DOWNLOAD_FIRMWARE | SNP 固件下载 |
| **0x06A** | SNP_GET_CONFIG | SNP 配置读取 |
| **0x06B** | SNP_COMMIT | SNP 提交 TCB |
| **0x070** | SNP_DBG_DECRYPT | SNP 调试解密 |
| **0x071** | SNP_DBG_EXPORT | SNP 调试导出 |
| **0x072** | SNP_DF_FLUSH_START | SNP DF 刷新开始 |
| **0x073** | SNP_DF_FLUSH_END | SNP DF 刷新结束 |
| **0x074** | SNP_DOWNLOAD_FIRMWARE_EXT | SNP 扩展固件下载 |
| **0x080** | SNP_SET_SECURE_TCB | SNP 设置安全 TCB |
| **0x081** | SNP_GET_GUEST_VMPL | SNP 获取 VMPL |

### 32.8 SEV 固件命令缓冲区格式

所有 SEV 固件命令通过 Hypervisor 与 PSP 之间的共享缓冲区传递。缓冲区格式:

```c
struct sev_cmd_buffer {
    uint32_t cmd;          // 命令码 (参见 32.7 节)
    uint32_t len;          // 数据长度
    uint8_t  data[];       // 命令参数数据
};

// 返回状态码
struct sev_cmd_response {
    uint32_t status;       // 状态码 (参见下方)
};
```

### 32.9 SEV 固件返回码

| 返回码 | 名称 | 描述 |
|:-----:|:----|------|
| **0x0000_0000** | SEV_RET_SUCCESS | 成功 |
| **0x0000_0001** | SEV_RET_FAIL | 一般性失败 |
| **0x0000_0002** | SEV_RET_BAD_SIGNATURE | 签名验证失败 |
| **0x0000_0003** | SEV_RET_RESOURCE_LIMIT | 资源不足 |
| **0x0000_0004** | SEV_RET_SECURE_DATA_INVALID | 安全数据无效 |
| **0x0000_0005** | SEV_RET_INVALID_PAGE_SIZE | 无效的页面大小 |
| **0x0000_0006** | SEV_RET_INVALID_PAGE_TYPE | 无效的页面类型 |
| **0x0000_0007** | SEV_RET_INVALID_GUEST | 无效的客户机句柄 |
| **0x0000_0008** | SEV_RET_INVALID_CONFIG | 无效的配置 |
| **0x0000_0009** | SEV_RET_INVALID_LEN | 无效的长度 |
| **0x0000_000A** | SEV_RET_ALREADY_ACTIVE | 客户机已激活 |
| **0x0000_000B** | SEV_RET_ALREADY_STOPPED | 客户机已停止 |
| **0x0000_000C** | SEV_RET_ALREADY_OWNED | 芯片已有所有者 |
| **0x0000_000D** | SEV_RET_FW_IS_OWNED | 固件被锁定 |
| **0x0000_000E** | SEV_RET_POLICY_FAILURE | 策略检查失败 |
| **0x0000_000F** | SEV_RET_INVALID_PARAMETER | 无效的参数 |
| **0x0000_0010** | SEV_RET_BUSY | 设备忙 |
| **0x0000_0011** | SEV_RET_INVALID_ADDR | 无效地址 |
| **0x0000_0012** | SEV_RET_BAD_ADDR | 错误地址 (RMP 违规) |
| **0x0000_0013** | SEV_RET_BAD_MEASUREMENT | 度量值不匹配 |
| **0x0000_0014** | SEV_RET_INVALID_ASID | 无效 ASID |
| **0x0000_0015** | SEV_RET_INVALID_ASID_RANGE | ASID 范围无效 |
| **0x0000_0016** | SEV_RET_PLATFORM_IS_OWNED | 平台已被所有 |
| **0x0000_0017** | SEV_RET_PLATFORM_IS_NOT_OWNED | 平台未被所有 |
| **0x0000_0018** | SEV_RET_UNITIALIZED | 固件未初始化 |
| **0x0000_0019** | SEV_RET_BAD_BUFFER | 缓冲区格式错误 |
| **0x0000_001A** | SEV_RET_DEACTIVATED | 客户机已停用 |
| **0x0000_001B** | SEV_RET_ACTIVATING | 客户机激活中 |
| **0x0000_001C** | SEV_RET_INVALID_CPU | 无效的 CPU 选择 |
| **0x0000_001D** | SEV_RET_INVALID_VMSA | 无效的 VMSA |
| **0x0000_001E** | SEV_RET_GUEST_COUNT_LIMIT | 客户机数量上限 |
| **0x0000_001F** | SEV_RET_TCG_VERSION | TCB 版本冲突 |
| **0x0000_0020** | SEV_RET_INVALID_VERSION | 无效的固件版本 |
| **0x0000_0021** | SEV_RET_HW_ERROR | 硬件错误 |
| **0x0000_0022** | SEV_RET_FW_INTERNAL_ERROR | 固件内部错误 |
| **0x0000_0023** | SEV_RET_FW_BIST_FAILURE | 内建自测试失败 |
| **0x0000_0024** | SEV_RET_AUTH_FAILURE | 固件认证失败 |
| **0x0000_0025** | SEV_RET_UNRECOVERABLE | 不可恢复错误 |

### 32.10 SEV-ES 详细机制

SEV-ES (Encrypted State) 在 SEV 基础上增加了寄存器状态保护。

**核心机制:**
- **VMSA 加密:** VMCB 保存区域 (VMSA) 被 AES 加密，Hypervisor 无法读取客户寄存器状态
- **自动退出 (Automatic Exit, AE):** 对于安全的退出原因 (某些 #VMEXIT 类型)，硬件直接在 VMSA 中填写 EXITINFO1/2 而不需要 Hypervisor 读取寄存器
- **#VC 异常:** 当客户机需要执行可能暴露寄存器状态的指令时，触发 #VC 异常
- **GHCB 页面:** 客户机与 Hypervisor 之间的共享通信页面 (不加密)
- **VMGEXIT 指令:** 客户机通过 `VMGEXIT` (F3 0F 01 D9) 发起与 Hypervisor 的通信

**自动退出 (AE) 处理的退出码:**
- CPUID (0x72) — 硬件直接处理或将结果写入 GHCB
- RDTSC (0x6E) — 返回 TSC 值
- RDTSCP (0x87) — 返回 TSC 值和 TSC 辅助信息
- RDPMC (0x6F) — 返回性能计数器值
- INVD (0x76) — 缓存失效 (无需 Hypervisor 参与)
- WBINVD (0x89) — 缓存写回并失效
- #VC 异常 (0x403) — VMGEXIT 退出码

**VMSA 中 SEV-ES 自动退出字段:**

| 相对偏移 | 字段 | 描述 |
|:--------:|:----|------|
| 0x390 | GUEST_EXITINFO1 | 自动退出信息 1 |
| 0x398 | GUEST_EXITINFO2 | 自动退出信息 2 |
| 0x3A0 | GUEST_EXITINTINFO | 自动退出中断信息 |
| 0x3A8 | GUEST_NRIP | 自动退出的下一条 RIP |
| 0x3C0 | GUEST_EXITCODE | 自动退出码 |

### 32.11 SEV-SNP 详细机制

SEV-SNP (Secure Nested Paging) 是 SEV 系列中最完整的保护方案，增加了内存完整性保护。

#### 32.11.1 RMP (Reverse Map Table) 结构

RMP 是一个新的硬件表，记录每个物理页面的所有权和状态。每个 4KB 物理页对应一个 **16 字节** 的 RMP 表项。

**RMP 表项格式 (16 字节 = 128 位):**

```
位 127     96 95           64 63  62 61  60  59          52  51       32 31         0
+-----------+---------------+------+------+------+----------+-----------+------------+
| Reserved  | GPA [51:12]   | Rsvd |VMSA | Asid |Immutable | Validated | Assigned   |
|           |               |      |     |[9:0] |          |           |            |
+-----------+---------------+------+------+------+----------+-----------+------------+
```

| 位域 | 大小 | 名称 | 描述 |
|:----:|:----:|:----|------|
| **0** | 1 | Assigned | 0=主机拥有, 1=客户机拥有 (Private) |
| **1** | 1 | Validated | 1=客户机已通过 PVALIDATE 确认所有权 |
| **2** | 1 | Immutable | 1=RMP 表项不可更改 (固件使用) |
| **3** | 1 | VMSA | 1=此页面是 VMSA 页面 |
| **4** | 1 | PageSize | 0=4KB, 1=2MB/1GB 映射的一部分 |
| **11:5** | 7 | Reserved | — |
| **21:12** | 10 | ASID | 拥有此页面的客户机 ASID |
| **51:22** | 30 | Reserved (GPA) | 客户物理地址的高 30 位 |
| **63:52** | 12 | Reserved | — |
| **127:64** | 64 | Reserved | — |

#### 32.11.2 RMP 页面状态

| 状态 | Assigned | Validated | Immutable | 描述 |
|:----:|:--------:|:---------:|:---------:|------|
| **Hypervisor** | 0 | 0 | 0 | 默认状态, 主机拥有 |
| **Guest-Invalid** | 1 | 0 | 0 | 分配给客户机但未验证 |
| **Guest-Valid** | 1 | 1 | 0 | 客户机已验证, 可用作私有内存 |
| **Pre-Guest** | 1 | — | 1 | 启动过程中, 不可更改 |
| **Pre-Swap Firmware** | 1 | — | 1 | 换出到磁盘时的页面 |
| **Metadata** | 1 | — | 1 | 元数据页面 (AES-GCM 认证标签) |
| **Context** | 1 | — | 1 | 每客户机上下文数据页面 |

#### 32.11.3 PVALIDATE 指令

- **操作码:** `F2 0F 01 FF`
- **功能:** 由客户机执行，验证 RMP 中的页面所有权
- **输入:** RAX = GPA, ECX = 页面大小 (0=4KB, 1=2MB)
- **结果:** RFLAGS.CF 指示成功/失败

**PVALIDATE 错误码 (RFLAGS.CF=1 时):**
- CF=1 且 RAX[31:0]=0: 页面已验证 (Page already validated)
- CF=1 且 RAX[31:0]=1: 页面大小不匹配
- CF=1 且 RAX[31:0]=2: RMP 权限违规
- CF=1 且 RAX[31:0]=3: RMP 更新失败

**使用流程:**
```c
// 客户机将共享页面转换为私有页面:
// 1. Hypervisor 通过 RMPUPDATE 将页面设置为 Guest-Invalid (Assigned=1, Validated=0)
// 2. 客户机执行 PVALIDATE 设置 Validated=1

// 客户机将私有页面转换为共享页面:
// 1. 客户机执行 PVALIDATE 清除 Validated=0 (变为 Guest-Invalid)
// 2. Hypervisor 通过 RMPUPDATE 将页面设置回 Hypervisor (Assigned=0)
```

#### 32.11.4 RMPADJUST 指令

- **操作码:** `F3 0F 01 FE`
- **功能:** 调整 RMP 中的 VMPL 权限
- **输入:** RAX = GPA, ECX = (VMPL_TARGET << 8) | VMPL_CUR, RDX = 权限掩码
- **权限:** 更高级别 VMPL 可以向更低级别 VMPL 授予权限

**RMPADJUST 错误码:**
- CF=1 且 RAX[31:0]=0: 成功 (权限已更新)
- CF=1 且 RAX[31:0]=1: VMPL 转换不允许
- CF=1 且 RAX[31:0]=2: 页面大小不匹配
- CF=1 且 RAX[31:0]=3: 页面未验证

#### 32.11.5 RMPUPDATE 指令

- **操作码:** `F2 0F 01 D9`
- **功能:** 由 Hypervisor 执行，更新 RMP 表项
- **输入:** RAX = GPA, RDX = 指向 16 字节新 RMP 表项的地址

> **注意:** RMPUPDATE **不清除** Validated 位 — 只有客户机通过 PVALIDATE 才能更改该位。这使得 Hypervisor 即使拥有所有者也受到限制。

#### 32.11.6 SNP 启动流程与策略

**SNP 策略结构 (8 字节):**

| 位域 | 大小 | 名称 | 描述 |
|:----:|:----:|:----|------|
| **0** | 1 | DEBUG | 0=禁止调试, 1=允许调试 |
| **1** | 1 | SMT | 0=禁止 SMT, 1=允许 SMT |
| **2** | 1 | MIGRATE_MA | 0=禁止迁移代理, 1=允许迁移代理 |
| **3** | 1 | SINGLE_SOCKET | 0=允许多插槽, 1=仅单插槽 |
| **4** | 1 | DEBUG_CPU | 0=禁止 CPU 调试, 1=允许 |
| **5** | 1 | CXL | 0=禁止 CXL 内存, 1=允许 |
| **6** | 1 | AES_256 | 0=允许 AES-128 或 AES-256, 1=必须 AES-256 |
| **7** | 1 | RAPL_DIS | 0=允许 RAPL, 1=禁止 RAPL |
| **15:8** | 8 | ABI_MAJOR | 最低 ABI 主版本 |
| **31:16** | 16 | ABI_MINOR | 最低 ABI 次版本 |
| **63:32** | 32 | 保留 | MBZ |

**SNP LAUNCH_START 差异:**
- 使用 `SNP_LAUNCH_START` (0x063) 代替 `LAUNCH_START`
- 包含策略字段 (8 字节)
- 包含启动类型标识

#### 32.11.7 SNP CPUID 表

SEV-SNP 使用 CPUID 表机制限制客户机看到的 CPUID 值。Hypervisor 在 `SNP_LAUNCH_UPDATE` 期间提供 CPUID 表，SP 验证后固化。

**CPUID 表项格式 (16 字节):**

| 偏移 | 大小 | 字段 | 描述 |
|:----:|:----:|:----|------|
| 0x00 | 4B | EAX_IN | CPUID 输入值 (EAX) |
| 0x04 | 4B | ECX_IN | CPUID 子叶子 (ECX) |
| 0x08 | 4B | XFEATURE_IN | XCR0/XSS 掩码 |
| 0x0C | 4B | Reserved | 保留 |
| 0x10 | 4B | EAX_OUT | CPUID 输出 EAX |
| 0x14 | 4B | EBX_OUT | CPUID 输出 EBX |
| 0x18 | 4B | ECX_OUT | CPUID 输出 ECX |
| 0x1C | 4B | EDX_OUT | CPUID 输出 EDX |

CPUID 表限制:
- 客户机执行 CPUID 时，SP 将输入与表匹配
- 如果找到匹配项，返回表中预定义的值
- 如果未找到匹配项，触发 #VC 异常由 Hypervisor 处理
- 这防止了 Hypervisor 在运行时篡改 CPUID 结果

---

## 33. #VC 异常处理详解 (SEV-ES)

### 33.1 #VC 异常概述

#VC (VMM Communication) 异常是 SEV-ES (和 SEV-SNP) 中引入的新异常类型，用于客户机在寄存器状态加密时与 Hypervisor 通信。

- **异常向量:** 29 (0x1D)
- **名称:** #VC — VMM Communication Exception
- **错误码:** 无 (与 #DB 类似，不推送错误码)
- **触发条件:** SEV-ES/SEV-SNP 客户机执行某些特权指令或发生拦截事件时
- **IDT 条目:** 必须由客户机操作系统在启动时安装

### 33.2 #VC 触发条件

在 SEV-ES/SEV-SNP 模式下，以下操作会触发 #VC 异常（而不是直接 #VMEXIT）:

| 操作 | 描述 |
|:----|------|
| CPUID 指令 | CPUID 拦截且非自动退出 |
| RDMSR/WRMSR | 访问被拦截的 MSR |
| IN/OUT | I/O 端口访问 |
| RDTSC/RDTSCP | TSC 读取 (如果非 AE) |
| WBINVD | 缓存写回 (如果非 AE) |
| INVD | 缓存失效 (如果非 AE) |
| MONITOR/MWAIT | 监控器/等待指令 |
| VMMCALL | 客户-Hypervisor 调用 |
| DR7 读/写 | 调试寄存器访问 |
| IOIO 拦截 | I/O 指令拦截 |
| CR 读/写 | 控制寄存器访问 |
| NPF (MMIO) | 嵌套页故障导致 MMIO |
| XSETBV | XCR 写入 |
| RDPRU | RDPRU 指令 |
| INVLPGB | 广播 TLB 失效 (SEV-SNP) |
| INVPCID | INVPCID 指令 |
| TLBSYNC | TLB 同步 |
| SKINIT | 安全启动指令 |

### 33.3 #VC 处理流程

```
客户机                        Hypervisor
  |                               |
  | 1. 客户机执行 CPUID            |
  | 2. 触发 #VC (向量 29)         |
  | 3. #VC handler 入口           |
  | 4. 保存客户机上下文            |
  | 5. 解码指令 (如 CPUID)        |
  | 6. 填充 GHCB 字段:            |
  |    - SW_EXITCODE = 0x72       |
  |    - RAX/RBX/RCX/RDX → GHCB   |
  | 7. 执行 VMGEXIT               |
  |  ──────────────────────────→  | 8. 读取 GHCB 字段
  |                               | 9. 处理请求
  |  ←──────────────────────────  | 10. 填充 GHCB 结果
  | 11. 检查 SW_EXITINFO1         | 11. VMRUN 恢复
  | 12. 从 GHCB 复制结果          |
  | 13. 推进 RIP                  |
  | 14. #VC 返回                  |
  | 15. 客户机继续执行             |
```

### 33.4 #VC Handler 设计

以下是 #VC 处理程序的框架代码 (客户机侧):

```c
// #VC 异常处理入口 (客户机侧)
void __attribute__((interrupt)) vc_handler(struct exc_frame *frame)
{
    uint64_t ghcb_pa = rdmsr(MSR_SEV_ES_GHCB);  // 0xC0010130
    struct ghcb *ghcb = phys_to_virt(ghcb_pa);
    uint64_t exit_code;
    
    // 1. 保存 guest 寄存器状态到 GHCB
    ghcb->save.rax = frame->rax;
    ghcb->save.rbx = frame->rbx;
    ghcb->save.rcx = frame->rcx;
    ghcb->save.rdx = frame->rdx;
    ghcb->save.r8  = frame->r8;
    // ... 保存其他寄存器
    
    // 2. 设置 VALID_BITMAP (标记哪些寄存器有效)
    ghcb->valid_bitmap = 
        GHCB_BITMAP_RAX | GHCB_BITMAP_RBX | 
        GHCB_BITMAP_RCX | GHCB_BITMAP_RDX;
    
    // 3. 设置 SW_EXITCODE (如 CPUID = 0x72)
    exit_code = decode_exitcode_from_instruction(frame->rip);
    ghcb->save.sw_exit_code = exit_code;
    
    // 4. 执行 VMGEXIT
    vmgexit(ghcb_pa);
    
    // 5. 检查返回状态
    if (ghcb->save.sw_exit_info_1 & 0xFFFFFFFF) {
        // 错误处理
        if ((ghcb->save.sw_exit_info_1 & 0xFFFFFFFF) == 1) {
            // Hypervisor 请求注入异常
            inject_exception_from_ghcb(ghcb);
        }
        terminate_guest();
    }
    
    // 6. 从 GHCB 复制结果到寄存器
    frame->rax = ghcb->save.rax;
    frame->rbx = ghcb->save.rbx;
    // ...
    
    // 7. 推进 RIP
    frame->rip = ghcb->save.sw_scratch;  // 或从指令解码
    
    // 8. 清除 GHCB 字段
    ghcb->save.sw_exit_code = 0;
}
```

### 33.5 #VC Handler 状态保存要求

#VC 处理程序必须保存和恢复以下状态:

| 状态 | 保存位置 | 说明 |
|:----|:---------|:------|
| 通用寄存器 | GHCB 保存区域 | RAX-R15 |
| RFLAGS | 栈/内存 | 由异常框架保存 |
| RIP | 栈/内存 | 由异常框架保存 |
| CR2 | VMSA 字段 | 如果涉及页故障 |
| XCR0 | 不保存 | 保留现有值 |
| GHCB 地址 | MSR_SEV_ES_GHCB | 必须在处理后清除 |

> **重要:** #VC 处理程序本身不能再触发 #VC — 必须使用 GHCB MSR 协议或确保使用的指令不触发拦截。

### 33.6 Nested #VC 处理

当在 #VC 处理程序执行过程中再次触发 #VC 时，称为嵌套 #VC。

**嵌套 #VC 策略:**
1. **2 级 #VC:** 如果 #VC 处理程序需要执行一个也会触发 #VC 的操作 (如 RDMSR), 使用 GHCB MSR 协议 (MSR 0xC0010130) 代替完整 VMGEXIT
2. **防止无限递归:** 设置 #VC 嵌套计数器，超过限制后执行紧急处理
3. **紧急退出:** 如果无法处理嵌套 #VC，客户机应执行 TERM_REQUEST 终止

**GHCB MSR 协议 (无嵌套风险):**
```c
// 使用 GHCB MSR 协议执行简单请求 (不会触发 #VC)
uint64_t ghcb_msr_val = (function_code & 0xFFF) | (data << 12);
wrmsr(MSR_SEV_ES_GHCB, ghcb_msr_val);
vmgexit();  // 直接执行 VMGEXIT

// 读取响应
uint64_t response = rdmsr(MSR_SEV_ES_GHCB);
if ((response & 0xFFF) == expected_response) {
    // 提取数据
    result = response >> 12;
}
```

---

## 34. VMSA 结构详解

### 34.1 VMSA 概述

VMSA (VM Save Area) 是 SEV-ES/SEV-SNP 使用的加密客户机状态保存区。

- **大小:** 4096 字节 (4KB, 恰好一页)
- **对齐:** 4KB 对齐
- **加密:** 使用客户机唯一 AES 密钥加密 (内存控制器自动处理)
- **RMP 要求:** RMP 中的 VMSA 位必须置 1
- **布局:** 与 VMCB 保存区域布局基本兼容 (但有额外字段)

### 34.2 VMSA 完整布局 (相对偏移)

| 相对偏移 | 大小 | 字段 | 描述 |
|:--------:|:----:|:----|------|
| **0x000** | 128B | ES, CS, SS, DS, FS, GS | 段寄存器 (每段 16 字节) |
| **0x080** | 64B | GDTR, LDTR, IDTR, TR | 描述符表 (每个 16 字节) |
| **0x0C0** | 8B | (保留) | |
| **0x0C8** | 6B | VMPL0_SSP, VMPL1_SSP, VMPL2_SSP, VMPL3_SSP | 影子栈指针 |
| **0x0D8** | 1B | VMPL | 当前 VMPL 级别 |
| **0x0D9** | 1B | CPL | 当前权限级 |
| **0x0DC** | 4B | (保留) | |
| **0x0E0** | 8B | EFER | 扩展功能启用寄存器 |
| **0x0E8** | 8B | (保留) | |
| **0x0F0** | 8B | XSS | 扩展监管者状态掩码 |
| **0x0F8** | 8B | (保留) | |
| **0x100** | 8B | CR4 | 控制寄存器 4 |
| **0x108** | 8B | CR3 | 控制寄存器 3 |
| **0x110** | 8B | CR0 | 控制寄存器 0 |
| **0x118** | 8B | DR7 | 调试寄存器 7 |
| **0x120** | 8B | DR6 | 调试寄存器 6 |
| **0x128** | 8B | RFLAGS | 标志寄存器 |
| **0x130** | 8B | RIP | 指令指针 |
| **0x138** | 16B | (保留) | |
| **0x148** | 8B | (保留) | |
| **0x150** | 8B | RSP | 栈指针 |
| **0x158** | 8B | S_CET | 监管者 CET |
| **0x160** | 8B | SSP | 影子栈指针 |
| **0x168** | 8B | ISST_ADDR | 中断影子栈表地址 |
| **0x170** | 8B | RAX | 通用寄存器 RAX |
| **0x178** | 8B | STAR | STAR MSR |
| **0x180** | 8B | LSTAR | LSTAR MSR |
| **0x188** | 8B | CSTAR | CSTAR MSR |
| **0x190** | 8B | SFMASK | SF_MASK MSR |
| **0x198** | 8B | KERNEL_GS_BASE | KernelGSBase MSR |
| **0x1A0** | 8B | SYSENTER_CS | SYSENTER_CS MSR |
| **0x1A8** | 8B | SYSENTER_ESP | SYSENTER_ESP MSR |
| **0x1B0** | 8B | SYSENTER_EIP | SYSENTER_EIP MSR |
| **0x1B8** | 8B | CR2 | 控制寄存器 2 |
| **0x1C0** | 8B | (保留) | |
| **0x1C8** | 8B | G_PAT | PAT MSR |
| **0x1D0** | 8B | DBGCTL | 调试控制 MSR |
| **0x1D8** | 8B | BR_FROM | LastBranchFromIP |
| **0x1E0** | 8B | BR_TO | LastBranchToIP |
| **0x1E8** | 8B | LASTEXCPFROM | LastIntFromIP |
| **0x1F0** | 8B | LASTEXCPTO | LastIntToIP |
| **0x1F8** | 8B | DBGEXTNCTL | DebugExtnCtl MSR |
| **0x200** | 8B | (保留) | |
| **0x208** | 8B | (保留) | |
| **0x210** | 8B | PKRU | 保护键权限寄存器 |
| **0x218-0x27F** | — | (保留) | |
| **0x280** | 8B | SPEC_CTRL | 预测控制 MSR |
| **0x288-0x2EF** | — | (保留) | |
| **0x2F0** | 8B | RCX | 通用寄存器 RCX |
| **0x2F8** | 8B | RDX | 通用寄存器 RDX |
| **0x300** | 8B | RBX | 通用寄存器 RBX |
| **0x308** | 8B | (保留) — RSP 已在 0x150 | |
| **0x310** | 8B | RBP | 通用寄存器 RBP |
| **0x318** | 8B | RSI | 通用寄存器 RSI |
| **0x320** | 8B | RDI | 通用寄存器 RDI |
| **0x328** | 8B | R8 | 通用寄存器 R8 |
| **0x330** | 8B | R9 | 通用寄存器 R9 |
| **0x338** | 8B | R10 | 通用寄存器 R10 |
| **0x340** | 8B | R11 | 通用寄存器 R11 |
| **0x348** | 8B | R12 | 通用寄存器 R12 |
| **0x350** | 8B | R13 | 通用寄存器 R13 |
| **0x358** | 8B | R14 | 通用寄存器 R14 |
| **0x360** | 8B | R15 | 通用寄存器 R15 |
| **0x368-0x37F** | — | (保留) | |
| **0x380** | 8B | GUEST_EXITINFO1 | 自动退出信息 1 (AE) |
| **0x388** | 8B | GUEST_EXITINFO2 | 自动退出信息 2 (AE) |
| **0x390** | 8B | GUEST_EXITINTINFO | 自动退出中断信息 (AE) |
| **0x398** | 8B | GUEST_NRIP | 自动退出 Next RIP (AE) |
| **0x3A0** | 8B | SEV_FEATURES | SEV 特性选择 |
| **0x3A8** | 8B | VINTR_CTRL | 客户控制的中断注入 |
| **0x3B0** | 8B | GUEST_EXITCODE | 自动退出码 (AE) |
| **0x3B8** | 8B | VIRTUAL_TOM | 虚拟内存顶 (vTOM) |
| **0x3C0** | 8B | TLB_ID | TLB 标识符 |
| **0x3C8** | 8B | PCPU_ID | 物理 CPU ID |
| **0x3D0** | 8B | EVENTINJ | 事件注入 |
| **0x3D8** | 8B | XCR0 | 扩展控制寄存器 0 |
| **0x3E0-0x3FF** | — | (保留) | |
| **0x400** | — | FPU/XMM/YMM 状态 | x87, XMM0-15, YMM_HI0-15 |
| **0x700-0xFFF** | — | 保留/未使用 | 填充到 4KB |

### 34.3 VMSA vs VMCB 保存区域差异

| 差异点 | VMCB 保存区域 | VMSA (SEV-ES) |
|:-------|:-------------|:---------------|
| 加密 | 未加密 | AES 加密 |
| 通用寄存器布局 | RAX 在 0x1F8, 其他从 0x308 开始 | RAX 在 0x170, RCX-R15 在 0x2F0-0x360 |
| XSS 字段 | 无 | 有 (0x0F0) |
| PKRU | 无 | 有 (0x210) |
| TLB_ID / PCPU_ID | 无 | 有 (0x3C0/0x3C8) |
| SEV_FEATURES | 无 | 有 (0x3A0) |
| RSP 位置 | 0x1D8 | 0x150 |
| GUEST_EXITCODE | 控制区域 | 保存区域 0x3B0 |

### 34.4 SEV_FEATURES 字段 (偏移 0x3A0)

| 位 | 名称 | 描述 |
|:--:|:----|------|
| **0** | SNPActive | SEV-SNP 激活 |
| **1** | vTOM | 虚拟 Top Of Memory 启用 |
| **2** | ReflectVC | #VC 反射到 VMPL0 (而非当前 VMPL) |
| **3** | RestrictedInjection | 限制 Hypervisor 注入中断 |
| **8** | VmplSSS | VMPL 影子栈支持 |
| **14** | VmsaRegProt | VMSA 寄存器保护 |
| **15** | SmtProtection | SMT 保护 |

### 34.5 VINTR_CTRL 字段 (偏移 0x3A8)

| 位 | 名称 | 描述 |
|:--:|:----|------|
| **0** | V_TPR | 虚拟 Task Priority Register |
| **1** | V_IRQ | 虚拟中断挂起 |
| **2** | V_GIF | 虚拟全局中断标志 |
| **7:3** | V_INTR_PRIO | 虚拟中断优先级 |
| **15:8** | V_INTR_VECTOR | 虚拟中断向量 |

### 34.6 AP VMSA (辅助 CPU)

在 SEV-ES/SEV-SNP 多处理器系统中，每个 AP (Application Processor) 也需要自己的 VMSA。AP VMSA 的创建通过 GHCB NAE 事件完成:

**AP 启动流程:**
1. BSP 通过 GHCB 请求 AP 创建 (`SNP_AP_CREATION` = 0x8000_0013)
2. Hypervisor 分配并初始化 AP 的 VMSA 页面
3. AP 执行 SIPI 序列后开始执行其 VMSA 中指定的 RIP
4. AP VMSA 也使用客户机唯一密钥加密

### 34.7 VMSA 必须为零的字段 (SEV-ES)

为保证 VMRUN 成功，SEV-ES VMSA 中的以下字段必须为零:

| 偏移 | 字段 | 说明 |
|:----:|:----|:------|
| 0x0C0-0x0C7 | 保留 | 必须为零 |
| 0x138-0x147 | 保留 (16 字节) | 必须为零 |
| 0x200-0x207 | 保留 | 必须为零 |
| 0x218-0x27F | 保留 | 必须为零 |
| 0x288-0x2EF | 保留 | 必须为零 |
| 0x368-0x37F | 保留 | 必须为零 |
| 0x3E0-0x3FF | 保留 | 必须为零 |

---

## 35. GHCB 通信协议详解

### 35.1 GHCB 概述

GHCB (Guest-Hypervisor Communication Block) 是 SEV-ES/SEV-SNP 客户机与 Hypervisor 之间的共享通信页面。

- **位置:** 客户机物理地址空间中的一页 (不加密, Shared)
- **大小:** 4096 字节 (4KB)
- **协议:** 定义在 AMD 文档 #56421 中
- **版本:** v1 (SEV-ES) 和 v2 (SEV-SNP 扩展)

### 35.2 GHCB MSR (0xC0010130) 格式

**地址:** `0xC0010130` — MSR_SEV_ES_GHCB

**GHCB MSR 位布局:**

| 位域 | 大小 | 描述 |
|:----:|:----:|------|
| **11:0** | 12 | 协议功能码 (Function Code) |
| **31:12** | 20 | 请求/响应数据 |
| **63:32** | 32 | 扩展数据 (用于 CPUID 协议等) |

**协议功能码:**

| 功能码 | 名称 | 方向 | 描述 |
|:-----:|:----|:----:|------|
| **0x000** | — | — | 空闲 (未使用) |
| **0x001** | SEV_INFORMATION_RESPONSE | SP→Guest | 平台信息响应 |
| **0x002** | SEV_INFORMATION_REQUEST | Guest→SP | 请求平台信息 |
| **0x003** | CPUID_RESPONSE | SP→Guest | CPUID 结果响应 |
| **0x004** | CPUID_REQUEST | Guest→SP | 请求 CPUID 值 |
| **0x005** | CPUID_RESPONSE_V2 | SP→Guest | CPUID 结果 v2 |
| **0x006** | CPUID_REQUEST_V2 | Guest→SP | 请求 CPUID v2 |
| **0x007** | GHCB_GPA_REGISTER | Guest→HV | 注册 GHCB 的 GPA |
| **0x008** | GHCB_GPA_RESPONSE | HV→Guest | GHCB GPA 确认 |
| **0x009** | GHCB_CACHE_INVALIDATE | Guest→HV | 请求 GHCB 缓存失效 |
| **0x00A** | GHCB_CACHE_INVALIDATE_RESPONSE | HV→Guest | 确认失效 |
| **0x012** | AP_RESET_HOLD | Guest→HV | AP 等待重置 |
| **0x013** | AP_RESET_HOLD_ACK | HV→Guest | AP 重置确认 |
| **0x015** | AP_RESET_HOLD_NMI | Guest→HV | NMI 到达时的 AP 重置 |
| **0x080** | HV_FEATURES_REQUEST | Guest→HV | 请求 Hypervisor 特性 |
| **0x081** | HV_FEATURES_RESPONSE | HV→Guest | Hypervisor 特性回应 |
| **0x100** | SNP_AP_CREATION | Guest→HV | SNP AP 创建请求 |
| **0x102** | SNP_AP_CREATION_RESPONSE | HV→Guest | SNP AP 创建响应 |
| **0x14x** | PSC_REQUEST | Guest→HV | 页面状态变更请求 |
| **0x15x** | PSC_RESPONSE | HV→Guest | 页面状态变更响应 |

### 35.3 GHCB 共享页面格式

GHCB 页面布局 (4096 字节):

| 偏移 | 大小 | 字段 | 描述 |
|:----:|:----:|:----|------|
| **0x000** | 8B | SW_EXITCODE | NAE 事件码 (Guest→HV) |
| **0x008** | 8B | SW_EXITINFO1 | NAE 信息 1 (Guest→HV) |
| **0x010** | 8B | SW_EXITINFO2 | NAE 信息 2 (Guest→HV) |
| **0x018** | 8B | SW_SCRATCH | 暂存字段 (Guest→HV) |
| **0x020** | 8B | SW_SCRATCH_PAD | 对齐填充 |
| **0x028** | 8B | SW_CALC_CFG | 计算配置 (SNP) |
| **0x030-0x037** | 8B | (保留) | |
| **0x038-0x08F** | 88B | XSAVE 头 | XSAVE 区域头 |
| **0x090-0x28F** | 512B | XSAVE 区域 | XSAVE 保存区域 |
| **0x290-0x37F** | — | (保留) | |
| **0x380** | 8B | CR0 | 客户 CR0 |
| **0x388** | 8B | CR1 | 客户 CR1 |
| **0x390** | 8B | CR2 | 客户 CR2 |
| **0x398** | 8B | CR3 | 客户 CR3 |
| **0x3A0** | 8B | CR4 | 客户 CR4 |
| **0x3A8-0x3BF** | — | (保留) | |
| **0x3C0** | 8B | RAX | 客户 RAX |
| **0x3C8** | 8B | RBX | 客户 RBX |
| **0x3D0** | 8B | RCX | 客户 RCX |
| **0x3D8** | 8B | RDX | 客户 RDX |
| **0x3E0** | 8B | R8 | 客户 R8 |
| **0x3E8** | 8B | R9 | 客户 R9 |
| **0x3F0** | 8B | R10 | 客户 R10 |
| **0x3F8** | 8B | R11 | 客户 R11 |
| **0x400** | 8B | R12 | 客户 R12 |
| **0x408** | 8B | R13 | 客户 R13 |
| **0x410** | 8B | R14 | 客户 R14 |
| **0x418** | 8B | R15 | 客户 R15 |
| **0x420-0x7EF** | — | (保留) | |
| **0x7F0** | 2B | VALID_BITMAP | 有效位图 (高 16 位) |
| **0x7F2** | 6B | (保留) | |
| **0x7F8** | 2B | GHCB_USAGE | GHCB 使用掩码 |
| **0x7FA** | 2B | GHCB_HV_FEATURES | Hypervisor 特性供客户机使用 |
| **0x7FC** | 2B | GHCB_PROTO_VER | GHCB 协议版本 |
| **0x7FE** | 2B | GHCB_PROTO_MIN | GHCB 最小协议版本 |
| **0x800-0xFFF** | 2KB | GHCB Shared Buffer | 共享缓冲区 (用于 SNPGuestRequest 等) |

### 35.4 NAE 事件编码

SW_EXITCODE 字段定义了 NAE (Non-Automatic Exit) 事件码:

| SW_EXITCODE | 名称 | 描述 |
|:----------:|:----|------|
| **0x000-0xFF** | — | 标准 #VMEXIT 退出码 (同 VMCB EXITCODE) |
| **0x8000_0010** | Page State Change | 页面状态变更 (SEV-SNP) |
| **0x8000_0011** | SNP Guest Request | SNP 客户机请求 (如 Attestation) |
| **0x8000_0012** | SNP Extended Guest Request | SNP 扩展客户机请求 |
| **0x8000_0013** | SNP AP Creation | SNP AP 创建 |
| **0x8000_0014** | HV Doorbell Page | Hypervisor 门铃页通知 |
| **0x8000_0015** | HV IPI | Hypervisor 虚拟 IPI |
| **0x8000_0016** | HV Timer | Hypervisor 定时器 |
| **0x8000_FFFD** | Hypervisor Features | Hypervisor 特性发现 |
| **0x8000_FFFE** | Terminate Request | 终止请求 |
| **0x8000_FFFF** | Unsupported Event | 不支持的事件 |

### 35.5 GHCB 协议 v1 vs v2 区别

| 区别 | v1 (SEV-ES) | v2 (SEV-SNP) |
|:-----|:-----------|:-------------|
| 页面状态变更 | 不支持 | 支持 (PSC 协议) |
| SNP 客户机请求 | 不支持 | 支持 (0x8000_0011) |
| AP 创建 | 通过 MSR 协议 | 通过 GHCB NAE |
| Hypervisor 特性 | 有限 | 扩展 (0x8000_FFFD) |
| GHCB_USAGE 字段 | 无 | 有 (0x7F8) |
| HV_FEATURES 字段 | 无 | 有 (0x7FA) |
| 共享缓冲区 | 无 | 有 (0x800-0xFFF) |
| VALID_BITMAP | 简单 | 扩展 |

### 35.6 GHCB 寄存器使用惯例

**VMGEXIT 时寄存器约定:**

| 寄存器 | 用途 |
|:------|:----|
| **RAX** | GHCB 物理地址 (低 12 位包含协议信息) |
| **RBX-R15** | 可通过 GHCB 传递的数据 (保存到 GHCB 保存区域) |

**Hypervisor 返回时:**

| 寄存器 | 用途 |
|:------|:----|
| **RAX** | 通常恢复为客户值 |
| **SW_EXITINFO1** | 位 [31:0] = 0 表示成功, 非零表示错误或异常注入请求 |
| **SW_EXITINFO2** | 如果 SW_EXITINFO1[31:0] == 1, 包含 EVENTINJ 值 |

### 35.7 GHCB 终止原因码

当客户机通过 `TERM_REQUEST` (0x8000_FFFE) 请求终止时的原因码:

| SW_EXITINFO2 位域 | 描述 |
|:-----------------|:-----|
| **0** | Guest requested termination |
| **1-31** | 终止原因码 |
| **32-63** | 厂商/平台特定数据 |

常见终止原因:

| 原因码 | 描述 |
|:-----:|:------|
| 0x0 | 一般性错误 |
| 0x1 | GHCB 协议版本不匹配 |
| 0x2 | Hypervisor 不支持所需功能 |
| 0x3 | 固件引导失败 |
| 0x4 | 第 2 级 #VC 无法处理 |
| 0x5 | 无效的 VMGEXIT 请求 |

### 35.8 Hypervisor 特性发现

客户机通过 `HV_FEATURES_REQUEST` (0x8000_FFFD) NAE 查询 Hypervisor 特性:

```c
// 客户机请求 Hypervisor 特性:
ghcb->save.sw_exit_code = 0x8000_FFFD;
ghcb->save.sw_exit_info_1 = 0;  // 请求所有特性
vmgexit();

// Hypervisor 返回:
// SW_EXITINFO1[31:0] = 0 (成功)
// SW_EXITINFO2 = 特性位图:
#define HV_FEAT_SNP                          (1ULL << 0)  // SNP 支持
#define HV_FEAT_SNP_AP_CREATION              (1ULL << 1)  // SNP AP 创建
#define HV_FEAT_RESTRICTED_INJECTION         (1ULL << 2)  // 限制注入
#define HV_FEAT_RESTRICTED_INJECTION_TIMER   (1ULL << 3)  // 限制注入定时器
#define HV_FEAT_APIC_ID_LIST                 (1ULL << 4)  // APIC ID 列表
#define HV_FEAT_MULTI_VMPL                   (1ULL << 5)  // 多 VMPL 支持
#define HV_FEAT_SEV_ES_PAGE_STATE_CHANGE     (1ULL << 6)  // SEV-ES 页面状态变更
```

---

## 36. AMD 硬件勘误与 Workaround

### 36.1 Erratum #383 — TLB 多匹配导致机器检查

- **受影响:** Family 10h (Barcelona) 及部分早期 Family 15h
- **症状:** 当 TLB 中存在多个匹配项时，可能导致机器检查异常 (MCE) 和客户状态损坏
- **检测:** `X86_BUG_AMD_TLB_MMATCH` 标志
- **Workaround:**
  - 写入 `MSR_AMD64_DC_CFG` (0xC0011022) 的位 47 启用硬件缓解
  - 在 MCE 拦截处理中检查 `MSR_IA32_MC0_STATUS` 值 `0xB600000000010015`
  - 清除 MCi_STATUS 寄存器 (bank 0-5)
  - 执行全局 TLB 刷新 (`__flush_tlb_all()`)
  - 触发三重故障终止客户机

### 36.2 Erratum #298 — OSVW 保留位

- **受影响:** Family 10h
- **症状:** OSVW (OS-Visible Workaround) 状态寄存器中的保留位可能被错误设置
- **Workaround:** 保守假设 errata 存在，当 OSVW 长度为 0 且 Family 为 10h 时

### 36.3 Erratum #400 — HLT/IO 拦截与 C1E

- **受影响:** Family 10h-15h 部分处理器
- **症状:** HLT 和 IO 指令拦截可能与 C1E 增强暂停状态交互导致问题
- **检测:** `X86_BUG_AMD_APIC_C1E`, `X86_BUG_AMD_E400`
- **Workaround:** 在 OSVW 位图中标记为固定 (假设 HLT 和 IO 被拦截)

### 36.4 Erratum #1218 — GMET NPF 用户/监管者位错误

- **受影响:** Zen 3 (Family 19h) 部分 stepping
- **症状:** 当 GMET (Guest Mode Execute Trap) 启用时，NPF 的 EXITINFO1[2] (U/S 位) 可能被错误设置
- **Workaround (KVM):** 在 `npf_interception()` 中重新推导 U/S 位:
  ```c
  if (erratum_1218) {
      exit_info_1 |= PFERR_USER_MASK;  // 强制设置为用户
      if (cpl == 0)
          exit_info_1 &= ~PFERR_USER_MASK;  // 如果 CPL=0 则改为监管者
  }
  ```

### 36.5 Erratum #1235 — AVIC/x2AVIC IPI 虚拟化问题

- **受影响:** Zen 3/Zen 4 (Family 19h) 部分 stepping
- **症状:** AVIC IPI 虚拟化可能导致不正确的 IPI 传递或中断丢失
- **Workaround (KVM):** 在受影响的 CPU 上禁用 AVIC IPI 虚拟化 (仅使用 AVIC 门铃机制，不使用 IPI 虚拟化)

### 36.6 Family 10h (Barcelona) 已知 SVM 勘误

| 勘误 | 描述 | Workaround |
|:----|------|:-----------|
| #170 | #VMEXIT 时 DR6 未正确保存 | 在退出处理中手动保存/恢复 DR6 |
| #176 | VMRUN 后 MSR 权限表状态不一致 | VMRUN 后显式重新加载 MSRPM |
| #224 | NPT 期间 A/D 位更新可能导致页故障 | 使用软件 A/D 位管理 |
| #262 | #VMEXIT 后 IF 标志状态不确定 | 在退出入口处显式设置 IF |

### 36.7 Family 15h (Bulldozer/Piledriver) 已知 SVM 勘误

| 勘误 | 描述 | Workaround |
|:----|------|:-----------|
| #720 | VMCB Clean Bits 在某些情况下不起作用 | 不使用 Clean Bits 优化或在每个 VMRUN 时清除所有 Clean Bits |
| #730 | 嵌套分页 (NPT) 下的 TLB 失效可能不完整 | 使用 `TLB_CONTROL_FLUSH_ALL` 代替 `FLUSH_ASID` |
| #746 | #VMEXIT 时 NRIP 在某些情况下不正确 | 使用手动指令解码回退路径 |
| #750 | VMLOAD/VMSAVE 在特定条件下可能损坏状态 | 避免频繁使用 VMLOAD/VMSAVE |
| #767 | AVIC 门铃可能在某些省电状态下丢失 | 禁用 AVIC 或使用非省电状态 |

### 36.8 Family 17h (Zen/Zen+/Zen2) 已知 SVM 勘误

| 勘误 | 描述 | Workaround |
|:----|------|:-----------|
| #1038 | 嵌套分页 A/D 位设置可能触发 #GP | 使用软件辅助的 A/D 位管理 |
| #1064 | VMCB Clean 位在跨核心迁移时可能导致错误 | 跨核心迁移时清除所有 Clean Bits |
| #1098 | NPT 下 TLBSYNC 可能不刷新所有 TLB 项 | 使用完整的 TLB 刷新替代 |
| #1116 | #VMEXIT 中断窗口可能延迟 | 在 VMRUN 之前检查待处理中断 |
| #1172 | SEV 加密页面在特定条件下可被主机读取 | 使用 SEV-ES 或 SEV-SNP 代替 |
| #1190 | AVIC 在特定负载下可能漏掉中断 | 启用 AVIC 回退路径 |

### 36.9 Family 19h (Zen3/Zen4/Zen5) 已知 SVM 勘误

| 勘误 | 描述 | Workaround |
|:----|------|:-----------|
| #1218 | GMET NPF U/S 位错误 (见 36.4) | 在 NPF 处理中重新推导 |
| #1235 | AVIC IPI 虚拟化问题 (见 36.5) | 禁用 AVIC IPI 虚拟化 |
| #1254 | SEV-SNP RMP 检查在特定条件下绕过 | 使用最新固件更新 |
| #1299 | 嵌套 NPT 下页面遍历可能返回错误结果 | 在 L1 Hypervisor 中验证页面遍历结果 |
| #1319 | VMRUN 在 SEV-ES 模式下可能不刷新 VMSA 缓存 | VMRUN 前执行 VMSA 缓存失效 |
| #1342 | SEV-SNP PVALIDATE 对于 2MB 页面可能错误 | 使用 4KB 页面代替 |
| #1387 | VMGEXIT 后 GHCB 缓存一致性可能丢失 | 在 VMRUN 前执行 WBINVD |
| #1412 | TSC_RATIO MSR 在特定 P-state 下可能不准确 | 固定 P-state 或使用校准 |
| #1476 | NPT 下 Accessed/Dirty 位设置存在竞争条件 | 使用软件仿真 A/D 位 |
| #1499 | SEV-SNP 迁移时加密上下文可能损坏 | 确保迁移使用的固件版本匹配 |

### 36.10 检测方法

```c
// 检测特定 CPU 的勘误
int detect_erratum_1218(void)
{
    uint32_t eax, ebx, ecx, edx;
    
    // 获取 CPU 基本信息
    cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
    uint32_t family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    uint32_t model  = ((eax >> 4) & 0xF) | ((eax >> 16) & 0xF0);
    uint32_t stepping = eax & 0xF;
    
    // Family 19h (Zen 3), 特定 model/stepping
    if (family == 0x19) {
        if (model == 0x01 && stepping < 2)
            return 1;  // 受 Erratum 1218 影响
        if (model == 0x21 && stepping == 0)
            return 1;
    }
    
    return 0;
}

// 通过 OSVW 检测
int detect_via_osvw(int erratum_num)
{
    uint64_t osvw_status = rdmsr(MSR_AMD64_OSVW_STATUS);  // 0xC0010140
    uint64_t osvw_length = rdmsr(MSR_AMD64_OSVW_ID_LENGTH); // 0xC0010141
    
    if (erratum_num < osvw_length) {
        return (osvw_status >> erratum_num) & 1;
    }
    
    // 超出已知范围 — 保守假设存在
    return 1;
}
```

---

## 37. 罕见的 #VMEXIT 场景处理

### 37.1 Shutdown (三重故障) 处理

**退出码:** `0x7F` (VMEXIT_SHUTDOWN)

当客户机触发三重故障时:

```c
void handle_shutdown(struct vmcb *vmcb)
{
    LogError("Guest triple fault at RIP=0x%llx, CS=0x%x\n",
             vmcb->save.rip, vmcb->save.cs.sel);
    
    // 对于嵌套虚拟化, 转发给 L1 Hypervisor
    if (is_nested()) {
        inject_nested_vmexit(vmcb, VMEXIT_SHUTDOWN, 0, 0);
        return;
    }
    
    // 否则停止或重启客户机
    if (should_restart_guest())
        reset_guest_to_real_mode(vmcb);
    else
        stop_guest();
}
```

### 37.2 INIT 处理

**退出码:** `0x63` (VMEXIT_INIT)

当客户机收到 INIT 信号时:
- 如果 `VM_CR.R_INIT=1`, Hypervisor 拦截所有 INIT
- SEV-SNP 客户机忽略 INIT 信号, 除非 VP 处于 HLT 状态
- 处理: 排队 INIT 或转发给 L1

### 37.3 VMRUN 期间的机器检查

当 VMRUN 执行期间发生 Machine Check:
- 检查是否为 Erratum 383 相关的假 MCE
- 清除 MCi_STATUS 并检查是否可恢复
- 对于严重 MCE, 必须停止主机

### 37.4 嵌套 #VMEXIT

当在 #VMEXIT 处理程序中发生另一次 #VMEXIT:
- 限制嵌套深度 (通常 2-3 级)
- 保存并恢复 VMCB 状态
- 过深嵌套时触发紧急处理

### 37.5 并发 NMI 与 #VMEXIT

- **GIF=0 时:** NMI 被阻塞, 直到 STGI 后传递
- **策略:** 在 #VMEXIT 入口立即执行 STGI
- **NMI 拦截:** 退出码 0x61

### 37.6 VMRUN 带 STI 阴影

确保 VMRUN 前 RFLAGS.IF=1, 避免 STI 阴影导致的问题:
```c
vmcb->save.rflags |= X86_EFLAGS_IF;
vmcb->control.v_intr_state &= ~INTR_SHADOW;
```

### 37.7 其他罕见退出码

| 退出码 | 名称 | 处理 |
|:-----:|:----|:------|
| 0x7E | FERR_FREEZE | 模拟 FERR# 信号, 通常可忽略 |
| 0x74 | IRET | 跟踪中断返回, 清除 VNMI_MASK |
| 0x7D | TASK_SWITCH | 32 位兼容模式下的任务切换 |
| 0x8F | EFER_WRITE_TRAP | 跟踪 EFER 变化但不阻止写入 |
| 0x96 | INVLPGB | 广播 TLB 失效, 跨核心协调 |
| 0x99 | TLBSYNC | 等待所有 INVLPGB 完成 |
| 0x98 | MCOMMIT | 等待异步 DRAM 刷新完成 |

---

## 38. AMD-V 嵌套虚拟化 (Nested SVM)

### 38.1 概述

嵌套虚拟化允许在虚拟机内部运行另一个 Hypervisor (L1 → L2)。

- **CPUID 支持:** `Fn8000_000A_EDX[NestedVirt]` (位 29) = 1 (Zen 4+)
- **旧版:** 通过软件仿真 (KVM/Xen)

### 38.2 VMRUN 仿真

当 L1 执行 VMRUN 时, L0 截获:

```c
void handle_nested_vmrun(struct vmcb *l1_vmcb, struct vcpu *vcpu)
{
    uint64_t vmcb12_pa = l1_vmcb->save.rax;
    struct vmcb *vmcb12 = nested_map(vcpu, vmcb12_pa);
    
    if (!validate_nested_state(vmcb12)) {
        inject_guest_exception(l1_vmcb, 13, 0);  // #GP(0)
        return;
    }
    
    // 创建影子 VMCB (VMCB02)
    struct vmcb *vmcb02 = get_shadow_vmcb(vcpu);
    merge_nested_vmcb(vmcb02, vcpu->vmcb01, vmcb12);
    
    // 保存 L1 返回点
    vcpu->nested.l1_rip = l1_vmcb->save.rip + insn_len;
    vcpu->nested.l1_rsp = l1_vmcb->save.rsp;
    vcpu->nested.vmcb12_pa = vmcb12_pa;
    vcpu->nested.in_l2 = true;
    vcpu->vmcb = vmcb02;
}
```

### 38.3 #VMEXIT 转发

```c
void handle_nested_vmexit(struct vmcb *vmcb02, struct vcpu *vcpu)
{
    struct vmcb *l1_vmcb = vcpu->vmcb01;
    struct vmcb *vmcb12 = vcpu->nested.vmcb12;
    
    // 转发退出信息到 VMCB12
    vmcb12->control.exit_code = vmcb02->control.exit_code;
    vmcb12->control.exit_info_1 = vmcb02->control.exit_info_1;
    vmcb12->control.exit_info_2 = vmcb02->control.exit_info_2;
    
    // 恢复 L2 状态到 VMCB12
    vmcb12->save.rip = vmcb02->save.rip;
    vmcb12->save.rsp = vmcb02->save.rsp;
    
    // 将 L1 RIP 设为 VMRUN 之后
    l1_vmcb->save.rip = vcpu->nested.l1_rip;
    l1_vmcb->save.rsp = vcpu->nested.l1_rsp;
    
    vcpu->vmcb = l1_vmcb;
    vcpu->nested.in_l2 = false;
}
```

### 38.4 VMCB 类型

| 类型 | 名称 | 描述 |
|:----:|:----|------|
| **VMCB01** | L0 物理 VMCB | L0 Hypervisor 使用的真实 VMCB |
| **VMCB12** | L1 虚拟 VMCB (vVMCB) | L1 看到和操作的 VMCB |
| **VMCB02** | L0 影子 VMCB (pVMCB) | L0 为 L2 创建的物理 VMCB |

### 38.5 拦截合并

L2 拦截 = L1 拦截 ∪ L0 拦截 (OR 关系):

```c
void merge_intercepts(struct vmcb *vmcb02, struct vmcb *vmcb01, struct vmcb *vmcb12)
{
    vmcb02->control.intercept_cr_read = 
        vmcb01->control.intercept_cr_read | vmcb12->control.intercept_cr_read;
    vmcb02->control.intercept_cr_write = 
        vmcb01->control.intercept_cr_write | vmcb12->control.intercept_cr_write;
    vmcb02->control.intercept_vectors_1 = 
        vmcb01->control.intercept_vectors_1 | vmcb12->control.intercept_vectors_1;
    vmcb02->control.intercept_vectors_2 = 
        (vmcb01->control.intercept_vectors_2 | vmcb12->control.intercept_vectors_2) 
        | (1 << 0);  // VMRUN 必须总是由 L0 拦截
}
```

### 38.6 嵌套 NPT

**合并 NPT (推荐):** L0 合并 L1 和 L2 的 NPT 为单一页表
- 优点: 硬件只需要一次 NPT 遍历
- 缺点: 重建开销

**影子 NPT (传统):** L2 通过 L1 的 NPT 翻译地址
- 优点: 无需重建
- 缺点: NPF 处理延迟增加

### 38.7 性能考虑

| 因素 | 影响 | 优化 |
|:----|:----|:-----|
| VMRUN 仿真 | 每个 L2 VMRUN 截获 | VMCB 缓存 |
| 拦截合并 | 每次 L2 退出需要检查转发 | 预计算合并 |
| NPT 合并 | 页面频繁变化时重建 | 惰性重建 |
| TLB 刷新 | L2 切换频繁 | ASID 分离 |
| 内存开销 | VMCB02+NPT 耗内存 | 模板缓存 |

### 38.8 安全注意事项

| CVE | 问题 | 影响 |
|:----|:-----|:-----|
| CVE-2021-3653 | int_ctl 验证缺失 → L1 可为 L2 启用 AVIC | L2 可写主机内存 |
| CVE-2021-3656 | virt_ext 验证缺失 → L1 可禁用 VMSAVE/VMLOAD 拦截 | L2 权限提升 |

---

## 39. SEV-SNP 远程证明 (Attestation)

### 39.1 证明流程

```
Guest → Hypervisor → AMD SP → 签名报告 → Guest
                      ↓
                  KDS (VCEK 证书)
                      ↓
                  验证方验证签名、策略、度量
```

### 39.2 VCEK 证书结构

VCEK 派生自芯片唯一秘密 + 当前 TCB 版本:
```
AMD Root Key (ARK) → AMD SEV CA (ASK) → VCEK → 签名证明报告
```

### 39.3 证明报告结构 (1184 字节)

| 偏移 | 字段 | 描述 |
|:----:|:----|------|
| 0x000 | VERSION | 版本 (2/3) |
| 0x008 | POLICY | 启动策略 |
| 0x010 | FAMILY_ID | 家族 ID |
| 0x020 | IMAGE_ID | 映像 ID |
| 0x080 | REPORT_DATA | 客户机提供的数据 (64B) |
| 0x0C0 | MEASUREMENT | 启动度量 (SHA-384, 48B) |
| 0x0F0 | HOST_DATA | Hypervisor 数据 (32B) |
| 0x110 | ID_KEY_DIGEST | ID 密钥摘要 (48B) |
| 0x140 | AUTHOR_KEY_DIGEST | 授权密钥摘要 (48B) |
| 0x170 | REPORT_ID | 客户机 ID (32B) |
| 0x190 | REPORT_ID_MA | 迁移代理 ID (32B) |
| 0x1B0 | REPORTED_TCB | TCB 版本 (48B) |
| 0x1E0 | CHIP_ID | 芯片 ID (32B) |
| **0x2A0** | **SIGNATURE** | ECDSA P-384 签名 |

### 39.4 ID_BLOCK / ID_AUTH

```c
struct id_block {
    uint8_t  ld[48];        // 期望的启动度量
    uint8_t  family_id[16];
    uint8_t  image_id[16];
    uint32_t version;
    uint32_t guest_svn;
    uint8_t  policy[8];
};

struct id_auth {
    uint8_t id_block_sig[96];  // ECDSA-384 签名
    uint8_t id_key[72];        // ID 公钥
};
```

### 39.5 策略执行

验证方检查:
- **DEBUG=0** — 禁止调试模式
- **SMT** — 与平台配置一致
- **MIGRATE_MA** — 迁移代理要求
- **SINGLE_SOCKET** — 平台拓扑匹配

### 39.6 迁移代理

REPORT_ID_MA 标识迁移代理, 负责:
- 安全传输客户机加密上下文
- 迁移前后提供证明报告
- 确保迁移中状态不被泄露

---

## 40. AMD-V 性能调优指南

### 40.1 VMCB Clean Bits 优化

正确的 Clean Bits 管理可减少 30-50% VMRUN 开销:

```c
void optimize_clean_bits(struct vmcb *vmcb, uint32_t modified_fields)
{
    uint32_t always_clean = VMCB_CLEAN_INTERCEPTS | VMCB_CLEAN_PERM_MAP | 
                           VMCB_CLEAN_ASID | VMCB_CLEAN_NPT | VMCB_CLEAN_CR2;
    
    if (modified_fields & MODIFIED_INTERRUPTS)
        always_clean &= ~VMCB_CLEAN_INTR;
    if (modified_fields & MODIFIED_SEGMENTS)
        always_clean &= ~VMCB_CLEAN_SEG;
    if (modified_fields & MODIFIED_CR)
        always_clean &= ~VMCB_CLEAN_CR;
    if (modified_fields & MODIFIED_DR)
        always_clean &= ~VMCB_CLEAN_DR;
    
    vmcb->control.clean_bits = always_clean;
}
```

### 40.2 ASID 分配

```c
struct asid_pool {
    uint32_t max_asid;
    uint32_t next_asid;
    uint64_t generation;
    uint64_t cpu_gen[256];
};

uint32_t asid_allocate(struct asid_pool *pool, int cpu_id)
{
    if (pool->cpu_gen[cpu_id] != pool->generation) {
        pool->cpu_asid[cpu_id] = pool->next_asid++;
        pool->cpu_gen[cpu_id] = pool->generation;
        if (pool->next_asid >= pool->max_asid) {
            pool->generation++;
            pool->next_asid = 1;
        }
    }
    return pool->cpu_asid[cpu_id];
}
```

### 40.3 NPT 页面大小选择

| 页面大小 | TLB 覆盖 | 每 GB 项数 | 场景 |
|:--------:|:--------:|:----------:|:-----|
| 4 KB | 小 | 262,144 | I/O 设备 |
| 2 MB | 中 | 512 | 通用内存 |
| 1 GB | 大 | 1 | 大内存 VM |

### 40.4 TSC_RATIO

```c
uint64_t tsc_ratio_calculate(uint64_t host_freq_khz, uint64_t guest_freq_khz)
{
    uint64_t ratio = (guest_freq_khz << 32) / host_freq_khz;
    if (ratio > TSC_RATIO_MAX) ratio = TSC_RATIO_MAX;
    if (ratio < 1) ratio = 1;
    return ratio;
}
```

### 40.5 PAUSE Filter 调优

| 参数 | 典型值 | 说明 |
|:----|:------:|:-----|
| `ple_window` | 4096 | 初始计数窗口 |
| `ple_window_max` | 32768 | 最大值 |
| `ple_window_grow` | 2 | 增长因子 |
| `ple_threshold` | 4096 | 周期阈值 |

### 40.6 AVIC 性能

| 模式 | 延迟 | 适用场景 |
|:----|:----:|:---------|
| APIC (无 AVIC) | 高 | 兼容性 |
| xAVIC (MMIO) | 中 | 通用 |
| x2AVIC (MSR) | 低 | 高性能 |

### 40.7 MSRPM 优化

```c
void msrpm_optimize(uint8_t *msrpm)
{
    msrpm_set_pass(msrpm, MSR_IA32_TSC);
    msrpm_set_pass(msrpm, MSR_IA32_SPEC_CTRL);
    msrpm_set_pass(msrpm, MSR_IA32_ARCH_CAPABILITIES);
    // 拦截: EFER, STAR, LSTAR, SYSENTER_*
}
```

### 40.8 VMRUN 延迟演进

| 代 | 微架构 | 延迟 | 优化 |
|:-:|:------|:----:|:-----|
| 10h | Barcelona | ~1000 周期 | 基础 SVM |
| 15h | Bulldozer | ~500-800 | Clean Bits |
| 17h | Zen 1 | ~300-500 | AVIC |
| 19h | Zen 3 | ~200-400 | SPEC_CTRL 直通 |
| 19h+ | Zen 4 | ~150-300 | VmcbPermissive |
| 1Ah | Zen 5 | ~100-250 | 进一步优化 |

---

## 41. VMCB Permissive Mode (Family 19h+)

### 41.1 概述

VMCB Permissive Mode (宽松模式) 是 Zen 4 引入的特性, 改变 VMCB 保留字段的处理方式。

- **CPUID:** `Fn8000_000A_EDX[VmcbPermissive]` (位 27)
- **引入:** Zen 4 (Family 19h)

### 41.2 机制

**传统模式:** 保留位非零 → VMRUN 立即 `#GP(0)` → Hypervisor 崩溃

**宽松模式:** 保留位非零 → VMRUN 成功 → 后续 #VMEXIT 时 EXITCODE=0xFF...FF (VMEXIT_INVALID) → Hypervisor 可优雅处理

### 41.3 好处

1. **向前兼容:** 未来 VMCB 布局变化不导致崩溃
2. **调试友好:** 区分无效状态和真正退出
3. **渐进式支持:** 逐步添加新特性

| 场景 | 建议 |
|:----|:-----|
| 开发 | 关闭 Permissive (尽早发现错误) |
| 生产 | 启用 Permissive (提高兼容性) |
| SEV-ES/SNP | 启用 (VMSA 更复杂) |

---

## 42. ROGPT (Read-Only Guest Page Table)

### 42.1 概述

ROGPT 是 Zen 3 (Family 19h) 的特性, 允许将客户页表设为只读。

- **CPUID:** `Fn8000_000A_EDX[ROGPT]` (位 23)
- **机制:** CR3 写入和 INVLPG 触发 #VMEXIT

### 42.2 对比

| 方法 | 开销 | 粒度 | 场景 |
|:----|:----:|:----:|:-----|
| 传统写保护 | 高 (每页) | 页级 | EPT 钩子 |
| ROGPT | 低 (仅 CR3) | CR3 级 | 跟踪页表切换 |

### 42.3 配置

```c
void enable_rogpt(struct vmcb *vmcb)
{
    vmcb->control.intercept_vectors_2 |= (1 << 19);  // CR3_WRITE_TRAP
    vmcb->control.intercept_vectors_1 |= (1 << 25);  // INVLPG
}

void handle_rogpt_cr3_write(struct vmcb *vmcb)
{
    uint64_t new_cr3 = vmcb->control.exit_info_1;
    
    if (!validate_cr3(new_cr3)) {
        inject_exception(vmcb, 13, 0, 0);  // #GP(0)
        return;
    }
    
    vmcb->save.cr3 = new_cr3;
    vmcb->save.rip = vmcb->control.next_rip;
}
```

---

## 43. GMET (Guest Mode Execute Trap)

### 43.1 概述

GMET 是 Zen 3 (Family 19h) 的特性, 用于监控客户机内核 (Ring 0) 代码执行。

- **CPUID:** `Fn8000_000A_EDX[GMET]` (位 17)
- **机制:** NPT NX 位结合 Ring 0 检测 → NPF

### 43.2 权限矩阵

| NPT NX | NPT R | 客户 Ring | GMET 行为 |
|:------:|:-----:|:---------:|:---------|
| 0 | 1 | Ring 3 | 允许执行 |
| 0 | 1 | Ring 0 | 允许执行 |
| 1 | 1 | Ring 3 | 允许执行 (GMET 不影响) |
| 1 | 1 | Ring 0 | **#VMEXIT (NPF)** |

### 43.3 配置

```c
void enable_gmet(struct vmcb *vmcb)
{
    // 设置 VIRT_EXT 中的 GMET 启用位
    // vmcb->control.virt_ext |= GMET_ENABLE;
    
    // 拦截 #PF 用于客户机页故障处理
    vmcb->control.intercept_exceptions |= (1 << 14);
}
```

### 43.4 NPF 中的 GMET 检测

```c
void handle_gmet_npf(struct vmcb *vmcb)
{
    uint64_t exit_info_1 = vmcb->control.exit_info_1;
    uint64_t exit_info_2 = vmcb->control.exit_info_2;
    
    int is_fetch  = (exit_info_1 >> 4) & 1;
    int is_user   = (exit_info_1 >> 2) & 1;
    int is_ptwalk = (exit_info_1 >> 33) & 1;
    
    if (is_fetch && !is_user && !is_ptwalk) {
        uint8_t cpl = vmcb->save.cpl;
        if (cpl == 0) {
            // Ring 0 执行被 GMET 捕获
            LogInfo("GMET: Ring-0 execute at GPA 0x%llx\n", exit_info_2);
            
            if (should_allow_execution(exit_info_2))
                npt_temp_allow_exec(vmcb, exit_info_2);
        }
    }
}
```

### 43.5 使用场景

| 场景 | 描述 |
|:----|:------|
| 内核完整性监控 | 检测恶意代码注入 |
| 代码执行策略 | 限制内核代码执行区域 |
| 安全启动验证 | 验证加载的内核模块 |

---

---

> **本文档是 `amd-svm-reference.md` 的补充章节，涵盖 SEV/SEV-ES/SEV-SNP、#VC 异常处理、VMSA 结构、GHCB 协议、硬件勘误、罕见的 #VMEXIT 场景、嵌套虚拟化 (Nested SVM)、性能调优及新兴 SVM 特性 (VMCB Permissive Mode、ROGPT、GMET)。**
> **主要参考:** AMD APM Volume 2 (#24593), SEV-ES GHCB Spec (#56421), SEV-SNP Firmware ABI Spec, Linux KVM 源码 (`arch/x86/kvm/svm/`)
> **版本:** 1.0 | **日期:** 2026-06-28
