// server.cpp — HTTP/WebSocket server for the pure-C++ core.
//
// Uses POSIX blocking sockets: one accept thread; per client a receiver
// thread (parses input) and a condvar-driven sender thread (control queue
// drains before the latest-frame slot).

#include "server.hpp"

#include "net.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace imgui_wasm_core {

namespace {

using net::WsConn;
using net::WsReader;

bool is_websocket_upgrade(const net::HttpRequest& req) {
    auto it = req.headers.find("upgrade");
    if (it == req.headers.end()) return false;
    std::string v = it->second;
    for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return v == "websocket";
}

void serve_static(int fd, const char* url_path) {
    for (size_t i = 0; i < kEmbeddedAssetCount; i++) {
        const Asset& a = kEmbeddedAssets[i];
        if (strcmp(a.url_path, url_path) == 0) {
            net::send_http_response(fd, "200 OK", a.content_type, a.data, a.size);
            return;
        }
    }
    const char* not_found = "not found";
    net::send_http_response(fd, "404 Not Found", "text/plain; charset=utf-8", not_found, 9);
}

// --- WebSocket session --------------------------------------------------------

// Waits for the capability ack within a fixed budget. Client input can
// legitimately beat the ack onto the wire and some clients batch the ack
// together with such input; anything whose first record is not the ack is
// dropped and the window keeps waiting rather than failing the connection.
bool wait_capability_ack(WsConn& conn, WsReader& reader, ClientId client_id, uint32_t& caps_out,
                         const std::atomic<bool>& stopped) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !stopped.load(std::memory_order_acquire)) {
        std::vector<uint8_t> payload;
        bool was_text = false;
        auto result = reader.read_message(conn, payload, was_text);
        if (result == WsReader::Result::Idle) continue;  // keep waiting within budget
        if (result != WsReader::Result::Message || was_text) return false;
        auto msgs = parse_client_msgs(payload.data(), payload.size());
        if (!msgs) continue;
        auto caps = leading_hello_ack(*msgs, client_id);
        if (caps.has_value()) {
            caps_out = *caps;
            return true;
        }
    }
    return false;
}

void sender_loop(std::shared_ptr<OutBox> out, std::shared_ptr<net::WsConn> conn);

void sender_thread(std::shared_ptr<OutBox> out, std::shared_ptr<net::WsConn> conn) {
    // A detached thread must never let an exception escape: that would
    // std::terminate the host process. Failures here only end this sender.
    try {
        sender_loop(out, conn);
    } catch (...) {
        out->closed.store(true, std::memory_order_release);
        out->cv.notify_all();
    }
}

void sender_loop(std::shared_ptr<OutBox> out, std::shared_ptr<net::WsConn> conn) {
    std::vector<std::vector<uint8_t>> pending_control;
    std::shared_ptr<const std::vector<uint8_t>> frame;
    while (!out->closed.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(out->mtx);
        out->cv.wait_for(lk, std::chrono::milliseconds(200), [&] {
            return out->closed.load(std::memory_order_acquire) || !out->control.empty() ||
                   out->frame_seq != out->sent_seq;
        });
        if (out->closed.load(std::memory_order_acquire)) break;

        // Control first: prerequisites of frames (string tables must precede
        // the call-stream frames referencing them, textures the draws
        // sampling them).
        pending_control.clear();
        while (!out->control.empty()) {
            pending_control.push_back(std::move(out->control.front()));
            out->control.pop_front();
        }
        bool has_frame = false;
        if (out->frame_seq != out->sent_seq) {
            // Refcount move only — the bytes are shared across clients.
            frame = std::move(out->frame);
            out->frame.reset();
            out->sent_seq = out->frame_seq;
            has_frame = true;
        }
        lk.unlock();

        for (auto& msg : pending_control) {
            if (!conn->send_binary(msg)) {
                out->closed.store(true, std::memory_order_release);
                out->cv.notify_all();
                return;
            }
        }
        if (has_frame) {
            // Cleared before the socket send so the coalescing detector only
            // trips on frames truly replaced unobserved, never in-flight ones.
            out->unobserved.store(false, std::memory_order_release);
            if (!conn->send_binary(*frame)) {
                out->closed.store(true, std::memory_order_release);
                out->cv.notify_all();
                return;
            }
        }
    }
}

