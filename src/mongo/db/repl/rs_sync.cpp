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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/db/repl/rs_sync.h"

#include <vector>

#include "third_party/murmurhash3/MurmurHash3.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/prefetch.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/base/counter.h"



namespace mongo {

    using namespace bson;
    extern unsigned replSetForceInitialSyncFailure;

    const int ReplSetImpl::maxSyncSourceLagSecs = 30;

    // For testing network failures in percolate() for chaining
    MONGO_FP_DECLARE(rsChaining1);
    MONGO_FP_DECLARE(rsChaining2);
    MONGO_FP_DECLARE(rsChaining3);

namespace replset {

    MONGO_FP_DECLARE(rsSyncApplyStop);

    // Number and time of each ApplyOps worker pool round
    static TimerStats applyBatchStats;
    static ServerStatusMetricField<TimerStats> displayOpBatchesApplied(
                                                    "repl.apply.batches",
                                                    &applyBatchStats );
    //The oplog entries applied
    static Counter64 opsAppliedStats;
    static ServerStatusMetricField<Counter64> displayOpsApplied( "repl.apply.ops",
                                                                &opsAppliedStats );


    SyncTail::SyncTail(BackgroundSyncInterface *q) :
        Sync(""), oplogVersion(0), _networkQueue(q)
    {}

    SyncTail::~SyncTail() {}

    bool SyncTail::peek(BSONObj* op) {
        return _networkQueue->peek(op);
    }
    /* apply the log op that is in param o
       @return bool success (true) or failure (false)
    */
    bool SyncTail::syncApply(const BSONObj &op, bool convertUpdateToUpsert) {
        const char *ns = op.getStringField("ns");
        verify(ns);

        if ( (*ns == '\0') || (*ns == '.') ) {
            // this is ugly
            // this is often a no-op
            // but can't be 100% sure
            if( *op.getStringField("op") != 'n' ) {
                error() << "replSet skipping bad op in oplog: " << op.toString() << rsLog;
            }
            return true;
        }

        bool isCommand(op["op"].valuestrsafe()[0] == 'c');

        boost::scoped_ptr<Lock::ScopedLock> lk;

        if(isCommand) {
            // a command may need a global write lock. so we will conservatively go 
            // ahead and grab one here. suboptimal. :-(
            lk.reset(new Lock::GlobalWrite());
        } else {
            // DB level lock for this operation
            lk.reset(new Lock::DBWrite(ns)); 
        }

        Client::Context ctx(ns, dbpath);
        ctx.getClient()->curop()->reset();
        // For non-initial-sync, we convert updates to upserts
        // to suppress errors when replaying oplog entries.
        bool ok = !applyOperation_inlock(op, true, convertUpdateToUpsert);
        opsAppliedStats.increment();
        getDur().commitIfNeeded();

        return ok;
    }

    void initializePrefetchThread() {
        if (!ClientBasic::getCurrent()) {
            Client::initThread("repl prefetch worker");
            replLocalAuth();
        }
    }

    static AtomicUInt32 replWriterWorkerId;
    void initializeWriterThread() {
        // Only do this once per thread
        if (!ClientBasic::getCurrent()) {
            string threadName = str::stream() << "repl writer worker " << replWriterWorkerId.addAndFetch(1);
            Client::initThread( threadName.c_str() );
            // allow us to get through the magic barrier
            Lock::ParallelBatchWriterMode::iAmABatchParticipant();
            replLocalAuth();
        }
    }

    // This free function is used by the writer threads to apply each op
    void multiSyncApply(const std::vector<BSONObj>& ops, SyncTail* st) {
        initializeWriterThread();

        // convert update operations only for 2.2.1 or greater, because we need guaranteed
        // idempotent operations for this to work.  See SERVER-6825
        bool convertUpdatesToUpserts = theReplSet->oplogVersion > 1 ? true : false;

        for (std::vector<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            try {
                if (!st->syncApply(*it, convertUpdatesToUpserts)) {
                    fassertFailedNoTrace(16359);
                }
            } catch (const DBException& e) {
                error() << "writer worker caught exception: " << causedBy(e)
                        << " on: " << it->toString() << endl;
                fassertFailedNoTrace(16360);
            }
        }
    }

