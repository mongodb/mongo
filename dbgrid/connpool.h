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
#include "../db/dbclient.h"

struct PoolForHost { 
    queue<DBClientConnection*> pool;
};

class DBConnectionPool { 
    map<string,PoolForHost*> pools;
public:
    DBClientConnection *get(const string& host);
    void release(const string& host, DBClientConnection *c) { 
        pools[host]->pool.push(c);
    }
};

extern DBConnectionPool pool;

class ScopedDbConnection { 
    const string host; 
    DBClientConnection *_conn;
public:
    DBClientConnection& conn() { return *_conn; }

    /* throws UserAssertionAcception if can't connect */
    ScopedDbConnection(const string& _host) : 
      host(_host), _conn( pool.get(_host) ) { }

    ~ScopedDbConnection() { pool.release(host, _conn); }
};
