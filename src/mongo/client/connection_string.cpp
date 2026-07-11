// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <set>
#include <string_view>
#include <utility>

namespace mongo {

ConnectionString::ConnectionString(HostAndPort server) : _type(ConnectionType::kStandalone) {
    _servers.push_back(std::move(server));
    _finishInit();
}

ConnectionString::ConnectionString(std::string_view replicaSetName,
                                   std::vector<HostAndPort> servers)
    : _type(ConnectionType::kReplicaSet),
      _servers(std::move(servers)),
      _replicaSetName(std::string{replicaSetName}) {
    _finishInit();
}

// TODO: unify c-tors
ConnectionString::ConnectionString(ConnectionType type, std::string s, std::string replicaSetName)
    : _type(type), _replicaSetName(std::move(replicaSetName)) {
    _fillServers(std::move(s));
    _finishInit();
}

ConnectionString::ConnectionString(ConnectionType type,
                                   std::vector<HostAndPort> servers,
                                   std::string replicaSetName)
    : _type(type), _servers(std::move(servers)), _replicaSetName(std::move(replicaSetName)) {
    _finishInit();
}

ConnectionString::ConnectionString(std::string s, ConnectionType connType) : _type(connType) {
    _fillServers(std::move(s));
    _finishInit();
}

ConnectionString::ConnectionString(ConnectionType connType) : _type(connType), _string("<local>") {
    invariant(_type == ConnectionType::kLocal);
}

ConnectionString ConnectionString::forReplicaSet(std::string_view replicaSetName,
                                                 std::vector<HostAndPort> servers) {
    return ConnectionString(replicaSetName, std::move(servers));
}

ConnectionString ConnectionString::forStandalones(std::vector<HostAndPort> servers) {
    return ConnectionString(ConnectionType::kStandalone, std::move(servers), "");
}

ConnectionString ConnectionString::forLocal() {
    return ConnectionString(ConnectionType::kLocal);
}

// TODO: rewrite parsing  make it more reliable
void ConnectionString::_fillServers(std::string s) {
    //
    // Custom-handled servers/replica sets start with '$'
    // According to RFC-1123/952, this will not overlap with valid hostnames
    // (also disallows $replicaSetName hosts)
    //

    if (s.find('$') == 0) {
        _type = ConnectionType::kCustom;
    }

    std::string::size_type idx = s.find('/');
    if (idx != std::string::npos) {
        _replicaSetName = s.substr(0, idx);
        s = s.substr(idx + 1);
        if (_type != ConnectionType::kCustom)
            _type = ConnectionType::kReplicaSet;
    }

    while ((idx = s.find(',')) != std::string::npos) {
        _servers.push_back(HostAndPort(s.substr(0, idx)));
        s = s.substr(idx + 1);
    }

    _servers.push_back(HostAndPort(s));

    if (_servers.size() == 1 && _type == ConnectionType::kInvalid) {
        _type = ConnectionType::kStandalone;
    }
}

void ConnectionString::_finishInit() {
    switch (_type) {
        case ConnectionType::kStandalone:
            uassert(ErrorCodes::FailedToParse,
                    "Cannot specify a replica set name for a standalone ConnectionString",
                    _replicaSetName.empty());
            break;
        case ConnectionType::kReplicaSet:
            uassert(ErrorCodes::FailedToParse,
                    "Must specify set name for replica set ConnectionStrings",
                    !_replicaSetName.empty());
            uassert(ErrorCodes::FailedToParse,
                    "Replica set ConnectionStrings must have at least one server specified",
                    _servers.size() >= 1);
            break;
        default:
            uassert(ErrorCodes::FailedToParse,
                    "ConnectionStrings must specify at least one server",
                    _servers.size() > 0);
    }

    // Needed here as well b/c the parsing logic isn't used in all constructors
    // TODO: Refactor so that the parsing logic *is* used in all constructors
    if (_type == ConnectionType::kStandalone && _servers.size() > 0) {
        if (_servers[0].host().find('$') == 0) {
            _type = ConnectionType::kCustom;
        }
    }

    std::stringstream ss;

    if (_type == ConnectionType::kReplicaSet) {
        ss << _replicaSetName << "/";
    }

    for (unsigned i = 0; i < _servers.size(); i++) {
        if (i > 0) {
            ss << ",";
        }

        ss << _servers[i].toString();
    }

    _string = ss.str();
}

ConnectionString ConnectionString::makeUnionWith(const ConnectionString& other) {
    invariant(type() == other.type());
    invariant(getSetName() == other.getSetName());
    std::set<HostAndPort> servers{_servers.begin(), _servers.end()};
    servers.insert(other._servers.begin(), other._servers.end());
    return ConnectionString(
        type(), std::vector<HostAndPort>(servers.begin(), servers.end()), getSetName());
}

bool ConnectionString::operator==(const ConnectionString& other) const {
    if (_type != other._type) {
        return false;
    }

    switch (_type) {
        case ConnectionType::kInvalid:
            return true;
        case ConnectionType::kStandalone:
            return _servers[0] == other._servers[0];
        case ConnectionType::kReplicaSet:
            return _replicaSetName == other._replicaSetName && _servers == other._servers;
        case ConnectionType::kCustom:
            return _string == other._string;
        case ConnectionType::kLocal:
            return true;
    }

    MONGO_UNREACHABLE;
}

bool ConnectionString::operator!=(const ConnectionString& other) const {
    return !(*this == other);
}

StatusWith<ConnectionString> ConnectionString::parse(const std::string& url) {
    const std::string::size_type i = url.find('/');

    // Replica set
    if (i != std::string::npos && i != 0) {
        return ConnectionString(ConnectionType::kReplicaSet, url.substr(i + 1), url.substr(0, i));
    }

    const int numCommas = str::count(url, ',');

    // Single host
    if (numCommas == 0) {
        HostAndPort singleHost;
        Status status = singleHost.initialize(url);
        if (!status.isOK()) {
            return status;
        }

        return ConnectionString(std::move(singleHost));
    }

    if (numCommas == 2) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "mirrored config server connections are not supported; for "
                                       "config server replica sets be sure to use the replica set "
                                       "connection string");
    }

    return Status(ErrorCodes::FailedToParse, str::stream() << "invalid url [" << url << "]");
}

ConnectionString ConnectionString::deserialize(std::string_view url) {
    return uassertStatusOK(parse(std::string{url}));
}

std::string ConnectionString::typeToString(ConnectionType type) {
    switch (type) {
        case ConnectionType::kInvalid:
            return "invalid";
        case ConnectionType::kStandalone:
            return "standalone";
        case ConnectionType::kReplicaSet:
            return "replicaSet";
        case ConnectionType::kCustom:
            return "custom";
        case ConnectionType::kLocal:
            return "local";
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
