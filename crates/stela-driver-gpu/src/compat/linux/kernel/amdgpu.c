// SPDX-License-Identifier: GPL-2.0-or-later
/* What amdgpu imports from beyond its composite and Linux's environment that
 * no path on a Navi 48 board reaches: traps, so a reach stops instead of
 * continuing with an invented answer. */
#include <drm/drm_client.h>
#include <drm/drm_device.h>
#include <unsupported.h>

/* The DRM client, which amdgpu sets up only for KFD (CONFIG_HSA_AMD). */
STELA_UNSUPPORTED(int, drm_client_init, struct drm_device *, struct drm_client_dev *,
		  const char *, const struct drm_client_funcs *)
STELA_UNSUPPORTED(void, drm_client_register, struct drm_client_dev *)
STELA_UNSUPPORTED(void, drm_client_release, struct drm_client_dev *)

/* amdxcp's partition devices, which amdgpu allocates only for partitioned
 * GPUs: GC 9.4.3 and 12.1. */
STELA_UNSUPPORTED(int, amdgpu_xcp_drm_dev_alloc, struct drm_device **)
STELA_UNSUPPORTED(void, amdgpu_xcp_drm_dev_free, struct drm_device *)

void amdgpu_xcp_drv_release(void);

/* amdxcp's release at amdgpu's exit, following amd/amdxcp/amdgpu_xcp_drv.c:
 * it frees the partition devices allocated, and none were. */
void amdgpu_xcp_drv_release(void)
{
}

/* A pfn's NUMA node, which Linux's mmzone.h defines once a memory model is
 * configured and Stela's Kconfig configures none; amdgpu asks only for GFX9
 * APUs' coherence flags. */
STELA_UNSUPPORTED(int, pfn_to_nid, unsigned long)
