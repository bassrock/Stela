//! amdgpu: the `amdgpu-y` composite of its makefiles under Stela's Kconfig,
//! compiled unchanged in Stela's Linux environment as a census of the kernel
//! services it imports that the environment does not supply. Nothing links
//! the archive: the census records what the AMD port must still provide.
//!
//! [`super::kbuild`] refuses amdgpu's makefiles, which define variables,
//! continue lines, branch on `ifneq`, add `subdir-ccflags-y` and include the
//! power-management and RAS makefiles through `$(addprefix ...)`. The objects
//! are listed here instead, as GNU make evaluates `amdgpu-y` with only
//! `CONFIG_DRM_AMDGPU` set, and checked against the makefiles' object words:
//! each must be listed, or excluded by the option that selects it, so a moved
//! pin fails the build until the list follows it. The services the composite
//! reaches that nothing supplies are pinned in [`OPEN`] and written, with
//! their importers, to `OUT_DIR/amdgpu-open-imports.txt`.
use super::native::{self, Linux};
use super::nouveau::{Member, members, open_imports, traps};
use std::{
    collections::{BTreeMap, BTreeSet},
    env, fs,
    path::{Path, PathBuf},
    thread,
};
use stela_c_build::linux::Module;

const AMD: &str = "../../third_party/linux/drivers/gpu/drm/amd";
const MODULE: Module<'static> = Module {
    name: "amdgpu",
    file: "drivers/gpu/drm/amd/amdgpu/amdgpu",
};
/// The census report, under `OUT_DIR`.
const REPORT: &str = "amdgpu-open-imports.txt";
/// Declarations of the services Linux's headers leave undeclared under
/// Stela's Kconfig, so the census counts them instead of failing to compile.
const CENSUS_HEADER: &str = "src/compat/linux/stela/amdgpu_census.h";

/// The makefiles `amdgpu-y` is composed from, below `AMD`.
const MAKEFILES: [&str; 15] = [
    "amdgpu/Makefile",
    "pm/Makefile",
    "pm/legacy-dpm/Makefile",
    "pm/powerplay/Makefile",
    "pm/powerplay/hwmgr/Makefile",
    "pm/powerplay/smumgr/Makefile",
    "pm/swsmu/Makefile",
    "pm/swsmu/smu11/Makefile",
    "pm/swsmu/smu12/Makefile",
    "pm/swsmu/smu13/Makefile",
    "pm/swsmu/smu14/Makefile",
    "pm/swsmu/smu15/Makefile",
    "ras/Makefile",
    "ras/rascore/Makefile",
    "ras/ras_mgr/Makefile",
];

/// The makefiles' `ccflags-y` and `subdir-ccflags-y` include directories,
/// below `AMD`, in their order.
const INCLUDES: [&str; 25] = [
    "include/asic_reg",
    "include",
    "amdgpu",
    "pm/inc",
    "acp/include",
    "display",
    "display/include",
    "display/modules/inc",
    "display/dc",
    "display/amdgpu_dm",
    "amdkfd",
    "ras/ras_mgr",
    "pm/swsmu",
    "pm/swsmu/inc",
    "pm/swsmu/inc/pmfw_if",
    "pm/swsmu/smu11",
    "pm/swsmu/smu12",
    "pm/swsmu/smu13",
    "pm/swsmu/smu14",
    "pm/swsmu/smu15",
    "pm/powerplay/inc",
    "pm/powerplay/smumgr",
    "pm/powerplay/hwmgr",
    "pm/legacy-dpm",
    "ras/rascore",
];

