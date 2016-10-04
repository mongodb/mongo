/**
 *    Copyright (C) 2009-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/connection_string.h"

#include "mongo/base/status_with.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

ConnectionString::ConnectionString(const HostAndPort& server) : _type(MASTER) {
    _servers.push_back(server);
    _finishInit();
}

ConnectionString::ConnectionString(StringData setName, std::vector<HostAndPort> servers)
    : _type(SET), _servers(std::move(servers)), _setName(setName.toString()) {
    _finishInit();
}

// TODO: unify c-tors
ConnectionString::ConnectionString(ConnectionType type,
                                   const std::string& s,
                                   const std::string& setName) {
    _type = type;
    _setName = setName;
    _fillServers(s);
    _finishInit();
}

ConnectionString::ConnectionString(ConnectionType type,
                                   std::vector<HostAndPort> servers,
                                   const std::string& setName)
    : _type(type), _servers(std::move(servers)), _setName(setName) {
    _finishInit();
}

ConnectionString::ConnectionString(const std::string& s, ConnectionType connType)
    : _type(connType) {
    _fillServers(s);
    _finishInit();
}

ConnectionString::ConnectionString(ConnectionType connType) : _type(connType), _string("<local>") {
    invariant(_type == LOCAL);
}

ConnectionString ConnectionString::forReplicaSet(StringData setName,
                                                 std::vector<HostAndPort> servers) {
    return ConnectionString(setName, std::move(servers));
}

ConnectionString ConnectionString::forLocal() {
    return ConnectionString(LOCAL);
}

// TODO: rewrite parsing  make it more reliable
void ConnectionString::_fillServers(std::string s) {
    //
    // Custom-handled servers/replica sets start with '$'
    // According to RFC-1123/952, this will not overlap with valid hostnames
    // (also disallows $replicaSetName hosts)
    //

    if (s.find('$') == 0) {
        _type = CUSTOM;
    }

    std::string::size_type idx = s.find('/');
    if (idx != std::string::npos) {
        _setName = s.substr(0, idx);
        s = s.substr(idx + 1);
        if (_type != CUSTOM)
            _type = SET;
    }

    while ((idx = s.find(',')) != std::string::npos) {
        _servers.push_back(HostAndPort(s.substr(0, idx)));
        s = s.substr(idx + 1);
    }

    _servers.push_back(HostAndPort(s));

    if (_servers.size() == 1 && _type == INVALID) {
        _type = MASTER;
    }
}

void ConnectionString::_finishInit() {
    switch (_type) {
        case MASTER:
            uassert(ErrorCodes::FailedToParse,
                    "Cannot specify a replica set name for a ConnectionString of type MASTER",
                    _setName.empty());
            uassert(ErrorCodes::FailedToParse,
                    "ConnectionStrings of type MASTER must contain exactly one server",
                    _servers.size() == 1);
            break;
        case SET:
            uassert(ErrorCodes::FailedToParse,
                    "Must specify set name for replica set ConnectionStrings",
                    !_setName.empty());
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
    if (_type == MASTER && _servers.size() > 0) {
        if (_servers[0].host().find('$') == 0) {
            _type = CUSTOM;
        }
    }

    std::stringstream ss;

    if (_type == SET) {
        ss << _setName << "/";
    }

    for (unsigned i = 0; i < _servers.size(); i++) {
        if (i > 0) {
            ss << ",";
        }

        ss << _servers[i].toString();
    }

    _string = ss.str();
}

bool ConnectionString::operator==(const ConnectionString& other) const {
    if (_type != other._type) {
        return false;
    }

    switch (_type) {
        case INVALID:
            return true;
        case MASTER:
            return _servers[0] == other._servers[0];
        case SET:
            return _setName == other._setName && _servers == other._servers;
        case CUSTOM:
            return _string == other._string;
        case LOCAL:
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
        return ConnectionString(SET, url.substr(i + 1), url.substr(0, i));
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

std::string ConnectionString::typeToString(ConnectionType type) {
    switch (type) {
        case INVALID:
            return "invalid";
        case MASTER:
            return "master";
        case SET:
            return "set";
        case CUSTOM:
            return "custom";
        case LOCAL:
            return "local";
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
