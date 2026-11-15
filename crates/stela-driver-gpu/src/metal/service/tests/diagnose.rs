//! DIAGNOSTIC (vector glyphs): the log lines of the vector renderer's
//! pipelines and draws.
use super::super::diagnose::{self, DRAWS_NOTED, VectorDraws};
use crate::metal::abi::PrimitiveTopologyClass;
use crate::metal::{
    abi::{
        BlendFactor, BlendOperation, ColorWriteMask, CpuCacheMode, HazardTrackingMode, LoadAction,
        MultisampleDepthResolveFilter, MultisampleStencilResolveFilter, ResourceOptions,
        StorageMode, StoreAction, StoreActionOptions, SwizzleChannels, TextureType, TextureUsage,
        VisibilityResultType,
    },
    device::{
        TextureId,
        queue::{AttachmentRequest, RenderPassRequest},
    },
    nvidia::fixture::capabilities,
    pipeline::{
        ColorAttachmentDescriptor, RenderPipelineDescriptor, RenderTargets, render_pipeline,
    },
    pixel_format::PixelFormat,
    resource::TextureDescriptor,
    shader::{ColorOutput, CompiledShader, NumericClass, Provenance, Stage, tests::interface},
    vulkan::object::ShaderCode,
};

#[test]
fn the_vector_renderer_s_pipelines_are_told_apart_by_their_entries() {
    assert!(diagnose::vector_pipeline(
        "path_interior_vertex",
        Some("path_interior_fragment")
    ));
    assert!(diagnose::vector_pipeline(
        "plane__vertex",
        Some("accumulator_color_fragment")
    ));
    assert!(diagnose::vector_pipeline("mask__vertex", None));
    // Core Animation's are not.
    assert!(!diagnose::vector_pipeline(
        "fixed_vert_lph_spc",
        Some("fixed_frag_lph_cpf")
    ));
}

/// A path pipeline as RenderBox makes one: the destination unwritten, the
/// coverage attachment 1 summing the winding the function writes there.
fn path_targets() -> RenderTargets {
    let mut targets = RenderTargets::default();
    targets.color_attachments[0] = ColorAttachmentDescriptor {
        pixel_format: Some(PixelFormat::Bgra8Unorm),
        write_mask: ColorWriteMask::EMPTY,
        ..ColorAttachmentDescriptor::default()
    };
    targets.color_attachments[1] = ColorAttachmentDescriptor {
        pixel_format: Some(PixelFormat::Rg16Float),
        blending_enabled: true,
        source_rgb_blend_factor: BlendFactor::One,
        destination_rgb_blend_factor: BlendFactor::One,
        rgb_blend_operation: BlendOperation::Add,
        source_alpha_blend_factor: BlendFactor::One,
        destination_alpha_blend_factor: BlendFactor::One,
        alpha_blend_operation: BlendOperation::Add,
        write_mask: ColorWriteMask::KNOWN,
    };
    targets
}

#[test]
fn a_vector_pipeline_s_line_names_its_targets_and_fragment_accesses() {
    let capabilities = capabilities();
    let shader = |stage, code, edit: &dyn Fn(&mut crate::metal::shader::Interface)| {
        let mut interface = interface(stage);
        edit(&mut interface);
        CompiledShader {
            code: ShaderCode(code),
            interface,
            provenance: Provenance::default(),
        }
    };
    let vertex = shader(Stage::Vertex, 1, &|_| {});
    let fragment = shader(Stage::Fragment, 2, &|interface| {
        interface.color_outputs = alloc::vec![ColorOutput {
            index: 1,
            numeric: NumericClass::Float,
            dual_source: false,
        }];
        interface.early_fragment_tests = true;
    });
    let descriptor = RenderPipelineDescriptor {
        vertex_function: &vertex,
        fragment_function: Some(&fragment),
        vertex_descriptor: None,
        targets: path_targets(),
        input_primitive_topology: PrimitiveTopologyClass::Unspecified,
        max_vertex_amplification_count: 1,
        tessellation: None,
    };
    let plan = render_pipeline(&capabilities, &descriptor).unwrap();
    let text = diagnose::targets(&descriptor.targets, &plan, Some(&fragment.interface));
    assert!(text.starts_with("DIAG VECTOR TARGETS samples=1 "), "{text}");
    assert!(
        text.contains(" c0=Bgra8Unorm>Some(B8G8R8A8Unorm) mask=0x0 "),
        "{text}"
    );
    assert!(
        text.contains(" c1=Rg16Float>Some(R16G16Sfloat) mask=0xf blend=One/One/Add,One/One/Add "),
        "{text}"
    );
    assert!(
        text.contains("| fs outputs=[1] reads=0x0 storage=0x0 early=true depth_out=false"),
        "{text}"
    );
    // Without a fragment function, only the targets.
    let text = diagnose::targets(&descriptor.targets, &plan, None);
    assert!(!text.contains("| fs"), "{text}");
}

