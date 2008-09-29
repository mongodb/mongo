// connpool.cpp

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

DBClientConnection* DBConnectionPool::get(const string& host) { 
    PoolForHost *&p = pools[host];
    if( p == 0 )
        p = new PoolForHost();
    if( p->pool.empty() ) {
        string errmsg;
        DBClientConnection *c = new DBClientConnection();
        if( !c->connect(host.c_str(), errmsg) ) { 
            delete c;
            uassert("dbconnectionpool: connect failed", false);
            return 0;
        }
        return c;
    }
    DBClientConnection *c = p->pool.front();
    p->pool.pop();
    return c;
}