/// `amdgpu-y` below `AMD`, in the makefiles' order.
const OBJECTS: &[&str] = &[
    // amdgpu/
    "amdgpu/amdgpu_drv",
    "amdgpu/amdgpu_device",
    "amdgpu/amdgpu_reg_access",
    "amdgpu/amdgpu_doorbell_mgr",
    "amdgpu/amdgpu_kms",
    "amdgpu/amdgpu_atombios",
    "amdgpu/atombios_crtc",
    "amdgpu/amdgpu_connectors",
    "amdgpu/atom",
    "amdgpu/amdgpu_fence",
    "amdgpu/amdgpu_ttm",
    "amdgpu/amdgpu_object",
    "amdgpu/amdgpu_gart",
    "amdgpu/amdgpu_encoders",
    "amdgpu/amdgpu_display",
    "amdgpu/amdgpu_i2c",
    "amdgpu/amdgpu_gem",
    "amdgpu/amdgpu_ring",
    "amdgpu/amdgpu_cs",
    "amdgpu/amdgpu_bios",
    "amdgpu/amdgpu_benchmark",
    "amdgpu/atombios_dp",
    "amdgpu/amdgpu_afmt",
    "amdgpu/amdgpu_trace_points",
    "amdgpu/atombios_encoders",
    "amdgpu/amdgpu_sa",
    "amdgpu/atombios_i2c",
    "amdgpu/amdgpu_dma_buf",
    "amdgpu/amdgpu_vm",
    "amdgpu/amdgpu_vm_pt",
    "amdgpu/amdgpu_vm_tlb_fence",
    "amdgpu/amdgpu_ib",
    "amdgpu/amdgpu_pll",
    "amdgpu/amdgpu_ucode",
    "amdgpu/amdgpu_bo_list",
    "amdgpu/amdgpu_ctx",
    "amdgpu/amdgpu_sync",
    "amdgpu/amdgpu_gtt_mgr",
    "amdgpu/amdgpu_preempt_mgr",
    "amdgpu/amdgpu_vram_mgr",
    "amdgpu/amdgpu_virt",
    "amdgpu/amdgpu_atomfirmware",
    "amdgpu/amdgpu_vf_error",
    "amdgpu/amdgpu_sched",
    "amdgpu/amdgpu_debugfs",
    "amdgpu/amdgpu_ids",
    "amdgpu/amdgpu_gmc",
    "amdgpu/amdgpu_mmhub",
    "amdgpu/amdgpu_hdp",
    "amdgpu/amdgpu_xgmi",
    "amdgpu/amdgpu_csa",
    "amdgpu/amdgpu_ras",
    "amdgpu/amdgpu_vm_cpu",
    "amdgpu/amdgpu_vm_sdma",
    "amdgpu/amdgpu_discovery",
    "amdgpu/amdgpu_ras_eeprom",
    "amdgpu/amdgpu_nbio",
    "amdgpu/amdgpu_umc",
    "amdgpu/smu_v11_0_i2c",
    "amdgpu/amdgpu_fru_eeprom",
    "amdgpu/amdgpu_rap",
    "amdgpu/amdgpu_fw_attestation",
    "amdgpu/amdgpu_securedisplay",
    "amdgpu/amdgpu_eeprom",
    "amdgpu/amdgpu_mca",
    "amdgpu/amdgpu_psp_ta",
    "amdgpu/amdgpu_lsdma",
    "amdgpu/amdgpu_lockdep",
    "amdgpu/amdgpu_ring_mux",
    "amdgpu/amdgpu_xcp",
    "amdgpu/amdgpu_seq64",
    "amdgpu/amdgpu_aca",
    "amdgpu/amdgpu_dev_coredump",
    "amdgpu/amdgpu_cper",
    "amdgpu/amdgpu_userq_fence",
    "amdgpu/amdgpu_eviction_fence",
    "amdgpu/amdgpu_ip",
    "amdgpu/vi",
    "amdgpu/mxgpu_vi",
    "amdgpu/nbio_v6_1",
    "amdgpu/soc15",
    "amdgpu/emu_soc",
    "amdgpu/mxgpu_ai",
    "amdgpu/nbio_v7_0",
    "amdgpu/vega10_reg_init",
    "amdgpu/vega20_reg_init",
    "amdgpu/nbio_v7_4",
    "amdgpu/nbio_v2_3",
    "amdgpu/nv",
    "amdgpu/arct_reg_init",
    "amdgpu/mxgpu_nv",
    "amdgpu/nbio_v7_2",
    "amdgpu/hdp_v4_0",
    "amdgpu/hdp_v5_0",
    "amdgpu/aldebaran_reg_init",
    "amdgpu/aldebaran",
    "amdgpu/soc21",
    "amdgpu/soc24",
    "amdgpu/sienna_cichlid",
    "amdgpu/smu_v13_0_10",
    "amdgpu/nbio_v4_3",
    "amdgpu/hdp_v6_0",
    "amdgpu/nbio_v7_7",
    "amdgpu/hdp_v5_2",
    "amdgpu/lsdma_v6_0",
    "amdgpu/nbio_v7_9",
    "amdgpu/aqua_vanjaram",
    "amdgpu/nbio_v7_11",
    "amdgpu/lsdma_v7_0",
    "amdgpu/hdp_v7_0",
    "amdgpu/nbif_v6_3_1",
    "amdgpu/cyan_skillfish_reg_init",
    "amdgpu/soc_v1_0",
    "amdgpu/lsdma_v7_1",
    "amdgpu/nbio_v6_3_2",
    "amdgpu/df_v1_7",
    "amdgpu/df_v3_6",
    "amdgpu/df_v4_3",
    "amdgpu/df_v4_6_2",
    "amdgpu/df_v4_15",
    "amdgpu/gmc_v7_0",
    "amdgpu/gmc_v8_0",
    "amdgpu/gfxhub_v1_0",
    "amdgpu/mmhub_v1_0",
    "amdgpu/gmc_v9_0",
    "amdgpu/gfxhub_v1_1",
    "amdgpu/mmhub_v9_4",
    "amdgpu/gfxhub_v2_0",
    "amdgpu/mmhub_v2_0",
    "amdgpu/gmc_v10_0",
    "amdgpu/gfxhub_v2_1",
    "amdgpu/mmhub_v2_3",
    "amdgpu/mmhub_v1_7",
    "amdgpu/gfxhub_v3_0",
    "amdgpu/mmhub_v3_0",
    "amdgpu/mmhub_v3_0_2",
    "amdgpu/gmc_v11_0",
    "amdgpu/mmhub_v3_0_1",
    "amdgpu/gfxhub_v3_0_3",
    "amdgpu/gfxhub_v1_2",
    "amdgpu/mmhub_v1_8",
    "amdgpu/mmhub_v3_3",
    "amdgpu/gfxhub_v11_5_0",
    "amdgpu/mmhub_v4_1_0",
    "amdgpu/gfxhub_v12_0",
    "amdgpu/gmc_v12_0",
    "amdgpu/mmhub_v4_2_0",
    "amdgpu/gfxhub_v12_1",
    "amdgpu/gmc_v12_1",
    "amdgpu/umc_v6_0",
    "amdgpu/umc_v6_1",
    "amdgpu/umc_v6_7",
    "amdgpu/umc_v8_7",
    "amdgpu/umc_v8_10",
    "amdgpu/umc_v12_0",
    "amdgpu/umc_v8_14",
    "amdgpu/amdgpu_irq",
    "amdgpu/amdgpu_ih",
    "amdgpu/iceland_ih",
    "amdgpu/tonga_ih",
    "amdgpu/cz_ih",
    "amdgpu/vega10_ih",
    "amdgpu/vega20_ih",
    "amdgpu/navi10_ih",
    "amdgpu/ih_v6_0",
    "amdgpu/ih_v6_1",
    "amdgpu/ih_v7_0",
    "amdgpu/amdgpu_psp",
    "amdgpu/psp_v3_1",
    "amdgpu/psp_v10_0",
    "amdgpu/psp_v11_0",
    "amdgpu/psp_v11_0_8",
    "amdgpu/psp_v12_0",
    "amdgpu/psp_v13_0",
    "amdgpu/psp_v13_0_4",
    "amdgpu/psp_v14_0",
    "amdgpu/psp_v15_0",
    "amdgpu/psp_v15_0_8",
    "amdgpu/dce_v10_0",
    "amdgpu/amdgpu_vkms",
    "amdgpu/amdgpu_gfx",
    "amdgpu/amdgpu_rlc",
    "amdgpu/gfx_v8_0",
    "amdgpu/gfx_v9_0",
    "amdgpu/gfx_v9_4",
    "amdgpu/gfx_v9_4_2",
    "amdgpu/gfx_v9_4_3",
    "amdgpu/gfx_v10_0",
    "amdgpu/imu_v11_0",
    "amdgpu/gfx_v11_0",
    "amdgpu/gfx_v11_0_3",
    "amdgpu/imu_v11_0_3",
    "amdgpu/gfx_v12_0",
    "amdgpu/gfx_v12_1",
    "amdgpu/imu_v12_0",
    "amdgpu/imu_v12_1",
    "amdgpu/amdgpu_sdma",
    "amdgpu/sdma_v2_4",
    "amdgpu/sdma_v3_0",
    "amdgpu/sdma_v4_0",
    "amdgpu/sdma_v4_4",
    "amdgpu/sdma_v4_4_2",
    "amdgpu/sdma_v5_0",
    "amdgpu/sdma_v5_2",
    "amdgpu/sdma_v6_0",
    "amdgpu/sdma_v7_0",
    "amdgpu/sdma_v7_1",
    "amdgpu/amdgpu_mes",
    "amdgpu/mes_v11_0",
    "amdgpu/mes_v12_0",
    "amdgpu/mes_v12_1",
    "amdgpu/mes_userqueue",
    "amdgpu/amdgpu_uvd",
    "amdgpu/uvd_v5_0",
    "amdgpu/uvd_v6_0",
    "amdgpu/uvd_v7_0",
    "amdgpu/amdgpu_vce",
    "amdgpu/vce_v3_0",
    "amdgpu/vce_v4_0",
    "amdgpu/amdgpu_vcn",
    "amdgpu/vcn_sw_ring",
    "amdgpu/vcn_v1_0",
    "amdgpu/vcn_v2_0",
    "amdgpu/vcn_v2_5",
    "amdgpu/vcn_v3_0",
    "amdgpu/vcn_v4_0",
    "amdgpu/vcn_v4_0_3",
    "amdgpu/vcn_v4_0_5",
    "amdgpu/vcn_v5_0_0",
    "amdgpu/vcn_v5_0_1",
    "amdgpu/vcn_v5_0_2",
    "amdgpu/amdgpu_jpeg",
    "amdgpu/jpeg_v1_0",
    "amdgpu/jpeg_v2_0",
    "amdgpu/jpeg_v2_5",
    "amdgpu/jpeg_v3_0",
    "amdgpu/jpeg_v4_0",
    "amdgpu/jpeg_v4_0_3",
    "amdgpu/jpeg_v4_0_5",
    "amdgpu/jpeg_v5_0_0",
    "amdgpu/jpeg_v5_0_1",
    "amdgpu/jpeg_v5_0_2",
    "amdgpu/jpeg_v5_3_0",
    "amdgpu/amdgpu_vpe",
    "amdgpu/vpe_v6_1",
    "amdgpu/vpe_v2_0",
    "amdgpu/amdgpu_umsch_mm",
    "amdgpu/umsch_mm_v4_0",
    "amdgpu/athub_v1_0",
    "amdgpu/athub_v2_0",
    "amdgpu/athub_v2_1",
    "amdgpu/athub_v3_0",
    "amdgpu/athub_v4_1_0",
    "amdgpu/smuio_v9_0",
    "amdgpu/smuio_v11_0",
    "amdgpu/smuio_v11_0_6",
    "amdgpu/smuio_v13_0",
    "amdgpu/smuio_v13_0_3",
    "amdgpu/smuio_v13_0_6",
    "amdgpu/smuio_v14_0_2",
    "amdgpu/smuio_v15_0_0",
    "amdgpu/smuio_v15_0_8",
    "amdgpu/amdgpu_reset",
    "amdgpu/mca_v3_0",
    "amdgpu/amdgpu_amdkfd",
    "amdgpu/amdgpu_userq",
    "amdgpu/amdgpu_cgs",
    "amdgpu/amdgpu_job",
    // pm/swsmu/smu11/
    "pm/swsmu/smu11/arcturus_ppt",
    "pm/swsmu/smu11/navi10_ppt",
    "pm/swsmu/smu11/sienna_cichlid_ppt",
    "pm/swsmu/smu11/vangogh_ppt",
    "pm/swsmu/smu11/cyan_skillfish_ppt",
    "pm/swsmu/smu11/smu_v11_0",
    // pm/swsmu/smu12/
    "pm/swsmu/smu12/renoir_ppt",
    "pm/swsmu/smu12/smu_v12_0",
    // pm/swsmu/smu13/
    "pm/swsmu/smu13/smu_v13_0",
    "pm/swsmu/smu13/aldebaran_ppt",
    "pm/swsmu/smu13/yellow_carp_ppt",
    "pm/swsmu/smu13/smu_v13_0_0_ppt",
    "pm/swsmu/smu13/smu_v13_0_4_ppt",
    "pm/swsmu/smu13/smu_v13_0_5_ppt",
    "pm/swsmu/smu13/smu_v13_0_7_ppt",
    "pm/swsmu/smu13/smu_v13_0_6_ppt",
    "pm/swsmu/smu13/smu_v13_0_12_ppt",
    // pm/swsmu/smu14/
    "pm/swsmu/smu14/smu_v14_0",
    "pm/swsmu/smu14/smu_v14_0_0_ppt",
    "pm/swsmu/smu14/smu_v14_0_2_ppt",
    // pm/swsmu/smu15/
    "pm/swsmu/smu15/smu_v15_0",
    "pm/swsmu/smu15/smu_v15_0_0_ppt",
    "pm/swsmu/smu15/smu_v15_0_8_ppt",
    // pm/swsmu/
    "pm/swsmu/amdgpu_smu",
    "pm/swsmu/smu_cmn",
    // pm/powerplay/smumgr/
    "pm/powerplay/smumgr/smumgr",
    "pm/powerplay/smumgr/smu8_smumgr",
    "pm/powerplay/smumgr/tonga_smumgr",
    "pm/powerplay/smumgr/fiji_smumgr",
    "pm/powerplay/smumgr/polaris10_smumgr",
    "pm/powerplay/smumgr/iceland_smumgr",
    "pm/powerplay/smumgr/smu7_smumgr",
    "pm/powerplay/smumgr/vega10_smumgr",
    "pm/powerplay/smumgr/smu10_smumgr",
    "pm/powerplay/smumgr/ci_smumgr",
    "pm/powerplay/smumgr/vega12_smumgr",
    "pm/powerplay/smumgr/vegam_smumgr",
    "pm/powerplay/smumgr/smu9_smumgr",
    "pm/powerplay/smumgr/vega20_smumgr",
    // pm/powerplay/hwmgr/
    "pm/powerplay/hwmgr/hwmgr",
    "pm/powerplay/hwmgr/processpptables",
    "pm/powerplay/hwmgr/hardwaremanager",
    "pm/powerplay/hwmgr/smu8_hwmgr",
    "pm/powerplay/hwmgr/pppcielanes",
    "pm/powerplay/hwmgr/process_pptables_v1_0",
    "pm/powerplay/hwmgr/ppatomctrl",
    "pm/powerplay/hwmgr/ppatomfwctrl",
    "pm/powerplay/hwmgr/smu7_hwmgr",
    "pm/powerplay/hwmgr/smu7_powertune",
    "pm/powerplay/hwmgr/smu7_thermal",
    "pm/powerplay/hwmgr/smu7_clockpowergating",
    "pm/powerplay/hwmgr/vega10_processpptables",
    "pm/powerplay/hwmgr/vega10_hwmgr",
    "pm/powerplay/hwmgr/vega10_powertune",
    "pm/powerplay/hwmgr/vega10_thermal",
    "pm/powerplay/hwmgr/smu10_hwmgr",
    "pm/powerplay/hwmgr/pp_psm",
    "pm/powerplay/hwmgr/vega12_processpptables",
    "pm/powerplay/hwmgr/vega12_hwmgr",
    "pm/powerplay/hwmgr/vega12_thermal",
    "pm/powerplay/hwmgr/pp_overdriver",
    "pm/powerplay/hwmgr/smu_helper",
    "pm/powerplay/hwmgr/vega20_processpptables",
    "pm/powerplay/hwmgr/vega20_hwmgr",
    "pm/powerplay/hwmgr/vega20_powertune",
    "pm/powerplay/hwmgr/vega20_thermal",
    "pm/powerplay/hwmgr/common_baco",
    "pm/powerplay/hwmgr/vega10_baco",
    "pm/powerplay/hwmgr/vega20_baco",
    "pm/powerplay/hwmgr/vega12_baco",
    "pm/powerplay/hwmgr/smu9_baco",
    "pm/powerplay/hwmgr/tonga_baco",
    "pm/powerplay/hwmgr/polaris_baco",
    "pm/powerplay/hwmgr/fiji_baco",
    "pm/powerplay/hwmgr/ci_baco",
    "pm/powerplay/hwmgr/smu7_baco",
    // pm/powerplay/
    "pm/powerplay/amd_powerplay",
    // pm/legacy-dpm/
    "pm/legacy-dpm/legacy_dpm",
    // pm/
    "pm/amdgpu_dpm",
    "pm/amdgpu_pm",
    "pm/amdgpu_dpm_internal",
    // ras/ras_mgr/
    "ras/ras_mgr/amdgpu_ras_sys",
    "ras/ras_mgr/amdgpu_ras_mgr",
    "ras/ras_mgr/amdgpu_ras_eeprom_i2c",
    "ras/ras_mgr/amdgpu_ras_mp1_v13_0",
    "ras/ras_mgr/amdgpu_ras_cmd",
    "ras/ras_mgr/amdgpu_virt_ras_cmd",
    "ras/ras_mgr/amdgpu_ras_process",
    "ras/ras_mgr/amdgpu_ras_nbio_v7_9",
    // ras/rascore/
    "ras/rascore/ras_core",
    "ras/rascore/ras_mp1",
    "ras/rascore/ras_mp1_v13_0",
    "ras/rascore/ras_aca",
    "ras/rascore/ras_aca_v1_0",
    "ras/rascore/ras_eeprom",
    "ras/rascore/ras_umc",
    "ras/rascore/ras_umc_v12_0",
    "ras/rascore/ras_cmd",
    "ras/rascore/ras_gfx",
    "ras/rascore/ras_gfx_v9_0",
    "ras/rascore/ras_process",
    "ras/rascore/ras_nbio",
    "ras/rascore/ras_nbio_v7_9",
    "ras/rascore/ras_log_ring",
    "ras/rascore/ras_cper",
    "ras/rascore/ras_psp",
    "ras/rascore/ras_psp_v13_0",
    "ras/rascore/ras_eeprom_fw",
];

