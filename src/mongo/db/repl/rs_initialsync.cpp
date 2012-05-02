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
#include "mongo/db/repl.h"
#include "mongo/db/client.h"
#include "rs.h"
#include "mongo/db/oplogreader.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/db/dbhelpers.h"
#include "rs_optime.h"
#include "mongo/db/oplog.h"

namespace mongo {

    using namespace mongoutils;
    using namespace bson;

    void dropAllDatabasesExceptLocal();

    // add try/catch with sleep

    void isyncassert(const string& msg, bool expr) {
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
        Client::WriteContext ctx(rsoplog);
        NamespaceDetails *d = nsdetails(rsoplog);

        // temp
        if( d && d->stats.nrecords == 0 )
            return; // already empty, ok.

        LOG(1) << "replSet empty oplog" << rsLog;
        d->emptyCappedCollection(rsoplog);
    }

    Member* ReplSetImpl::getMemberToSyncTo() {

        bool buildIndexes = true;

        {
            // if we have a target we've requested to sync from, use it
            lock lk(this);

            if (_forceSyncTarget) {
                _currentSyncTarget = _forceSyncTarget;
                _forceSyncTarget = 0;
                sethbmsg( str::stream() << "syncing to: " << _currentSyncTarget->fullName() << " by request", 0);
                return _currentSyncTarget;
            }
        }

        // wait for 2N pings before choosing a sync target
        if (_cfg) {
            int needMorePings = config().members.size()*2 - HeartbeatInfo::numPings;

            if (needMorePings > 0) {
                OCCASIONALLY log() << "waiting for " << needMorePings << " pings from other members before syncing" << endl;
                return NULL;
            }

            buildIndexes = myConfig().buildIndexes;
        }

        Member *closest = 0;
        time_t now = 0;
        // find the member with the lowest ping time that has more data than me
        for (Member *m = _members.head(); m; m = m->next()) {
            if (m->hbinfo().up() &&
                // make sure members with buildIndexes sync from other members w/indexes
                (!buildIndexes || (buildIndexes && m->config().buildIndexes)) &&
                (m->state() == MemberState::RS_PRIMARY ||
                 (m->state() == MemberState::RS_SECONDARY && m->hbinfo().opTime > lastOpTimeWritten)) &&
                (!closest || m->hbinfo().ping < closest->hbinfo().ping) &&
                ( myConfig().slaveDelay >= m->config().slaveDelay )) {

                map<string,time_t>::iterator vetoed = _veto.find(m->fullName());
                if (vetoed == _veto.end()) {
                    closest = m;
                    break;
                }

                if (now == 0) {
                    now = time(0);
                }

                // if this was on the veto list, check if it was vetoed in the last "while"
                if ((*vetoed).second < now) {
                    _veto.erase(vetoed);
                    closest = m;
                    break;
                }

                // if it was recently vetoed, skip
                log() << "replSet not trying to sync from " << (*vetoed).first
                      << ", it is vetoed for " << ((*vetoed).second - now) << " more seconds" << rsLog;
            }
        }

        {
            lock lk(this);        

            if (!closest) {
                _currentSyncTarget = NULL;
                return NULL;
            }
            
            _currentSyncTarget = closest;
        }

        sethbmsg( str::stream() << "syncing to: " << closest->fullName(), 0);

        return closest;
    }

    void ReplSetImpl::veto(const string& host, const unsigned secs) {
        _veto[host] = time(0)+secs;
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
                        Client::WriteContext ctx(db);
                        ok = clone(sourceHostname.c_str(), db);
                    }
                    if( !ok ) {
                        sethbmsg( str::stream() << "initial sync error clone of " << db << " failed sleeping 5 minutes" ,0);
                        veto(source->fullName(), 600);
                        sleepsecs(300);
                        return;
                    }
                }
            }
        }

        sethbmsg("initial sync query minValid",0);

        /* our cloned copy will be strange until we apply oplog events that occurred
           through the process.  we note that time point here. */
        BSONObj minValid;
        try {
            // It may have been a long time since we last used this connection to
            // query the oplog, depending on the size of the databases we needed to clone.
            // A common problem is that TCP keepalives are set too infrequent, and thus
            // our connection here is terminated by a firewall due to inactivity.
            // Solution is to increase the TCP keepalive frequency.
            minValid = r.getLastOp(rsoplog);
        } catch ( SocketException & ) {
            log() << "connection lost to " << source->h().toString() << "; is your tcp keepalive interval set appropriately?";
            if( !r.connect(sourceHostname) ) {
                sethbmsg( str::stream() << "initial sync couldn't connect to " << source->h().toString() , 0);
                throw;
            }
            // retry
            minValid = r.getLastOp(rsoplog);
        }


        isyncassert( "getLastOp is empty ", !minValid.isEmpty() );
        OpTime mvoptime = minValid["ts"]._opTime();
        verify( !mvoptime.isNull() );
        verify( mvoptime >= startingTS );

        // apply startingTS..mvoptime portion of the oplog
        {
            // note we assume here that this call does not throw
            if( ! initialSyncOplogApplication(startingTS, mvoptime) ) {
                log() << "replSet initial sync failed during oplog application phase" << rsLog;

                emptyOplog(); // otherwise we'll be up!
                
                lastOpTimeWritten = OpTime();
                lastH = 0;
                
                log() << "replSet cleaning up [1]" << rsLog;
                {
                    Client::WriteContext cx( "local." );
                    cx.ctx().db()->flushFiles(true);
                }
                log() << "replSet cleaning up [2]" << rsLog;

                log() << "replSet initial sync failed will try again" << endl;

                sleepsecs(5);
                return;
            }
        }

        sethbmsg("initial sync finishing up",0);

        verify( !box.getState().primary() ); // wouldn't make sense if we were.

        {
            Client::WriteContext cx( "local." );
            cx.ctx().db()->flushFiles(true);
            try {
                log() << "replSet set minValid=" << minValid["ts"]._opTime().toString() << rsLog;
            }
            catch(...) { }
            Helpers::putSingleton("local.replset.minvalid", minValid);
            cx.ctx().db()->flushFiles(true);
        }

        sethbmsg("initial sync done",0);
    }

}
