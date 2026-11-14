//! Opens the Metal service for one AMD GPU: admits its Vulkan device,
//! derives the platform facts Vulkan does not report, and serves a
//! [`Device`] over the backend and compiler. [`open`] composes the native
//! stack as the NVIDIA route does ([`nvidia::open::stack`]), with RADV's
//! entry point in place of NVK's, and says on the log what RADV reports and
//! how a first submission went, which is all a boot shows of a GPU without
//! display.
//!
//! [`nvidia::open::stack`]: crate::metal::nvidia::open::stack
use super::{Attachment, Chip, GcVersion, Refusal};
#[cfg(feature = "metal-vulkan")]
use crate::metal::{
    command_buffer::Segment,
    compiler::{self, AirCompiler},
    native::{Loader, MemoryExporter, NativeBackend, vk},
    nvidia::open::stack,
    vulkan::backend::Object,
};
use crate::metal::{
    device::Device,
    error::Error,
    service::{Service, identity::DeviceEpoch},
    shader::Compiler,
    vulkan::{
        MemoryHeapFlags,
        backend::Backend,
        physical::{PciBusInfo, PhysicalDevice},
    },
};
#[cfg(feature = "metal-vulkan")]
use alloc::boxed::Box;
use alloc::{format, string::String};

/// The GPU the platform hands the Metal device.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Gpu {
    pub pci: PciBusInfo,
    /// The graphics core amdgpu reports.
    pub gc: GcVersion,
    /// Ignored for an APU, whose graphics core has no link of its own.
    pub attachment: Attachment,
}

/// Why the Metal service did not open.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OpenError {
    /// The device is not an AMD GPU this route drives.
    Refused(Refusal),
    /// The device lacks a requirement or could not be created.
    Device(Error),
}

impl From<Error> for OpenError {
    fn from(error: Error) -> Self {
        Self::Device(error)
    }
}

/// Serves the Metal device over `backend`, which drives `gpu`, and
/// `compiler`, in the gate's `epoch` for it.
pub fn serve<B: Backend, C: Compiler>(
    backend: B,
    compiler: C,
    gpu: &Gpu,
    epoch: DeviceEpoch,
) -> Result<Service<B, C>, OpenError> {
    Ok(Service::new(device(backend, compiler, gpu)?, epoch))
}

/// The Metal device over `backend`, once it is admitted as `gpu`.
fn device<B: Backend, C: Compiler>(
    backend: B,
    compiler: C,
    gpu: &Gpu,
) -> Result<Device<B, C>, OpenError> {
    let identity = &backend.physical_device().identity;
    if identity.pci_bus != Some(gpu.pci) {
        return Err(Error::Invalid("the backend drives another GPU").into());
    }
    let chip = Chip::admit(identity, gpu.gc).map_err(OpenError::Refused)?;
    let platform = chip.platform(gpu.attachment);
    Ok(Device::new(backend, compiler, &platform)?)
}

/// What RADV reports of `physical` for the boot log: `RADV device …`.
pub fn describe(physical: &PhysicalDevice, gc: GcVersion) -> String {
    let identity = &physical.identity;
    let api = identity.api_version;
    let mut line = format!(
        "RADV device \"{}\" {:04x}:{:04x} gc={gc} {:?} vulkan={}.{}.{} subgroup={}..{} heaps:",
        identity.device_name,
        identity.vendor_id,
        identity.device_id,
        identity.device_type,
        api >> 22,
        (api >> 12) & 0x3ff,
        api & 0xfff,
        physical.limits.min_subgroup_size,
        physical.limits.max_subgroup_size,
    );
    for heap in &physical.memory.heaps {
        let local = if heap.flags.contains(MemoryHeapFlags::DEVICE_LOCAL) {
            "local"
        } else {
            "host"
        };
        line.push_str(&format!(" {}M/{local}", heap.size >> 20));
    }
    let visible = physical.memory.types.iter().any(|memory| {
        memory.properties.contains(
            crate::metal::vulkan::MemoryProperties::DEVICE_LOCAL
                | crate::metal::vulkan::MemoryProperties::HOST_VISIBLE,
        )
    });
    line.push_str(if visible {
        " host-visible-vram=yes"
    } else {
        " host-visible-vram=no"
    });
    line
}

