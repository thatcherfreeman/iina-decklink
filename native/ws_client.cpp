#include "ws_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#include <CommonCrypto/CommonDigest.h>

#include "log.h"

namespace {

// The fixed GUID from RFC 6455 §1.3, concatenated with the client key to form
// the server's expected accept token.
constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string base64(const uint8_t *data, size_t len)
{
    static const char *t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= (uint32_t)data[i + 2];
        out += t[(n >> 18) & 63];
        out += t[(n >> 12) & 63];
        out += (i + 1 < len) ? t[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? t[n & 63] : '=';
    }
    return out;
}

double now_seconds()
{
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ws://host:port/path
bool parse_ws_url(const std::string &url, std::string *host, std::string *port,
                  std::string *path)
{
    const std::string prefix = "ws://";
    if (url.compare(0, prefix.size(), prefix) != 0)
        return false;
    std::string rest = url.substr(prefix.size());

    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    *path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        *host = authority;
        *port = "80";
    } else {
        *host = authority.substr(0, colon);
        *port = authority.substr(colon + 1);
    }
    return !host->empty() && !port->empty();
}

}  // namespace

WsClient::~WsClient()
{
    stop();
}

bool WsClient::connect(const std::string &url, std::string *err)
{
    std::string host, port, path;
    if (!parse_ws_url(url, &host, &port, &path)) {
        if (err)
            *err = "malformed WebSocket URL: " + url;
        return false;
    }

    struct addrinfo hints = {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        if (err)
            *err = std::string("could not resolve ") + host + ": " + gai_strerror(rc);
        return false;
    }

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd_ = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd_ < 0)
            continue;
        if (::connect(fd_, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        ::close(fd_);
        fd_ = -1;
    }
    freeaddrinfo(res);

    if (fd_ < 0) {
        if (err)
            *err = "could not connect to " + host + ":" + port;
        return false;
    }

    // Control messages are small and latency-sensitive; Nagle would batch them.
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (!handshake(host + ":" + port, path, err)) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    connected_ = true;
    last_message_at_.store(now_seconds());
    return true;
}

bool WsClient::handshake(const std::string &host, const std::string &path,
                         std::string *err)
{
    uint8_t nonce[16];
    std::random_device rd;
    for (uint8_t &b : nonce)
        b = (uint8_t)(rd() & 0xFF);
    std::string key = base64(nonce, sizeof(nonce));

    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     path.c_str(), host.c_str(), key.c_str());
    // ::send, not the member function of the same name.
    if (::send(fd_, req, (size_t)n, 0) != n) {
        if (err)
            *err = "could not send the WebSocket handshake";
        return false;
    }

    // Read until the end of the response headers.  Anything after them is the
    // start of the first frame and has to be kept.
    std::string response;
    char buf[1024];
    size_t header_end = std::string::npos;
    while (response.size() < 16384) {
        ssize_t got = recv(fd_, buf, sizeof(buf), 0);
        if (got <= 0) {
            if (err)
                *err = "connection closed during the WebSocket handshake";
            return false;
        }
        response.append(buf, (size_t)got);
        header_end = response.find("\r\n\r\n");
        if (header_end != std::string::npos)
            break;
    }
    if (header_end == std::string::npos) {
        if (err)
            *err = "WebSocket handshake response too large";
        return false;
    }

    if (response.compare(0, 12, "HTTP/1.1 101") != 0) {
        if (err)
            *err = "server refused the WebSocket upgrade: " +
                   response.substr(0, response.find("\r\n"));
        return false;
    }

    // Verify the accept token, which is what distinguishes a real WebSocket
    // peer from anything else that happens to answer on that port.
    std::string expected_input = key + kWsGuid;
    uint8_t digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(expected_input.data(), (CC_LONG)expected_input.size(), digest);
    std::string expected = base64(digest, sizeof(digest));

    std::string lowered = response.substr(0, header_end);
    for (char &c : lowered)
        c = (char)tolower((unsigned char)c);
    size_t pos = lowered.find("sec-websocket-accept:");
    if (pos == std::string::npos) {
        if (err)
            *err = "handshake response had no Sec-WebSocket-Accept header";
        return false;
    }
    size_t value_start = response.find_first_not_of(" \t", pos + 21);
    size_t value_end   = response.find("\r\n", value_start);
    std::string got_token = response.substr(value_start, value_end - value_start);
    if (got_token != expected) {
        if (err)
            *err = "handshake accept token mismatch";
        return false;
    }

    size_t body_start = header_end + 4;
    pending_.assign(response.begin() + (long)body_start, response.end());
    return true;
}

