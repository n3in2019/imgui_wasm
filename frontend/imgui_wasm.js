"use strict";

const canvas = document.getElementById("canvas");
const statusEl = document.getElementById("status");
// preserveDrawingBuffer lets the canvas be screenshotted (otherwise the GL
// backbuffer is cleared after compositing and captures come back blank).
const gl = canvas.getContext("webgl", { alpha: false, antialias: false, premultipliedAlpha: false, preserveDrawingBuffer: true });

if (!gl) {
    document.body.innerText = "WebGL not supported";
    throw new Error("No WebGL");
}

const extUintIdx = gl.getExtension("OES_element_index_uint");
if (!extUintIdx) {
    statusEl.textContent = "WebGL extension OES_element_index_uint is required";
    statusEl.className = "disconnected";
    throw new Error("OES_element_index_uint not available");
}

const VERT_SRC = `
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;
uniform mat4 u_proj;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
}`;

const FRAG_SRC = `
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    gl_FragColor = v_color * texture2D(u_tex, v_uv);
}`;

function compileShader(src, type) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
        console.error("Shader compile error:", gl.getShaderInfoLog(s));
        return null;
    }
    return s;
}

const prog = gl.createProgram();
gl.attachShader(prog, compileShader(VERT_SRC, gl.VERTEX_SHADER));
gl.attachShader(prog, compileShader(FRAG_SRC, gl.FRAGMENT_SHADER));
gl.linkProgram(prog);
if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
    console.error("Shader link error:", gl.getProgramInfoLog(prog));
}
gl.useProgram(prog);

const a_pos = gl.getAttribLocation(prog, "a_pos");
const a_uv = gl.getAttribLocation(prog, "a_uv");
const a_color = gl.getAttribLocation(prog, "a_color");
const u_proj_loc = gl.getUniformLocation(prog, "u_proj");
const u_tex_loc = gl.getUniformLocation(prog, "u_tex");

const vbo = gl.createBuffer();
const ibo = gl.createBuffer();

gl.enable(gl.BLEND);
gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
gl.enable(gl.SCISSOR_TEST);
gl.disable(gl.DEPTH_TEST);

const textures = new Map();

let lastDpx = 0, lastDpy = 0, lastDsw = 0, lastDsh = 0;

function uploadTexture(id, width, height, pixels) {
    let tex = textures.get(id);
    if (!tex) {
        tex = gl.createTexture();
        textures.set(id, tex);
    }
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(pixels));
}

function ortho(l, r, b, t) {
    const out = new Float32Array(16);
    out[0] = 2.0 / (r - l);
    out[5] = 2.0 / (t - b);
    out[10] = -1.0;
    out[12] = -(r + l) / (r - l);
    out[13] = -(t + b) / (t - b);
    out[15] = 1.0;
    return out;
}

function readF32(dv, off) { return [dv.getFloat32(off, true), off + 4]; }
function readU32(dv, off) { return [dv.getUint32(off, true), off + 4]; }
function readU64(dv, off) {
    const lo = dv.getUint32(off, true);
    const hi = dv.getUint32(off + 4, true);
    return [lo + hi * 0x100000000, off + 8];
}

function parseDrawLists(data) {
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    let off = 1;

    const [dpx] = readF32(dv, off); off += 4;
    const [dpy] = readF32(dv, off); off += 4;
    const [dsw] = readF32(dv, off); off += 4;
    const [dsh] = readF32(dv, off); off += 4;
    const [fbsx] = readF32(dv, off); off += 4;
    const [fbsy] = readF32(dv, off); off += 4;
    const [numLists] = readU32(dv, off); off += 4;

    const lists = [];
    for (let i = 0; i < numLists; i++) {
        const [numVtx] = readU32(dv, off); off += 4;
        const [numIdx] = readU32(dv, off); off += 4;
        const [numCmd] = readU32(dv, off); off += 4;

        const vtxBytes = numVtx * 20;
        const idxBytes = numIdx * 4;

        const vtxRaw = new Uint8Array(data.buffer, data.byteOffset + off, vtxBytes);
        off += vtxBytes;
        const idxRaw = new Uint8Array(data.buffer, data.byteOffset + off, idxBytes);
        off += idxBytes;

        const cmds = [];
        for (let j = 0; j < numCmd; j++) {
            const [cx] = readF32(dv, off); off += 4;
            const [cy] = readF32(dv, off); off += 4;
            const [cz] = readF32(dv, off); off += 4;
            const [cw] = readF32(dv, off); off += 4;
            const [texId] = readU64(dv, off); off += 8;
            const [idxOff] = readU32(dv, off); off += 4;
            const [vtxOff] = readU32(dv, off); off += 4;
            const [elemCount] = readU32(dv, off); off += 4;
            cmds.push({ cx, cy, cz, cw, texId, idxOff, vtxOff, elemCount });
        }

        lists.push({ vtxRaw, idxRaw, cmds });
    }

    return { dpx, dpy, dsw, dsh, fbsx, fbsy, numLists, lists };
}

