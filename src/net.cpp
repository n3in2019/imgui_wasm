// net.cpp — HTTP request parsing, SHA-1, base64, and WebSocket framing.

#include "net.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace imgui_wasm_core {
namespace net {

// --- raw socket helpers ------------------------------------------------------

bool send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;  // dead peer
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void configure_client_socket(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Bounded blocking so a dead peer cannot wedge a thread forever; the
    // receive loop treats a timeout as "nothing yet" and re-checks shutdown.
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// --- HTTP ---------------------------------------------------------------------

std::optional<HttpRequest> read_http_request(int fd) {
    std::string raw;
    char chunk[2048];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        if (raw.size() > 16 * 1024) return std::nullopt;
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return std::nullopt;
        }
        raw.append(chunk, static_cast<size_t>(n));
    }

    HttpRequest req;
    size_t line_end = raw.find("\r\n");
    std::string request_line = raw.substr(0, line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = request_line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) return std::nullopt;
    req.method = request_line.substr(0, sp1);
    req.path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    size_t pos = line_end + 2;
    while (pos < raw.size()) {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos || eol == pos) break;
        std::string line = raw.substr(pos, eol - pos);
        pos = eol + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        for (auto& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        size_t vstart = colon + 1;
        while (vstart < line.size() && line[vstart] == ' ') vstart++;
        req.headers[name] = line.substr(vstart);
    }
    return req;
}

void send_http_response(int fd, const char* status, const char* content_type,
                        const void* body, size_t body_len) {
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, content_type, body_len);
    if (n <= 0) return;
    send_all(fd, head, static_cast<size_t>(n));
    send_all(fd, body, body_len);
}

// --- SHA-1 + base64 ------------------------------------------------------------

namespace {

struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t total = 0;
    uint8_t buf[64];
    size_t buf_len = 0;

    static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
                   (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n > 0) {
            size_t take = std::min(n, sizeof(buf) - buf_len);
            memcpy(buf + buf_len, p, take);
            buf_len += take;
            p += take;
            n -= take;
            if (buf_len == sizeof(buf)) {
                block(buf);
                buf_len = 0;
            }
        }
    }

    void finish(uint8_t out[20]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buf_len != 56) update(&zero, 1);
        uint8_t len_be[8];
        for (int i = 0; i < 8; i++) len_be[i] = uint8_t(bits >> (56 - 8 * i));
        update(len_be, 8);
        for (int i = 0; i < 5; i++) {
            out[4 * i] = uint8_t(h[i] >> 24);
            out[4 * i + 1] = uint8_t(h[i] >> 16);
            out[4 * i + 2] = uint8_t(h[i] >> 8);
            out[4 * i + 3] = uint8_t(h[i]);
        }
    }
};

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16;
        if (i + 1 < n) v |= uint32_t(p[i + 1]) << 8;
        if (i + 2 < n) v |= uint32_t(p[i + 2]);
        out.push_back(kBase64[(v >> 18) & 63]);
        out.push_back(kBase64[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? kBase64[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? kBase64[v & 63] : '=');
    }
    return out;
}

}  // namespace

std::string ws_accept_key(const std::string& key) {
    const std::string guided = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 sha;
    sha.update(reinterpret_cast<const uint8_t*>(guided.data()), guided.size());
    uint8_t digest[20];
    sha.finish(digest);
    return base64_encode(digest, sizeof(digest));
}

bool send_ws_upgrade(int fd, const std::string& ws_key) {
    std::string accept = ws_accept_key(ws_key);
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " +
                       accept + "\r\n\r\n";
    return send_all(fd, resp.data(), resp.size());
}

// --- WebSocket framing ---------------------------------------------------------

