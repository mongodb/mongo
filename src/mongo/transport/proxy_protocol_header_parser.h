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

#pragma once

#include <algorithm>
#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#ifndef _WIN32
#include <sys/un.h>
#endif

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/sockaddr.h"

namespace mongo::transport {

/**
 * The maximum number of bytes ever needed by a proxy protocol header; represents
 * the minimum TCP MTU.
 */
constexpr size_t kProxyProtocolHeaderSizeUpperBound = 536;
constexpr uint8_t kProxyProtocolSSLTlvType = 0x20;

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
boost::optional<ParserResults> parseProxyProtocolHeader(StringData buffer, bool isProxyUnixSock);

/**
 * Peek a buffer fo at least 12 bytes to determine if it may be a proxy protocol header.
 *
 * Note that this does not definitively identify the initial packet as proxy protocol,
 * it only establishes that it is possible that it is such.
 * To be used in determining appropriate error messages during otherwise failed
 * initial handshakes only.
 */
bool maybeProxyProtocolHeader(StringData buffer);

namespace proxy_protocol_details {
static constexpr size_t kMaxUnixPathLength = 108;

template <typename AddrUn = sockaddr_un>
AddrUn parseSockAddrUn(StringData buffer) {
    AddrUn addr{};
    addr.sun_family = AF_UNIX;

    StringData path = buffer.substr(0, buffer.find('\0'));
    uassert(ErrorCodes::FailedToParse,
            fmt::format("Provided unix path longer than system supports: {}", buffer),
            path.size() < sizeof(AddrUn::sun_path));
    std::copy(path.begin(), path.end(), addr.sun_path);
    return addr;
}

void validateIpv4Address(StringData addr);
void validateIpv6Address(StringData addr);

}  // namespace proxy_protocol_details

}  // namespace mongo::transport
