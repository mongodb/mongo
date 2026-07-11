// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#endif

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * CIDR (Classless Inter-Domain Routing)
 */
class CIDR {
public:
    explicit CIDR(std::string_view);

    /**
     * If the given BSONElement represents a valid CIDR range,
     * constructs and returns the CIDR.
     * Otherwise returns an error.
     */
    static StatusWith<CIDR> parse(BSONElement from);

    /**
     * If the given string represents a valid CIDR range,
     * constructs and returns the CIDR.
     * Otherwise returns an error.
     */
    static StatusWith<CIDR> parse(std::string_view from);

    /**
     * Returns true if the provided address range is contained
     * entirely within this one, false otherwise.
     */
    bool contains(const CIDR& cidr) const {
        if ((_family != cidr._family) || (_len > cidr._len)) {
            return false;
        }

        auto bytes = _len / 8;
        auto const range = _ip.begin();
        auto const ip = cidr._ip.begin();
        if (!std::equal(range, range + bytes, ip, ip + bytes)) {
            return false;
        }

        if ((_len % 8) == 0) {
            return true;
        }

        auto mask = (0xFF << (8 - (_len % 8))) & 0xFF;
        return (_ip[bytes] & mask) == (cidr._ip[bytes] & mask);
    }

    friend bool operator==(const CIDR& lhs, const CIDR& rhs);
    friend bool operator!=(const CIDR& lhs, const CIDR& rhs) {
        return !(lhs == rhs);
    }
    friend std::ostream& operator<<(std::ostream& s, const CIDR& rhs);
    friend StringBuilder& operator<<(StringBuilder& s, const CIDR& rhs);

    /**
     * Return a string representation of this CIDR (i.e. "169.254.0.0/16")
     */
    std::string toString() const {
        StringBuilder s;
        s << *this;
        return s.str();
    }

    /**
     * Supports use of CIDR with the BSON macro:
     *     BSON("cidr" << cidr) -> { cidr: "..." }
     */
    friend void appendToBson(BSONObjBuilder& builder,
                             std::string_view fieldName,
                             const CIDR& value) {
        builder.append(fieldName, value.toString());
    }

    bool getLinkLocal() const {
        return _isLinkLocal;
    }

    std::string getScopeStr() const {
        return _scopeStr;
    }

private:
#ifdef _WIN32
    using sa_family_t = int;
#endif

    CIDR();

    typedef struct {
        std::string ip;
        std::string zone;
    } ipv6WithZone_t;

    auto _equalityLens() const {
        return std::tie(_ip, _family, _len);
    }

    ipv6WithZone_t _splitIPv6String(const std::string& ipv6);
    bool _ipv6LinkLocalAddress(const std::string& ipv6string);
    std::string _toLower(std::string s);
    std::vector<std::string> _splitString(const std::string& s, char delimiter);

    std::array<std::uint8_t, 16> _ip;
    sa_family_t _family;
    std::uint8_t _len;
    bool _isLinkLocal{false};
    std::string _scopeStr;
};

inline bool operator==(const CIDR& lhs, const CIDR& rhs) {
    return lhs._equalityLens() == rhs._equalityLens();
}

std::ostream& operator<<(std::ostream& s, const CIDR& cidr);
StringBuilder& operator<<(StringBuilder& s, const CIDR& cidr);


/**
 * A list of CIDR or strings. When the value is a string, it is assumed to be a unix path.
 *
 * The unix path string value is used to exempt anonymous unix socket.
 */
using CIDRList = std::vector<std::variant<CIDR, std::string>>;

}  // namespace mongo
