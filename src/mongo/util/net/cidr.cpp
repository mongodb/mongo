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

#include "mongo/util/net/cidr.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/ctype.h"

#ifdef _WIN32
#include <Ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

using std::begin;
using std::end;
using std::find;

namespace mongo {

namespace {

constexpr std::uint8_t kIPv4Bits = 32;
constexpr std::uint8_t kIPv6Bits = 128;

#ifdef _WIN32
/**
 * Windows doesn't declare the second arg as const, but it is.
 * Also, it should blindly downcast to void*, but it doesn't.
 */
const char* inet_ntop(INT af, const std::uint8_t* addr, PSTR buf, std::size_t bufsz) {
    return ::inet_ntop(af, const_cast<void*>(reinterpret_cast<const void*>(addr)), buf, bufsz);
}
#endif

/**
 * `std::stoi()` naturally throws `std::invalid_argument` if str
 * isn't numeric at all or `std::out_of_range` if str won't fit in an int.
 *
 * Extend that to include a check that the string is entirely numeric and throw
 * `std::invalid_argument` for that as well
 */
int strict_stoi(const std::string& str, int base = 10) {
    std::size_t pos;
    auto len = std::stoi(str, &pos, base);
    if (pos != str.size()) {
        throw std::invalid_argument("Invalid characters encountered parsing: " + str + " at " +
                                    str.substr(pos));
    }
    return len;
}

template <class T>
T& append(T& s, int family, const std::array<uint8_t, 16> ip, int len) {
    char buffer[INET6_ADDRSTRLEN + 1] = {};
    if (inet_ntop(family, ip.data(), buffer, sizeof(buffer) - 1)) {
        s << buffer << '/' << (int)len;
    }
    return s;
}

}  // namespace

StatusWith<CIDR> CIDR::parse(BSONElement from) {
    if (from.type() != BSONType::string) {
        return {ErrorCodes::UnsupportedFormat, "CIDR range must be a string"};
    }
    return parse(from.valueStringData());
}

StatusWith<CIDR> CIDR::parse(StringData s) {
    CIDR value;
    auto slash = find(begin(s), end(s), '/');
    auto ip = (slash == end(s)) ? std::string{s} : std::string{s.substr(0, slash - begin(s))};

    if (inet_pton(AF_INET, ip.c_str(), value._ip.data())) {
        value._family = AF_INET;
        value._len = kIPv4Bits;
    } else {
        // If this is a IPv6 link-local address, it may have a zone id embedded in the address. The
        // inet_pton() function does not handle that. We need to strip off the zone id before
        // calling inet_pton(). The splitIPv6String() function does that by returning the IPv6
        // address without the zone id and the zone id in a separate field.
        CIDR::ipv6WithZone_t ipv6cidr = value._splitIPv6String(ip.c_str());
        // We expect to get back either one or two elements:
        // - If we get back one element, then this is the IPv6 address with an optional mask
        // - If we get back two elements, then the first element is the IPv6 address without the
        // zone id, and the second element is the zone id
        if (ipv6cidr.ip.empty()) {
            return Status(ErrorCodes::UnsupportedFormat, "Invalid IP address format");
        }

        if (inet_pton(AF_INET6, ipv6cidr.ip.c_str(), value._ip.data())) {
            value._family = AF_INET6;
            value._len = kIPv6Bits;
            if (ipv6cidr.zone.size() > 0) {
                // We have a zone id. Save it
                value._isLinkLocal = true;
                value._scopeStr = ipv6cidr.zone;
            }
        } else {
            return Status(ErrorCodes::UnsupportedFormat, "Invalid IP address in CIDR string");
        }
    }

    if (slash == end(s)) {
        return value;
    }

    try {
        auto len = strict_stoi(std::string(slash + 1, end(s)), 10);

        if (len < 0 || len > value._len) {
            return Status(ErrorCodes::UnsupportedFormat, "Invalid length in CIDR string");
        }
        value._len = len;
    } catch (const std::invalid_argument&) {
        return Status(ErrorCodes::UnsupportedFormat, "Non-numeric length in CIDR string");
    } catch (const std::out_of_range&) {
        return Status(ErrorCodes::UnsupportedFormat, "Invalid length in CIDR string");
    }
    return value;
}

std::vector<std::string> CIDR::_splitString(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::stringstream ss(s);  // Create a stringstream from the input string

    // Read tokens from the stringstream, separated by the delimiter
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string CIDR::_toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](auto c) { return mongo::ctype::toLower(c); });
    return s;
}

