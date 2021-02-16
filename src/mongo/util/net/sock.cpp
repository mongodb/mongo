/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/sock.h"

#include <algorithm>
#include <fmt/format.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#if defined(__OpenBSD__)
#include <sys/uio.h>
#endif
#else
#include <mstcpip.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/value.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hex.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/str.h"
#include "mongo/util/winutil.h"

namespace mongo {

using std::pair;
using std::string;
using std::stringstream;
using std::vector;

MONGO_FAIL_POINT_DEFINE(throwSockExcep);

namespace {

// Provides a cross-platform function for setting a file descriptor/socket to non-blocking mode.
bool setBlock(int fd, bool block) {
#ifdef _WIN32
    u_long ioMode = block ? 0 : 1;
    return (NO_ERROR == ::ioctlsocket(fd, FIONBIO, &ioMode));
#else
    int flags = fcntl(fd, F_GETFL, fd);
    if (block) {
        return (-1 != fcntl(fd, F_SETFL, (flags & ~O_NONBLOCK)));
    } else {
        return (-1 != fcntl(fd, F_SETFL, (flags | O_NONBLOCK)));
    }
#endif
}

void networkWarnWithDescription(const Socket& socket, StringData call, int errorCode = -1) {
#ifdef _WIN32
    if (errorCode == -1) {
        errorCode = WSAGetLastError();
    }
#endif
    auto ewd = errnoWithDescription(errorCode);
    LOGV2_WARNING(23190,
                  "failed to connect to {remoteSocketAddress}:{remoteSocketAddressPort}, "
                  "in({call}), reason: {error}",
                  "Failed to connect to remote host",
                  "remoteSocketAddress"_attr = socket.remoteAddr().getAddr(),
                  "remoteSocketAddressPort"_attr = socket.remoteAddr().getPort(),
                  "call"_attr = call,
                  "error"_attr = ewd);
}

const double kMaxConnectTimeoutMS = 5000;

void setSockTimeouts(int sock, double secs) {
    bool report =
        shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(4)) || kDebugBuild;
#if defined(_WIN32)
    DWORD timeout = secs * 1000;  // Windows timeout is a DWORD, in milliseconds.
    int status =
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(DWORD));
    if (report && (status == SOCKET_ERROR))
        LOGV2(23177,
              "unable to set SO_RCVTIMEO: {reason}",
              "Unable to set SO_RCVTIMEO",
              "reason"_attr = errnoWithDescription(WSAGetLastError()));
    status =
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(DWORD));
    if (kDebugBuild && report && (status == SOCKET_ERROR))
        LOGV2(23178,
              "unable to set SO_SNDTIMEO: {reason}",
              "Unable to set SO_SNDTIME0",
              "reason"_attr = errnoWithDescription(WSAGetLastError()));
#else
    struct timeval tv;
    tv.tv_sec = (int)secs;
    tv.tv_usec = (int)((long long)(secs * 1000 * 1000) % (1000 * 1000));
    bool ok = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv)) == 0;
    if (report && !ok)
        LOGV2(23179, "unable to set SO_RCVTIMEO");
    ok = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv)) == 0;
    if (kDebugBuild && report && !ok)
        LOGV2(23180, "unable to set SO_SNDTIMEO");
#endif
}

void disableNagle(int sock) {
    int x = 1;
#ifdef _WIN32
    const int level = IPPROTO_TCP;
#elif defined(SOL_TCP)
    const int level = SOL_TCP;
#else
    const int level = SOL_SOCKET;
#endif

    if (setsockopt(sock, level, TCP_NODELAY, (char*)&x, sizeof(x)))
        LOGV2_ERROR(23195,
                    "disableNagle failed: {error}",
                    "DisableNagle failed",
                    "error"_attr = errnoWithDescription());

#ifdef SO_KEEPALIVE
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*)&x, sizeof(x)))
        LOGV2_ERROR(23196,
                    "SO_KEEPALIVE failed: {error}",
                    "SO_KEEPALIVE failed",
                    "error"_attr = errnoWithDescription());
#endif

    setSocketKeepAliveParams(sock);
}

int socketGetLastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

