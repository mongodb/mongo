// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <cstdio>

#ifndef _WIN32

#include <cerrno>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#ifdef __OpenBSD__
#include <sys/uio.h>
#endif

#endif  // not _WIN32

#include "mongo/config.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/sockaddr.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

#ifdef MONGO_CONFIG_SSL
class SSLManagerInterface;
class SSLConnectionInterface;
#endif
class SSLPeerInfo;

extern const int portSendFlags;
extern const int portRecvFlags;

#if !defined(_WIN32)

inline void closesocket(int s) {
    close(s);
}
const int INVALID_SOCKET = -1;
typedef int SOCKET;

#endif  // _WIN32

/**
 * thin wrapper around file descriptor and system calls
 * todo: ssl
 */
class Socket {
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

public:
    static const int errorPollIntervalSecs;

    Socket(int sock, const SockAddr& remote);

    /** In some cases the timeout will actually be 2x this value - eg we do a partial send,
        then the timeout fires, then we try to send again, then the timeout fires again with
        no data sent, then we detect that the other side is down.

        Generally you don't want a timeout, you should be very prepared for errors if you set one.
    */
    Socket(double so_timeout = 0, logv2::LogSeverity logLevel = logv2::LogSeverity::Log());

    ~Socket();

    /** The correct way to initialize and connect to a socket is as follows: (1) construct the
     *  SockAddr, (2) check whether the SockAddr isValid(), (3) if the SockAddr is valid, a
     *  Socket may then try to connect to that SockAddr. It is critical to check the return
     *  value of connect as a false return indicates that there was an error, and the Socket
     *  failed to connect to the given SockAddr. This failure may be due to ConnectBG returning
     *  an error, or due to a timeout on connection, or due to the system socket deciding the
     *  socket is invalid.
     */
    bool connect(const SockAddr& remote, Milliseconds connectTimeoutMillis);

    /**
     * Connect using a default connect timeout of min(_timeout * 1000, kMaxConnectTimeoutMS)
     */
    bool connect(const SockAddr& remote);

    void close();
    void send(const char* data, int len, const char* context);
    void send(const std::vector<std::pair<char*, int>>& data, const char* context);

    // recv len or throw SocketException
    void recv(char* data, int len);
    int unsafe_recv(char* buf, int max);

    logv2::LogSeverity getLogLevel() const {
        return _logLevel;
    }
    void setLogLevel(logv2::LogSeverity ll) {
        _logLevel = ll;
    }

    SockAddr remoteAddr() const {
        return _remote;
    }
    std::string remoteString() const {
        return _remote.toString();
    }
    unsigned remotePort() const {
        return _remote.getPort();
    }

    SockAddr localAddr() const {
        return _local;
    }

    void clearCounters() {
        _bytesIn = 0;
        _bytesOut = 0;
    }
    long long getBytesIn() const {
        return _bytesIn;
    }
    long long getBytesOut() const {
        return _bytesOut;
    }
    int rawFD() const {
        return _fd;
    }

    /**
     * This sets the Sock's socket descriptor to be invalid and returns the old descriptor. This
     * only gets called in listen.cpp in Listener::_accepted(). This gets called on the listener
     * thread immediately after the thread creates the Sock, so it doesn't need to be thread-safe.
     */
    int stealSD() {
        int tmp = _fd;
        _fd = -1;
        return tmp;
    }

    void setTimeout(double secs);
    bool isStillConnected();

    void setHandshakeReceived() {
        _awaitingHandshake = false;
    }

    bool isAwaitingHandshake() {
        return _awaitingHandshake;
    }

#ifdef MONGO_CONFIG_SSL
    /** secures inline
     *  ssl - Pointer to the global SSLManager.
     *  remoteHost - The hostname of the remote server.
     */
    Status secure(SSLManagerInterface* ssl, const std::string& remoteHost);

    void secureAccepted(SSLManagerInterface* ssl);
#endif

    /**
     * This function calls SSL_accept() if SSL-encrypted sockets
     * are desired. SSL_accept() waits until the remote host calls
     * SSL_connect(). The return value is the subject name of any
     * client certificate provided during the handshake.
     *
     * @firstBytes is the first bytes received on the socket used
     * to detect the connection SSL, @len is the number of bytes
     *
     * This function may throw SocketException.
     */
    SSLPeerInfo doSSLHandshake(const char* firstBytes = nullptr, int len = 0);

    /**
     * @return the time when the socket was opened.
     */
    uint64_t getSockCreationMicroSec() const {
        return _fdCreationMicroSec;
    }

    void handleRecvError(int ret, int len);
    void handleSendError(int ret, const char* context);

private:
    void _init();

    /** sends dumbly, just each buffer at a time */
    void _send(const std::vector<std::pair<char*, int>>& data, const char* context);

    /** raw send, same semantics as ::send with an additional context parameter */
    int _send(const char* data, int len, const char* context);

    /** raw recv, same semantics as ::recv */
    int _recv(char* buf, int max);

    SOCKET _fd;
    uint64_t _fdCreationMicroSec = 0;
    SockAddr _local;
    SockAddr _remote;
    double _timeout;

    long long _bytesIn;
    long long _bytesOut;
    time_t _lastValidityCheckAtSecs;

#ifdef MONGO_CONFIG_SSL
    std::unique_ptr<SSLConnectionInterface> _sslConnection;
    SSLManagerInterface* _sslManager;
#endif
    logv2::LogSeverity _logLevel;  // passed to log() when logging errors

    /** true until the first packet has been received or an outgoing connect has been made */
    bool _awaitingHandshake;
};

}  // namespace mongo
