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
#include <boost/optional.hpp>
#include <fmt/format.h>

#ifndef _WIN32
#include <sys/un.h>
#endif

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/sockaddr.h"

namespace mongo::transport {

/**
 * The maximum number of bytes ever needed by a proxy protocol header; represents
 * the minimum TCP MTU.
 */
static constexpr size_t kProxyProtocolHeaderSizeUpperBound = 536;

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
 * Contains the results of parsing a Proxy Protocol header. bytesParsed contains the
 * length of the parsed header, and endpoints contains any endpoint information that the
 * header optionally contained.
 */
struct ParserResults {
    // The endpoint metadata should be populated iff parsing is complete, the connection
    // is marked as remote, and the connection is not marked as UNKNOWN.
    boost::optional<ProxiedEndpoints> endpoints = {};
    size_t bytesParsed = 0;
};

/**
 * Parses a string potentially starting with a proxy protocol header (either V1 or V2).
 * If the string begins with a partial but incomplete header, returns an empty optional;
 * otherwise, returns a ParserResults with the results of the parse.
 *
 * Will throw eagerly on a malformed header.
 */
boost::optional<ParserResults> parseProxyProtocolHeader(StringData buffer);

namespace proxy_protocol_details {
static constexpr size_t kMaxUnixPathLength = 108;

template <typename AddrUn = sockaddr_un>
AddrUn parseSockAddrUn(StringData buffer) {
    using namespace fmt::literals;

    AddrUn addr{};
    addr.sun_family = AF_UNIX;

    StringData path = buffer.substr(0, buffer.find('\0'));
    uassert(ErrorCodes::FailedToParse,
            "Provided unix path longer than system supports: {}"_format(buffer),
            path.size() < sizeof(AddrUn::sun_path));
    std::copy(path.begin(), path.end(), addr.sun_path);
    return addr;
}

void validateIpv4Address(StringData addr);
void validateIpv6Address(StringData addr);

}  // namespace proxy_protocol_details

}  // namespace mongo::transport
