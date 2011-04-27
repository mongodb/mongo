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
#include "../repl.h"
#include "connections.h"
namespace mongo {

    using namespace bson;
    extern unsigned replSetForceInitialSyncFailure;

    /* apply the log op that is in param o */
    void ReplSetImpl::syncApply(const BSONObj &o) {
        char db[MaxDatabaseNameLen];
        const char *ns = o.getStringField("ns");
        nsToDatabase(ns, db);

        if ( *ns == '.' || *ns == 0 ) {
            if( *o.getStringField("op") == 'n' )
                return;
            log() << "replSet skipping bad op in oplog: " << o.toString() << endl;
            return;
        }

        Client::Context ctx(ns);
        ctx.getClient()->curop()->reset();

        /* todo : if this asserts, do we want to ignore or not? */
        applyOperation_inlock(o);
    }

    /* initial oplog application, during initial sync, after cloning.
       @return false on failure.
       this method returns an error and doesn't throw exceptions (i think).
    */
    bool ReplSetImpl::initialSyncOplogApplication(
        const Member *source,
        OpTime applyGTE,
        OpTime minValid) {
        if( source == 0 ) return false;

        const string hn = source->h().toString();
        OplogReader r;
        try {
            if( !r.connect(hn) ) {
                log() << "replSet initial sync error can't connect to " << hn << " to read " << rsoplog << rsLog;
                return false;
            }

            r.queryGTE( rsoplog, applyGTE );
            assert( r.haveCursor() );

            {
                if( !r.more() ) {
                    sethbmsg("replSet initial sync error reading remote oplog");
                    log() << "replSet initial sync error remote oplog (" << rsoplog << ") on host " << hn << " is empty?" << rsLog;
                    return false;
                }
                bo op = r.next();
                OpTime t = op["ts"]._opTime();
                r.putBack(op);

                if( op.firstElement().fieldName() == string("$err") ) {
                    log() << "replSet initial sync error querying " << rsoplog << " on " << hn << " : " << op.toString() << rsLog;
                    return false;
                }

                uassert( 13508 , str::stream() << "no 'ts' in first op in oplog: " << op , !t.isNull() );
                if( t > applyGTE ) {
                    sethbmsg(str::stream() << "error " << hn << " oplog wrapped during initial sync");
                    log() << "replSet initial sync expected first optime of " << applyGTE << rsLog;
                    log() << "replSet initial sync but received a first optime of " << t << " from " << hn << rsLog;
                    return false;
                }

                sethbmsg(str::stream() << "initial oplog application from " << hn << " starting at "
                         << t.toStringPretty() << " to " << minValid.toStringPretty());
            }
        }
        catch(DBException& e) {
            log() << "replSet initial sync failing: " << e.toString() << rsLog;
            return false;
        }

        /* we lock outside the loop to avoid the overhead of locking on every operation. */
        writelock lk("");

        // todo : use exhaust
        OpTime ts;
        time_t start = time(0);
        unsigned long long n = 0;
        while( 1 ) {
            try {
                if( !r.more() )
                    break;
                BSONObj o = r.nextSafe(); /* note we might get "not master" at some point */
                {
                    ts = o["ts"]._opTime();

                    /* if we have become primary, we dont' want to apply things from elsewhere
                        anymore. assumePrimary is in the db lock so we are safe as long as
                        we check after we locked above. */
                    if( (source->state() != MemberState::RS_PRIMARY &&
                            source->state() != MemberState::RS_SECONDARY) ||
                            replSetForceInitialSyncFailure ) {

                        int f = replSetForceInitialSyncFailure;
                        if( f > 0 ) {
                            replSetForceInitialSyncFailure = f-1;
                            log() << "replSet test code invoked, replSetForceInitialSyncFailure" << rsLog;
                            throw DBException("forced error",0);
                        }
                        log() << "replSet we are now primary" << rsLog;
                        throw DBException("primary changed",0);
                    }

                    if( ts >= applyGTE ) {
                        // optimes before we started copying need not be applied.
                        syncApply(o);
                    }
                    _logOpObjRS(o);   /* with repl sets we write the ops to our oplog too */
                }

                if ( ++n % 1000 == 0 ) {
                    time_t now = time(0);
                    if (now - start > 10) {
                        // simple progress metering
                        log() << "initialSyncOplogApplication applied " << n << " operations, synced to "
                              << ts.toStringPretty() << rsLog;
                        start = now;
                    }
                }
                
                getDur().commitIfNeeded();
            }
            catch (DBException& e) {
                // skip duplicate key exceptions
                if( e.getCode() == 11000 || e.getCode() == 11001 ) {
                    continue;
                }
                
                // handle cursor not found (just requery)
                if( e.getCode() == 13127 ) {
                    r.resetCursor();
                    r.queryGTE(rsoplog, ts);
                    if( r.haveCursor() ) {
                        continue;
                    }
                }

                // TODO: handle server restart

                if( ts <= minValid ) {
                    // didn't make it far enough
                    log() << "replSet initial sync failing, error applying oplog " << e.toString() << rsLog;
                    return false;
                }

                // otherwise, whatever
                break;
            }
        }
        return true;
    }

