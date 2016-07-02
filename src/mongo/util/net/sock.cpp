// @file sock.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/sock.h"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#if defined(__OpenBSD__)
#include <sys/uio.h>
#endif
#endif

#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/value.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/socket_poll.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/quick_exit.h"

namespace mongo {

using std::endl;
using std::pair;
using std::string;
using std::stringstream;
using std::vector;

MONGO_FP_DECLARE(throwSockExcep);

static bool ipv6 = false;
void enableIPv6(bool state) {
    ipv6 = state;
}
bool IPv6Enabled() {
    return ipv6;
}

void setSockTimeouts(int sock, double secs) {
    bool report = shouldLog(logger::LogSeverity::Debug(4));
    DEV report = true;
#if defined(_WIN32)
    DWORD timeout = secs * 1000;  // Windows timeout is a DWORD, in milliseconds.
    int status =
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(DWORD));
    if (report && (status == SOCKET_ERROR))
        log() << "unable to set SO_RCVTIMEO: " << errnoWithDescription(WSAGetLastError()) << endl;
    status =
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(DWORD));
    DEV if (report && (status == SOCKET_ERROR)) log()
        << "unable to set SO_SNDTIMEO: " << errnoWithDescription(WSAGetLastError()) << endl;
#else
    struct timeval tv;
    tv.tv_sec = (int)secs;
    tv.tv_usec = (int)((long long)(secs * 1000 * 1000) % (1000 * 1000));
    bool ok = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv)) == 0;
    if (report && !ok)
        log() << "unable to set SO_RCVTIMEO" << endl;
    ok = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv)) == 0;
    DEV if (report && !ok) log() << "unable to set SO_SNDTIMEO" << endl;
#endif
}

#if defined(_WIN32)
void disableNagle(int sock) {
    int x = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&x, sizeof(x)))
        error() << "disableNagle failed: " << errnoWithDescription() << endl;
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*)&x, sizeof(x)))
        error() << "SO_KEEPALIVE failed: " << errnoWithDescription() << endl;
}
#else

void disableNagle(int sock) {
    int x = 1;

#ifdef SOL_TCP
    int level = SOL_TCP;
#else
    int level = SOL_SOCKET;
#endif

    if (setsockopt(sock, level, TCP_NODELAY, (char*)&x, sizeof(x)))
        error() << "disableNagle failed: " << errnoWithDescription() << endl;

#ifdef SO_KEEPALIVE
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*)&x, sizeof(x)))
        error() << "SO_KEEPALIVE failed: " << errnoWithDescription() << endl;

#ifdef __linux__
    socklen_t len = sizeof(x);
    if (getsockopt(sock, level, TCP_KEEPIDLE, (char*)&x, &len))
        error() << "can't get TCP_KEEPIDLE: " << errnoWithDescription() << endl;

    if (x > 300) {
        x = 300;
        if (setsockopt(sock, level, TCP_KEEPIDLE, (char*)&x, sizeof(x))) {
            error() << "can't set TCP_KEEPIDLE: " << errnoWithDescription() << endl;
        }
    }

    len = sizeof(x);  // just in case it changed
    if (getsockopt(sock, level, TCP_KEEPINTVL, (char*)&x, &len))
        error() << "can't get TCP_KEEPINTVL: " << errnoWithDescription() << endl;

    if (x > 300) {
        x = 300;
        if (setsockopt(sock, level, TCP_KEEPINTVL, (char*)&x, sizeof(x))) {
            error() << "can't set TCP_KEEPINTVL: " << errnoWithDescription() << endl;
        }
    }
#endif
#endif
}

#endif

string getAddrInfoStrError(int code) {
#if !defined(_WIN32)
    return gai_strerror(code);
#else
    /* gai_strerrorA is not threadsafe on windows. don't use it. */
    return errnoWithDescription(code);
#endif
}

// --- SockAddr

string makeUnixSockPath(int port) {
    return mongoutils::str::stream() << serverGlobalParams.socket << "/mongodb-" << port << ".sock";
}


// If an ip address is passed in, just return that.  If a hostname is passed
// in, look up its ip and return that.  Returns "" on failure.
string hostbyname(const char* hostname) {
    SockAddr sockAddr(hostname, 0);
    if (!sockAddr.isValid() || sockAddr.getAddr() == "0.0.0.0")
        return "";
    else
        return sockAddr.getAddr();
}

//  --- my --

DiagStr& _hostNameCached = *(new DiagStr);  // this is also written to from commands/cloud.cpp

string getHostName() {
    char buf[256];
    int ec = gethostname(buf, 127);
    if (ec || *buf == 0) {
        log() << "can't get this server's hostname " << errnoWithDescription() << endl;
        return "";
    }
    return buf;
}

/** we store our host name once */
string getHostNameCached() {
    string temp = _hostNameCached.get();
    if (_hostNameCached.empty()) {
        temp = getHostName();
        _hostNameCached = temp;
    }
    return temp;
}