namespace {

bool recv_exact(int fd, uint8_t* p, size_t n) {
    while (n > 0) {
        ssize_t got = ::recv(fd, p, n, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return false;
        p += got;
        n -= static_cast<size_t>(got);
    }
    return true;
}

// Writes a server frame as [header (<=14 bytes, stack)][payload] without
// copying the payload: the hot broadcast path sends the same frame buffer to
// every client. Single writev keeps the write atomic under the send mutex.
bool send_frame(int fd, uint8_t first_byte, const uint8_t* payload, size_t len) {
    uint8_t hdr[10];
    size_t hdr_len = 0;
    hdr[hdr_len++] = first_byte;
    if (len < 126) {
        hdr[hdr_len++] = static_cast<uint8_t>(len);
    } else if (len <= 0xFFFF) {
        hdr[hdr_len++] = 126;
        hdr[hdr_len++] = static_cast<uint8_t>(len >> 8);
        hdr[hdr_len++] = static_cast<uint8_t>(len);
    } else {
        hdr[hdr_len++] = 127;
        for (int i = 7; i >= 0; i--) hdr[hdr_len++] = static_cast<uint8_t>(len >> (8 * i));
    }
    iovec iov[2] = {{hdr, hdr_len}, {const_cast<uint8_t*>(payload), len}};
    size_t sent = 0;
    size_t total = hdr_len + len;
    while (sent < total) {
        ssize_t n = ::writev(fd, iov, 2);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;  // dead peer
        }
        sent += static_cast<size_t>(n);
        if (sent < total) {
            // Partial write: slide the iovecs past the bytes already sent.
            size_t skip = sent;
            for (int i = 0; i < 2; i++) {
                if (skip == 0) break;
                size_t take = std::min(skip, iov[i].iov_len);
                iov[i].iov_base = static_cast<char*>(iov[i].iov_base) + take;
                iov[i].iov_len -= take;
                skip -= take;
            }
        }
    }
    return true;
}

}  // namespace

bool WsConn::send_binary(const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lk(send_mtx);
    return send_frame(fd, 0x82, payload.data(), payload.size());
}

bool WsConn::send_pong(const std::vector<uint8_t>& payload) {
    // RFC 6455 caps control-frame payloads at 125 bytes; a larger ping is
    // answered with a truncated pong (the length byte must never claim the
    // 126/127 extended-length encodings without their extension bytes).
    std::lock_guard<std::mutex> lk(send_mtx);
    return send_frame(fd, 0x8A, payload.data(), std::min(payload.size(), size_t(125)));
}

bool WsConn::send_close() {
    uint8_t frame[2] = {0x88, 0x00};
    std::lock_guard<std::mutex> lk(send_mtx);
    return send_all(fd, frame, sizeof(frame));
}

WsReader::Result WsReader::read_message(WsConn& conn, std::vector<uint8_t>& payload,
                                        bool& was_text) {
    for (;;) {
        uint8_t hdr[2];
        ssize_t first = ::recv(conn.fd, hdr, 1, 0);
        if (first < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return Result::Idle;
            return Result::Error;
        }
        if (first == 0) return Result::Closed;
        if (!recv_exact(conn.fd, &hdr[1], 1)) return Result::Error;

        bool fin = (hdr[0] & 0x80) != 0;
        int opcode = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        uint64_t len = hdr[1] & 0x7F;
        if (len == 126) {
            uint8_t ext[2];
            if (!recv_exact(conn.fd, ext, 2)) return Result::Error;
            len = (uint64_t(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!recv_exact(conn.fd, ext, 8)) return Result::Error;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
        }
        if (len > 64u * 1024 * 1024) return Result::Error;

        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked && !recv_exact(conn.fd, mask, 4)) return Result::Error;

        std::vector<uint8_t> data(static_cast<size_t>(len));
        if (len > 0 && !recv_exact(conn.fd, data.data(), data.size())) return Result::Error;
        if (masked) {
            for (size_t i = 0; i < data.size(); i++) data[i] ^= mask[i & 3];
        }

        switch (opcode) {
            case 0x8:
                conn.send_close();
                return Result::Closed;
            case 0x9:
                conn.send_pong(data);
                continue;
            case 0xA:
                continue;  // unsolicited pong
            default:
                break;
        }

        if (opcode == 0x0) {
            // Continuation of a fragmented message.
            if (!fragmenting) return Result::Error;
            fragment_acc.insert(fragment_acc.end(), data.begin(), data.end());
            if (fin) {
                payload = std::move(fragment_acc);
                fragment_acc.clear();
                fragmenting = false;
                was_text = false;  // binary is all we care about
                return Result::Message;
            }
            continue;
        }

        if (opcode == 0x1 || opcode == 0x2) {
            if (!fin) {
                fragment_acc = std::move(data);
                fragmenting = true;
                continue;
            }
            payload = std::move(data);
            was_text = (opcode == 0x1);
            return Result::Message;
        }

        // Unknown opcode: drop.
    }
}

}  // namespace net
}  // namespace imgui_wasm_core