SockAddr getLocalAddrForBoundSocketFd(int fd) {
    SockAddr result;
    int rc = getsockname(fd, result.raw(), &result.addressSize);
    if (rc != 0) {
        LOGV2_WARNING(23191,
                      "Could not resolve local address for socket with fd {fd}: "
                      "{error}",
                      "Could not resolve local address for socket with fd",
                      "fd"_attr = fd,
                      "error"_attr = getAddrInfoStrError(socketGetLastError()));
        result = SockAddr();
    }
    return result;
}

#ifdef _WIN32

int socketPoll(pollfd* fdarray, unsigned long nfds, int timeout) {
    return WSAPoll(fdarray, nfds, timeout);
}

#else

int socketPoll(pollfd* fdarray, unsigned long nfds, int timeout) {
    return ::poll(fdarray, nfds, timeout);
}
#endif

}  // namespace

#ifdef MSG_NOSIGNAL
const int portSendFlags = MSG_NOSIGNAL;
const int portRecvFlags = MSG_NOSIGNAL;
#else
const int portSendFlags = 0;
const int portRecvFlags = 0;
#endif

// ------------ Socket -----------------

Socket::Socket(int fd, const SockAddr& remote)
    : _fd(fd),
      _remote(remote),
      _timeout(0),
      _lastValidityCheckAtSecs(time(nullptr)),
      _logLevel(logv2::LogSeverity::Log()) {
    _init();
    if (fd >= 0) {
        _local = getLocalAddrForBoundSocketFd(_fd);
    }
}

Socket::Socket(double timeout, logv2::LogSeverity ll) : _logLevel(ll) {
    _fd = INVALID_SOCKET;
    _timeout = timeout;
    _lastValidityCheckAtSecs = time(nullptr);
    _init();
}

Socket::~Socket() {
    close();
}

void Socket::_init() {
    _bytesOut = 0;
    _bytesIn = 0;
    _awaitingHandshake = true;
#ifdef MONGO_CONFIG_SSL
    _sslManager = nullptr;
#endif
}

void Socket::close() {
    if (_fd != INVALID_SOCKET) {
// Stop any blocking reads/writes, and prevent new reads/writes
#if defined(_WIN32)
        shutdown(_fd, SD_BOTH);
#else
        shutdown(_fd, SHUT_RDWR);
#endif
        closesocket(_fd);
        _fd = INVALID_SOCKET;
    }
}

#ifdef MONGO_CONFIG_SSL
bool Socket::secure(SSLManagerInterface* mgr, const std::string& remoteHost) {
    fassert(16503, mgr);
    if (_fd == INVALID_SOCKET) {
        return false;
    }
    _sslManager = mgr;
    _sslConnection.reset(_sslManager->connect(this));
    mgr->parseAndValidatePeerCertificateDeprecated(_sslConnection.get(), remoteHost, HostAndPort());
    return true;
}

void Socket::secureAccepted(SSLManagerInterface* ssl) {
    _sslManager = ssl;
}

SSLPeerInfo Socket::doSSLHandshake(const char* firstBytes, int len) {
    if (!_sslManager)
        return SSLPeerInfo();
    fassert(16506, _fd != INVALID_SOCKET);
    if (_sslConnection.get()) {
        throwSocketError(SocketErrorKind::RECV_ERROR,
                         "Attempt to call SSL_accept on already secure Socket from " +
                             remoteString());
    }
    _sslConnection.reset(_sslManager->accept(this, firstBytes, len));
    return _sslManager->parseAndValidatePeerCertificateDeprecated(
        _sslConnection.get(), "", HostAndPort());
}

#endif

bool Socket::connect(SockAddr& remote) {
    const Milliseconds connectTimeoutMillis(static_cast<int64_t>(
        _timeout > 0 ? std::min(kMaxConnectTimeoutMS, (_timeout * 1000)) : kMaxConnectTimeoutMS));
    return connect(remote, connectTimeoutMillis);
}

