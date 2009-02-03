/* connpool.h */

/**
*    Copyright (C) 2008 10gen Inc.
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
*/

#pragma once

#include <queue>
#include "dbclient.h"

namespace mongo {

    struct PoolForHost {
        std::queue<DBClientConnection*> pool;
    };

    /** Database connection pool.
        Generally, use ScopedDbConnection and do not call these directly.

        This class, so far, is suitable for use with unauthenticated connections. 
        Support for authenticated connections requires some adjustements: please 
        request...

        Usage:

        {
           ScopedDbConnection c("myserver");
           c.conn()...
        }
    */
    class DBConnectionPool {
        boost::mutex poolMutex;
        map<string,PoolForHost*> pools; // servername -> pool
    public:
        DBClientConnection *get(const string& host);
        void release(const string& host, DBClientConnection *c) {
            boostlock L(poolMutex);
            pools[host]->pool.push(c);
        }
    };

    extern DBConnectionPool pool;

    /** Use to get a connection from the pool.  On exceptions things
       clean up nicely.
    */
    class ScopedDbConnection {
        const string host;
        DBClientConnection *_conn;
    public:
        DBClientConnection& conn() {
            uassert( "did you call done already" , _conn );
            return *_conn;
        }

        /* throws UserAssertionAcception if can't connect */
        ScopedDbConnection(const string& _host) :
                host(_host), _conn( pool.get(_host) ) { }

        /* Force closure of the connection.  You should call this if you leave it in
           a bad state.  Destructor will do this too, but it is verbose.
        */
        void kill() {
            delete _conn;
            _conn = 0;
        }

        /* Call this when you are done with the ocnnection.
             Why?  See note in the destructor below.
        */
        void done() {
            if ( ! _conn )
                return;

            if ( _conn->isFailed() )
                kill();
            else
                pool.release(host, _conn);
            _conn = 0;
        }

        ~ScopedDbConnection() {
            if ( _conn ) {
                /* you are supposed to call done().  if you did that, correctly, we
                   only get here if an exception was thrown.  in such a scenario, we can't
                   be sure we fully read all expected data of a reply on the socket.  so
                   we don't try to reuse the connection.  The out() is just informational.
                   */
                out() << "~ScopedDBConnection: _conn != null\n";
                kill();
            }
        }
    };

} // namespace mongo
