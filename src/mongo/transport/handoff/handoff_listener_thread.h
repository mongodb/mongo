// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/handoff/handoff_posix_interface.h"

#include <filesystem>
#include <vector>

#include <s2n.h>

#include <boost/optional.hpp>
#include <sys/types.h>

namespace mongo::transport {

class SessionManager;
class TransportLayer;

/**
 * `HandoffListenerThread` manages a thread of execution that accepts connections on a configured
 * set of unix domain sockets, and for each creates a `HandoffSession` and passes it to a configured
 * `SessionManager`.
 *
 * `HandoffListenerThread` is an implementation detail of `HandoffTransportLayer`.
 *
 * `HandoffListenerThread` deals with three kinds of files.
 *
 * - The listening sockets:
 *     - purpose: New connections are accepted on these unix domain sockets.
 *     - count: Specified in `Params::sockets`.
 *     - lifecycle: Created in `setup()`, closed and deleted in `shutdown()`.
 * - Client sockets:
 *     - purpose: Data transfer with clients that connected through one of the listening sockets.
 *     - count: One per incoming connection.
 *     - lifecycle: Created when `accept()` succeeds in `_listenerLoop()`. Closed on error, but
 *       otherwise ownership is transferred to a new `HandoffSession` instance.
 * - Wake pipe:
 *     - purpose: Signals the `_listenerThread` to return from `poll()` and shut down.
 *     - count: One pipe, with a file descriptor each for the read and write ends.
 *     - lifecycle: Created in `setup()`, closed in `shutdown()`.
 */
class HandoffListenerThread {
public:
    struct ListeningSocket {
        /** File descriptor of the listening unix domain socket, or `-1` if closed or unbound. */
        int fd = -1;
        /** File path to which `fd` is bound. */
        std::filesystem::path path;
        /**
         * Indicates whether connections accepted on this socket are expected to be reverse proxied
         * through an L4 load balancer.
         */
        bool isLoadBalanced;
        /**
         * Indicates whether connections accepted on this socket should be exempt from session
         * limits and rate limiters.
         */
        bool isPriority;
    };

    struct Params {
        /** The name of the listener thread is used for logging. */
        std::string threadName;
        /**
         * The unix domain sockets that this listener will create, bind, listen, and accept
         * connections on.
         * `sockets` must be nonempty.
         */
        std::vector<ListeningSocket> sockets;
        /**
         * The unix user group ID that will be owner of the created unix domain sockets, and
         * additionally connected processes must have as their user primary group ID. If unset, the
         * group ID of the current process will be used.
         */
        boost::optional<gid_t> socketGroupID;
        /**
         * The size of the kernel queue for unaccepted connections. Each listening socket has its
         * own queue. `listenBacklog` must be greater than zero.
         */
        int listenBacklog;
        /** The session manager to which `HandoffSession` objects will be sent. */
        SessionManager* sessionManager;
        /**
         * The transport layer that owns this `HandoffListenerThread`. It's passed to
         * `HandoffSession` instances when they are constructed.
         */
        TransportLayer* transportLayer;
        /** The implementation of system functions that the transport layer is using. */
        POSIXInterface& posix;
    };

    explicit HandoffListenerThread(Params);

    struct SetupParams {
        /*
         * The TLS configuration to use for the s2n_connection into which the handed-off
         * TLS state will be deserialized.
         * `s2nConfig` must be non-null, except for in unit tests.
         */
        const s2n_config* s2nConfig;
    };

    /**
     * Binds unix domain sockets managed by this object, but does not start accepting connections.
     * Once `setup()` has succeeded, `shutdown()` must be called before this object is destroyed.
     * `setup()` must not be called more than once.
     * If `setup()` fails, then any bound unix domain sockets are deleted.
     */
    Status setup(SetupParams);

    /**
     * Starts listening on the sockets managed by this object, but does not begin accepting
     * connections.
     */
    Status listen();

    /**
     * Spawns the listener thread that begins accepting connections.
     * `start()` must not be called more than once.
     */
    void start();

    /**
     * Signals the listener thread to stop accepting connections, close its listener sockets, and
     * joins the listener thread if applicable. `stopAcceptingSessions` is idempotent.
     * `stopAcceptingSessions` must not be called concurrently from two or more threads.
     */
    void stopAcceptingSessions();

    /**
     * Stops accepting connections, joins the listener thread if applicable, and closes all files
     * managed by this object.
     * `shutdown` must not be called concurrently from two or more threads.
     */
    void shutdown();

private:
    /**
     * Called from the listener thread when it detects that it has been told to stop listening, or
     * called from `shutdown()` if the listener thread has not started. Closes and deletes the
     * listener sockets.
     */
    void _clearListeners();

    /**
     * The body of `_listenerThread` loops forever accepting connections until
     * `stopAcceptingConnections` or `shutdown` is called.
     */
    void _listenerLoop();

    POSIXInterface& _posix;
    std::vector<ListeningSocket> _sockets;
    boost::optional<gid_t> _socketGroupID;
    SessionManager* _sessionManager;
    const s2n_config* _s2nConfig;
    TransportLayer* _transportLayer;
    /**
     * Self-pipe for waking the listener thread on shutdown. The listener thread monitors for new
     * connections using `poll()` -- in order to wake it up, we need to have a file in the pollset
     * that we can write to. That's the pipe.
     * - `_wakePipe[0]` is the end of the pipe to read from
     * - `_wakePipe[1]` is the end of the pipe to write to
     */
    int _wakePipe[2]{-1, -1};
    int _listenBacklog;
    stdx::thread _listenerThread;
    std::string _threadName;
};

}  // namespace mongo::transport