bool CIDR::_ipv6LinkLocalAddress(const std::string& ipv6string) {
    if (ipv6string.size() < 4) {
        return false;
    }
    std::string prefix = ipv6string.substr(0, 4);
    return _toLower(prefix) == "fe80";
}


// We may get an IPv6 string like this:
// Not link-local 1234:5678:9abc:def0:1234:5678:9abc:def0/123";
// Link-local without zone fe80:5678:9abc:def0:1234:5678:9abc:def0/123";
// Link-local with zone: fe80:5678:9abc:def0:1234:5678:9abc:def0%ens5/123";
//
CIDR::ipv6WithZone_t CIDR::_splitIPv6String(const std::string& ipv6) {
    CIDR::ipv6WithZone_t result;
    if (ipv6.empty()) {
        return result;
    }
    // If the IPv6 address does not start with "fe80" (case-insensitive), it is not a
    // link-local address. Return the original string
    if (!_ipv6LinkLocalAddress(ipv6)) {
        result.ip = ipv6;
        return result;
    }
    // If this is a IPv6 link-local address, it should be one of the following formats:
    // - Without the zone identifier, for example fe80:5678:9abc:def0:1234:5678:9abc:def0/123";
    // - With the zone identifier, for example
    // fe80:5678:9abc:def0:1234:5678:9abc:def0%ens5/123";
    //
    // In general, the format is:
    // fe80:xxx[%zoneid][/mask]
    //
    // First, we extract the IPv6 address with the optional zone
    std::vector<std::string> workingIPv6withoutMask = _splitString(ipv6, '/');

    // We expect to get back at least one element. If we don't, this is probably an invalid
    // format, but we leave the caller to validate the IPv6 address
    if (workingIPv6withoutMask.empty()) {
        return result;
    }

    // At this stage, we have the IP address with the mask stripped. That is, we have either
    // - Ipv6 without the zone identifier, for example fe80:5678:9abc:def0:1234:5678:9abc:def0";
    // - Ipv6 with zone identifier, for example fe80:5678:9abc:def0:1234:5678:9abc:def0%ens5";
    //
    // The zone identifier is delimited by an ampersand ('%'). See if it has a zone identifier
    std::vector<std::string> workingIPv6withOptionalZone =
        _splitString(workingIPv6withoutMask[0], '%');

    // If we get only one element, then it does not have a zone identifier. Just return the
    // original IPv6 address
    if (workingIPv6withOptionalZone.size() == 1) {
        result.ip = ipv6;
        return result;
    }

    // If we get to this stage, we have a zone identifier:
    // 1. Concatenate the IPv6 address without the zone identifier with the mask. Place this in the
    // first element
    // 2. Place the zone identifier in the second element. Since the numeric zone identifier can
    // change across system configurations and reboots, we leave the zone identifier in its original
    // form until it is actually used
    if (workingIPv6withoutMask.size() >= 2) {
        result.ip = workingIPv6withOptionalZone[0] + "/" + workingIPv6withoutMask[1];
    } else {
        result.ip = workingIPv6withOptionalZone[0];
    }
    result.zone = workingIPv6withOptionalZone[1];

    return result;
}

CIDR::CIDR(StringData s) {
    auto status = parse(s);
    uassertStatusOK(status);
    *this = status.getValue();
}

CIDR::CIDR() : _family(AF_UNSPEC), _len(0) {
    _ip.fill(0);
}

std::ostream& operator<<(std::ostream& s, const CIDR& cidr) {
    return append(s, cidr._family, cidr._ip, cidr._len);
}

StringBuilder& operator<<(StringBuilder& s, const CIDR& cidr) {
    return append(s, cidr._family, cidr._ip, cidr._len);
}

}  // namespace mongo
