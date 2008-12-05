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

#include "../stdafx.h"
#include "../grid/message.h"

void jniCallback(Message& m, Message& out);

class MutexInfo { 
    unsigned long long start, last, enter, timeLocked; // all in microseconds
    int locked;

public:
    MutexInfo() : locked(0) { 
        start = last = curTimeMicros64();
        last = 0;
    }
    void entered() { 
        enter = curTimeMicros64();
        locked++;
        assert( locked == 1 );
    }
    void leaving() { 
        locked--;
        assert( locked == 0 );
        last = curTimeMicros64();
        timeLocked += last - enter;
    }
    int isLocked() const { return locked; }
    void timingInfo(unsigned long long &s, unsigned long long &l, unsigned long long &tl) { 
        s = start; l = last; tl = timeLocked;
    }
};

extern boost::mutex dbMutex;
extern MutexInfo dbMutexInfo;
//extern int dbLocked;

struct dblock { 
    boostlock bl;
    dblock() : bl(dbMutex) { 
        dbMutexInfo.entered();
    }
	~dblock() { 
        dbMutexInfo.leaving();
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

// tempish...move to TLS or pass all the way down as a parm
extern map<string,Database*> databases;
extern Database *database;
extern const char *curNs;
extern bool master;

/* returns true if the database ("database") did not exist, and it was created on this call */
inline bool setClient(const char *ns) { 
    /* we must be in critical section at this point as these are global 
       variables. 
    */
    assert( dbMutexInfo.isLocked() );

	char cl[256];
	curNs = ns;
	nsToClient(ns, cl);
	map<string,Database*>::iterator it = databases.find(cl);
	if( it != databases.end() ) {
		database = it->second;
		return false;
	}

    // when master for replication, we advertise all the db's, and that 
    // looks like a 'first operation'. so that breaks this log message's 
    // meaningfulness.  instead of fixing (which would be better), we just
    // stop showing for now.
    if( !master )
        log() << "first operation for database " << cl << endl;

	bool justCreated;
	Database *c = new Database(cl, justCreated);
	databases[cl] = c;
	database = c;
    database->finishInit();
	return justCreated;
}

/* We normally keep around a curNs ptr -- if this ns is temporary, 
   use this instead so we don't have a bad ptr.  we could have made a copy,
   but trying to be fast as we call setClient this for every single operation.
*/
inline bool setClientTempNs(const char *ns) { 
	bool jc = setClient(ns); 
	curNs = "";
	return jc;
}

struct dbtemprelease {
    string clientname;
    dbtemprelease() {
        if( database ) 
            clientname = database->name;
        dbMutexInfo.leaving();
        boost::detail::thread::lock_ops<boost::mutex>::unlock(dbMutex);
    }
    ~dbtemprelease() { 
        boost::detail::thread::lock_ops<boost::mutex>::lock(dbMutex);
        dbMutexInfo.entered();
        if( clientname.empty() )
            database = 0;
        else
            setClient(clientname.c_str());
    }
};

#include "dbinfo.h"
