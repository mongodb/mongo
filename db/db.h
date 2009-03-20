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
#include "../util/message.h"
#include "../util/top.h"

namespace mongo {

    void jniCallback(Message& m, Message& out);

    class MutexInfo {
        unsigned long long start, enter, timeLocked; // all in microseconds
        int locked;

    public:
        MutexInfo() : locked(0) {
            start = curTimeMicros64();
        }
        void entered() {
            enter = curTimeMicros64();
            locked++;
            assert( locked == 1 );
        }
        void leaving() {
            locked--;
            assert( locked == 0 );
            timeLocked += curTimeMicros64() - enter;
        }
        int isLocked() const {
            return locked;
        }
        void timingInfo(unsigned long long &s, unsigned long long &tl) {
            s = start;
            tl = timeLocked;
        }
    };

    extern boost::mutex &dbMutex;
    extern MutexInfo dbMutexInfo;

    struct lock {
        boostlock bl_;
        MutexInfo& info_;
        lock( boost::mutex &mutex, MutexInfo &info ) :
                bl_( mutex ),
                info_( info ) {
            info_.entered();
        }
        ~lock() {
            info_.leaving();
        }
    };

    void dbunlocking();

    struct dblock : public lock {
        dblock() :
                lock( dbMutex, dbMutexInfo ) {
        }
        ~dblock() { 
            /* todo: this should be inlined */
            dbunlocking();
            Top::clientStop();
        }
    };

} // namespace mongo

#include "boost/version.hpp"

namespace mongo {

    /* a scoped release of a mutex temporarily -- like a scopedlock but reversed.
    */
    struct temprelease {
        boost::mutex& m;
        temprelease(boost::mutex& _m) : m(_m) {
#if BOOST_VERSION >= 103500
            m.unlock();
#else
            boost::detail::thread::lock_ops<boost::mutex>::unlock(m);
#endif
        }
        ~temprelease() {
#if BOOST_VERSION >= 103500
            m.lock();
#else
            boost::detail::thread::lock_ops<boost::mutex>::lock(m);
#endif
        }
    };

} // namespace mongo

#include "pdfile.h"

namespace mongo {

// tempish...move to TLS or pass all the way down as a parm
    extern map<string,Database*> databases;
    extern Database *database;
    extern const char *curNs;
    extern bool master;

    inline string getKey( const char *ns, const char *path ) {
        char cl[256];
        nsToClient(ns, cl);
        return string( cl ) + ":" + path;
    }

    /* returns true if the database ("database") did not exist, and it was created on this call */
    inline bool setClient(const char *ns, const char *path=dbpath) {
        /* we must be in critical section at this point as these are global
           variables.
        */
        assert( dbMutexInfo.isLocked() );
        Top::clientStart( ns );
        
        curNs = ns;
        string key = getKey( ns, path );
        map<string,Database*>::iterator it = databases.find(key);
        if ( it != databases.end() ) {
            database = it->second;
            return false;
        }

        // when master for replication, we advertise all the db's, and that
        // looks like a 'first operation'. so that breaks this log message's
        // meaningfulness.  instead of fixing (which would be better), we just
        // stop showing for now.
        // 2008-12-22 We now open every database on startup, so this log is
        // no longer helpful.  Commenting.
//    if( !master )
//        log() << "first operation for database " << key << endl;

        char cl[256];
        nsToClient(ns, cl);
        bool justCreated;
        Database *c = new Database(cl, justCreated, path);
        databases[key] = c;
        database = c;
        database->finishInit();

        return justCreated;
    }

// shared functionality for removing references to a database from this program instance
// does not delete the files on disk
    void closeClient( const char *cl, const char *path = dbpath );

    /* remove database from the databases map */
    inline void eraseDatabase( const char *ns, const char *path=dbpath ) {
        string key = getKey( ns, path );
        databases.erase( key );
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
        string clientpath;
        dbtemprelease() {
            if ( database ) {
                clientname = database->name;
                clientpath = database->path;
            }
            Top::clientStop();
            dbMutexInfo.leaving();
#if BOOST_VERSION >= 103500
            dbMutex.unlock();
#else
            boost::detail::thread::lock_ops<boost::mutex>::unlock(dbMutex);
#endif
        }
        ~dbtemprelease() {
#if BOOST_VERSION >= 103500
            dbMutex.lock();
#else
            boost::detail::thread::lock_ops<boost::mutex>::lock(dbMutex);
#endif
            dbMutexInfo.entered();
            if ( clientname.empty() )
                database = 0;
            else
                setClient(clientname.c_str(), clientpath.c_str());
        }
    };

} // namespace mongo

#include "dbinfo.h"
#include "concurrency.h"