    // This free function is used by the initial sync writer threads to apply each op
    void multiInitialSyncApply(const std::vector<BSONObj>& ops, SyncTail* st) {
        initializeWriterThread();
        for (std::vector<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            try {
                if (!st->syncApply(*it)) {
                    bool status;
                    {
                        Lock::GlobalWrite lk;
                        status = st->shouldRetry(*it);
                    }
                    if (status) {
                        // retry
                        if (!st->syncApply(*it)) {
                            fassertFailedNoTrace(15915);
                        }
                    }
                    // If shouldRetry() returns false, fall through.
                    // This can happen if the document that was moved and missed by Cloner
                    // subsequently got deleted and no longer exists on the Sync Target at all
                }
            }
            catch (const DBException& e) {
                error() << "exception: " << causedBy(e) << " on: " << it->toString() << endl;
                fassertFailedNoTrace(16361);
            }
        }
    }


    // The pool threads call this to prefetch each op
    void SyncTail::prefetchOp(const BSONObj& op) {
        initializePrefetchThread();

        const char *ns = op.getStringField("ns");
        if (ns && (ns[0] != '\0')) {
            try {
                // one possible tweak here would be to stay in the read lock for this database 
                // for multiple prefetches if they are for the same database.
                Client::ReadContext ctx(ns);
                prefetchPagesForReplicatedOp(op);
            }
            catch (const DBException& e) {
                LOG(2) << "ignoring exception in prefetchOp(): " << e.what() << endl;
            }
            catch (const std::exception& e) {
                log() << "Unhandled std::exception in prefetchOp(): " << e.what() << endl;
                fassertFailed(16397);
            }
        }
    }

    // Doles out all the work to the reader pool threads and waits for them to complete
    void SyncTail::prefetchOps(const std::deque<BSONObj>& ops) {
        threadpool::ThreadPool& prefetcherPool = theReplSet->getPrefetchPool();
        for (std::deque<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            prefetcherPool.schedule(&prefetchOp, *it);
        }
        prefetcherPool.join();
    }
    
    // Doles out all the work to the writer pool threads and waits for them to complete
    void SyncTail::applyOps(const std::vector< std::vector<BSONObj> >& writerVectors, 
                                     MultiSyncApplyFunc applyFunc) {
        ThreadPool& writerPool = theReplSet->getWriterPool();
        TimerHolder timer(&applyBatchStats);
        for (std::vector< std::vector<BSONObj> >::const_iterator it = writerVectors.begin();
             it != writerVectors.end();
             ++it) {
            if (!it->empty()) {
                writerPool.schedule(applyFunc, boost::cref(*it), this);
            }
        }
        writerPool.join();
    }

    // Doles out all the work to the writer pool threads and waits for them to complete
    void SyncTail::multiApply( std::deque<BSONObj>& ops, MultiSyncApplyFunc applyFunc ) {

        // Use a ThreadPool to prefetch all the operations in a batch.
        prefetchOps(ops);
        
        std::vector< std::vector<BSONObj> > writerVectors(theReplSet->replWriterThreadCount);
        fillWriterVectors(ops, &writerVectors);
        LOG(1) << "replication batch size is " << ops.size() << endl;
        // We must grab this because we're going to grab write locks later.
        // We hold this mutex the entire time we're writing; it doesn't matter
        // because all readers are blocked anyway.
        SimpleMutex::scoped_lock fsynclk(filesLockedFsync);

        // stop all readers until we're done
        Lock::ParallelBatchWriterMode pbwm;

        applyOps(writerVectors, applyFunc);
    }


    void SyncTail::fillWriterVectors(const std::deque<BSONObj>& ops, 
                                              std::vector< std::vector<BSONObj> >* writerVectors) {
        for (std::deque<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            const BSONElement e = it->getField("ns");
            verify(e.type() == String);
            const char* ns = e.valuestr();
            int len = e.valuestrsize();
            uint32_t hash = 0;
            MurmurHash3_x86_32( ns, len, 0, &hash);

            (*writerVectors)[hash % writerVectors->size()].push_back(*it);
        }
    }


    InitialSync::InitialSync(BackgroundSyncInterface *q) : 
        SyncTail(q) {}

    InitialSync::~InitialSync() {}

