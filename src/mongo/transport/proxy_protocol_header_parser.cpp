// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/transport/proxy_protocol_header_parser.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/un.h>
#endif

#include "mongo/base/parse_number.h"
#include "mongo/base/static_assert.h"
#include "mongo/platform/endian.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::transport {
using namespace std::literals::string_view_literals;

namespace {
std::string_view parseToken(std::string_view& s, const char c) {
    size_t pos = s.find(c);
    uassert(ErrorCodes::FailedToParse,
            fmt::format("Proxy Protocol Version 1 address string malformed: {}", s),
            pos != std::string::npos);
    std::string_view result = s.substr(0, pos);
    s = s.substr(pos + 1);
    return result;
}

bool isValidTLVType(uint8_t type) {
    // Valid TLV types are defined here:
    // https://www.haproxy.com/documentation/haproxy-configuration-tutorials/proxying-essentials/client-ip-preservation/enable-proxy-protocol/#tlv-format
    return (type >= 0x01 && type <= 0x05) || (type >= 0x20 && type <= 0x25) || (type == 0x30) ||
        (type >= 0xE0 && type <= 0xEF);
}
}  // namespace

namespace proxy_protocol_details {

void validateIpv4Address(std::string_view addr) {
    std::string_view buffer = addr;
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
                fmt::format(
                    "Proxy Protocol Version 1 address string specified malformed IPv4 address: {}",
                    addr),
                octet <= 255);
        }
    } catch (const ExceptionFor<ErrorCodes::FailedToParse>&) {
        uasserted(
            ErrorCodes::FailedToParse,
            fmt::format(
                "Proxy Protocol Version 1 address string specified malformed IPv4 address: {}",
                addr));
    }
}

void validateIpv6Address(std::string_view addr) {
    static constexpr std::string_view doubleColon = "::"sv;

    auto validateHexadectets = [](std::string_view buffer) -> size_t {
        auto validateHexadectet = [](std::string_view hexadectet) {
            const NumberParser hexadectetParser =
                NumberParser().skipWhitespace(false).base(16).allowTrailingText(false);
            unsigned value = 0;
            uassertStatusOK(hexadectetParser(hexadectet, &value));
            uassert(ErrorCodes::FailedToParse,
                    fmt::format("Proxy Protocol Version 1 address string contains malformed IPv6 "
                                "hexadectet: {}",
                                hexadectet),
                    hexadectet.size() == 4);
        };

        if (buffer.empty())
            return 0;

        uassert(
            ErrorCodes::FailedToParse,
            fmt::format(
                "Proxy Protocol Version 1 address string contains malformed IPv6 hexadectet: {}",
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
            fmt::format(
                "Proxy Protocol Version 1 address string contains malformed IPv6 hexadectet: {}",
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
                fmt::format(
                    "Proxy Protocol Version 1 address string specified malformed IPv6 address: {}",
                    addr),
                numHexadectets < 8);
        } else {
            const size_t numHexadectets = validateHexadectets(addr);
            uassert(
                ErrorCodes::FailedToParse,
                fmt::format(
                    "Proxy Protocol Version 1 address string specified malformed IPv6 address: {}",
                    addr),
                numHexadectets == 8);
        }
    } catch (const ExceptionFor<ErrorCodes::FailedToParse>&) {
        uasserted(
            ErrorCodes::FailedToParse,
            fmt::format(
                "Proxy Protocol Version 1 address string specified malformed IPv6 address: {}",
                addr));
    }
}

}  // namespace proxy_protocol_details

