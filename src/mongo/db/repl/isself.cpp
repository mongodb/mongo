/**
*    Copyright (C) 2014 MongoDB Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/db/repl/isself.h"

#include <boost/algorithm/string.hpp>

#include "mongo/base/init.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/net/listen.h"
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

namespace mongo {
namespace repl {

OID instanceId;

MONGO_INITIALIZER(GenerateInstanceId)(InitializerContext*) {
    instanceId = OID::gen();
    return Status::OK();
}

namespace {

/**
 * Helper to convert a message from a networking function to a string.
 * Needed because errnoWithDescription uses strerror on linux, when
 * we need gai_strerror.
 */
std::string stringifyError(int code) {
#if FASTPATH_UNIX
    return gai_strerror(code);
#elif defined(_WIN32)
    // FormatMessage in errnoWithDescription works here on windows
    return errnoWithDescription(code);
#endif
}

/**
 * Resolves a host and port to a list of IP addresses. This requires a syscall. If the
 * ipv6enabled parameter is true, both IPv6 and IPv4 addresses will be returned.
 */
std::vector<std::string> getAddrsForHost(const std::string& iporhost,
                                         const int port,
                                         const bool ipv6enabled) {
    addrinfo* addrs = NULL;
    addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = (ipv6enabled ? AF_UNSPEC : AF_INET);

    const std::string portNum = BSONObjBuilder::numStr(port);

    std::vector<std::string> out;

    int err = getaddrinfo(iporhost.c_str(), portNum.c_str(), &hints, &addrs);

    if (err) {
        warning() << "getaddrinfo(\"" << iporhost << "\") failed: " << stringifyError(err)
                  << std::endl;
        return out;
    }

    ON_BLOCK_EXIT(freeaddrinfo, addrs);

    for (addrinfo* addr = addrs; addr != NULL; addr = addr->ai_next) {
        int family = addr->ai_family;
        char host[NI_MAXHOST];

        if (family == AF_INET || family == AF_INET6) {
            err = getnameinfo(
                addr->ai_addr, addr->ai_addrlen, host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (err) {
                warning() << "getnameinfo() failed: " << stringifyError(err) << std::endl;
                continue;
            }
            out.push_back(host);
        }
    }

    if (shouldLog(logger::LogSeverity::Debug(2))) {
        StringBuilder builder;
        builder << "getAddrsForHost(\"" << iporhost << ":" << port << "\"):";
        for (std::vector<std::string>::const_iterator o = out.begin(); o != out.end(); ++o) {
            builder << " [ " << *o << "]";
        }
        LOG(2) << builder.str();
    }

    return out;
}

}  // namespace

bool isSelf(const HostAndPort& hostAndPort, ServiceContext* const ctx) {
    // Fastpath: check if the host&port in question is bound to one
    // of the interfaces on this machine.
    // No need for ip match if the ports do not match
    if (hostAndPort.port() == serverGlobalParams.port) {
        std::vector<std::string> myAddrs = serverGlobalParams.bind_ip.empty()
            ? getBoundAddrs(IPv6Enabled())
            : std::vector<std::string>();

        if (!serverGlobalParams.bind_ip.empty()) {
            boost::split(myAddrs, serverGlobalParams.bind_ip, boost::is_any_of(", "));
        }

        const std::vector<std::string> hostAddrs =
            getAddrsForHost(hostAndPort.host(), hostAndPort.port(), IPv6Enabled());

        for (std::vector<std::string>::const_iterator i = myAddrs.begin(); i != myAddrs.end();
             ++i) {
            for (std::vector<std::string>::const_iterator j = hostAddrs.begin();
                 j != hostAddrs.end();
                 ++j) {
                if (*i == *j) {
                    return true;
                }
            }
        }
    }

    const auto listener = Listener::get(ctx);
    if (!listener) {
        return false;
    }
    // Ensure that the server is up and ready to accept incoming network requests.
    listener->waitUntilListening();

    try {
        DBClientConnection conn;
        conn.setSoTimeout(30);  // 30 second timeout

        // We need to avoid the isMaster call triggered by a normal connect, which would
        // cause a deadlock. 'isSelf' is called by the Replication Coordinator when validating
        // a replica set configuration document, but the 'isMaster' command requires a lock on the
        // replication coordinator to execute. As such we call we call 'connectSocketOnly', which
        // does not call 'isMaster'.
        if (!conn.connectSocketOnly(hostAndPort).isOK()) {
            return false;
        }

        if (isInternalAuthSet() && !conn.authenticateInternalUser()) {
            return false;
        }
        BSONObj out;
        bool ok = conn.simpleCommand("admin", &out, "_isSelf");
        bool me = ok && out["id"].type() == jstOID && instanceId == out["id"].OID();

        return me;
    } catch (const std::exception& e) {
        warning() << "couldn't check isSelf (" << hostAndPort << ") " << e.what() << std::endl;
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

    int err = getifaddrs(&addrs);
    if (err) {
        warning() << "getifaddrs failure: " << errnoWithDescription(err) << std::endl;
        return out;
    }
    ON_BLOCK_EXIT(freeifaddrs, addrs);

    // based on example code from linux getifaddrs manpage
    for (ifaddrs* addr = addrs; addr != NULL; addr = addr->ifa_next) {
        if (addr->ifa_addr == NULL)
            continue;
        int family = addr->ifa_addr->sa_family;
        char host[NI_MAXHOST];

        if (family == AF_INET || (ipv6enabled && (family == AF_INET6))) {
            err = getnameinfo(
                addr->ifa_addr,
                (family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)),
                host,
                NI_MAXHOST,
                NULL,
                0,
                NI_NUMERICHOST);
            if (err) {
                warning() << "getnameinfo() failed: " << gai_strerror(err) << std::endl;
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
                                       GAA_FLAG_SKIP_MULTICAST |
                                       GAA_FLAG_SKIP_DNS_SERVER,
                                   NULL,
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
        warning() << "GetAdaptersAddresses() failed: " << errnoWithDescription(err) << std::endl;
        return out;
    }

    for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != NULL; adapter = adapter->Next) {
        for (IP_ADAPTER_UNICAST_ADDRESS* addr = adapter->FirstUnicastAddress; addr != NULL;
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
                    warning() << "inet_ntop failed during IPv4 address conversion: " << ec.message()
                              << std::endl;
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
                    warning() << "inet_ntop failed during IPv6 address conversion: " << ec.message()
                              << std::endl;
                    continue;
                }
                out.push_back(addrstr);
            }
        }
    }

#endif  // defined(_WIN32)

    if (shouldLog(logger::LogSeverity::Debug(2))) {
        StringBuilder builder;
        builder << "getBoundAddrs():";
        for (std::vector<std::string>::const_iterator o = out.begin(); o != out.end(); ++o) {
            builder << " [ " << *o << "]";
        }
        LOG(2) << builder.str();
    }
    return out;
}

}  // namespace repl
}  // namespace mongo
