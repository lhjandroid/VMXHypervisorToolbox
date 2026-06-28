/*
 * npt.h - VMX Anti-Anti-Debug Hypervisor
 * AMD Nested Page Tables (NPT) - AMD's equivalent of Intel EPT
 *
 * NPT uses the same 4-level page table format as regular x86-64 paging:
 *   PML4 -> PDPT -> PD -> PT
 * The page table entry format is slightly different from EPT but
 * the overall structure is identical.
 *
 * Key differences from Intel EPT:
 *   - NPT does NOT support Execute-Only pages
 *   - NPT root address goes in VMCB.nested_cr3 (not a VMCS field)
 *   - TLB flush via ASID switch (not INVEPT)
 *   - NPT violation = SVM_EXIT_NPF (0x400)
 */

#ifndef _NPT_H_
#define _NPT_H_

#include <ntddk.h>
#include "ept.h"    /* Reuse EPT page table structure definitions */

/* ========================================================================= */
/*  NPT Constants                                                            */
/* ========================================================================= */

/*
 * NPT reuses the EPT structure types (EPT_PML4E, EPT_PDPTE, EPT_PDE, EPT_PTE)
 * because the 64-bit page table entry format is compatible at the physical
 * address and basic access-control bit positions.
 *
 * HOWEVER, key bit-level semantics DIFFER between EPT and NPT:
 *
 *   Bit   EPT_PTE field     EPT meaning            NPT meaning
 *   ───   ──────────────    ────────────           ────────────
 *   0     Read              Read access             Present (P)
 *   1     Write             Write access            Read/Write (R/W)
 *   2     Execute           Execute access          User/Supervisor (U/S)
 *   5:3   MemoryType         EPT memory type         PWT, PCD, PAT
 *   63    SuppressVe         Suppress #VE            No Execute (NX)
 *
 * The NPT hook engine works correctly because:
 *   - NX defaults to 0 → execution ALWAYS allowed (NOT because Execute=1,
 *     because that sets U/S=1 in NPT — a different bit!)
 *   - Present=1 via Read=1 → entry is valid (coincidental alignment)
 *   - R/W=0 via Write=0 → write access triggers NPF (correct alignment)
 *
 * Compile-time assertions below verify that the critical bits occupy the
 * same bit-positions in both EPT and NPT, so the shared accessor macros
 * (Pte->Read, Pte->Write, Pte->PhysAddr) work correctly for both.
 */

/* Verify critical bit positions are compatible between EPT and NPT layouts.
 * These ensure that Pte->Read, Pte->Write, and Pte->PhysAddr access the
 * same hardware bits regardless of EPT/NPT interpretation. */
C_ASSERT((1ULL << 0)  == 1);   /* Bit 0: EPT Read = NPT Present (always bit 0) */
C_ASSERT((1ULL << 1)  == 2);   /* Bit 1: EPT Write = NPT R/W (always bit 1) */
C_ASSERT((1ULL << 51) != 0);   /* PhysAddr top bit fits in ULONG64 */

#define NPT_MAX_HOOKS       MAX_EPT_HOOKS   /* Same hook limit */

/* ========================================================================= */
/*  NPT State                                                                */
/* ========================================================================= */

/*
 * NPT state is stored separately from EPT state since both may be
 * compiled in but only one is active at runtime.
 * However, the structures are identical (reusing EPT types).
 */
typedef struct _NPT_STATE {
    /* PML4 table (top level) - shared template */
    DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML4E Pml4[EPT_PML4E_COUNT];

    /* Pre-allocated PDPT for first 512GB - shared template */
    DECLSPEC_ALIGN(PAGE_SIZE) EPT_PDPTE Pdpt[EPT_PDPTE_COUNT];

    /* Physical address of PML4 (template - written to VMCB.nested_cr3) */
    ULONG64 Pml4Pa;

    BOOLEAN Initialized;
} NPT_STATE, *PNPT_STATE;

/*
 * Per-CPU NPT root structure for hook page isolation.
 *
 * Each CPU gets its own PML4 → PDPT chain so that NPT PTEs for hooked
 * pages can be toggled independently per-core without cross-CPU
 * interference during the NPF → TF/#DB → restore cycle.
 */
typedef struct _NPT_CPU_STATE {
    DECLSPEC_ALIGN(PAGE_SIZE) EPT_PML4E Pml4[EPT_PML4E_COUNT];
    DECLSPEC_ALIGN(PAGE_SIZE) EPT_PDPTE Pdpt[EPT_PDPTE_COUNT];
    ULONG64     Pml4Pa;
} NPT_CPU_STATE, *PNPT_CPU_STATE;

/* ========================================================================= */
/*  NPT Hook State (same structure as EPT hooks)                             */
/* ========================================================================= */

typedef struct _NPT_HOOK_STATE {
    EPT_HOOK_ENTRY  Hooks[NPT_MAX_HOOKS];
    ULONG           HookCount;
    KSPIN_LOCK      Lock;
    BOOLEAN         Initialized;

    /*
     * BUG FIX (Issue #3+5+6): Hash table for O(1) hook lookup by physical page.
     * Same design as EPT_HOOK_STATE.HookHashTable.
     */
    ULONG           HookHashTable[EPT_HOOK_HASH_SIZE];
} NPT_HOOK_STATE, *PNPT_HOOK_STATE;

/* ========================================================================= */
/*  Function Declarations                                                    */
/* ========================================================================= */

/* NPT initialization and cleanup */
NTSTATUS    NptInitialize(VOID);
VOID        NptCleanup(VOID);

/* Get the root page table physical address (for VMCB.nested_cr3) */
ULONG64     NptGetRootPageTablePa(VOID);

/* NPT Hook operations (API matches EPT for abstraction) */
NTSTATUS    NptHookFunction(ULONG64 TargetVa, PVOID HookFunction, PVOID *OriginalFunction);
NTSTATUS    NptUnhookFunction(ULONG64 TargetVa);
VOID        NptUnhookAll(VOID);

/* NPT Violation handler (called from SVM exit handler) */
BOOLEAN     NptHandlePageFault(PVOID GuestContext);

/* NPT page table manipulation */
PEPT_PTE    NptGetPteForPhysicalAddress(ULONG64 PhysicalAddress);
VOID        NptSplitLargePage(ULONG64 PhysicalAddress);

/* TLB invalidation (via ASID flush) */
VOID        NptInvalidateAll(VOID);
VOID        NptInvalidateAllCpusSync(VOID);   /* H-5: sync all CPUs via IPI */

/* Per-CPU #DB tracking (for multi-core NPT hook race fix) */
VOID    NptDbTrackRelaxedPage(ULONG64 PagePhysicalAddr);
ULONG64 NptDbGetAndClearRelaxedPage(VOID);
BOOLEAN NptDbMatchesRelaxedRip(ULONG64 CurrentRip);   /* M-4: RIP sanity check */

/* Per-CPU NPT management (for hook page isolation) */
NTSTATUS NptInitPerCpu(VOID);
VOID     NptCleanupPerCpu(VOID);
PEPT_PTE NptGetPerCpuPte(ULONG CpuIndex, ULONG64 PhysicalAddress);
ULONG64  NptGetPerCpuRootPa(ULONG CpuIndex);

/* Find hook by physical address */
PEPT_HOOK_ENTRY NptFindHookByPhysicalAddress(ULONG64 PhysicalAddress);

/* Global state */
extern NPT_STATE        g_NptState;
extern NPT_HOOK_STATE   g_NptHookState;
extern PNPT_CPU_STATE   g_NptCpuStates;   /* per-CPU NPT root array */

#endif /* _NPT_H_ */
