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

#include "mongo/db/repl/isself.h"

#include <boost/algorithm/string.hpp>

#include "mongo/base/init.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/scopeguard.h"

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__sun) || \
    defined(__OpenBSD__)
#define FASTPATH_UNIX 1
#endif

#if !defined(_WIN32) && !defined(FASTPATH_UNIX)
#error isself needs to be implemented for this platform
#endif


#ifdef FASTPATH_UNIX
#include <ifaddrs.h>
#include <netdb.h>

#ifdef __FreeBSD__
#include <netinet/in.h>
#endif

#elif defined(_WIN32)
#include <Ws2tcpip.h>
#include <boost/asio/detail/socket_ops.hpp>
#include <boost/system/error_code.hpp>
#include <iphlpapi.h>
#include <winsock2.h>
#endif  // defined(_WIN32)

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(failIsSelfCheck);
MONGO_FAIL_POINT_DEFINE(transientDNSErrorInFastPath);

OID instanceId;

MONGO_INITIALIZER(GenerateInstanceId)(InitializerContext*) {
    instanceId = OID::gen();
}

namespace {

/**
 * Resolves a host and port to a list of IP addresses. This requires a syscall. If the
 * ipv6enabled parameter is true, both IPv6 and IPv4 addresses will be returned.
 */
std::vector<std::string> getAddrsForHost(const std::string& iporhost,
                                         const int port,
                                         const bool ipv6enabled) {
    addrinfo* addrs = nullptr;
    addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = (ipv6enabled ? AF_UNSPEC : AF_INET);

    const std::string portNum = std::to_string(port);

    int err = 0;
    int attempts = 0;
    int maxAttempts = 4;

    while (true) {
        err = getaddrinfo(iporhost.c_str(), portNum.c_str(), &hints, &addrs);

        // We do not sleep for as long in tests.
        auto waitCoefficient = 1.0;

        // Simulate transient DNS error if set.
        if (MONGO_unlikely(transientDNSErrorInFastPath.shouldFail())) {
            waitCoefficient = 0.1;
            err = EAI_AGAIN;
        }

        // Skip waiting if we succeed, get a different error, or run out of attempts.
        if (err != EAI_AGAIN || attempts == maxAttempts) {
            break;
        }

        // Free what we have ahead of the next getaddrinfo call.
        freeaddrinfo(addrs);

        // Wait 1, 2, 4, 8 seconds (and a tenth of that in tests).
        sleepmillis(std::pow(2, attempts++) * 1000 * waitCoefficient);
    }

    std::vector<std::string> out;

    if (err) {
        auto ec = addrInfoError(err);
        LOGV2_WARNING(21207,
                      "getaddrinfo(\"{host}\") failed: {error}",
                      "getaddrinfo() failed",
                      "host"_attr = iporhost,
                      "error"_attr = errorMessage(ec),
                      "timedOut"_attr = (attempts == maxAttempts));
        freeaddrinfo(addrs);
        return out;
    }

    ON_BLOCK_EXIT([&] { freeaddrinfo(addrs); });

    for (addrinfo* addr = addrs; addr != nullptr; addr = addr->ai_next) {
        int family = addr->ai_family;
        char host[NI_MAXHOST];

        if (family == AF_INET || family == AF_INET6) {
            err = getnameinfo(
                addr->ai_addr, addr->ai_addrlen, host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);
            if (err) {
                auto ec = addrInfoError(err);
                LOGV2_WARNING(21208,
                              "getnameinfo() failed: {error}",
                              "getnameinfo() failed",
                              "error"_attr = errorMessage(ec));
                continue;
            }
            out.push_back(host);
        }
    }

    if (shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(2))) {
        LOGV2_DEBUG(21205,
                    2,
                    "getAddrsForHost()",
                    "host"_attr = iporhost,
                    "port"_attr = port,
                    "result"_attr = out);
    }

    return out;
}

}  // namespace

bool isSelf(const HostAndPort& hostAndPort, ServiceContext* const ctx, Milliseconds timeout) {
    if (isSelfFastPath(hostAndPort)) {
        return true;
    }
    return isSelfSlowPath(hostAndPort, ctx, timeout);
}

