// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/handoff/handoff_listener_thread.h"

#include "mongo/base/status_with.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/transport/handoff/handoff_session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/scopeguard.h"

#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <fmt/format.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {
namespace {
/**
 * Logs the LOG_ID with a message and attributes indicated by the trailing arguments (...) at
 * DEFAULT_SEVERITY at most once every COOLDOWN_PERIOD, and at COOLDOWN_SEVERITY otherwise.
 *
 * For example, the following will log a message at error severity at most once every 10 seconds,
 * and at debug(2) severity at other times:
 *
 *     LOG_SUPPRESSED(123456, // ID
 *                 Seconds(10), // cooldown period
 *                 logv2::LogSeverity::Error(), // default severity
 *                 logv2::LogSeverity::Debug(2), // cooldown severity
 *                 "Here is the message that will be logged", // message
 *                 "some attribute"_attr = some_attribute_value); // zero or more attributes
 */
#define LOG_SUPPRESSED(LOG_ID, COOLDOWN_PERIOD, DEFAULT_SEVERITY, COOLDOWN_SEVERITY, ...) \
    do {                                                                                  \
        static logv2::SeveritySuppressor suppressor{                                      \
            (COOLDOWN_PERIOD), (DEFAULT_SEVERITY), (COOLDOWN_SEVERITY)};                  \
        LOGV2_DEBUG((LOG_ID), suppressor().toInt(), __VA_ARGS__);                         \
    } while (false)

Status validatePeerGroupID(POSIXInterface& posix, int fd, const boost::optional<gid_t>& expected) {
    uid_t peerUid;
    gid_t peerGid;
    if (posix.getpeereid(fd, &peerUid, &peerGid) != 0) {
        return Status(ErrorCodes::InternalError,
                      fmt::format("HandoffTransportLayer: getpeereid() failed: {}",
                                  errorMessage(lastSystemError())));
    }

    const gid_t expectedGid = expected ? *expected : posix.getegid();
    if (peerGid != expectedGid) {
        return Status(
            ErrorCodes::Unauthorized,
            fmt::format(
                "HandoffTransportLayer: peer process user group ID mismatch: expected {}, got {}",
                expectedGid,
                peerGid));
    }

    return Status::OK();
}

/**
 * Helper for cleaning up a variable number of things when returning/throwing from a function, with
 * the option of skipping the cleanup.
 * When `CleanupGuard` is destroyed, its `actions` are executed in the reverse order in which they
 * were added, i.e. it's a stack.
 */
struct CleanupGuard {
    std::vector<std::function<void()>> actions;
    ~CleanupGuard() {
        while (!actions.empty()) {
            actions.back()();
            actions.pop_back();
        }
    }
};

Status setNonBlocking(POSIXInterface& posix,
                      int fd,
                      std::string_view nameForErrorDiagnostic,
                      ErrorCodes::Error errorCode) {
    const int flags = posix.fcntl(fd, F_GETFL);
    if (flags == -1) {
        return Status(errorCode,
                      fmt::format("HandoffTransportLayer: fcntl(F_GETFL) failed on {}: {}",
                                  nameForErrorDiagnostic,
                                  errorMessage(lastSystemError())));
    }
    if (posix.fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return Status(
            errorCode,
            fmt::format("HandoffTransportLayer: fcntl(F_SETFL, O_NONBLOCK) failed on {}: {}",
                        nameForErrorDiagnostic,
                        errorMessage(lastSystemError())));
    }
    return Status::OK();
}

/**
 * Creates a unix domain socket bound to the specified `path`, unlinking any existing file if
 * necessary, and sets the ownership of the new file to the optionally specified `groupID`. Returns
 * the resulting socket file descriptor or an error.
 */
StatusWith<int> bindNewSocket(POSIXInterface& posix,
                              const std::filesystem::path& path,
                              boost::optional<gid_t> groupID) {
    posix.unlink(path.c_str());

    const int default_protocol = 0;
    const int fd = posix.socket(AF_UNIX, SOCK_STREAM, default_protocol);
    if (fd == -1) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("HandoffTransportLayer: failed to create unix domain socket "
                                  "at {}: {}",
                                  path.string(),
                                  errorMessage(lastSystemError())));
    }
    CleanupGuard cleanup;
    cleanup.actions.push_back([&]() { posix.close(fd); });

    // Set the listener socket to nonblocking so that `accept()` can't block.
    Status status = setNonBlocking(posix, fd, path.string(), ErrorCodes::SocketException);
    if (!status.isOK()) {
        return status;
    }

    ::sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (path.native().size() >= sizeof(addr.sun_path)) {
        return Status(
            ErrorCodes::InvalidOptions,
            fmt::format("HandoffTransportLayer: socket path too long: {}", path.string()));
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (posix.bind(fd, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr)) == -1) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("HandoffTransportLayer: bind failed on {}: {}",
                                  path.string(),
                                  errorMessage(lastSystemError())));
    }
    cleanup.actions.push_back([&]() { posix.unlink(path.c_str()); });

    if (posix.chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) != 0) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("HandoffTransportLayer: chmod failed on {}: {}",
                                  path.string(),
                                  errorMessage(lastSystemError())));
    }

    if (groupID && posix.chown(path.c_str(), -1, *groupID) == -1) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("HandoffTransportLayer: chown failed on {}: {}",
                                  path.string(),
                                  errorMessage(lastSystemError())));
    }

    cleanup.actions.clear();
    return fd;
}

}  // namespace

