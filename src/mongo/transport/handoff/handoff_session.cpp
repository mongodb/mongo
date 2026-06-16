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
#include <limits>

#include <poll.h>
#include <unistd.h>

#include <sys/socket.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::handoff_transport {

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
Status s2nCheck(int result, StringData op) {
    if (result == S2N_SUCCESS) {
        return Status::OK();
    }
    return Status(ErrorCodes::InternalError,
                  fmt::format("{} failed: {}", op, s2n_strerror(s2n_errno, "EN")));
}

}  // namespace

HandoffSession::HandoffSession(
    TransportLayer* tl, int fd, HostAndPort remote, HostAndPort local, struct s2n_config* s2nConfig)
    : Session(/* isIngress= */ true),
      _tl(tl),
      _fd(fd),
      _state(HandoffSessionState::Cleartext),
      _remote(std::move(remote)),
      _local(std::move(local)),
      _s2nConnection(nullptr),
      _s2nConfig(s2nConfig) {
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
}

void HandoffSession::end() {
    // swap(-1) atomically takes ownership of the fd, making end() idempotent: a second call
    // gets -1 back and skips the shutdown/close entirely, preventing a double-close.
    int fd = _fd.swap(-1);
    if (fd >= 0) {
        // shutdown() before close() unblocks any thread blocked in poll/recv/send/recvmsg on this
        // fd.
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

bool HandoffSession::isConnected() {
    int fd = _fd.load();
    if (fd < 0) {
        return false;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLRDHUP;
    pfd.revents = 0;

    int ret;
    do {
        ret = ::poll(&pfd, 1, 0);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        return false;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL | POLLRDHUP)) {
        return false;
    }
    return true;
}

void HandoffSession::setTimeout(boost::optional<Milliseconds> timeout) {
    _timeout = timeout;
    int fd = _fd.load();
    if (fd >= 0) {
        uassertStatusOK(_applyTimeout(fd));
    }
}

Status HandoffSession::_pollForRead(int fd, int timeoutMs) {
    // Uses poll() rather than recv() so callers are safe to invoke concurrently with end().
    // TSAN recognizes poll(fd) + close(fd) as a safe concurrent pattern but not recv(fd) +
    // close(fd).
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLRDHUP;
    pfd.revents = 0;

    int ret;
    do {
        ret = ::poll(&pfd, 1, timeoutMs);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("poll failed: {}", errorMessage(lastSocketError())));
    }
    if (ret == 0) {
        return Status(ErrorCodes::NetworkTimeout, "Timed out waiting for data");
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL | POLLRDHUP)) {
        return Status(ErrorCodes::SocketException, "Socket error or peer disconnect");
    }
    return Status::OK();
}

Status HandoffSession::_applyTimeout(int fd) {
    // Zero disables the timeout. Non-zero sets a deadline for blocking recv/send calls.
    struct timeval tv = {0, 0};
    if (_timeout) {
        auto ms = durationCount<Milliseconds>(*_timeout);
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
    }
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return Status(
            ErrorCodes::InternalError,
            fmt::format("setsockopt SO_RCVTIMEO failed: {}", errorMessage(lastSocketError())));
    }
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return Status(
            ErrorCodes::InternalError,
            fmt::format("setsockopt SO_SNDTIMEO failed: {}", errorMessage(lastSocketError())));
    }
    return Status::OK();
}

