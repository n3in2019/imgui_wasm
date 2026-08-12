use std::collections::HashMap;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc::{Receiver, Sender, TryRecvError};
use std::sync::Mutex;

use tokio::sync::broadcast;
use tokio::sync::mpsc::{unbounded_channel, UnboundedReceiver, UnboundedSender};

pub type ClientId = u32;

#[derive(Debug, Clone)]
pub struct InputEvent {
    pub ev_type: i32,
    pub x: f32,
    pub y: f32,
    pub button: i32,
    pub key: i32,
    pub character: u32,
    pub wheel_x: f32,
    pub wheel_y: f32,
    pub display_w: f32,
    pub display_h: f32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_batched_pointer_click_in_order() {
        let id = 42u32;
        let mut batch = vec![0x19];
        batch.extend_from_slice(&id.to_le_bytes());
        batch.extend_from_slice(&2u16.to_le_bytes());

        let mut position = Vec::new();
        position.extend_from_slice(&12.5f32.to_le_bytes());
        position.extend_from_slice(&34.0f32.to_le_bytes());
        batch.push(0x10);
        batch.extend_from_slice(&(position.len() as u16).to_le_bytes());
        batch.extend_from_slice(&position);

        batch.push(0x11);
        batch.extend_from_slice(&1u16.to_le_bytes());
        batch.push(0);

        let messages = parse_client_msgs(&batch).unwrap();
        assert_eq!(messages.len(), 2);
        assert_eq!(messages[0].0, id);
        match &messages[0].1 {
            ClientMsg::Input(event) => {
                assert_eq!(event.ev_type, 0);
                assert_eq!((event.x, event.y), (12.5, 34.0));
            }
            _ => panic!("expected pointer position"),
        }
        match &messages[1].1 {
            ClientMsg::Input(event) => {
                assert_eq!(event.ev_type, 1);
                assert_eq!(event.button, 0);
            }
            _ => panic!("expected pointer down"),
        }
    }

    #[test]
    fn rejects_truncated_or_trailing_batch_data() {
        let batch = [0x19, 1, 0, 0, 0, 1, 0, 0x11, 1, 0];
        assert!(parse_client_msgs(&batch).is_none());

        let mut valid = batch.to_vec();
        valid.push(0);
        valid.push(0xff);
        assert!(parse_client_msgs(&valid).is_none());
    }

    #[test]
    fn resize_selects_the_active_client() {
        let (state, _) = ImGuiWasmState::new();
        let (first, _) = state.add_client();
        let (second, _) = state.add_client();
        state.set_display_size(second, 800.0, 437.0);
        assert_eq!(state.get_display_size(), [800.0, 437.0]);
        state.set_display_size(first, 1024.0, 768.0);
        assert_eq!(state.get_display_size(), [1024.0, 768.0]);
    }

    #[test]
    fn clipboard_write_is_sent_only_to_active_client() {
        let (state, _) = ImGuiWasmState::new();
        let (_first, mut first_rx) = state.add_client();
        let (second, mut second_rx) = state.add_client();
        state.set_display_size(second, 800.0, 600.0);

        state.set_clipboard_text("private");

        assert!(first_rx.try_recv().is_err());
        assert_eq!(
            second_rx.try_recv().unwrap(),
            make_clipboard_write_msg("private")
        );
    }

    #[test]
    fn draw_frames_are_delivered_once_per_client() {
        let (state, _) = ImGuiWasmState::new();
        let (_first, mut first_rx) = state.add_client();
        let (_second, mut second_rx) = state.add_client();
        assert!(state.begin_frame(0.0, 0.0, 640.0, 480.0, 1.0, 1.0));

        state.end_frame();

        assert_eq!(first_rx.try_recv().unwrap()[0], 0x01);
        assert!(first_rx.try_recv().is_err());
        assert_eq!(second_rx.try_recv().unwrap()[0], 0x01);
        assert!(second_rx.try_recv().is_err());
    }
}

pub enum ClientMsg {
    Input(InputEvent),
    Resize { w: f32, h: f32 },
    ClipboardText(String),
}

