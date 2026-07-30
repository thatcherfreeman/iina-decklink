/*
 * A minimal RFC 6455 WebSocket client.
 *
 * The plugin runs the server (iina.ws.createServer) because that is the only
 * socket primitive IINA exposes to JavaScript — there is no raw TCP — so the
 * helper connects outward to it.  That direction turns out to be useful
 * anyway: when IINA quits or the plugin is disabled the connection drops, and
 * the helper treats that as its cue to exit and release the card.  Without it
 * a stranded helper would hold the device open with nothing able to reclaim it,
 * since utils.exec gives the plugin no way to kill a process.
 *
 * Only what the control channel needs is implemented: a single connection,
 * text and binary data frames, ping/pong, and close.  No extensions, no
 * fragmentation on send.
 */

#ifndef IINA_DECKLINK_WS_CLIENT_H
#define IINA_DECKLINK_WS_CLIENT_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WsClient {
public:
    using MessageHandler = std::function<void(const std::string &)>;

    WsClient() = default;
    ~WsClient();

    WsClient(const WsClient &) = delete;
    WsClient &operator=(const WsClient &) = delete;

    // url is ws://host:port/path — the only scheme the plugin ever hands us.
    bool connect(const std::string &url, std::string *err);

    // Starts the read loop on its own thread.  on_message is called from that
    // thread for every complete text or binary frame.
    void start(MessageHandler on_message);

    // Closes the socket and joins the read thread.
    void stop();

    bool send(const std::string &text);
    bool connected() const { return connected_; }

    // Seconds since the last frame arrived.  The helper uses this as a
    // watchdog: a plugin that has gone away stops sending position reports
    // long before the socket itself notices.
    double seconds_since_last_message() const;

private:
    bool handshake(const std::string &host, const std::string &path,
                   std::string *err);
    void read_loop();
    bool send_frame(uint8_t opcode, const void *data, size_t len);
    bool read_exact(void *buf, size_t len);

    int  fd_ = -1;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::thread       thread_;
    std::mutex        send_mutex_;
    MessageHandler    on_message_;
    std::atomic<double> last_message_at_{0.0};

    // Bytes read past the end of the handshake response, which belong to the
    // first frame.
    std::vector<uint8_t> pending_;
};

#endif  // IINA_DECKLINK_WS_CLIENT_H
