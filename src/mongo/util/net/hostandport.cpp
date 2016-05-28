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

#include "mongo/platform/basic.h"

#include "mongo/util/net/hostandport.h"

#include <boost/functional/hash.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

StatusWith<HostAndPort> HostAndPort::parse(StringData text) {
    HostAndPort result;
    Status status = result.initialize(text);
    if (!status.isOK()) {
        return StatusWith<HostAndPort>(status);
    }
    return StatusWith<HostAndPort>(result);
}

HostAndPort::HostAndPort() : _port(-1) {}

HostAndPort::HostAndPort(StringData text) {
    uassertStatusOK(initialize(text));
}

HostAndPort::HostAndPort(const std::string& h, int p) : _host(h), _port(p) {}

bool HostAndPort::operator<(const HostAndPort& r) const {
    const int cmp = host().compare(r.host());
    if (cmp)
        return cmp < 0;
    return port() < r.port();
}

bool HostAndPort::operator==(const HostAndPort& r) const {
    return host() == r.host() && port() == r.port();
}

int HostAndPort::port() const {
    if (hasPort())
        return _port;
    return ServerGlobalParams::DefaultDBPort;
}

bool HostAndPort::isLocalHost() const {
    return (_host == "localhost" || str::startsWith(_host.c_str(), "127.") || _host == "::1" ||
            _host == "anonymous unix socket" || _host.c_str()[0] == '/'  // unix socket
            );
}

std::string HostAndPort::toString() const {
    StringBuilder ss;
    append(ss);
    return ss.str();
}

void HostAndPort::append(StringBuilder& ss) const {
    // wrap ipv6 addresses in []s for roundtrip-ability
    if (host().find(':') != std::string::npos) {
        ss << '[' << host() << ']';
    } else {
        ss << host();
    }
    ss << ':' << port();
}

bool HostAndPort::empty() const {
    return _host.empty() && _port < 0;
}

Status HostAndPort::initialize(StringData s) {
    size_t colonPos = s.rfind(':');
    StringData hostPart = s.substr(0, colonPos);

    // handle ipv6 hostPart (which we require to be wrapped in []s)
    const size_t openBracketPos = s.find('[');
    const size_t closeBracketPos = s.find(']');
    if (openBracketPos != std::string::npos) {
        if (openBracketPos != 0) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "'[' present, but not first character in "
                                        << s.toString());
        }
        if (closeBracketPos == std::string::npos) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "ipv6 address is missing closing ']' in hostname in "
                                        << s.toString());
        }

        hostPart = s.substr(openBracketPos + 1, closeBracketPos - openBracketPos - 1);
        // prevent accidental assignment of port to the value of the final portion of hostPart
        if (colonPos < closeBracketPos) {
            colonPos = std::string::npos;
        } else if (colonPos != closeBracketPos + 1) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Extraneous characters between ']' and pre-port ':'"
                                        << " in "
                                        << s.toString());
        }
    } else if (closeBracketPos != std::string::npos) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "']' present without '[' in " << s.toString());
    } else if (s.find(':') != colonPos) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "More than one ':' detected. If this is an ipv6 address,"
                                    << " it needs to be surrounded by '[' and ']'; "
                                    << s.toString());
    }

    if (hostPart.empty()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Empty host component parsing HostAndPort from \""
                                    << escape(s.toString())
                                    << "\"");
    }

    int port;
    if (colonPos != std::string::npos) {
        const StringData portPart = s.substr(colonPos + 1);
        Status status = parseNumberFromStringWithBase(portPart, 10, &port);
        if (!status.isOK()) {
            return status;
        }
        if (port <= 0) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Port number " << port
                                        << " out of range parsing HostAndPort from \""
                                        << escape(s.toString())
                                        << "\"");
        }
    } else {
        port = -1;
    }
    _host = hostPart.toString();
    _port = port;
    return Status::OK();
}

std::ostream& operator<<(std::ostream& os, const HostAndPort& hp) {
    return os << hp.toString();
}

}  // namespace mongo

MONGO_HASH_NAMESPACE_START
size_t hash<mongo::HostAndPort>::operator()(const mongo::HostAndPort& host) const {
    hash<int> intHasher;
    size_t hash = intHasher(host.port());
    boost::hash_combine(hash, host.host());
    return hash;
}
MONGO_HASH_NAMESPACE_END