/// The makefiles' objects Stela's Kconfig drops, by the option that selects
/// them, and the composite itself.
const EXCLUDED: &[(&str, &[&str])] = &[
    ("CONFIG_PROC_FS", &["amdgpu_fdinfo"]),
    ("CONFIG_PERF_EVENTS", &["amdgpu_pmu"]),
    (
        "CONFIG_DRM_AMDGPU_CIK",
        &[
            "cik",
            "cik_ih",
            "dce_v8_0",
            "gfx_v7_0",
            "cik_sdma",
            "uvd_v4_2",
            "vce_v2_0",
            "amdgpu_amdkfd_gfx_v7",
            "kv_dpm",
            "kv_smc",
        ],
    ),
    (
        "CONFIG_DRM_AMDGPU_SI",
        &[
            "si", "gmc_v6_0", "gfx_v6_0", "si_ih", "si_dma", "dce_v6_0", "uvd_v3_1", "vce_v1_0",
            "si_dpm", "si_smc",
        ],
    ),
    (
        "CONFIG_HSA_AMD",
        &[
            "amdgpu_amdkfd_fence",
            "amdgpu_amdkfd_gpuvm",
            "amdgpu_amdkfd_gfx_v8",
            "amdgpu_amdkfd_gfx_v9",
            "amdgpu_amdkfd_arcturus",
            "amdgpu_amdkfd_aldebaran",
            "amdgpu_amdkfd_gc_9_4_3",
            "amdgpu_amdkfd_gfx_v10",
            "amdgpu_amdkfd_gfx_v10_3",
            "amdgpu_amdkfd_gfx_v11",
            "amdgpu_amdkfd_gfx_v12",
            "amdgpu_amdkfd_gfx_v12_1",
        ],
    ),
    ("CONFIG_DRM_AMD_ACP", &["amdgpu_acp"]),
    ("CONFIG_COMPAT", &["amdgpu_ioc32"]),
    ("CONFIG_VGA_SWITCHEROO", &["amdgpu_atpx_handler"]),
    ("CONFIG_ACPI", &["amdgpu_acpi"]),
    ("CONFIG_HMM_MIRROR", &["amdgpu_hmm"]),
    (
        "CONFIG_DRM_AMD_ISP",
        &["amdgpu_isp", "isp_v4_1_0", "isp_v4_1_1"],
    ),
    (
        "obj-$(CONFIG_DRM_AMDGPU), the composite itself",
        &["amdgpu"],
    ),
];

