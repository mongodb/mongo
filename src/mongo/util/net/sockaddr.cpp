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

#include <iterator>
#include <memory>
#include <set>
#include <utility>
#include <vector>

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
#include "mongo/logv2/log.h"
#include "mongo/util/itoa.h"

namespace mongo {
namespace {
constexpr int SOCK_FAMILY_UNKNOWN_ERROR = 13078;

struct AddrInfoDeleter {
    void operator()(addrinfo* p) const noexcept {
        freeaddrinfo(p);
    }
};
using AddrInfoPtr = std::unique_ptr<addrinfo, AddrInfoDeleter>;

struct AddrErr {
    AddrInfoPtr addr;
    int err;
};

AddrErr resolveAddrInfo(const std::string& hostOrIp, int port, sa_family_t familyHint) {
    const std::string portStr{StringData{ItoA(port)}};
    auto tryResolve = [&](bool allowDns) noexcept->AddrErr {
        addrinfo hints;
        memset(&hints, 0, sizeof(addrinfo));
        hints.ai_socktype = SOCK_STREAM;
        if (!allowDns)
            hints.ai_flags |= AI_NUMERICHOST;
        hints.ai_family = familyHint;
        addrinfo* addrs = nullptr;
        int ret = getaddrinfo(hostOrIp.c_str(), portStr.c_str(), &hints, &addrs);
        AddrInfoPtr rvPtr(addrs);
        return {std::move(rvPtr), ret};
    };

    switch (auto r = tryResolve(false); r.err) {
        case EAI_NONAME:
#ifdef EAI_NODATA
#if (EAI_NODATA != EAI_NONAME)  // In MSVC these have the same value.
        case EAI_NODATA:        // Old IPv6-capable hosts can return EAI_NODATA.
#endif
#endif
            return tryResolve(true);  // Not an IP address. Retry with DNS.
        default:
            return r;
    }
}

}  // namespace

std::string getAddrInfoStrError(int code) {
#if !defined(_WIN32)
    return gai_strerror(code);
#else
    /* gai_strerrorA is not threadsafe on windows. don't use it. */
    return errnoWithDescription(code);
#endif
}

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

void SockAddr::initUnixDomainSocket(const std::string& path, int port) {
#ifdef _WIN32
    uassert(13080, "no unix socket support on windows", false);
#endif
    uassert(
        13079, "path to unix socket too long", path.size() < sizeof(as<sockaddr_un>().sun_path));
    as<sockaddr_un>().sun_family = AF_UNIX;
    strcpy(as<sockaddr_un>().sun_path, path.c_str());
    addressSize = sizeof(sockaddr_un);
    _isValid = true;
}

SockAddr::SockAddr(StringData target, int port, sa_family_t familyHint)
    : _hostOrIp(target.toString()) {
    if (_hostOrIp == "localhost") {
        _hostOrIp = "127.0.0.1";
    }

    if (str::contains(_hostOrIp, '/') || familyHint == AF_UNIX) {
        initUnixDomainSocket(_hostOrIp, port);
        return;
    }

    auto addrErr = resolveAddrInfo(_hostOrIp, port, familyHint);

    if (addrErr.err) {
        // we were unsuccessful
        if (_hostOrIp != "0.0.0.0") {  // don't log if this as it is a
                                       // CRT construction and log() may not work yet.
            LOGV2(23175,
                  "getaddrinfo(\"{host}\") failed: {error}",
                  "Command getaddrinfo failed",
                  "host"_attr = _hostOrIp,
                  "error"_attr = getAddrInfoStrError(addrErr.err));
            _isValid = false;
            return;
        }
        *this = SockAddr(port);
        return;
    }

    // This throws away all but the first address.
    // Use SockAddr::createAll() to get all addresses.
    const auto* addrs = addrErr.addr.get();
    fassert(16501, static_cast<size_t>(addrs->ai_addrlen) <= sizeof(sa));
    memcpy(&sa, addrs->ai_addr, addrs->ai_addrlen);
    addressSize = addrs->ai_addrlen;
    _isValid = true;
}

std::vector<SockAddr> SockAddr::createAll(StringData target, int port, sa_family_t familyHint) {
    std::string hostOrIp = target.toString();
    if (str::contains(hostOrIp, '/')) {
        std::vector<SockAddr> ret = {SockAddr()};
        ret[0].initUnixDomainSocket(hostOrIp, port);
        // Currently, this is always valid since initUnixDomainSocket()
        // will uassert() on failure. Be defensive against future changes.
        return ret[0].isValid() ? ret : std::vector<SockAddr>();
    }

    auto addrErr = resolveAddrInfo(hostOrIp, port, familyHint);
    if (addrErr.err) {
        LOGV2(23176,
              "getaddrinfo(\"{host}\") failed: {error}",
              "getaddrinfo invocation failed",
              "host"_attr = hostOrIp,
              "error"_attr = getAddrInfoStrError(addrErr.err));
        return {};
    }

    std::set<SockAddr> ret;
    struct sockaddr_storage storage;
    memset(&storage, 0, sizeof(storage));
    for (const auto* addrs = addrErr.addr.get(); addrs; addrs = addrs->ai_next) {
        fassert(40594, static_cast<size_t>(addrs->ai_addrlen) <= sizeof(struct sockaddr_storage));
        ret.emplace(addrs->ai_addr, addrs->ai_addrlen);
    }
    return std::vector<SockAddr>(ret.begin(), ret.end());
}

SockAddr::SockAddr(const sockaddr* other, socklen_t size)
    : addressSize(size), _hostOrIp(), sa(), _isValid(true) {
    memcpy(&sa, other, size);
    _hostOrIp = toString(true);
}

bool SockAddr::isIP() const {
    return (getType() == AF_INET) || (getType() == AF_INET6);
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

bool SockAddr::isDefaultRoute() const {
    using std::begin;
    using std::end;
    switch (getType()) {
        case AF_INET:
            return as<sockaddr_in>().sin_addr.s_addr == 0;
        case AF_INET6: {
            const auto& addr6 = as<sockaddr_in6>().sin6_addr;
            return std::all_of(
                begin(addr6.s6_addr), end(addr6.s6_addr), [](const auto c) { return c == 0; });
        }
        default:
            return false;
    }
}

bool SockAddr::isAnonymousUNIXSocket() const {
    return ((getType() == AF_UNIX) && (as<sockaddr_un>().sun_path[0] == '\0'));
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
            int ret = getnameinfo(raw(), addressSize, buffer, buflen, nullptr, 0, NI_NUMERICHOST);
            massert(
                13082, str::stream() << "getnameinfo error " << getAddrInfoStrError(ret), ret == 0);
            return buffer;
        }

        case AF_UNIX:
            return (!isAnonymousUNIXSocket() ? as<sockaddr_un>().sun_path
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
    // Address family first
    if (getType() < r.getType()) {
        return true;
    }
    if (getType() > r.getType()) {
        return false;
    }

    // Address second
    int cmp;
    switch (getType()) {
        case AF_INET: {
            const auto laddr = ntohl(as<sockaddr_in>().sin_addr.s_addr);
            const auto raddr = ntohl(r.as<sockaddr_in>().sin_addr.s_addr);
            cmp = (laddr < raddr) ? -1 : (laddr > raddr) ? 1 : 0;
            break;
        }
        case AF_INET6:
            cmp = memcmp(as<sockaddr_in6>().sin6_addr.s6_addr,
                         r.as<sockaddr_in6>().sin6_addr.s6_addr,
                         sizeof(in6_addr));
            break;
        case AF_UNIX:
            cmp = strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path);
            break;
        default:
            massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
    }
    if (cmp < 0) {
        return true;
    }
    if (cmp > 0) {
        return false;
    }

    // All else equal, compare port
    return getPort() < r.getPort();
}

}  // namespace mongo
