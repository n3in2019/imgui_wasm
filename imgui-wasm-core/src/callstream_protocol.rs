//! Wire framing for the call-stream transport.
//!
//! Message types (all little-endian; first byte is the type):
//!
//! Server -> client:
//!   0x07  Call-stream frame.
//!         header (6 x f32: dpx,dpy,dsw,dsh,fbsx,fbsy)
//!         frame_id (u32, monotonic per server)
//!         call_count (u32)
//!         <call bytes>   // call_count calls, each: opcode u16 + args per schema
//!
//!   0x08  LZ4-compressed call-stream frame.
//!         uncompressed_len (u32)
//!         <lz4 block bytes of a 0x07 payload (without the 0x08 type byte)>
//!         Mirrors the existing 0x03/0x05 layout so the frontend's existing
//!         lz4Decompress (imgui_wasm.js) can decode it.
//!
//!   0x09  String-table update.
//!         count (u32)
//!         count x { id (u32), len (u32), <utf-8 bytes> }
//!         Assigns the stable interned ids the server has minted since the
//!         last update. The client MUST apply a 0x09 before decoding any 0x07
//!         that references a new id.
//!
//! Client -> server: unchanged (0x10..0x17 input, 0x18 clipboard).
//!
//! The frame body (call bytes) is produced verbatim by the capture layer
//! (src/capture.rs); this module only adds the frame envelope. Compression
//! and per-client delta logic live in state.rs::send_frame, mirroring the
//! draw-data path.

use crate::capture;

/// Frame header: display pos/size/scale, identical layout to the draw-data
/// path so the client's projection math is shared.
#[derive(Clone, Copy)]
pub struct FrameHeader {
    pub dpx: f32,
    pub dpy: f32,
    pub dsw: f32,
    pub dsh: f32,
    pub fbsx: f32,
    pub fbsy: f32,
}

impl FrameHeader {
    fn write(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&self.dpx.to_le_bytes());
        out.extend_from_slice(&self.dpy.to_le_bytes());
        out.extend_from_slice(&self.dsw.to_le_bytes());
        out.extend_from_slice(&self.dsh.to_le_bytes());
        out.extend_from_slice(&self.fbsx.to_le_bytes());
        out.extend_from_slice(&self.fbsy.to_le_bytes());
    }
}

/// Build a `0x07` call-stream frame from already-captured call bytes.
///
/// `call_bytes` is the raw captured stream (opcodes + args, as written by the
/// generated host wrappers via capture.rs). `frame_id` is the monotonic frame
/// counter the caller maintains. `call_count` is the number of calls in the
/// buffer; if the caller does not track it, pass `estimate_call_count`'s
/// result (a best-effort count is acceptable because the client also stops at
/// buffer end; the count is an optimization to pre-allocate, not a correctness
/// boundary, since the byte stream is self-framing by opcode).
pub fn serialize_callstream_frame(
    header: FrameHeader,
    frame_id: u32,
    call_bytes: &[u8],
    call_count: u32,
) -> Vec<u8> {
    let mut out = Vec::with_capacity(1 + 24 + 4 + 4 + call_bytes.len());
    out.push(0x07);
    header.write(&mut out);
    out.extend_from_slice(&frame_id.to_le_bytes());
    out.extend_from_slice(&call_count.to_le_bytes());
    out.extend_from_slice(call_bytes);
    out
}

/// Build a `0x09` string-table update from newly-interned strings.
/// Each entry assigns a stable id. The server sends one 0x09 per frame
/// carrying any new strings (often empty after the first few frames).
pub fn serialize_string_update(new_strings: &[(u32, Vec<u8>)]) -> Vec<u8> {
    let total_bytes: usize = new_strings.iter().map(|(_, b)| b.len()).sum();
    let mut out = Vec::with_capacity(1 + 4 + new_strings.len() * 8 + total_bytes);
    out.push(0x09);
    out.extend_from_slice(&(new_strings.len() as u32).to_le_bytes());
    for (id, bytes) in new_strings {
        out.extend_from_slice(&id.to_le_bytes());
        out.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
        out.extend_from_slice(bytes);
    }
    out
}