function renderFromParsed(frame) {
    const fbW = canvas.width;
    const fbH = canvas.height;
    // The server lays out ImGui in CSS pixels while the canvas backing buffer
    // uses device pixels. Derive the actual scale from this frame rather than
    // relying on the server backend's framebuffer scale (normally 1).
    const scaleX = frame.dsw > 0 ? fbW / frame.dsw : (frame.fbsx || 1);
    const scaleY = frame.dsh > 0 ? fbH / frame.dsh : (frame.fbsy || 1);

    gl.viewport(0, 0, fbW, fbH);
    gl.scissor(0, 0, fbW, fbH);
    gl.clearColor(0.1, 0.1, 0.12, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    gl.uniformMatrix4fv(u_proj_loc, false, ortho(frame.dpx, frame.dpx + frame.dsw, frame.dpy + frame.dsh, frame.dpy));
    gl.uniform1i(u_tex_loc, 0);

    for (let i = 0; i < frame.lists.length; i++) {
        const list = frame.lists[i];

        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER, list.vtxRaw, gl.DYNAMIC_DRAW);

        const stride = 20;
        gl.enableVertexAttribArray(a_pos);
        gl.vertexAttribPointer(a_pos, 2, gl.FLOAT, false, stride, 0);
        gl.enableVertexAttribArray(a_uv);
        gl.vertexAttribPointer(a_uv, 2, gl.FLOAT, false, stride, 8);
        gl.enableVertexAttribArray(a_color);
        gl.vertexAttribPointer(a_color, 4, gl.UNSIGNED_BYTE, true, stride, 16);

        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, list.idxRaw, gl.DYNAMIC_DRAW);

        for (const cmd of list.cmds) {
            if (cmd.elemCount === 0) continue;

            const clipX = Math.max(0, (cmd.cx - frame.dpx) * scaleX);
            const clipY = Math.max(0, fbH - (cmd.cw - frame.dpy) * scaleY);
            const clipW = Math.max(0, (cmd.cz - cmd.cx) * scaleX);
            const clipH = Math.max(0, (cmd.cw - cmd.cy) * scaleY);

            gl.scissor(clipX, clipY, clipW, clipH);

            const tex = textures.get(cmd.texId);
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, tex || null);

            gl.drawElements(gl.TRIANGLES, cmd.elemCount, gl.UNSIGNED_INT, cmd.idxOff * 4);
        }
    }
}

// --- Call-stream transport ----------------------------------------------------
// The call-stream helper (imgui_wasm_callstream.js) drives a WASM ImGui twin
// that replays captured calls. Loaded lazily on the first 0x07/0x09 message.
let _callstreamHelper = null;
let _callstreamLoading = null;
// Once the replay twin uploads its font atlas, texture id 1 belongs to that
// atlas. A server font resend (for example after a WebSocket reconnect) uses
// different glyph packing and must not overwrite it.
let _twinFontAtlasActive = false;
function loadCallstreamHelper() {
    if (_callstreamHelper) return Promise.resolve(_callstreamHelper);
    if (_callstreamLoading) return _callstreamLoading;
    _callstreamLoading = new Promise((resolve, reject) => {
        const s = document.createElement("script");
        s.src = "/imgui_wasm_callstream.js";
        s.onload = () => {
            _callstreamHelper = window.imgui_wasmCallstream;
            resolve(_callstreamHelper);
        };
        s.onerror = () => reject(new Error("failed to load imgui_wasm_callstream.js"));
        document.head.appendChild(s);
    }).catch((e) => {
        console.error("[imgui_wasm] call-stream helper load failed:", e);
        _callstreamLoading = null;
        throw e;
    });
    return _callstreamLoading;
}

