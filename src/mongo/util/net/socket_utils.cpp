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

#ifdef _WIN32
#ifdef _UNICODE
#define X_STR_CONST(str) (L##str)
#else
#define X_STR_CONST(str) (str)
#endif
const CString kKeepAliveGroup(
    X_STR_CONST("SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters"));
const CString kKeepAliveTime(X_STR_CONST("KeepAliveTime"));
const CString kKeepAliveInterval(X_STR_CONST("KeepAliveInterval"));
#undef X_STR_CONST
#endif

void setSocketKeepAliveParams(int sock,
                              logv2::LogSeverity errorLogSeverity,
                              Seconds maxKeepIdleSecs,
                              Seconds maxKeepIntvlSecs) {
    int logSeverity = errorLogSeverity.toInt();

#ifdef _WIN32
    // Defaults per MSDN when registry key does not exist.
    // Expressed in seconds here to be consistent with posix,
    // though Windows uses milliseconds.
    static constexpr Seconds kWindowsKeepAliveTimeDefault{Hours{2}};
    static constexpr Seconds kWindowsKeepAliveIntervalDefault{1};

    const auto getKey = [&](const CString& key, Seconds defaultValue) {
        auto swValOpt = windows::getDWORDRegistryKey(kKeepAliveGroup, key);
        if (swValOpt.isOK()) {
            auto valOpt = swValOpt.getValue();
            // Return seconds
            return valOpt ? duration_cast<Seconds>(Milliseconds(valOpt.get())) : defaultValue;
        }
        LOGV2_DEBUG(23203,
                    logSeverity,
                    "can't get KeepAlive parameter: {error}",
                    "Can't get KeepAlive parameter",
                    "error"_attr = swValOpt.getStatus());

        return defaultValue;
    };

    const auto keepIdleSecs = getKey(kKeepAliveTime, kWindowsKeepAliveTimeDefault);
    const auto keepIntvlSecs = getKey(kKeepAliveInterval, kWindowsKeepAliveIntervalDefault);

    if ((keepIdleSecs > maxKeepIdleSecs) || (keepIntvlSecs > maxKeepIntvlSecs)) {
        DWORD sent = 0;
        struct tcp_keepalive keepalive;
        keepalive.onoff = TRUE;
        keepalive.keepalivetime =
            durationCount<Milliseconds>(std::min(keepIdleSecs, maxKeepIdleSecs));
        keepalive.keepaliveinterval =
            durationCount<Milliseconds>(std::min(keepIntvlSecs, maxKeepIntvlSecs));
        if (WSAIoctl(sock,
                     SIO_KEEPALIVE_VALS,
                     &keepalive,
                     sizeof(keepalive),
                     nullptr,
                     0,
                     &sent,
                     nullptr,
                     nullptr)) {
            auto ec = lastSocketError();
            LOGV2_DEBUG(23204,
                        logSeverity,
                        "failed setting keepalive values: {error}",
                        "Failed setting keepalive values",
                        "error"_attr = errorMessage(ec));
        }
    }
#elif defined(__APPLE__) || defined(__linux__)
    const auto updateSockOpt = [&](int level, int optnum, Seconds maxVal, StringData optname) {
        Seconds optVal{1};
        unsigned int rawOptVal = durationCount<Seconds>(optVal);
        socklen_t optValLen = sizeof(rawOptVal);

        if (getsockopt(sock, level, optnum, reinterpret_cast<char*>(&rawOptVal), &optValLen)) {
            auto ec = lastSystemError();
            LOGV2_DEBUG(23205,
                        logSeverity,
                        "can't get {optname}: {error}",
                        "Can't get socket option",
                        "optname"_attr = optname,
                        "error"_attr = errorMessage(ec));
        }

        if (optVal > maxVal) {
            unsigned int rawMaxVal = durationCount<Seconds>(maxVal);
            socklen_t maxValLen = sizeof(rawMaxVal);

            if (setsockopt(sock, level, optnum, reinterpret_cast<char*>(&rawMaxVal), maxValLen)) {
                auto ec = lastSystemError();
                LOGV2_DEBUG(23206,
                            logSeverity,
                            "can't set {optname}: {error}",
                            "Can't set socket option",
                            "optname"_attr = optname,
                            "error"_attr = errorMessage(ec));
            }
        }
    };

#ifdef __APPLE__
    updateSockOpt(IPPROTO_TCP, TCP_KEEPALIVE, maxKeepIdleSecs, "TCP_KEEPALIVE");
#endif

#ifdef __linux__
#ifdef SOL_TCP
    const int level = SOL_TCP;
#else
    const int level = SOL_SOCKET;
#endif
    updateSockOpt(level, TCP_KEEPIDLE, maxKeepIdleSecs, "TCP_KEEPIDLE");
    updateSockOpt(level, TCP_KEEPINTVL, maxKeepIntvlSecs, "TCP_KEEPINTVL");
#endif

#endif
}

std::string makeUnixSockPath(int port) {
    return str::stream() << serverGlobalParams.socket << "/mongodb-" << port << ".sock";
}

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