    BSONObj SyncTail::oplogApplySegment(const BSONObj& applyGTEObj, const BSONObj& minValidObj,
                                     MultiSyncApplyFunc func) {
        OpTime applyGTE = applyGTEObj["ts"]._opTime();
        OpTime minValid = minValidObj["ts"]._opTime();

        // We have to keep track of the last op applied to the data, because there's no other easy
        // way of getting this data synchronously.  Batches may go past minValidObj, so we need to
        // know to bump minValid past minValidObj.
        BSONObj lastOp = applyGTEObj;
        OpTime ts = applyGTE;

        time_t start = time(0);
        time_t now = start;

        unsigned long long n = 0, lastN = 0;

        while( ts < minValid ) {
            OpQueue ops;

            while (ops.getSize() < replBatchLimitBytes) {
                if (tryPopAndWaitForMore(&ops)) {
                    break;
                }

                // apply replication batch limits
                now = time(0);
                if (!ops.empty()) {
                    if (now > replBatchLimitSeconds)
                        break;
                    if (ops.getDeque().size() > replBatchLimitOperations)
                        break;
                }
            }
            setOplogVersion(ops.getDeque().front());
            
            multiApply(ops.getDeque(), func);

            n += ops.getDeque().size();

            if ( n > lastN + 1000 ) {
                if (now - start > 10) {
                    // simple progress metering
                    log() << "replSet initialSyncOplogApplication applied " << n << " operations, synced to "
                          << ts.toStringPretty() << rsLog;
                    start = now;
                    lastN = n;
                }
            }

            // we want to keep a record of the last op applied, to compare with minvalid
            lastOp = ops.getDeque().back();
            OpTime tempTs = lastOp["ts"]._opTime();
            applyOpsToOplog(&ops.getDeque());

            ts = tempTs;
        }

        return lastOp;
    }

    /* initial oplog application, during initial sync, after cloning.
    */
    BSONObj InitialSync::oplogApplication(const BSONObj& applyGTEObj, const BSONObj& minValidObj) {
        if (replSetForceInitialSyncFailure > 0) {
            log() << "replSet test code invoked, forced InitialSync failure: " << replSetForceInitialSyncFailure << rsLog;
            replSetForceInitialSyncFailure--;
            throw DBException("forced error",0);
        }

        // create the initial oplog entry
        syncApply(applyGTEObj);
        _logOpObjRS(applyGTEObj);

        return oplogApplySegment(applyGTEObj, minValidObj, multiInitialSyncApply);
    }

    BSONObj SyncTail::oplogApplication(const BSONObj& applyGTEObj, const BSONObj& minValidObj) {
        return oplogApplySegment(applyGTEObj, minValidObj, multiSyncApply);
    }

    void SyncTail::setOplogVersion(const BSONObj& op) {
        BSONElement version = op["v"];
        // old primaries do not get the unique index ignoring feature
        // because some of their ops are not imdepotent, see
        // SERVER-7186
        if (version.eoo()) {
            theReplSet->oplogVersion = 1;
            RARELY log() << "warning replset primary is an older version than we are; upgrade recommended" << endl;
        } else {
            theReplSet->oplogVersion = version.Int();
        }
    }