pub struct ClientState {
    pub display_size: [f32; 2],
    pub delta_state: DeltaState,
    pub force_frames: usize,
    pub clipboard_text: String,
    sender: UnboundedSender<Vec<u8>>,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct DrawCmd {
    pub clip_rect: [f32; 4],
    pub texture_id: u64,
    pub vtx_offset: u32,
    pub idx_offset: u32,
    pub elem_count: u32,
}

pub struct DrawList {
    pub vtx: Vec<u8>,
    pub idx: Vec<u8>,
    pub cmds: Vec<DrawCmd>,
    pub hash: u32,
}

pub struct FrameBuilder {
    pub dpx: f32,
    pub dpy: f32,
    pub dsw: f32,
    pub dsh: f32,
    pub fbsx: f32,
    pub fbsy: f32,
    pub lists: Vec<DrawList>,
}

pub struct DeltaState {
    pub last_frame_hash: u32,
    pub last_list_hashes: Vec<u32>,
    pub skip_count: u32,
    pub frames_since_full: u32,
}

fn read_f32(data: &[u8], off: usize) -> Option<f32> {
    let bytes = data.get(off..off + 4)?;
    Some(f32::from_le_bytes(bytes.try_into().ok()?))
}

fn read_u16(data: &[u8], off: usize) -> Option<u16> {
    let bytes = data.get(off..off + 2)?;
    Some(u16::from_le_bytes(bytes.try_into().ok()?))
}

fn read_u32(data: &[u8], off: usize) -> Option<u32> {
    let bytes = data.get(off..off + 4)?;
    Some(u32::from_le_bytes(bytes.try_into().ok()?))
}

pub fn make_clipboard_write_msg(text: &str) -> Vec<u8> {
    let bytes = text.as_bytes();
    let mut msg = Vec::with_capacity(1 + 4 + bytes.len());
    msg.push(0x18);
    msg.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
    msg.extend_from_slice(bytes);
    msg
}

pub fn parse_client_msg(data: &[u8]) -> Option<(ClientId, ClientMsg)> {
    let msg_type = *data.first()?;
    let id = read_u32(data, 1)?;
    parse_client_payload(msg_type, data.get(5..)?).map(|msg| (id, msg))
}

fn parse_client_payload(msg_type: u8, data: &[u8]) -> Option<ClientMsg> {
    let base = 0;
    match msg_type {
        0x10 => {
            let x = read_f32(data, base)?;
            let y = read_f32(data, base + 4)?;
            Some(ClientMsg::Input(InputEvent {
                ev_type: 0,
                x,
                y,
                button: 0,
                key: 0,
                character: 0,
                wheel_x: 0.0,
                wheel_y: 0.0,
                display_w: 0.0,
                display_h: 0.0,
            }))
        }
        0x11 => {
            let b = *data.get(base)? as i32;
            Some(ClientMsg::Input(InputEvent {
                ev_type: 1,
                x: 0.0,
                y: 0.0,
                button: b,
                key: 0,
                character: 0,
                wheel_x: 0.0,
                wheel_y: 0.0,
                display_w: 0.0,
                display_h: 0.0,
            }))
        }
        0x12 => {
            let b = *data.get(base)? as i32;
            Some(ClientMsg::Input(InputEvent {
                ev_type: 2,
                x: 0.0,
                y: 0.0,
                button: b,
                key: 0,
                character: 0,
                wheel_x: 0.0,
                wheel_y: 0.0,
                display_w: 0.0,
                display_h: 0.0,
            }))
        }
        0x13 => {
            let x = read_f32(data, base)?;
            let y = read_f32(data, base + 4)?;
            Some(ClientMsg::Input(InputEvent {
                ev_type: 3,
                x: 0.0,
                y: 0.0,
                button: 0,
                key: 0,
                character: 0,
                wheel_x: x,
                wheel_y: y,
                display_w: 0.0,
                display_h: 0.0,
            }))
        }
        0x14 => {
            let k = read_u16(data, base)? as i32;
            Some(ClientMsg::Input(InputEvent {
                ev_type: 4,
                x: 0.0,
                y: 0.0,
                button: 0,
                key: k,
                character: 0,
                wheel_x: 0.0,
                wheel_y: 0.0,
                display_w: 0.0,
                display_h: 0.0,
            }))
        }
        0x15 => {
            let k = read_u16(data, base)? as i32;
            Some(ClientMsg::Input(InputEvent {
                ev_type: 5,
                x: 0.0,
                y: 0.0,
                button: 0,
                key: k,
                character: 0,
                wheel_x: 0.0,
                wheel_y: 0.0,
                display_w: 0.0,
                display_h: 0.0,
            }))
        }
        0x16 => {
            let ch = read_u32(data, base)?;
            Some(ClientMsg::Input(InputEvent {
                ev_type: 6,
                x: 0.0,
                y: 0.0,
                button: 0,
                key: 0,
                character: ch,
                wheel_x: 0.0,
                wheel_y: 0.0,
                display_w: 0.0,
                display_h: 0.0,
            }))
        }
        0x17 => {
            let w = read_f32(data, base)?;
            let h = read_f32(data, base + 4)?;
            Some(ClientMsg::Resize { w, h })
        }
        0x18 => {
            let text_len = read_u32(data, base)? as usize;
            let text_bytes = data.get(base + 4..base + 4 + text_len)?;
            let text = String::from_utf8_lossy(text_bytes).into_owned();
            Some(ClientMsg::ClipboardText(text))
        }
        _ => None,
    }
}

/// Decode either one legacy client message or a 0x19 batch. Batch records
/// share the outer client id and contain schema-sized payloads without the
/// repeated four-byte id.
pub fn parse_client_msgs(data: &[u8]) -> Option<Vec<(ClientId, ClientMsg)>> {
    if data.first().copied()? != 0x19 {
        return Some(vec![parse_client_msg(data)?]);
    }

    let id = read_u32(data, 1)?;
    let count = read_u16(data, 5)? as usize;
    let mut off = 7;
    let mut messages = Vec::with_capacity(count);
    for _ in 0..count {
        let msg_type = *data.get(off)?;
        let payload_len = read_u16(data, off + 1)? as usize;
        off += 3;
        let payload = data.get(off..off + payload_len)?;
        off += payload_len;
        messages.push((id, parse_client_payload(msg_type, payload)?));
    }
    (off == data.len()).then_some(messages)
}

pub struct ImGuiWasmState {
    pub frame_tx: broadcast::Sender<Vec<u8>>,
    pub textures: Mutex<HashMap<u64, Vec<u8>>>,
    pub clients: Mutex<HashMap<ClientId, ClientState>>,
    pub frame_builder: Mutex<Option<FrameBuilder>>,
    next_client_id: AtomicU32,
    active_client: Mutex<Option<ClientId>>,
    pub input_tx: Sender<(ClientId, InputEvent)>,
    pub input_rx: Mutex<Receiver<(ClientId, InputEvent)>>,
    // Call-stream transport: the frame header captured at begin_frame (needed
    // to build the 0x07 envelope at end_frame) and a monotonic frame id.
    callstream_header: Mutex<crate::callstream_protocol::FrameHeader>,
    callstream_frame_id: AtomicU32,
    // Last call-stream frame hash, for the "identical frame" skip optimization
    // (mirrors the draw-data skip in end_frame).
    last_callstream_hash: Mutex<u32>,
}

fn compression_enabled() -> bool {
    crate::is_compression_enabled()
}

pub fn hash_draw_list(vtx: &[u8], idx: &[u8], cmds: &[DrawCmd]) -> u32 {
    use xxhash_rust::xxh3;
    let mut h: u64 = 0;
    h = xxh3::xxh3_64_with_seed(vtx, h);
    h = xxh3::xxh3_64_with_seed(idx, h);
    for cmd in cmds {
        let bytes = unsafe {
            std::slice::from_raw_parts(
                (cmd as *const DrawCmd) as *const u8,
                std::mem::size_of::<DrawCmd>(),
            )
        };
        h = xxh3::xxh3_64_with_seed(bytes, h);
    }
    (h ^ (h >> 32)) as u32
}

impl ImGuiWasmState {
    pub fn new() -> (Self, broadcast::Receiver<Vec<u8>>) {
        let (frame_tx, frame_rx) = broadcast::channel(8);
        let (input_tx, input_rx) = std::sync::mpsc::channel();

        (
            Self {
                frame_tx,
                textures: Mutex::new(HashMap::new()),
                clients: Mutex::new(HashMap::new()),
                frame_builder: Mutex::new(None),
                next_client_id: AtomicU32::new(1),
                active_client: Mutex::new(None),
                input_tx,
                input_rx: Mutex::new(input_rx),
                callstream_header: Mutex::new(crate::callstream_protocol::FrameHeader {
                    dpx: 0.0,
                    dpy: 0.0,
                    dsw: 0.0,
                    dsh: 0.0,
                    fbsx: 1.0,
                    fbsy: 1.0,
                }),
                callstream_frame_id: AtomicU32::new(1),
                last_callstream_hash: Mutex::new(0),
            },
            frame_rx,
        )
    }

