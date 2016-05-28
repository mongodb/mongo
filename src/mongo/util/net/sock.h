// @file sock.h

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <stdio.h>

#ifndef _WIN32

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#ifdef __OpenBSD__
#include <sys/uio.h>
#endif

#endif  // not _WIN32

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/config.h"
#include "mongo/logger/log_severity.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/sockaddr.h"

namespace mongo {

#ifdef MONGO_CONFIG_SSL
class SSLManagerInterface;
class SSLConnection;
#endif

extern const int portSendFlags;
extern const int portRecvFlags;

const int SOCK_FAMILY_UNKNOWN_ERROR = 13078;

void disableNagle(int sock);

// Generate a string representation for getaddrinfo return codes
std::string getAddrInfoStrError(int code);

#if !defined(_WIN32)

inline void closesocket(int s) {
    close(s);
}
const int INVALID_SOCKET = -1;
typedef int SOCKET;

#endif  // _WIN32

std::string makeUnixSockPath(int port);

// If an ip address is passed in, just return that.  If a hostname is passed
// in, look up its ip and return that.  Returns "" on failure.
std::string hostbyname(const char* hostname);

void enableIPv6(bool state = true);
bool IPv6Enabled();
void setSockTimeouts(int sock, double secs);

/** this is not cache and does a syscall */
std::string getHostName();

/** this is cached, so if changes during the process lifetime
 * will be stale */
std::string getHostNameCached();

std::string prettyHostName();

/**
 * thin wrapped around file descriptor and system calls
 * todo: ssl
 */
class Socket {
    MONGO_DISALLOW_COPYING(Socket);

public:
    static const int errorPollIntervalSecs;

    Socket(int sock, const SockAddr& farEnd);

    /** In some cases the timeout will actually be 2x this value - eg we do a partial send,
        then the timeout fires, then we try to send again, then the timeout fires again with
        no data sent, then we detect that the other side is down.

        Generally you don't want a timeout, you should be very prepared for errors if you set one.
    */
    Socket(double so_timeout = 0, logger::LogSeverity logLevel = logger::LogSeverity::Log());

    ~Socket();

    /** The correct way to initialize and connect to a socket is as follows: (1) construct the
     *  SockAddr, (2) check whether the SockAddr isValid(), (3) if the SockAddr is valid, a
     *  Socket may then try to connect to that SockAddr. It is critical to check the return
     *  value of connect as a false return indicates that there was an error, and the Socket
     *  failed to connect to the given SockAddr. This failure may be due to ConnectBG returning
     *  an error, or due to a timeout on connection, or due to the system socket deciding the
     *  socket is invalid.
     */
    bool connect(SockAddr& farEnd);

    void close();
    void send(const char* data, int len, const char* context);
    void send(const std::vector<std::pair<char*, int>>& data, const char* context);

    // recv len or throw SocketException
    void recv(char* data, int len);
    int unsafe_recv(char* buf, int max);

    logger::LogSeverity getLogLevel() const {
        return _logLevel;
    }
    void setLogLevel(logger::LogSeverity ll) {
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
    bool secure(SSLManagerInterface* ssl, const std::string& remoteHost);

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
    std::string doSSLHandshake(const char* firstBytes = NULL, int len = 0);

    /**
     * @return the time when the socket was opened.
     */
    uint64_t getSockCreationMicroSec() const {
        return _fdCreationMicroSec;
    }

    void handleRecvError(int ret, int len);
    MONGO_COMPILER_NORETURN void handleSendError(int ret, const char* context);

private:
    void _init();

    /** sends dumbly, just each buffer at a time */
    void _send(const std::vector<std::pair<char*, int>>& data, const char* context);

    /** raw send, same semantics as ::send with an additional context parameter */
    int _send(const char* data, int len, const char* context);

    /** raw recv, same semantics as ::recv */
    int _recv(char* buf, int max);

    int _fd;
    uint64_t _fdCreationMicroSec;
    SockAddr _local;
    SockAddr _remote;
    double _timeout;

    long long _bytesIn;
    long long _bytesOut;
    time_t _lastValidityCheckAtSecs;

#ifdef MONGO_CONFIG_SSL
    std::unique_ptr<SSLConnection> _sslConnection;
    SSLManagerInterface* _sslManager;
#endif
    logger::LogSeverity _logLevel;  // passed to log() when logging errors

    /** true until the first packet has been received or an outgoing connect has been made */
    bool _awaitingHandshake;
};


}  // namespace mongo
