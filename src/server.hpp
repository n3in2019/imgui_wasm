// server.hpp — HTTP/WebSocket server handle for the pure-C++ core.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core.hpp"

namespace imgui_wasm_core {

// Owns the listener and tracks live client sockets so shutdown can wake
// every blocked thread. Client threads hold shared_ptr copies, so the state
// outlives any in-flight session.
struct ServerHandle {
    std::atomic<bool> stopped{false};
    int listen_fd = -1;

    void track_socket(int fd);
    void untrack_socket(int fd);
    // shutdown(2)s every tracked socket: wakes blocked IO without freeing
    // descriptors (the owning WsConn closes them when its refcount hits 0,
    // so fd numbers are never closed twice or recycled under a live user).
    void close_all_sockets();

   private:
    std::mutex sockets_mtx_;
    std::vector<int> sockets_;
};

// Binds and starts the accept thread. Returns nullptr on failure.
std::shared_ptr<ServerHandle> run_server(std::shared_ptr<State> state, const char* host,
                                         uint16_t port);
void stop_server(std::shared_ptr<ServerHandle> server);

}  // namespace imgui_wasm_core