HandoffListenerThread::HandoffListenerThread(Params params)
    : _posix(params.posix),
      _sockets(std::move(params.sockets)),
      _socketGroupID(params.socketGroupID),
      _sessionManager(params.sessionManager),
      _s2nConfig(nullptr),  // specified in `setup(...)`
      _transportLayer(params.transportLayer),
      _listenBacklog(params.listenBacklog),
      _threadName(std::move(params.threadName)) {
    invariant(!_sockets.empty());
    invariant(_sessionManager);
    invariant(_listenBacklog > 0);
}

Status HandoffListenerThread::setup(SetupParams params) {
    _s2nConfig = params.s2nConfig;

    ScopeGuard cleanupOnError{[this]() {
        for (ListeningSocket& sock : _sockets) {
            if (sock.fd == -1) {
                continue;
            }
            _posix.close(sock.fd);
            sock.fd = -1;
            _posix.unlink(sock.path.c_str());
        }
        for (int& fd : _wakePipe) {
            if (fd == -1) {
                continue;
            }
            _posix.close(fd);
            fd = -1;
        }
    }};

    for (ListeningSocket& sock : _sockets) {
        const StatusWith<int> swFd = bindNewSocket(_posix, sock.path, _socketGroupID);
        if (!swFd.isOK()) {
            return swFd.getStatus();
        }
        sock.fd = swFd.getValue();
    }

    // Make a pipe and set its write end to be nonblocking, so that using the pipe to wake up the
    // listener thread never blocks the calling thread.
    // Realistically the pipe buffer is guaranteed to be at least 512 bytes, but no limit is better.
    if (_posix.pipe(_wakePipe) != 0) {
        return Status(ErrorCodes::InternalError,
                      fmt::format("HandoffTransportLayer: pipe() failed: {}",
                                  errorMessage(lastSystemError())));
    }
    Status status = setNonBlocking(_posix, _wakePipe[1], "pipe", ErrorCodes::InternalError);
    if (!status.isOK()) {
        return status;
    }

    cleanupOnError.dismiss();
    return status;
}

Status HandoffListenerThread::listen() {
    for (const ListeningSocket& sock : _sockets) {
        if (_posix.listen(sock.fd, _listenBacklog) == -1) {
            return Status(ErrorCodes::SocketException,
                          fmt::format("HandoffTransportLayer: listen failed on {}: {}",
                                      sock.path.string(),
                                      errorMessage(lastSystemError())));
        }
    }
    return Status::OK();
}

void HandoffListenerThread::start() {
    _listenerThread = stdx::thread([this] { _listenerLoop(); });
}

void HandoffListenerThread::stopAcceptingSessions() {
    if (!_listenerThread.joinable()) {
        return;
    }

    // Wake the listener thread out of poll().
    if (_wakePipe[1] >= 0) {
        const char byte = '\0';  // the value doesn't matter
        int rc;
        do {
            rc = _posix.write(_wakePipe[1], &byte, sizeof byte);
        } while (rc == -1 && errno == EINTR);
        if (rc == -1) {
            LOGV2_ERROR(
                12779308, "write to pipe failed", "msg"_attr = errorMessage(lastSystemError()));
        }
    }

    _listenerThread.join();
}