bool isSelfFastPath(const HostAndPort& hostAndPort) {
    if (MONGO_unlikely(failIsSelfCheck.shouldFail())) {
        LOGV2(356490,
              "failIsSelfCheck failpoint activated, returning false from isSelfFastPath",
              "hostAndPort"_attr = hostAndPort);
        return false;
    }

    // Fastpath: check if the host&port in question is bound to one
    // of the interfaces on this machine.
    // No need for ip match if the ports do not match
    if (hostAndPort.port() == serverGlobalParams.port) {
        std::vector<std::string> myAddrs = serverGlobalParams.bind_ips;

        // If any of the bound addresses is the default route (0.0.0.0 on IPv4) it means we are
        // listening on all network interfaces and need to check against any of them.
        auto defaultRoute = false;
        if (myAddrs.empty() ||
            std::any_of(myAddrs.cbegin(), myAddrs.cend(), [](std::string const& addrStr) {
                return HostAndPort(addrStr, serverGlobalParams.port).isDefaultRoute();
            })) {
            myAddrs = getBoundAddrs(IPv6Enabled());
            defaultRoute = true;
        }

        const std::vector<std::string> hostAddrs =
            getAddrsForHost(hostAndPort.host(), hostAndPort.port(), IPv6Enabled());

        for (std::vector<std::string>::const_iterator i = myAddrs.begin(); i != myAddrs.end();
             ++i) {
            for (std::vector<std::string>::const_iterator j = hostAddrs.begin();
                 j != hostAddrs.end();
                 ++j) {

                // If we are listening on the default route and the host address is in the range of
                // addresses that correspond to the loopback interface, then we consider the host as
                // ourself.
                //
                // Note that this logic is included to account for the fact that Debian systems add
                // the "127.0.1.1" address for the local host name (as opposed to "127.0.0.1").
                // Debian does not do this for IPv6 so we don't need to check that here.
                try {
                    CIDR loopbackAddrs("127.0.0.1/8");
                    if (defaultRoute && loopbackAddrs.contains(CIDR(*j))) {
                        return true;
                    }
                } catch (const std::exception& e) {
                    LOGV2_WARNING(4754500,
                                  "Error checking host against loopback addresses",
                                  "host"_attr = *j,
                                  "error"_attr = e.what());
                }

                if (*i == *j) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool isSelfSlowPath(const HostAndPort& hostAndPort,
                    ServiceContext* const ctx,
                    Milliseconds timeout) {
    ctx->waitForStartupComplete();
    if (MONGO_unlikely(failIsSelfCheck.shouldFail())) {
        LOGV2(6605000,
              "failIsSelfCheck failpoint activated, returning false from isSelfSlowPath",
              "hostAndPort"_attr = hostAndPort);
        return false;
    }

    try {
        DBClientConnection conn;
        double timeoutSeconds = static_cast<double>(durationCount<Milliseconds>(timeout)) / 1000.0;
        conn.setSoTimeout(timeoutSeconds);

        // We need to avoid the isMaster call triggered by a normal connect, which would
        // cause a deadlock. 'isSelf' is called by the Replication Coordinator when validating
        // a replica set configuration document, but the 'isMaster' command requires a lock on the
        // replication coordinator to execute. As such we call we call 'connectSocketOnly', which
        // does not call 'isMaster'.
        auto connectSocketResult = conn.connectSocketOnly(hostAndPort, boost::none);
        if (!connectSocketResult.isOK()) {
            LOGV2(4834700,
                  "isSelf could not connect via connectSocketOnly",
                  "hostAndPort"_attr = hostAndPort,
                  "error"_attr = connectSocketResult);
            return false;
        }

        if (auth::isInternalAuthSet()) {
            auto authInternalUserResult =
                conn.authenticateInternalUser(auth::StepDownBehavior::kKeepConnectionOpen);
            if (!authInternalUserResult.isOK()) {
                LOGV2(4834701,
                      "isSelf could not authenticate internal user",
                      "hostAndPort"_attr = hostAndPort,
                      "error"_attr = authInternalUserResult);
                return false;
            }
        }
        BSONObj out;
        bool ok = conn.runCommand(DatabaseName::kAdmin, BSON("_isSelf" << 1), out);
        bool me = ok && out["id"].type() == jstOID && instanceId == out["id"].OID();

        return me;
    } catch (const std::exception& e) {
        LOGV2_WARNING(21209,
                      "couldn't check isSelf ({hostAndPort}) {error}",
                      "Couldn't check isSelf",
                      "hostAndPort"_attr = hostAndPort,
                      "error"_attr = e.what());
    }

    return false;
}

/**
 * Returns all the IP addresses bound to the network interfaces of this machine.
 * This requires a syscall. If the ipv6enabled parameter is true, both IPv6 AND IPv4
 * addresses will be returned.
 */
std::vector<std::string> getBoundAddrs(const bool ipv6enabled) {
    std::vector<std::string> out;
#ifdef FASTPATH_UNIX

    ifaddrs* addrs;

    if (getifaddrs(&addrs)) {
        auto ec = lastSystemError();
        LOGV2_WARNING(21210,
                      "getifaddrs failure: {error}",
                      "getifaddrs() failed",
                      "error"_attr = errorMessage(ec));
        return out;
    }
    ON_BLOCK_EXIT([&] { freeifaddrs(addrs); });

    // based on example code from linux getifaddrs manpage
    for (ifaddrs* addr = addrs; addr != nullptr; addr = addr->ifa_next) {
        if (addr->ifa_addr == nullptr)
            continue;
        int family = addr->ifa_addr->sa_family;
        char host[NI_MAXHOST];

        if (family == AF_INET || (ipv6enabled && (family == AF_INET6))) {
            int err = getnameinfo(
                addr->ifa_addr,
                (family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)),
                host,
                NI_MAXHOST,
                nullptr,
                0,
                NI_NUMERICHOST);
            if (err) {
                LOGV2_WARNING(21211,
                              "getnameinfo() failed: {error}",
                              "getnameinfo() failed",
                              "error"_attr = errorMessage(addrInfoError(err)));
                continue;
            }
            out.push_back(host);
        }
    }

#elif defined(_WIN32)

    // Start with the MS recommended 15KB buffer. Use multiple attempts
    // for the rare case that the adapter config changes between calls

    ULONG adaptersLen = 15 * 1024;
    std::unique_ptr<char[]> buf(new char[adaptersLen]);
    IP_ADAPTER_ADDRESSES* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get());
    DWORD err;

    ULONG family = ipv6enabled ? AF_UNSPEC : AF_INET;

    for (int tries = 0; tries < 3; ++tries) {
        err = GetAdaptersAddresses(family,
                                   GAA_FLAG_SKIP_ANYCAST |  // only want unicast addrs
                                       GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                   nullptr,
                                   adapters,
                                   &adaptersLen);

        if (err == ERROR_BUFFER_OVERFLOW) {
            // in this case, adaptersLen will be set to the size we need to allocate
            buf.reset(new char[adaptersLen]);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get());
        } else {
            break;  // only retry for incorrectly sized buffer
        }
    }

    if (err != NO_ERROR) {
        LOGV2_WARNING(21212,
                      "GetAdaptersAddresses() failed: {error}",
                      "GetAdaptersAddresses() failed",
                      "error"_attr = errorMessage(systemError(err)));
        return out;
    }

    for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        for (IP_ADAPTER_UNICAST_ADDRESS* addr = adapter->FirstUnicastAddress; addr != nullptr;
             addr = addr->Next) {
            short family = reinterpret_cast<SOCKADDR_STORAGE*>(addr->Address.lpSockaddr)->ss_family;

            if (family == AF_INET) {
                // IPv4
                SOCKADDR_IN* sock = reinterpret_cast<SOCKADDR_IN*>(addr->Address.lpSockaddr);
                char addrstr[INET_ADDRSTRLEN] = {0};
                boost::system::error_code ec;
                // Not all windows versions have inet_ntop
                boost::asio::detail::socket_ops::inet_ntop(
                    AF_INET, &(sock->sin_addr), addrstr, INET_ADDRSTRLEN, 0, ec);
                if (ec) {
                    LOGV2_WARNING(21213,
                                  "inet_ntop failed during IPv4 address conversion: {error}",
                                  "inet_ntop failed during IPv4 address conversion",
                                  "error"_attr = ec.message());
                    continue;
                }
                out.push_back(addrstr);
            } else if (family == AF_INET6) {
                // IPv6
                SOCKADDR_IN6* sock = reinterpret_cast<SOCKADDR_IN6*>(addr->Address.lpSockaddr);
                char addrstr[INET6_ADDRSTRLEN] = {0};
                boost::system::error_code ec;
                boost::asio::detail::socket_ops::inet_ntop(
                    AF_INET6, &(sock->sin6_addr), addrstr, INET6_ADDRSTRLEN, 0, ec);
                if (ec) {
                    LOGV2_WARNING(21214,
                                  "inet_ntop failed during IPv6 address conversion: {error}",
                                  "inet_ntop failed during IPv6 address conversion",
                                  "error"_attr = ec.message());
                    continue;
                }
                out.push_back(addrstr);
            }
        }
    }

#endif  // defined(_WIN32)

    if (shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(2))) {
        LOGV2_DEBUG(21206, 2, "getBoundAddrs()", "result"_attr = out);
    }
    return out;
}

}  // namespace repl
}  // namespace mongo
