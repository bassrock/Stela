// SPDX-License-Identifier: GPL-2.0-or-later
/* What libdrm_amdgpu's device and buffer code asks of libdrm beyond `drm.c`,
 * and of its own command submission and asic ID table, which are not built.
 * Every device is a render node here, as `drm.c` makes them, so no client is
 * ever authenticated. The table is a file in libdrm's data directory, which
 * the image has not: without it, libdrm names the device "AMD Radeon
 * Graphics". */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <xf86drm.h>

#define AMDGPU_TIMEOUT_INFINITE 0xffffffffffffffffull

struct amdgpu_device;

int drmGetNodeTypeFromFd(int fd)
{
	(void)fd;
	return DRM_NODE_RENDER;
}

/* The render node's name, which libdrm_amdgpu compares to find a device it
 * already opened. */
char *drmGetPrimaryDeviceNameFromFd(int fd)
{
	struct stat status;
	char *name = malloc(32);
	if (!name)
		return NULL;
	if (fstat(fd, &status)) {
		free(name);
		return NULL;
	}
	snprintf(name, 32, DRM_DIR_NAME "/renderD%u", minor(status.st_rdev));
	return name;
}

void drmMsg(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

/* Only RADV's debug output names a modifier. */
char *drmGetFormatModifierName(uint64_t modifier)
{
	(void)modifier;
	return NULL;
}

/* A relative timeout in nanoseconds as the monotonic deadline the kernel's
 * waits take, infinite where that does not fit. */
uint64_t amdgpu_cs_calculate_timeout(uint64_t timeout)
{
	struct timespec now;
	if (timeout == AMDGPU_TIMEOUT_INFINITE || clock_gettime(CLOCK_MONOTONIC, &now))
		return AMDGPU_TIMEOUT_INFINITE;
	uint64_t current = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
	return timeout > AMDGPU_TIMEOUT_INFINITE - current ? AMDGPU_TIMEOUT_INFINITE : current + timeout;
}

void amdgpu_parse_asic_ids(struct amdgpu_device *dev)
{
	(void)dev;
}