    /* should be in RECOVERING state on arrival here.
       readlocks
       @return true if transitioned to SECONDARY
    */
    bool ReplSetImpl::tryToGoLiveAsASecondary(OpTime& /*out*/ minvalid) {
        bool golive = false;
        {
            readlock lk("local.replset.minvalid");
            BSONObj mv;
            if( Helpers::getSingleton("local.replset.minvalid", mv) ) {
                minvalid = mv["ts"]._opTime();
                if( minvalid <= lastOpTimeWritten ) {
                    golive=true;
                }
            }
            else
                golive = true; /* must have been the original member */
        }
        if( golive ) {
            sethbmsg("");
            changeState(MemberState::RS_SECONDARY);
        }
        return golive;
    }

    /**
     * Checks if the oplog given is too far ahead to read from.
     *
     * @param r the oplog
     * @param hn the hostname (for log messages)
     *
     * @return if we are stale compared to the oplog on hn
     */
    bool ReplSetImpl::_isStale(OplogReader& r, const string& hn) {
        BSONObj remoteOldestOp = r.findOne(rsoplog, Query());
        OpTime ts = remoteOldestOp["ts"]._opTime();
        DEV log() << "replSet remoteOldestOp:    " << ts.toStringLong() << rsLog;
        else log(3) << "replSet remoteOldestOp: " << ts.toStringLong() << rsLog;
        DEV {
            // debugging sync1.js...
            log() << "replSet lastOpTimeWritten: " << lastOpTimeWritten.toStringLong() << rsLog;
            log() << "replSet our state: " << state().toString() << rsLog;
        }
        if( lastOpTimeWritten < ts ) {
            log() << "replSet error RS102 too stale to catch up, at least from " << hn << rsLog;
            log() << "replSet our last optime : " << lastOpTimeWritten.toStringLong() << rsLog;
            log() << "replSet oldest at " << hn << " : " << ts.toStringLong() << rsLog;
            log() << "replSet See http://www.mongodb.org/display/DOCS/Resyncing+a+Very+Stale+Replica+Set+Member" << rsLog;
            sethbmsg("error RS102 too stale to catch up");
            changeState(MemberState::RS_RECOVERING);
            sleepsecs(120);
            return true;
        }
        return false;
    }

    /**
     * Tries to connect the oplog reader to a potential sync source.  If
     * successful, it checks that we are not stale compared to this source.
     *
     * @param r reader to populate
     * @param hn hostname to try
     *
     * @return if both checks pass, it returns true, otherwise false.
     */
    bool ReplSetImpl::_getOplogReader(OplogReader& r, string& hn) {
        assert(r.conn() == 0);

        if( !r.connect(hn) ) {
            log(2) << "replSet can't connect to " << hn << " to read operations" << rsLog;
            r.resetConnection();
            return false;
        }
        if( _isStale(r, hn)) {
            r.resetConnection();
            return false;
        }
        return true;
    }