/// Remap a serialized 0x07 payload into a 0x08 (LZ4-compressed) payload,
/// mirroring the draw-data remap in state.rs::send_frame. Returns the
/// compressed payload, or None if compression did not help (caller sends the
/// original 0x07). Mirrors `lz4_flex::compress` + the +5 byte envelope.
pub fn maybe_compress_callstream(payload_0x07: &[u8]) -> Option<Vec<u8>> {
    // payload_0x07[0] == 0x07; compress payload_0x07[1..].
    debug_assert_eq!(payload_0x07.first(), Some(&0x07));
    let body = &payload_0x07[1..];
    let compressed = lz4_compress(body);
    if compressed.len() + 5 < body.len() {
        let mut out = Vec::with_capacity(5 + compressed.len());
        out.push(0x08);
        out.extend_from_slice(&(body.len() as u32).to_le_bytes());
        out.extend_from_slice(&compressed);
        Some(out)
    } else {
        None
    }
}

/// FNV-1a 32 over the call bytes + header, for the "identical frame
/// skip" optimization (mirrors the draw-data skip in state.rs). Cheaper to
/// hash here because the call buffer is much smaller than vertex data.
pub fn frame_hash(header: FrameHeader, call_bytes: &[u8]) -> u32 {
    let mut h: u32 = 0x811c9dc5;
    let mut mix = |bytes: &[u8]| {
        for &b in bytes {
            h ^= b as u32;
            h = h.wrapping_mul(0x0100_0193);
        }
    };
    mix(&header.dpx.to_le_bytes());
    mix(&header.dpy.to_le_bytes());
    mix(&header.dsw.to_le_bytes());
    mix(&header.dsh.to_le_bytes());
    mix(&header.fbsx.to_le_bytes());
    mix(&header.fbsy.to_le_bytes());
    mix(call_bytes);
    h
}

// --- LZ4 block compression -----------------------------------------------
// We depend on the same lz4_flex crate the draw-data path uses (state.rs).
// Re-exported here as a thin wrapper so all compression logic is in one place.

fn lz4_compress(input: &[u8]) -> Vec<u8> {
    lz4_flex::compress(input)
}

// --- Convenience: drain capture + strings into a ready-to-send frame -----

/// Pull this frame's captured calls + any new interned strings, returning:
///   - the 0x07 call-stream frame payload (with the given header/frame_id),
///   - the 0x09 string-update payload (empty payload has count 0; the caller
///     may skip sending it entirely).
///
/// The caller (state.rs) is responsible for per-client hash-skip, compression,
/// and broadcast.
pub fn drain_frame(header: FrameHeader, frame_id: u32) -> (Vec<u8>, Vec<u8>) {
    let call_bytes = capture::frame_drain();
    let new_strings = capture::drain_new_strings();
    let call_count = estimate_call_count(&call_bytes);
    let frame = serialize_callstream_frame(header, frame_id, &call_bytes, call_count);
    let strings = serialize_string_update(&new_strings);
    (frame, strings)
}

/// Best-effort call count by scanning opcodes. Not strictly needed for
/// decoding (the byte stream is self-framing: the client reads opcode then
/// schema-driven args), but gives the client a pre-allocate hint and lets it
/// detect truncation. If the buffer is malformed this returns 0; the client
/// still decodes what it can.
fn estimate_call_count(_call_bytes: &[u8]) -> u32 {
    // A precise count requires the arg schema per opcode (the generator emits
    // it; importing it into Rust at runtime would mean embedding the JSON).
    // For now we return 0 = "unknown; decode until buffer end". This is
    // correct because the wire is self-framing. A later optimization can
    // embed the per-opcode arg-size table for fixed-arg opcodes.
    0
}
