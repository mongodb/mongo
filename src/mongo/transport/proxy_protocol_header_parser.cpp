/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/transport/proxy_protocol_header_parser.h"

#include <boost/optional.hpp>
#include <cstring>
#include <exception>
#include <fmt/format.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/un.h>
#endif

#include "mongo/base/parse_number.h"
#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/endian.h"
#include "mongo/util/assert_util.h"

namespace mongo::transport {

using namespace fmt::literals;

namespace {
StringData parseToken(StringData& s, const char c) {
    size_t pos = s.find(c);
    uassert(ErrorCodes::FailedToParse,
            "Proxy Protocol Version 1 address string malformed: {}"_format(s),
            pos != std::string::npos);
    StringData result = s.substr(0, pos);
    s = s.substr(pos + 1);
    return result;
}
}  // namespace

namespace proxy_protocol_details {

void validateIpv4Address(StringData addr) {
    StringData buffer = addr;
    const NumberParser octetParser =
        NumberParser().skipWhitespace(false).base(10).allowTrailingText(false);
    try {
        for (size_t i = 0; i < 4; ++i) {
            unsigned octet = 0;
            if (i == 3) {
                uassertStatusOK(octetParser(buffer, &octet));
            } else {
                uassertStatusOK(octetParser(parseToken(buffer, '.'), &octet));
            }
            uassert(
                ErrorCodes::FailedToParse,
                "Proxy Protocol Version 1 address string specified malformed IPv4 address: {}"_format(
                    addr),
                octet <= 255);
        }
    } catch (const ExceptionFor<ErrorCodes::FailedToParse>&) {
        uasserted(
            ErrorCodes::FailedToParse,
            "Proxy Protocol Version 1 address string specified malformed IPv4 address: {}"_format(
                addr));
    }
}

void validateIpv6Address(StringData addr) {
    static constexpr StringData doubleColon = "::"_sd;

    auto validateHexadectets = [](StringData buffer) -> size_t {
        auto validateHexadectet = [](StringData hexadectet) {
            const NumberParser hexadectetParser =
                NumberParser().skipWhitespace(false).base(16).allowTrailingText(false);
            unsigned value = 0;
            uassertStatusOK(hexadectetParser(hexadectet, &value));
            uassert(
                ErrorCodes::FailedToParse,
                "Proxy Protocol Version 1 address string contains malformed IPv6 hexadectet: {}"_format(
                    hexadectet),
                hexadectet.size() == 4 && value >= 0);
        };

        if (buffer.empty())
            return 0;

        uassert(
            ErrorCodes::FailedToParse,
            "Proxy Protocol Version 1 address string contains malformed IPv6 hexadectet: {}"_format(
                buffer),
            buffer.find(doubleColon) == std::string::npos);

        size_t numHexadectets = 0;
        while (!buffer.empty()) {
            if (const size_t pos = buffer.find(':'); pos != std::string::npos) {
                validateHexadectet(buffer.substr(0, pos));
                ++numHexadectets;
                buffer = buffer.substr(pos + 1);
            } else {
                validateHexadectet(buffer);
                return numHexadectets + 1;
            }
        }
        uasserted(
            ErrorCodes::FailedToParse,
            "Proxy Protocol Version 1 address string contains malformed IPv6 hexadectet: {}"_format(
                buffer));
    };

    // There can be at most one double colon in our address. Split on the first
    // one and validate neither half has another implicitly.
    try {
        if (const auto pos = addr.find(doubleColon); pos != std::string::npos) {
            const size_t numHexadectets = validateHexadectets(addr.substr(0, pos)) +
                validateHexadectets(addr.substr(pos + doubleColon.size()));
            uassert(
                ErrorCodes::FailedToParse,
                "Proxy Protocol Version 1 address string specified malformed IPv6 address: {}"_format(
                    addr),
                numHexadectets < 8);
        } else {
            const size_t numHexadectets = validateHexadectets(addr);
            uassert(
                ErrorCodes::FailedToParse,
                "Proxy Protocol Version 1 address string specified malformed IPv6 address: {}"_format(
                    addr),
                numHexadectets == 8);
        }
    } catch (const ExceptionFor<ErrorCodes::FailedToParse>&) {
        uasserted(
            ErrorCodes::FailedToParse,
            "Proxy Protocol Version 1 address string specified malformed IPv6 address: {}"_format(
                addr));
    }
}

}  // namespace proxy_protocol_details

namespace {

// Interprets the first sizeof(T) bytes of data as a T and returns it, advancing the data cursor
// by the same amount. Does not account for endianness of the data buffer.
template <typename T>
T extract(StringData& data) {
    MONGO_STATIC_ASSERT(std::is_trivially_copyable_v<T>);
    static constexpr size_t numBytes = sizeof(T);
    if (data.size() < numBytes) {
        throw std::out_of_range("Not enough space to extract object of size {}"_format(numBytes));
    }

    T result;
    memcpy(&result, data.rawData(), numBytes);
    data = data.substr(numBytes);
    return result;
}

constexpr StringData kV1Start = "PROXY"_sd;

bool parseV1Buffer(StringData& buffer, boost::optional<ProxiedEndpoints>& endpoints) {
    buffer = buffer.substr(kV1Start.size());
    if (buffer.empty())
        return false;

    // Scan the buffer for a newline and prepare an output buffer which begins just past
    // the line.
    static constexpr StringData crlf = "\x0D\x0A"_sd;
    const auto crlfPos = buffer.find(crlf);

    static constexpr size_t kMaximumV1HeaderSize = 107;
    static constexpr size_t kMaximumV1InetLineSize = kMaximumV1HeaderSize - kV1Start.size();
    if (crlfPos == std::string::npos) {
        // If we couldn't find a newline sequence, then fail if there cannot be enough room
        // for one to appear in the future.
        uassert(ErrorCodes::FailedToParse,
                "No terminating newline found in Proxy Protocol header V1: {}"_format(buffer),
                buffer.size() <= kMaximumV1InetLineSize);
        return false;
    } else {
        // If we could, then fail if the sequence doesn't occur within the maximum line length.
        uassert(ErrorCodes::FailedToParse,
                "No terminating newline found in Proxy Protocol header V1: {}"_format(buffer),
                crlfPos + crlf.size() <= kMaximumV1InetLineSize);
    }

    // Prepare a result buffer pointing to just after the crlf sequence.
    const auto resultBuffer = buffer.substr(crlfPos + crlf.size());

    static constexpr StringData kTcp4Prefix = " TCP4 "_sd;
    static constexpr StringData kTcp6Prefix = " TCP6 "_sd;
    int aFamily = AF_UNSPEC;
    if (buffer.startsWith(kTcp4Prefix)) {
        aFamily = AF_INET;
        buffer = buffer.substr(kTcp4Prefix.size());
    } else if (buffer.startsWith(kTcp6Prefix)) {
        aFamily = AF_INET6;
        buffer = buffer.substr(kTcp6Prefix.size());
    } else if (buffer.startsWith(" UNKNOWN"_sd)) {
        buffer = resultBuffer;
        endpoints = {};
        return true;
    } else {
        uasserted(ErrorCodes::FailedToParse,
                  "Proxy Protocol Version 1 address string malformed: {}"_format(buffer));
    }

    // The remainder of the string should now tokenize into four substrings:
    // srcAddr dstAddr srcPort dstPort
    const StringData srcAddr = parseToken(buffer, ' ');
    const StringData dstAddr = parseToken(buffer, ' ');

    invariant(aFamily == AF_INET || aFamily == AF_INET6);
    if (aFamily == AF_INET) {
        proxy_protocol_details::validateIpv4Address(srcAddr);
        proxy_protocol_details::validateIpv4Address(dstAddr);
    } else {
        proxy_protocol_details::validateIpv6Address(srcAddr);
        proxy_protocol_details::validateIpv6Address(dstAddr);
    }

    const StringData srcPortStr = parseToken(buffer, ' ');
    const StringData dstPortStr = parseToken(buffer, '\r');

    const NumberParser portParser =
        NumberParser().skipWhitespace(false).base(10).allowTrailingText(false);
    unsigned srcPort, dstPort = 0;
    uassertStatusOK(portParser(srcPortStr, &srcPort));
    uassertStatusOK(portParser(dstPortStr, &dstPort));

    auto validatePort = [](int port) {
        uassert(ErrorCodes::FailedToParse,
                "Proxy Protocol Version 1 address string specified invalid port: {}"_format(port),
                port <= 65535);
    };
    validatePort(srcPort);
    validatePort(dstPort);

    buffer = resultBuffer;
    try {
        endpoints = ProxiedEndpoints{SockAddr::create(srcAddr, srcPort, aFamily),
                                     SockAddr::create(dstAddr, dstPort, aFamily)};
        return true;
    } catch (const ExceptionFor<ErrorCodes::HostUnreachable>&) {
        // SockAddr can throw on construction if the address passed in is malformed.
        uasserted(ErrorCodes::FailedToParse,
                  "Proxy Protocol Version 1 address string specified unreachable host: {}"_format(
                      buffer));
    }
}

// Since this string contains a null, it's critical we use a literal here.
constexpr StringData kV2Start = "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"_sd;

bool parseV2Buffer(StringData& buffer, boost::optional<ProxiedEndpoints>& endpoints) {
    buffer = buffer.substr(kV2Start.size());
    if (buffer.empty())
        return false;

    try {
        const char protocolVersionAndCommandByte = extract<char>(buffer);

        bool isLocal = false;
        // The high nibble must be 2 (version 2) and the low nibble must be either 0 or 1 (local or
        // remote).
        if (protocolVersionAndCommandByte == '\x20') {
            isLocal = true;
        } else if (protocolVersionAndCommandByte != '\x21') {
            uasserted(
                ErrorCodes::FailedToParse,
                "Invalid version or command byte given in Proxy Protocol header V2: {}"_format(
                    protocolVersionAndCommandByte));
        }

        if (buffer.empty())
            return false;

        const uint8_t transportProtocolAndAddressFamilyByte = extract<uint8_t>(buffer);

        int aFamily = 0;
        {
            // Discard the family if this is a local connection.
            uint8_t aFamilyByte = isLocal ? 0 : (transportProtocolAndAddressFamilyByte & 0xF0) >> 4;
            switch (aFamilyByte) {
                case 0:
                    aFamily = AF_UNSPEC;
                    break;
                case 1:
                    aFamily = AF_INET;
                    break;
                case 2:
                    aFamily = AF_INET6;
                    break;
                case 3:
                    aFamily = AF_UNIX;
                    break;
                default:
                    uasserted(ErrorCodes::FailedToParse,
                              "Invalid address family given in Proxy Protocol header V2: {}"_format(
                                  aFamilyByte));
            }
        }

        uint8_t protocol = (transportProtocolAndAddressFamilyByte & 0xF);
        uassert(ErrorCodes::FailedToParse,
                "Invalid protocol given in Proxy Protocol header V2: {}"_format(protocol),
                protocol <= 0x2);

        // If protocol is unspecified, we should also ignore address information.
        if (protocol == 0) {
            aFamily = AF_UNSPEC;
        }

        if (buffer.size() < sizeof(uint16_t))
            return false;

        const size_t length = endian::bigToNative(extract<uint16_t>(buffer));
        if (buffer.size() < length)
            return false;

        // Prepare an output buffer that skips past the end of the header.
        // We'll assign this to the buffer if we fully succeed in parsing the header.
        const auto resultBuffer = buffer.substr(length);

        switch (aFamily) {
            case AF_UNSPEC:
                break;
            case AF_INET: {
                // The proxy protocol allocates 12 bytes to represent a pair of IPv4 addresses
                // along with their ports.
                static constexpr size_t kIPv4ProxyProtocolSize = 12;
                MONGO_STATIC_ASSERT(2 * (sizeof(in_addr) + sizeof(uint16_t)) ==
                                    kIPv4ProxyProtocolSize);
                uassert(ErrorCodes::FailedToParse,
                        "Proxy Protocol Version 2 address string too short: {}"_format(buffer),
                        length >= kIPv4ProxyProtocolSize);
                sockaddr_in src_addr{};
                sockaddr_in dst_addr{};
                src_addr.sin_family = dst_addr.sin_family = AF_INET;
                // These are specified by the protocol to be in network byte order, which
                // is what sin_addr/sin_port expect, so we copy them directly.
                src_addr.sin_addr = extract<in_addr>(buffer);
                dst_addr.sin_addr = extract<in_addr>(buffer);
                src_addr.sin_port = extract<uint16_t>(buffer);
                dst_addr.sin_port = extract<uint16_t>(buffer);
                endpoints = ProxiedEndpoints{SockAddr((sockaddr*)&src_addr, sizeof(sockaddr_in)),
                                             SockAddr((sockaddr*)&dst_addr, sizeof(sockaddr_in))};
                break;
            }
            case AF_INET6: {
                // The proxy protocol allocates 36 bytes to represent a pair of IPv6 addresses
                // along with their ports.
                static constexpr size_t kIPv6ProxyProtocolSize = 36;
                MONGO_STATIC_ASSERT(2 * (sizeof(in6_addr) + sizeof(uint16_t)) ==
                                    kIPv6ProxyProtocolSize);
                uassert(ErrorCodes::FailedToParse,
                        "Proxy Protocol Version 2 address string too short: {}"_format(buffer),
                        length >= kIPv6ProxyProtocolSize);
                sockaddr_in6 src_addr{};
                sockaddr_in6 dst_addr{};
                src_addr.sin6_family = dst_addr.sin6_family = AF_INET6;
                // These are specified by the protocol to be in network byte order, which
                // is what sin_addr/sin_port expect, so we copy them directly.
                src_addr.sin6_addr = extract<in6_addr>(buffer);
                dst_addr.sin6_addr = extract<in6_addr>(buffer);
                src_addr.sin6_port = extract<uint16_t>(buffer);
                dst_addr.sin6_port = extract<uint16_t>(buffer);
                endpoints = ProxiedEndpoints{SockAddr((sockaddr*)&src_addr, sizeof(sockaddr_in6)),
                                             SockAddr((sockaddr*)&dst_addr, sizeof(sockaddr_in6))};
                break;
            }
            case AF_UNIX: {
                // The proxy protocol allocates 216 bytes to represent a pair of UNIX address,
                // but we don't assert type sizes here because some platforms don't support
                // UNIX addresses of this length - they are checked in parseSockAddrUn.
                static constexpr size_t kUnixProxyProtocolSize = 216;
                uassert(ErrorCodes::FailedToParse,
                        "Proxy Protocol Version 2 address string too short: {}"_format(buffer),
                        length >= kUnixProxyProtocolSize);
                const auto src_addr = proxy_protocol_details::parseSockAddrUn(
                    buffer.substr(0, proxy_protocol_details::kMaxUnixPathLength));
                const auto dst_addr = proxy_protocol_details::parseSockAddrUn(
                    buffer.substr(proxy_protocol_details::kMaxUnixPathLength,
                                  proxy_protocol_details::kMaxUnixPathLength));

                endpoints = ProxiedEndpoints{SockAddr((sockaddr*)&src_addr, sizeof(sockaddr_un)),
                                             SockAddr((sockaddr*)&dst_addr, sizeof(sockaddr_un))};
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
        buffer = resultBuffer;
        return true;
    } catch (const std::out_of_range&) {
        return false;
    }
}

}  // namespace

boost::optional<ParserResults> parseProxyProtocolHeader(StringData buffer) {
    // Check if the buffer presented is V1, V2, or neither.
    const size_t originalBufferSize = buffer.size();

    ParserResults results;
    bool complete = false;
    if (buffer.startsWith(kV1Start)) {
        complete = parseV1Buffer(buffer, results.endpoints);
    } else if (buffer.startsWith(kV2Start)) {
        complete = parseV2Buffer(buffer, results.endpoints);
    } else {
        uassert(ErrorCodes::FailedToParse,
                "Initial Proxy Protocol header bytes invalid: {}"
                "; Make sure your proxy is configured to emit a Proxy "
                "Protocol header"_format(buffer),
                kV1Start.startsWith(buffer) || kV2Start.startsWith(buffer));
    }

    if (complete) {
        results.bytesParsed = originalBufferSize - buffer.size();
        return results;
    } else {
        return {};
    }
}


}  // namespace mongo::transport
