//! RADV, ACO and addrlib for `stela.efi`: the lane of [`super::nvk`] for
//! AMD's Vulkan driver. RADV is C and its compiler, ACO, and addrlib C++; it
//! has no Rust. Its kernel interface is libdrm_amdgpu, whose device, buffer
//! and address space code builds here from libdrm's own sources.
use super::nvk::Lane;

pub(super) const RADV: Lane = Lane {
    name: "radv",
    vulkan_drivers: "amd",
    drivers: &["src/amd/vulkan/libvulkan_radeon.so"],
    // RADV's own objects are the shared library's, not a static library's.
    whole_libraries: &[
        "libvulkan_lite_runtime_a",
        "libvulkan_runtime_a",
        "libvulkan_instance_a",
        "libvulkan_radeon_so",
    ],
    skipped: &[
        "build_id",
        "u_qsort",
        "os_file_notify",
        "os_socket",
        "disk_cache",
        "mesa_cache_db",
        "u_debug_stack",
        "wsi_common_display",
    ],
    // addrlib's AVX2 swizzles, chosen by a CPU check of compiler-rt's that the
    // image lacks; the image runs on any x86-64.
    host_flags: &["-DADDR_ALLOW_SIMD=1"],
    stand_ins: &[
        "build_id",
        "drm",
        "soft_float",
        "drm_amdgpu",
        "radv_nir_options",
    ],
    vendored: &[
        ("libdrm", "amdgpu/amdgpu_bo.c"),
        ("libdrm", "amdgpu/amdgpu_device.c"),
        ("libdrm", "amdgpu/amdgpu_gpu_info.c"),
        ("libdrm", "amdgpu/amdgpu_vamgr.c"),
        ("libdrm", "amdgpu/handle_table.c"),
    ],
    vendored_flags: &[
        "-DHAVE_LIBDRM_ATOMIC_PRIMITIVES=1",
        // amdgpu in the image's Linux environment has functions of these
        // names; Mesa calls neither, only libdrm's own units.
        "-Damdgpu_bo_list_create=stela_libdrm_amdgpu_bo_list_create",
        "-Damdgpu_bo_set_metadata=stela_libdrm_amdgpu_bo_set_metadata",
        "-I{libdrm}",
        "-I{libdrm}/include/drm",
        "-I{libdrm}/amdgpu",
    ],
    model_unit: "radv_query.c",
    rust: false,
    // Without it, RADV reads ACO's binaries directly and leaves out the ELF
    // of LLVM's and of RGP traces.
    libelf: false,
    packages: &[(
        "libdrm_amdgpu.pc",
        "Name: libdrm_amdgpu\nDescription: vendored, as `radv::RADV` builds it\n\
         Version: {version}\nRequires: libdrm\n\
         Cflags: -I{libdrm} -I{libdrm}/include/drm -I{libdrm}/amdgpu\n",
    )],
    archive: "libstela-radv.a",
};