fn attachment(texture: u32) -> AttachmentRequest<[f64; 4]> {
    AttachmentRequest {
        texture: TextureId(texture),
        level: 0,
        slice: 0,
        depth_plane: 0,
        resolve: None,
        load_action: LoadAction::Clear,
        store_action: StoreAction::DontCare,
        store_action_options: StoreActionOptions::EMPTY,
        clear: [0.0; 4],
        surface: None,
    }
}

fn pass() -> RenderPassRequest {
    RenderPassRequest {
        color_attachments: [
            Some(attachment(1)),
            Some(attachment(2)),
            None,
            None,
            None,
            None,
            None,
            None,
        ],
        depth_attachment: None,
        depth_resolve_filter: MultisampleDepthResolveFilter::Sample0,
        stencil_attachment: None,
        stencil_resolve_filter: MultisampleStencilResolveFilter::Sample0,
        visibility_result_buffer: None,
        visibility_result_type: VisibilityResultType::Reset,
        render_target_array_length: 0,
        render_target_width: 64,
        render_target_height: 32,
        default_raster_sample_count: 1,
        tile_memory: Default::default(),
        sample_positions: alloc::vec::Vec::new(),
        rasterization_rate_map: false,
        counter_samples: alloc::vec::Vec::new(),
    }
}

fn texture(format: PixelFormat, storage_mode: StorageMode) -> TextureDescriptor {
    TextureDescriptor {
        texture_type: TextureType::D2,
        pixel_format: format,
        width: 64,
        height: 32,
        depth: 1,
        mipmap_level_count: 1,
        sample_count: 1,
        array_length: 1,
        options: ResourceOptions {
            cpu_cache_mode: CpuCacheMode::DefaultCache,
            storage_mode,
            hazard_tracking_mode: HazardTrackingMode::Default,
        },
        usage: TextureUsage::RENDER_TARGET,
        swizzle: SwizzleChannels::IDENTITY,
    }
}

#[test]
fn vector_draws_are_logged_once_per_pipeline_and_kind_of_pass() {
    let describe = |id: TextureId| match id.0 {
        1 => Some(texture(PixelFormat::Bgra8Unorm, StorageMode::Private)),
        2 => Some(texture(PixelFormat::Rg16Float, StorageMode::Memoryless)),
        _ => None,
    };
    let pass = pass();
    let routes = |id: TextureId| (id.0 == 1, false);
    let text = diagnose::draw(
        "plane__vertex",
        Some("accumulator_color_fragment"),
        3,
        &pass,
        describe,
        routes,
    );
    assert!(
        text.starts_with(
            "DIAG VECTOR DRAW vertex=plane__vertex fragment=accumulator_color_fragment storage=0x3 "
        ),
        "{text}"
    );
    assert!(
        text.contains(" c1=[Rg16Float 64x32x1 samples=1 storage=Memoryless "),
        "{text}"
    );
    assert!(text.contains(" load=Clear store=DontCare]"), "{text}");
    // The destination lies in a mirror of its rows; the coverage does not.
    assert!(
        text.contains(" load=Clear store=DontCare mirror] c1=["),
        "{text}"
    );

    let mut draws = VectorDraws::default();
    let kind = diagnose::pass_kind(&pass, describe);
    assert!(draws.is_new(7, kind));
    draws.record(7, kind);
    assert!(
        !draws.is_new(7, kind),
        "the same pipeline in the same kind of pass"
    );
    assert!(draws.is_new(8, kind), "another pipeline");
    let other = diagnose::pass_kind(&pass, |id| {
        describe(id).map(|mut d| {
            d.pixel_format = PixelFormat::Rgba16Float;
            d
        })
    });
    assert!(draws.is_new(7, other), "another kind of pass");
    // The lines are bounded, and once they are all logged no pass is kept.
    draws.pass = Some(pass);
    for pipeline in 100..100 + DRAWS_NOTED as u32 {
        draws.record(pipeline, kind);
    }
    assert!(!draws.open());
    assert!(!draws.is_new(8, kind));
    assert!(draws.pass.is_none());
}
