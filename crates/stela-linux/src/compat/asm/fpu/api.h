/* SPDX-License-Identifier: GPL-2.0-only */
/* The kernel-mode FPU API of x86's <asm/fpu/api.h>, which kernel/fpu.c
 * implements: what code compiled with CC_FLAGS_FPU brackets its FPU use
 * with. The task FPU state, signal frames and KVM hooks of the original
 * header have no counterpart: Linux code here runs no user tasks. */
#ifndef _ASM_X86_FPU_API_H
#define _ASM_X86_FPU_API_H

#include <linux/bits.h>
#include <linux/types.h>

/* Kernel FPU states to initialize in kernel_fpu_begin_mask() */
#define KFPU_387	_BITUL(0)	/* 387 state will be initialized */
#define KFPU_MXCSR	_BITUL(1)	/* MXCSR will be initialized */

extern void kernel_fpu_begin_mask(unsigned int kfpu_mask);
extern void kernel_fpu_end(void);
extern bool irq_fpu_usable(void);

/* Code that is unaware of kernel_fpu_begin_mask() can use this. Any 64-bit
 * code that uses 387 instructions must explicitly request KFPU_387. */
static inline void kernel_fpu_begin(void)
{
	kernel_fpu_begin_mask(KFPU_MXCSR);
}

#endif
