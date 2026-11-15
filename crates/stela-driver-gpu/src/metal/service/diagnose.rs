//! DIAGNOSTIC (vector glyphs on RADV): what the platform's log says about
//! the pipelines and passes of Apple's vector renderer (RenderBox), whose
//! path coverage — SF Symbols, template images — draws blank on RADV while
//! its analytic shapes and images draw.
//!
//! A RenderBox path accumulates its winding in the second channel of the
//! coverage attachment, color attachment 1, through the blend unit
//! (`path_*`, `mask_accumulate_fragment`); a later draw of the same pass
//! (`accumulator_*`, `mask_*`, `primitive_coverage_*`) reads that
//! attachment, by framebuffer fetch (a storage attachment here) or as a
//! texture, turns the winding into coverage and composites. Which of those
//! steps fails is the question these lines answer:
//!
//! - `DIAG VECTOR TARGETS`, joined to a pipeline's `PIPELINE` line: each
//!   attachment's pixel format, the Vulkan format it renders as, its Metal
//!   blend state and write mask and the blend unit's Vulkan write mask, the
//!   sample count and depth and stencil formats, then what the compiled
//!   fragment function reads and writes: the attachments it reads and those
//!   it accesses as storage, its early tests and its depth, stencil and
//!   sample mask outputs.
//! - `DIAG VECTOR DRAW`, at the first binding of such a pipeline in a pass of
//!   attachments of a new kind ([`DRAWS_NOTED`] in all): the pass's
//!   attachments' textures (format, size, samples, storage mode, usage),
//!   load and store actions, and whether each color attachment lies in a
//!   mirror of buffer rows (RADV's alone: its linear images take rows of
//!   multiples of 256 bytes) or is read through a copy.
//!
//! Remove once the glyphs draw.
use crate::metal::{
    device::{TextureId, queue::RenderPassRequest},
    pipeline::{MAX_COLOR_ATTACHMENTS, RenderPipelinePlan, RenderTargets},
    pixel_format::PixelFormat,
    resource::TextureDescriptor,
    shader::Interface,
};
use alloc::{format, string::String, vec::Vec};
use core::fmt::Write;

/// The entry name prefixes of RenderBox's render functions.
const VECTOR_PREFIXES: [&str; 11] = [
    "path_",
    "mask_",
    "primitive_",
    "accumulator_",
    "plane_",
    "alpha_effect_",
    "uber_",
    "stroke_",
    "filter_",
    "resolve_",
    "spill_",
];

/// The `DIAG VECTOR DRAW` lines a service logs at most.
pub(super) const DRAWS_NOTED: usize = 48;

/// Whether a render pipeline of these vertex and fragment entries is the
/// vector renderer's.
pub(super) fn vector_pipeline(vertex: &str, fragment: Option<&str>) -> bool {
    let ours = |entry: &str| {
        VECTOR_PREFIXES
            .iter()
            .any(|prefix| entry.starts_with(prefix))
    };
    ours(vertex) || fragment.is_some_and(ours)
}

/// `DIAG VECTOR TARGETS …` of a created render pipeline: its Metal targets,
/// how its plan renders them, and its compiled fragment function's
/// attachment accesses and outputs.
pub(super) fn targets(
    targets: &RenderTargets,
    plan: &RenderPipelinePlan,
    fragment: Option<&Interface>,
) -> String {
    let mut text = format!(
        "DIAG VECTOR TARGETS samples={} a2c={} raster={} tests_fragments={}",
        targets.raster_sample_count,
        targets.alpha_to_coverage_enabled,
        targets.rasterization_enabled,
        plan.tests_fragments,
    );
    for (name, format) in [
        ("depth", targets.depth_attachment_pixel_format),
        ("stencil", targets.stencil_attachment_pixel_format),
    ] {
        if let Some(format) = format {
            let _ = write!(text, " {name}={format:?}");
        }
    }
    for (index, attachment) in targets.color_attachments.iter().enumerate() {
        let Some(format) = attachment.pixel_format else {
            continue;
        };
        let vulkan = plan.info.color_formats.get(index).copied().flatten();
        let _ = write!(
            text,
            " c{index}={format:?}>{vulkan:?} mask={:#x}",
            attachment.write_mask.bits()
        );
        if attachment.blending_enabled {
            let _ = write!(
                text,
                " blend={:?}/{:?}/{:?},{:?}/{:?}/{:?}",
                attachment.source_rgb_blend_factor,
                attachment.destination_rgb_blend_factor,
                attachment.rgb_blend_operation,
                attachment.source_alpha_blend_factor,
                attachment.destination_alpha_blend_factor,
                attachment.alpha_blend_operation,
            );
        }
        if let Some(blend) = plan.info.color_blend.get(index) {
            let _ = write!(text, " unit={:#x}", blend.write_mask.bits());
        }
    }
    if let Some(fragment) = fragment {
        let outputs: Vec<u32> = fragment.color_outputs.iter().map(|o| o.index).collect();
        let _ = write!(
            text,
            " | fs outputs={outputs:?} reads={:#x} storage={:#x} early={} depth_out={} \
             stencil_out={} mask_out={} discards={}",
            fragment.attachment_reads,
            fragment.storage_attachments,
            fragment.early_fragment_tests,
            fragment.writes_depth,
            fragment.writes_stencil,
            fragment.writes_sample_mask,
            fragment.discards,
        );
    }
    text
}

