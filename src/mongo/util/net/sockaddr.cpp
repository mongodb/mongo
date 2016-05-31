/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/util/net/sockaddr.h"

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

#include "mongo/bson/util/builder.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"

namespace mongo {

SockAddr::SockAddr() {
    addressSize = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    sa.ss_family = AF_UNSPEC;
    _isValid = true;
}

SockAddr::SockAddr(int sourcePort) {
    memset(as<sockaddr_in>().sin_zero, 0, sizeof(as<sockaddr_in>().sin_zero));
    as<sockaddr_in>().sin_family = AF_INET;
    as<sockaddr_in>().sin_port = htons(sourcePort);
    as<sockaddr_in>().sin_addr.s_addr = htonl(INADDR_ANY);
    addressSize = sizeof(sockaddr_in);
    _isValid = true;
}

SockAddr::SockAddr(const char* _iporhost, int port) {
    std::string target = _iporhost;
    if (target == "localhost") {
        target = "127.0.0.1";
    }

    if (mongoutils::str::contains(target, '/')) {
#ifdef _WIN32
        uassert(13080, "no unix socket support on windows", false);
#endif
        uassert(13079,
                "path to unix socket too long",
                target.size() < sizeof(as<sockaddr_un>().sun_path));
        as<sockaddr_un>().sun_family = AF_UNIX;
        strcpy(as<sockaddr_un>().sun_path, target.c_str());
        addressSize = sizeof(sockaddr_un);
        _isValid = true;
        return;
    }

    addrinfo* addrs = NULL;
    addrinfo hints;
    memset(&hints, 0, sizeof(addrinfo));
    hints.ai_socktype = SOCK_STREAM;
    // hints.ai_flags = AI_ADDRCONFIG; // This is often recommended but don't do it.
    // SERVER-1579
    hints.ai_flags |= AI_NUMERICHOST;  // first pass tries w/o DNS lookup
    hints.ai_family = (IPv6Enabled() ? AF_UNSPEC : AF_INET);

    StringBuilder ss;
    ss << port;
    int ret = getaddrinfo(target.c_str(), ss.str().c_str(), &hints, &addrs);

// old C compilers on IPv6-capable hosts return EAI_NODATA error
#ifdef EAI_NODATA
    int nodata = (ret == EAI_NODATA);
#else
    int nodata = false;
#endif
    if ((ret == EAI_NONAME || nodata)) {
        // iporhost isn't an IP address, allow DNS lookup
        hints.ai_flags &= ~AI_NUMERICHOST;
        ret = getaddrinfo(target.c_str(), ss.str().c_str(), &hints, &addrs);
    }

    if (ret) {
        // we were unsuccessful
        if (target != "0.0.0.0") {  // don't log if this as it is a
                                    // CRT construction and log() may not work yet.
            log() << "getaddrinfo(\"" << target << "\") failed: " << getAddrInfoStrError(ret)
                  << std::endl;
            _isValid = false;
            return;
        }
        *this = SockAddr(port);
        return;
    }

    // TODO: handle other addresses in linked list;
    fassert(16501, addrs->ai_addrlen <= sizeof(sa));
    memcpy(&sa, addrs->ai_addr, addrs->ai_addrlen);
    addressSize = addrs->ai_addrlen;
    freeaddrinfo(addrs);
    _isValid = true;
}

bool SockAddr::isLocalHost() const {
    switch (getType()) {
        case AF_INET:
            return getAddr() == "127.0.0.1";
        case AF_INET6:
            return getAddr() == "::1";
        case AF_UNIX:
            return true;
        default:
            return false;
    }
    fassert(16502, false);
    return false;
}

std::string SockAddr::toString(bool includePort) const {
    if (includePort && (getType() != AF_UNIX) && (getType() != AF_UNSPEC)) {
        StringBuilder ss;

        if (getType() == AF_INET6) {
            ss << '[' << getAddr() << "]:" << getPort();
        } else {
            ss << getAddr() << ':' << getPort();
        }

        return ss.str();
    } else {
        return getAddr();
    }
}

sa_family_t SockAddr::getType() const {
    return sa.ss_family;
}

unsigned SockAddr::getPort() const {
    switch (getType()) {
        case AF_INET:
            return ntohs(as<sockaddr_in>().sin_port);
        case AF_INET6:
            return ntohs(as<sockaddr_in6>().sin6_port);
        case AF_UNIX:
            return 0;
        case AF_UNSPEC:
            return 0;
        default:
            massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
            return 0;
    }
}

std::string SockAddr::getAddr() const {
    switch (getType()) {
        case AF_INET:
        case AF_INET6: {
            const int buflen = 128;
            char buffer[buflen];
            int ret = getnameinfo(raw(), addressSize, buffer, buflen, NULL, 0, NI_NUMERICHOST);
            massert(13082,
                    mongoutils::str::stream() << "getnameinfo error " << getAddrInfoStrError(ret),
                    ret == 0);
            return buffer;
        }

        case AF_UNIX:
            return (as<sockaddr_un>().sun_path[0] != '\0' ? as<sockaddr_un>().sun_path
                                                          : "anonymous unix socket");
        case AF_UNSPEC:
            return "(NONE)";
        default:
            massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
            return "";
    }
}

bool SockAddr::operator==(const SockAddr& r) const {
    if (getType() != r.getType())
        return false;

    if (getPort() != r.getPort())
        return false;

    switch (getType()) {
        case AF_INET:
            return as<sockaddr_in>().sin_addr.s_addr == r.as<sockaddr_in>().sin_addr.s_addr;
        case AF_INET6:
            return memcmp(as<sockaddr_in6>().sin6_addr.s6_addr,
                          r.as<sockaddr_in6>().sin6_addr.s6_addr,
                          sizeof(in6_addr)) == 0;
        case AF_UNIX:
            return strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path) == 0;
        case AF_UNSPEC:
            return true;  // assume all unspecified addresses are the same
        default:
            massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
    }
    return false;
}

bool SockAddr::operator!=(const SockAddr& r) const {
    return !(*this == r);
}

bool SockAddr::operator<(const SockAddr& r) const {
    if (getType() < r.getType())
        return true;
    else if (getType() > r.getType())
        return false;

    if (getPort() < r.getPort())
        return true;
    else if (getPort() > r.getPort())
        return false;

    switch (getType()) {
        case AF_INET:
            return as<sockaddr_in>().sin_addr.s_addr < r.as<sockaddr_in>().sin_addr.s_addr;
        case AF_INET6:
            return memcmp(as<sockaddr_in6>().sin6_addr.s6_addr,
                          r.as<sockaddr_in6>().sin6_addr.s6_addr,
                          sizeof(in6_addr)) < 0;
        case AF_UNIX:
            return strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path) < 0;
        case AF_UNSPEC:
            return false;
        default:
            massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
    }
    return false;
}

}  // namespace mongo