    pub fn add_client(&self) -> (ClientId, UnboundedReceiver<Vec<u8>>) {
        let id = self.next_client_id.fetch_add(1, Ordering::SeqCst);
        let (sender, receiver) = unbounded_channel();
        let mut clients = self.clients.lock().unwrap();
        clients.insert(
            id,
            ClientState {
                display_size: [1280.0, 720.0],
                delta_state: DeltaState {
                    last_frame_hash: 0,
                    last_list_hashes: Vec::new(),
                    skip_count: 0,
                    frames_since_full: 0,
                },
                force_frames: 3,
                clipboard_text: String::new(),
                sender,
            },
        );

        let mut active = self.active_client.lock().unwrap();
        if active.is_none() {
            *active = Some(id);
        }

        (id, receiver)
    }

    pub fn remove_client(&self, id: ClientId) {
        let mut clients = self.clients.lock().unwrap();
        clients.remove(&id);
        drop(clients);

        let mut active = self.active_client.lock().unwrap();
        if *active == Some(id) {
            let clients = self.clients.lock().unwrap();
            *active = clients.keys().next().copied();
        }
    }

    pub fn push_input(&self, client_id: ClientId, event: InputEvent) {
        let _ = self.input_tx.send((client_id, event));
        let mut active = self.active_client.lock().unwrap();
        *active = Some(client_id);
    }

