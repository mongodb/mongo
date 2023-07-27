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


#include "mongo/platform/basic.h"

#include "mongo/util/net/socket_utils.h"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/value.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/str.h"
#include "mongo/util/winutil.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

#if defined(_WIN32)
const struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        if (int e = WSAStartup(MAKEWORD(2, 2), &d)) {
            LOGV2(23201,
                  "ERROR: wsastartup failed {error}",
                  "ERROR: wsastartup failed",
                  "error"_attr = errorMessage(systemError(e)));
            quickExit(ExitCode::ntServiceError);
        }
    }
} winsock_init;
#endif

static bool ipv6 = false;
void enableIPv6(bool state) {
    ipv6 = state;
}
bool IPv6Enabled() {
    return ipv6;
}

namespace {

#ifdef _WIN32
/**
 * Search the registry for the `key` TCP parameter, a millisecond value.
 * Returns an empty optional on error or if key is not found.
 */
boost::optional<Milliseconds> getTcpMillisKey(const CString& key, logv2::LogSeverity logSeverity) {
    auto swValOpt = windows::getDWORDRegistryKey(
        _T("SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters"), key);
    if (!swValOpt.isOK()) {
        LOGV2_DEBUG(23203,
                    logSeverity.toInt(),
                    "can't get KeepAlive parameter: {error}",
                    "Can't get KeepAlive parameter",
                    "error"_attr = swValOpt.getStatus());
        return {};
    }
    const auto& oDword = swValOpt.getValue();
    if (!oDword)
        return {};
    return Milliseconds{*oDword};
}

template <typename In>
std::error_code windowsSetIoctl(int sock, DWORD controlCode, In&& in) {
    DWORD outSize = 0;
    if (WSAIoctl(sock, controlCode, &in, sizeof(in), nullptr, 0, &outSize, nullptr, nullptr))
        return lastSocketError();
    return {};
}

/**
 * Configure a socket's TCP keepalive behavior. The first keepalive probe is
 * sent after `idle` with no received packets. Subsequent probes are sent after
 * each `interval`.
 */
std::error_code windowsSetTcpKeepAlive(int sock, Milliseconds idle, Milliseconds interval) {
    auto uMsec = [](const auto& d) {
        return u_long(durationCount<Milliseconds>(d));
    };
    struct tcp_keepalive keepalive;
    keepalive.onoff = TRUE;
    keepalive.keepalivetime = uMsec(idle);
    keepalive.keepaliveinterval = uMsec(interval);
    return windowsSetIoctl(sock, SIO_KEEPALIVE_VALS, keepalive);
}

/**
 * Configure a TCP socket's keepalive options on Windows.
 *
 * On Windows, there has historically been no way to query a socket's TCP
 * keepalive parameters. We guess from the registry what the settings would be.
 * If the registry is missing a key, we use the defaults documented at MSDN.
 *
 * As of Windows 10, version 1709, the TCP keepalive settings are available as
 * IPPROTO_TCP options just as on macOS and Linux, and this setup can be much
 * simpler eventually.
 */
void windowsApplyMaxTcpKeepAlive(int sock,
                                 logv2::LogSeverity logSeverity,
                                 Milliseconds maxIdle,
                                 Milliseconds maxInterval) {
    static constexpr auto defaultIdle = Hours{2};
    static constexpr auto defaultInterval = Seconds{1};
    auto idle = getTcpMillisKey(_T("KeepAliveTime"), logSeverity).value_or(defaultIdle);
    auto interval = getTcpMillisKey(_T("KeepAliveInterval"), logSeverity).value_or(defaultInterval);
    if (idle > maxIdle || interval > maxInterval) {
        idle = std::min(idle, maxIdle);
        interval = std::min(interval, maxInterval);
        if (auto ec = windowsSetTcpKeepAlive(sock, idle, interval))
            LOGV2_DEBUG(23204,
                        logSeverity.toInt(),
                        "failed setting keepalive values: {error}",
                        "Failed setting keepalive values",
                        "error"_attr = errorMessage(ec));
    }
}
#endif  // _WIN32

template <typename T>
void getSocketOption(int sock, int level, int option, T& val) {
    socklen_t size = sizeof(val);
    if (getsockopt(sock, level, option, &val, &size))
        throw std::system_error(lastSocketError());
}

template <typename T>
void setSocketOption(int sock, int level, int option, const T& val) {
    if (setsockopt(sock, level, option, &val, sizeof(val)))
        throw std::system_error(lastSocketError());
}

/**
 * Applies a maximum to a socket option. Gets the specified option from `sock`,
 * and if its current value is greater than `maxVal`, sets it to `maxVal`.
 * Failures are logged with `severity`, and if the get operation fails, we do
 * not attempt the set operation.
 */
template <typename T>
void applyMax(
    int sock, int level, int optnum, T maxVal, StringData optName, logv2::LogSeverity severity) {
    T val;
    try {
        getSocketOption(sock, level, optnum, val);
    } catch (const std::system_error& ex) {
        LOGV2_DEBUG(23205,
                    severity.toInt(),
                    "can't get {optname}: {error}",
                    "Can't get socket option",
                    "optname"_attr = optName,
                    "error"_attr = errorMessage(ex.code()));
        return;
    }

    if (val <= maxVal)
        return;

    try {
        setSocketOption(sock, level, optnum, maxVal);
    } catch (const std::system_error& ex) {
        LOGV2_DEBUG(23206,
                    severity.toInt(),
                    "can't set {optname}: {error}",
                    "Can't set socket option",
                    "optname"_attr = optName,
                    "error"_attr = errorMessage(ex.code()));
    }
}
}  // namespace

