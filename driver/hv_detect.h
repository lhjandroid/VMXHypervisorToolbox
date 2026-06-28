/*
 * hv_detect.h - VMX Anti-Anti-Debug Hypervisor
 * CPU vendor detection and virtualization capability probing
 */

#ifndef _HV_DETECT_H_
#define _HV_DETECT_H_

#include <ntddk.h>
#include "hv_ops.h"

/* ========================================================================= */
/*  CPU Detection                                                            */
/* ========================================================================= */

/*
 * Detect CPU vendor using CPUID leaf 0.
 * Returns CPU_VENDOR_INTEL, CPU_VENDOR_AMD, or CPU_VENDOR_UNKNOWN.
 */
CPU_VENDOR HvDetectCpuVendor(VOID);

/*
 * Check if Intel VT-x (VMX) is supported and enabled.
 * Checks CPUID.1:ECX[5] and IA32_FEATURE_CONTROL MSR.
 */
BOOLEAN HvCheckVmxSupport(VOID);

/*
 * Check if AMD SVM is supported and enabled.
 * Checks CPUID 0x80000001:ECX[2] and MSR_VM_CR.
 */
BOOLEAN HvCheckSvmSupport(VOID);

/*
 * Check if AMD NPT (Nested Page Tables) is supported.
 * Checks CPUID 0x8000000A:EDX[0].
 */
BOOLEAN HvCheckNptSupport(VOID);

/*
 * Get AMD SVM revision and number of ASIDs.
 * SVM CPUID function 0x8000000A.
 */
ULONG HvGetSvmRevision(VOID);
ULONG HvGetMaxAsid(VOID);

/*
 * HvIsRunningUnderHypervisor - Detect if we are already running under a
 * hypervisor (i.e., we are NOT on bare metal).
 *
 * Checks CPUID.1:ECX[31] (hypervisor present bit) and CPUID leaf
 * 0x40000000 (hypervisor vendor ID leaf).  If either reports a
 * hypervisor, returns TRUE.
 *
 * This project MUST run on bare metal: no nested virtualization,
 * no Hyper-V, no VMware, no VirtualBox, no KVM, no Xen.
 */
BOOLEAN HvIsRunningUnderHypervisor(VOID);

/*
 * HvIsHyperVEnabled - Detect if Windows Hyper-V (HypervisorPlatform or
 * VirtualSecureMode / VBS) is active by checking for the Hyper-V
 * synthetic interface via CPUID 0x40000001 (Hv#1 interface ID).
 *
 * Returns TRUE if Hyper-V enlightenments are available.
 */
BOOLEAN HvIsHyperVEnabled(VOID);

#endif /* _HV_DETECT_H_ */
