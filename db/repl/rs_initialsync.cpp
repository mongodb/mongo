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

    /* todo : progress metering to sethbmsg. */
    static bool clone(const char *master, string db) {
        string err;
        return cloneFrom(master, err, db, false,
                         /* slave_ok */ true, true, false, /*mayYield*/true, /*mayBeInterrupted*/false);
    }

    void _logOpObjRS(const BSONObj& op);

    static void emptyOplog() {
        writelock lk(rsoplog);
        Client::Context ctx(rsoplog);
        NamespaceDetails *d = nsdetails(rsoplog);

        // temp
        if( d && d->stats.nrecords == 0 )
            return; // already empty, ok.

        log(1) << "replSet empty oplog" << rsLog;
        d->emptyCappedCollection(rsoplog);
    }

    const Member* ReplSetImpl::getMemberToSyncTo() {
        Member *closest = 0;
        
        // find the member with the lowest ping time that has more data than me
        for (Member *m = _members.head(); m; m = m->next()) {
            if (m->hbinfo().up() &&
                (m->state() == MemberState::RS_PRIMARY ||
                 (m->state() == MemberState::RS_SECONDARY && m->hbinfo().opTime > lastOpTimeWritten)) &&
                (!closest || m->hbinfo().ping < closest->hbinfo().ping)) {
                closest = m;
            }
        }

        if (!closest) {
            _currentSyncTarget = NULL;
            return NULL;
        }

        sethbmsg( str::stream() << "syncing to: " << closest->fullName(), 0);
        _currentSyncTarget = closest;
        return const_cast<Member*>(closest);
    }

    /**
     * Do the initial sync for this member.
     */
    void ReplSetImpl::_syncDoInitialSync() {
        sethbmsg("initial sync pending",0);

        // if this is the first node, it may have already become primary
        if ( box.getState().primary() ) {
            sethbmsg("I'm already primary, no need for initial sync",0);
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

        isyncassert( "initial sync source must remain readable throughout our initial sync", source->state().readable() );

        /* our cloned copy will be strange until we apply oplog events that occurred
           through the process.  we note that time point here. */
        BSONObj minValid = r.getLastOp(rsoplog);
        isyncassert( "getLastOp is empty ", !minValid.isEmpty() );
        OpTime mvoptime = minValid["ts"]._opTime();
        assert( !mvoptime.isNull() );

        /* apply relevant portion of the oplog
        */
        {
            isyncassert( "initial sync source must remain readable throughout our initial sync [2]", source->state().readable() );
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
