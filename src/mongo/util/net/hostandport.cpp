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

#include "mongo/util/net/hostandport.h"

#include "mongo/base/parse_number.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/functional/hash.hpp>

namespace mongo {

StatusWith<HostAndPort> HostAndPort::parse(StringData text) {
    HostAndPort result;
    Status status = result.initialize(text);
    if (!status.isOK()) {
        return StatusWith<HostAndPort>(status);
    }
    return StatusWith<HostAndPort>(result);
}

Status validateHostAndPort(const std::string& hostAndPortStr, const boost::optional<TenantId>&) {
    if (hostAndPortStr.empty()) {
        return Status::OK();
    }
    return HostAndPort::parse(hostAndPortStr).getStatus();
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

bool HostAndPort::isDefaultRoute() const {
    if (_host == "0.0.0.0") {
        return true;
    }

    // There are multiple ways to write IPv6 addresses.
    // We're looking for any representation of the address "0:0:0:0:0:0:0:0".
    // A single sequence of "0" bytes in an IPv6 address may be represented as "::",
    // so we must also match addresses like "::" or "0::0:0".
    // Return false if a character other than ':' or '0' is contained in the address.
    auto firstNonDefaultIPv6Char =
        std::find_if(std::begin(_host), std::end(_host), [](const char& c) {
            return c != ':' && c != '0' && c != '[' && c != ']';
        });
    return firstNonDefaultIPv6Char == std::end(_host);
}

std::string HostAndPort::toString() const {
    StringBuilder ss;
    ss << *this;
    return ss.str();
}

void HostAndPort::_appendToVisitor(AppendVisitor& write) const {
    // wrap ipv6 addresses in []s for roundtrip-ability
    if (host().find(':') != std::string::npos) {
        write("[");
        write(host());
        write("]");
    } else {
        write(host());
    }
    if (host().find('/') == std::string::npos) {
        write(":");
        write(port());
    }
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
                          str::stream()
                              << "'[' present, but not first character in " << std::string{s});
        }
        if (closeBracketPos == std::string::npos) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "ipv6 address is missing closing ']' in hostname in "
                                        << std::string{s});
        }

        hostPart = s.substr(openBracketPos + 1, closeBracketPos - openBracketPos - 1);
        // prevent accidental assignment of port to the value of the final portion of hostPart
        if (colonPos < closeBracketPos) {
            // If the last colon is inside the brackets, then there must not be a port.
            if (s.size() != closeBracketPos + 1) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "missing colon after ']' before the port in "
                                            << std::string{s});
            }
            colonPos = std::string::npos;
        } else if (colonPos != closeBracketPos + 1) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Extraneous characters between ']' and pre-port ':'"
                                        << " in " << std::string{s});
        }
    } else if (closeBracketPos != std::string::npos) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "']' present without '[' in " << std::string{s});
    } else if (s.find(':') != colonPos) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "More than one ':' detected. If this is an ipv6 address,"
                          << " it needs to be surrounded by '[' and ']'; " << std::string{s});
    }

    if (hostPart.empty()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Empty host component parsing HostAndPort from \""
                                    << str::escape(std::string{s}) << "\"");
    }

    int port;
    if (colonPos != std::string::npos) {
        const StringData portPart = s.substr(colonPos + 1);
        Status status = NumberParser().base(10)(portPart, &port);
        if (!status.isOK()) {
            return status;
        }
        if (port <= 0 || port > 65535) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Port number " << port
                                        << " out of range parsing HostAndPort from \""
                                        << str::escape(std::string{s}) << "\"");
        }
    } else {
        port = -1;
    }
    _host = std::string{hostPart};
    _port = port;
    return Status::OK();
}

}  // namespace mongo
