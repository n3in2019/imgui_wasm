// net.hpp — minimal HTTP + RFC 6455 WebSocket plumbing (POSIX sockets).

#pragma once

#include <unistd.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace imgui_wasm_core {
namespace net {

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;  // lower-cased names
};

// Reads one request head (terminated by \r\n\r\n, capped at 16 KiB).
std::optional<HttpRequest> read_http_request(int fd);

bool send_all(int fd, const void* data, size_t len);
void send_http_response(int fd, const char* status, const char* content_type,
                        const void* body, size_t body_len);
// Writes the 101 upgrade response for the given Sec-WebSocket-Key.
bool send_ws_upgrade(int fd, const std::string& ws_key);

// --- WebSocket frames -------------------------------------------------------
//
// One send mutex per connection: the sender thread streams frames while the
// receiver thread may answer pings; whole-frame writes under the mutex keep
// the byte stream well-formed. Server frames are never masked.

struct WsConn {
    int fd;
    std::mutex send_mtx;

    explicit WsConn(int f) : fd(f) {}
    ~WsConn() {
        if (fd >= 0) ::close(fd);
    }
    WsConn(const WsConn&) = delete;
    WsConn& operator=(const WsConn&) = delete;

    bool send_binary(const std::vector<uint8_t>& payload);
    bool send_close();
   private:
    bool send_pong(const std::vector<uint8_t>& payload);
    friend struct WsReader;
};

struct WsReader {
    std::vector<uint8_t> fragment_acc;
    bool fragmenting = false;

    enum class Result { Message, Closed, Error, Idle };
    // Blocks until a complete binary/text message is assembled. Pings are
    // answered inline; pongs are dropped; close returns Closed. Idle means
    // the receive timeout expired with no bytes read (healthy keep-wait).
    Result read_message(WsConn& conn, std::vector<uint8_t>& payload, bool& was_text);
};

// Computes the Sec-WebSocket-Accept value (base64(SHA1(key + GUID))).
std::string ws_accept_key(const std::string& key);

// Decodes standard base64 (used for HTTP Basic credentials). Returns false
// on non-base64 input.
bool base64_decode(const std::string& in, std::vector<uint8_t>& out);

// Socket options shared by client connections: bounded send/recv so a dead
// peer cannot wedge a thread forever.
void configure_client_socket(int fd);

}  // namespace net
}  // namespace imgui_wasm_core
