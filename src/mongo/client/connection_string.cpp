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
        : _type(SET),
          _servers(std::move(servers)),
          _setName(setName.toString()) {

        _finishInit();
    }

    ConnectionString::ConnectionString(ConnectionType type,
                                       const std::string& s,
                                       const std::string& setName) {
        _type = type;
        _setName = setName;
        _fillServers(s);

        switch (_type) {
        case MASTER:
            verify(_servers.size() == 1);
            break;
        case SET:
            verify(_setName.size());
            verify(_servers.size() >= 1); // 1 is ok since we can derive
            break;
        default:
            verify(_servers.size() > 0);
        }

        _finishInit();
    }

    ConnectionString::ConnectionString(const std::string& s, ConnectionType favoredMultipleType) {
        _fillServers(s);

        if (_type != INVALID) {
            // set already
        }
        else if (_servers.size() == 1) {
            _type = MASTER;
        }
        else {
            _type = favoredMultipleType;
            verify(_type == SET || _type == SYNC);
        }

        _finishInit();
    }

    ConnectionString ConnectionString::forReplicaSet(StringData setName,
                                                     std::vector<HostAndPort> servers) {
        return ConnectionString(setName, std::move(servers));
    }

    void ConnectionString::_fillServers( std::string s ) {

        //
        // Custom-handled servers/replica sets start with '$'
        // According to RFC-1123/952, this will not overlap with valid hostnames
        // (also disallows $replicaSetName hosts)
        //

        if( s.find( '$' ) == 0 ) _type = CUSTOM;

        {
            std::string::size_type idx = s.find( '/' );
            if ( idx != std::string::npos ) {
                _setName = s.substr( 0 , idx );
                s = s.substr( idx + 1 );
                if( _type != CUSTOM ) _type = SET;
            }
        }

        std::string::size_type idx;
        while ( ( idx = s.find( ',' ) ) != std::string::npos ) {
            _servers.push_back(HostAndPort(s.substr(0, idx)));
            s = s.substr( idx + 1 );
        }
        _servers.push_back(HostAndPort(s));

    }

    void ConnectionString::_finishInit() {
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

    bool ConnectionString::sameLogicalEndpoint(const ConnectionString& other) const {
        if (_type != other._type) {
            return false;
        }

        switch (_type) {
        case INVALID:
            return true;
        case MASTER:
            return _servers[0] == other._servers[0];
        case SET:
            return _setName == other._setName;
        case SYNC:
            // The servers all have to be the same in each, but not in the same order.
            if (_servers.size() != other._servers.size()) {
                return false;
            }

            for (unsigned i = 0; i < _servers.size(); i++) {
                bool found = false;
                for (unsigned j = 0; j < other._servers.size(); j++) {
                    if (_servers[i] == other._servers[j]) {
                        found = true;
                        break;
                    }
                }

                if (!found) return false;
            }

            return true;
        case CUSTOM:
            return _string == other._string;
        }

        MONGO_UNREACHABLE;
    }

    ConnectionString ConnectionString::parse(const std::string& url, std::string& errmsg) {
        auto status = parse(url);
        if (status.isOK()) {
            errmsg = "";
            return status.getValue();
        }

        errmsg = status.getStatus().toString();
        return ConnectionString();
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

        // Sharding config server
        if (numCommas == 2) {
            return ConnectionString(SYNC, url, "");
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
        case SYNC:
            return "sync";
        case CUSTOM:
            return "custom";
        }

        MONGO_UNREACHABLE;
    }

} // namespace mongo