void handle_ws(std::shared_ptr<State> state, std::shared_ptr<ServerHandle> server,
               std::shared_ptr<net::WsConn> conn, ClientId client_id,
               std::shared_ptr<OutBox> out) {
    WsReader reader;

    // The done: cleanup (client removal) must run even when an exception
    // escapes; a detached thread letting one out would terminate the process.
    try {
        // Client id assignment, then the capability hello; no further server
        // state until the client acks.
        std::vector<uint8_t> assign_msg = {0x06, uint8_t(client_id), uint8_t(client_id >> 8),
                                           uint8_t(client_id >> 16), uint8_t(client_id >> 24)};
        if (!conn->send_binary(assign_msg)) goto done;
        {
            std::vector<uint8_t> hello;
            hello.reserve(9);
            hello.push_back(0x0a);
            hello.insert(hello.end(), {'I', 'M', 'G', 'W'});
            uint32_t server_caps = 1u | (1u << 1);  // capability bits: 0 legacy, 1 call-stream
            for (int i = 0; i < 4; i++) hello.push_back(uint8_t(server_caps >> (8 * i)));
            if (!conn->send_binary(hello)) goto done;
        }

        {
            uint32_t capabilities = 0;
            if (!wait_capability_ack(*conn, reader, client_id, capabilities, server->stopped))
                goto done;
            state->set_client_capabilities(client_id, capabilities);
        }

        // Replay existing textures + the full interned string table so the new
        // client can decode frames immediately.
        for (const auto& tex : state->snapshot_textures()) {
            if (!conn->send_binary(tex)) goto done;
        }
        {
            auto strings = capture::snapshot_all_strings();
            if (!strings.empty()) {
                std::vector<uint8_t> msg = serialize_string_update(strings);
                if (!conn->send_binary(msg)) goto done;
            }
        }

        // Sender thread streams control + frames; this thread becomes the
        // receiver.
        {
            std::thread sender([out, conn] { sender_thread(out, conn); });
            sender.detach();

            while (!server->stopped.load(std::memory_order_acquire) &&
                   !out->closed.load(std::memory_order_acquire)) {
                std::vector<uint8_t> payload;
                bool was_text = false;
                auto result = reader.read_message(*conn, payload, was_text);
                if (result == WsReader::Result::Idle) continue;  // healthy keep-wait
                if (result != WsReader::Result::Message) break;
                if (was_text) continue;

                auto msgs = parse_client_msgs(payload.data(), payload.size());
                if (!msgs) continue;
                for (auto& [cid, msg] : *msgs) {
                    // A connection may only mutate the state assigned to it:
                    // prevents cross-client input spoofing and rejects stale
                    // messages left over around a reconnect.
                    if (cid != client_id) continue;
                    switch (msg.kind) {
                        case ClientMsg::Kind::HelloAck:
                            break;
                        case ClientMsg::Kind::Input:
                            state->push_input(cid, msg.input);
                            break;
                        case ClientMsg::Kind::Resize:
                            state->set_display_size(cid, msg.resize_w, msg.resize_h);
                            break;
                        case ClientMsg::Kind::ClipboardText:
                            state->on_clipboard_text(cid, msg.clipboard_text);
                            break;
                    }
                }
            }
        }
    } catch (...) {
        fprintf(stderr, "[imgui_wasm] connection handler failed\n");
    }

done:
    state->remove_client(client_id);  // idempotent; marks the out-box closed
    conn->send_close();
}