bool Socket::connect(SockAddr& remote, Milliseconds connectTimeoutMillis) {
    _remote = remote;

    _fd = ::socket(remote.getType(), SOCK_STREAM, 0);
    if (_fd == INVALID_SOCKET) {
        networkWarnWithDescription(*this, "socket");
        return false;
    }

    if (!setBlock(_fd, false)) {
        networkWarnWithDescription(*this, "set socket to non-blocking mode");
        return false;
    }

    const Date_t expiration = Date_t::now() + connectTimeoutMillis;
    bool connectSucceeded = ::connect(_fd, _remote.raw(), _remote.addressSize) == 0;

    if (!connectSucceeded) {
#ifdef _WIN32
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            networkWarnWithDescription(*this, "connect");
            return false;
        }
#else
        if (errno != EINTR && errno != EINPROGRESS) {
            networkWarnWithDescription(*this, "connect");
            return false;
        }
#endif

        pollfd pfd;
        pfd.fd = _fd;
        pfd.events = POLLOUT;

        while (true) {
            const auto timeout = std::max(Milliseconds(0), expiration - Date_t::now());

            int pollReturn = socketPoll(&pfd, 1, timeout.count());
#ifdef _WIN32
            if (pollReturn == SOCKET_ERROR) {
                networkWarnWithDescription(*this, "poll");
                return false;
            }
#else
            if (pollReturn == -1) {
                if (errno != EINTR) {
                    networkWarnWithDescription(*this, "poll");
                    return false;
                }

                // EINTR in poll, try again
                continue;
            }
#endif
            // No activity for the full duration of the timeout.
            if (pollReturn == 0) {
                LOGV2_WARNING(23192,
                              "Failed to connect to {remoteAddr}:{remotePort} after "
                              "{connectTimeout} milliseconds, giving up.",
                              "Failed to connect to remote host. Giving up",
                              "remoteAddr"_attr = _remote.getAddr(),
                              "remotePort"_attr = _remote.getPort(),
                              "connectTimeout"_attr = connectTimeoutMillis);
                return false;
            }

            // We had a result, see if there's an error on the socket.
            int optVal;
            socklen_t optLen = sizeof(optVal);
            if (::getsockopt(
                    _fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&optVal), &optLen) == -1) {
                networkWarnWithDescription(*this, "getsockopt");
                return false;
            }
            if (optVal != 0) {
                networkWarnWithDescription(*this, "checking socket for error after poll", optVal);
                return false;
            }

            // We had activity and we don't have errors on the socket, we're connected.
            break;
        }
    }

    if (!setBlock(_fd, true)) {
        networkWarnWithDescription(*this, "could not set socket to blocking mode");
        return false;
    }

    if (_timeout > 0) {
        setTimeout(_timeout);
    }

    if (remote.getType() != AF_UNIX)
        disableNagle(_fd);

#ifdef SO_NOSIGPIPE
    // ignore SIGPIPE signals on osx, to avoid process exit
    const int one = 1;
    setsockopt(_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(int));
#endif

    _local = getLocalAddrForBoundSocketFd(_fd);

    _fdCreationMicroSec = curTimeMicros64();

    _awaitingHandshake = false;

    return true;
}

// throws if SSL_write or send fails
int Socket::_send(const char* data, int len, const char* context) {
#ifdef MONGO_CONFIG_SSL
    if (_sslConnection.get()) {
        return _sslManager->SSL_write(_sslConnection.get(), data, len);
    }
#endif
    int ret = ::send(_fd, data, len, portSendFlags);

    return ret;
}

// sends all data or throws an exception
void Socket::send(const char* data, int len, const char* context) {
    while (len > 0) {
        int ret = -1;
        if (MONGO_unlikely(throwSockExcep.shouldFail())) {
#if defined(_WIN32)
            WSASetLastError(WSAENETUNREACH);
#else
            errno = ENETUNREACH;
#endif
        } else {
            ret = _send(data, len, context);
        }

        if (ret < 0) {
            handleSendError(ret, context);
            continue;
        }

        _bytesOut += ret;

        fassert(16507, ret <= len);
        len -= ret;
        data += ret;
    }
}

void Socket::_send(const vector<pair<char*, int>>& data, const char* context) {
    for (vector<pair<char*, int>>::const_iterator i = data.begin(); i != data.end(); ++i) {
        char* data = i->first;
        int len = i->second;
        send(data, len, context);
    }
}

