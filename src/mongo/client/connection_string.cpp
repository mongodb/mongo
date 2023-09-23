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

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <set>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

ConnectionString::ConnectionString(const HostAndPort& server) : _type(ConnectionType::kStandalone) {
    _servers.push_back(server);
    _finishInit();
}

ConnectionString::ConnectionString(StringData replicaSetName, std::vector<HostAndPort> servers)
    : _type(ConnectionType::kReplicaSet),
      _servers(std::move(servers)),
      _replicaSetName(replicaSetName.toString()) {
    _finishInit();
}

// TODO: unify c-tors
ConnectionString::ConnectionString(ConnectionType type,
                                   const std::string& s,
                                   const std::string& replicaSetName) {
    _type = type;
    _replicaSetName = replicaSetName;
    _fillServers(s);
    _finishInit();
}

ConnectionString::ConnectionString(ConnectionType type,
                                   std::vector<HostAndPort> servers,
                                   const std::string& replicaSetName)
    : _type(type), _servers(std::move(servers)), _replicaSetName(replicaSetName) {
    _finishInit();
}

ConnectionString::ConnectionString(const std::string& s, ConnectionType connType)
    : _type(connType) {
    _fillServers(s);
    _finishInit();
}

ConnectionString::ConnectionString(ConnectionType connType) : _type(connType), _string("<local>") {
    invariant(_type == ConnectionType::kLocal);
}

ConnectionString ConnectionString::forReplicaSet(StringData replicaSetName,
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

        return ConnectionString(singleHost);
    }

    if (numCommas == 2) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "mirrored config server connections are not supported; for "
                                       "config server replica sets be sure to use the replica set "
                                       "connection string");
    }

    return Status(ErrorCodes::FailedToParse, str::stream() << "invalid url [" << url << "]");
}

ConnectionString ConnectionString::deserialize(StringData url) {
    return uassertStatusOK(parse(url.toString()));
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