    /* tail an oplog.  ok to return, will be re-called. */
    void SyncTail::oplogApplication() {
        while( 1 ) {
            OpQueue ops;

            verify( !Lock::isLocked() );

            Timer batchTimer;
            int lastTimeChecked = 0;

            do {
                if (theReplSet->isPrimary()) {
                    massert(16620, "there are ops to sync, but I'm primary", ops.empty());
                    return;
                }

                int now = batchTimer.seconds();

                // apply replication batch limits
                if (!ops.empty()) {
                    if (now > replBatchLimitSeconds)
                        break;
                    if (ops.getDeque().size() > replBatchLimitOperations)
                        break;
                }
                // occasionally check some things
                // (always checked in the first iteration of this do-while loop, because
                // ops is empty)
                if (ops.empty() || now > lastTimeChecked) {
                    lastTimeChecked = now;
                    // can we become secondary?
                    // we have to check this before calling mgr, as we must be a secondary to
                    // become primary
                    if (!theReplSet->isSecondary()) {
                        OpTime minvalid;
                        theReplSet->tryToGoLiveAsASecondary(minvalid);
                    }

                    // normally msgCheckNewState gets called periodically, but in a single node repl set
                    // there are no heartbeat threads, so we do it here to be sure.  this is relevant if the
                    // singleton member has done a stepDown() and needs to come back up.
                    if (theReplSet->config().members.size() == 1 &&
                        theReplSet->myConfig().potentiallyHot()) {
                        Manager* mgr = theReplSet->mgr;
                        // When would mgr be null?  During replsettest'ing, in which case we should
                        // fall through and actually apply ops as if we were a real secondary.
                        if (mgr) { 
                            mgr->send(boost::bind(&Manager::msgCheckNewState, theReplSet->mgr));
                            sleepsecs(1);
                            // There should never be ops to sync in a 1-member set, anyway
                            return;
                        }
                    }
                }

                const int slaveDelaySecs = theReplSet->myConfig().slaveDelay;
                if (!ops.empty() && slaveDelaySecs > 0) {
                    const BSONObj& lastOp = ops.getDeque().back();
                    const unsigned int opTimestampSecs = lastOp["ts"]._opTime().getSecs();

                    // Stop the batch as the lastOp is too new to be applied. If we continue
                    // on, we can get ops that are way ahead of the delay and this will
                    // make this thread sleep longer when handleSlaveDelay is called
                    // and apply ops much sooner than we like.
                    if (opTimestampSecs > static_cast<unsigned int>(time(0) - slaveDelaySecs)) {
                        break;
                    }
                }
                // keep fetching more ops as long as we haven't filled up a full batch yet
            } while (!tryPopAndWaitForMore(&ops) && // tryPopAndWaitForMore returns true 
                                                    // when we need to end a batch early
                   (ops.getSize() < replBatchLimitBytes));

            // For pausing replication in tests
            while (MONGO_FAIL_POINT(rsSyncApplyStop)) {
                sleepmillis(0);
            }

            const BSONObj& lastOp = ops.getDeque().back();
            setOplogVersion(lastOp);
            handleSlaveDelay(lastOp);

            // Set minValid to the last op to be applied in this next batch.
            // This will cause this node to go into RECOVERING state
            // if we should crash and restart before updating the oplog
            theReplSet->setMinValid(lastOp);

            multiApply(ops.getDeque(), multiSyncApply);

            applyOpsToOplog(&ops.getDeque());

            // If we're just testing (no manager), don't keep looping if we exhausted the bgqueue
            if (!theReplSet->mgr) {
                BSONObj op;
                if (!peek(&op)) {
                    return;
                }
            }
        }
    }

    // Copies ops out of the bgsync queue into the deque passed in as a parameter.
    // Returns true if the batch should be ended early.
    // Batch should end early if we encounter a command, or if
    // there are no further ops in the bgsync queue to read.
    // This function also blocks 1 second waiting for new ops to appear in the bgsync
    // queue.  We can't block forever because there are maintenance things we need
    // to periodically check in the loop.
    bool SyncTail::tryPopAndWaitForMore(SyncTail::OpQueue* ops) {
        BSONObj op;
        // Check to see if there are ops waiting in the bgsync queue
        bool peek_success = peek(&op);

        if (!peek_success) {
            // if we don't have anything in the queue, wait a bit for something to appear
            if (ops->empty()) {
                // block up to 1 second
                _networkQueue->waitForMore();
                return false;
            }

            // otherwise, apply what we have
            return true;
        }

	const char* ns = op["ns"].valuestrsafe();

        // check for commands
        if ((op["op"].valuestrsafe()[0] == 'c') ||
            // Index builds are acheived through the use of an insert op, not a command op.
            // The following line is the same as what the insert code uses to detect an index build.
            ( *ns != '\0' && nsToCollectionSubstring(ns) == "system.indexes" )) {

            if (ops->empty()) {
                // apply commands one-at-a-time
                ops->push_back(op);
                _networkQueue->consume();
            }

            // otherwise, apply what we have so far and come back for the command
            return true;
        }

        // check for oplog version change
        BSONElement elemVersion = op["v"];
        int curVersion = 0;
        if (elemVersion.eoo())
            // missing version means version 1
            curVersion = 1;
        else
            curVersion = elemVersion.Int();

        if (curVersion != oplogVersion) {
            // Version changes cause us to end a batch.
            // If we are starting a new batch, reset version number
            // and continue.
            if (ops->empty()) {
                oplogVersion = curVersion;
            } 
            else {
                // End batch early
                return true;
            }
        }
    
        // Copy the op to the deque and remove it from the bgsync queue.
        ops->push_back(op);
        _networkQueue->consume();

        // Go back for more ops
        return false;
    }

