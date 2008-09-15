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

#include "../stdafx.h"
#include "../grid/message.h"

void jniCallback(Message& m, Message& out);

extern boost::mutex dbMutex;
extern int dbLocked;

struct dblock { 
    boostlock bl;
    dblock() : bl(dbMutex) { 
        dbLocked++;
        assert( dbLocked == 1 );
    }
	~dblock() { 
        dbLocked--;
        assert( dbLocked == 0 );
    }
};

/* a scoped release of a mutex temporarily -- like a scopedlock but reversed. 
*/
struct temprelease {
    boost::mutex& m;
    temprelease(boost::mutex& _m) : m(_m) { 
        boost::detail::thread::lock_ops<boost::mutex>::unlock(m);
    }
    ~temprelease() { 
        boost::detail::thread::lock_ops<boost::mutex>::lock(m);
    }
};

#include "pdfile.h"

struct dbtemprelease {
    string clientname;
    dbtemprelease() {
        if( client ) 
            clientname = client->name;
        dbLocked--;
        assert( dbLocked == 0 );
        boost::detail::thread::lock_ops<boost::mutex>::unlock(dbMutex);
    }
    ~dbtemprelease() { 
        boost::detail::thread::lock_ops<boost::mutex>::lock(dbMutex);
        dbLocked++;
        assert( dbLocked == 1 );
        if( clientname.empty() )
            client = 0;
        else
            setClient(clientname.c_str());
    }
};