void HandoffListenerThread::shutdown() {
    stopAcceptingSessions();
    // in case there was no thread for `stopAcceptingSessions()` to join
    _clearListeners();

    for (int& fd : _wakePipe) {
        if (fd == -1) {
            continue;
        }
        _posix.close(fd);
        fd = -1;
    }
}

void HandoffListenerThread::_clearListeners() {
    for (ListeningSocket& sock : _sockets) {
        if (sock.fd == -1) {
            continue;
        }
        _posix.close(sock.fd);
        sock.fd = -1;
        _posix.unlink(sock.path.c_str());
    }
}

void HandoffListenerThread::_listenerLoop() {
    setThreadName(_threadName);

    std::vector<::pollfd> interests;
    // One `pollfd` for each of `_sockets`, and one more for `_wakePipe`.
    // We're interested in the input event ("POLLIN"). On a listening socket that means a connection
    // has arrived. On a pipe it means that data has been written to the pipe.
    for (const ListeningSocket& sock : _sockets) {
        interests.push_back(::pollfd{sock.fd, POLLIN, 0});
    }
    interests.push_back(::pollfd{_wakePipe[0], POLLIN, 0});

    for (;;) {
        const int no_timeout = -1;
        int rc = _posix.poll(interests.data(), interests.size(), no_timeout);
        if (rc == -1) {
            switch (errno) {
                case EINTR:
                    continue;  // poll() was interrupted by a signal, so retry
                default:
                    // Other error conditions are:
                    // - EAGAIN if the kernel is low on memory, but might recover
                    // - EINVAL if we invoked `poll` improperly, which we didn't.
                    // So, log and retry.
                    LOG_SUPPRESSED(12779304,
                                   Seconds(1),
                                   logv2::LogSeverity::Error(),
                                   logv2::LogSeverity::Debug(2),
                                   "HandoffTransportLayer: poll() failed",
                                   "error"_attr = errorMessage(lastSystemError()));
                    continue;
            }
        }

        // The "wake pipe" was written to, indicating that we should stop accepting new connections.
        if (interests.back().revents & POLLIN) {
            break;
        }

        // The first `_sockets.size()` elements of `interests` correspond one-to-one with the
        // elements of `_sockets`. The final element of `interests` is the pipe, which we checked
        // already above so we skip it here.
        for (std::size_t i = 0; i < interests.size() - 1; ++i) {
            if (!(interests[i].revents & POLLIN)) {
                continue;
            }

            const int connFd = _posix.accept(interests[i].fd, nullptr, nullptr);
            if (connFd == -1) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                LOG_SUPPRESSED(12779305,
                               Seconds(1),
                               logv2::LogSeverity::Error(),
                               logv2::LogSeverity::Debug(2),
                               "HandoffTransportLayer: accept() failed",
                               "error"_attr = errorMessage(lastSystemError()));
                continue;
            }

            if (const Status status = validatePeerGroupID(_posix, connFd, _socketGroupID);
                !status.isOK()) {
                LOG_SUPPRESSED(12779306,
                               Seconds(1),
                               logv2::LogSeverity::Error(),
                               logv2::LogSeverity::Debug(2),
                               "HandoffTransportLayer: rejecting connection due to peer process "
                               "user group ID mismatch",
                               "unixSocket"_attr = _sockets[i].path.string(),
                               "error"_attr = status);
                _posix.close(connFd);
                continue;
            }

            _sessionManager->startSession(std::make_shared<HandoffSession>(HandoffSession::Params{
                .fd = connFd,
                .localAddress = _sockets[i].path,
                .transportLayer = _transportLayer,
                .acceptedOnLoadBalancedSocket = _sockets[i].isLoadBalanced,
                .acceptedOnPrioritySocket = _sockets[i].isPriority,
                .s2nConfig = _s2nConfig,
                .posix = _posix,
            }));
        }
    }

    _clearListeners();
}

}  // namespace mongo::transport
