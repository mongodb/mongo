/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/transport/handoff/handoff_session.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/auth_options_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/handoff/session_handoff_message_gen.h"
#include "mongo/transport/proxy_protocol_header_parser.h"
#include "mongo/transport/proxy_protocol_tlv_extraction.h"
#include "mongo/transport/transport_options_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"

#include <cerrno>
#include <filesystem>
#include <limits>
#include <mutex>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <sys/socket.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {
using namespace std::literals::string_view_literals;

namespace {

constexpr size_t kHeaderSize = sizeof(MSGHEADER::Value);

// The size of the PROXY v2 fixed header: 12-byte signature + 1-byte version/command + 1-byte
// address family + 2-byte length.
constexpr size_t kProxyV2FixedHeaderSize = 16;

// Number of bytes to peek: enough to identify PROXY v1 ("PROXY", 5 bytes) or the start of
// the PROXY v2 signature ("\r\n\r\n", first 4 bytes of kProxyV2Signature).
constexpr size_t kProxyPeekSize = transport::kProxyV1Signature.size();

/**
 * Given an s2n return code, returns OK on S2N_SUCCESS or an InternalError with the s2n error
 * message on S2N_FAILURE.
 */
Status s2nCheck(int result, std::string_view op) {
    if (result == S2N_SUCCESS) {
        return Status::OK();
    }
    return Status(ErrorCodes::InternalError,
                  fmt::format("{} failed: {}", op, s2n_strerror(s2n_errno, "EN")));
}

std::string toString(HandoffSession::State state) {
    switch (state) {
        case HandoffSession::State::Cleartext:
            return "cleartext";
        case HandoffSession::State::TLS:
            return "tls";
    }
    MONGO_UNREACHABLE;
}

HostAndPort toHostAndPort(const SockAddr& sa) {
    // For a unix domain socket, `SockAddr::getPort` will return zero, but the convention with
    // `HostAndPort` is that unix domain sockets have port -1, which is interpreted by `HostAndPort`
    // to mean "no port."
    return HostAndPort(sa.getAddr(), sa.getType() == AF_UNIX ? -1 : sa.getPort());
}

/**
 * Polls fd for readability. Returns OK if data is available, Returns NetworkTimeout on timeout, or
 * SocketException on error or peer disconnect. timeoutMs of -1 blocks indefinitely.
 */
Status pollForRead(POSIXInterface& posix, int fd, int timeoutMs) {
    tassert(12938601, "Invalid file descriptor", fd >= 0);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret;
    do {
        ret = posix.poll(&pfd, 1, timeoutMs);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("poll failed: {}", errorMessage(lastSocketError())));
    }
    if (ret == 0) {
        return Status(ErrorCodes::NetworkTimeout, "Timed out waiting for data");
    }
    return Status::OK();
}

Status setBlocking(POSIXInterface& posix, int fd, std::string_view nameForErrorDiagnostic) {
    const int flags = posix.fcntl(fd, F_GETFL);
    if (flags == -1) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("HandoffSession: fcntl(F_GETFL) failed on {}: {}",
                                  nameForErrorDiagnostic,
                                  errorMessage(lastSystemError())));
    }
    if (posix.fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("HandoffSession: set blocking mode failed on {}: {}",
                                  nameForErrorDiagnostic,
                                  errorMessage(lastSystemError())));
    }
    return Status::OK();
}

}  // namespace

HandoffSession::HandoffSession(Params params)
    : Session(/* isIngress= */ true),
      _fd(params.fd),
      _state(State::Cleartext),
      _isShutDown(false),
      _endpointsBeforeHandoff{.local = HostAndPort(params.localAddress.string()),
                              .remote = HostAndPort("anonymous unix socket")},
      _s2nConnection(nullptr),
      _s2nConfig(params.s2nConfig),
      _posix(params.posix),
      _tl(params.transportLayer) {
    _isConnectedToLoadBalancerPort = params.acceptedOnLoadBalancedSocket;
    _isConnectedToPriorityPort = params.acceptedOnPrioritySocket;
    _isConnectedToProxyUnixSocket = true;
}

