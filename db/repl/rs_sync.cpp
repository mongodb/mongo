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

namespace mongo {

    using namespace bson;

    void startSyncThread() { 
        Client::initThread("rs_sync");
        cc().iAmSyncThread();
        theReplSet->syncThread();
        cc().shutdown();
    }

    void ReplSetImpl::syncApply(const BSONObj &o) {
        //const char *op = o.getStringField("op");
        
        char db[MaxDatabaseLen];
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

    bool ReplSetImpl::initialSyncOplogApplication(
        string hn, 
        const Member *primary,
        OpTime applyGTE,
        OpTime minValid)
    { 
        if( primary == 0 ) return false;

        OpTime ts;
        try {
            OplogReader r;
            if( !r.connect(hn) ) { 
                log(2) << "replSet can't connect to " << hn << " to read operations" << rsLog;
                return false;
            }

            r.query(rsoplog, bo());
            assert( r.haveCursor() );

            /* we lock outside the loop to avoid the overhead of locking on every operation.  server isn't usable yet anyway! */
            writelock lk("");

            {
                if( !r.more() ) { 
                    sethbmsg("replSet initial sync error reading remote oplog");
                    return false;
                }
                bo op = r.next();
                OpTime t = op["ts"]._opTime();
                r.putBack(op);
                assert( !t.isNull() );
                if( t > applyGTE ) {
                    sethbmsg(str::stream() << "error " << hn << " oplog wrapped during initial sync");
                    return false;
                }
            }

            // todo : use exhaust
            unsigned long long n = 0;
            while( 1 ) { 
                if( !r.more() )
                    break;
                BSONObj o = r.nextSafe(); /* note we might get "not master" at some point */
                {
                    //writelock lk("");

                    ts = o["ts"]._opTime();

                    /* if we have become primary, we dont' want to apply things from elsewhere
                        anymore. assumePrimary is in the db lock so we are safe as long as 
                        we check after we locked above. */
					const Member *p1 = box.getPrimary();
                    if( p1 != primary ) {
					  log() << "replSet primary was:" << primary->fullName() << " now:" << 
						(p1 != 0 ? p1->fullName() : "none") << rsLog;
                        throw DBException("primary changed",0);
                    }

                    if( ts >= applyGTE ) {
                        // optimes before we started copying need not be applied.
                        syncApply(o);
                    }
                    _logOpObjRS(o);   /* with repl sets we write the ops to our oplog too */
                }
                if( ++n % 100000 == 0 ) { 
                    // simple progress metering
                    log() << "replSet initialSyncOplogApplication " << n << rsLog;
                }
            }
        }
        catch(DBException& e) { 
            if( ts <= minValid ) {
                // didn't make it far enough
                log() << "replSet initial sync failing, error applying oplog " << e.toString() << rsLog;
                return false;
            }
        }
        return true;
    }

    /* tail the primary's oplog.  ok to return, will be re-called. */
    void ReplSetImpl::syncTail() { 
        // todo : locking vis a vis the mgr...

        const Member *primary = box.getPrimary();
        if( primary == 0 ) return;
        string hn = primary->h().toString();
        OplogReader r;
        if( !r.connect(primary->h().toString()) ) { 
            log(2) << "replSet can't connect to " << hn << " to read operations" << rsLog;
            return;
        }

        /* first make sure we are not hopelessly out of sync by being very stale. */
        {
            BSONObj remoteOldestOp = r.findOne(rsoplog, Query());
            OpTime ts = remoteOldestOp["ts"]._opTime();
            DEV log() << "remoteOldestOp: " << ts.toStringPretty() << endl;
            else log(3) << "remoteOldestOp: " << ts.toStringPretty() << endl;
            if( lastOpTimeWritten < ts ) { 
                log() << "replSet error RS102 too stale to catch up, at least from primary: " << hn << rsLog;
                log() << "replSet our last optime : " << lastOpTimeWritten.toStringLong() << rsLog;
                log() << "replSet oldest at " << hn << " : " << ts.toStringLong() << rsLog;
                log() << "replSet See http://www.mongodb.org/display/DOCS/Resyncing+a+Very+Stale+Replica+Set+Member" << rsLog;
                sethbmsg("error RS102 too stale to catch up");
                sleepsecs(120);
                return;
            }
        }

        r.tailingQueryGTE(rsoplog, lastOpTimeWritten);
        assert( r.haveCursor() );
        assert( r.awaitCapable() );

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
                /*
                log() << "replSet syncTail error querying oplog >= " << lastOpTimeWritten.toString() << " from " << hn << rsLog;
                try {
                    log() << "replSet " << hn << " last op: " << r.getLastOp(rsoplog).toString() << rsLog;
                }
                catch(...) { }
                sleepsecs(1);
                return;*/
            }

            BSONObj o = r.nextSafe();
            OpTime ts = o["ts"]._opTime();
            long long h = o["h"].numberLong();
            if( ts != lastOpTimeWritten || h != lastH ) { 
                log(1) << "TEMP our last op time written: " << lastOpTimeWritten.toStringPretty() << endl;
                log(1) << "TEMP primary's GTE: " << ts.toStringPretty() << endl;
                /*
                }*/

                syncRollback(r);
                return;
            }
        }

