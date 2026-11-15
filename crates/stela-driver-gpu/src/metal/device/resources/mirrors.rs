//! Mirrors of buffer rows no linear image takes ([`Mirror`]), one for
//! every texture over the same rows.
//!
//! RADV's linear images take rows of multiples of 256 bytes, so the small
//! IOSurfaces Core Animation and the icon services render (rows of 128 or
//! 896 bytes) lie in no image: each such texture samples and renders an
//! optimal image instead. One process renders into its texture over the
//! surface and another samples its own texture over the same surface, so
//! every texture over the same rows, of any connection, shares one image,
//! the rows' mirror: the GPU's writes reach every reader through it with
//! the ordering of the device's one queue, which every connection submits
//! to, and its full barriers. Mirrors are refcounted by the textures that
//! use them, and freed once the last is destroyed and every command buffer
//! submitted until then completes ([`Scope::DEVICE`], as any connection's
//! may have copied them).
//!
//! The rows are the same rows when they are the same bytes: the guest's
//! pages an import names (each connection imports a surface's pages
//! itself, as memory of its own), or the same device memory, at the same
//! place. Their texels and the image are the same when the image's
//! creation and the copy between them are ([`MirrorKey`]).
//!
//! Copies between the rows and the image keep the guest's CPU coherent.
//! Before a command buffer, a mirror whose rows may have changed since the
//! image was filled from them is filled again; after one that may have
//! written its image, the image goes back to the rows, for the CPU and any
//! texture over the rows that lies in them. Nothing tells the device that
//! the guest's CPU wrote a surface (its seed comes only with the passes
//! that render to it, and `didModifyRange:` only with managed buffers), so
//! each commit compares a digest of the rows with the one the image was
//! last filled from ([`Backend::digest_memory`]), for rows up to
//! [`DIGESTED_BYTES`] and [`DIGEST_BUDGET`] of them a commit; larger rows,
//! and rows the budget leaves, are copied every commit, as are rows whose
//! digest is not known: after the image went back to them, or a known
//! write to them on the GPU (a copy into their buffer, an extended-range
//! texture's encode into its planes, another mirror's copy back over
//! them), which the command buffer follows with a copy into the image
//! itself. Rows a shader writes through a buffer's address are seen only
//! by the digest of a commit after that work completed. While a
//! command buffer whose image copy back is held for an event has not been
//! submitted, the rows are not copied into the image, which holds what its
//! submitted work wrote: the held copy back overwrites the rows anyway.
use super::{Device, TextureId};
use crate::metal::{
    device::pins::Scope,
    error::Result,
    hazard::FULL,
    pixel_format::PixelFormat,
    resource::{Mirror, PlaneLayout, TexturePlan},
    shader::Compiler,
    vulkan::{
        Access, PipelineStages,
        backend::{Backend, Object},
        command::{Barrier, BufferImageCopy, Command},
        object::{Buffer, Image, ImageInfo, Memory},
    },
};
use alloc::{collections::BTreeMap, format, vec::Vec};
use stela_contracts::gate::MemoryRange;

/// Rows of at most this many bytes are compared with a digest before each
/// commit; larger rows are copied into their image every commit.
pub(super) const DIGESTED_BYTES: u64 = 1 << 20;

/// The rows one commit digests at most; mirrors past it are copied.
pub(super) const DIGEST_BUDGET: u64 = 4 << 20;

/// Notices kept for the platform's log until a call takes them.
const NOTICES_KEPT: usize = 16;

/// Where bytes are: the guest's RAM at its addresses, which every import
/// of the same pages views, or device memory of the device's own.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Space {
    Guest,
    Device(Memory),
}

/// A run of bytes `start..end` in a [`Space`].
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Span {
    space: Space,
    start: u64,
    end: u64,
}

impl Span {
    fn overlaps(&self, other: &Span) -> bool {
        self.space == other.space && self.start < other.end && other.start < self.end
    }
}

