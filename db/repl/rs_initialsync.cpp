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
#include "../repl.h"
#include "../client.h"
#include "../../client/dbclient.h"
#include "rs.h"
#include "../oplogreader.h"
#include "../../util/mongoutils/str.h"
#include "../dbhelpers.h"
#include "rs_optime.h"
#include "../oplog.h"

namespace mongo {

    using namespace mongoutils;
    using namespace bson;

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
        createOplog();
        
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
            /* slave_ok */ true, true, false);
    }

    void _logOpObjRS(const BSONObj& op);

    bool copyCollectionFromRemote(const string& host, const string& ns, const BSONObj& query, string &errmsg, bool logforrepl);

    static void emptyOplog() {
        writelock lk(rsoplog);
        Client::Context ctx(rsoplog);
		NamespaceDetails *d = nsdetails(rsoplog);

		// temp
		if( d && d->stats.nrecords == 0 )
		  return; // already empty, ok.

        log(1) << "replSet empty oplog" << rsLog;
        d->emptyCappedCollection(rsoplog);

        /*
        string errmsg;
        bob res;
        dropCollection(rsoplog, errmsg, res);
		log() << "replSet recreated oplog so it is empty.  todo optimize this..." << rsLog;
		createOplog();*/

      	// TEMP: restart to recreate empty oplog
        //log() << "replSet FATAL error during initial sync.  mongod restart required." << rsLog;
        //dbexit( EXIT_CLEAN );

        /*
        writelock lk(rsoplog);
        Client::Context c(rsoplog, dbpath, 0, doauth/false);
        NamespaceDetails *oplogDetails = nsdetails(rsoplog);
        uassert(13412, str::stream() << "replSet error " << rsoplog << " is missing", oplogDetails != 0);
        oplogDetails->cappedTruncateAfter(rsoplog, h.commonPointOurDiskloc, false);
        */
    }

    /**
     * Choose a member to sync from. Prefers a secondary, if available.
     *
     * TODO: make a config setting like "cloneFromPrimary" or something to force
     * machines to clone from the primary.  We might want to integrate this with
     * tags functionality, if we only want it to be able to clone from certain
     * machines.
     */
    const Member* ReplSetImpl::getMemberToSyncTo() {
        for( Member *m = head(); m; m = m->next() ) {
            if (m->hbinfo().up() && m->state() == MemberState::RS_SECONDARY) {
                sethbmsg( str::stream() << "syncing to secondary: " << m->fullName(), 0);
                return const_cast<Member*>(m);
            }
        }

        // can't find secondary, try primary
        StateBox::SP sp = box.get();
        assert( !sp.state.primary() ); // wouldn't make sense if we were.

        // we just checked in _syncDoInitialSync, but we could have lost a
        // primary since then
        if (!sp.primary) {
            return 0;
        }
        sethbmsg( str::stream() << "syncing to primary: " << sp.primary->fullName(), 0);
        return const_cast<Member*>(sp.primary);
    }
    
    /**
     * Do the initial sync for this member.  There must be a primary available
     * for the whole intial sync, even if we're syncing from a secondary.
     */
    void ReplSetImpl::_syncDoInitialSync() { 
        sethbmsg("initial sync pending",0);
        
        const Member *cp = box.getPrimary();
        if (!cp) {
            sethbmsg("initial sync needs a member to be primary to begin");
            sleepsecs(15);
            return;
        }
        
        const Member *source = getMemberToSyncTo();
        if (!source) {
            sethbmsg("initial sync need a member to be primary or secondary to do our initial sync", 0);
            sleepsecs(15);
            return;
        }
        
        string sourceHostname = source->h().toString();
        OplogReader r;
        if( !r.connect(sourceHostname) ) {
            sethbmsg( str::stream() << "initial sync couldn't connect to " << source->h().toString() , 0);
            sleepsecs(15);
            return;
        }

        BSONObj lastOp = r.getLastOp(rsoplog);
        if( lastOp.isEmpty() ) { 
            sethbmsg("initial sync couldn't read remote oplog", 0);
            sleepsecs(15);
            return;
        }
        OpTime startingTS = lastOp["ts"]._opTime();
        
        if (replSettings.fastsync) {
            log() << "fastsync: skipping database clone" << rsLog;
        }
        else {
            sethbmsg("initial sync drop all databases", 0);
            dropAllDatabasesExceptLocal();

            sethbmsg("initial sync clone all databases", 0);

            list<string> dbs = r.conn()->getDatabaseNames();
            for( list<string>::iterator i = dbs.begin(); i != dbs.end(); i++ ) {
                string db = *i;
                if( db != "local" ) {
                    sethbmsg( str::stream() << "initial sync cloning db: " << db , 0);
                    bool ok;
                    {
                        writelock lk(db);
                        Client::Context ctx(db);
                        ok = clone(sourceHostname.c_str(), db);
                    }
                    if( !ok ) { 
                        sethbmsg( str::stream() << "initial sync error clone of " << db << " failed sleeping 5 minutes" ,0);
                        sleepsecs(300);
                        return;
                    }
                }
            }
        }

        sethbmsg("initial sync query minValid",0);

        isyncassert( "initial sync source must remain primary throughout our initial sync", box.getPrimary() == cp );

        /* our cloned copy will be strange until we apply oplog events that occurred 
           through the process.  we note that time point here. */
        BSONObj minValid = r.getLastOp(rsoplog);
        isyncassert( "getLastOp is empty ", !minValid.isEmpty() );
        OpTime mvoptime = minValid["ts"]._opTime();
        assert( !mvoptime.isNull() );

        /* apply relevant portion of the oplog 
        */
        {
            sethbmsg("initial sync initial oplog application");
            isyncassert( "initial sync source must remain primary throughout our initial sync [2]", box.getPrimary() == cp );
            if( ! initialSyncOplogApplication(source, /*applyGTE*/startingTS, /*minValid*/mvoptime) ) { // note we assume here that this call does not throw
                log() << "replSet initial sync failed during applyoplog" << rsLog;
                emptyOplog(); // otherwise we'll be up!
				lastOpTimeWritten = OpTime();
				lastH = 0;
                log() << "replSet cleaning up [1]" << rsLog;
                {
                    writelock lk("local.");
                    Client::Context cx( "local." );
                    cx.db()->flushFiles(true);            
                }
                log() << "replSet cleaning up [2]" << rsLog;
                sleepsecs(5);
                return;
            }
        }

        sethbmsg("initial sync finishing up",0);
        
        assert( !box.getState().primary() ); // wouldn't make sense if we were.

        {
            writelock lk("local.");
            Client::Context cx( "local." );
            cx.db()->flushFiles(true);            
            try {
                log() << "replSet set minValid=" << minValid["ts"]._opTime().toString() << rsLog;
            }
            catch(...) { }
            Helpers::putSingleton("local.replset.minvalid", minValid);
            cx.db()->flushFiles(true);
        }

        sethbmsg("initial sync done",0);
    }

}