// Hooks passed to the call-stream helper so it can reuse our renderer.
function callstreamHooks() {
    return {
        parseDrawLists,
        renderFromParsed,
        setLastFrame(parsed) {
            lastDpx = parsed.dpx;
            lastDpy = parsed.dpy;
            lastDsw = parsed.dsw;
            lastDsh = parsed.dsh;
        },
    };
}

// Compatibility hook for call-stream input. The helper deliberately keeps
// the WASM twin passive; only the authoritative server processes input.
function feedCallstreamInput(kind, ...args) {
    if (_callstreamHelper && _callstreamHelper.isReady()) {
        try { _callstreamHelper.feedInput(kind, ...args); } catch (_) {}
    }
}

// Called by the call-stream helper after the WASM twin inits: upload the
// twin's own font atlas as texture id 1 (the id its draw commands reference).
// The twin's glyph UVs only match THESE pixels, not the server's texture.
window.__imgui_wasmUploadTwinFontAtlas = function(id, w, h, pixels) {
    uploadTexture(id, w, h, pixels);
    if (id === 1) _twinFontAtlasActive = true;
};

const IMGUI_KEY_MAP = {
    "Tab": 512, "ArrowLeft": 513, "ArrowRight": 514, "ArrowUp": 515, "ArrowDown": 516,
    "PageUp": 517, "PageDown": 518, "Home": 519, "End": 520, "Insert": 521,
    "Delete": 522, "Backspace": 523, "Space": 524, "Enter": 525, "Escape": 526,
    "ControlLeft": 527, "ShiftLeft": 528, "AltLeft": 529, "MetaLeft": 530,
    "ControlRight": 531, "ShiftRight": 532, "AltRight": 533, "MetaRight": 534,
    "ContextMenu": 535,
    "Digit0": 536, "Digit1": 537, "Digit2": 538, "Digit3": 539, "Digit4": 540,
    "Digit5": 541, "Digit6": 542, "Digit7": 543, "Digit8": 544, "Digit9": 545,
    "KeyA": 546, "KeyB": 547, "KeyC": 548, "KeyD": 549, "KeyE": 550, "KeyF": 551,
    "KeyG": 552, "KeyH": 553, "KeyI": 554, "KeyJ": 555, "KeyK": 556, "KeyL": 557,
    "KeyM": 558, "KeyN": 559, "KeyO": 560, "KeyP": 561, "KeyQ": 562, "KeyR": 563,
    "KeyS": 564, "KeyT": 565, "KeyU": 566, "KeyV": 567, "KeyW": 568, "KeyX": 569,
    "KeyY": 570, "KeyZ": 571,
    "F1": 572, "F2": 573, "F3": 574, "F4": 575, "F5": 576, "F6": 577,
    "F7": 578, "F8": 579, "F9": 580, "F10": 581, "F11": 582, "F12": 583,
    "F13": 584, "F14": 585, "F15": 586, "F16": 587, "F17": 588, "F18": 589,
    "F19": 590, "F20": 591, "F21": 592, "F22": 593, "F23": 594, "F24": 595,
    "Quote": 596, "Comma": 597, "Minus": 598, "Period": 599,
    "Slash": 600, "Semicolon": 601, "Equal": 602,
    "BracketLeft": 603, "Backslash": 604, "BracketRight": 605,
    "Backquote": 606,
    "CapsLock": 607, "ScrollLock": 608, "NumLock": 609,
    "PrintScreen": 610, "Pause": 611,
    "Numpad0": 612, "Numpad1": 613, "Numpad2": 614, "Numpad3": 615,
    "Numpad4": 616, "Numpad5": 617, "Numpad6": 618, "Numpad7": 619,
    "Numpad8": 620, "Numpad9": 621, "NumpadDecimal": 622,
    "NumpadDivide": 623, "NumpadMultiply": 624, "NumpadSubtract": 625,
    "NumpadAdd": 626, "NumpadEnter": 627, "NumpadEqual": 628,
};