/// How long the first submission may take to signal: a GPU whose rings do
/// not run never does, and the boot goes on without its Metal device.
#[cfg(feature = "metal-vulkan")]
const FIRST_SUBMISSION_NS: u64 = 2_000_000_000;

/// Submits one empty command buffer that signals a timeline semaphore and
/// waits for it: the first time amdgpu's command submission, its fence and
/// its interrupt run for RADV. Whether it signaled in time.
#[cfg(feature = "metal-vulkan")]
fn first_submission(backend: &mut NativeBackend) -> Result<bool, Error> {
    let semaphore = backend.create_semaphore(0)?;
    let segment = Segment {
        waits: alloc::vec::Vec::new(),
        binds: alloc::vec::Vec::new(),
        commands: alloc::vec::Vec::new(),
        signals: alloc::vec![(semaphore, 1)],
    };
    let signaled = backend
        .submit(core::slice::from_ref(&segment))
        .and_then(|()| backend.wait_semaphore(semaphore, 1, FIRST_SUBMISSION_NS));
    // A semaphore whose signal never came stays, as the GPU may yet write it.
    if matches!(signaled, Ok(true)) {
        backend.destroy(Object::Semaphore(semaphore));
    }
    signaled
}

/// The Metal service over RADV and the AIR compiler.
#[cfg(feature = "metal-vulkan")]
pub type NativeService = Service<NativeBackend, AirCompiler>;

/// Opens RADV's device for `gpu` and serves it with the AIR compiler service
/// `compiler` answers for that device. RADV's report of the device and the
/// first submission's result go to `loader.log`.
///
/// # Safety
///
/// `loader` is RADV's entry point and `compiler` answers the AIR compiler
/// service with RADV's NIR options for the device it is given, both
/// outliving the service.
#[cfg(feature = "metal-vulkan")]
pub unsafe fn open(
    loader: Loader,
    exporter: Box<dyn MemoryExporter>,
    compiler: impl FnOnce(vk::PhysicalDevice) -> compiler::Service,
    gpu: &Gpu,
    epoch: DeviceEpoch,
) -> Result<NativeService, OpenError> {
    // SAFETY: the caller's guarantees; the stack names no Vulkan driver.
    let (mut backend, compiler) = unsafe { stack(loader, gpu.pci, exporter, compiler) }?;
    (loader.log)(&describe(backend.physical_device(), gpu.gc));
    let failed = match first_submission(&mut backend) {
        Ok(true) => {
            (loader.log)("RADV first submission signaled");
            None
        }
        Ok(false) => {
            (loader.log)("RADV first submission did not signal within 2 s");
            Some(Error::Unsupported("a GPU whose first submission never signaled"))
        }
        Err(error) => {
            (loader.log)(&format!("RADV first submission failed: {error:?}"));
            Some(error)
        }
    };
    if let Some(error) = failed {
        // A GPU whose first work hung is one amdgpu resets, and releasing
        // the device closes RADV's file, whose close waits, unbounded, for
        // the VM work that reset holds back. The device stays unreleased.
        core::mem::forget(backend);
        return Err(error.into());
    }
    serve(backend, compiler, gpu, epoch)
}

