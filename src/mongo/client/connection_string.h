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

#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>
#include <string>
#include <vector>

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
     *    server,server,server          SYNC
     *                                    Warning - you usually don't want "SYNC", it's used
     *                                    for some special things such as sharding config servers.
     *                                    See syncclusterconnection.h for more info.
     *
     * tyipcal use
     * std::string errmsg,
     * ConnectionString cs = ConnectionString::parse( url , errmsg );
     * if ( ! cs.isValid() ) throw "bad: " + errmsg;
     * DBClientBase * conn = cs.connect( errmsg );
     */
    class ConnectionString {
    public:
        enum ConnectionType { INVALID, MASTER, SET, SYNC, CUSTOM };

        ConnectionString() {
            _type = INVALID;
        }

        // Note: This should only be used for direct connections to a single server.  For replica
        // set and SyncClusterConnections, use ConnectionString::parse.
        ConnectionString( const HostAndPort& server ) {
            _type = MASTER;
            _servers.push_back( server );
            _finishInit();
        }

        ConnectionString( ConnectionType type , const std::string& s , const std::string& setName = "" ) {
            _type = type;
            _setName = setName;
            _fillServers( s );

            switch ( _type ) {
            case MASTER:
                verify( _servers.size() == 1 );
                break;
            case SET:
                verify( _setName.size() );
                verify( _servers.size() >= 1 ); // 1 is ok since we can derive
                break;
            default:
                verify( _servers.size() > 0 );
            }

            _finishInit();
        }

        ConnectionString( const std::string& s , ConnectionType favoredMultipleType ) {
            _type = INVALID;

            _fillServers( s );
            if ( _type != INVALID ) {
                // set already
            }
            else if ( _servers.size() == 1 ) {
                _type = MASTER;
            }
            else {
                _type = favoredMultipleType;
                verify( _type == SET || _type == SYNC );
            }
            _finishInit();
        }

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

        static ConnectionString parse( const std::string& url , std::string& errmsg );

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
            boost::lock_guard<boost::mutex> lk( _connectHookMutex );
            _connectHook = hook;
        }

        static ConnectionHook* getConnectionHook() {
            boost::lock_guard<boost::mutex> lk( _connectHookMutex );
            return _connectHook;
        }

        // Allows ConnectionStrings to be stored more easily in sets/maps
        bool operator<(const ConnectionString& other) const {
            return _string < other._string;
        }

        //
        // FOR TESTING ONLY - useful to be able to directly mock a connection std::string without
        // including the entire client library.
        //

        static ConnectionString mock( const HostAndPort& server ) {
            ConnectionString connStr;
            connStr._servers.push_back( server );
            connStr._string = server.toString();
            return connStr;
        }

    private:

        void _fillServers( std::string s );
        void _finishInit();

        ConnectionType _type;
        std::vector<HostAndPort> _servers;
        std::string _string;
        std::string _setName;

        static boost::mutex _connectHookMutex;
        static ConnectionHook* _connectHook;
    };
} // namespace mongo
