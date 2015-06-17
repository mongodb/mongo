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

#include "mongo/base/string_data.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

    class DBClientBase;
    template <typename T> class StatusWith;

    /**
     * ConnectionString handles parsing different ways to connect to mongo and determining method
     * samples:
     *    server
     *    server:port
     *    foo/server:port,server:port   SET
     *    server,server,server          SYNC
     *                                    Warning - you usually don't want "SYNC", it's used
     *                                    for some special things such as sharding config servers.
     *                                    See syncclusterconnection.h for more info.
     *
     * Typical use:
     *
     * ConnectionString cs(uassertStatusOK(ConnectionString::parse(url)));
     * std::string errmsg;
     * DBClientBase * conn = cs.connect( errmsg );
     */
    class ConnectionString {
    public:
        enum ConnectionType { INVALID, MASTER, SET, SYNC, CUSTOM };

        ConnectionString() = default;

        /**
         * Constructs a connection string representing a replica set.
         */
        static ConnectionString forReplicaSet(StringData setName,
                                              std::vector<HostAndPort> servers);

        /**
         * Creates a MASTER connection string with the specified server.
         */
        explicit ConnectionString(const HostAndPort& server);

        ConnectionString(ConnectionType type, const std::string& s, const std::string& setName);

        ConnectionString(const std::string& s, ConnectionType favoredMultipleType);

        bool isValid() const { return _type != INVALID; }

        const std::string& toString() const { return _string; }

        const std::string& getSetName() const { return _setName; }

        const std::vector<HostAndPort>& getServers() const { return _servers; }

        ConnectionType type() const { return _type; }

        /**
         * This returns true if this and other point to the same logical entity.
         * For single nodes, thats the same address.
         * For replica sets, thats just the same replica set name.
         * For pair (deprecated) or sync cluster connections, that's the same hosts in any ordering.
         */
        bool sameLogicalEndpoint( const ConnectionString& other ) const;

        DBClientBase* connect(std::string& errmsg, double socketTimeout = 0) const;

        static ConnectionString parse(const std::string& url, std::string& errmsg);
        static StatusWith<ConnectionString> parse(const std::string& url);

        static std::string typeToString( ConnectionType type );

        //
        // Allow overriding the default connection behavior
        // This is needed for some tests, which otherwise would fail because they are unable to contact
        // the correct servers.
        //

        class ConnectionHook {
        public:
            virtual ~ConnectionHook(){}

            // Returns an alternative connection object for a string
            virtual DBClientBase* connect( const ConnectionString& c,
                                           std::string& errmsg,
                                           double socketTimeout ) = 0;
        };

        static void setConnectionHook( ConnectionHook* hook ){
            stdx::lock_guard<stdx::mutex> lk( _connectHookMutex );
            _connectHook = hook;
        }

        static ConnectionHook* getConnectionHook() {
            stdx::lock_guard<stdx::mutex> lk( _connectHookMutex );
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


        void _fillServers( std::string s );
        void _finishInit();

        ConnectionType _type{INVALID};
        std::vector<HostAndPort> _servers;
        std::string _string;
        std::string _setName;

        static stdx::mutex _connectHookMutex;
        static ConnectionHook* _connectHook;
    };
} // namespace mongo
