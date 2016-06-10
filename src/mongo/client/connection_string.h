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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class DBClientBase;

/**
 * ConnectionString handles parsing different ways to connect to mongo and determining method
 * samples:
 *    server
 *    server:port
 *    foo/server:port,server:port   SET
 *
 * Typical use:
 *
 * ConnectionString cs(uassertStatusOK(ConnectionString::parse(url)));
 * std::string errmsg;
 * DBClientBase * conn = cs.connect( errmsg );
 */
class ConnectionString {
public:
    enum ConnectionType { INVALID, MASTER, SET, CUSTOM, LOCAL };

    ConnectionString() = default;

    /**
     * Constructs a connection string representing a replica set.
     */
    static ConnectionString forReplicaSet(StringData setName, std::vector<HostAndPort> servers);

    /**
     * Constructs a local connection string.
     */
    static ConnectionString forLocal();

    /**
     * Creates a MASTER connection string with the specified server.
     */
    explicit ConnectionString(const HostAndPort& server);

    /**
     * Creates a connection string from an unparsed list of servers, type, and setName.
     */
    ConnectionString(ConnectionType type, const std::string& s, const std::string& setName);

    /**
     * Creates a connection string from a pre-parsed list of servers, type, and setName.
     */
    ConnectionString(ConnectionType type,
                     std::vector<HostAndPort> servers,
                     const std::string& setName);

    ConnectionString(const std::string& s, ConnectionType connType);

    bool isValid() const {
        return _type != INVALID;
    }

    const std::string& toString() const {
        return _string;
    }

    const std::string& getSetName() const {
        return _setName;
    }

    const std::vector<HostAndPort>& getServers() const {
        return _servers;
    }

    ConnectionType type() const {
        return _type;
    }

    /**
     * Returns true if two connection strings match in terms of their type and the exact order of
     * their hosts.
     */
    bool operator==(const ConnectionString& other) const;
    bool operator!=(const ConnectionString& other) const;

    DBClientBase* connect(std::string& errmsg, double socketTimeout = 0) const;

    static StatusWith<ConnectionString> parse(const std::string& url);

    static std::string typeToString(ConnectionType type);

    //
    // Allow overriding the default connection behavior
    // This is needed for some tests, which otherwise would fail because they are unable to contact
    // the correct servers.
    //

    class ConnectionHook {
    public:
        virtual ~ConnectionHook() {}

        // Returns an alternative connection object for a string
        virtual DBClientBase* connect(const ConnectionString& c,
                                      std::string& errmsg,
                                      double socketTimeout) = 0;
    };

    static void setConnectionHook(ConnectionHook* hook) {
        stdx::lock_guard<stdx::mutex> lk(_connectHookMutex);
        _connectHook = hook;
    }

    static ConnectionHook* getConnectionHook() {
        stdx::lock_guard<stdx::mutex> lk(_connectHookMutex);
        return _connectHook;
    }

    // Allows ConnectionStrings to be stored more easily in sets/maps
    bool operator<(const ConnectionString& other) const {
        return _string < other._string;
    }

private:
    /**
     * Creates a SET connection string with the specified set name and servers.
     */
    ConnectionString(StringData setName, std::vector<HostAndPort> servers);

    /**
     * Creates a connection string with the specified type. Used for creating LOCAL strings.
     */
    explicit ConnectionString(ConnectionType connType);

    void _fillServers(std::string s);
    void _finishInit();

    ConnectionType _type{INVALID};
    std::vector<HostAndPort> _servers;
    std::string _string;
    std::string _setName;

    static stdx::mutex _connectHookMutex;
    static ConnectionHook* _connectHook;
};
}  // namespace mongo
