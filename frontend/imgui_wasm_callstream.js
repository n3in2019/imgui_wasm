// imgui_wasm_callstream.js — browser-side call-stream replay.
//
// The server streams the sequence of captured ImGui API calls (messages
// 0x07/0x09); this module drives a WASM-compiled Dear ImGui "twin" that
// replays them to regenerate draw data locally. The produced draw data uses
// a flat vtx/idx/cmd byte layout, so the existing parseDrawLists +
// renderFromParsed (imgui_wasm.js) render it unchanged.
//
// State authority: the SERVER is authoritative. The twin only reproduces the
// drawing call sequence; the server echoes output-pointer values (slider &v,
// checkbox &v, ...) each frame, so the twin's own writes are discarded. Input
// from the browser round-trips only to the authoritative server. The twin
// does not run an independent widget input state machine.
//
// Lifecycle:
//   - on first 0x07/0x09: import() the emscripten glue, init the twin
//   - on each 0x09: install/replace interned strings in the twin
//   - on each 0x07: run imgui_wasm_replay_frame, parse output, render

let replayModule = null;   // the emscripten MODULARIZE export
let replayReady = false;   // init() has run
let stringUpdates = [];    // 0x09 messages buffered before init (rare)
let replayChain = Promise.resolve();
let replayBusy = false;
let pendingLatestReplay = null;
let lastAppliedIniId = null;
let lastUiSnapshot = null;
let lastCanvas = null;
let lastHooks = null;

// lazily instantiate the WASM twin
async function ensureReplay(canvas) {
    if (replayReady) return replayModule;
    if (!replayModule) {
        // imgui_wasm_replay.js is the emscripten MODULARIZE factory (EXPORT_NAME=
        // imgui_wasm_replay). It defines a global `imgui_wasm_replay` function that
        // returns a Promise<Module>. Load it via a <script> tag (it is not an
        // ES module), then await the factory.
        await new Promise((resolve, reject) => {
            const s = document.createElement("script");
            s.src = "/imgui_wasm_replay.js";
            s.onload = resolve;
            s.onerror = () => reject(new Error("failed to load imgui_wasm_replay.js"));
            document.head.appendChild(s);
        });
        const factory = window.imgui_wasm_replay;
        if (typeof factory !== "function") {
            throw new Error("imgui_wasm_replay factory not found after script load");
        }
        replayModule = await factory();
    }
    if (!replayReady) {
        replayModule._imgui_wasm_replay_init(canvas.width, canvas.height);
        replayReady = true;
        // Upload the twin's OWN font atlas as texture id 1. The twin's draw
        // commands reference glyph UVs computed from this atlas, so the
        // browser must render with THESE pixels (not the server's, whose glyph
        // packing may differ). Done once after init.
        uploadTwinFontAtlas();
        // flush any strings that arrived before init
        for (const { id, bytes } of stringUpdates) {
            installString(id, bytes);
        }
        stringUpdates = [];
    }
    return replayModule;
}

// Read the twin's font atlas out of WASM linear memory and hand it to the
// frontend's texture uploader under id 1 (the id ImGui's font cmds reference).
function uploadTwinFontAtlas() {
    if (!replayModule) return;
    const w = replayModule._imgui_wasm_replay_font_tex_width();
    const h = replayModule._imgui_wasm_replay_font_tex_height();
    const ptr = replayModule._imgui_wasm_replay_font_tex_pixels();
    if (!w || !h || !ptr) return;
    const len = w * h * 4; // RGBA
    const pixels = new Uint8Array(replayModule.HEAPU8.buffer, ptr, len);
    // Copy because HEAPU8 may move on reallocation; the uploader stores it.
    const copy = new Uint8Array(len);
    copy.set(pixels);
    if (typeof window.__imgui_wasmUploadTwinFontAtlas === "function") {
        window.__imgui_wasmUploadTwinFontAtlas(1, w, h, copy);
    }
}

function installString(id, bytes) {
    if (!replayReady || !replayModule) {
        stringUpdates.push({ id, bytes });
        return;
    }
    const len = bytes.length;
    const ptr = replayModule._malloc(len);
    if (!ptr) return;
    replayModule.HEAPU8.set(bytes, ptr);
    replayModule._imgui_wasm_replay_set_string(id, ptr, len);
    replayModule._free(ptr);
}

