#include "portal.hpp"

#include "log.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <libportal/portal.h>

namespace lsfg {

static const char* TAG = "portal";

PortalSession::~PortalSession() {
    if (session_) {
        xdp_session_close(session_);
        g_object_unref(session_);
    }
    if (portal_)
        g_object_unref(portal_);
}

bool PortalSession::begin(const std::string& restore_token) {
    portal_ = xdp_portal_new();
    if (!portal_) {
        fail("failed to connect to xdg-desktop-portal (is it running?)");
        return false;
    }
    state_ = State::CreatingSession;
    logInfo(TAG, "requesting screencast session (window capture)%s",
            restore_token.empty() ? "" : " with restore token");
    xdp_portal_create_screencast_session(
        portal_, XDP_OUTPUT_WINDOW, XDP_SCREENCAST_FLAG_NONE,
        XDP_CURSOR_MODE_HIDDEN, XDP_PERSIST_MODE_PERSISTENT,
        restore_token.empty() ? nullptr : restore_token.c_str(), nullptr,
        reinterpret_cast<GAsyncReadyCallback>(&PortalSession::onSessionCreated),
        this);
    return true;
}

void PortalSession::onSessionCreated(void* src, void* res, void* data) {
    auto* self = static_cast<PortalSession*>(data);
    GError* err = nullptr;
    self->session_ = xdp_portal_create_screencast_session_finish(
        XDP_PORTAL(src), G_ASYNC_RESULT(res), &err);
    if (!self->session_) {
        self->fail(std::string("create session: ") +
                   (err ? err->message : "unknown error"));
        g_clear_error(&err);
        return;
    }
    g_signal_connect(self->session_, "closed",
                     G_CALLBACK(&PortalSession::onSessionClosed), self);
    self->state_ = State::Starting;
    logInfo(TAG, "session created, opening picker (select the window to capture)");
    xdp_session_start(
        self->session_, nullptr, nullptr,
        reinterpret_cast<GAsyncReadyCallback>(&PortalSession::onSessionStarted),
        self);
}

void PortalSession::onSessionStarted(void* src, void* res, void* data) {
    auto* self = static_cast<PortalSession*>(data);
    GError* err = nullptr;
    if (!xdp_session_start_finish(XDP_SESSION(src), G_ASYNC_RESULT(res), &err)) {
        self->fail(std::string("start session (picker cancelled?): ") +
                   (err ? err->message : "unknown error"));
        g_clear_error(&err);
        return;
    }

    GVariant* streams = xdp_session_get_streams(self->session_);
    if (!streams) {
        self->fail("session started but no streams were granted");
        return;
    }
    GVariantIter iter;
    g_variant_iter_init(&iter, streams);
    uint32_t node_id = 0;
    GVariant* props = nullptr;
    if (!g_variant_iter_next(&iter, "(u@a{sv})", &node_id, &props)) {
        self->fail("could not parse stream list");
        return;
    }
    if (props)
        g_variant_unref(props);

    int fd = xdp_session_open_pipewire_remote(self->session_);
    if (fd < 0) {
        self->fail("OpenPipeWireRemote failed");
        return;
    }
    self->stream_.pipewire_fd = fd;
    self->stream_.node_id = node_id;
    self->state_ = State::Done;
    logInfo(TAG, "screencast granted: pipewire node %u", node_id);
}

void PortalSession::onSessionClosed(void*, void* data) {
    auto* self = static_cast<PortalSession*>(data);
    logWarn(TAG, "screencast session closed by compositor/user");
    if (self->on_closed)
        self->on_closed();
}

void PortalSession::fail(const std::string& msg) {
    error_ = msg;
    state_ = State::Failed;
    logError(TAG, "%s", msg.c_str());
}

bool PortalSession::pump() {
    while (g_main_context_iteration(nullptr, FALSE))
        ;
    return state_ == State::CreatingSession || state_ == State::Starting;
}

std::string PortalSession::restoreToken() const {
    if (!session_)
        return {};
    char* tok = xdp_session_get_restore_token(session_);
    if (!tok)
        return {};
    std::string out = tok;
    g_free(tok);
    return out;
}

} // namespace lsfg