namespace {

void parseTLVVectors(std::string_view buffer,
                     std::vector<ProxiedSupplementaryDataEntry>& data,
                     boost::optional<ProxiedSSLData>& sslTlvs);
void parseSubTLVVectors(std::string_view buffer, boost::optional<ProxiedSSLData>& sslTlvs);

// Interprets the first sizeof(T) bytes of data as a T and returns it, advancing the data cursor
// by the same amount. Does not account for endianness of the data buffer.
template <typename T>
T extract(std::string_view& data) {
    MONGO_STATIC_ASSERT(std::is_trivially_copyable_v<T>);
    static constexpr size_t numBytes = sizeof(T);
    if (data.size() < numBytes) {
        throw std::out_of_range(
            fmt::format("Not enough space to extract object of size {}", numBytes));
    }

    T result;
    memcpy(&result, data.data(), numBytes);
    data = data.substr(numBytes);
    return result;
}

void parseSubTLVVectors(std::string_view buffer, boost::optional<ProxiedSSLData>& sslTlvs) {
    static constexpr size_t kSubTLVHeaderSize = 5;
    uassert(ErrorCodes::FailedToParse,
            "Proxy Protocol Version 2 sub-TLV header size too small",
            buffer.size() >= kSubTLVHeaderSize);

    invariant(!sslTlvs);
    sslTlvs.emplace();
    sslTlvs->clientFlags = extract<uint8_t>(buffer);
    sslTlvs->verify = endian::bigToNative(extract<uint32_t>(buffer));
    parseTLVVectors(buffer, sslTlvs->subTLVs, sslTlvs);
}

// Parses buffer to extract TLV vectors into tlvs and sslTlvs. This function assumes that buffer
// only contains TLV vectors.
void parseTLVVectors(std::string_view buffer,
                     std::vector<ProxiedSupplementaryDataEntry>& tlvs,
                     boost::optional<ProxiedSSLData>& sslTlvs) {
    size_t tlvCount = 0;
    while (buffer.size()) {
        static constexpr size_t kTLVHeaderSize = 3;
        uassert(ErrorCodes::FailedToParse,
                fmt::format("Proxy Protocol Version 2 TLV header size larger than buffer size. "
                            "Expected {} > {}",
                            buffer.size(),
                            kTLVHeaderSize),
                buffer.size() > kTLVHeaderSize);
        uassert(ErrorCodes::FailedToParse,
                fmt::format("Proxy Protocol Version 2 TLV entry count exceeds {}",
                            kMaxProxyProtocolTLVEntriesPerVector),
                tlvCount < kMaxProxyProtocolTLVEntriesPerVector);
        ++tlvCount;

        auto type = extract<uint8_t>(buffer);
        uassert(ErrorCodes::FailedToParse,
                fmt::format("Proxy Protocol Version 2 TLV type not valid: {}", type),
                isValidTLVType(type));

        auto length = endian::bigToNative(extract<uint16_t>(buffer));
        uassert(
            ErrorCodes::FailedToParse,
            fmt::format(
                "Proxy Protocol Version 2 TLV length larger than buffer size. Expected {} >= {}",
                buffer.size(),
                length),
            buffer.size() >= length);

        if (type == kProxyProtocolSSLTlvType) {
            if (!sslTlvs) {
                parseSubTLVVectors(buffer.substr(0, length), sslTlvs);
            }
            buffer = buffer.substr(length);
            continue;
        }

        std::string data(buffer.substr(0, length));
        buffer = buffer.substr(length);
        tlvs.emplace_back(type, std::move(data));
    }
}

bool parseV1Buffer(std::string_view& buffer, boost::optional<ProxiedEndpoints>& endpoints) {
    buffer = buffer.substr(kProxyV1Signature.size());
    if (buffer.empty())
        return false;

    // Scan the buffer for a newline and prepare an output buffer which begins just past
    // the line.
    static constexpr std::string_view crlf = "\x0D\x0A"sv;
    const auto crlfPos = buffer.find(crlf);

    static constexpr size_t kMaximumV1HeaderSize = 107;
    static constexpr size_t kMaximumV1InetLineSize =
        kMaximumV1HeaderSize - kProxyV1Signature.size();
    if (crlfPos == std::string::npos) {
        // If we couldn't find a newline sequence, then fail if there cannot be enough room
        // for one to appear in the future.
        uassert(ErrorCodes::FailedToParse,
                fmt::format("No terminating newline found in Proxy Protocol header V1: {}", buffer),
                buffer.size() <= kMaximumV1InetLineSize);
        return false;
    } else {
        // If we could, then fail if the sequence doesn't occur within the maximum line length.
        uassert(ErrorCodes::FailedToParse,
                fmt::format("No terminating newline found in Proxy Protocol header V1: {}", buffer),
                crlfPos + crlf.size() <= kMaximumV1InetLineSize);
    }

    // Prepare a result buffer pointing to just after the crlf sequence.
    const auto resultBuffer = buffer.substr(crlfPos + crlf.size());

    static constexpr std::string_view kTcp4Prefix = " TCP4 "sv;
    static constexpr std::string_view kTcp6Prefix = " TCP6 "sv;
    int aFamily = AF_UNSPEC;
    if (buffer.starts_with(kTcp4Prefix)) {
        aFamily = AF_INET;
        buffer = buffer.substr(kTcp4Prefix.size());
    } else if (buffer.starts_with(kTcp6Prefix)) {
        aFamily = AF_INET6;
        buffer = buffer.substr(kTcp6Prefix.size());
    } else if (buffer.starts_with(" UNKNOWN"sv)) {
        buffer = resultBuffer;
        endpoints = {};
        return true;
    } else {
        uasserted(ErrorCodes::FailedToParse,
                  fmt::format("Proxy Protocol Version 1 address string malformed: {}", buffer));
    }

    // The remainder of the string should now tokenize into four substrings:
    // srcAddr dstAddr srcPort dstPort
    const std::string_view srcAddr = parseToken(buffer, ' ');
    const std::string_view dstAddr = parseToken(buffer, ' ');

    invariant(aFamily == AF_INET || aFamily == AF_INET6);
    if (aFamily == AF_INET) {
        proxy_protocol_details::validateIpv4Address(srcAddr);
        proxy_protocol_details::validateIpv4Address(dstAddr);
    } else {
        proxy_protocol_details::validateIpv6Address(srcAddr);
        proxy_protocol_details::validateIpv6Address(dstAddr);
    }

    const std::string_view srcPortStr = parseToken(buffer, ' ');
    const std::string_view dstPortStr = parseToken(buffer, '\r');

    const NumberParser portParser =
        NumberParser().skipWhitespace(false).base(10).allowTrailingText(false);
    unsigned srcPort, dstPort = 0;
    uassertStatusOK(portParser(srcPortStr, &srcPort));
    uassertStatusOK(portParser(dstPortStr, &dstPort));

    auto validatePort = [](int port) {
        uassert(
            ErrorCodes::FailedToParse,
            fmt::format("Proxy Protocol Version 1 address string specified invalid port: {}", port),
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
        uasserted(
            ErrorCodes::FailedToParse,
            fmt::format("Proxy Protocol Version 1 address string specified unreachable host: {}",
                        buffer));
    }
}

bool parseV2Buffer(std::string_view& buffer, ParserResults& results, bool isProxyUnixSock) {
    auto& endpoints = results.endpoints;
    buffer = buffer.substr(kProxyV2Signature.size());
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
                fmt::format("Invalid version or command byte given in Proxy Protocol header V2: {}",
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
                    uasserted(
                        ErrorCodes::FailedToParse,
                        fmt::format("Invalid address family given in Proxy Protocol header V2: {}",
                                    aFamilyByte));
            }
        }

        uint8_t protocol = (transportProtocolAndAddressFamilyByte & 0xF);
        uassert(ErrorCodes::FailedToParse,
                fmt::format("Invalid protocol given in Proxy Protocol header V2: {}", protocol),
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
        auto remainingBytes = length;

        switch (aFamily) {
            case AF_UNSPEC:
                break;
            case AF_INET: {
                // The proxy protocol allocates 12 bytes to represent a pair of IPv4 addresses
                // along with their ports.
                static constexpr size_t kIPv4ProxyProtocolSize = 12;
                MONGO_STATIC_ASSERT(2 * (sizeof(in_addr) + sizeof(uint16_t)) ==
                                    kIPv4ProxyProtocolSize);
                uassert(
                    ErrorCodes::FailedToParse,
                    fmt::format("Proxy Protocol Version 2 address string too short: {}", buffer),
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
                remainingBytes -= kIPv4ProxyProtocolSize;
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
                uassert(
                    ErrorCodes::FailedToParse,
                    fmt::format("Proxy Protocol Version 2 address string too short: {}", buffer),
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
                remainingBytes -= kIPv6ProxyProtocolSize;
                endpoints = ProxiedEndpoints{SockAddr((sockaddr*)&src_addr, sizeof(sockaddr_in6)),
                                             SockAddr((sockaddr*)&dst_addr, sizeof(sockaddr_in6))};
                break;
            }
            case AF_UNIX: {
                // The proxy protocol allocates 216 bytes to represent a pair of UNIX address,
                // but we don't assert type sizes here because some platforms don't support
                // UNIX addresses of this length - they are checked in parseSockAddrUn.
                static constexpr size_t kUnixProxyProtocolSize = 216;
                uassert(
                    ErrorCodes::FailedToParse,
                    fmt::format("Proxy Protocol Version 2 address string too short: {}", buffer),
                    length >= kUnixProxyProtocolSize);
                const auto src_addr = proxy_protocol_details::parseSockAddrUn(
                    buffer.substr(0, proxy_protocol_details::kMaxUnixPathLength));
                const auto dst_addr = proxy_protocol_details::parseSockAddrUn(
                    buffer.substr(proxy_protocol_details::kMaxUnixPathLength,
                                  proxy_protocol_details::kMaxUnixPathLength));
                remainingBytes -= kUnixProxyProtocolSize;

                endpoints = ProxiedEndpoints{SockAddr((sockaddr*)&src_addr, sizeof(sockaddr_un)),
                                             SockAddr((sockaddr*)&dst_addr, sizeof(sockaddr_un))};

                buffer = buffer.substr(kUnixProxyProtocolSize);
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
        // Only parse TLV vectors for server connections over the proxy unix domain socket.
        if (isProxyUnixSock) {
            uassert(ErrorCodes::FailedToParse,
                    "Proxy protocol header must contain TLV vectors on the proxy unix socket",
                    remainingBytes);
            parseTLVVectors(buffer.substr(0, remainingBytes), results.tlvs, results.sslTlvs);
            uassert(ErrorCodes::FailedToParse,
                    "TLV vectors are missing on the proxy unix socket",
                    !results.tlvs.empty());
        }

        buffer = resultBuffer;
        return true;
    } catch (const std::out_of_range&) {
        return false;
    }
}

}  // namespace

boost::optional<ParserResults> parseProxyProtocolHeader(std::string_view buffer,
                                                        bool isProxyUnixSock) {
    // Check if the buffer presented is V1, V2, or neither.
    const size_t originalBufferSize = buffer.size();

    ParserResults results;
    bool complete = false;
    if (buffer.starts_with(kProxyV1Signature)) {
        uassert(ErrorCodes::FailedToParse,
                "Proxy protocol V1 is not supported on the proxy unix socket",
                !isProxyUnixSock);
        complete = parseV1Buffer(buffer, results.endpoints);
    } else if (buffer.starts_with(kProxyV2Signature)) {
        complete = parseV2Buffer(buffer, results, isProxyUnixSock);
    } else {
        uassert(ErrorCodes::FailedToParse,
                fmt::format("Initial Proxy Protocol header bytes invalid: {}; "
                            "Make sure your proxy is configured to emit a Proxy Protocol header",
                            buffer),
                kProxyV1Signature.starts_with(buffer) || kProxyV2Signature.starts_with(buffer));
    }

    if (complete) {
        results.bytesParsed = originalBufferSize - buffer.size();
        return results;
    } else {
        return {};
    }
}

bool maybeProxyProtocolHeader(std::string_view buffer) {
    return buffer.starts_with(kProxyV1Signature) || buffer.starts_with(kProxyV2Signature);
}

}  // namespace mongo::transport
