use axum::body::Body;
use axum::extract::ws::{Message, WebSocket};
use axum::extract::{State, WebSocketUpgrade};
use axum::http::header;
use axum::http::StatusCode;
use axum::response::{Html, IntoResponse, Response};
use axum::routing::get;
use axum::Router;
use futures_util::{SinkExt, StreamExt};
use std::sync::Arc;

use crate::state::ImGuiWasmState;

static INDEX_HTML: &str = include_str!("../frontend/index.html");
static IMGUI_WASM_JS: &str = include_str!("../frontend/imgui_wasm.js");
static IMGUI_WASM_CSS: &str = include_str!("../frontend/style.css");
static IMGUI_WASM_CALLSTREAM_JS: &str = include_str!("../frontend/imgui_wasm_callstream.js");
// The WASM replay twin is rebuilt by every Cargo build and embedded here.
static IMGUI_WASM_REPLAY_JS: &str = include_str!("../wasm/imgui_wasm_replay.js");
static IMGUI_WASM_REPLAY_WASM: &[u8] = include_bytes!("../wasm/imgui_wasm_replay.wasm");
async fn get_index() -> impl IntoResponse {
    (
        [(header::CONTENT_TYPE, "text/html; charset=utf-8")],
        Html(INDEX_HTML),
    )
}

async fn get_js() -> impl IntoResponse {
    (
        [(
            header::CONTENT_TYPE,
            "application/javascript; charset=utf-8",
        )],
        IMGUI_WASM_JS,
    )
}

async fn get_callstream_js() -> impl IntoResponse {
    (
        [(
            header::CONTENT_TYPE,
            "application/javascript; charset=utf-8",
        )],
        IMGUI_WASM_CALLSTREAM_JS,
    )
}

async fn get_replay_js() -> Response {
    (
        StatusCode::OK,
        [(
            header::CONTENT_TYPE,
            "application/javascript; charset=utf-8",
        )],
        IMGUI_WASM_REPLAY_JS,
    )
        .into_response()
}

async fn get_replay_wasm() -> Response {
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "application/wasm")
        .body(Body::from(IMGUI_WASM_REPLAY_WASM))
        .unwrap()
}

async fn get_css() -> impl IntoResponse {
    (
        [(header::CONTENT_TYPE, "text/css; charset=utf-8")],
        IMGUI_WASM_CSS,
    )
}

async fn ws_handler(
    ws: WebSocketUpgrade,
    State(state): State<Arc<ImGuiWasmState>>,
) -> impl IntoResponse {
    ws.on_upgrade(move |socket| handle_ws(socket, state))
}

async fn handle_ws(mut socket: WebSocket, state: Arc<ImGuiWasmState>) {
    let (client_id, mut client_rx) = state.add_client();

    let assign_msg = {
        let mut msg = Vec::with_capacity(5);
        msg.push(0x06);
        msg.extend_from_slice(&client_id.to_le_bytes());
        msg
    };
    if socket
        .send(Message::Binary(assign_msg.into()))
        .await
        .is_err()
    {
        state.remove_client(client_id);
        return;
    }

    {
        let tex_data_list: Vec<Vec<u8>> = {
            let textures = state.textures.lock().unwrap();
            textures.values().cloned().collect()
        };
        for tex_data in tex_data_list {
            if socket.send(Message::Binary(tex_data.into())).await.is_err() {
                state.remove_client(client_id);
                return;
            }
        }
    }

    // Call-stream transport: replay the full interned string table so the new
    // client can decode frames immediately. Without this the client would miss
    // the string-update (0x09) messages broadcast before it subscribed (the
    // table is populated on the first server frame, which may race connect).
    if crate::is_callstream_enabled() {
        let strings = crate::capture::snapshot_all_strings();
        if !strings.is_empty() {
            let msg = crate::callstream_protocol::serialize_string_update(&strings);
            if socket.send(Message::Binary(msg.into())).await.is_err() {
                state.remove_client(client_id);
                return;
            }
        }
    }

    let mut rx = state.frame_tx.subscribe();
    let (mut sender, mut receiver) = socket.split();

    let send_task = tokio::spawn(async move {
        loop {
            tokio::select! {
                result = rx.recv() => match result {
                    Ok(frame_data) => {
                        if sender
                            .send(Message::Binary(frame_data.into()))
                            .await
                            .is_err()
                        {
                            break;
                        }
                    }
                    Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => continue,
                    Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
                },
                Some(frame_data) = client_rx.recv() => {
                    if sender.send(Message::Binary(frame_data.into())).await.is_err() {
                        break;
                    }
                },
            }
        }
    });

    let state_clone = state.clone();
    let recv_task = tokio::spawn(async move {
        while let Some(Ok(msg)) = receiver.next().await {
            if let Message::Binary(data) = msg {
                if let Some(messages) = crate::state::parse_client_msgs(&data) {
                    for (cid, client_msg) in messages {
                        // A connection may only mutate the state assigned to it.
                        // Besides preventing cross-client input spoofing, this
                        // rejects stale messages left over around a reconnect.
                        if cid != client_id {
                            continue;
                        }
                        match client_msg {
                            crate::state::ClientMsg::Input(ev) => state_clone.push_input(cid, ev),
                            crate::state::ClientMsg::Resize { w, h } => {
                                state_clone.set_display_size(cid, w, h)
                            }
                            crate::state::ClientMsg::ClipboardText(text) => {
                                state_clone.on_clipboard_text(cid, text)
                            }
                        }
                    }
                }
            }
        }
    });

    tokio::select! {
        _ = send_task => {},
        _ = recv_task => {},
    }

    state.remove_client(client_id);
}

pub async fn run_server(state: Arc<ImGuiWasmState>, addr: std::net::SocketAddr) {
    let app = Router::new()
        .route("/", get(get_index))
        .route("/imgui_wasm.js", get(get_js))
        .route("/imgui_wasm_callstream.js", get(get_callstream_js))
        .route("/imgui_wasm_replay.js", get(get_replay_js))
        .route("/imgui_wasm_replay.wasm", get(get_replay_wasm))
        .route("/style.css", get(get_css))
        .route("/ws", get(ws_handler))
        .with_state(state);

    let listener = tokio::net::TcpListener::bind(addr).await.unwrap();
    eprintln!("[imgui_wasm] Server listening on http://{}", addr);
    axum::serve(listener, app).await.unwrap();
}