/** sends all data or throws an exception
 * @param context descriptive for logging
 */
void Socket::send(const vector<pair<char*, int>>& data, const char* context) {
#ifdef MONGO_CONFIG_SSL
    if (_sslConnection.get()) {
        _send(data, context);
        return;
    }
#endif

#if defined(_WIN32)
    // TODO use scatter/gather api
    _send(data, context);
#else
    vector<struct iovec> d(data.size());
    int i = 0;
    for (vector<pair<char*, int>>::const_iterator j = data.begin(); j != data.end(); ++j) {
        if (j->second > 0) {
            d[i].iov_base = j->first;
            d[i].iov_len = j->second;
            ++i;
            _bytesOut += j->second;
        }
    }
    struct msghdr meta;
    memset(&meta, 0, sizeof(meta));
    meta.msg_iov = &d[0];
    meta.msg_iovlen = d.size();

    while (meta.msg_iovlen > 0) {
        int ret = -1;
        if (MONGO_unlikely(throwSockExcep.shouldFail())) {
#if defined(_WIN32)
            WSASetLastError(WSAENETUNREACH);
#else
            errno = ENETUNREACH;
#endif
        } else {
            ret = ::sendmsg(_fd, &meta, portSendFlags);
        }

        if (ret == -1) {
            handleSendError(ret, context);
        } else {
            struct iovec*& i = meta.msg_iov;
            while (ret > 0) {
                if (i->iov_len > unsigned(ret)) {
                    i->iov_len -= ret;
                    i->iov_base = (char*)(i->iov_base) + ret;
                    ret = 0;
                } else {
                    ret -= i->iov_len;
                    ++i;
                    --(meta.msg_iovlen);
                }
            }
        }
    }
#endif
}

void Socket::recv(char* buf, int len) {
    while (len > 0) {
        int ret = -1;
        if (MONGO_unlikely(throwSockExcep.shouldFail())) {
#if defined(_WIN32)
            WSASetLastError(WSAENETUNREACH);
#else
            errno = ENETUNREACH;
#endif
            if (ret <= 0) {
                handleRecvError(ret, len);
                continue;
            }
        } else {
            ret = unsafe_recv(buf, len);
        }

        fassert(16508, ret <= len);
        len -= ret;
        buf += ret;
    }
}

int Socket::unsafe_recv(char* buf, int max) {
    int x = _recv(buf, max);
    _bytesIn += x;
    return x;
}

// throws if SSL_read fails or recv returns an error
int Socket::_recv(char* buf, int max) {
#ifdef MONGO_CONFIG_SSL
    if (_sslConnection.get()) {
        return _sslManager->SSL_read(_sslConnection.get(), buf, max);
    }
#endif
    int ret = ::recv(_fd, buf, max, portRecvFlags);
    if (ret <= 0) {
        handleRecvError(ret, max);  // If no throw return and call _recv again
        return 0;
    }
    return ret;
}