void connection_thread(std::shared_ptr<State> state, std::shared_ptr<ServerHandle> server, int fd) {
    // RAII ownership of the raw fd until it is handed to a WsConn; guarantees
    // close on every exit path, including exceptions.
    struct FdGuard {
        int fd;
        ~FdGuard() {
            if (fd >= 0) ::close(fd);
        }
    } guard{fd};

    try {
        net::configure_client_socket(fd);
        auto req = net::read_http_request(fd);
        if (!req) return;

        if (req->path == "/ws" && is_websocket_upgrade(*req)) {
            auto key_it = req->headers.find("sec-websocket-key");
            if (key_it == req->headers.end()) return;
            if (!net::send_ws_upgrade(fd, key_it->second)) return;

            auto conn = std::make_shared<net::WsConn>(fd);
            guard.fd = -1;  // ownership transferred to the WsConn
            ClientId client_id = state->add_client();
            std::shared_ptr<OutBox> out;
            state->with_clients([&](std::unordered_map<ClientId, ClientState>& clients) {
                auto it = clients.find(client_id);
                out = it != clients.end() ? it->second.out : nullptr;
            });
            if (out) {
                server->track_socket(fd);
                handle_ws(state, server, conn, client_id, out);
                server->untrack_socket(fd);
            }
            // The WsConn destructor closes fd when the last thread (sender or
            // receiver) releases its shared_ptr.
            return;
        }

        serve_static(fd, req->path.c_str());
    } catch (...) {
        fprintf(stderr, "[imgui_wasm] connection thread failed\n");
    }
}

}  // namespace

void ServerHandle::track_socket(int fd) {
    std::lock_guard<std::mutex> lk(sockets_mtx_);
    sockets_.push_back(fd);
}

void ServerHandle::untrack_socket(int fd) {
    std::lock_guard<std::mutex> lk(sockets_mtx_);
    sockets_.erase(std::remove(sockets_.begin(), sockets_.end(), fd), sockets_.end());
}

void ServerHandle::close_all_sockets() {
    std::lock_guard<std::mutex> lk(sockets_mtx_);
    // shutdown(2) only: wakes blocked IO without freeing descriptors. The
    // owning WsConn performs the real close, so fd numbers are never closed
    // twice or recycled under a live user.
    for (int fd : sockets_) ::shutdown(fd, SHUT_RDWR);
    sockets_.clear();
}

std::shared_ptr<ServerHandle> run_server(std::shared_ptr<State> state, const char* host,
                                         uint16_t port) {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "[imgui_wasm] socket() failed: %s\n", strerror(errno));
        return nullptr;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "[imgui_wasm] invalid bind address '%s'\n", host);
        ::close(listen_fd);
        return nullptr;
    }
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(listen_fd, 16) < 0) {
        fprintf(stderr, "[imgui_wasm] bind/listen on %s:%u failed: %s\n", host, port,
                strerror(errno));
        ::close(listen_fd);
        return nullptr;
    }

    auto server = std::make_shared<ServerHandle>();
    server->listen_fd = listen_fd;
    server->stopped.store(false, std::memory_order_release);

    fprintf(stderr, "[imgui_wasm] Server listening on http://%s:%u\n", host, port);

    std::thread acceptor([state, server, listen_fd] {
        while (!server->stopped.load(std::memory_order_acquire)) {
            int fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) continue;
                break;  // listener closed (shutdown) or fatal
            }
            try {
                std::thread([state, server, fd] { connection_thread(state, server, fd); }).detach();
            } catch (...) {
                // Thread creation failed (resource exhaustion): drop the
                // connection rather than terminating the process.
                ::close(fd);
            }
        }
    });
    acceptor.detach();
    return server;
}

void stop_server(std::shared_ptr<ServerHandle> server) {
    if (!server) return;
    server->stopped.store(true, std::memory_order_release);
    if (server->listen_fd >= 0) {
        ::shutdown(server->listen_fd, SHUT_RDWR);
        ::close(server->listen_fd);
        server->listen_fd = -1;
    }
    // Wake blocked receiver threads; senders observe closed via their out-box.
    server->close_all_sockets();
}

}  // namespace imgui_wasm_core