/// The kernel services the composite imports that Stela's Linux environment
/// lacks, or defines only as a trap, by service: the census of 2026-11-12 at
/// Linux 7.2.12, less what the environment has supplied since. Building fails
/// when the imports change, so each supplied service leaves this list with
/// its change.
const OPEN: &[(&str, &[&str])] = &[
    (
        "PCI traps: a function reset Navi 48 never chooses, a message restore only MI300 makes and a search by class only APUs make",
        &[
            "pci_get_class",
            "pci_reset_function",
            "pci_restore_msi_state",
        ],
    ),
    (
        "upstream sources outside the DRM class: the DRM client, reached only with KFD, and amdxcp",
        &[
            "drm_client_init",
            "drm_client_register",
            "drm_client_release",
            "amdgpu_xcp_drm_dev_alloc",
            "amdgpu_xcp_drm_dev_free",
            "amdgpu_xcp_drv_release",
        ],
    ),
    (
        "formatted input: Stela's vsprintf.c has no vsscanf, and Mesa's libc compat keeps the sscanf name in the image",
        &["sscanf"],
    ),
    (
        "interrupts: the domain only amdgpu's VI-era and IH 6.1 handlers create, and only ACP and ISP map into",
        &[
            "generic_handle_domain_irq",
            "handle_simple_irq",
            "irq_create_mapping",
            "irq_domain_create_linear",
            "irq_domain_remove",
            "irq_set_chip_and_handler_name",
        ],
    ),
    (
        "device model: links, which only HDMI audio adds and only APUs' USB-C power link removes, legacy DCE encoders' backlights, and platform devices, as for nouveau",
        &[
            "device_link_add",
            "device_link_del",
            "backlight_device_register",
            "backlight_device_unregister",
            "platform_device_unregister",
        ],
    ),
    (
        "memory: System RAM and page nodes, which only GFX9 APUs' coherence flags look up",
        &["page_is_ram", "pfn_to_nid"],
    ),
    (
        "emergency restart, which only a Vega20 with early firmware requests after a fatal RAS error",
        &["emergency_restart"],
    ),
    (
        "traps nothing amdgpu runs reaches, as for nouveau: shmem-backed pages and reopening a file without a VFS path",
        &[
            "folio_clear_dirty_for_io",
            "mm_get_unmapped_area",
            "shmem_read_folio_gfp",
            "shmem_read_mapping_page_gfp",
            "shmem_truncate_range",
            "shmem_writeout",
            "stela_linux_reopen_unowned_path",
        ],
    ),
];