fn overlap(spans: &[Span], others: &[Span]) -> bool {
    spans
        .iter()
        .any(|span| others.iter().any(|other| span.overlaps(other)))
}

/// What makes two textures' mirrors one: the same bytes, copied the same
/// way into the same image.
#[derive(Clone, Debug, PartialEq)]
struct MirrorKey {
    /// The rows' bytes, from the first row's first to the last's last.
    spans: Vec<Span>,
    image: ImageInfo,
    /// The copy, at offset zero of the rows.
    region: BufferImageCopy,
}

/// One texture's plane in a shared mirror.
#[derive(Clone, Copy, Debug)]
struct MirrorUser {
    texture: TextureId,
    /// Its buffer, at whose offset `mirror.region` copies, and the rows'
    /// first byte in the buffer's memory.
    buffer: Buffer,
    memory: Memory,
    start: u64,
    mirror: Mirror,
}

/// An image every texture over the same rows renders and samples, with its
/// memory.
#[derive(Debug)]
struct SharedMirror {
    key: MirrorKey,
    image: Image,
    memory: Memory,
    bytes: u64,
    /// The rows' length from the first row.
    span: u64,
    /// The textures that use it; commits copy through the first's buffer.
    users: Vec<MirrorUser>,
    /// The digest of the rows the image was last filled from, while the
    /// device knows nothing else wrote either since: `None` copies the rows
    /// into the image at the next commit.
    rows: Option<u64>,
    /// The newest command buffer that copies the image back to the rows.
    copy_back: Option<u64>,
}

/// A texture's plane's place in a shared mirror, which its image names.
#[derive(Clone, Copy, Debug)]
pub(super) struct MirrorSlot {
    pub index: u32,
    pub image: Image,
    /// Where the texture's rows lie in its buffer.
    pub mirror: Mirror,
    pub bytes: u64,
}

/// The device's mirrors and the guest pages of its imports.
#[derive(Debug, Default)]
pub(super) struct Mirrors {
    shared: super::Slab<SharedMirror>,
    /// The guest ranges each import views, in order.
    imports: BTreeMap<Memory, Vec<MemoryRange>>,
    report: MirrorReport,
}

/// What the mirrors hold and did since the device opened.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MirrorReport {
    /// Mirrors alive, and their bytes.
    pub live: usize,
    pub bytes: u64,
    /// Mirrors made, and textures that took one another texture made.
    pub created: u64,
    pub shared: u64,
    /// Copies of rows into images before commits, and those the rows'
    /// digest (or a held copy back) let commits skip.
    pub uploads: u64,
    pub skipped: u64,
    /// Copies of images back to their rows, and of rows into images after
    /// work that wrote the rows on the GPU.
    pub copy_backs: u64,
    pub refreshes: u64,
}

/// The copies [`Device::mirror_syncs`] answers for one command buffer, and
/// what the mirrors learn once it reaches the queue
/// ([`Device::apply_mirror_syncs`]).
#[derive(Debug, Default)]
pub(in crate::metal::device) struct MirrorSyncs {
    /// The textures whose mirrors it copies, whose destruction waits for it.
    pub textures: Vec<TextureId>,
    /// Copies of rows into images, ending in a barrier; empty when there
    /// are none.
    pub before: Vec<Command>,
    /// Copies of images back to their rows, then of rows into images,
    /// between barriers, the last making the rows the host's; empty when
    /// there are none.
    pub after: Vec<Command>,
    /// The mirrors filled from rows of each digest before the work.
    filled: Vec<(u32, Option<u64>)>,
    /// The mirrors whose rows or image the work may change.
    changed: Vec<u32>,
    /// The mirrors it copies back.
    copied_back: Vec<u32>,
    skipped: u64,
    refreshes: u64,
}

impl<B: Backend, C: Compiler> Device<B, C> {
    /// Notes the guest ranges an import views, which name its rows.
    pub(super) fn note_import(&mut self, memory: Memory, ranges: &[MemoryRange]) {
        self.resources
            .mirrors
            .imports
            .insert(memory, ranges.to_vec());
    }

