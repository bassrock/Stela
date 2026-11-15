/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef STELA_COSTS_H
#define STELA_COSTS_H
/* Stela diagnostics: how often Linux code calls the environment's hot
 * primitives and the TSC cycles they take, for the host's 10 s `LINUX COSTS`
 * line (crates/stela-boot/src/engine/gpu.rs). Observation only: nothing in
 * Linux reads them back. Only the entered owner writes them, and one owner
 * runs Linux code at a time, so each count grows by a plain load and store,
 * with no locked instruction on the paths it measures. The counts only grow;
 * the host reads them and reports how far each grew since it last looked. */
#include <linux/types.h>

struct stela_linux_cost {
	/* Calls, the calls timed (every call, or one in a power of two for the
	 * most frequent), and the cycles those timed took. */
	u64 calls, timed, cycles;
};

/* Mirrored by `LinuxCosts` in crates/stela-boot/src/engine/gpu.rs. */
struct stela_linux_costs {
	/* Validations of the current task: current, current_thread_info (so
	 * every preempt_count change and test) and the scheduler's own. */
	struct stela_linux_cost task;
	/* ktime_get, through the owner's clock. */
	struct stela_linux_cost clock;
	struct stela_linux_cost kmalloc;
	struct stela_linux_cost kfree;
	struct stela_linux_cost cache_alloc;
	struct stela_linux_cost cache_free;
	/* raw_copy_from_user and raw_copy_to_user. */
	struct stela_linux_cost copy_user;
	struct stela_linux_cost resched;
	/* Slabs kmem_cache_alloc and kmem_cache_free passed over. */
	u64 cache_steps;
	/* stela_linux_cache_range's lookups and the slabs they passed over. */
	u64 range_calls, range_steps;
	u64 copy_bytes;
};

extern struct stela_linux_costs stela_linux_costs;

static inline u64 stela_linux_cost_clock(void)
{
#ifdef __x86_64__
	return __builtin_ia32_rdtsc();
#else
	return 0;
#endif
}

static inline void stela_linux_cost_add(u64 *counter, u64 value)
{
	__atomic_store_n(counter, __atomic_load_n(counter, __ATOMIC_RELAXED) + value,
			 __ATOMIC_RELAXED);
}

/* Counts a call of `cost` and times one in `every` (a power of two): the
 * starting cycle count, or zero for a call not timed. */
static inline u64 stela_linux_cost_start(struct stela_linux_cost *cost, u64 every)
{
	u64 calls = __atomic_load_n(&cost->calls, __ATOMIC_RELAXED);
	__atomic_store_n(&cost->calls, calls + 1, __ATOMIC_RELAXED);
	return (calls & (every - 1)) ? 0 : stela_linux_cost_clock();
}

static inline void stela_linux_cost_end(struct stela_linux_cost *cost, u64 start)
{
	if (!start)
		return;
	stela_linux_cost_add(&cost->timed, 1);
	stela_linux_cost_add(&cost->cycles, stela_linux_cost_clock() - start);
}
#endif