    pub fn try_poll_input(&self) -> Option<InputEvent> {
        let rx = self.input_rx.lock().unwrap();
        match rx.try_recv() {
            Ok((client_id, mut ev)) => {
                let clients = self.clients.lock().unwrap();
                if let Some(cs) = clients.get(&client_id) {
                    ev.display_w = cs.display_size[0];
                    ev.display_h = cs.display_size[1];
                }
                drop(clients);
                let mut active = self.active_client.lock().unwrap();
                *active = Some(client_id);
                Some(ev)
            }
            Err(TryRecvError::Empty) => None,
            Err(TryRecvError::Disconnected) => None,
        }
    }

    fn prepare_frame(&self, data: Vec<u8>) -> Vec<u8> {
        let payload = if compression_enabled() {
            let msg_type = data[0];
            let compressed = lz4_flex::compress(&data);
            let mut out = Vec::with_capacity(1 + 4 + compressed.len());
            let compressed_type = match msg_type {
                0x01 => 0x03,
                0x04 => 0x05,
                _ => 0,
            };
            if compressed_type != 0 && compressed.len() + 5 < data.len() {
                out.push(compressed_type);
                out.extend_from_slice(&(data.len() as u32).to_le_bytes());
                out.extend_from_slice(&compressed);
                out
            } else {
                data
            }
        } else {
            data
        };

        payload
    }

    pub fn send_frame(&self, data: Vec<u8>) {
        let _ = self.frame_tx.send(self.prepare_frame(data));
    }

    pub fn send_frame_to(&self, client_id: ClientId, data: Vec<u8>) {
        let payload = self.prepare_frame(data);
        let clients = self.clients.lock().unwrap();
        if let Some(client) = clients.get(&client_id) {
            let _ = client.sender.send(payload);
        }
    }

    pub fn send_texture(&self, id: u64, data: Vec<u8>) {
        let mut textures = self.textures.lock().unwrap();
        textures.insert(id, data.clone());
        drop(textures);
        let _ = self.frame_tx.send(data);
    }

    pub fn set_display_size(&self, client_id: ClientId, w: f32, h: f32) {
        let mut clients = self.clients.lock().unwrap();
        if let Some(cs) = clients.get_mut(&client_id) {
            cs.display_size = [w, h];
            drop(clients);
            // Select the resized client before its first pointer event. This
            // prevents a click from changing the shared viewport mid-frame.
            *self.active_client.lock().unwrap() = Some(client_id);
        }
    }