// Copy a byte slice into WASM linear memory and return the pointer.
function copyToHeap(bytes) {
    const ptr = replayModule._malloc(bytes.length);
    if (!ptr) return 0;
    replayModule.HEAPU8.set(bytes, ptr);
    return ptr;
}

// Reconstruct a frame in the flat draw-list layout parseDrawLists expects
// from the WASM twin's draw output. The twin output is: per-list
// [numVtx u32][numIdx u32][numCmd u32] [vtx][idx u32*][cmd...], with NO type
// byte and NO header. We prepend:
//   0x01 (frame type byte), dpx,dpy,dsw,dsh,fbsx,fbsy (6 f32), numLists (u32)
function buildSyntheticFrame(drawOut, listCount, dpx, dpy, dsw, dsh, fbsx, fbsy) {
    const headerLen = 1 + 24 + 4; // type + 6 floats + numLists
    const frame = new Uint8Array(headerLen + drawOut.length);
    const dv = new DataView(frame.buffer);
    let off = 0;
    frame[off++] = 0x01;
    dv.setFloat32(off, dpx, true); off += 4;
    dv.setFloat32(off, dpy, true); off += 4;
    dv.setFloat32(off, dsw, true); off += 4;
    dv.setFloat32(off, dsh, true); off += 4;
    dv.setFloat32(off, fbsx, true); off += 4;
    dv.setFloat32(off, fbsy, true); off += 4;
    dv.setUint32(off, listCount, true); off += 4;
    frame.set(drawOut, off);
    return frame;
}

// Handle a 0x07 call-stream frame. `canvas` and the `hooks` (parseDrawLists,
// renderFromParsed, setLastFrame) are provided by imgui_wasm.js.
async function replaySnapshot(snapshot, canvas, hooks) {
    const { callBytes, callCount, dpx, dpy, dsw, dsh, fbsx, fbsy } = snapshot;
    const mod = await ensureReplay(canvas);

    const callPtr = copyToHeap(callBytes);
    if (!callPtr) return;
    const drawPtr = mod._imgui_wasm_replay_frame(callPtr, callBytes.length, callCount,
        dpx, dpy, dsw, dsh, fbsx, fbsy);
    mod._free(callPtr);
    if (!drawPtr) return;

    const drawLen = mod._imgui_wasm_replay_draw_data_len();
    const listCount = mod._imgui_wasm_replay_list_count();
    if (drawLen === 0 || listCount === 0) return;
    const drawOut = new Uint8Array(mod.HEAPU8.buffer, drawPtr, drawLen);
    const frame = buildSyntheticFrame(drawOut, listCount, dpx, dpy, dsw, dsh, fbsx, fbsy);

    try {
        const parsed = hooks.parseDrawLists(frame);
        hooks.setLastFrame(parsed);
        hooks.renderFromParsed(parsed);
    } catch (e) {
        console.error('[imgui_wasm] call-stream render error:', e);
    }
}

function enqueueReplay(snapshot, canvas, hooks, reliable = false) {
    const work = { snapshot, canvas, hooks };
    if (replayBusy && !reliable) {
        // The twin is stateful, but intermediate UI frames are not: replaying
        // stale network frames only increases latency. Keep the newest one.
        pendingLatestReplay = work;
        return replayChain;
    }
    replayBusy = true;
    replayChain = replayChain.then(async () => {
        await replaySnapshot(work.snapshot, work.canvas, work.hooks);
        while (pendingLatestReplay) {
            const latest = pendingLatestReplay;
            pendingLatestReplay = null;
            await replaySnapshot(latest.snapshot, latest.canvas, latest.hooks);
        }
        replayBusy = false;
    }).catch((e) => {
        replayBusy = false;
        // A failed replay can orphan a queued (older) snapshot; replaying it
        // after a newer one would rewind the twin, so drop it instead.
        pendingLatestReplay = null;
        console.error('[imgui_wasm] call-stream replay error:', e);
    });
    return replayChain;
}

