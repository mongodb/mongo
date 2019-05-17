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

#pragma once

#include <fmt/format.h>
#include <iosfwd>
#include <string>

#include <boost/optional.hpp>

#include "mongo/bson/util/builder.h"

namespace mongo {

class Status;
template <typename T>
class StatusWith;
class StringData;

/**
 * Name of a process on the network.
 *
 * Composed of some name component, followed optionally by a colon and a numeric port.  The name
 * might be an IPv4 or IPv6 address or a relative or fully qualified host name, or an absolute
 * path to a unix socket.
 */
struct HostAndPort {
    /**
     * Parses "text" to produce a HostAndPort.  Returns either that or an error status describing
     * the parse failure.
     */
    static StatusWith<HostAndPort> parse(StringData text);

    /**
     * A version of 'parse' that throws a UserException if a parsing error is encountered.
     */
    static HostAndPort parseThrowing(StringData text) {
        return uassertStatusOK(parse(text));
    }

    /**
     * Construct an empty/invalid HostAndPort.
     */
    HostAndPort();

    /**
     * Constructs a HostAndPort by parsing "text" of the form hostname[:portnumber]
     * Throws an AssertionException if bad config std::string or bad port #.
     */
    explicit HostAndPort(StringData text);

    /**
     * Constructs a HostAndPort with the hostname "h" and port "p".
     *
     * If "p" is -1, port() returns ServerGlobalParams::DefaultDBPort.
     */
    HostAndPort(const std::string& h, int p);

    /**
     * (Re-)initializes this HostAndPort by parsing "s".  Returns
     * Status::OK on success.  The state of this HostAndPort is unspecified
     * after initialize() returns a non-OK status, though it is safe to
     * assign to it or re-initialize it.
     */
    Status initialize(StringData s);

    bool operator<(const HostAndPort& r) const;
    bool operator==(const HostAndPort& r) const;
    bool operator!=(const HostAndPort& r) const {
        return !(*this == r);
    }

    /**
     * Returns true if the hostname looks localhost-y.
     *
     * TODO: Make a more rigorous implementation, perhaps elsewhere in
     * the networking library.
     */
    bool isLocalHost() const;

    /**
     * Returns true if the hostname is an IP matching the default route.
     */
    bool isDefaultRoute() const;

    /**
     * Returns a string representation of "host:port".
     */
    std::string toString() const;

    /**
     * Like toString(), but writes to various `sink` instead.
     */
    void append(StringBuilder& sink) const;
    void append(fmt::writer& sink) const;

    /**
     * Returns true if this object represents no valid HostAndPort.
     */
    bool empty() const;

    const std::string& host() const {
        return _host;
    }
    int port() const;

    bool hasPort() const {
        return _port >= 0;
    }

    template <typename H>
    friend H AbslHashValue(H h, const HostAndPort& hostAndPort) {
        return H::combine(std::move(h), hostAndPort.host(), hostAndPort.port());
    }

private:
    std::string _host;
    int _port;  // -1 indicates unspecified
};

std::ostream& operator<<(std::ostream& os, const HostAndPort& hp);

StringBuilder& operator<<(StringBuilder& os, const HostAndPort& hp);

StackStringBuilder& operator<<(StackStringBuilder& os, const HostAndPort& hp);

}  // namespace mongo

template <>
struct fmt::formatter<mongo::HostAndPort> {
    template <typename ParseContext>
    auto parse(ParseContext& ctx) -> decltype(ctx.begin()) {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const mongo::HostAndPort& hp, FormatContext& ctx) -> decltype(ctx.out()) {
        fmt::writer w(ctx.out());
        hp.append(w);
        return ctx.out();
    }
};
