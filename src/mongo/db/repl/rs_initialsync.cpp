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

#include "mongo/db/repl/rs.h"

#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/oplog.h"
#include "mongo/db/oplogreader.h"
#include "mongo/db/repl.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/rs_optime.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/util/mongoutils/str.h"

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
        const static int maxFailedAttempts = 10;
        createOplog();
        int failedAttempts = 0;
        while ( failedAttempts < maxFailedAttempts ) {
            try {
                _syncDoInitialSync();
                break;
            }
            catch(DBException& e) {
                failedAttempts++;
                str::stream msg;
                msg << "initial sync exception: ";
                msg << e.toString() << " " << (maxFailedAttempts - failedAttempts) << " attempts remaining" ;
                sethbmsg(msg, 0);
                sleepsecs(30);
            }
        }
        fassert( 16233, failedAttempts < maxFailedAttempts);
    }

    /* todo : progress metering to sethbmsg. */
    static bool clone(const char *master, string db, bool dataPass ) {
        CloneOptions options;

        options.fromDB = db;

        options.logForRepl = false;
        options.slaveOk = true;
        options.useReplAuth = true;
        options.snapshot = false;
        options.mayYield = true;
        options.mayBeInterrupted = false;
        
        options.syncData = dataPass;
        options.syncIndexes = ! dataPass;

        string err;
        return cloneFrom(master, options , err );
    }


    bool ReplSetImpl::_syncDoInitialSync_clone( const char *master, const list<string>& dbs , bool dataPass ) {
        for( list<string>::const_iterator i = dbs.begin(); i != dbs.end(); i++ ) {
            string db = *i;
            if( db == "local" ) 
                continue;
            
            if ( dataPass )
                sethbmsg( str::stream() << "initial sync cloning db: " << db , 0);
            else
                sethbmsg( str::stream() << "initial sync cloning indexes for : " << db , 0);

            Client::WriteContext ctx(db);
            if ( ! clone( master, db, dataPass ) ) {
                sethbmsg( str::stream() 
                              << "initial sync error clone of " << db 
                              << " dataPass: " << dataPass << " failed sleeping 5 minutes" ,0);
                return false;
            }
        }

        return true;
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
        lock lk(this);

        bool buildIndexes = true;

        // if we have a target we've requested to sync from, use it

        if (_forceSyncTarget) {
            Member* target = _forceSyncTarget;
            _forceSyncTarget = 0;
            sethbmsg( str::stream() << "syncing to: " << target->fullName() << " by request", 0);
            return target;
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

        // find the member with the lowest ping time that has more data than me

        // Find primary's oplog time. Reject sync candidates that are more than
        // MAX_SLACK_TIME seconds behind.
        OpTime primaryOpTime;
        static const unsigned maxSlackDurationSeconds = 10 * 60; // 10 minutes
        const Member* primary = box.getPrimary();
        if (primary) 
            primaryOpTime = primary->hbinfo().opTime;
        else
            // choose a time that will exclude no candidates, since we don't see a primary
            primaryOpTime = OpTime(maxSlackDurationSeconds, 0);

        if ( primaryOpTime.getSecs() < maxSlackDurationSeconds ) {
            // erh - I think this means there was just a new election
            // and we don't yet know the new primary's optime
            primaryOpTime = OpTime(maxSlackDurationSeconds, 0);
        }

        OpTime oldestSyncOpTime(primaryOpTime.getSecs() - maxSlackDurationSeconds, 0);

        Member *closest = 0;
        time_t now = 0;

        // Make two attempts.  The first attempt, we ignore those nodes with
        // slave delay higher than our own.  The second attempt includes such
        // nodes, in case those are the only ones we can reach.
        // This loop attempts to set 'closest'.
        for (int attempts = 0; attempts < 2; ++attempts) {
            for (Member *m = _members.head(); m; m = m->next()) {
                if (!m->hbinfo().up())
                    continue;
                // make sure members with buildIndexes sync from other members w/indexes
                if (buildIndexes && !m->config().buildIndexes)
                    continue;

                if (!m->state().readable())
                    continue;

                if (m->state() == MemberState::RS_SECONDARY) {
                    // only consider secondaries that are ahead of where we are
                    if (m->hbinfo().opTime <= lastOpTimeWritten)
                        continue;
                    // omit secondaries that are excessively behind, on the first attempt at least.
                    if (attempts == 0 && 
                        m->hbinfo().opTime < oldestSyncOpTime)
                        continue;
                }

                // omit nodes that are more latent than anything we've already considered
                if (closest && 
                    (m->hbinfo().ping > closest->hbinfo().ping))
                    continue;

                if ( attempts == 0 &&
                     myConfig().slaveDelay < m->config().slaveDelay ) {
                    continue; // skip this one in the first attempt
                }

                map<string,time_t>::iterator vetoed = _veto.find(m->fullName());
                if (vetoed != _veto.end()) {
                    // Do some veto housekeeping
                    if (now == 0) {
                        now = time(0);
                    }

                    // if this was on the veto list, check if it was vetoed in the last "while".
                    // if it was, skip.
                    if (vetoed->second >= now) {
                        if (time(0) % 5 == 0) {
                            log() << "replSet not trying to sync from " << (*vetoed).first
                                  << ", it is vetoed for " << ((*vetoed).second - now) << " more seconds" << rsLog;
                        }
                        continue;
                    }
                    _veto.erase(vetoed);
                    // fall through, this is a valid candidate now
                }
                // This candidate has passed all tests; set 'closest'
                closest = m;
            }
            if (closest) break; // no need for second attempt
        }

        if (!closest) {
            return NULL;
        }

        sethbmsg( str::stream() << "syncing to: " << closest->fullName(), 0);

        return closest;
    }

    void ReplSetImpl::veto(const string& host, const unsigned secs) {
        lock lk(this);
        _veto[host] = time(0)+secs;
    }

    bool ReplSetImpl::_syncDoInitialSync_applyToHead( replset::InitialSync& init, OplogReader* r, 
                                                      const Member* source, const BSONObj& lastOp , 
                                                      BSONObj& minValid ) {
        /* our cloned copy will be strange until we apply oplog events that occurred
           through the process.  we note that time point here. */

        try {
            // It may have been a long time since we last used this connection to
            // query the oplog, depending on the size of the databases we needed to clone.
            // A common problem is that TCP keepalives are set too infrequent, and thus
            // our connection here is terminated by a firewall due to inactivity.
            // Solution is to increase the TCP keepalive frequency.
            minValid = r->getLastOp(rsoplog);
        } catch ( SocketException & ) {
            log() << "connection lost to " << source->h().toString() << "; is your tcp keepalive interval set appropriately?";
            if( !r->connect(source->h().toString()) ) {
                sethbmsg( str::stream() << "initial sync couldn't connect to " << source->h().toString() , 0);
                throw;
            }
            // retry
            minValid = r->getLastOp(rsoplog);
        }

        isyncassert( "getLastOp is empty ", !minValid.isEmpty() );

        OpTime mvoptime = minValid["ts"]._opTime();
        verify( !mvoptime.isNull() );

        OpTime startingTS = lastOp["ts"]._opTime();
        verify( mvoptime >= startingTS );

        // apply startingTS..mvoptime portion of the oplog
        {
            // note we assume here that this call does not throw
            if (!init.oplogApplication(lastOp, minValid)) {
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
                return false;
            }
        }
        
        return true;
    }

    /**
     * Do the initial sync for this member.
     */
    void ReplSetImpl::_syncDoInitialSync() {
        replset::InitialSync init(replset::BackgroundSync::get());
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

        if (replSettings.fastsync) {
            log() << "fastsync: skipping database clone" << rsLog;

            // prime oplog
            init.oplogApplication(lastOp, lastOp);
            return;
        }
        else {
            sethbmsg("initial sync drop all databases", 0);
            dropAllDatabasesExceptLocal();

            sethbmsg("initial sync clone all databases", 0);

            list<string> dbs = r.conn()->getDatabaseNames();

            if ( ! _syncDoInitialSync_clone( sourceHostname.c_str(), dbs, true ) ) {
                veto(source->fullName(), 600);
                sleepsecs(300);
                return;
            }

            sethbmsg("initial sync data copy, starting syncup",0);
            
            BSONObj minValid;
            if ( ! _syncDoInitialSync_applyToHead( init, &r , source , lastOp , minValid ) ) {
                return;
            }

            lastOp = minValid;

            // reset state, as that "didn't count"
            emptyOplog(); 
            lastOpTimeWritten = OpTime();
            lastH = 0;

            sethbmsg("initial sync building indexes",0);
            if ( ! _syncDoInitialSync_clone( sourceHostname.c_str(), dbs, false ) ) {
                veto(source->fullName(), 600);
                sleepsecs(300);
                return;
            }
        }

        sethbmsg("initial sync query minValid",0);

        BSONObj minValid;
        if ( ! _syncDoInitialSync_applyToHead( init, &r, source, lastOp, minValid ) ) {
            return;
        }
        
        // ---------


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