HandoffSession::~HandoffSession() {
    end();
    if (_s2nConnection) {
        s2n_blocked_status blocked;
        s2n_shutdown(_s2nConnection, &blocked);
        s2n_connection_free(_s2nConnection);
        _s2nConnection = nullptr;
    }
    _posix.close(_fd);
}

void HandoffSession::end() {
    // This function can be called from any thread. It must hold a lock on _mutex when it accesses
    // or modifies any data members.
    std::lock_guard lock{_mutex};
    if (_isShutDown) {
        return;
    }
    _posix.shutdown(_fd, SHUT_RDWR);
    _isShutDown = true;
}

bool HandoffSession::isConnected() {
    // This function can be called from any thread. It must hold a lock on _mutex when it accesses
    // or modifies any data members.
    std::lock_guard lock{_mutex};
    if (_isShutDown) {
        return false;
    }
    struct pollfd pfd;
    pfd.fd = _fd;
    pfd.events = POLLIN | POLLRDHUP;
    pfd.revents = 0;

    int ret;
    do {
        ret = _posix.poll(&pfd, 1, 0);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        return false;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL | POLLRDHUP)) {
        return false;
    }
    return true;
}

void HandoffSession::setTimeout(boost::optional<Milliseconds>) {
    // setTimeout is called from DBClientSession only, and thus shall not be called on an ingress
    // session.
    MONGO_UNREACHABLE;
}

StatusWith<size_t> HandoffSession::_syncRead(char* buf, size_t len) {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.
    size_t totalRead = 0;
    while (totalRead < len) {
        switch (_state) {
            case HandoffSession::State::TLS: {
                // First use any data remaining in _unconsumedBytes.
                const size_t toConsume = std::min(len, _unconsumedBytes.size());
                std::copy_n(_unconsumedBytes.data(), toConsume, buf);
                _unconsumedBytes = _unconsumedBytes.subspan(toConsume);
                if (_unconsumedBytes.empty() && !_unconsumedBuffer.empty()) {
                    _unconsumedBuffer.clear();
                    _unconsumedBuffer.shrink_to_fit();
                }
                totalRead += toConsume;
                if (totalRead == len) {
                    return totalRead;
                }

                s2n_blocked_status blocked = S2N_NOT_BLOCKED;
                ssize_t n = s2n_recv(_s2nConnection, buf + totalRead, len - totalRead, &blocked);

                if (n > 0) {
                    totalRead += n;
                    continue;
                }
                if (n == 0) {
                    return Status(ErrorCodes::SocketException,
                                  "Connection closed by peer, or session has been shut down");
                }
                if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
                    return Status(ErrorCodes::NetworkTimeout, "Timed out waiting for data");
                }
                return Status(ErrorCodes::SocketException,
                              fmt::format("s2n_recv failed: {}", s2n_strerror(s2n_errno, "EN")));
            }
            case HandoffSession::State::Cleartext: {
                ssize_t n;
                do {
                    n = _posix.recv(_fd, buf + totalRead, len - totalRead, 0);
                } while (n < 0 && errno == EINTR);

                if (n > 0) {
                    totalRead += n;
                    continue;
                }
                if (n == 0) {
                    return Status(ErrorCodes::SocketException,
                                  "Connection closed by peer, or session has been shut down");
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return Status(ErrorCodes::NetworkTimeout, "Timed out waiting for data");
                }
                return Status(ErrorCodes::SocketException,
                              fmt::format("recv failed: {}", errorMessage(lastSocketError())));
            }
        }
        MONGO_UNREACHABLE;
    }
    return totalRead;
}

Status HandoffSession::_syncWrite(const char* buf, size_t len) {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.
    size_t totalWritten = 0;
    while (totalWritten < len) {
        switch (_state) {
            case HandoffSession::State::TLS: {
                s2n_blocked_status blocked = S2N_NOT_BLOCKED;
                ssize_t n =
                    s2n_send(_s2nConnection, buf + totalWritten, len - totalWritten, &blocked);
                if (n > 0) {
                    totalWritten += n;
                    continue;
                }
                if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
                    return Status(ErrorCodes::NetworkTimeout, "Timed out waiting to send data");
                }
                return Status(ErrorCodes::SocketException,
                              fmt::format("s2n_send failed: {}", s2n_strerror(s2n_errno, "EN")));
            }
            case HandoffSession::State::Cleartext: {
                ssize_t n;
                do {
                    n = _posix.send(_fd, buf + totalWritten, len - totalWritten, /* flags= */ 0);
                } while (n < 0 && errno == EINTR);
                if (n > 0) {
                    totalWritten += n;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return Status(ErrorCodes::NetworkTimeout, "Timed out waiting to send data");
                }
                return Status(ErrorCodes::SocketException,
                              fmt::format("send failed: {}", errorMessage(lastSocketError())));
            }
        }
        MONGO_UNREACHABLE;
    }
    return Status::OK();
}