let ws = null;
let clientId = 0;
let pendingMouseMove = null;
let mouseMoveQueued = false;
let pendingSends = [];
let sendFlushQueued = false;

function flushSends() {
    sendFlushQueued = false;
    const messages = pendingSends;
    pendingSends = [];
    if (messages.length === 0 || clientId === 0 || !ws || ws.readyState !== WebSocket.OPEN) return;
    if (messages.length === 1) {
        ws.send(messages[0]);
        return;
    }

    // 0x19 input batch: client_id u32, count u16, then repeated
    // { type u8, payload_len u16, payload bytes }. The shared client id is
    // omitted from each record to keep rapid pointer/key sequences compact.
    const count = Math.min(messages.length, 0xffff);
    let total = 7;
    for (let i = 0; i < count; i++) total += 3 + messages[i].byteLength - 5;
    const batch = new Uint8Array(total);
    const dv = new DataView(batch.buffer);
    batch[0] = 0x19;
    dv.setUint32(1, clientId, true);
    dv.setUint16(5, count, true);
    let off = 7;
    for (let i = 0; i < count; i++) {
        const msg = messages[i];
        const payload = msg.subarray(5);
        batch[off++] = msg[0];
        dv.setUint16(off, payload.byteLength, true); off += 2;
        batch.set(payload, off); off += payload.byteLength;
    }
    ws.send(batch);
}

function sendBytes(data) {
    // Client messages are not valid until the server's 0x06 assignment has
    // arrived. In particular, WebSocket.onopen fires before that message, so
    // sending the initial resize from onopen used client id 0 and the server
    // silently ignored it.
    if (clientId !== 0 && ws && ws.readyState === WebSocket.OPEN) {
        pendingSends.push(new Uint8Array(data));
        if (!sendFlushQueued) {
            sendFlushQueued = true;
            queueMicrotask(flushSends);
        }
    }
}

function sendClipboardText(text) {
    const encoded = new TextEncoder().encode(text);
    const buf = new ArrayBuffer(5 + 4 + encoded.length);
    const dv = new DataView(buf);
    dv.setUint8(0, 0x18);
    dv.setUint32(1, clientId, true);
    dv.setUint32(5, encoded.length, true);
    new Uint8Array(buf).set(encoded, 9);
    sendBytes(buf);
}

function sendF32F32(type, a, b) {
    const buf = new ArrayBuffer(13);
    const dv = new DataView(buf);
    dv.setUint8(0, type);
    dv.setUint32(1, clientId, true);
    dv.setFloat32(5, a, true);
    dv.setFloat32(9, b, true);
    sendBytes(buf);
}

function sendU8(type, value) {
    const buf = new ArrayBuffer(6);
    const dv = new DataView(buf);
    dv.setUint8(0, type);
    dv.setUint32(1, clientId, true);
    dv.setUint8(5, value);
    sendBytes(buf);
}

function sendU16(type, value) {
    const buf = new ArrayBuffer(7);
    const dv = new DataView(buf);
    dv.setUint8(0, type);
    dv.setUint32(1, clientId, true);
    dv.setUint16(5, value, true);
    sendBytes(buf);
}

function sendU32(type, value) {
    const buf = new ArrayBuffer(9);
    const dv = new DataView(buf);
    dv.setUint8(0, type);
    dv.setUint32(1, clientId, true);
    dv.setUint32(5, value, true);
    sendBytes(buf);
}

function sendHelloAck() {
    let capabilities = 1 << 0; // WebGL baseline
    if (typeof Worker !== "undefined" && typeof OffscreenCanvas !== "undefined") capabilities |= 1 << 1;
    if (globalThis.crossOriginIsolated && typeof SharedArrayBuffer !== "undefined") capabilities |= 1 << 2;
    if (navigator.gpu) capabilities |= 1 << 3;
    const buf = new ArrayBuffer(9);
    const dv = new DataView(buf);
    dv.setUint8(0, 0x1a);
    dv.setUint32(1, clientId, true);
    dv.setUint32(5, capabilities, true);
    // Negotiation must be its own record; normal input may be microtask-batched.
    ws.send(buf);
}