        bool achievedMinValid = false;
        while( 1 ) {
            while( 1 ) {
                if( !r.moreInCurrentBatch() ) { 
                    /* we need to occasionally check some things. between 
                       batches is probably a good time. */

                    /* perhaps we should check this earlier? but not before the rollback checks. */
                    if( state().recovering() ) { 
                        /* can we go to RS_SECONDARY state?  we can if not too old and if minvalid achieved */
                        bool golive = false;			
                        OpTime minvalid;
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
                            achievedMinValid = true;
                            sethbmsg("");
                            //log() << "replSet SECONDARY" << rsLog;
                            changeState(MemberState::RS_SECONDARY);
                        }
                        else { 
                            sethbmsg(str::stream() << "still syncing, not yet to minValid optime " << minvalid.toString());
                        }

                        /* todo: too stale capability */
                    }
                    else {
                        achievedMinValid = true;
                    }

                    if( box.getPrimary() != primary ) 
                        return;
                }
                if( !r.more() )
                    break;
                { 
                    BSONObj o = r.nextSafe(); /* note we might get "not master" at some point */
                    {
                        writelock lk("");

                        /* if we have become primary, we dont' want to apply things from elsewhere
                           anymore. assumePrimary is in the db lock so we are safe as long as 
                           we check after we locked above. */
                        if( box.getPrimary() != primary ) {
                            if( box.getState().primary() )
                                log(0) << "replSet stopping syncTail we are now primary" << rsLog;
                            return;
                        }

                        syncApply(o);
                        _logOpObjRS(o);   /* with repl sets we write the ops to our oplog too: */                   
                    }
                    int sd = myConfig().slaveDelay;
                    if( sd ) { 
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
                                    if( box.getPrimary() != primary )
                                        break;
                                    if( myConfig().slaveDelay != sd ) // reconf
                                        break;
                                }
                            }
                        }
                    }
                }
            }
            r.tailCheck();
            if( !r.haveCursor() ) {
                log(1) << "replSet end syncTail pass with " << hn << rsLog;
                // TODO : reuse our connection to the primary.
                return;
            }
            if( box.getPrimary() != primary )
                return;
            // looping back is ok because this is a tailable cursor
        }
    }

    void ReplSetImpl::_syncThread() {
        StateBox::SP sp = box.get();
        if( sp.state.primary() ) {
            sleepsecs(1);
            return;
        }

        /* later, we can sync from up secondaries if we want. tbd. */
        if( sp.primary == 0 )
            return;

        /* do we have anything at all? */
        if( lastOpTimeWritten.isNull() ) {
            syncDoInitialSync();
            return; // _syncThread will be recalled, starts from top again in case sync failed.
        }

        /* we have some data.  continue tailing. */
        syncTail();
    }

    void ReplSetImpl::syncThread() {
        if( myConfig().arbiterOnly )
            return;
        while( 1 ) { 
            try {
                _syncThread();
            }
            catch(DBException& e) { 
                sethbmsg("syncThread: " + e.toString());
                sleepsecs(10);
            }
            catch(...) { 
                sethbmsg("unexpected exception in syncThread()");
                // TODO : SET NOT SECONDARY here.
                sleepsecs(60);
            }
            sleepsecs(1);
        }
    }

}