    void SyncTail::applyOpsToOplog(std::deque<BSONObj>* ops) {
        {
            Lock::DBWrite lk("local");
            while (!ops->empty()) {
                const BSONObj& op = ops->front();
                // this updates theReplSet->lastOpTimeWritten
                _logOpObjRS(op);
                ops->pop_front();
             }
        }

        // Update write concern on primary
        BackgroundSync::notify();
    }

    void SyncTail::handleSlaveDelay(const BSONObj& lastOp) {
        int sd = theReplSet->myConfig().slaveDelay;

        // ignore slaveDelay if the box is still initializing. once
        // it becomes secondary we can worry about it.
        if( sd && theReplSet->isSecondary() ) {
            const OpTime ts = lastOp["ts"]._opTime();
            long long a = ts.getSecs();
            long long b = time(0);
            long long lag = b - a;
            long long sleeptime = sd - lag;
            if( sleeptime > 0 ) {
                uassert(12000, "rs slaveDelay differential too big check clocks and systems", sleeptime < 0x40000000);
                if( sleeptime < 60 ) {
                    sleepsecs((int) sleeptime);
                }
                else {
                    log() << "replSet slavedelay sleep long time: " << sleeptime << rsLog;
                    // sleep(hours) would prevent reconfigs from taking effect & such!
                    long long waitUntil = b + sleeptime;
                    while( 1 ) {
                        sleepsecs(6);
                        if( time(0) >= waitUntil )
                            break;

                        if( theReplSet->myConfig().slaveDelay != sd ) // reconf
                            break;
                    }
                }
            }
        } // endif slaveDelay
    }

} // namespace replset

    /* should be in RECOVERING state on arrival here.
       readlocks
       @return true if transitioned to SECONDARY
    */
    bool ReplSetImpl::tryToGoLiveAsASecondary(OpTime& /*out*/ minvalid) {
        bool golive = false;

        lock rsLock( this );
        Lock::GlobalWrite writeLock;

        // make sure we're not primary, secondary, rollback, or fatal already
        if (box.getState().primary() || box.getState().secondary() ||
            box.getState().fatal()) {
            return false;
        }

        if (_maintenanceMode > 0) {
            // we're not actually going live
            return true;
        }

        // if we're blocking sync, don't change state
        if (_blockSync) {
            return false;
        }

        minvalid = getMinValid();
        if( minvalid <= lastOpTimeWritten ) {
            golive=true;
        }
        else {
            sethbmsg(str::stream() << "still syncing, not yet to minValid optime " <<
                     minvalid.toString());
        }

        if( golive ) {
            sethbmsg("");
            changeState(MemberState::RS_SECONDARY);
        }
        return golive;
    }


    bool ReplSetImpl::forceSyncFrom(const string& host, string& errmsg, BSONObjBuilder& result) {
        lock lk(this);

        // initial sanity check
        if (iAmArbiterOnly()) {
            errmsg = "arbiters don't sync";
            return false;
        }
        if (box.getState().primary()) {
            errmsg = "primaries don't sync";
            return false;
        }
        if (_self != NULL && host == _self->fullName()) {
            errmsg = "I cannot sync from myself";
            return false;
        }

        // find the member we want to sync from
        Member *newTarget = 0;
        for (Member *m = _members.head(); m; m = m->next()) {
            if (m->fullName() == host) {
                newTarget = m;
                break;
            }
        }

        // do some more sanity checks
        if (!newTarget) {
            // this will also catch if someone tries to sync a member from itself, as _self is not
            // included in the _members list.
            errmsg = "could not find member in replica set";
            return false;
        }
        if (newTarget->config().arbiterOnly) {
            errmsg = "I cannot sync from an arbiter";
            return false;
        }
        if (!newTarget->config().buildIndexes && myConfig().buildIndexes) {
            errmsg = "I cannot sync from a member who does not build indexes";
            return false;
        }
        if (newTarget->hbinfo().authIssue) {
            errmsg = "I cannot authenticate against the requested member";
            return false;
        }
        if (newTarget->hbinfo().health == 0) {
            errmsg = "I cannot reach the requested member";
            return false;
        }
        if (newTarget->hbinfo().opTime.getSecs()+10 < lastOpTimeWritten.getSecs()) {
            log() << "attempting to sync from " << newTarget->fullName()
                  << ", but its latest opTime is " << newTarget->hbinfo().opTime.getSecs()
                  << " and ours is " << lastOpTimeWritten.getSecs() << " so this may not work"
                  << rsLog;
            result.append("warning", "requested member is more than 10 seconds behind us");
            // not returning false, just warning
        }

        // record the previous member we were syncing from
        const Member *prev = replset::BackgroundSync::get()->getSyncTarget();
        if (prev) {
            result.append("prevSyncTarget", prev->fullName());
        }

        // finally, set the new target
        _forceSyncTarget = newTarget;
        return true;
    }