void Socket::handleSendError(int ret, const char* context) {
#if defined(_WIN32)
    const int mongo_errno = WSAGetLastError();
    if (mongo_errno == WSAETIMEDOUT && _timeout != 0) {
#else
    const int mongo_errno = errno;
    if ((mongo_errno == EAGAIN || mongo_errno == EWOULDBLOCK) && _timeout != 0) {
#endif
        LOGV2_DEBUG(23181,
                    _logLevel.toInt(),
                    "Socket {context} send() timed out {remoteHost}",
                    "Socket send() to remote host timed out",
                    "context"_attr = context,
                    "remoteHost"_attr = remoteString());
        throwSocketError(SocketErrorKind::SEND_TIMEOUT, remoteString());
    } else if (mongo_errno != EINTR) {
        LOGV2_DEBUG(23182,
                    _logLevel.toInt(),
                    "Socket {context} send() {error} {remoteHost}",
                    "Socket send() to remote host failed",
                    "context"_attr = context,
                    "error"_attr = errnoWithDescription(mongo_errno),
                    "remoteHost"_attr = remoteString());
        throwSocketError(SocketErrorKind::SEND_ERROR, remoteString());
    }
}  // namespace mongo

void Socket::handleRecvError(int ret, int len) {
    if (ret == 0) {
        LOGV2_DEBUG(23183,
                    3,
                    "Socket recv() conn closed? {remoteHost}",
                    "Socket recv() failed; connection may have been closed",
                    "remoteHost"_attr = remoteString());
        throwSocketError(SocketErrorKind::CLOSED, remoteString());
    }

// ret < 0
#if defined(_WIN32)
    int e = WSAGetLastError();
#else
    int e = errno;
#if defined(EINTR)
    if (e == EINTR) {
        return;
    }
#endif
#endif

#if defined(_WIN32)
    // Windows
    if ((e == EAGAIN || e == WSAETIMEDOUT) && _timeout > 0) {
#else
    if (e == EAGAIN && _timeout > 0) {
#endif
        // this is a timeout
        LOGV2_DEBUG(23184,
                    _logLevel.toInt(),
                    "Socket recv() timeout {remoteHost}",
                    "Socket recv() timeout",
                    "remoteHost"_attr = remoteString());
        throwSocketError(SocketErrorKind::RECV_TIMEOUT, remoteString());
    }

    LOGV2_DEBUG(23185,
                _logLevel.toInt(),
                "Socket recv() {error} {remoteHost}",
                "Socket recv() error",
                "error"_attr = errnoWithDescription(e),
                "remoteHost"_attr = remoteString());
    throwSocketError(SocketErrorKind::RECV_ERROR, remoteString());
}

void Socket::setTimeout(double secs) {
    setSockTimeouts(_fd, secs);
}

// TODO: allow modification?
//
// <positive value> : secs to wait between stillConnected checks
// 0 : always check
// -1 : never check
const int Socket::errorPollIntervalSecs(5);

// Patch to allow better tolerance of flaky network connections that get broken
// while we aren't looking.
// TODO: Remove when better async changes come.
//
// isStillConnected() polls the socket at max every Socket::errorPollIntervalSecs to determine
// if any disconnection-type events have happened on the socket.
bool Socket::isStillConnected() {
    if (_fd == INVALID_SOCKET) {
        // According to the man page, poll will respond with POLLVNAL for invalid or
        // unopened descriptors, but it doesn't seem to be properly implemented in
        // some platforms - it can return 0 events and 0 for revent. Hence this workaround.
        return false;
    }

    if (errorPollIntervalSecs < 0)
        return true;

    time_t now = time(nullptr);
    time_t idleTimeSecs = now - _lastValidityCheckAtSecs;

    // Only check once every 5 secs
    if (idleTimeSecs < errorPollIntervalSecs)
        return true;
    // Reset our timer, we're checking the connection
    _lastValidityCheckAtSecs = now;

    // It's been long enough, poll to see if our socket is still connected

    pollfd pollInfo;
    pollInfo.fd = _fd;
    // We only care about reading the EOF message on clean close (and errors)
    pollInfo.events = POLLIN;

    // Poll( info[], size, timeout ) - timeout == 0 => nonblocking
    int nEvents = socketPoll(&pollInfo, 1, 0);

    LOGV2_DEBUG(
        23186,
        2,
        "polling for status of connection to {remoteHost}, {errorOrEventDetected}",
        "Polling for status of connection to remote host",
        "remoteHost"_attr = remoteString(),
        "errorOrEventDetected"_attr =
            (nEvents == 0 ? "no events" : nEvents == -1 ? "error detected" : "event detected"));

    if (nEvents == 0) {
        // No events incoming, return still connected AFAWK
        return true;
    } else if (nEvents < 0) {
        // Poll itself failed, this is weird, warn and log errno
        LOGV2_WARNING(23193,
                      "Socket poll() failed during connectivity check (idle {idleTimeSecs} secs, "
                      "remote host {remoteHost}){error}",
                      "Socket poll() to remote host failed during connectivity check",
                      "idleTimeSecs"_attr = idleTimeSecs,
                      "remoteHost"_attr = remoteString(),
                      "error"_attr = causedBy(errnoWithDescription()));

        // Return true since it's not clear that we're disconnected.
        return true;
    }

    dassert(nEvents == 1);
    dassert(pollInfo.revents > 0);

    // Return false at this point, some event happened on the socket, but log what the
    // actual event was.

    if (pollInfo.revents & POLLIN) {
        // There shouldn't really be any data to recv here, so make sure this
        // is a clean hangup.

        const int testBufLength = 1024;
        char testBuf[testBufLength];

        int recvd = ::recv(_fd, testBuf, testBufLength, portRecvFlags);

        if (recvd < 0) {
            // An error occurred during recv, warn and log errno
            LOGV2_WARNING(23194,
                          "Socket recv() failed during connectivity check (idle {idleTimeSecs} "
                          "secs, remote host {remoteHost}){error}",
                          "Socket recv() failed during connectivity check",
                          "idleTimeSecs"_attr = idleTimeSecs,
                          "remoteHost"_attr = remoteString(),
                          "error"_attr = causedBy(errnoWithDescription()));
        } else if (recvd > 0) {
            // We got nonzero data from this socket, very weird?
            // Log and warn at runtime, log and abort at devtime
            // TODO: Dump the data to the log somehow?
            LOGV2_ERROR(23197,
                        "Socket found pending {recvd} bytes of data during connectivity "
                        "check (idle {idleTimeSecs} secs, remote host {remoteHost})",
                        "Socket found pending bytes of data during connectivity check to remote "
                        "host",
                        "recvd"_attr = recvd,
                        "idleTimeSecs"_attr = idleTimeSecs,
                        "remoteHost"_attr = remoteString());
            if (kDebugBuild) {
                LOGV2_ERROR(23198,
                            "Hex dump of stale log data: {hex}",
                            "Hex dump of stale log data",
                            "hex"_attr = hexdump(testBuf, recvd));
            }
            dassert(false);
        } else {
            // recvd == 0, socket closed remotely, just return false
            LOGV2(23187,
                  "Socket closed remotely, no longer connected (idle {idleTimeSecs} secs, remote "
                  "host {remoteHost})",
                  "Socket closed remotely, no longer connected to remote host",
                  "idleTimeSecs"_attr = idleTimeSecs,
                  "remoteHost"_attr = remoteString());
        }
    } else if (pollInfo.revents & POLLHUP) {
        // A hangup has occurred on this socket
        LOGV2(23188,
              "Socket hangup detected, no longer connected (idle {idleTimeSecs} secs, remote host "
              "{remoteHost})",
              "Socket hangup detected, no longer connected to remote host",
              "idleTimeSecs"_attr = idleTimeSecs,
              "remoteHost"_attr = remoteString());
    } else if (pollInfo.revents & POLLERR) {
        // An error has occurred on this socket
        LOGV2(23189,
              "Socket error detected, no longer connected (idle {idleTimeSecs} secs, remote host "
              "{remoteHost})",
              "Socket error detected, no longer connected to remote host",
              "idleTimeSecs"_attr = idleTimeSecs,
              "remoteHost"_attr = remoteString());
    } else if (pollInfo.revents & POLLNVAL) {
        // Socket descriptor itself is weird
        // Log and warn at runtime, log and abort at devtime
        LOGV2_ERROR(23199,
                    "Socket descriptor detected as invalid (idle {idleTimeSecs} secs, remote host "
                    "{remoteHost})",
                    "Socket descriptor detected as invalid",
                    "idleTimeSecs"_attr = idleTimeSecs,
                    "remoteHost"_attr = remoteString());
        dassert(false);
    } else {
        // Don't know what poll is saying here
        // Log and warn at runtime, log and abort at devtime
        LOGV2_ERROR(23200,
                    "Socket had unknown event ({pollEvents}) (idle "
                    "{idleTimeSecs} secs, remote host {remoteString})",
                    "Socket had unknown event",
                    "pollEvents"_attr = static_cast<int>(pollInfo.revents),
                    "idleTimeSecs"_attr = idleTimeSecs,
                    "remoteString"_attr = remoteString());
        dassert(false);
    }

    return false;
}

}  // namespace mongo