/// What a pass's attachments are, by kind: the formats, sample counts and
/// storage modes of its textures, which says whether a draw in it shows a
/// new case.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(super) struct PassKind {
    color: [Option<(PixelFormat, u64, u64)>; MAX_COLOR_ATTACHMENTS],
    depth: Option<PixelFormat>,
    stencil: Option<PixelFormat>,
}

/// The vector draws a service has logged and the pass it encodes now.
#[derive(Default)]
pub(super) struct VectorDraws {
    /// The pass encoding now, kept while lines remain to log.
    pub(super) pass: Option<RenderPassRequest>,
    /// (pipeline, pass kind) of each line logged.
    seen: Vec<(u32, PassKind)>,
}

impl VectorDraws {
    /// Whether lines remain to log.
    pub(super) fn open(&self) -> bool {
        self.seen.len() < DRAWS_NOTED
    }

    /// Whether `pipeline` drawing in a pass of `kind` makes a line not
    /// logged yet, while lines remain.
    pub(super) fn is_new(&self, pipeline: u32, kind: PassKind) -> bool {
        self.open() && !self.seen.contains(&(pipeline, kind))
    }

    /// Records the line of `pipeline` drawing in a pass of `kind`; once the
    /// last is logged, no pass is kept.
    pub(super) fn record(&mut self, pipeline: u32, kind: PassKind) {
        self.seen.push((pipeline, kind));
        if !self.open() {
            self.pass = None;
        }
    }
}

/// The kind of `pass`, its textures described by `describe`.
pub(super) fn pass_kind(
    pass: &RenderPassRequest,
    describe: impl Fn(TextureId) -> Option<TextureDescriptor>,
) -> PassKind {
    let mut kind = PassKind::default();
    for (slot, attachment) in kind.color.iter_mut().zip(&pass.color_attachments) {
        *slot = attachment
            .as_ref()
            .and_then(|attachment| describe(attachment.texture))
            .map(|d| (d.pixel_format, d.sample_count, d.options.storage_mode.raw()));
    }
    kind.depth = pass
        .depth_attachment
        .as_ref()
        .and_then(|attachment| describe(attachment.texture))
        .map(|d| d.pixel_format);
    kind.stencil = pass
        .stencil_attachment
        .as_ref()
        .and_then(|attachment| describe(attachment.texture))
        .map(|d| d.pixel_format);
    kind
}

/// A texture for the log: format, size, samples, storage mode and usage.
pub(super) fn texture(descriptor: Option<TextureDescriptor>) -> String {
    descriptor.map_or_else(
        || String::from("?"),
        |d| {
            format!(
                "{:?} {}x{}x{} samples={} storage={:?} usage={:?}",
                d.pixel_format,
                d.width,
                d.height,
                d.depth,
                d.sample_count,
                d.options.storage_mode,
                d.usage
            )
        },
    )
}

/// `DIAG VECTOR DRAW …`: the pipeline's entries and its pass's attachments,
/// each color attachment's `routes` (whether it lies in a mirror of buffer
/// rows, and whether framebuffer reads go through a copy of it) marked
/// `mirror` and `copy`.
pub(super) fn draw(
    vertex: &str,
    fragment: Option<&str>,
    storage: u32,
    pass: &RenderPassRequest,
    describe: impl Fn(TextureId) -> Option<TextureDescriptor>,
    routes: impl Fn(TextureId) -> (bool, bool),
) -> String {
    let mut text = format!(
        "DIAG VECTOR DRAW vertex={vertex} fragment={} storage={storage:#x}",
        fragment.unwrap_or("-")
    );
    for (index, attachment) in pass.color_attachments.iter().enumerate() {
        let Some(attachment) = attachment else {
            continue;
        };
        let _ = write!(
            text,
            " c{index}=[{} load={:?} store={:?}",
            texture(describe(attachment.texture)),
            attachment.load_action,
            attachment.store_action
        );
        let (mirrored, copied) = routes(attachment.texture);
        for (flag, name) in [(mirrored, " mirror"), (copied, " copy")] {
            if flag {
                text.push_str(name);
            }
        }
        text.push(']');
    }
    for (name, attachment) in [
        (
            "depth",
            pass.depth_attachment
                .as_ref()
                .map(|a| (a.texture, a.load_action, a.store_action)),
        ),
        (
            "stencil",
            pass.stencil_attachment
                .as_ref()
                .map(|a| (a.texture, a.load_action, a.store_action)),
        ),
    ] {
        if let Some((id, load, store)) = attachment {
            let _ = write!(
                text,
                " {name}=[{} load={load:?} store={store:?}]",
                texture(describe(id))
            );
        }
    }
    text
}