/// Opens RADV's device for `gpu` as [`open`] does, for a service whose device
/// was lost ([`Service::with_reopen`]).
///
/// # Safety
///
/// As for [`open`].
#[cfg(feature = "metal-vulkan")]
pub unsafe fn reopen(
    loader: Loader,
    exporter: Box<dyn MemoryExporter>,
    compiler: impl FnOnce(vk::PhysicalDevice) -> compiler::Service,
    gpu: &Gpu,
) -> Result<Device<NativeBackend, AirCompiler>, Error> {
    // SAFETY: the caller's guarantees.
    let (backend, compiler) = unsafe { stack(loader, gpu.pci, exporter, compiler) }?;
    device(backend, compiler, gpu).map_err(|error| match error {
        OpenError::Device(error) => error,
        OpenError::Refused(_) => Error::Invalid("the GPU was refused as it reopened"),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::metal::{
        amd::PcieLink,
        device::fixture::{MockBackend, MockCompiler},
        vulkan::{DriverId, PhysicalDeviceType},
    };
    use core::num::NonZeroU64;

    const NAVI21: GcVersion = GcVersion::new(10, 3, 0);
    const PCI: PciBusInfo = PciBusInfo {
        domain: 0,
        bus: 3,
        device: 0,
        function: 0,
    };

    fn gpu(gc: GcVersion) -> Gpu {
        Gpu {
            pci: PCI,
            gc,
            attachment: Attachment {
                link: PcieLink {
                    speed: 4,
                    width: 16,
                },
                external: false,
                location_number: 0,
            },
        }
    }

    /// The fixture's device, as RADV names an RX 6800 XT. Its limits and
    /// features stay the fixture's: admission reads only the identity.
    fn radv() -> MockBackend {
        let mut backend = MockBackend::new();
        let identity = &mut backend.physical.as_mut().unwrap().identity;
        identity.vendor_id = super::super::VENDOR_ID;
        identity.device_id = 0x73bf;
        identity.driver_id = DriverId::MesaRadv;
        identity.device_type = PhysicalDeviceType::DiscreteGpu;
        identity.device_name = "AMD Radeon RX 6800 XT (RADV NAVI21)".into();
        identity.pci_bus = Some(PCI);
        backend
    }

    fn serve_on(
        backend: MockBackend,
        gpu: &Gpu,
    ) -> Result<Service<MockBackend, MockCompiler>, OpenError> {
        let epoch = DeviceEpoch::from_registered(NonZeroU64::MIN);
        serve(backend, MockCompiler::default(), gpu, epoch)
    }

    #[test]
    fn an_rx_6800_xt_is_served_in_its_slot() {
        let mut service = serve_on(radv(), &gpu(NAVI21)).unwrap();
        let capabilities = service.device().capabilities();
        assert_eq!(capabilities.architecture, "Navi 21");
        assert!(!capabilities.removable);
        assert_eq!(capabilities.location_number, 0);
        // PCIe 4.0 x16.
        assert_eq!(capabilities.max_transfer_rate, 31_506_000_000);
    }

    #[test]
    fn other_gpus_cores_and_drivers_are_refused() {
        let mut other = gpu(NAVI21);
        other.pci.bus = 4;
        assert_eq!(
            serve_on(radv(), &other).err(),
            Some(OpenError::Device(Error::Invalid(
                "the backend drives another GPU"
            )))
        );
        // An RX 5700 (Navi 10) has neither mesh shaders nor barycentrics.
        let navi10 = GcVersion::new(10, 1, 10);
        assert_eq!(
            serve_on(radv(), &gpu(navi10)).err(),
            Some(OpenError::Refused(Refusal::BeforeRdna2 { gc: navi10 }))
        );
        let mut nvk = radv();
        nvk.physical.as_mut().unwrap().identity.driver_id = DriverId::MesaNvk;
        assert_eq!(
            serve_on(nvk, &gpu(NAVI21)).err(),
            Some(OpenError::Refused(Refusal::NotRadv(DriverId::MesaNvk)))
        );
        let mut nvidia = radv();
        nvidia.physical.as_mut().unwrap().identity.vendor_id = 0x10de;
        assert_eq!(
            serve_on(nvidia, &gpu(NAVI21)).err(),
            Some(OpenError::Refused(Refusal::NotAmd { vendor_id: 0x10de }))
        );
        // Raphael is an APU: RADV reporting it discrete disagrees with amdgpu.
        let raphael = GcVersion::new(10, 3, 6);
        assert_eq!(
            serve_on(radv(), &gpu(raphael)).err(),
            Some(OpenError::Refused(Refusal::Placement {
                reported: PhysicalDeviceType::DiscreteGpu
            }))
        );
    }

    #[test]
    fn the_log_names_the_device_its_core_and_heaps() {
        let backend = radv();
        let line = describe(backend.physical.as_ref().unwrap(), NAVI21);
        assert!(line.starts_with(
            "RADV device \"AMD Radeon RX 6800 XT (RADV NAVI21)\" 1002:73bf gc=10.3.0 DiscreteGpu"
        ));
        assert!(line.contains(" heaps: "));
    }
}
