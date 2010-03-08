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
#include "boost/version.hpp"
#include "concurrency.h"
#include "pdfile.h"
#include "client.h"

namespace mongo {

//    void jniCallback(Message& m, Message& out);

    /* Note the limit here is rather arbitrary and is simply a standard. generally the code works
       with any object that fits in ram.

       Also note that the server has some basic checks to enforce this limit but those checks are not exhaustive
       for example need to check for size too big after
         update $push (append) operation
         various db.eval() type operations

       Note also we sometimes do work with objects slightly larger - an object in the replication local.oplog
       could be slightly larger.
    */
    const int MaxBSONObjectSize = 4 * 1024 * 1024;

    // tempish...move to TLS or pass all the way down as a parm
    extern map<string,Database*> databases;
    extern bool master;

    /* sometimes we deal with databases with the same name in different directories - thus this */
    inline string makeDbKeyStr( const char *ns, const string& path ) {
        char cl[256];
        nsToClient(ns, cl);
        return string( cl ) + ":" + path;
    }

    /* returns true if the database ("database") did not exist, and it was created on this call 
       path - datafiles directory, if not the default, so we can differentiate between db's of the same
              name in different places (for example temp ones on repair).
    */
    inline bool setClient(const char *ns, const string& path=dbpath) {
        if( logLevel > 5 )
            log() << "setClient: " << ns << endl;

        cc().top.clientStart( ns );

        string key = makeDbKeyStr( ns, path );
        map<string,Database*>::iterator it = databases.find(key);
        if ( it != databases.end() ) {
            cc().setns(ns, it->second);
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

        assertInWriteLock();

        char cl[256];
        nsToClient(ns, cl);
        bool justCreated;
        Database *newdb = new Database(cl, justCreated, path);
        databases[key] = newdb;
        newdb->finishInit();
        cc().setns(ns, newdb);

        return justCreated;
    }

// shared functionality for removing references to a database from this program instance
// does not delete the files on disk
    void closeClient( const char *cl, const string& path = dbpath );

    /* remove database from the databases map */
    inline void eraseDatabase( const char *ns, const string& path=dbpath ) {
        string key = makeDbKeyStr( ns, path );
        databases.erase( key );
    }

    inline bool clientIsEmpty() {
        return !cc().database()->namespaceIndex.allocated();
    }

    struct dbtemprelease {
        string clientname;
        string clientpath;
        dbtemprelease() {
            Client& client = cc();
            Database *database = client.database();
            if ( database ) {
                clientname = database->name;
                clientpath = database->path;
            }
            client.top.clientStop();
            dbMutexInfo.leaving();
            dbMutex.unlock();
        }
        ~dbtemprelease() {
            dbMutex.lock();
            dbMutexInfo.entered();
            if ( clientname.empty() )
                cc().setns("", 0);
            else
                setClient(clientname.c_str(), clientpath.c_str());
        }
    };

} // namespace mongo

#include "dbinfo.h"
#include "concurrency.h"
