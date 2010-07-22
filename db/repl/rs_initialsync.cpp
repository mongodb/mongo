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

#include "pch.h"
#include "../client.h"
#include "../../client/dbclient.h"
#include "rs.h"
#include "../oplogreader.h"
#include "../../util/mongoutils/str.h"
#include "../dbhelpers.h"

namespace mongo {

    using namespace mongoutils;

    void dropAllDatabasesExceptLocal();

    // add try/catch with sleep

    void isyncassert(const char *msg, bool expr) { 
        if( !expr ) { 
            string m = str::stream() << "initial sync " << msg;
            theReplSet->sethbmsg(m, 0);
            uasserted(13404, m);
        }
    }

    void ReplSetImpl::syncDoInitialSync() { 
        while( 1 ) {
            try {
                _syncDoInitialSync();
                break;
            }
            catch(DBException& e) {
                sethbmsg("initial sync exception " + e.toString(), 0);
                sleepsecs(30);
            }
        }
    }

    bool cloneFrom(const char *masterHost, string& errmsg, const string& fromdb, bool logForReplication, 
				   bool slaveOk, bool useReplAuth, bool snapshot);

    /* todo : progress metering to sethbmsg. */
    static bool clone(const char *master, string db) {
        string err;
        return cloneFrom(master, err, db, false, 
            /*slaveok later can be true*/ false, true, false);
    }

    void _logOpObjRS(const BSONObj& op);

    void ReplSetImpl::_syncDoInitialSync() { 
        sethbmsg("initial sync pending",0);

        assert( !isPrimary() ); // wouldn't make sense if we were.

        const Member *cp = currentPrimary();
        if( cp == 0 ) {
            sethbmsg("initial sync need a member to be primary",0);
            sleepsecs(15);
            return;
        }

        string masterHostname = cp->h().toString();
        OplogReader r;
        if( !r.connect(masterHostname) ) {
            sethbmsg( str::stream() << "initial sync couldn't connect to " << cp->h().toString() , 0);
            sleepsecs(15);
            return;
        }

        BSONObj lastOp = r.getLastOp(rsoplog);
        if( lastOp.isEmpty() ) { 
            sethbmsg("initial sync couldn't read remote oplog", 0);
            sleepsecs(15);
            return;
        }
        OpTime ts = lastOp["ts"]._opTime();
        
        {
            /* make sure things aren't too flappy */
            sleepsecs(5);
            isyncassert( "flapping?", currentPrimary() == cp );
            BSONObj o = r.getLastOp(rsoplog);
            isyncassert( "flapping [2]?", !o.isEmpty() );
        }

        sethbmsg("initial sync drop all databases", 0);
        dropAllDatabasesExceptLocal();
        sethbmsg("initial sync continues");

        list<string> dbs = r.conn()->getDatabaseNames();
        for( list<string>::iterator i = dbs.begin(); i != dbs.end(); i++ ) {
            string db = *i;
            if( db != "local" ) {
                sethbmsg( str::stream() << "initial sync cloning db: " << db , 0);
                bool ok;
                {
                    writelock lk(db);
                    Client::Context ctx(db);
                    ok = clone(masterHostname.c_str(), db);
                }
                if( !ok ) { 
                    sethbmsg( str::stream() << "initial sync error clone of " << db << " failed sleeping 5 minutes" ,0);
                    sleepsecs(300);
                    return;
                }
            }
        }

        sethbmsg("initial sync query minValid",0);

        /* our cloned copy will be strange until we apply oplog events that occurred 
           through the process.  we note that time point here. */
        BSONObj minValid = r.getLastOp(rsoplog);

        MemoryMappedFile::flushAll(true);
        sethbmsg("initial sync clone done first write to oplog still pending",0);

        assert( !isPrimary() ); // wouldn't make sense if we were.

        {
            writelock lk("local.");
            Helpers::putSingleton("local.replset.minvalid", minValid);
            // write an op from the primary to our oplog that existed when 
            // we started cloning.  that will be our starting point.
            //
            // todo : handle case where lastOp on the primary has rolled back.  may have to just 
            //        reclone, but don't get stuck with manual error at least...
            //
            _logOpObjRS(lastOp);
        }
        MemoryMappedFile::flushAll(true);
        sethbmsg("initial sync done",0);
    }

}
