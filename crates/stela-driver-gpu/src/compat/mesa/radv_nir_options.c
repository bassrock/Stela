/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The NIR options a device's shaders are built with, for the AIR compiler,
 * which reaches RADV by no route Vulkan offers: `nir_options.c` for RADV.
 *
 * RADV keeps its options per logical device and per stage
 * (radv_device.c's compiler info, get_nir_options_for_stage), and
 * replaces a stage's NIR's options with its own as it compiles it
 * (radv_shader_spirv_to_nir). These are the options of a stage with no
 * split FMA, the default, built from the physical device's compiler info as
 * RADV builds its own, for what the AIR compiler does before RADV takes the
 * NIR. Each physical device's options live as long as the image. */
#include "ac_nir.h"
#include "radv_physical_device.h"

const struct nir_shader_compiler_options *stela_radv_nir_options(VkPhysicalDevice physical);

#define STELA_RADV_DEVICES 8

static struct {
	const struct radv_physical_device *pdev;
	nir_shader_compiler_options options;
} built[STELA_RADV_DEVICES];

const struct nir_shader_compiler_options *stela_radv_nir_options(VkPhysicalDevice physical)
{
	VK_FROM_HANDLE(radv_physical_device, pdev, physical);
	for (unsigned index = 0; index < STELA_RADV_DEVICES; index++) {
		if (built[index].pdev == pdev)
			return &built[index].options;
		if (built[index].pdev)
			continue;
		nir_shader_compiler_options *options = &built[index].options;
		ac_nir_set_options(&pdev->info.compiler_info, pdev->use_llvm, options);
		options->max_unroll_iterations = 32;
		options->max_unroll_iterations_aggressive = 128;
		options->lower_doubles_options = nir_lower_drcp | nir_lower_dsqrt | nir_lower_drsq | nir_lower_ddiv;
		options->frag_coord_form = nir_frag_coord_xy_z_w_separate | nir_frag_coord_use_w_rcp;
		options->io_options |= nir_io_mediump_is_32bit | nir_io_radv_intrinsic_component_workaround;
		options->varying_expression_max_cost = ac_nir_varying_expression_max_cost;
		built[index].pdev = pdev;
		return options;
	}
	return NULL;
}
