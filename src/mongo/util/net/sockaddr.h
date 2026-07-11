// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32

#include <cerrno>  // IWYU pragma: export

#include <sys/socket.h>  // IWYU pragma: export
#include <sys/types.h>   // IWYU pragma: export
#include <sys/un.h>      // IWYU pragma: export

#ifdef __OpenBSD__
#include <sys/uio.h>
#endif  // __OpenBSD__

#endif  // not _WIN32


namespace [[MONGO_MOD_PUBLIC]] mongo {
class BSONObjBuilder;

#if defined(_WIN32)

typedef short sa_family_t;
typedef int socklen_t;

// This won't actually be used on windows
struct sockaddr_un {
    short sun_family;
    char sun_path[108];  // length from unix header
};

#endif  // _WIN32

/**
 * Wrapper around os representation of network address.
 */
struct SockAddr {
    SockAddr();

    explicit SockAddr(int sourcePort); /* listener side */
    explicit SockAddr(const sockaddr* other, socklen_t size);
    explicit SockAddr(const sockaddr* other, socklen_t size, std::string_view hostOrIp);


    /**
     * Initialize a SockAddr for a given IP or Hostname.
     *
     * If target fails to resolve/parse, this function may throw or the resulting SockAddr may be
     * equivalent to SockAddr(port).
     *
     * If target is a unix domain socket, a uassert() exception will be thrown on windows or if addr
     * exceeds maximum path length.
     *
     * If target resolves to more than one address, only the first address will be used. Others will
     * be discarded. SockAddr::createAll() is recommended for capturing all addresses.
     */
    static SockAddr create(std::string_view target, int port, sa_family_t familyHint);

    /**
     * Resolve an ip or hostname to a vector of SockAddr objects.
     *
     * Works similar to SockAddr(std::string_view, int, sa_family_t) above,
     * however all addresses returned from ::getaddrinfo() are used,
     * it never falls-open to SockAddr(port),
     * and isInvalid() SockAddrs are excluded.
     *
     * May return an empty vector.
     */
    static std::vector<SockAddr> createAll(std::string_view target,
                                           int port,
                                           sa_family_t familyHint);

    template <typename T>
    T& as() {
        return *(T*)(&sa);
    }
    template <typename T>
    const T& as() const {
        return *(const T*)(&sa);
    }

    std::string hostOrIp() const {
        return _hostOrIp;
    }

    std::string toString(bool includePort = true) const;

    bool isValid() const {
        return _isValid;
    }

    bool isIP() const;

    /**
     * @return one of AF_INET, AF_INET6, or AF_UNIX
     */
    sa_family_t getType() const;

    unsigned getPort() const;
    void setPort(int port);

    std::string getAddr() const;

    bool isLocalHost() const;
    bool isDefaultRoute() const;
    bool isAnonymousUNIXSocket() const;

    bool operator==(const SockAddr& r) const;
    bool operator!=(const SockAddr& r) const;
    bool operator<(const SockAddr& r) const;

    const sockaddr* raw() const {
        return (sockaddr*)&sa;
    }
    sockaddr* raw() {
        return (sockaddr*)&sa;
    }

    socklen_t addressSize;

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const;

private:
    void initUnixDomainSocket(std::string_view path, int port);

    std::string _hostOrIp;
    struct sockaddr_storage sa;
    bool _isValid = false;
};

}  // namespace mongo
