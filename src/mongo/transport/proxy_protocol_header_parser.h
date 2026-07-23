// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/sockaddr.h"

#include <algorithm>
#include <cstddef>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#ifndef _WIN32
#include <sys/un.h>
#endif

namespace mongo::transport {

// PROXY protocol signature strings. kProxyV2Signature contains an embedded null byte; always use
// .size() rather than strlen() when working with it.
inline constexpr std::string_view kProxyV1Signature = "PROXY";
inline constexpr std::string_view kProxyV2Signature = []() {
    using namespace std::literals::string_view_literals;  // required due to embedded NUL
    return "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"sv;
}();

/**
 * The maximum number of bytes ever needed by a proxy protocol header; represents
 * the minimum TCP MTU.
 */
inline constexpr size_t kDefaultProxyProtocolHeaderReadSize = 536;

/**
 * Adapted from https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt
 */

/**
 * Authority TLV containing the authority portion of the HTTP request, as defined in RFC 7230
 * Section 5.3. In the Proxy Protocol v2 (PP2), this corresponds to the top-level AUTHORITY TLV
 * (PP2_TYPE_AUTHORITY, 0x02). It is commonly used by proxies to convey the Server Name Indication
 * (SNI) from the TLS handshake, allowing routing of encrypted traffic based on the intended
 * hostname without decrypting it. For example, if the client connects to https://my.mongodb.com,
 * the kProxyProtocolTypeAuthority value will likely be my.mongodb.com. This field is used to
 * populate the host names in SSLPeerInfo, so that downstream servers can know about the original
 * hostname requested. It is defined in section 2.2.2 of the Proxy Protocol specification
 * (https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt). In MongoDB, this is used by the
 * split horizon logic to determine the horizons to apply to the connection, and is used to populate
 * the SNI field in SSLPeerInfo.
 */
constexpr uint8_t kProxyProtocolTypeAuthority = 0x02;

/**
 * SSL TLV types, used to indicate various SSL-related information.
 */
constexpr uint8_t kProxyProtocolSSLTlvType = 0x20;

/**
 * Maximum TLV entries parsed from a single proxy protocol TLV vector, including SSL sub-TLV
 * vectors. This bounds per-connection allocations on the proxy unix-socket path while remaining
 * well above expected production metadata usage.
 */
constexpr size_t kMaxProxyProtocolTLVEntriesPerVector = 64;

/**
 * MongoDB custom PP2 TLV type as per MongoDB Proxy Protocol Technical Design document.
 * The kProxyProtocolSSLTlvDN TLV is used to indicate the distinguished name (DN) from the client's
 * SSL certificate. The value of this TLV is a string representing the distinguished name, such as
 * "CN=client.mongodb.com, OU=Clients, O=MongoDB, L=New York, ST=NY, C=US"
 */
constexpr uint8_t kProxyProtocolSSLTlvDN = 0xE0;

/**
 * MongoDB custom PP2 TLV type as per MongoDB Proxy Protocol Technical Design document.
 * The kProxyProtocolSSLTlvPeerRoles TLV is used to indicate the roles of the peer in the SSL
 * connection. The value of this TLV is a string representing the MongoDB roles. Use the
 * parsePeerRoles function to parse this data into a format the server understands
 */
constexpr uint8_t kProxyProtocolSSLTlvPeerRoles = 0xE1;

/**
 * Represents the true endpoints that a proxy using the Proxy Protocol is proxying for us.
 */
struct ProxiedEndpoints {
    // The true origin of the connection, i.e. the IP address of the client behind the
    // proxy.
    SockAddr sourceAddress;
    // The true destination of the connection, almost always the address that the proxy
    // is listening on.
    SockAddr destinationAddress;
};
/**
 * Represents the optional TLV (type-length-value) data in the V2 proxy protocol. See sections 2.2.X
 * below for more information on the type and expected data.
 * https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt
 */
struct ProxiedSupplementaryDataEntry {
    uint8_t type;
    std::string data;
};

/**
 * Represents the optional ssl TLV (type-length-value) data in the V2 proxy protocol. See
 * section 2.2.6 below for more details on these fields.
 * https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt
 */
struct ProxiedSSLData {
    uint8_t clientFlags;
    uint32_t verify;
    std::vector<ProxiedSupplementaryDataEntry> subTLVs;
};

/**
 * Contains the results of parsing a Proxy Protocol header. bytesParsed contains the
 * length of the parsed header, and endpoints contains any endpoint information that the
 * header optionally contained.
 */
struct ParserResults {
    // The endpoint metadata should be populated iff parsing is complete, the connection
    // is marked as remote, and the connection is not marked as UNKNOWN.
    boost::optional<ProxiedEndpoints> endpoints = {};
    // Optional tlv vectors supplied by the client. This is only supported on V2
    // of the proxy protocol.
    std::vector<ProxiedSupplementaryDataEntry> tlvs;
    boost::optional<ProxiedSSLData> sslTlvs;

    size_t bytesParsed = 0;
};

/**
 * Parses a string potentially starting with a proxy protocol header (either V1 or V2). The parsing
 * logic adjusts based on whether the socket being read from is a Unix domain socket
 * (`isProxyUnixSock`). If the string begins with a partial but incomplete header, returns an empty
 * optional; otherwise, returns a ParserResults with the results of the parse.
 *
 * Will throw eagerly on a malformed header.
 */
boost::optional<ParserResults> parseProxyProtocolHeader(std::string_view buffer,
                                                        bool isProxyUnixSock);

/**
 * Peek a buffer for at least 12 bytes to determine if it may be a proxy protocol header.
 *
 * Note that this does not definitively identify the initial packet as proxy protocol,
 * it only establishes that it is possible that it is such.
 * To be used in determining appropriate error messages during otherwise failed
 * initial handshakes only.
 */
bool maybeProxyProtocolHeader(std::string_view buffer);

namespace proxy_protocol_details {
static constexpr size_t kMaxUnixPathLength = 108;

template <typename AddrUn = sockaddr_un>
AddrUn parseSockAddrUn(std::string_view buffer) {
    AddrUn addr{};
    addr.sun_family = AF_UNIX;

    std::string_view path = buffer.substr(0, buffer.find('\0'));
    uassert(ErrorCodes::FailedToParse,
            fmt::format("Provided unix path longer than system supports: {}", buffer),
            path.size() < sizeof(AddrUn::sun_path));
    std::copy(path.begin(), path.end(), addr.sun_path);
    return addr;
}

void validateIpv4Address(std::string_view addr);
void validateIpv6Address(std::string_view addr);

}  // namespace proxy_protocol_details

}  // namespace mongo::transport