    /* tail an oplog.  ok to return, will be re-called. */
    void ReplSetImpl::syncTail() {
        // todo : locking vis a vis the mgr...
        OplogReader r;
        string hn;
        const Member *target = 0;

        // if we cannot reach the master but someone else is more up-to-date
        // than we are, sync from them.
        target = getMemberToSyncTo();
        if (target != 0) {
            hn = target->h().toString();
            if (!_getOplogReader(r, hn)) {
                // we might be stale wrt the primary, but could still sync from
                // a secondary
                target = 0;
            }
        }
            
        // no server found
        if (target == 0) {
            // if there is no one to sync from
            OpTime minvalid;
            tryToGoLiveAsASecondary(minvalid);
            return;
        }
        
        r.tailingQueryGTE(rsoplog, lastOpTimeWritten);
        assert( r.haveCursor() );

        uassert(1000, "replSet source for syncing doesn't seem to be await capable -- is it an older version of mongodb?", r.awaitCapable() );

        {
            if( !r.more() ) {
                /* maybe we are ahead and need to roll back? */
                try {
                    bo theirLastOp = r.getLastOp(rsoplog);
                    if( theirLastOp.isEmpty() ) {
                        log() << "replSet error empty query result from " << hn << " oplog" << rsLog;
                        sleepsecs(2);
                        return;
                    }
                    OpTime theirTS = theirLastOp["ts"]._opTime();
                    if( theirTS < lastOpTimeWritten ) {
                        log() << "replSet we are ahead of the primary, will try to roll back" << rsLog;
                        syncRollback(r);
                        return;
                    }
                    /* we're not ahead?  maybe our new query got fresher data.  best to come back and try again */
                    log() << "replSet syncTail condition 1" << rsLog;
                    sleepsecs(1);
                }
                catch(DBException& e) {
                    log() << "replSet error querying " << hn << ' ' << e.toString() << rsLog;
                    sleepsecs(2);
                }
                return;
            }

            BSONObj o = r.nextSafe();
            OpTime ts = o["ts"]._opTime();
            long long h = o["h"].numberLong();
            if( ts != lastOpTimeWritten || h != lastH ) {
                log() << "replSet our last op time written: " << lastOpTimeWritten.toStringPretty() << endl;
                log() << "replset source's GTE: " << ts.toStringPretty() << endl;
                syncRollback(r);
                return;
            }
        }

        /* we have now checked if we need to rollback and we either don't have to or did it. */
        {
            OpTime minvalid;
            tryToGoLiveAsASecondary(minvalid);
        }

        while( 1 ) {
            while( 1 ) {
                if( !r.moreInCurrentBatch() ) {
                    /* we need to occasionally check some things. between
                       batches is probably a good time. */

                    /* perhaps we should check this earlier? but not before the rollback checks. */
                    if( state().recovering() ) {
                        /* can we go to RS_SECONDARY state?  we can if not too old and if minvalid achieved */
                        OpTime minvalid;
                        bool golive = ReplSetImpl::tryToGoLiveAsASecondary(minvalid);
                        if( golive ) {
                            ;
                        }
                        else {
                            sethbmsg(str::stream() << "still syncing, not yet to minValid optime" << minvalid.toString());
                        }

                        /* todo: too stale capability */
                    }

                    if( !target->hbinfo().hbstate.readable() ) {
                        return;
                    }
                }
                if( !r.more() )
                    break;
                {
                    BSONObj o = r.nextSafe(); /* note we might get "not master" at some point */

                    int sd = myConfig().slaveDelay;
                    // ignore slaveDelay if the box is still initializing. once
                    // it becomes secondary we can worry about it.
                    if( sd && box.getState().secondary() ) {
                        const OpTime ts = o["ts"]._opTime();
                        long long a = ts.getSecs();
                        long long b = time(0);
                        long long lag = b - a;
                        long long sleeptime = sd - lag;
                        if( sleeptime > 0 ) {
                            uassert(12000, "rs slaveDelay differential too big check clocks and systems", sleeptime < 0x40000000);
                            log() << "replSet temp slavedelay sleep:" << sleeptime << rsLog;
                            if( sleeptime < 60 ) {
                                sleepsecs((int) sleeptime);
                            }
                            else {
                                // sleep(hours) would prevent reconfigs from taking effect & such!
                                long long waitUntil = b + sleeptime;
                                while( 1 ) {
                                    sleepsecs(6);
                                    if( time(0) >= waitUntil )
                                        break;
                                    if( !target->hbinfo().hbstate.readable() ) {
                                        break;
                                    }
                                    if( myConfig().slaveDelay != sd ) // reconf
                                        break;
                                }
                            }
                        }

                    }

                    {
                        writelock lk("");

                        /* if we have become primary, we dont' want to apply things from elsewhere
                           anymore. assumePrimary is in the db lock so we are safe as long as
                           we check after we locked above. */
                        if( box.getState().primary() ) {
                            log(0) << "replSet stopping syncTail we are now primary" << rsLog;
                            return;
                        }

                        syncApply(o);
                        _logOpObjRS(o);   /* with repl sets we write the ops to our oplog too: */
                    }
                }
            }
            r.tailCheck();
            if( !r.haveCursor() ) {
                log(1) << "replSet end syncTail pass with " << hn << rsLog;
                // TODO : reuse our connection to the primary.
                return;
            }
            if( !target->hbinfo().hbstate.readable() ) {
                return;
            }
            // looping back is ok because this is a tailable cursor
        }
    }