StatusWith<size_t> HandoffSession::_recvWithFd(char* buf, size_t len, int* receivedFd) {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.
    *receivedFd = -1;

    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = len;

    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } controlMsg;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = controlMsg.buf;
    msg.msg_controllen = sizeof(controlMsg.buf);

    ssize_t n;
    do {
        n = _posix.recvmsg(_fd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Status(ErrorCodes::NetworkTimeout, "Timed out waiting for data");
        }
        return Status(ErrorCodes::SocketException,
                      fmt::format("recvmsg failed: {}", errorMessage(lastSocketError())));
    }
    if (n == 0) {
        return Status(ErrorCodes::SocketException,
                      "Connection closed by peer, or session has been shut down");
    }

    // Extract file descriptors from SCM_RIGHTS ancillary data. Keep the first one in
    // *receivedFd. Close any extras immediately to prevent leaks.
    // Note: on 64-bit Linux, CMSG_SPACE(sizeof(int)) == CMSG_SPACE(2 * sizeof(int)) == 24
    // bytes due to alignment, so two fds fit in the buffer without triggering MSG_CTRUNC.
    // We therefore count fds explicitly rather than relying solely on MSG_CTRUNC.
    int fdCount = 0;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        int nFds = static_cast<int>((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < nFds; ++i) {
            int fd;
            memcpy(
                &fd, reinterpret_cast<const char*>(CMSG_DATA(cmsg)) + i * sizeof(int), sizeof(int));
            if (fdCount == 0) {
                *receivedFd = fd;
            } else {
                _posix.close(fd);
            }
            ++fdCount;
        }
    }

    // Reject if more than one fd was sent, whether all fit in the control buffer
    // (fdCount > 1) or the buffer was too small to hold them all (MSG_CTRUNC).
    if (fdCount > 1 || (msg.msg_flags & MSG_CTRUNC)) {
        if (*receivedFd >= 0) {
            _posix.close(*receivedFd);
            *receivedFd = -1;
        }
        return Status(ErrorCodes::ProtocolError,
                      "Expected at most one SCM_RIGHTS fd. Found multiple file descriptors");
    }

    return static_cast<size_t>(n);
}

