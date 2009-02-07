/* connpool.cpp
*/

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

// _ todo: reconnect?

#include "stdafx.h"
#include "connpool.h"

namespace mongo {

    DBConnectionPool pool;

    DBClientBase* DBConnectionPool::get(const string& host) {
        boostlock L(poolMutex);

        PoolForHost *&p = pools[host];
        if ( p == 0 )
            p = new PoolForHost();
        if ( p->pool.empty() ) {
            string errmsg;
            DBClientBase *c;
            if( host.find(',') == string::npos ) {
                DBClientConnection *cc = new DBClientConnection(true);
                if ( !cc->connect(host.c_str(), errmsg) ) {
                    delete cc;
                    uassert( (string)"dbconnectionpool: connect failed" + host , false);
                    return 0;
                }
                c = cc;
            }
            else { 
                DBClientPaired *p = new DBClientPaired();
                if( !p->connect(host) ) { 
                    delete p;
                    uassert( (string)"dbconnectionpool: connect failed [2] " + host , false);
                    return 0;
                }
                c = p;
            }
            return c;
        }
        DBClientBase *c = p->pool.front();
        p->pool.pop();
        return c;
    }

} // namespace mongo