/// Compiles the composite over the nouveau port's DRM environment, writes the
/// services it reaches that nothing supplies to [`REPORT`] with their
/// importers, and compares them with [`OPEN`].
pub fn census(linux: &Linux) {
    check_objects();
    let includes = INCLUDES.map(|directory| format!("amd/{directory}"));
    let includes = includes.iter().map(String::as_str).collect::<Vec<_>>();
    let sources = OBJECTS
        .iter()
        .map(|object| Path::new(AMD).join(object).with_extension("c"))
        .collect::<Vec<_>>();
    let mut build = stela_c_build::native::configure();
    linux.environment.apply(
        &mut build,
        None,
        &native::drm_includes(&includes),
        Some(&MODULE),
    );
    // The makefiles' `-Wno-override-init` is clang's `-Wno-initializer-overrides`,
    // already in Linux's profile.
    stela_c_build::linux_build::upstream_warnings(&mut build);
    // For the image's target clang checks printf formats by Windows' rules,
    // where the `L` length modifier on an integer conversion is undefined;
    // Linux, and Stela's vsprintf.c, read it as `ll`, as amdgpu_vm.c and
    // vcn_v3_0.c use it. The host build keeps checking amdgpu's formats by
    // Linux's rules.
    if env::var("CARGO_CFG_TARGET_OS").is_ok_and(|os| os == "uefi") {
        build.flag("-Wno-format");
    }
    build
        .flag("-include")
        .flag(CENSUS_HEADER)
        .opt_level(2)
        .cargo_metadata(false);
    println!("cargo:rerun-if-changed={CENSUS_HEADER}");
    let jobs = env::var("NUM_JOBS")
        .ok()
        .and_then(|jobs| jobs.parse().ok())
        .unwrap_or(1)
        .max(1);
    let objects = thread::scope(|scope| {
        sources
            .chunks(sources.len().div_ceil(jobs))
            .map(|chunk| {
                let build = &build;
                scope.spawn(move || build.clone().files(chunk).compile_intermediates())
            })
            .collect::<Vec<_>>()
            .into_iter()
            .flat_map(|compiling| compiling.join().expect("a compiling thread"))
            .collect::<Vec<_>>()
    });
    build.objects(objects).compile("stela-amdgpu-census");
    let output = stela_c_build::linux_build::out_dir();
    let port = members(&output.join("libstela-amdgpu-census.a"));
    let open = open_imports(&port, &[&linux.archive, &linux.common_archive]);
    let report = output.join(REPORT);
    fs::write(&report, report_text(&port, &open)).expect("write the amdgpu census");
    println!("cargo:amdgpu_census={}", report.display());
    let mut open = open;
    let mut stale = Vec::new();
    for symbol in OPEN.iter().flat_map(|(_, symbols)| symbols.iter()) {
        if !open.remove(*symbol) {
            stale.push(*symbol);
        }
    }
    assert!(
        open.is_empty() && stale.is_empty(),
        "amdgpu's imports changed: newly open {open:?}; now supplied {stale:?}"
    );
}