Status HandoffSession::_handleSessionHandoff(const Message& msg, int clientFd) {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.
    // It additionally must hold a lock on _mutex when closing clientFd.

    // before:
    //     _fd = udsFd, _state = Cleartext
    // on error:
    //     _fd = udsFd (shut down), _state = Cleartext
    //     clientFd is shut down and closed
    // on success:
    //     _fd = clientFd (blocking mode), _state = TLS
    //     udsFd is shut down and closed

    ScopeGuard cleanupOnFailure([&] {
        if (clientFd >= 0) {
            _posix.shutdown(clientFd, SHUT_RDWR);
            _posix.close(clientFd);
        }
        if (_s2nConnection) {
            s2n_connection_free(_s2nConnection);
            _s2nConnection = nullptr;
        }
        // Since we failed, _fd is still the udsFd. Shut it down to indicate to the pre-auth process
        // that handoff is complete, albeit unsuccessful.
        _posix.shutdown(_fd, SHUT_RDWR);
    });

    if (clientFd < 0) {
        return Status(ErrorCodes::ProtocolError,
                      "OP_HANDOFF did not include a client file descriptor via SCM_RIGHTS");
    }

    // We use blocking IO on the handed-over socket, but the socket might have been set to
    // nonblocking. Set it to blocking mode.
    if (auto status = setBlocking(_posix, clientFd, "handed-off client socket"); !status.isOK()) {
        return status;
    }

    // The message body contains a BSON document with the serialized s2n state.
    if (auto status = validateBSON(msg.singleData().data(), msg.singleData().dataLen());
        !status.isOK()) {
        return Status(ErrorCodes::ProtocolError,
                      fmt::format("OP_HANDOFF message has invalid BSON: {}", status.reason()));
    }

    SessionHandoffMessage handoffMsg;
    try {
        handoffMsg = SessionHandoffMessage::parse(BSONObj(msg.singleData().data()),
                                                  IDLParserContext("OP_HANDOFF"));
    } catch (const DBException& e) {
        return e.toStatus();
    }

    const ConstDataRange s2nState = handoffMsg.getS2nState();

    // Create, configure, and deserialize the s2n server connection.
    _s2nConnection = s2n_connection_new(S2N_SERVER);
    if (!_s2nConnection) {
        return Status(ErrorCodes::InternalError,
                      fmt::format("s2n_connection_new failed: {}", s2n_strerror(s2n_errno, "EN")));
    }

    // s2n_connection_set_config takes its config parameter by pointer-to-non-const, but in fact it
    // will not modify through it, it's just a C-style convention.
    auto status =
        s2nCheck(s2n_connection_set_config(_s2nConnection, const_cast<s2n_config*>(_s2nConfig)),
                 "s2n_connection_set_config"sv);
    if (!status.isOK()) {
        return status;
    }

    status = s2nCheck(s2n_connection_deserialize(
                          _s2nConnection,
                          const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(s2nState.data())),
                          static_cast<uint32_t>(s2nState.length())),
                      "s2n_connection_deserialize"sv);
    if (!status.isOK()) {
        return status;
    }

    // Self-service blinding: s2n returns errors immediately instead of sleeping.
    status = s2nCheck(s2n_connection_set_blinding(_s2nConnection, S2N_SELF_SERVICE_BLINDING),
                      "s2n_connection_set_blinding"sv);
    if (!status.isOK()) {
        return status;
    }

    status = s2nCheck(s2n_connection_set_fd(_s2nConnection, clientFd), "s2n_connection_set_fd"sv);
    if (!status.isOK()) {
        return status;
    }

    status = _updateEndpointsForClientFd(clientFd);
    if (!status.isOK()) {
        return status;
    }

    bool isShutDown;
    {
        std::lock_guard lock{_mutex};
        const int udsFd = _fd;
        // Do the shutdown here, instead of above, to aid synchronization with unit tests.
        _posix.shutdown(udsFd, SHUT_RDWR);
        _fd = clientFd;
        _posix.close(udsFd);
        isShutDown = _isShutDown;
        _state = State::TLS;
    }

    // If another thread called `end()` before we entered the critical region above, we need to
    // honor their request by shutting down the recently-changed _fd (now `clientFd`).
    if (isShutDown) {
        _posix.shutdown(_fd, SHUT_RDWR);
    }

    if (boost::optional<ConstDataRange> extra = handoffMsg.getExtraCleartext()) {
        _unconsumedBuffer.assign(extra->data(), extra->data() + extra->length());
        _unconsumedBytes = std::span<char>{_unconsumedBuffer.begin(), _unconsumedBuffer.end()};
    }

    cleanupOnFailure.dismiss();

    LOGV2(12823501,
          "S2N session handoff complete",
          "sessionId"_attr = id(),
          "clientFd"_attr = clientFd,
          "remote"_attr = _endpointsAfterHandoff.remote);

    return Status::OK();
}

Status HandoffSession::_updateEndpointsForClientFd(int clientFd) {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.
    ConnectionEndpoints newEndpoints;

    sockaddr_storage peerStorage;
    socklen_t peerLen = sizeof(peerStorage);
    const auto peerAddr = reinterpret_cast<sockaddr*>(&peerStorage);
    if (_posix.getpeername(clientFd, peerAddr, &peerLen) != 0) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("getpeername failed after TLS handoff: {}",
                                  errorMessage(lastSocketError())));
    }
    const SockAddr peerSa(peerAddr, peerLen);
    newEndpoints.remote = toHostAndPort(peerSa);

    sockaddr_storage localStorage;
    socklen_t localLen = sizeof(localStorage);
    const auto localAddr = reinterpret_cast<sockaddr*>(&localStorage);
    if (_posix.getsockname(clientFd, localAddr, &localLen) != 0) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("getsockname failed after TLS handoff: {}",
                                  errorMessage(lastSocketError())));
    }
    const SockAddr localSa(localAddr, localLen);
    newEndpoints.local = toHostAndPort(localSa);

    {
        std::lock_guard lock{_mutex};
        _endpointsAfterHandoff = std::move(newEndpoints);
    }

    // Don't need to lock the mutex here, because _restrictionEnvironment is never accessed from
    // another thread.
    if (_proxiedSource && clientSourceAuthenticationRestrictionMode == "origin"sv) {
        _restrictionEnvironment = RestrictionEnvironment(_proxiedSource->address, localSa);
    } else {
        _restrictionEnvironment = RestrictionEnvironment(peerSa, localSa);
    }

    return Status::OK();
}