    /// Forgets a freed import's ranges.
    pub(in crate::metal::device) fn forget_import(&mut self, memory: Memory) {
        self.resources.mirrors.imports.remove(&memory);
    }

    /// The bytes `offset..offset + length` of `memory` are: guest pages
    /// for an import, else the memory's own.
    fn spans(&self, memory: Memory, offset: u64, length: u64) -> Vec<Span> {
        let end = offset + length;
        let Some(ranges) = self.resources.mirrors.imports.get(&memory) else {
            return alloc::vec![Span {
                space: Space::Device(memory),
                start: offset,
                end,
            }];
        };
        let mut spans: Vec<Span> = Vec::new();
        let mut base = 0;
        for range in ranges {
            let (from, to) = (offset.max(base), end.min(base + range.bytes));
            if from < to {
                let start = range.address + (from - base);
                let span_end = range.address + (to - base);
                // Contiguous pages make one run.
                match spans.last_mut() {
                    Some(last) if last.end == start => last.end = span_end,
                    _ => spans.push(Span {
                        space: Space::Guest,
                        start,
                        end: span_end,
                    }),
                }
            }
            base += range.bytes;
        }
        spans
    }

    /// The mirror of `plan`'s image over rows at `start` of `memory`, as
    /// `mirror` copies them: another texture's over the same rows, or a
    /// new one, which is logged. Its texture uses it once
    /// [`Self::join_mirrors`] names it; [`Self::abandon_mirrors`] frees an
    /// unused one.
    pub(super) fn claim_mirror(
        &mut self,
        (memory, start): (Memory, u64),
        plan: &TexturePlan,
        mirror: Mirror,
        format: PixelFormat,
        layout: PlaneLayout,
    ) -> Result<MirrorSlot> {
        let key = MirrorKey {
            spans: self.spans(memory, start, mirror.span),
            image: plan.image.clone(),
            region: BufferImageCopy {
                buffer_offset: 0,
                ..mirror.region
            },
        };
        let mirrors = &mut self.resources.mirrors;
        let found = mirrors
            .shared
            .entries
            .iter()
            .position(|entry| entry.as_ref().is_some_and(|shared| shared.key == key));
        if let Some(index) = found {
            let shared = mirrors.shared.get(index as u32)?;
            mirrors.report.shared += 1;
            return Ok(MirrorSlot {
                index: index as u32,
                image: shared.image,
                mirror,
                bytes: shared.bytes,
            });
        }
        let (image, own, bytes) = self.backend.create_image(&plan.image, plan.memory)?;
        self.allocated += bytes;
        let [width, height, _] = plan.image.extent;
        let (rows, offsets) = self.capabilities.linear_texture_alignments();
        self.book_memory(own, bytes, || {
            format!("mirror of a {width}x{height} {format:?} texture's rows")
        });
        // Each new mirror is logged, up to a bound no call drains; textures
        // that share one are not.
        if self.notices.len() < NOTICES_KEPT {
            self.notices.push(format!(
                "texture {width}x{height} {format:?} at bytesPerRow {} offset {} mirrored: \
                 the device's linear images take rows of multiples of {rows} bytes at \
                 multiples of {offsets}; {} mirrors live",
                layout.bytes_per_row,
                layout.offset,
                self.resources.mirrors.report.live + 1,
            ));
        }
        let mirrors = &mut self.resources.mirrors;
        mirrors.report.created += 1;
        mirrors.report.live += 1;
        mirrors.report.bytes += bytes;
        let index = mirrors.shared.insert(SharedMirror {
            key,
            image,
            memory: own,
            bytes,
            span: mirror.span,
            users: Vec::new(),
            rows: None,
            copy_back: None,
        });
        Ok(MirrorSlot {
            index,
            image,
            mirror,
            bytes,
        })
    }