StatusWith<size_t> HandoffSession::_syncRead(char* buf, size_t len) {
    size_t totalRead = 0;
    while (totalRead < len) {
        switch (_state.load()) {
            case HandoffSessionState::TLS: {
                s2n_blocked_status blocked = S2N_NOT_BLOCKED;
                ssize_t n = s2n_recv(_s2nConnection, buf + totalRead, len - totalRead, &blocked);

                if (n > 0) {
                    totalRead += n;
                    continue;
                }
                if (n == 0) {
                    return Status(ErrorCodes::SocketException, "Connection closed by peer");
                }
                if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED) {
                    return Status(ErrorCodes::NetworkTimeout, "Timed out waiting for data");
                }
                return Status(ErrorCodes::SocketException,
                              fmt::format("s2n_recv failed: {}", s2n_strerror(s2n_errno, "EN")));
            }
            case HandoffSessionState::Cleartext: {
                ssize_t n;
                do {
                    n = ::recv(_fd.load(), buf + totalRead, len - totalRead, 0);
                } while (n < 0 && errno == EINTR);

                if (n > 0) {
                    totalRead += n;
                    continue;
                }
                if (n == 0) {
                    return Status(ErrorCodes::SocketException, "Connection closed by peer");
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
    size_t totalWritten = 0;
    while (totalWritten < len) {
        switch (_state.load()) {
            case HandoffSessionState::TLS: {
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
            case HandoffSessionState::Cleartext: {
                ssize_t n;
                do {
                    n = ::send(_fd.load(), buf + totalWritten, len - totalWritten, /* flags= */ 0);
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
        n = ::recvmsg(_fd.load(), &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Status(ErrorCodes::NetworkTimeout, "Timed out waiting for data");
        }
        return Status(ErrorCodes::SocketException,
                      fmt::format("recvmsg failed: {}", errorMessage(lastSocketError())));
    }
    if (n == 0) {
        return Status(ErrorCodes::SocketException, "Connection closed by peer");
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
                ::close(fd);
            }
            ++fdCount;
        }
    }

    // Reject if more than one fd was sent, whether all fit in the control buffer
    // (fdCount > 1) or the buffer was too small to hold them all (MSG_CTRUNC).
    if (fdCount > 1 || (msg.msg_flags & MSG_CTRUNC)) {
        if (*receivedFd >= 0) {
            ::close(*receivedFd);
            *receivedFd = -1;
        }
        return Status(ErrorCodes::ProtocolError,
                      "Expected at most one SCM_RIGHTS fd. Found multiple file descriptors");
    }

    return static_cast<size_t>(n);
}

Status HandoffSession::_handleSessionHandoff(const Message& msg, int clientFd) {
    // On any failure, close both the received client TLS fd and the UDS fd.
    ScopeGuard cleanupOnFailure([&] {
        if (clientFd >= 0) {
            ::close(clientFd);
        }
        int fd = _fd.swap(-1);
        if (fd >= 0) {
            ::close(fd);
        }
        if (_s2nConnection) {
            s2n_connection_free(_s2nConnection);
            _s2nConnection = nullptr;
        }
    });

    if (clientFd < 0) {
        return Status(ErrorCodes::ProtocolError,
                      "OP_HANDOFF did not include a client file descriptor via SCM_RIGHTS");
    }

    // The message body contains a BSON document with the serialized s2n state.
    int bodyDataLen = msg.singleData().dataLen();
    if (bodyDataLen < 5) {
        return Status(ErrorCodes::ProtocolError, "OP_HANDOFF message body is empty");
    }

    if (auto status = validateBSON(msg.singleData().data(), bodyDataLen); !status.isOK()) {
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

    ConstDataRange s2nState = handoffMsg.getS2nState();
    int s2nStateLen = static_cast<int>(s2nState.length());
    const char* s2nStateData = s2nState.data();

    // Create, configure, and deserialize the s2n server connection.
    _s2nConnection = s2n_connection_new(S2N_SERVER);
    if (!_s2nConnection) {
        return Status(ErrorCodes::InternalError,
                      fmt::format("s2n_connection_new failed: {}", s2n_strerror(s2n_errno, "EN")));
    }

    auto status = s2nCheck(s2n_connection_set_config(_s2nConnection, _s2nConfig),
                           "s2n_connection_set_config"_sd);
    if (!status.isOK()) {
        return status;
    }

    status = s2nCheck(s2n_connection_deserialize(
                          _s2nConnection,
                          const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(s2nStateData)),
                          static_cast<uint32_t>(s2nStateLen)),
                      "s2n_connection_deserialize"_sd);
    if (!status.isOK()) {
        return status;
    }

    // Self-service blinding: s2n returns errors immediately instead of sleeping.
    status = s2nCheck(s2n_connection_set_blinding(_s2nConnection, S2N_SELF_SERVICE_BLINDING),
                      "s2n_connection_set_blinding"_sd);
    if (!status.isOK()) {
        return status;
    }

    // Do all setup on clientFd before updating _fd so any failure still leaves us able to
    // close both fds cleanly via the ScopeGuard.

    // Apply any active timeout to the client fd so blocking s2n_recv/s2n_send respect it.
    status = _applyTimeout(clientFd);
    if (!status.isOK()) {
        return status;
    }

    status = s2nCheck(s2n_connection_set_fd(_s2nConnection, clientFd), "s2n_connection_set_fd"_sd);
    if (!status.isOK()) {
        return status;
    }

    // Complete all state updates before closing the UDS fd. Per the threading contract,
    // closing the UDS fd is the memory barrier that publishes them.
    status = _updateEndpointsForClientFd(clientFd);
    if (!status.isOK()) {
        return status;
    }
    _state.store(HandoffSessionState::TLS);

    // Swap _fd to clientFd before closing the UDS fd so that any thread observing the
    // resulting EOF already sees _fd == clientFd.
    int udsFd = _fd.swap(clientFd);
    clientFd = -1;  // Prevent double-close by the scope guard.
    ::close(udsFd);

    cleanupOnFailure.dismiss();

    LOGV2(12823501,
          "S2N session handoff complete",
          "sessionId"_attr = id(),
          "clientFd"_attr = _fd.load(),
          "remote"_attr = _remote);

    return Status::OK();
}

Status HandoffSession::_updateEndpointsForClientFd(int clientFd) {
    struct sockaddr_storage peerAddr;
    socklen_t peerLen = sizeof(peerAddr);
    if (::getpeername(clientFd, reinterpret_cast<struct sockaddr*>(&peerAddr), &peerLen) != 0) {
        return Status(ErrorCodes::SocketException,
                      fmt::format("getpeername failed after TLS handoff: {}",
                                  errorMessage(lastSocketError())));
    }
    SockAddr sa(reinterpret_cast<struct sockaddr*>(&peerAddr), peerLen);
    _remote = HostAndPort(sa.getAddr(), sa.getPort());

    struct sockaddr_storage localAddr;
    socklen_t localLen = sizeof(localAddr);
    SockAddr localSa;
    if (::getsockname(clientFd, reinterpret_cast<struct sockaddr*>(&localAddr), &localLen) == 0) {
        localSa = SockAddr(reinterpret_cast<struct sockaddr*>(&localAddr), localLen);
        _local = HostAndPort(localSa.getAddr(), localSa.getPort());
    }

    if (_proxiedSrcAddr && clientSourceAuthenticationRestrictionMode == "origin"_sd) {
        _restrictionEnvironment = RestrictionEnvironment(_proxiedSrcAddr.value(), localSa);
    } else {
        _restrictionEnvironment = RestrictionEnvironment(sa, localSa);
    }

    return Status::OK();
}

const HostAndPort& HandoffSession::getSourceRemoteEndpoint() const {
    if (_proxiedSrcEndpoint) {
        return _proxiedSrcEndpoint.value();
    }
    return _remote;
}

transport::ParserResults HandoffSession::_parseProxyProtocolHeader() {
    int fd = _fd.load();

    // Apply the proxy header read timeout for the duration of this call, then restore the
    // session timeout on return.
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
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    }
    ON_BLOCK_EXIT([this, fd] {
        if (auto status = _applyTimeout(fd); !status.isOK()) {
            LOGV2_WARNING(12823503,
                          "Failed to restore session timeout after proxy header read",
                          "error"_attr = status);
        }
    });

    int timeoutMs = static_cast<int>(
        std::min<int64_t>(proxyTimeoutSecs * 1000LL, std::numeric_limits<int>::max()));
    uassertStatusOK(_pollForRead(fd, timeoutMs));

    // Peek at the first bytes to distinguish PROXY v1 (unsupported) from v2, and reject
    // anything else. If v2, consume the full header below.
    char prefix[kProxyPeekSize];
    ssize_t peeked;
    do {
        peeked = ::recv(fd, prefix, kProxyPeekSize, MSG_PEEK | MSG_WAITALL);
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

    StringData proxyData(proxyBuf.data(), proxyBuf.size());
    auto results = transport::parseProxyProtocolHeader(proxyData, /*isProxyUnixSock=*/true);
    uassert(ErrorCodes::ProtocolError,
            "Failed to parse PROXY v2 header on proxy UDS",
            results.has_value());
    return std::move(*results);
}

void HandoffSession::prelude() {
    auto proxyHeader = _parseProxyProtocolHeader();
    if (proxyHeader.endpoints) {
        _proxiedSrcAddr = proxyHeader.endpoints->sourceAddress;
        _proxiedSrcEndpoint = HostAndPort(_proxiedSrcAddr->getAddr(), _proxiedSrcAddr->getPort());
    }
    applyProxyProtocolTlvs(proxyHeader, shared_from_this());
}

StatusWith<Message> HandoffSession::sourceMessage() {
    if (_fd.load() < 0) {
        return Status(ErrorCodes::SocketException, "Session is not connected");
    }

    // Read the message header. In Cleartext mode, use recvmsg to capture any
    // SCM_RIGHTS fd that accompanies an OP_HANDOFF.
    char headerBuf[kHeaderSize];
    int receivedFd = -1;
    ScopeGuard fdGuard([&] {
        if (receivedFd >= 0) {
            ::close(receivedFd);
        }
    });

    switch (_state.load()) {
        case HandoffSessionState::Cleartext: {
            auto swRead = _recvWithFd(headerBuf, kHeaderSize, &receivedFd);
            if (!swRead.isOK()) {
                return swRead.getStatus();
            }
            // recvmsg may return less than requested; read the remainder.
            if (size_t bytesRead = swRead.getValue(); bytesRead < kHeaderSize) {
                auto swRemain = _syncRead(headerBuf + bytesRead, kHeaderSize - bytesRead);
                if (!swRemain.isOK()) {
                    return swRemain.getStatus();
                }
            }
            break;
        }
        case HandoffSessionState::TLS: {
            auto swRead = _syncRead(headerBuf, kHeaderSize);
            if (!swRead.isOK()) {
                return swRead.getStatus();
            }
            break;
        }
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
    if (_state.load() == HandoffSessionState::Cleartext && msg.operation() == dbSessionHandoff) {
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
    if (_fd.load() < 0) {
        return Status(ErrorCodes::SocketException, "Session is not connected");
    }

    return _syncWrite(message.buf(), message.size());
}

Future<Message> HandoffSession::asyncSourceMessage(const BatonHandle&) {
    MONGO_UNREACHABLE;
}

Future<void> HandoffSession::asyncSinkMessage(Message, const BatonHandle&) {
    MONGO_UNREACHABLE;
}

Status HandoffSession::waitForData() {
    int fd = _fd.load();
    if (fd < 0) {
        return Status(ErrorCodes::SocketException, "Session is not connected");
    }
    int timeoutMs = _timeout ? durationCount<Milliseconds>(*_timeout) : -1;
    return _pollForRead(fd, timeoutMs);
}

Future<void> HandoffSession::asyncWaitForData() {
    MONGO_UNREACHABLE;
}

void HandoffSession::setIsLoadBalancerPeer(bool) {
    MONGO_UNREACHABLE;
}

void HandoffSession::cancelAsyncOperations(const BatonHandle&) {
    // No async operations to cancel.
}

void HandoffSession::appendToBSON(BSONObjBuilder& bb) const {
    bb.append("id", static_cast<long long>(id()));
    bb.append("remote", _remote.toString());
    bb.append("local", _local.toString());
    bb.append("state", _state.load() == HandoffSessionState::Cleartext ? "cleartext" : "tls");
}

}  // namespace mongo::transport::handoff_transport