string prettyHostName() {
    StringBuilder s;
    s << getHostNameCached();
    if (serverGlobalParams.port != ServerGlobalParams::DefaultDBPort)
        s << ':' << mongo::serverGlobalParams.port;
    return s.str();
}

#ifdef MSG_NOSIGNAL
const int portSendFlags = MSG_NOSIGNAL;
const int portRecvFlags = MSG_NOSIGNAL;
#else
const int portSendFlags = 0;
const int portRecvFlags = 0;
#endif

// ------------ Socket -----------------

static int socketGetLastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static SockAddr getLocalAddrForBoundSocketFd(int fd) {
    SockAddr result;
    int rc = getsockname(fd, result.raw(), &result.addressSize);
    if (rc != 0) {
        warning() << "Could not resolve local address for socket with fd " << fd << ": "
                  << getAddrInfoStrError(socketGetLastError());
        result = SockAddr();
    }
    return result;
}

Socket::Socket(int fd, const SockAddr& remote)
    : _fd(fd),
      _remote(remote),
      _timeout(0),
      _lastValidityCheckAtSecs(time(0)),
      _logLevel(logger::LogSeverity::Log()) {
    _init();
    if (fd >= 0) {
        _local = getLocalAddrForBoundSocketFd(_fd);
    }
}

Socket::Socket(double timeout, logger::LogSeverity ll) : _logLevel(ll) {
    _fd = INVALID_SOCKET;
    _timeout = timeout;
    _lastValidityCheckAtSecs = time(0);
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
    _sslManager = 0;
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
    mgr->parseAndValidatePeerCertificateDeprecated(_sslConnection.get(), remoteHost);
    return true;
}

void Socket::secureAccepted(SSLManagerInterface* ssl) {
    _sslManager = ssl;
}

std::string Socket::doSSLHandshake(const char* firstBytes, int len) {
    if (!_sslManager)
        return "";
    fassert(16506, _fd != INVALID_SOCKET);
    if (_sslConnection.get()) {
        throw SocketException(SocketException::RECV_ERROR,
                              "Attempt to call SSL_accept on already secure Socket from " +
                                  remoteString());
    }
    _sslConnection.reset(_sslManager->accept(this, firstBytes, len));
    return _sslManager->parseAndValidatePeerCertificateDeprecated(_sslConnection.get(), "");
}
#endif

class ConnectBG : public BackgroundJob {
public:
    ConnectBG(int sock, SockAddr remote) : _sock(sock), _remote(remote) {}

    void run() {
#if defined(_WIN32)
        if ((_res = _connect()) == SOCKET_ERROR) {
            _errnoWithDescription = errnoWithDescription();
        }
#else
        while ((_res = _connect()) == -1) {
            const int error = errno;
            if (error != EINTR) {
                _errnoWithDescription = errnoWithDescription(error);
                break;
            }
        }
#endif
    }

    std::string name() const {
        return "ConnectBG";
    }
    std::string getErrnoWithDescription() const {
        return _errnoWithDescription;
    }
    int inError() const {
        return _res;
    }

private:
    int _connect() const {
        return ::connect(_sock, _remote.raw(), _remote.addressSize);
    }

    int _sock;
    int _res;
    SockAddr _remote;
    std::string _errnoWithDescription;
};