function handleCallstreamFrame(data, canvas, hooks) {
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    let off = 1; // skip type byte
    const dpx = dv.getFloat32(off, true); off += 4;
    const dpy = dv.getFloat32(off, true); off += 4;
    const dsw = dv.getFloat32(off, true); off += 4;
    const dsh = dv.getFloat32(off, true); off += 4;
    const fbsx = dv.getFloat32(off, true); off += 4;
    const fbsy = dv.getFloat32(off, true); off += 4;
    const frameId = dv.getUint32(off, true); off += 4;
    const callCount = dv.getUint32(off, true); off += 4;
    // Own the bytes independently of the WebSocket receive buffer so this
    // exact call batch can be replayed locally after server idle suppression.
    let callBytes = data.subarray(off);
    let iniCallBytes = null;
    // The native backend prefixes the authoritative persisted INI as a
    // LoadIniSettingsFromMemory record: [opcode u16][interned ini id u32]
    // [ini size u32] (the arg schema of the call). Applying the same INI
    // inside every ImGui frame resets transient widget/render state — and
    // pins window geometry, so drags would snap back. Replay it only when
    // the interned settings blob changes, then strip the record from the
    // batch before the widget calls.
    if (callBytes.length >= 10) {
        const callView = new DataView(callBytes.buffer, callBytes.byteOffset, callBytes.byteLength);
        if (callView.getUint16(0, true) === 81) {
            const iniId = callView.getUint32(2, true);
            const recordLen = 10;
            if (iniId !== lastAppliedIniId) {
                lastAppliedIniId = iniId;
                iniCallBytes = new Uint8Array(callBytes.subarray(0, recordLen));
            }
            // INI is never replayed in the same ImGui frame as widgets. A
            // changed blob gets an empty preparation frame below; every UI
            // frame starts after settings have settled.
            callBytes = callBytes.subarray(recordLen);
        }
    }

    const snapshot = {
        dpx, dpy, dsw, dsh, fbsx, fbsy, frameId, callCount,
        callBytes: new Uint8Array(callBytes),
    };
    lastUiSnapshot = snapshot;
    lastCanvas = canvas;
    lastHooks = hooks;
    if (iniCallBytes) {
        const iniSnapshot = {
            dpx, dpy, dsw, dsh, fbsx, fbsy, frameId, callCount: 1,
            callBytes: iniCallBytes,
        };
        enqueueReplay(iniSnapshot, canvas, hooks, true);
    }
    return enqueueReplay(snapshot, canvas, hooks);
}

// Handle a 0x09 string-table update.
function handleStringUpdate(data) {
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    let off = 1;
    const count = dv.getUint32(off, true); off += 4;
    for (let i = 0; i < count; i++) {
        const id = dv.getUint32(off, true); off += 4;
        const len = dv.getUint32(off, true); off += 4;
        const bytes = data.subarray(off, off + len); off += len;
        // Copy because installString copies again into the heap; subarray views
        // the WS buffer which may be recycled.
        installString(id, new Uint8Array(bytes));
    }
}

// Mirror browser input into the twin so its io state (hover/active/scroll)
// matches the server's. The server is authoritative for geometry, but widget
// interaction visuals (e.g. a button press highlight) need local input to
// render correctly between server round-trips.
function feedInputToTwin(kind, ...args) {
    if (!replayReady || !replayModule || !lastUiSnapshot || !lastCanvas || !lastHooks) return;

    // Window movement and resizing live in ImGui's internal state and are not
    // explicit host calls. Feed the browser event into the twin, then replay
    // the last authoritative UI call batch so dragging gets an immediate
    // frame even when the server's call bytes are otherwise unchanged and
    // suppressed as idle traffic.
    replayChain = replayChain.then(async () => {
        switch (kind) {
            case "mouse_pos": replayModule._imgui_wasm_replay_input_mouse_pos(args[0], args[1]); break;
            case "mouse_down": replayModule._imgui_wasm_replay_input_mouse_down(args[0]); break;
            case "mouse_up": replayModule._imgui_wasm_replay_input_mouse_up(args[0]); break;
            case "wheel": replayModule._imgui_wasm_replay_input_wheel(args[0], args[1]); break;
            case "key": replayModule._imgui_wasm_replay_input_key(args[0], args[1] ? 1 : 0); break;
            case "char": replayModule._imgui_wasm_replay_input_char(args[0]); break;
            default: return;
        }
        await replaySnapshot(lastUiSnapshot, lastCanvas, lastHooks);
    }).catch((e) => console.error('[imgui_wasm] call-stream input replay error:', e));
}

// Public API consumed by imgui_wasm.js's ws.onmessage dispatch. Exposed as a global
// (window.imgui_wasmCallstream) because imgui_wasm.js loads as a classic script, not an
// ES module. See frontend/index.html.
window.imgui_wasmCallstream = {
    handleFrame: handleCallstreamFrame,
    handleStringUpdate: handleStringUpdate,
    feedInput: feedInputToTwin,
    isReady: () => replayReady,
};
