#pragma once

#include <cstdint>
#include <functional>
#include <string>

typedef struct _XdpPortal XdpPortal;
typedef struct _XdpSession XdpSession;

namespace lsfg {

// Drives the xdg-desktop-portal ScreenCast handshake via libportal:
// CreateSession -> SelectSources (window picker dialog) -> Start ->
// OpenPipeWireRemote. Asynchronous; pump() must be called until finished.
class PortalSession {
public:
    struct Stream {
        int pipewire_fd = -1;
        uint32_t node_id = 0;
    };

    ~PortalSession();

    // restore_token may be empty; when valid the portal skips the picker
    // dialog and re-grants the previous selection.
    bool begin(const std::string& restore_token);

    // Iterates the GLib main context. Returns true while still in progress.
    bool pump();

    bool succeeded() const { return state_ == State::Done; }
    const std::string& error() const { return error_; }
    const Stream& stream() const { return stream_; }
    std::string restoreToken() const;

    // Fired (from pump()) when the compositor closes the session, e.g. the
    // user hits the "stop sharing" button or the source window goes away.
    std::function<void()> on_closed;

private:
    enum class State { Idle, CreatingSession, Starting, Done, Failed };

    static void onSessionCreated(void* src, void* res, void* data);
    static void onSessionStarted(void* src, void* res, void* data);
    static void onSessionClosed(void* session, void* data);
    void fail(const std::string& msg);

    XdpPortal* portal_ = nullptr;
    XdpSession* session_ = nullptr;
    State state_ = State::Idle;
    std::string error_;
    Stream stream_;
};

} // namespace lsfg