    bool ReplSetImpl::gotForceSync() {
        lock lk(this);
        return _forceSyncTarget != 0;
    }

    bool ReplSetImpl::shouldChangeSyncTarget(const OpTime& targetOpTime) const {
        for (Member *m = _members.head(); m; m = m->next()) {
            if (m->syncable() &&
                targetOpTime.getSecs()+maxSyncSourceLagSecs < m->hbinfo().opTime.getSecs()) {
                return true;
            }
        }

        return false;
    }

    void ReplSetImpl::_syncThread() {
        StateBox::SP sp = box.get();
        if( sp.state.primary() ) {
            sleepsecs(1);
            return;
        }
        if( _blockSync || sp.state.fatal() || sp.state.startup() ) {
            sleepsecs(5);
            return;
        }

        // Check criteria for doing an initial sync:
        // 1. If the oplog is empty, do an initial sync
        // 2. If minValid has _initialSyncFlag set, do an initial sync
        if (lastOpTimeWritten.isNull() || getInitialSyncFlag()) {
            syncDoInitialSync();
            return; // _syncThread will be recalled, starts from top again in case sync failed.
        }

        /* we have some data.  continue tailing. */
        replset::SyncTail tail(replset::BackgroundSync::get());
        tail.oplogApplication();
    }

    void ReplSetImpl::syncThread() {
        while( 1 ) {
            // After a reconfig, we may not be in the replica set anymore, so
            // check that we are in the set (and not an arbiter) before
            // trying to sync with other replicas.
            if( ! _self ) {
                log() << "replSet warning did not receive a valid config yet, sleeping 20 seconds " << rsLog;
                sleepsecs(20);
                continue;
            }
            if( myConfig().arbiterOnly ) {
                return;
            }

            fassert(16113, !Lock::isLocked());

            try {
                _syncThread();
            }
            catch(const DBException& e) {
                sethbmsg(str::stream() << "syncThread: " << e.toString());
                sleepsecs(10);
            }
            catch(...) {
                sethbmsg("unexpected exception in syncThread()");
                // TODO : SET NOT SECONDARY here?
                sleepsecs(60);
            }
            sleepsecs(1);
        }
    }

    void startSyncThread() {
        static int n;
        if( n != 0 ) {
            log() << "replSet ERROR : more than one sync thread?" << rsLog;
            verify( n == 0 );
        }
        n++;

        Client::initThread("rsSync");
        replLocalAuth();
        theReplSet->syncThread();
        cc().shutdown();
    }

    void GhostSync::starting() {
        Client::initThread("rsGhostSync");
        replLocalAuth();
    }

    void ReplSetImpl::blockSync(bool block) {
        // RS lock is already taken in Manager::checkAuth
        _blockSync = block;
        if (_blockSync) {
            // syncing is how we get into SECONDARY state, so we'll be stuck in
            // RECOVERING until we unblock
            changeState(MemberState::RS_RECOVERING);
        }
    }

    void GhostSync::clearCache() {
        rwlock lk(_lock, true);
        _ghostCache.clear();
    }

