// SPDX-License-Identifier: GPL-2.0-or-later
/* Clock reads from the retained owner, independent of timer delivery. */
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/sched/clock.h>
#include <linux/timekeeping.h>
#include <stela/costs.h>
#include <stela/linux.h>
#include <stela/time.h>

ktime_t ktime_get(void)
{
	const struct stela_linux_owner *owner = stela_linux_owner();
	if (!owner->services->now_ns)
		__builtin_trap();
	/* Diagnostics (stela/costs.h): one read in 16 timed. */
	u64 start = stela_linux_cost_start(&stela_linux_costs.clock, 16);
	ktime_t now = owner->services->now_ns(owner->context);
	stela_linux_cost_end(&stela_linux_costs.clock, start);
	return now;
}

notrace u64 sched_clock(void)
{
	ktime_t now = ktime_get();
	if (now < 0 || now == KTIME_MAX)
		stela_linux_fatal("invalid scheduler clock sample");
	return (u64)now;
}

int stela_linux_boottime_ns(s64 *now)
{
	const struct stela_linux_owner *owner = stela_linux_owner();
	s64 sample = KTIME_MAX;
	if (!owner->services->boottime_ns ||
	    !owner->services->boottime_ns(owner->context, &sample))
		return -ENODEV;
	if (sample < 0 || sample == KTIME_MAX)
		return -ERANGE;
	*now = sample;
	return 0;
}

static void realtime(struct timespec64 *ts, u32 mode, u32 tick_ns)
{
	const struct stela_linux_owner *owner = stela_linux_owner();
	s64 seconds;
	u32 nanoseconds;

	if (owner->services->realtime_at &&
	    owner->services->realtime_at(owner->context, ktime_get(), mode,
					 tick_ns, &seconds, &nanoseconds)) {
		*ts = (struct timespec64){.tv_sec = seconds, .tv_nsec = nanoseconds};
		return;
	}

	/* The void Linux API uses the epoch when this host has no UTC source. */
	stela_linux_unsupported("realtime clock unavailable");
	*ts = (struct timespec64){};
}

void ktime_get_real_ts64(struct timespec64 *ts)
{
	realtime(ts, 0, 0);
}

void ktime_get_coarse_real_ts64_mg(struct timespec64 *ts)
{
	realtime(ts, 1, TICK_NSEC);
}

void ktime_get_real_ts64_mg(struct timespec64 *ts)
{
	realtime(ts, 2, 0);
}

ktime_t ktime_get_with_offset(enum tk_offsets offs)
{
	switch (offs) {
	case TK_OFFS_BOOT: {
		s64 sample;
		if (stela_linux_boottime_ns(&sample))
			stela_linux_fatal("Linux boottime clock unavailable");
		return sample;
	}
	case TK_OFFS_REAL: {
		struct timespec64 ts;
		ktime_get_real_ts64(&ts);
		__int128 ns = (__int128)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
		if (ns < S64_MIN || ns >= KTIME_MAX)
			stela_linux_fatal("Linux realtime clock outside ktime range");
		return (ktime_t)ns;
	}
	case TK_OFFS_TAI:
		stela_linux_fatal("Linux TAI clock unavailable");
	default:
		stela_linux_fatal("invalid Linux clock offset");
	}
}

/* The NMI-safe CLOCK_MONOTONIC accessor. Linux code here runs in one context
 * at a time, so the ordinary read serves it. */
u64 notrace ktime_get_mono_fast_ns(void)
{
	return (u64)ktime_get();
}

/* CLOCK_MONOTONIC_RAW: the monotonic clock without NTP's frequency
 * adjustment. The owner's clock is never slewed, so it is the same reading. */
ktime_t ktime_get_raw(void)
{
	return ktime_get();
}

/* CLOCK_REALTIME's seconds, the epoch's when the owner has no UTC source. */
time64_t ktime_get_real_seconds(void)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	return ts.tv_sec;
}

#ifdef STELA_LINUX_TIMEKEEPING_TEST
#include <stela/test.h>

static s64 now = 1234567890;
static bool utc = true;
static unsigned int unavailable;

static s64 test_now(void *context)
{
	(void)context;
	return now;
}

static bool test_realtime(void *context, s64 now_ns, u32 mode, u32 tick_ns, s64 *seconds,
			  u32 *nanoseconds)
{
	(void)context;
	assert(now_ns == now && !mode && !tick_ns);
	if (!utc)
		return false;
	*seconds = 1763049600;
	*nanoseconds = 5;
	return true;
}

static void test_unsupported(void *context, const char *operation)
{
	(void)context;
	assert(!strcmp(operation, "realtime clock unavailable"));
	unavailable++;
}

int main(void)
{
	const struct stela_linux_services services = {
		.now_ns = test_now,
		.realtime_at = test_realtime,
		.unsupported = test_unsupported,
	};
	const struct stela_linux_owner owner = {.services = &services};
	stela_linux_enter(&owner);
	/* The fast monotonic accessor reads the owner's monotonic clock. */
	assert(ktime_get_mono_fast_ns() == 1234567890);
	now += 1000;
	assert(ktime_get_mono_fast_ns() == 1234568890);
	/* Wall seconds come from the owner's UTC source, else the epoch's. */
	assert(ktime_get_real_seconds() == 1763049600 && !unavailable);
	utc = false;
	assert(ktime_get_real_seconds() == 0 && unavailable == 1);
	stela_linux_exit();
	printf("Linux timekeeping: the fast monotonic clock and wall seconds passed\n");
	return 0;
}
#endif