/// Each open service with the composite's members that import it, or
/// `via environment` when only the environment's own objects reach it, and
/// `trap` when the environment defines it without serving it.
fn report_text(port: &[Member], open: &BTreeSet<String>) -> String {
    let mut importers: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
    for member in port {
        for symbol in &member.imported {
            if open.contains(symbol) {
                importers
                    .entry(symbol)
                    .or_default()
                    .push(member_name(&member.name));
            }
        }
    }
    let traps = traps();
    let mut text = format!(
        "# amdgpu census: {} kernel services the composite reaches that Stela's \
         Linux environment does not supply\n# service [trap] <- importing objects\n",
        open.len()
    );
    for symbol in open {
        text.push_str(symbol);
        if traps.contains(symbol) {
            text.push_str(" [trap]");
        }
        text.push_str(" <- ");
        match importers.get(symbol.as_str()) {
            Some(names) => text.push_str(&names.join(" ")),
            None => text.push_str("via environment"),
        }
        text.push('\n');
    }
    text
}

/// An archive member's object name without its directories, its extension
/// and the source path hash cc prefixes it with.
fn member_name(name: &str) -> &str {
    let name = name.rsplit(['/', '\\']).next().unwrap_or(name);
    let name = name.strip_suffix(".o").unwrap_or(name);
    match name.split_once('-') {
        Some((hash, object))
            if hash.len() == 16 && hash.bytes().all(|byte| byte.is_ascii_hexdigit()) =>
        {
            object
        }
        _ => name,
    }
}

