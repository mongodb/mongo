// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/handoff/handoff_listener_thread.h"
#include "mongo/transport/handoff/handoff_posix_interface.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"

#include <filesystem>
#include <memory>
#include <string_view>

#include <s2n.h>

#include <boost/optional.hpp>
#include <sys/types.h>

namespace mongo::transport {

/**
 * `HandoffTransportLayer` is a `TransportLayer` implementation that listens on a set of dedicated
 * unix domain sockets. A secure pre-auth process accepts client connections on behalf of the
 * database and forwards those clients to the database through these unix domain sockets.
 *
 * For each connection accepted on one of the unix domain sockets, `HandoffTransportLayer` creates a
 * `HandoffSession` instance and sends it to the `SessionManager` provided on construction.
 *
 * `HandoffTransportLayer` is ingress-only. It does not implement the egress portions of the
 * `TransportLayer` interface.
 *
 * `HandoffTransportLayer` also does not support the SSL-related portions of the `TransportLayer`
 * interface, because `HandoffSession` does not terminate TLS. See `handoff_session.h` for more
 * information.
 */
class HandoffTransportLayer : public TransportLayer {
public:
    struct Params {
        /** Path to the directory in which unix domain sockets will be created. */
        std::filesystem::path socketPrefix;
        /**
         * The primary port that the server is configured to listen on. This transport layer does
         * not bind to any ports, but the port number is appended to the names of unix domain
         * sockets that this transport layer creates, to avoid naming collisions on hosts where
         * multiple mongod/s servers are running.
         */
        int port;
        /**
         * The unix user group ID that will be owner of the created unix domain sockets, and
         * additionally connected processes must have as their user primary group ID. If unset, the
         * group ID of the current process will be used instead.
         */
        boost::optional<gid_t> socketGroupID;
        /**
         * The size of the kernel queue for unaccepted connections. Each listening socket has its
         * own queue. `listenBacklog` must be greater than zero.
         */
        int listenBacklog;
        /**
         * The session manager to which new sessions (accepted connections) will be sent.
         * `sessionManager` must not be null.
         */
        std::unique_ptr<SessionManager> sessionManager;
        /**
         * Test-only dependency injection for libc functions used by `HandoffTransportLayer`.
         * If null, which is the default, `HandoffTransportLayer` uses the libc functions.
         * Unit tests supply a value for `posix` to simulate failures.
         */
        std::unique_ptr<POSIXInterface> posix = nullptr;
    };

    explicit HandoffTransportLayer(Params);
    ~HandoffTransportLayer() override;

    /** unimplemented: returns an error */
    StatusWith<std::shared_ptr<Session>> connect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        const boost::optional<TransientSSLParams>& transientSSLParams = boost::none) override;

    /** unimplemented: returns an error */
    Future<std::shared_ptr<Session>> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext) override;

    /**
     * Binds unix domain sockets managed by this object, but does not start accepting connections.
     * Once `setup()` has succeeded, `shutdown()` must be called before this object is destroyed.
     * `setup()` must not be called more than once.
     */
    Status setup() override;

    /** Begins accepting connections. `start()` must not be called more than once. */
    Status start() override;

    /**
     * Stops accepting connections and waits for all active sessions to end, waiting for up to an
     * unspecified timeout, and then closes all files managed by this object.
     * `shutdown()` must be called before this object is destroyed if `setup()` was ever successful.
     * `shutdown()` is idempotent.
     */
    void shutdown() override;

    /**
     * Stops accepting connections. `stopAcceptingSessions()` is idempotent.
     */
    void stopAcceptingSessions() override;

    std::string_view getNameForLogging() const override;

    /** unimplemented: returns nullptr */
    ReactorHandle getReactor(WhichReactor which) override;

    TransportProtocol getTransportProtocol() const override;

    SessionManager* getSessionManager() const override;

    std::shared_ptr<SessionManager> getSharedSessionManager() const override;

    bool isIngress() const override;

    bool isEgress() const override;

#ifdef MONGO_CONFIG_SSL
    /** unimplemented: returns an error */
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface>,
                              bool /*asyncOCSPStaple*/) override;

    /** unimplemented: returns an error */
    StatusWith<std::shared_ptr<const SSLConnectionContext>> createTransientSSLContext(
        const TransientSSLParams&) override;
#endif

private:
    const std::unique_ptr<POSIXInterface> _posix;
    const std::shared_ptr<SessionManager> _sessionManager;

    s2n_config* _s2nConfig;

    /** thread that accepts connections on the priority socket */
    HandoffListenerThread _priorityListenerThread;
    /** thread that accepts connections on all other sockets */
    HandoffListenerThread _standardListenerThread;
};

}  // namespace mongo::transport