    void GhostSync::associateSlave(const BSONObj& id, const int memberId) {
        const OID rid = id["_id"].OID();
        rwlock lk( _lock , true );
        shared_ptr<GhostSlave> &g = _ghostCache[rid];
        if( g.get() == 0 ) {
            g.reset( new GhostSlave() );
            wassert( _ghostCache.size() < 10000 );
        }
        GhostSlave &slave = *g;
        if (slave.init) {
            LOG(1) << "tracking " << slave.slave->h().toString() << " as " << rid << rsLog;
            return;
        }

        slave.slave = (Member*)rs->findById(memberId);
        if (slave.slave != 0) {
            slave.init = true;
        }
        else {
            log() << "replset couldn't find a slave with id " << memberId
                  << ", not tracking " << rid << rsLog;
        }
    }

    void GhostSync::updateSlave(const mongo::OID& rid, const OpTime& last) {
        rwlock lk( _lock , false );
        MAP::iterator i = _ghostCache.find( rid );
        if ( i == _ghostCache.end() ) {
            OCCASIONALLY warning() << "couldn't update position of the secondary with replSet _id '"
                                   << rid << "' because we have no entry for it" << rsLog;
            return;
        }

        GhostSlave& slave = *(i->second);
        if (!slave.init) {
            OCCASIONALLY log() << "couldn't update position of the secondary with replSet _id '"
                               << rid << "' because it has not been initialized" << rsLog;
            return;
        }

        ((ReplSetConfig::MemberCfg)slave.slave->config()).updateGroups(last);
    }

    void GhostSync::percolate(const mongo::OID& rid, const OpTime& last) {
        shared_ptr<GhostSlave> slave;
        {
            rwlock lk( _lock , false );

            MAP::iterator i = _ghostCache.find( rid );
            if ( i == _ghostCache.end() ) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " no entry" << rsLog;
                return;
            }

            slave = i->second;
            if (!slave->init) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " not init" << rsLog;
                return;
            }
        }
        verify(slave->slave);

        // Keep trying to update until we either succeed or we become primary.
        // Note that this can block the ghostsync thread for quite a while if there
        // are connection problems to the current sync source ("sync target")
        while (true) {
            const Member *target = replset::BackgroundSync::get()->getSyncTarget();
            if (!target || rs->box.getState().primary()
                // we are currently syncing from someone who's syncing from us
                // the target might end up with a new Member, but s.slave never
                // changes so we'll compare the names
                || target == slave->slave || target->fullName() == slave->slave->fullName()) {
                LOG(1) << "replica set ghost target no good" << endl;
                return;
            }

            try {
                if (MONGO_FAIL_POINT(rsChaining1)) {
                    mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                        setMode(FailPoint::nTimes, 1);
                }

                // haveCursor() does not necessarily tell us if we have a non-dead cursor, 
                // so we check tailCheck() as well; see SERVER-8420
                slave->reader.tailCheck();
                if (!slave->reader.haveCursor()) {
                    if (!slave->reader.connect(rid, slave->slave->id(), target->fullName())) {
                        // error message logged in OplogReader::connect
                        sleepsecs(1);
                        continue;
                    }

                    if (MONGO_FAIL_POINT(rsChaining2)) {
                        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                            setMode(FailPoint::nTimes, 1);
                    }

                    slave->reader.ghostQueryGTE(rsoplog, last);
                    // if we lose the connection between connecting and querying, the cursor may not
                    // exist so we have to check again before using it.
                    if (!slave->reader.haveCursor()) {
                        sleepsecs(1);
                        continue;
                    }
                }

                LOG(1) << "replSet last: " << slave->last.toString() << " to " 
                       << last.toString() << rsLog;
                if (slave->last > last) {
                    // Nothing to do; already up to date.
                    return;
                }

                while (slave->last <= last) {
                    if (MONGO_FAIL_POINT(rsChaining3)) {
                        mongo::getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->
                            setMode(FailPoint::nTimes, 1);
                    }

                    if (!slave->reader.more()) {
                        // Hit the end of the oplog on the sync source; we're fully up to date now.
                        return;
                    }

                    BSONObj o = slave->reader.nextSafe();
                    slave->last = o["ts"]._opTime();
                }
                LOG(2) << "now last is " << slave->last.toString() << rsLog;
                // We moved the cursor forward enough; we're done.
                return;
            }
            catch (const DBException& e) {
                // This captures SocketExceptions as well.
                log() << "replSet ghost sync error: " << e.what() << " for "
                      << slave->slave->fullName() << rsLog;
                slave->reader.resetConnection();
            }
        }
    }
}
