// SPDX-License-Identifier: GPL-2.0-only
/* Kernel-mode FPU sections, as arch/x86/kernel/fpu/core.c's
 * kernel_fpu_begin_mask and kernel_fpu_end provide them: code compiled with
 * CC_FLAGS_FPU (amdgpu's Display Core DML) may use SSE only between the two.
 *
 * Stela's native C is -mgeneral-regs-only and its Rust is soft-float, but the
 * extended state is not free for the taking: the engine keeps translated
 * guests' vector registers in XMM, and its interrupt entry saves and restores
 * whatever is there. So begin saves the CPU's whole enabled extended state
 * (XSAVE of XCR0's components, or FXSAVE without OSXSAVE) and gives the
 * section Linux's clean MXCSR (and a reset x87 for KFPU_387); end restores the
 * saved state exactly, whoever owned it.
 *
 * Linux code runs for one owner at a time, uniprocessor and non-preemptible
 * (stela/linux.h), so one save area serves. The section holds a preemption
 * count, as Linux's does: the scheduler traps on a nonzero count
 * (kernel/task.c's schedule_current) and stela_linux_exit refuses one, so no
 * task switch and no owner exit can happen with the area in use. Hardware
 * interrupts during a section are safe: Stela's interrupt entry saves and
 * restores the full extended state around every handler. Simulated Linux IRQs
 * run as ordinary calls, which keep the callee-saved XMM6-15 and MXCSR
 * controls by the Win64 ABI and use no FPU of their own. A nested begin,
 * which Linux forbids, warns and keeps the outer save. */
#include <linux/bug.h>
#include <linux/export.h>
#include <linux/preempt.h>
#include <linux/string.h>
#include <linux/types.h>
#include <asm/fpu/api.h>
#include <stela/linux.h>

/* XSAVE's standard-format area for XCR0's components: CPUID(0xD, 0).EBX,
 * 832 bytes for x87, SSE and AVX, about 2.7 KiB with AVX-512. */
#define STELA_FPU_AREA 16384
#define XSAVE_HEADER 512
#define XSAVE_HEADER_SIZE 64
#define MXCSR_DEFAULT 0x1f80
#define CPUID1_ECX_OSXSAVE (1U << 27)

static u8 area[STELA_FPU_AREA] __aligned(64);
/* Nesting depth of the active section; nonzero while the area holds state. */
static unsigned int depth;
/* XCR0 when the area was last sized, or 0 to use FXSAVE. */
static u64 saved_mask;

static void cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
	asm volatile("cpuid"
		     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		     : "a"(leaf), "c"(subleaf));
}

static bool osxsave(void)
{
	static int known = -1;
	if (known < 0) {
		u32 eax, ebx, ecx, edx;
		cpuid(1, 0, &eax, &ebx, &ecx, &edx);
		known = !!(ecx & CPUID1_ECX_OSXSAVE);
	}
	return known;
}

static u64 xcr0(void)
{
	u32 low, high;
	asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
	return (u64)high << 32 | low;
}

/* Size the area for the XCR0 in force; CPUID(0xD, 0).EBX is the standard
 * format's size for the components XCR0 enables. */
static u64 save_mask(void)
{
	static u64 sized;
	if (!osxsave())
		return 0;
	u64 mask = xcr0();
	if (mask != sized) {
		u32 eax, ebx, ecx, edx;
		cpuid(0xd, 0, &eax, &ebx, &ecx, &edx);
		if (ebx > sizeof(area))
			panic("kernel FPU: XSAVE area of %u bytes for XCR0 %#llx exceeds %zu",
			      ebx, mask, sizeof(area));
		sized = mask;
	}
	return mask;
}

static void save(void)
{
	saved_mask = save_mask();
	if (!saved_mask) {
		asm volatile("fxsave64 %0" : "=m"(*(u8 (*)[512])area));
		return;
	}
	/* XSAVE writes XSTATE_BV but neither XCOMP_BV nor the reserved header
	 * bytes, which XRSTOR's standard form requires to be zero. */
	memset(area + XSAVE_HEADER, 0, XSAVE_HEADER_SIZE);
	asm volatile("xsave64 %0"
		     : "+m"(*(u8 (*)[STELA_FPU_AREA])area)
		     : "a"((u32)saved_mask), "d"((u32)(saved_mask >> 32)));
}

static void restore(void)
{
	if (!saved_mask) {
		asm volatile("fxrstor64 %0" : : "m"(*(const u8 (*)[512])area));
		return;
	}
	asm volatile("xrstor64 %0"
		     :
		     : "m"(*(const u8 (*)[STELA_FPU_AREA])area),
		       "a"((u32)saved_mask), "d"((u32)(saved_mask >> 32)));
}

void kernel_fpu_begin_mask(unsigned int kfpu_mask)
{
	preempt_disable();
	if (WARN_ON_ONCE(depth++))
		return;
	save();
	if (kfpu_mask & KFPU_MXCSR) {
		u32 mxcsr = MXCSR_DEFAULT;
		asm volatile("ldmxcsr %0" : : "m"(mxcsr));
	}
	if (kfpu_mask & KFPU_387)
		asm volatile("fninit");
}
EXPORT_SYMBOL_GPL(kernel_fpu_begin_mask);

void kernel_fpu_end(void)
{
	if (WARN_ON_ONCE(!depth))
		return;
	if (!--depth)
		restore();
	preempt_enable();
}
EXPORT_SYMBOL_GPL(kernel_fpu_end);

unsigned int stela_linux_kernel_fpu_depth(void)
{
	return depth;
}

/* Linux code never runs in a hardware interrupt here, and a simulated IRQ
 * may use the FPU only outside another section. */
bool irq_fpu_usable(void)
{
	return !depth;
}
EXPORT_SYMBOL(irq_fpu_usable);