const HostAndPort& HandoffSession::_remote(WithLock) const {
    switch (_state) {
        case State::Cleartext:
            return _endpointsBeforeHandoff.remote;
        case State::TLS:
            return _endpointsAfterHandoff.remote;
    }
    MONGO_UNREACHABLE;
}

const HostAndPort& HandoffSession::remote() const {
    // This function can be called from any thread. It must hold a lock on _mutex when it accesses
    // any data members.
    std::lock_guard lock{_mutex};
    return _remote(lock);
}

const HostAndPort& HandoffSession::_local(WithLock) const {
    switch (_state) {
        case State::Cleartext:
            return _endpointsBeforeHandoff.local;
        case State::TLS:
            return _endpointsAfterHandoff.local;
    }
    MONGO_UNREACHABLE;
}

const HostAndPort& HandoffSession::local() const {
    // This function can be called from any thread. It must hold a lock on _mutex when it accesses
    // any data members.
    std::lock_guard lock{_mutex};
    return _local(lock);
}

const HostAndPort& HandoffSession::getSourceRemoteEndpoint() const {
    // This function can be called from any thread. It must hold a lock on _mutex when it accesses
    // any data members.
    std::lock_guard lock{_mutex};
    if (_proxiedSource) {
        return _proxiedSource->endpoint;
    }
    return _remote(lock);
}

