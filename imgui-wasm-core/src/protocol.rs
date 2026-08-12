use crate::state::{DrawList, FrameBuilder};

pub fn make_texture_msg(id: u64, width: u32, height: u32, pixels: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(1 + 8 + 4 + 4 + 4 + pixels.len());
    out.push(0x02);
    out.extend_from_slice(&id.to_le_bytes());
    out.extend_from_slice(&width.to_le_bytes());
    out.extend_from_slice(&height.to_le_bytes());
    out.extend_from_slice(&(pixels.len() as u32).to_le_bytes());
    out.extend_from_slice(pixels);
    out
}

pub fn serialize_full_frame(b: &FrameBuilder) -> Vec<u8> {
    let mut out = Vec::with_capacity(4096);
    out.push(0x01);
    write_header(&mut out, b);
    out.extend_from_slice(&(b.lists.len() as u32).to_le_bytes());

    for list in &b.lists {
        write_draw_list(&mut out, list);
    }
    out
}

pub fn serialize_delta_frame(b: &FrameBuilder, old_hashes: &[u32], new_hashes: &[u32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(1024);
    out.push(0x04);
    write_header(&mut out, b);
    out.extend_from_slice(&(b.lists.len() as u32).to_le_bytes());

    let mut changed_indices = Vec::new();
    for i in 0..b.lists.len() {
        if i >= old_hashes.len() || old_hashes[i] != new_hashes[i] {
            changed_indices.push(i);
        }
    }

    out.extend_from_slice(&(changed_indices.len() as u32).to_le_bytes());
    for &idx in &changed_indices {
        out.extend_from_slice(&(idx as u32).to_le_bytes());
        write_draw_list(&mut out, &b.lists[idx]);
    }

    out
}

fn write_header(out: &mut Vec<u8>, b: &FrameBuilder) {
    out.extend_from_slice(&b.dpx.to_le_bytes());
    out.extend_from_slice(&b.dpy.to_le_bytes());
    out.extend_from_slice(&b.dsw.to_le_bytes());
    out.extend_from_slice(&b.dsh.to_le_bytes());
    out.extend_from_slice(&b.fbsx.to_le_bytes());
    out.extend_from_slice(&b.fbsy.to_le_bytes());
}

fn write_draw_list(out: &mut Vec<u8>, list: &DrawList) {
    out.extend_from_slice(&(list.vtx.len() as u32 / 20).to_le_bytes());
    out.extend_from_slice(&(list.idx.len() as u32 / 4).to_le_bytes());
    out.extend_from_slice(&(list.cmds.len() as u32).to_le_bytes());

    out.extend_from_slice(&list.vtx);
    out.extend_from_slice(&list.idx);

    for cmd in &list.cmds {
        out.extend_from_slice(&cmd.clip_rect[0].to_le_bytes());
        out.extend_from_slice(&cmd.clip_rect[1].to_le_bytes());
        out.extend_from_slice(&cmd.clip_rect[2].to_le_bytes());
        out.extend_from_slice(&cmd.clip_rect[3].to_le_bytes());
        out.extend_from_slice(&cmd.texture_id.to_le_bytes());
        out.extend_from_slice(&cmd.idx_offset.to_le_bytes());
        out.extend_from_slice(&cmd.vtx_offset.to_le_bytes());
        out.extend_from_slice(&cmd.elem_count.to_le_bytes());
    }
}
