/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RADV's counterparts of `submit_thread.c`'s NVK refusal hooks.
 *
 * A submission amdgpu refuses short of losing the context still makes its
 * signals (radv_queue_refuse, Mesa patch 0034), so the queue goes on; the
 * caller asks for the failure by a signal once it shows
 * (stela_radv_submit_failure). */
#include "radv_queue.h"
#include "vk_queue.h"
#include "vk_semaphore.h"

VkResult stela_radv_submit_failure(VkQueue queue_handle, VkSemaphore semaphore_handle, uint64_t value);
uint32_t stela_radv_forget_failures(VkQueue queue_handle);
void stela_radv_count_failures(VkQueue queue_handle, uint32_t *outstanding);

/* The failure of the refused submission that signaled `value` on
 * `semaphore_handle`, taken once; VK_SUCCESS for every other. */
VkResult stela_radv_submit_failure(VkQueue queue_handle, VkSemaphore semaphore_handle, uint64_t value)
{
	VK_FROM_HANDLE(vk_queue, queue, queue_handle);
	VK_FROM_HANDLE(vk_semaphore, semaphore, semaphore_handle);
	struct radv_queue *radv = container_of(queue, struct radv_queue, vk);
	return radv_queue_take_failure(radv, vk_semaphore_get_active_sync(semaphore), value);
}

/* Drops the kept failures nobody will take; answers how many. */
uint32_t stela_radv_forget_failures(VkQueue queue_handle)
{
	VK_FROM_HANDLE(vk_queue, queue, queue_handle);
	return radv_queue_forget_failures(container_of(queue, struct radv_queue, vk));
}

/* Has the queue raise `*outstanding` by each failure it keeps, before the
 * refused submission's signals are made, and lower it by each it drops
 * unasked; the caller lowers it by those it takes and settles. Before the
 * queue's first submission. */
void stela_radv_count_failures(VkQueue queue_handle, uint32_t *outstanding)
{
	VK_FROM_HANDLE(vk_queue, queue, queue_handle);
	container_of(queue, struct radv_queue, vk)->failures_outstanding = outstanding;
}
