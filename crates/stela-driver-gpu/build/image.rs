//! The canonical registration image of the GPU drivers' Linux environment:
//! the shared cohort, DRM's units and each port's registration unit, in their
//! global order, which `gpu::linux::start` runs. Each port compiles its unit
//! here and keeps its object in its own archive; on UEFI, where the image
//! runs, the ports' archives link beside the image.
use super::native::{self, Linux};
use std::{env, path::Path};
use stela_c_build::linux_capture::{self as initcalls, Captured, Projection, Unit, UnitSpec};

/// What a port compiled: its archive and its captured registration unit.
pub struct Port {
    pub archive: &'static str,
    pub spec: UnitSpec,
    pub unit: Captured,
}

/// A port's registration unit, compiled with Linux's registration macros
/// captured, from the port's own build.
pub fn capture(build: &cc::Build, spec: UnitSpec) -> Captured {
    let compiler = initcalls::compiler(build);
    let placement = initcalls::Placement::new(
        &Unit::from(spec).path(),
        &stela_c_build::linux_build::out_dir(),
        &[
            Path::new("../.."),
            compiler.parent().expect("compiler root"),
        ],
    )
    .expect("port source placement");
    initcalls::compile(
        build,
        Path::new("../stela-linux/src/compat/stela/registration.h"),
        spec,
        &placement,
    )
}

/// Compose the image over the environment's units and the ports', in the
/// ports' order, and link it with the ports on UEFI.
pub fn link(linux: &mut Linux, ports: Vec<Port>) {
    let mut captured = linux.take_captured();
    let environment_count = captured.len();
    let mut expected = native::environment_units();
    let mut archives = Vec::new();
    for port in ports {
        expected.push(port.spec);
        captured.push(port.unit);
        archives.push(port.archive);
    }
    let image = initcalls::source(
        &captured,
        Projection {
            symbol: "stela_linux_gpu_image",
            expected: &expected,
            imported_units: environment_count,
            descriptors_only: false,
        },
    );
    let mut build = stela_c_build::native::configure();
    linux.environment.apply(&mut build, None, &[], None);
    stela_c_build::linux_build::upstream_warnings(&mut build);
    build
        .opt_level(2)
        .cargo_metadata(false)
        .file(image)
        .compile("stela-linux-gpu-image");
    let output = stela_c_build::linux_build::out_dir();
    let digest = initcalls::write(
        &output.join("captured-units.bin"),
        &captured[initcalls::COMMON.len()..],
    );
    println!("cargo:output={}", output.display());
    println!("cargo:cohort_sha256={digest}");
    // Hosted programs test the environment and link no driver.
    if env::var("CARGO_CFG_TARGET_OS").is_ok_and(|os| os == "uefi") {
        println!("cargo:rustc-link-lib=static=stela-linux-gpu-image");
        for archive in archives {
            println!("cargo:rustc-link-lib=static={archive}");
        }
    }
}