transport::ParserResults HandoffSession::_parseProxyProtocolHeader() {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.

    // Apply the proxy header read timeout for the duration of this call only.
    const int64_t proxyTimeoutSecs = gProxyProtocolTimeoutSecs.load();

    {
        struct timeval tv;
        tv.tv_sec = proxyTimeoutSecs;
        // A timeout of 0 means expire immediately if the header is not already available.
        // SO_RCVTIMEO of {0,0} disables the recv deadline on Linux, so use the smallest
        // non-zero timeval (1 microsecond) instead to ensure blocking reads time out immediately
        // rather than hanging indefinitely.
        tv.tv_usec = (proxyTimeoutSecs == 0) ? 1 : 0;
        uassert(ErrorCodes::SocketException,
                fmt::format("Failed to set SO_RCVTIMEO on proxy UDS socket: {}",
                            errorMessage(lastSocketError())),
                _posix.setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    }

    ON_BLOCK_EXIT([this] {
        struct timeval tv = {0, 0};
        _posix.setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    });

    int timeoutMs = static_cast<int>(
        std::min<int64_t>(proxyTimeoutSecs * 1000LL, std::numeric_limits<int>::max()));
    uassertStatusOK(pollForRead(_posix, _fd, timeoutMs));

    // Peek at the first bytes to distinguish PROXY v1 (unsupported) from v2, and reject
    // anything else. If v2, consume the full header below.
    char prefix[kProxyPeekSize];
    ssize_t peeked;
    do {
        peeked = _posix.recv(_fd, prefix, kProxyPeekSize, MSG_PEEK | MSG_WAITALL);
    } while (peeked < 0 && errno == EINTR);
    if (peeked < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            uasserted(ErrorCodes::NetworkTimeout, "Timed out waiting for PROXY protocol header");
        }
        uasserted(ErrorCodes::SocketException,
                  fmt::format("recv peek failed: {}", errorMessage(lastSocketError())));
    }
    if (peeked == 0) {
        uasserted(ErrorCodes::SocketException,
                  "Proxy UDS peer closed connection before sending PROXY header");
    }
    if (peeked < static_cast<ssize_t>(kProxyPeekSize)) {
        // Fewer bytes than requested arrived. With MSG_WAITALL, that can only happen when
        // SO_RCVTIMEO fires after partial data has arrived — MSG_WAITALL keeps waiting for the
        // full count past any partial arrival, so it exhausts the timeout budget and returns
        // whatever was buffered rather than -1/EAGAIN. There is nothing left to wait for, so
        // treat this as a timeout.
        uasserted(
            ErrorCodes::NetworkTimeout,
            fmt::format("Timed out waiting for PROXY protocol header ({} of {} bytes received)",
                        peeked,
                        kProxyPeekSize));
    }

    uassert(ErrorCodes::ProtocolError,
            "Proxy UDS connection sent a PROXY v1 header. Only PROXY v2 is supported",
            memcmp(prefix,
                   transport::kProxyV1Signature.data(),
                   transport::kProxyV1Signature.size()) != 0);
    uassert(ErrorCodes::ProtocolError,
            "Proxy UDS connection did not begin with a PROXY v2 header",
            memcmp(prefix, transport::kProxyV2Signature.data(), kProxyPeekSize - 1) == 0);

    // Read the header in two passes: first the fixed-size header to learn the variable length,
    // then the address + TLV block. This avoids over-reading up to proxyUnixSocketMaximumHeaderSize
    // in a single shot.

    // Consume the fixed header (signature + version/command + address family + length).
    std::vector<char> proxyBuf(kProxyV2FixedHeaderSize);
    uassertStatusOK(_syncRead(proxyBuf.data(), kProxyV2FixedHeaderSize));

    // The last 2 bytes of the fixed header are the big-endian length of the address + TLV block.
    uint16_t addrLen =
        ConstDataRange(proxyBuf.data() + kProxyV2FixedHeaderSize - 2, sizeof(uint16_t))
            .read<BigEndian<uint16_t>>();
    size_t maxHeaderSize = static_cast<size_t>(proxyUnixSocketMaximumHeaderSize.loadRelaxed());
    uassert(ErrorCodes::ProtocolError,
            fmt::format("PROXY v2 header on proxy UDS exceeds "
                        "proxyUnixSocketMaximumHeaderSize ({} > {})",
                        kProxyV2FixedHeaderSize + addrLen,
                        maxHeaderSize),
            kProxyV2FixedHeaderSize + static_cast<size_t>(addrLen) <= maxHeaderSize);

    if (addrLen > 0) {
        proxyBuf.resize(kProxyV2FixedHeaderSize + addrLen);
        uassertStatusOK(_syncRead(proxyBuf.data() + kProxyV2FixedHeaderSize, addrLen));
    }

    std::string_view proxyData(proxyBuf.data(), proxyBuf.size());
    auto results = transport::parseProxyProtocolHeader(proxyData, /*isProxyUnixSock=*/true);
    uassert(ErrorCodes::ProtocolError,
            "Failed to parse PROXY v2 header on proxy UDS",
            results.has_value());
    return std::move(*results);
}

void HandoffSession::prelude() {
    auto proxyHeader = _parseProxyProtocolHeader();
    if (proxyHeader.endpoints) {
        const auto& sourceAddress = proxyHeader.endpoints->sourceAddress;
        std::lock_guard lock{_mutex};
        _proxiedSource = ProxiedSource{
            .address = sourceAddress,
            .endpoint = toHostAndPort(sourceAddress),
        };
    }
    applyProxyProtocolTlvs(proxyHeader, shared_from_this());
}