    /// Frees the mirrors of `slots` that no texture uses: those claimed for
    /// a texture that was not made.
    pub(super) fn abandon_mirrors(&mut self, slots: &[MirrorSlot]) {
        for slot in slots {
            let unused = self
                .resources
                .mirrors
                .shared
                .get(slot.index)
                .is_ok_and(|shared| shared.users.is_empty());
            if unused {
                self.free_shared_mirror(slot.index);
            }
        }
    }

    /// Makes texture `id`, just made over a buffer's rows, a user of the
    /// mirrors its planes claimed. The buffer's destruction then waits for
    /// every connection's work, which copies through it.
    pub(super) fn join_mirrors(&mut self, id: TextureId) -> Result<()> {
        let texture = self.resources.textures.get(id.0)?;
        if texture.mirrors.is_empty() {
            return Ok(());
        }
        let super::Backing::Buffer(buffer_id) = texture.backing else {
            return Ok(());
        };
        let slots = texture.mirrors.clone();
        let buffer = self.resources.buffers.get_mut(buffer_id.0)?;
        buffer.mirrored = true;
        let (vk_buffer, memory, memory_offset) =
            (buffer.buffer, buffer.memory, buffer.memory_offset);
        for slot in slots {
            let shared = self.resources.mirrors.shared.get_mut(slot.index)?;
            shared.users.push(MirrorUser {
                texture: id,
                buffer: vk_buffer,
                memory,
                start: memory_offset + slot.mirror.region.buffer_offset,
                mirror: slot.mirror,
            });
        }
        Ok(())
    }

    /// Texture `id`, destroyed, no longer uses the mirrors of `slots`; one
    /// no texture uses is freed once every command buffer submitted so far
    /// completes.
    pub(super) fn leave_mirrors(&mut self, id: TextureId, slots: &[MirrorSlot]) {
        for slot in slots {
            let Ok(shared) = self.resources.mirrors.shared.get_mut(slot.index) else {
                continue;
            };
            shared.users.retain(|user| user.texture != id);
            if shared.users.is_empty() {
                self.free_shared_mirror(slot.index);
            }
        }
    }

    fn free_shared_mirror(&mut self, index: u32) {
        let Ok(shared) = self.resources.mirrors.shared.remove(index) else {
            return;
        };
        let report = &mut self.resources.mirrors.report;
        report.live -= 1;
        report.bytes -= shared.bytes;
        // Any connection's command buffers may have copied or read it.
        self.release_for(Scope::DEVICE, |device| {
            device.defer(Object::Image(shared.image));
            device.release_memory(shared.memory, shared.bytes);
        });
    }

    /// The CPU wrote rows of `buffer` through the device: the next commit
    /// copies its mirrors' rows into their images.
    pub(super) fn mirror_rows_written(&mut self, buffer: Buffer) {
        for shared in self.resources.mirrors.shared.iter_mut() {
            if shared.users.iter().any(|user| user.buffer == buffer) {
                shared.rows = None;
            }
        }
    }

    /// What the mirrors hold and did.
    pub fn mirror_report(&self) -> MirrorReport {
        self.resources.mirrors.report
    }