function queueMouseMove(x, y) {
    pendingMouseMove = { x, y };
    if (mouseMoveQueued) return;
    mouseMoveQueued = true;
    queueMicrotask(flushMouseMove);
}

function flushMouseMove() {
    mouseMoveQueued = false;
    if (pendingMouseMove) {
        sendF32F32(0x10, pendingMouseMove.x, pendingMouseMove.y);
        pendingMouseMove = null;
    }
}

function resize() {
    const cssWidth = Math.max(1, Math.floor(canvas.clientWidth));
    const cssHeight = Math.max(1, Math.floor(canvas.clientHeight));
    const pixelRatio = Math.max(1, window.devicePixelRatio || 1);
    const backingWidth = Math.round(cssWidth * pixelRatio);
    const backingHeight = Math.round(cssHeight * pixelRatio);
    if (canvas.width !== backingWidth || canvas.height !== backingHeight) {
        canvas.width = backingWidth;
        canvas.height = backingHeight;
    }
    // ImGui coordinates and browser pointer coordinates are both CSS pixels.
    sendF32F32(0x17, cssWidth, cssHeight);
}

function connect() {
    clientId = 0;
    pendingSends = [];
    sendFlushQueued = false;
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    ws = new WebSocket(`${proto}//${location.host}/ws`);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
        statusEl.textContent = "connected";
        statusEl.className = "connected";
        console.log("[imgui_wasm] WebSocket connected");
    };

    ws.onclose = () => {
        clientId = 0;
        pendingSends = [];
        statusEl.textContent = "disconnected - reconnecting...";
        statusEl.className = "disconnected";
        setTimeout(connect, 1000);
    };

    ws.onerror = (e) => { console.error("[imgui_wasm] WS error", e); };

    ws.onmessage = (evt) => {
        if (typeof evt.data === "string") return;
        const data = new Uint8Array(evt.data);
        if (data.length < 1) return;

        const msgType = data[0];
        switch (msgType) {
            case 0x06: {
                const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
                clientId = dv.getUint32(1, true);
                console.log("[imgui_wasm] Assigned client ID:", clientId);
                break;
            }
            case 0x0a: {
                const magic = String.fromCharCode(data[1], data[2], data[3], data[4]);
                if (magic !== "IMGW" || data.length < 9) {
                    ws.close(1002, "unsupported imgui-wasm protocol");
                    break;
                }
                sendHelloAck();
                // Synchronize client state after the protocol acknowledgement;
                // the server deliberately sends no state before negotiation.
                resize();
                if (navigator.clipboard && navigator.clipboard.readText) {
                    navigator.clipboard.readText().then((text) => {
                        if (text) sendClipboardText(text);
                    }).catch(() => {});
                }
                break;
            }
            case 0x02: {
                const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
                let off = 1;
                const id_lo = dv.getUint32(off, true); off += 4;
                const id_hi = dv.getUint32(off, true); off += 4;
                const id = id_lo + id_hi * 0x100000000;
                const w = dv.getUint32(off, true); off += 4;
                const h = dv.getUint32(off, true); off += 4;
                const pixLen = dv.getUint32(off, true); off += 4;
                const pixels = data.slice(off, off + pixLen);
                if (!(id === 1 && _twinFontAtlasActive)) {
                    uploadTexture(id, w, h, pixels);
                }
                break;
            }
            case 0x07: {
                // Call-stream frame (server replays via the WASM twin). The
                // helper script is loaded once, then handles every frame.
                loadCallstreamHelper().then((cs) => {
                    cs.handleFrame(data, canvas, callstreamHooks());
                });
                break;
            }
            case 0x09: {
                loadCallstreamHelper().then((cs) => {
                    cs.handleStringUpdate(data);
                });
                break;
            }
            case 0x18: {
                const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
                const textLen = dv.getUint32(1, true);
                const textBytes = data.slice(5, 5 + textLen);
                const text = new TextDecoder().decode(textBytes);
                if (navigator.clipboard && navigator.clipboard.writeText) {
                    navigator.clipboard.writeText(text).catch(() => {});
                }
                break;
            }
        }
    };
}