    pub fn get_display_size(&self) -> [f32; 2] {
        let active = self.active_client.lock().unwrap();
        let client_id = match *active {
            Some(id) => id,
            None => return [1280.0, 720.0],
        };
        drop(active);
        let clients = self.clients.lock().unwrap();
        clients
            .get(&client_id)
            .map(|cs| cs.display_size)
            .unwrap_or([1280.0, 720.0])
    }

    pub fn set_clipboard_text(&self, text: &str) {
        let active = self.active_client.lock().unwrap();
        let client_id = match *active {
            Some(id) => id,
            None => return,
        };
        drop(active);
        let mut clients = self.clients.lock().unwrap();
        if let Some(cs) = clients.get_mut(&client_id) {
            cs.clipboard_text = text.to_string();
        }
        drop(clients);
        let msg = make_clipboard_write_msg(text);
        self.send_frame_to(client_id, msg);
    }

    pub fn get_clipboard_text(&self) -> String {
        let active = self.active_client.lock().unwrap();
        let client_id = match *active {
            Some(id) => id,
            None => return String::new(),
        };
        drop(active);
        let clients = self.clients.lock().unwrap();
        clients
            .get(&client_id)
            .map(|cs| cs.clipboard_text.clone())
            .unwrap_or_default()
    }

    pub fn on_clipboard_text(&self, client_id: ClientId, text: String) {
        let mut clients = self.clients.lock().unwrap();
        if let Some(cs) = clients.get_mut(&client_id) {
            cs.clipboard_text = text;
        }
    }

    pub fn has_clients(&self) -> bool {
        let clients = self.clients.lock().unwrap();
        !clients.is_empty()
    }

    pub fn begin_frame(
        &self,
        dpx: f32,
        dpy: f32,
        dsw: f32,
        dsh: f32,
        fbsx: f32,
        fbsy: f32,
    ) -> bool {
        if !self.has_clients() {
            return false;
        }

        let mut builder = self.frame_builder.lock().unwrap();
        *builder = Some(FrameBuilder {
            dpx,
            dpy,
            dsw,
            dsh,
            fbsx,
            fbsy,
            lists: Vec::new(),
        });
        true
    }

    pub fn add_draw_list(&self, vtx: &[u8], idx: &[u8], idx_size: i32, cmds: Vec<DrawCmd>) {
        let mut builder = self.frame_builder.lock().unwrap();
        if let Some(ref mut b) = *builder {
            let idx_data = if idx_size == 2 {
                let mut out = Vec::with_capacity(idx.len() * 2);
                let u16_slice = unsafe {
                    std::slice::from_raw_parts(idx.as_ptr() as *const u16, idx.len() / 2)
                };
                for &i in u16_slice {
                    out.extend_from_slice(&(i as u32).to_le_bytes());
                }
                out
            } else {
                idx.to_vec()
            };

            let hash = hash_draw_list(vtx, &idx_data, &cmds);
            b.lists.push(DrawList {
                vtx: vtx.to_vec(),
                idx: idx_data,
                cmds,
                hash,
            });
        }
    }

    pub fn end_frame(&self) {
        let builder = self.frame_builder.lock().unwrap().take();
        if let Some(b) = builder {
            let mut clients = self.clients.lock().unwrap();
            let mut frames = Vec::with_capacity(clients.len());
            for (&client_id, cs) in clients.iter_mut() {
                let force = cs.force_frames > 0;

                let mut new_hashes = Vec::with_capacity(b.lists.len());
                let mut frame_hash = 2166136261u32;
                frame_hash ^= b.lists.len() as u32;
                frame_hash = frame_hash.wrapping_mul(16777619);
                for list in &b.lists {
                    new_hashes.push(list.hash);
                    frame_hash ^= list.hash;
                    frame_hash = frame_hash.wrapping_mul(16777619);
                }

                let mut force_full = false;
                if !force
                    && frame_hash == cs.delta_state.last_frame_hash
                    && cs.delta_state.last_frame_hash != 0
                {
                    cs.delta_state.skip_count += 1;
                    if cs.delta_state.skip_count < 120 {
                        continue;
                    }
                    force_full = true;
                }
                cs.delta_state.skip_count = 0;

                if force {
                    cs.force_frames = cs.force_frames.saturating_sub(1);
                }

                let full_frame_interval = 60;
                let use_delta = !force_full
                    && !force
                    && cs.delta_state.last_frame_hash != 0
                    && cs.delta_state.frames_since_full < full_frame_interval
                    && b.lists.len() == cs.delta_state.last_list_hashes.len();

                let payload = if use_delta {
                    cs.delta_state.frames_since_full += 1;
                    crate::protocol::serialize_delta_frame(
                        &b,
                        &cs.delta_state.last_list_hashes,
                        &new_hashes,
                    )
                } else {
                    cs.delta_state.frames_since_full = 0;
                    cs.delta_state.last_list_hashes = new_hashes;
                    crate::protocol::serialize_full_frame(&b)
                };

                cs.delta_state.last_frame_hash = frame_hash;

                frames.push((client_id, payload));
            }
            drop(clients);
            for (client_id, payload) in frames {
                self.send_frame_to(client_id, payload);
            }
        }
    }