void WsClient::start(MessageHandler on_message)
{
    on_message_ = std::move(on_message);
    stopping_   = false;
    thread_     = std::thread(&WsClient::read_loop, this);
}

void WsClient::stop()
{
    stopping_ = true;
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
    }
    if (thread_.joinable())
        thread_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    connected_ = false;
}

bool WsClient::read_exact(void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    size_t filled = 0;

    // Drain anything left over from the handshake read first.
    if (!pending_.empty()) {
        size_t take = pending_.size() < len ? pending_.size() : len;
        memcpy(out, pending_.data(), take);
        pending_.erase(pending_.begin(), pending_.begin() + (long)take);
        filled = take;
    }

    while (filled < len) {
        ssize_t got = recv(fd_, out + filled, len - filled, 0);
        if (got <= 0)
            return false;
        filled += (size_t)got;
    }
    return true;
}

void WsClient::read_loop()
{
    std::string message;   // accumulates across fragmented frames
    uint8_t message_opcode = 0;

    while (!stopping_) {
        uint8_t header[2];
        if (!read_exact(header, 2))
            break;

        bool    fin    = (header[0] & 0x80) != 0;
        uint8_t opcode = header[0] & 0x0F;
        bool    masked = (header[1] & 0x80) != 0;
        uint64_t len   = header[1] & 0x7F;

        if (len == 126) {
            uint8_t ext[2];
            if (!read_exact(ext, 2))
                break;
            len = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!read_exact(ext, 8))
                break;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = (len << 8) | ext[i];
        }

        // A server must not mask, but handle it rather than desynchronising
        // the stream if one does.
        uint8_t mask[4] = {};
        if (masked && !read_exact(mask, 4))
            break;

        // Guard against a malformed or hostile length claiming gigabytes.
        if (len > (16u << 20)) {
            log_error("websocket: frame of %llu bytes is implausible — closing",
                      (unsigned long long)len);
            break;
        }

        std::string payload;
        payload.resize((size_t)len);
        if (len > 0 && !read_exact(&payload[0], (size_t)len))
            break;
        if (masked) {
            for (size_t i = 0; i < payload.size(); i++)
                payload[i] = (char)(payload[i] ^ mask[i % 4]);
        }

        last_message_at_.store(now_seconds());

        switch (opcode) {
        case 0x0:  // continuation
            message += payload;
            break;
        case 0x1:  // text
        case 0x2:  // binary — IINA's sendText arrives this way
            message = payload;
            message_opcode = opcode;
            break;
        case 0x8:  // close
            log_debug("websocket: server closed the connection");
            connected_ = false;
            return;
        case 0x9:  // ping
            send_frame(0xA, payload.data(), payload.size());
            continue;
        case 0xA:  // pong
            continue;
        default:
            log_debug("websocket: ignoring unknown opcode 0x%x", opcode);
            continue;
        }

        if (fin && (message_opcode == 0x1 || message_opcode == 0x2)) {
            if (on_message_)
                on_message_(message);
            message.clear();
        }
    }

    connected_ = false;
}

bool WsClient::send_frame(uint8_t opcode, const void *data, size_t len)
{
    std::lock_guard<std::mutex> guard(send_mutex_);
    if (fd_ < 0)
        return false;

    uint8_t header[14];
    size_t  hlen = 0;
    header[hlen++] = (uint8_t)(0x80 | opcode);   // FIN + opcode

    // Clients must mask every frame.
    if (len < 126) {
        header[hlen++] = (uint8_t)(0x80 | len);
    } else if (len <= 0xFFFF) {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (uint8_t)((len >> 8) & 0xFF);
        header[hlen++] = (uint8_t)(len & 0xFF);
    } else {
        header[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            header[hlen++] = (uint8_t)((len >> (i * 8)) & 0xFF);
    }

    uint8_t mask[4];
    std::random_device rd;
    for (uint8_t &b : mask)
        b = (uint8_t)(rd() & 0xFF);
    memcpy(header + hlen, mask, 4);
    hlen += 4;

    std::string frame((const char *)header, hlen);
    frame.reserve(hlen + len);
    const uint8_t *src = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        frame += (char)(src[i] ^ mask[i % 4]);

    size_t sent = 0;
    while (sent < frame.size()) {
        ssize_t n = ::send(fd_, frame.data() + sent, frame.size() - sent, 0);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

bool WsClient::send(const std::string &text)
{
    return send_frame(0x1, text.data(), text.size());
}

double WsClient::seconds_since_last_message() const
{
    return now_seconds() - last_message_at_.load();
}