function getCanvasPos(e) {
    const rect = canvas.getBoundingClientRect();
    const cssX = e.clientX - rect.left;
    const cssY = e.clientY - rect.top;
    // A frame from the previously active client may briefly be scaled to this
    // canvas. Hit-test in the coordinates of the frame actually on screen.
    const logicalWidth = lastDsw > 0 ? lastDsw : rect.width;
    const logicalHeight = lastDsh > 0 ? lastDsh : rect.height;
    return {
        x: cssX * logicalWidth / Math.max(1, rect.width),
        y: cssY * logicalHeight / Math.max(1, rect.height),
    };
}

canvas.addEventListener("pointermove", (e) => {
    const p = getCanvasPos(e);
    queueMouseMove(p.x, p.y);
    // Mirror to the WASM replay twin (if active) so hover visuals track the
    // cursor between server round-trips. The server stays authoritative.
    feedCallstreamInput("mouse_pos", p.x, p.y);
});

canvas.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    canvas.setPointerCapture(e.pointerId);
    const p = getCanvasPos(e);
    flushMouseMove();
    // ImGui must see the click position before the button transition. Without
    // this, a first click (with no preceding move) targets the stale position.
    sendF32F32(0x10, p.x, p.y);
    sendU8(0x11, e.button);
    feedCallstreamInput("mouse_pos", p.x, p.y);
    feedCallstreamInput("mouse_down", e.button);
});

function releasePointer(e) {
    const p = getCanvasPos(e);
    sendF32F32(0x10, p.x, p.y);
    sendU8(0x12, e.button);
    feedCallstreamInput("mouse_pos", p.x, p.y);
    feedCallstreamInput("mouse_up", e.button);
    if (canvas.hasPointerCapture(e.pointerId)) {
        canvas.releasePointerCapture(e.pointerId);
    }
}

canvas.addEventListener("pointerup", releasePointer);
canvas.addEventListener("pointercancel", releasePointer);

canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    const dx = e.deltaX * 0.01, dy = -e.deltaY * 0.01;
    sendF32F32(0x13, dx, dy);
    feedCallstreamInput("wheel", dx, dy);
}, { passive: false });

canvas.addEventListener("contextmenu", (e) => e.preventDefault());

const keysDown = new Set();

document.addEventListener("keydown", (e) => {
    const key = IMGUI_KEY_MAP[e.code];
    if (key === undefined) return;
    if (e.ctrlKey && !e.altKey && e.code === "KeyV") {
        if (!keysDown.has(IMGUI_KEY_MAP["ControlLeft"])) {
            keysDown.add(IMGUI_KEY_MAP["ControlLeft"]);
            sendU16(0x14, IMGUI_KEY_MAP["ControlLeft"]);
            feedCallstreamInput("key", IMGUI_KEY_MAP["ControlLeft"], true);
        }
        return;
    }
    e.preventDefault();
    if (!keysDown.has(key)) {
        keysDown.add(key);
        sendU16(0x14, key);
        feedCallstreamInput("key", key, true);
    }
    if (e.key && e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
        const cp = e.key.codePointAt(0);
        sendU32(0x16, cp);
        feedCallstreamInput("char", cp);
    }
});

document.addEventListener("paste", (e) => {
    const text = e.clipboardData && e.clipboardData.getData("text/plain");
    if (text) {
        sendClipboardText(text);
        for (let i = 0; i < text.length; i++) {
            const cp = text.codePointAt(i);
            sendU32(0x16, cp);
            feedCallstreamInput("char", cp);
            if (cp > 0xFFFF) i++;
        }
    }
});

document.addEventListener("keyup", (e) => {
    const key = IMGUI_KEY_MAP[e.code];
    if (key !== undefined) {
        e.preventDefault();
        keysDown.delete(key);
        sendU16(0x15, key);
        feedCallstreamInput("key", key, false);
    }
});

window.addEventListener("blur", () => {
    for (const key of keysDown) {
        sendU16(0x15, key);
        feedCallstreamInput("key", key, false);
    }
    keysDown.clear();
});

window.addEventListener("resize", resize);
if (window.ResizeObserver) {
    new ResizeObserver(resize).observe(canvas);
}

resize();
connect();