StatusWith<Message> HandoffSession::sourceMessage() {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.

    // Read the message header. In Cleartext mode, use recvmsg to capture any
    // SCM_RIGHTS fd that accompanies an OP_HANDOFF.
    char headerBuf[kHeaderSize];
    int receivedFd = -1;
    ScopeGuard fdGuard([&] {
        if (receivedFd >= 0) {
            _posix.close(receivedFd);
        }
    });

    switch (_state) {
        case State::Cleartext: {
            auto swRead = _recvWithFd(headerBuf, kHeaderSize, &receivedFd);
            if (!swRead.isOK()) {
                return swRead.getStatus();
            }
            // _recvWithFd (recvmsg) may return less than requested; read the remainder.
            if (size_t bytesRead = swRead.getValue(); bytesRead < kHeaderSize) {
                auto swRemain = _syncRead(headerBuf + bytesRead, kHeaderSize - bytesRead);
                if (!swRemain.isOK()) {
                    return swRemain.getStatus();
                }
            }
            break;
        }
        case State::TLS: {
            auto swRead = _syncRead(headerBuf, kHeaderSize);
            if (!swRead.isOK()) {
                return swRead.getStatus();
            }
            break;
        }
            MONGO_UNREACHABLE;
    }

    // Validate message length and read the body.
    MSGHEADER::ConstView headerView(headerBuf);
    int32_t msgLen = headerView.getMessageLength();
    if (msgLen < static_cast<int32_t>(kHeaderSize) ||
        msgLen > static_cast<int32_t>(MaxMessageSizeBytes)) {
        return Status(ErrorCodes::ProtocolError, fmt::format("Invalid message length: {}", msgLen));
    }

    auto buffer = SharedBuffer::allocate(msgLen);
    memcpy(buffer.get(), headerBuf, kHeaderSize);
    if (msgLen > static_cast<int32_t>(kHeaderSize)) {
        auto swBody = _syncRead(buffer.get() + kHeaderSize, msgLen - kHeaderSize);
        if (!swBody.isOK()) {
            return swBody.getStatus();
        }
    }

    Message msg(std::move(buffer));

    // Handle OP_HANDOFF. Complete the handoff inline, then recurse to read the first TLS message.
    if (_state != State::TLS && msg.operation() == dbSessionHandoff) {
        int fd = std::exchange(receivedFd, -1);  // Disarm the guard; handoff takes ownership.
        auto status = _handleSessionHandoff(msg, fd);
        if (!status.isOK()) {
            return status;
        }
        return sourceMessage();
    }

    if (receivedFd >= 0) {
        LOGV2_WARNING(12823502,
                      "Received SCM_RIGHTS fd with non-handoff message, closing",
                      "fd"_attr = receivedFd,
                      "opcode"_attr = static_cast<int>(msg.operation()));
    }

    return msg;
}

Status HandoffSession::sinkMessage(Message message) {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.
    return _syncWrite(message.buf(), message.size());
}

Future<Message> HandoffSession::asyncSourceMessage(const BatonHandle&) {
    MONGO_UNREACHABLE;
}

Future<void> HandoffSession::asyncSinkMessage(Message, const BatonHandle&) {
    MONGO_UNREACHABLE;
}

Status HandoffSession::waitForData() {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.
    const int noTimeout = -1;
    return pollForRead(_posix, _fd, noTimeout);
}

Future<void> HandoffSession::asyncWaitForData() {
    MONGO_UNREACHABLE;
}

void HandoffSession::setIsLoadBalancerPeer(bool value) {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.
    _isLoadBalancerPeer = value;
}

Status HandoffSession::validateProxyUnixSocketPeerPermissions() {
    // This function is an implementation detail of AsioTransportLayer, and as such cannot be called
    // on objects of this derived type.
    MONGO_UNREACHABLE;
}

bool HandoffSession::bindsToOperationState() const {
    // This function is called from the service executor thread only. It may access any data members
    // except _isShutDown without holding a lock on _mutex. To modify data members, or to access
    // _isShutDown, it must hold a lock on _mutex.
    return _isLoadBalancerPeer;
}

bool HandoffSession::isExemptedByCIDRList(const CIDRList&) const {
    // Exemption from rate limiters and eligibility for the reserved service executor, i.e. being a
    // privileged session, is determined by whether the secure frontend process connection was
    // accepted on HandoffTransportLayer's "priority" unix domain socket.
    // The configured CIDRList of exemptions by remote address is not used for handoff sessions.
    return false;
}

void HandoffSession::cancelAsyncOperations(const BatonHandle&) {
    // No async operations to cancel.
}

void HandoffSession::appendToBSON(BSONObjBuilder& bb) const {
    // This function can be called from any thread. It must hold a lock on _mutex when it accesses
    // any data members.
    bb.append("id", static_cast<long long>(id()));
    std::lock_guard lock{_mutex};
    bb.append("remote", _remote(lock).toString());
    bb.append("local", _local(lock).toString());
    bb.append("state", toString(_state));
}

}  // namespace mongo::transport
