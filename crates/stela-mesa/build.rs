//! Links the archives `cargo xtask gpu nvk` or `cargo xtask gpu radv` builds,
//! by the crate's driver feature.
use std::{env, path::PathBuf};

fn main() {
    let nvk = env::var_os("CARGO_FEATURE_NVK").is_some();
    let radv = env::var_os("CARGO_FEATURE_RADV").is_some();
    // The lanes share the runtime, the AIR compiler and the C library's
    // units, so an image links one driver.
    assert!(
        nvk != radv,
        "stela-mesa links one Vulkan driver: enable exactly one of `nvk` and `radv`"
    );
    let (lane, driver) = if nvk {
        ("nvk", "stela-nvk")
    } else {
        ("radv", "stela-radv")
    };
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("set by Cargo"));
    let gpu = manifest.join("../../target/gpu");
    let directory = gpu.join(lane);
    println!("cargo:rustc-link-search=native={}", directory.display());
    // LLVM, for the AIR compiler, builds once into NVK's tree for both lanes
    // (`xtask/src/gpu/llvm.rs`).
    let llvm = gpu.join("nvk");
    if lane != "nvk" {
        println!("cargo:rustc-link-search=native={}", llvm.display());
    }
    let command = format!("cargo xtask gpu {lane}");
    // Mesa links its Vulkan runtime and drivers whole: their entry points
    // meet in tables by weak reference, which pulls no member from an archive.
    for (name, kind, from) in [
        ("stela-vulkan", "static:+whole-archive", &directory),
        (driver, "static", &directory),
        ("stela-llvm", "static", &llvm),
    ] {
        let archive = from.join(format!("lib{name}.a"));
        assert!(
            archive.is_file(),
            "{} is missing: run `{command}`",
            archive.display()
        );
        println!("cargo:rerun-if-changed={}", archive.display());
        println!("cargo:rustc-link-lib={kind}={name}");
    }
    // What turns NIR into machine code: the C archives (the driver, NIR and
    // the Vulkan runtime), NVK's Rust NAK and NIL, with the toolchain. NVK's
    // own pipeline cache UUID is the same for every build
    // (`compat/mesa/build_id.c`), so this keys the pipeline store instead.
    let inputs: &[&str] = if nvk {
        &[
            ".",
            "../../target/gpu/nvk/libstela-nvk.a",
            "../../target/gpu/nvk/libstela-vulkan.a",
            "../../target/gpu/nvk/build/src/compiler/rust/libcompiler.rlib.p",
            "../../target/gpu/nvk/build/src/nouveau/compiler/latencies",
            "../../target/gpu/nvk/build/src/nouveau/compiler/nak_bindings.rs",
            "../../target/gpu/nvk/build/src/nouveau/headers/lib.rs",
            "../../target/gpu/nvk/build/src/nouveau/nil/nil_bindings.rs",
            "../../third_party/mesa/src/compiler/rust",
            "../../third_party/mesa/src/nouveau/compiler",
            "../../third_party/mesa/src/nouveau/headers/nv_push_rs",
            "../../third_party/mesa/src/nouveau/nil",
            "../../third_party/mesa/src/nouveau/rust",
            "../../rust-toolchain.toml",
        ]
    } else {
        &[
            ".",
            "../../target/gpu/radv/libstela-radv.a",
            "../../target/gpu/radv/libstela-vulkan.a",
            "../../rust-toolchain.toml",
        ]
    };
    stela_fingerprint::emit("STELA_MESA_COMPILER_SHA256", inputs);
}
