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

    void startSyncThread() { 
        Client::initThread("rs_sync");
        theReplSet->syncThread();
        cc().shutdown();
    }

    void ReplSetImpl::syncApply(const BSONObj &o) {
        //const char *op = o.getStringField("op");
        
        char db[MaxDatabaseLen];
        const char *ns = o.getStringField("ns");
        nsToDatabase(ns, db);

        if ( *ns == '.' || *ns == 0 ) {
            log() << "replSet skipping bad op in oplog: " << o.toString() << endl;
            return;
        }

        Client::Context ctx(ns);
        ctx.getClient()->curop()->reset();

        /* todo : if this asserts, do we want to ignore or not? */
        applyOperation_inlock(o);
    }

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
                log() << "replSet error too stale to catch up, at least from primary " << hn << rsLog;
                log() << "replSet our last optime : " << lastOpTimeWritten.toStringPretty() << rsLog;
                log() << "replSet oldest at " << hn << " : " << ts.toStringPretty() << rsLog;
                log() << "replSet See http://www.mongodb.org/display/DOCS/Resyncing+a+Very+Stale+Replica+Set+Member" << rsLog;
                sethbmsg("error too stale to catch up");
                sleepsecs(120);
                return;
            }
        }

        r.tailingQueryGTE(rsoplog, lastOpTimeWritten);
        assert( r.haveCursor() );
        assert( r.awaitCapable() );

        {
            BSONObj o = r.nextSafe();
            OpTime ts = o["ts"]._opTime();
            long long h = o["h"].numberLong();
            if( ts != lastOpTimeWritten || h != lastH ) { 
                log() << "TEMP " << lastOpTimeWritten.toStringPretty() << endl;
                log() << "TEMP " << ts.toStringPretty() << endl;
                /*
                }*/

                syncRollback(r);
                return;
            }
        }

        // TODO : switch state to secondary here when appropriate...

        while( 1 ) { 
            while( 1 ) {
                if( !r.moreInCurrentBatch() ) { 
                    /* we need to occasionally check some things. between 
                       batches is probably a good time. */

                    /* perhaps we should check this earlier? but not before the rollback checks. */
                    if( state().recovering() ) { 
                        /* can we go to RS_SECONDARY state?  we can if not too old and not minvalid */
                        bool golive = false;
                        {
                            readlock lk("local.replset.minvalid");
                            BSONObj mv;
                            if( Helpers::getSingleton("local.replset.minvalid", mv) ) { 
                                if( mv["ts"]._opTime() <= lastOpTimeWritten ) { 
                                    golive=true;
                                }
                            }
                            else 
                                golive = true; /* must have been the original member */
                        }
                        if( golive )
                            changeState(MemberState::RS_SECONDARY);

                        /* todo: too stale capability */
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
                        syncApply(o);
                        _logOpObjRS(o);   /* with repl sets we write the ops to our oplog too: */                   
                    }
                }
            }
            r.tailCheck();
            if( !r.haveCursor() ) {
                log() << "replSet TEMP end syncTail pass with " << hn << rsLog;
                // TODO : reuse our cnonection to the primary.
                return;
            }
            if( box.getPrimary() != primary )
                return;
            // looping back is ok because this is a tailable cursor
        }
    }

    void ReplSetImpl::_syncThread() {
        StateBox::SP sp = box.get();
        if( sp.state.primary() )
            return;

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
                log() << "replSet syncThread: " << e.toString() << rsLog;
                sleepsecs(10);
            }
            catch(...) { 
                sethbmsg("unexpected exception in syncThread()");
                // TODO : SET NOT SECONDARY here.
                sleepsecs(60);
            }
            sleepsecs(2);
        }
    }

}