/// Every object word of the makefiles is listed or excluded, every listed or
/// excluded object is a word of theirs, and every listed object has a source.
fn check_objects() {
    let mut words = BTreeSet::new();
    for makefile in MAKEFILES {
        let path = Path::new(AMD).join(makefile);
        println!("cargo:rerun-if-changed={}", path.display());
        let text =
            fs::read_to_string(&path).unwrap_or_else(|error| panic!("{}: {error}", path.display()));
        for word in text.split(|c: char| c.is_whitespace() || c == '\\') {
            if let Some(object) = word.strip_suffix(".o") {
                words.insert(object.rsplit('/').next().unwrap_or(object).to_owned());
            }
        }
    }
    let listed = OBJECTS
        .iter()
        .map(|object| object.rsplit('/').next().unwrap_or(object))
        .collect::<BTreeSet<_>>();
    assert_eq!(
        listed.len(),
        OBJECTS.len(),
        "an amdgpu object is listed twice"
    );
    let excluded = EXCLUDED
        .iter()
        .flat_map(|(_, objects)| objects.iter().copied())
        .collect::<BTreeSet<_>>();
    for word in &words {
        assert!(
            listed.contains(word.as_str()) || excluded.contains(word.as_str()),
            "amdgpu's makefiles name {word}.o, which is neither listed nor excluded"
        );
    }
    for object in listed.iter().chain(&excluded) {
        assert!(
            words.contains(*object),
            "{object}.o is listed or excluded, but no amdgpu makefile names it"
        );
    }
    for object in OBJECTS {
        let source: PathBuf = Path::new(AMD).join(object).with_extension("c");
        assert!(source.is_file(), "{} is missing", source.display());
    }
}
