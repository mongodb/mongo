// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/net/hostandport.h"

#include "mongo/base/parse_number.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/functional/hash.hpp>

namespace mongo {

StatusWith<HostAndPort> HostAndPort::parse(std::string_view text) {
    HostAndPort result;
    Status status = result.initialize(text);
    if (!status.isOK()) {
        return StatusWith<HostAndPort>(status);
    }
    return StatusWith<HostAndPort>(result);
}

Status validateHostAndPort(std::string_view hostAndPortStr, const boost::optional<TenantId>&) {
    if (hostAndPortStr.empty()) {
        return Status::OK();
    }
    return HostAndPort::parse(hostAndPortStr).getStatus();
}

HostAndPort::HostAndPort() : _port(-1) {}

HostAndPort::HostAndPort(std::string_view text) {
    uassertStatusOK(initialize(text));
}

HostAndPort::HostAndPort(std::string h, int p) : _host(std::move(h)), _port(p) {}

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
            isUds());
}

bool HostAndPort::isUds() const {
    return _host.c_str()[0] == '/' || _host == "anonymous unix socket";
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
    if (!isUds()) {
        write(":");
        write(port());
    }
}

bool HostAndPort::empty() const {
    return _host.empty() && _port < 0;
}

Status HostAndPort::initialize(std::string_view s) {
    if (s.empty()) {
        return Status(ErrorCodes::FailedToParse, "Cannot parse HostAndPort from empty string");
    }

    size_t colonPos = s.rfind(':');
    std::string_view hostPart = s.substr(0, colonPos);

    // handle ipv6 hostPart (which we require to be wrapped in []s)
    const size_t openBracketPos = s.find('[');
    const size_t closeBracketPos = s.find(']');
    if (openBracketPos != std::string::npos) {
        if (openBracketPos != 0) {
            return Status(ErrorCodes::FailedToParse,
                          fmt::format("'[' present, but not first character in {}", s));
        }
        if (closeBracketPos == std::string::npos) {
            return Status(ErrorCodes::FailedToParse,
                          fmt::format("ipv6 address is missing closing ']' in hostname in {}", s));
        }

        hostPart = s.substr(openBracketPos + 1, closeBracketPos - openBracketPos - 1);
        // prevent accidental assignment of port to the value of the final portion of hostPart
        if (colonPos < closeBracketPos) {
            // If the last colon is inside the brackets, then there must not be a port.
            if (s.size() != closeBracketPos + 1) {
                return Status(ErrorCodes::FailedToParse,
                              fmt::format("missing colon after ']' before the port in {}", s));
            }
            colonPos = std::string::npos;
        } else if (colonPos != closeBracketPos + 1) {
            return Status(
                ErrorCodes::FailedToParse,
                fmt::format("Extraneous characters between ']' and pre-port ':' in {}", s));
        }
    } else if (closeBracketPos != std::string::npos) {
        return Status(ErrorCodes::FailedToParse, fmt::format("']' present without '[' in {}", s));
    } else if (s.find(':') != colonPos) {
        return Status(ErrorCodes::FailedToParse,
                      fmt::format("More than one ':' detected. If this is an ipv6 address, it "
                                  "needs to be surrounded by '[' and ']'; {}",
                                  s));
    }

    if (hostPart.empty()) {
        return Status(ErrorCodes::FailedToParse,
                      fmt::format("Empty host component parsing HostAndPort from {}", s));
    }

    int port;
    if (colonPos != std::string::npos) {
        const std::string_view portPart = s.substr(colonPos + 1);
        Status status = NumberParser().base(10)(portPart, &port);
        if (!status.isOK()) {
            return status;
        }
        if (port <= 0 || port > 65535) {
            return Status(
                ErrorCodes::FailedToParse,
                fmt::format("Port number {} out of range parsing HostAndPort from {}", port, s));
        }
    } else {
        port = -1;
    }
    _host.assign(hostPart.data(), hostPart.size());
    _port = port;
    return Status::OK();
}

}  // namespace mongo
