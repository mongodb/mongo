/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/bson/util/builder.h"
#include "mongo/platform/hash_namespace.h"

namespace mongo {
class Status;
class StringData;
template <typename T>
class StatusWith;

/**
 * Name of a process on the network.
 *
 * Composed of some name component, followed optionally by a colon and a numeric port.  The name
 * might be an IPv4 or IPv6 address or a relative or fully qualified host name, or an absolute
 * path to a unix socket.
 */
struct HostAndPort {
    /**
     * Parses "text" to produce a HostAndPort.  Returns either that or an error
     * status describing the parse failure.
     */
    static StatusWith<HostAndPort> parse(StringData text);

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
     * Returns a string representation of "host:port".
     */
    std::string toString() const;

    /**
     * Like toString(), above, but writes to "ss", instead.
     */
    void append(StringBuilder& ss) const;

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

private:
    std::string _host;
    int _port;  // -1 indicates unspecified
};

std::ostream& operator<<(std::ostream& os, const HostAndPort& hp);

}  // namespace mongo

MONGO_HASH_NAMESPACE_START

template <>
struct hash<mongo::HostAndPort> {
    size_t operator()(const mongo::HostAndPort& host) const;
};

MONGO_HASH_NAMESPACE_END