    // --- Call-stream transport path ---------------------------------------
    //
    // When transport == CALLSTREAM, begin_frame resets the capture buffer and
    // stashes the header; the host then emits ImGui calls (captured by the
    // generated wrappers); end_callstream_frame drains the buffer, builds a
    // 0x07 frame + any 0x09 string update, and broadcasts. The draw-data
    // FrameBuilder is bypassed entirely (add_draw_list is a no-op in this mode).

    pub fn set_callstream_header(
        &self,
        dpx: f32,
        dpy: f32,
        dsw: f32,
        dsh: f32,
        fbsx: f32,
        fbsy: f32,
    ) {
        let mut h = self.callstream_header.lock().unwrap();
        h.dpx = dpx;
        h.dpy = dpy;
        h.dsw = dsw;
        h.dsh = dsh;
        h.fbsx = fbsx;
        h.fbsy = fbsy;
    }

    pub fn end_callstream_frame(&self) {
        let header = *self.callstream_header.lock().unwrap();
        let frame_id = self.callstream_frame_id.fetch_add(1, Ordering::Relaxed);

        // Drain the capture buffer + any newly-interned strings. These are
        // process-global (the string table is shared across clients); we
        // compute one frame and broadcast it.
        let (frame_payload, string_payload) =
            crate::callstream_protocol::drain_frame(header, frame_id);

        // Identical-frame skip: if the call sequence + scalars are unchanged
        // since last frame (and there are no new strings), skip broadcasting.
        // This is the call-stream analog of the draw-data hash skip above, and
        // it's where the big idle-frame win comes from.
        //
        // frame_payload layout:
        //   [0]      0x07
        //   [1..25]  6 x f32 header
        //   [25..29] frame_id u32
        //   [29..33] call_count u32
        //   [33..]   call bytes
        // Hash only the schema payload. The envelope's frame_id changes every
        // frame and must not defeat identical-frame suppression.
        let call_region = &frame_payload[33..];
        let new_hash = crate::callstream_protocol::frame_hash(header, call_region);

        // A newly connected client needs an initial replay even when the call
        // stream is otherwise unchanged. One broadcast initializes every
        // subscriber; never broadcast once per client (broadcast::Sender
        // already fans a message out to all receivers).
        let force = {
            let mut clients = self.clients.lock().unwrap();
            let force = clients.values().any(|cs| cs.force_frames > 0);
            if force {
                for cs in clients.values_mut() {
                    cs.force_frames = cs.force_frames.saturating_sub(1);
                }
            }
            force
        };

        let mut last_hash = self.last_callstream_hash.lock().unwrap();
        let unchanged =
            !force && new_hash == *last_hash && *last_hash != 0 && string_payload.len() <= 5;
        if unchanged {
            // Idle frame: nothing new to send. Keep last_hash; clients keep
            // their last rendered frame.
            return;
        }
        *last_hash = new_hash;
        drop(last_hash);

        // New clients must get a full frame: force-frames handling mirrors the
        // draw-data path. The string table is shared, so a newly-connected
        // client still needs the strings it missed — the simplest correct
        // approach is to let the client reconnect-resync handle the initial
        // state (a full frame + string set is sent on the first changed frame
        // after it connects, which is near-immediate for any live UI).
        // broadcast::Sender performs the fan-out. Sending inside a client loop
        // made N clients receive N copies of every frame (quadratic traffic).
        if string_payload.len() > 5 {
            self.send_frame(string_payload);
        }
        self.send_frame(frame_payload);
    }
}
