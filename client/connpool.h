/** @file connpool.h */

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

#include <stack>
#include "dbclient.h"

namespace mongo {

    struct PoolForHost {
        std::stack<DBClientBase*> pool;
    };
    
    class DBConnectionHook {
    public:
        virtual ~DBConnectionHook(){}

        virtual void onCreate( DBClientBase * conn ){}
        virtual void onHandedOut( DBClientBase * conn ){}

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
        list<DBConnectionHook*> _hooks;
        
        void onCreate( DBClientBase * conn );
        void onHandedOut( DBClientBase * conn );
    public:
        void flush();
        DBClientBase *get(const string& host);
        void release(const string& host, DBClientBase *c) {
            if ( c->isFailed() )
                return;
            boostlock L(poolMutex);
            pools[host]->pool.push(c);
        }
        void addHook( DBConnectionHook * hook );
    };

    extern DBConnectionPool pool;

    /** Use to get a connection from the pool.  On exceptions things
       clean up nicely.
    */
    class ScopedDbConnection {
        const string host;
        DBClientBase *_conn;
    public:
        /** get the associated connection object */
        DBClientBase* operator->(){ 
            uassert( "did you call done already" , _conn );
            return _conn; 
        }

        /** get the associated connection object */
        DBClientBase& conn() {
            uassert( "did you call done already" , _conn );
            return *_conn;
        }

        /** throws UserException if can't connect */
        ScopedDbConnection(const string& _host) :
                host(_host), _conn( pool.get(_host) ) {
            //cout << " for: " << _host << " got conn: " << _conn << endl;
        }

        /** Force closure of the connection.  You should call this if you leave it in
            a bad state.  Destructor will do this too, but it is verbose.
        */
        void kill() {
            delete _conn;
            _conn = 0;
        }

        /** Call this when you are done with the connection.
            
            If you do not call done() before this object goes out of scope, 
            we can't be sure we fully read all expected data of a reply on the socket.  so
            we don't try to reuse the connection in that situation.
        */
        void done() {
            if ( ! _conn )
                return;

            /* we could do this, but instead of assume one is using autoreconnect mode on the connection 
            if ( _conn->isFailed() )
                kill();
            else
            */
                pool.release(host, _conn);
            _conn = 0;
        }

        ~ScopedDbConnection() {
            if ( _conn && ! _conn->isFailed() ) {
                /* see done() comments above for why we log this line */
                log() << "~ScopedDBConnection: _conn != null" << endl;
                kill();
            }
        }
    };

} // namespace mongo