    /// The copies that keep every mirror's image and rows the same around
    /// one command buffer, whose `work` passes attach the textures
    /// `rendered` (mirrored ones, and extended-range ones, which encode
    /// into their rows). Before the work, each image whose rows may have
    /// changed is filled from them; after it, each image the work may have
    /// written (a texture of it rendered to, copied or cleared into, or
    /// shader-writable, which nothing names) goes back to its rows, and
    /// each image whose rows the work may have written otherwise is filled
    /// from them again.
    pub(in crate::metal::device) fn mirror_syncs<'a>(
        &mut self,
        rendered: &[TextureId],
        work: impl Iterator<Item = &'a Command>,
    ) -> MirrorSyncs {
        let mut syncs = MirrorSyncs::default();
        if self.resources.mirrors.report.live == 0 {
            return syncs;
        }
        // What the work writes by name: images, and buffers.
        let mut images: Vec<Image> = Vec::new();
        let mut buffers: Vec<Buffer> = Vec::new();
        for command in work {
            match command {
                Command::CopyBufferToImage { dst, .. }
                | Command::CopyImage { dst, .. }
                | Command::BlitImage { dst, .. }
                | Command::ClearColorImage { image: dst, .. } => images.push(*dst),
                Command::CopyBuffer { dst, .. }
                | Command::CopyBufferRegions { dst, .. }
                | Command::CopyImageToBuffer { dst, .. }
                | Command::FillBuffer { buffer: dst, .. } => buffers.push(*dst),
                _ => {}
            }
        }
        // Rows extended-range passes may encode into: their buffers whole.
        let mut written: Vec<Span> = Vec::new();
        for &id in rendered {
            if !self.has_extended_planes(id) {
                continue;
            }
            let Ok(texture) = self.resources.textures.get(id.0) else {
                continue;
            };
            let super::Backing::Buffer(buffer) = texture.backing else {
                continue;
            };
            let Ok(buffer) = self.resources.buffers.get(buffer.0) else {
                continue;
            };
            let (memory, offset, length) = (buffer.memory, buffer.memory_offset, buffer.length);
            written.extend(self.spans(memory, offset, length));
        }
        let mut budget = DIGEST_BUDGET;
        let mut uploads = Vec::new();
        let mut copies = Vec::new();
        let mut refreshes = Vec::new();
        let Self {
            resources,
            backend,
            queue,
            ..
        } = self;
        let shared = &resources.mirrors.shared;
        let live = || {
            shared
                .entries
                .iter()
                .enumerate()
                .filter_map(|(index, entry)| Some((index as u32, entry.as_ref()?)))
                .filter(|(_, mirror)| !mirror.users.is_empty())
        };
        for (index, mirror) in live() {
            let first = mirror.users[0];
            let mut synced = false;
            // Before: the rows, unless the image holds them or a held copy
            // back will overwrite them.
            if mirror.copy_back.is_some_and(|serial| queue.holds(serial)) {
                syncs.skipped += 1;
            } else {
                let digest = if mirror.span <= DIGESTED_BYTES && mirror.span <= budget {
                    budget -= mirror.span;
                    backend
                        .digest_memory(first.memory, first.start, mirror.span)
                        .ok()
                } else {
                    None
                };
                if digest.is_some() && digest == mirror.rows {
                    syncs.skipped += 1;
                } else {
                    uploads.push(Command::CopyBufferToImage {
                        src: first.buffer,
                        dst: mirror.image,
                        region: first.mirror.region,
                    });
                    syncs.filled.push((index, digest));
                    synced = true;
                }
            }
            // After: the image, if the work may have written it.
            let writes = images.contains(&mirror.image)
                || mirror.users.iter().any(|user| {
                    user.mirror.shader_writes
                        || user.mirror.writes && rendered.contains(&user.texture)
                });
            if writes {
                copies.push(Command::CopyImageToBuffer {
                    src: mirror.image,
                    dst: first.buffer,
                    region: first.mirror.region,
                });
                syncs.copied_back.push(index);
                syncs.changed.push(index);
                synced = true;
            }
            if synced {
                syncs
                    .textures
                    .extend(mirror.users.iter().map(|user| user.texture));
            }
        }
        // Rows the work wrote on the GPU otherwise, or another mirror's
        // copy back writes, go into images the work did not write.
        for &index in &syncs.copied_back {
            if let Ok(mirror) = shared.get(index) {
                written.extend_from_slice(&mirror.key.spans);
            }
        }
        for (index, mirror) in live() {
            if syncs.copied_back.contains(&index) {
                continue;
            }
            let into_buffer = mirror
                .users
                .iter()
                .any(|user| buffers.contains(&user.buffer));
            if !into_buffer && !overlap(&mirror.key.spans, &written) {
                continue;
            }
            let first = mirror.users[0];
            refreshes.push(Command::CopyBufferToImage {
                src: first.buffer,
                dst: mirror.image,
                region: first.mirror.region,
            });
            syncs.changed.push(index);
            syncs.refreshes += 1;
            syncs
                .textures
                .extend(mirror.users.iter().map(|user| user.texture));
        }
        syncs.textures.sort_unstable();
        syncs.textures.dedup();
        if !uploads.is_empty() {
            syncs.before = uploads;
            syncs.before.push(Command::Barrier(FULL));
        }
        if !copies.is_empty() || !refreshes.is_empty() {
            syncs.after.push(Command::Barrier(FULL));
            if !copies.is_empty() && !refreshes.is_empty() {
                copies.push(Command::Barrier(FULL));
            }
            syncs.after.extend(copies);
            syncs.after.extend(refreshes);
            // The host reads the rows once the command buffer completes.
            syncs.after.push(Command::Barrier(Barrier {
                dst_stages: FULL.dst_stages.union(PipelineStages::HOST),
                dst_access: FULL.dst_access.union(Access::HOST_READ),
                ..FULL
            }));
        }
        syncs
    }

    /// What command buffer `serial`'s mirror copies, now on the queue,
    /// leave: images filled from rows of their digests, and rows or images
    /// its work may change, whose digest the next commit does not trust.
    pub(in crate::metal::device) fn apply_mirror_syncs(
        &mut self,
        serial: u64,
        syncs: &MirrorSyncs,
    ) {
        let mirrors = &mut self.resources.mirrors;
        for &(index, digest) in &syncs.filled {
            if let Ok(mirror) = mirrors.shared.get_mut(index) {
                mirror.rows = digest;
            }
        }
        for &index in &syncs.changed {
            if let Ok(mirror) = mirrors.shared.get_mut(index) {
                mirror.rows = None;
            }
        }
        for &index in &syncs.copied_back {
            if let Ok(mirror) = mirrors.shared.get_mut(index) {
                mirror.copy_back = Some(serial);
            }
        }
        let report = &mut mirrors.report;
        report.uploads += syncs.filled.len() as u64;
        report.skipped += syncs.skipped;
        report.copy_backs += syncs.copied_back.len() as u64;
        report.refreshes += syncs.refreshes;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use crate::metal::vulkan::backend::RowsDigest;

    fn digest(bytes: &[u8]) -> u64 {
        let mut digest = RowsDigest::new();
        digest.update(bytes);
        digest.finish()
    }

    #[test]
    fn a_digest_changes_with_any_word_and_reads_in_pieces_alike() {
        let rows: Vec<u8> = (0..4100u32).map(|at| (at * 7) as u8).collect();
        let whole = digest(&rows);
        let mut pieces = RowsDigest::new();
        pieces.update(&rows[..4096]);
        pieces.update(&rows[4096..]);
        assert_eq!(pieces.finish(), whole);
        for at in [0, 9, 31, 32, 4095, 4099] {
            let mut changed = rows.clone();
            changed[at] ^= 0x40;
            assert_ne!(digest(&changed), whole, "byte {at}");
        }
        assert_ne!(digest(&rows[..4099]), whole);
        for length in [0, 31, 32, 4099, 4100] {
            let mut read = RowsDigest::new();
            // SAFETY: `rows` holds at least `length` bytes.
            unsafe { read.update_from(rows.as_ptr(), length) };
            assert_eq!(read.finish(), digest(&rows[..length]), "{length} bytes");
        }
        assert_ne!(digest(&[0; 64]), digest(&[0; 96]));
    }

    #[test]
    fn guest_spans_join_contiguous_pages() {
        let spans = [Span {
            space: Space::Guest,
            start: 0x1000,
            end: 0x3000,
        }];
        let other = Span {
            space: Space::Guest,
            start: 0x2fff,
            end: 0x4000,
        };
        assert!(overlap(&spans, &[other]));
        let device = Span {
            space: Space::Device(Memory(1)),
            ..other
        };
        assert!(!overlap(&spans, &[device]));
    }
}