    void ReplSetImpl::_syncThread() {
        StateBox::SP sp = box.get();
        if( sp.state.primary() ) {
            sleepsecs(1);
            return;
        }
        if( sp.state.fatal() ) {
            sleepsecs(5);
            return;
        }

        /* do we have anything at all? */
        if( lastOpTimeWritten.isNull() ) {
            syncDoInitialSync();
            return; // _syncThread will be recalled, starts from top again in case sync failed.
        }

        /* we have some data.  continue tailing. */
        syncTail();
    }

    void ReplSetImpl::syncThread() {
        /* test here was to force a receive timeout
        ScopedConn c("localhost");
        bo info;
        try {
            log() << "this is temp" << endl;
            c.runCommand("admin", BSON("sleep"<<120), info);
            log() << info.toString() << endl;
            c.runCommand("admin", BSON("sleep"<<120), info);
            log() << "temp" << endl;
        }
        catch( DBException& e ) {
            log() << e.toString() << endl;
            c.runCommand("admin", BSON("sleep"<<120), info);
            log() << "temp" << endl;
        }
        */

        while( 1 ) {
            // After a reconfig, we may not be in the replica set anymore, so
            // check that we are in the set (and not an arbiter) before
            // trying to sync with other replicas.
            if( ! _self || myConfig().arbiterOnly )
                return;

            try {
                _syncThread();
            }
            catch(DBException& e) {
                sethbmsg("syncThread: " + e.toString());
                sleepsecs(10);
            }
            catch(...) {
                sethbmsg("unexpected exception in syncThread()");
                // TODO : SET NOT SECONDARY here?
                sleepsecs(60);
            }
            sleepsecs(1);

            /* normally msgCheckNewState gets called periodically, but in a single node repl set there
               are no heartbeat threads, so we do it here to be sure.  this is relevant if the singleton
               member has done a stepDown() and needs to come back up.
               */
            OCCASIONALLY {
            	log() << "default rs heartbeat starting..." << endl;
            	mgr->send( boost::bind(&Manager::msgCheckNewState, theReplSet->mgr) );
            	log() << "rs heartbeat finished" << endl;
            }
        }
    }

    void startSyncThread() {
        static int n;
        if( n != 0 ) {
            log() << "replSet ERROR : more than one sync thread?" << rsLog;
            assert( n == 0 );
        }
        n++;

        Client::initThread("replica set sync");
        cc().iAmSyncThread();
        if (!noauth) {
            cc().getAuthenticationInfo()->authorize("local");
        }
        theReplSet->syncThread();
        cc().shutdown();
    }

}