void setSocketKeepAliveParams(int sock,
                              logv2::LogSeverity severity,
                              Seconds maxIdle,
                              Seconds maxInterval) {
#if defined(_WIN32)
    // Windows implementation is funky enough to get its own forwarded function.
    // More modern Windows versions (i.e. >1709) would support the `applyMax`
    // steps, and we'll be able to get rid of this special case eventually.
    windowsApplyMaxTcpKeepAlive(sock, severity, maxIdle, maxInterval);
#else  // _WIN32
#if defined(__APPLE__)
    int idleOpt = TCP_KEEPALIVE;
#else
    int idleOpt = TCP_KEEPIDLE;
#endif
    auto iSec = [](const auto& d) -> int {
        return durationCount<Seconds>(d);
    };
    applyMax(sock, IPPROTO_TCP, idleOpt, iSec(maxIdle), "TCP_KEEPIDLE", severity);
    applyMax(sock, IPPROTO_TCP, TCP_KEEPINTVL, iSec(maxInterval), "TCP_KEEPINTVL", severity);
#endif  // _WIN32
}

std::string makeUnixSockPath(int port) {
    return str::stream() << serverGlobalParams.socket << "/mongodb-" << port << ".sock";
}

#ifndef _WIN32
void setUnixDomainSocketPermissions(const std::string& path, int permissions) {
    if (::chmod(path.c_str(), permissions) == -1) {
        auto ec = lastPosixError();
        LOGV2_ERROR(23026,
                    "Failed to chmod socket file",
                    "path"_attr = path.c_str(),
                    "error"_attr = errorMessage(ec));
        fassertFailedNoTrace(40487);
    }
}
#endif

// If an ip address is passed in, just return that.  If a hostname is passed
// in, look up its ip and return that.  Returns "" on failure.
std::string hostbyname(const char* hostname) {
    try {
        auto addr = SockAddr::create(hostname, 0, IPv6Enabled() ? AF_UNSPEC : AF_INET).getAddr();
        if (addr == "0.0.0.0") {
            return "";
        }

        return addr;
    } catch (const DBException&) {
        return "";
    }
}

//  --- my --

DiagStr& _hostNameCached = *(new DiagStr);  // this is also written to from commands/cloud.cpp

std::string getHostName() {
    char buf[256];
    int ec = gethostname(buf, 127);
    if (ec || *buf == 0) {
        auto ec = lastSocketError();
        LOGV2(23202,
              "can't get this server's hostname {error}",
              "Can't get this server's hostname",
              "error"_attr = errorMessage(ec));
        return "";
    }
    return buf;
}

/** we store our host name once */
std::string getHostNameCached() {
    std::string temp = _hostNameCached.get();
    if (_hostNameCached.empty()) {
        temp = getHostName();
        _hostNameCached = temp;
    }
    return temp;
}

std::string getHostNameCachedAndPort() {
    return str::stream() << getHostNameCached() << ':' << serverGlobalParams.port;
}

std::string prettyHostName() {
    return (serverGlobalParams.port == ServerGlobalParams::DefaultDBPort
                ? getHostNameCached()
                : getHostNameCachedAndPort());
}

}  // namespace mongo