bool Socket::connect(SockAddr& remote) {
    _remote = remote;

    _fd = socket(remote.getType(), SOCK_STREAM, 0);
    if (_fd == INVALID_SOCKET) {
        LOG(_logLevel) << "ERROR: connect invalid socket " << errnoWithDescription() << endl;
        return false;
    }

    if (_timeout > 0) {
        setTimeout(_timeout);
    }

    static const unsigned int connectTimeoutMillis = 5000;
    ConnectBG bg(_fd, remote);
    bg.go();
    if (bg.wait(connectTimeoutMillis)) {
        if (bg.inError()) {
            warning() << "Failed to connect to " << _remote.getAddr() << ":" << _remote.getPort()
                      << ", reason: " << bg.getErrnoWithDescription() << endl;
            close();
            return false;
        }
    } else {
        // time out the connect
        close();
        bg.wait();  // so bg stays in scope until bg thread terminates
        warning() << "Failed to connect to " << _remote.getAddr() << ":" << _remote.getPort()
                  << " after " << connectTimeoutMillis << " milliseconds, giving up." << endl;
        return false;
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
    if (ret < 0) {
        handleSendError(ret, context);
    }
    return ret;
}

// sends all data or throws an exception
void Socket::send(const char* data, int len, const char* context) {
    while (len > 0) {
        int ret = -1;
        if (MONGO_FAIL_POINT(throwSockExcep)) {
#if defined(_WIN32)
            WSASetLastError(WSAENETUNREACH);
#else
            errno = ENETUNREACH;
#endif
            handleSendError(ret, context);
        } else {
            ret = _send(data, len, context);
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
        if (MONGO_FAIL_POINT(throwSockExcep)) {
#if defined(_WIN32)
            WSASetLastError(WSAENETUNREACH);
#else
            errno = ENETUNREACH;
#endif
        } else {
            ret = ::sendmsg(_fd, &meta, portSendFlags);
        }

        if (ret == -1) {
            if (errno != EAGAIN || _timeout == 0) {
                LOG(_logLevel) << "Socket " << context << " send() " << errnoWithDescription()
                               << ' ' << remoteString() << endl;
                throw SocketException(SocketException::SEND_ERROR, remoteString());
            } else {
                LOG(_logLevel) << "Socket " << context << " send() remote timeout "
                               << remoteString() << endl;
                throw SocketException(SocketException::SEND_TIMEOUT, remoteString());
            }
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
        if (MONGO_FAIL_POINT(throwSockExcep)) {
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
        LOG(_logLevel) << "Socket " << context << " send() timed out " << remoteString() << endl;
        throw SocketException(SocketException::SEND_TIMEOUT, remoteString());
    } else {
        LOG(_logLevel) << "Socket " << context << " send() " << errnoWithDescription(mongo_errno)
                       << ' ' << remoteString() << endl;
        throw SocketException(SocketException::SEND_ERROR, remoteString());
    }
}

void Socket::handleRecvError(int ret, int len) {
    if (ret == 0) {
        LOG(3) << "Socket recv() conn closed? " << remoteString() << endl;
        throw SocketException(SocketException::CLOSED, remoteString());
    }

// ret < 0
#if defined(_WIN32)
    int e = WSAGetLastError();
#else
    int e = errno;
#if defined(EINTR)
    if (e == EINTR) {
        LOG(_logLevel) << "EINTR returned from recv(), retrying";
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
        LOG(_logLevel) << "Socket recv() timeout  " << remoteString() << endl;
        throw SocketException(SocketException::RECV_TIMEOUT, remoteString());
    }

    LOG(_logLevel) << "Socket recv() " << errnoWithDescription(e) << " " << remoteString() << endl;
    throw SocketException(SocketException::RECV_ERROR, remoteString());
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
    if (!isPollSupported())
        return true;  // nothing we can do

    time_t now = time(0);
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

    LOG(2) << "polling for status of connection to " << remoteString() << ", "
           << (nEvents == 0 ? "no events" : nEvents == -1 ? "error detected" : "event detected")
           << endl;

    if (nEvents == 0) {
        // No events incoming, return still connected AFAWK
        return true;
    } else if (nEvents < 0) {
        // Poll itself failed, this is weird, warn and log errno
        warning() << "Socket poll() failed during connectivity check"
                  << " (idle " << idleTimeSecs << " secs,"
                  << " remote host " << remoteString() << ")" << causedBy(errnoWithDescription())
                  << endl;

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
            warning() << "Socket recv() failed during connectivity check"
                      << " (idle " << idleTimeSecs << " secs,"
                      << " remote host " << remoteString() << ")"
                      << causedBy(errnoWithDescription()) << endl;
        } else if (recvd > 0) {
            // We got nonzero data from this socket, very weird?
            // Log and warn at runtime, log and abort at devtime
            // TODO: Dump the data to the log somehow?
            error() << "Socket found pending " << recvd
                    << " bytes of data during connectivity check"
                    << " (idle " << idleTimeSecs << " secs,"
                    << " remote host " << remoteString() << ")" << endl;
            DEV {
                std::string hex = hexdump(testBuf, recvd);
                error() << "Hex dump of stale log data: " << hex << endl;
            }
            dassert(false);
        } else {
            // recvd == 0, socket closed remotely, just return false
            LOG(0) << "Socket closed remotely, no longer connected"
                   << " (idle " << idleTimeSecs << " secs,"
                   << " remote host " << remoteString() << ")" << endl;
        }
    } else if (pollInfo.revents & POLLHUP) {
        // A hangup has occurred on this socket
        LOG(0) << "Socket hangup detected, no longer connected"
               << " (idle " << idleTimeSecs << " secs,"
               << " remote host " << remoteString() << ")" << endl;
    } else if (pollInfo.revents & POLLERR) {
        // An error has occurred on this socket
        LOG(0) << "Socket error detected, no longer connected"
               << " (idle " << idleTimeSecs << " secs,"
               << " remote host " << remoteString() << ")" << endl;
    } else if (pollInfo.revents & POLLNVAL) {
        // Socket descriptor itself is weird
        // Log and warn at runtime, log and abort at devtime
        error() << "Socket descriptor detected as invalid"
                << " (idle " << idleTimeSecs << " secs,"
                << " remote host " << remoteString() << ")" << endl;
        dassert(false);
    } else {
        // Don't know what poll is saying here
        // Log and warn at runtime, log and abort at devtime
        error() << "Socket had unknown event (" << static_cast<int>(pollInfo.revents) << ")"
                << " (idle " << idleTimeSecs << " secs,"
                << " remote host " << remoteString() << ")" << endl;
        dassert(false);
    }

    return false;
}

#if defined(_WIN32)
struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            log() << "ERROR: wsastartup failed " << errnoWithDescription() << endl;
            quickExit(EXIT_NTSERVICE_ERROR);
        }
    }
} winsock_init;
#endif

}  // namespace mongo
