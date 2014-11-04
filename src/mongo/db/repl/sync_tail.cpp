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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"
#include "mongo/platform/bits.h"

#include "mongo/db/repl/sync_tail.h"

#include <boost/functional/hash.hpp>
#include "third_party/murmurhash3/MurmurHash3.h"

#include "mongo/base/counter.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/prefetch.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace repl {
#if defined(MONGO_PLATFORM_64)
    const int replWriterThreadCount = 16;
    const int replPrefetcherThreadCount = 16;
#elif defined(MONGO_PLATFORM_32)
    const int replWriterThreadCount = 2;
    const int replPrefetcherThreadCount = 2;
#else
#error need to include something that defines MONGO_PLATFORM_XX
#endif

    static Counter64 opsAppliedStats;

    //The oplog entries applied
    static ServerStatusMetricField<Counter64> displayOpsApplied( "repl.apply.ops",
                                                                &opsAppliedStats );

    MONGO_FP_DECLARE(rsSyncApplyStop);

    // Number and time of each ApplyOps worker pool round
    static TimerStats applyBatchStats;
    static ServerStatusMetricField<TimerStats> displayOpBatchesApplied(
                                                    "repl.apply.batches",
                                                    &applyBatchStats );
    void initializePrefetchThread() {
        if (!ClientBasic::getCurrent()) {
            Client::initThreadIfNotAlready();
            replLocalAuth();
        }
    }
    namespace {
        bool isCrudOpType( const char* field ) {
            switch ( field[0] ) {
            case 'd':
            case 'i':
            case 'u':
                return field[1] == 0;
            }
            return false;
        }

        size_t hashBSONObj(const BSONObj& obj);

        size_t hashBSONElement(const BSONElement& elem) {
            size_t hash = 0;

            boost::hash_combine(hash, elem.canonicalType());

            const StringData fieldName = elem.fieldNameStringData();
            if (!fieldName.empty()) {
                boost::hash_combine(hash, StringData::Hasher()(fieldName));
            }

            switch (elem.type()) {
                // Order of types is the same as in compareElementValues().

            case mongo::EOO:
            case mongo::Undefined:
            case mongo::jstNULL:
            case mongo::MaxKey:
            case mongo::MinKey:
                // These are valueless types
                break;

            case mongo::Bool:
                boost::hash_combine(hash, elem.boolean());
                break;

            case mongo::Timestamp:
            case mongo::Date:
                // Need to treat these the same until SERVER-3304 is resolved.
                boost::hash_combine(hash, elem.date().asInt64());
                break;

            case mongo::NumberDouble:
            case mongo::NumberLong:
            case mongo::NumberInt: {
                // This converts all numbers to doubles for compatibility with woCompare.
                // This ignores // the low-order bits of NumberLongs > 2**53, but that is required
                // until SERVER-3719 is resolved.
                const double dbl = elem.numberDouble();
                if (isNaN(dbl)) {
                    boost::hash_combine(hash, numeric_limits<double>::quiet_NaN());
                }
                else {
                    boost::hash_combine(hash, dbl);
                }
                break;
            }

            case mongo::jstOID:
                elem.__oid().hash_combine(hash);
                break;

            case mongo::Code:
            case mongo::Symbol:
            case mongo::String:
                boost::hash_combine(hash, StringData::Hasher()(elem.valueStringData()));
                break;

            case mongo::Object:
            case mongo::Array:
                boost::hash_combine(hash, hashBSONObj(elem.embeddedObject()));
                break;

            case mongo::DBRef:
            case mongo::BinData:
                // All bytes of the value are required to be identical.
                boost::hash_combine(hash, StringData::Hasher()(StringData(elem.value(),
                                                                          elem.valuesize())));
                break;

            case mongo::RegEx:
                boost::hash_combine(hash, StringData::Hasher()(elem.regex()));
                boost::hash_combine(hash, StringData::Hasher()(elem.regexFlags()));
                break;

            case mongo::CodeWScope: {
                // SERVER-7804
                // Intentionally not using codeWScopeCodeLen for compatibility with
                // compareElementValues. Using codeWScopeScopeDataUnsafe (as a string!) for the same
                // reason.
                boost::hash_combine(hash, StringData::Hasher()(elem.codeWScopeCode()));
                boost::hash_combine(hash, StringData::Hasher()(elem.codeWScopeScopeDataUnsafe()));
                break;
            }
            }
            return hash;
        }

        size_t hashBSONObj(const BSONObj& obj) {
            size_t hash = 0;
            BSONForEach(elem, obj) {
                boost::hash_combine(hash, hashBSONElement(elem));
            }
            return hash;
        }

    }

    SyncTail::SyncTail(BackgroundSyncInterface *q, MultiSyncApplyFunc func) :
        Sync(""), 
        _networkQueue(q), 
        _applyFunc(func),
        _writerPool(replWriterThreadCount, "repl writer worker "),
        _prefetcherPool(replPrefetcherThreadCount, "repl prefetch worker ")
    {}

    SyncTail::~SyncTail() {}

    bool SyncTail::peek(BSONObj* op) {
        return _networkQueue->peek(op);
    }
    /* apply the log op that is in param o
       @return bool success (true) or failure (false)
    */
    bool SyncTail::syncApply(
                        OperationContext* txn, const BSONObj &op, bool convertUpdateToUpsert) {
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

        const char* opType = op["op"].valuestrsafe();

        bool isCommand(opType[0] == 'c');

        for ( int createCollection = 0; createCollection < 2; createCollection++ ) {
            try {
                boost::scoped_ptr<Lock::ScopedLock> lk;
                boost::scoped_ptr<Lock::CollectionLock> lk2;

                bool isIndexBuild = opType[0] == 'i' &&
                    nsToCollectionSubstring( ns ) == "system.indexes";

                if(isCommand) {
                    // a command may need a global write lock. so we will conservatively go
                    // ahead and grab one here. suboptimal. :-(
                    lk.reset(new Lock::GlobalWrite(txn->lockState()));
                } else if (isCrudOpType(opType)) {
                    LockMode mode = createCollection ? MODE_X : MODE_IX;
                    lk.reset(new Lock::DBLock(txn->lockState(), nsToDatabaseSubstring(ns), mode));
                    lk2.reset(new Lock::CollectionLock(txn->lockState(), ns, mode));

                    if (!createCollection && !dbHolder().get(txn, nsToDatabaseSubstring(ns))) {
                        // need to create database, try again
                        continue;
                    }

                } else if (isIndexBuild) {
                    lk.reset(new Lock::DBLock(txn->lockState(), nsToDatabaseSubstring(ns), MODE_X));
                    lk2.reset(new Lock::CollectionLock(txn->lockState(), ns, MODE_X));
                } else {
                    // DB level lock for this operation
                    log() << "non command or crup op: " << op;
                    lk.reset(new Lock::DBLock(txn->lockState(), nsToDatabaseSubstring(ns), MODE_X));
                }

                Client::Context ctx(txn, ns);
                ctx.getClient()->curop()->reset();

                if ( createCollection == 0 &&
                     isCrudOpType(opType) &&
                     ctx.db()->getCollection(txn,ns) == NULL ) {
                    // uh, oh, we need to create collection
                    // try again
                    continue;
                }

                // For non-initial-sync, we convert updates to upserts
                // to suppress errors when replaying oplog entries.
                bool ok = !applyOperation_inlock(txn, ctx.db(), op, true, convertUpdateToUpsert);
                opsAppliedStats.increment();
                return ok;
            }
            catch ( const WriteConflictException& wce ) {
                log() << "WriteConflictException while doing oplog application on: " << ns
                      << ", retrying.";
                createCollection--;
            }
        }
        invariant(!"impossible");
    }

    // The pool threads call this to prefetch each op
    void SyncTail::prefetchOp(const BSONObj& op) {
        initializePrefetchThread();

        const char *ns = op.getStringField("ns");
        if (ns && (ns[0] != '\0')) {
            try {
                // one possible tweak here would be to stay in the read lock for this database 
                // for multiple prefetches if they are for the same database.
                OperationContextImpl txn;
                AutoGetCollectionForRead ctx(&txn, ns);
                prefetchPagesForReplicatedOp(&txn, ctx.getDb(), op);
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
        for (std::deque<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            _prefetcherPool.schedule(&prefetchOp, *it);
        }
        _prefetcherPool.join();
    }
    
    // Doles out all the work to the writer pool threads and waits for them to complete
    void SyncTail::applyOps(const std::vector< std::vector<BSONObj> >& writerVectors) {
        TimerHolder timer(&applyBatchStats);
        for (std::vector< std::vector<BSONObj> >::const_iterator it = writerVectors.begin();
             it != writerVectors.end();
             ++it) {
            if (!it->empty()) {
                _writerPool.schedule(_applyFunc, boost::cref(*it), this);
            }
        }
        _writerPool.join();
    }

    // Doles out all the work to the writer pool threads and waits for them to complete
    OpTime SyncTail::multiApply( std::deque<BSONObj>& ops) {

        if (getGlobalEnvironment()->getGlobalStorageEngine()->isMmapV1()) {
            // Use a ThreadPool to prefetch all the operations in a batch.
            prefetchOps(ops);
        }
        
        std::vector< std::vector<BSONObj> > writerVectors(replWriterThreadCount);
        fillWriterVectors(ops, &writerVectors);
        LOG(2) << "replication batch size is " << ops.size() << endl;
        // We must grab this because we're going to grab write locks later.
        // We hold this mutex the entire time we're writing; it doesn't matter
        // because all readers are blocked anyway.
        SimpleMutex::scoped_lock fsynclk(filesLockedFsync);

        // stop all readers until we're done
        Lock::ParallelBatchWriterMode pbwm;

        ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
        if (replCoord->getCurrentMemberState().primary() &&
            !replCoord->isWaitingForApplierToDrain()) {

            severe() << "attempting to replicate ops while primary";
            fassertFailed(28527);
        }

        applyOps(writerVectors);
        return applyOpsToOplog(&ops);
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

            const char* opType = it->getField( "op" ).valuestrsafe();

            if (getGlobalEnvironment()->getGlobalStorageEngine()->supportsDocLocking() &&
                isCrudOpType(opType)) {
                BSONElement id;
                switch (opType[0]) {
                case 'u':
                    id = it->getField("o2").Obj()["_id"];
                    break;
                case 'd':
                case 'i':
                    id = it->getField("o").Obj()["_id"];
                    break;
                }

                const size_t idHash = hashBSONElement( id );
                MurmurHash3_x86_32(&idHash, sizeof(idHash), hash, &hash);
            }

            (*writerVectors)[hash % writerVectors->size()].push_back(*it);
        }
    }
    void SyncTail::oplogApplication(OperationContext* txn, const OpTime& endOpTime) {
        _applyOplogUntil(txn, endOpTime);
    }

    /* applies oplog from "now" until endOpTime using the applier threads for initial sync*/
    void SyncTail::_applyOplogUntil(OperationContext* txn, const OpTime& endOpTime) {
        unsigned long long bytesApplied = 0;
        unsigned long long entriesApplied = 0;
        while (true) {
            OpQueue ops;

            while (!tryPopAndWaitForMore(txn, &ops, getGlobalReplicationCoordinator())) {
                // nothing came back last time, so go again
                if (ops.empty()) continue;

                // Check if we reached the end
                const BSONObj currentOp = ops.back();
                const OpTime currentOpTime = currentOp["ts"]._opTime();

                // When we reach the end return this batch
                if (currentOpTime == endOpTime) {
                    break;
                }
                else if (currentOpTime > endOpTime) {
                    severe() << "Applied past expected end " << endOpTime << " to " << currentOpTime
                            << " without seeing it. Rollback?" << rsLog;
                    fassertFailedNoTrace(18693);
                }

                // apply replication batch limits
                if (ops.getSize() > replBatchLimitBytes)
                    break;
                if (ops.getDeque().size() > replBatchLimitOperations)
                    break;
            };

            if (ops.empty()) {
                severe() << "got no ops for batch...";
                fassertFailedNoTrace(18692);
            }

            const BSONObj lastOp = ops.back().getOwned();

            // Tally operation information
            bytesApplied += ops.getSize();
            entriesApplied += ops.getDeque().size();

            const OpTime lastOpTime = multiApply(ops.getDeque());

            // if the last op applied was our end, return
            if (lastOpTime == endOpTime) {
                LOG(1) << "SyncTail applied " << entriesApplied
                       << " entries (" << bytesApplied << " bytes)"
                       << " and finished at opTime " << endOpTime.toStringPretty();
                return;
            }
        } // end of while (true)
    }

namespace {
    void tryToGoLiveAsASecondary(OperationContext* txn, ReplicationCoordinator* replCoord) {
        Lock::GlobalRead readLock(txn->lockState());

        if (replCoord->getMaintenanceMode()) {
            // we're not actually going live
            return;
        }

        // Only state RECOVERING can transition to SECONDARY.
        MemberState state(replCoord->getCurrentMemberState());
        if (!state.recovering()) {
            return;
        }

        OpTime minvalid = getMinValid(txn);
        if (minvalid > replCoord->getMyLastOptime()) {
            return;
        }

        bool worked = replCoord->setFollowerMode(MemberState::RS_SECONDARY);
        if (!worked) {
            warning() << "Failed to transition into " << MemberState(MemberState::RS_SECONDARY)
                      << ". Current state: " << replCoord->getCurrentMemberState();
        }
    }
}

    /* tail an oplog.  ok to return, will be re-called. */
    void SyncTail::oplogApplication() {
        ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();

        while(!inShutdown()) {
            OpQueue ops;
            OperationContextImpl txn;

            Timer batchTimer;
            int lastTimeChecked = 0;

            do {
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
                    BackgroundSync* bgsync = BackgroundSync::get();
                    if (bgsync->getInitialSyncRequestedFlag()) {
                        // got a resync command
                        Lock::DBLock lk(txn.lockState(), "local", MODE_X);
                        WriteUnitOfWork wunit(&txn);
                        Client::Context ctx(&txn, "local");

                        ctx.db()->dropCollection(&txn, "local.oplog.rs");

                        // Note: the following order is important.
                        // The bgsync thread uses an empty optime as a sentinel to know to wait
                        // for initial sync (done in this thread after we return); thus, we must
                        // ensure the lastAppliedOptime is empty before pausing the bgsync thread
                        // via stop().
                        // We must clear the sync source blacklist after calling stop()
                        // because the bgsync thread, while running, may update the blacklist.
                        replCoord->setMyLastOptime(&txn, OpTime());
                        bgsync->stop();
                        replCoord->clearSyncSourceBlacklist();

                        wunit.commit();

                        return;
                    }
                    lastTimeChecked = now;
                    // can we become secondary?
                    // we have to check this before calling mgr, as we must be a secondary to
                    // become primary
                    tryToGoLiveAsASecondary(&txn, replCoord);

                    // TODO(emilkie): This can be removed once we switch over from legacy;
                    // this code is what moves 1-node sets to PRIMARY state.
                    // normally msgCheckNewState gets called periodically, but in a single node
                    // replset there are no heartbeat threads, so we do it here to be sure.  this is
                    // relevant if the singleton member has done a stepDown() and needs to come back
                    // up.
                    if (theReplSet &&
                            theReplSet->config().members.size() == 1 &&
                            theReplSet->myConfig().potentiallyHot()) {
                        Manager* mgr = theReplSet->mgr;
                        // When would mgr be null?  During replsettest'ing, in which case we should
                        // fall through and actually apply ops as if we were a real secondary.
                        if (mgr) { 
                            mgr->send(stdx::bind(&Manager::msgCheckNewState, theReplSet->mgr));
                            sleepsecs(1);
                            // There should never be ops to sync in a 1-member set, anyway
                            return;
                        }
                    }
                }

                const int slaveDelaySecs = replCoord->getSlaveDelaySecs().total_seconds();
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
            } while (!tryPopAndWaitForMore(&txn, &ops, replCoord) && // tryPopAndWaitForMore returns
                                                                     // true when we need to end a
                                                                     // batch early
                   (ops.getSize() < replBatchLimitBytes) &&
                   !inShutdown());

            // For pausing replication in tests
            while (MONGO_FAIL_POINT(rsSyncApplyStop)) {
                sleepmillis(0);
            }

            if (ops.empty()) {
                continue;
            }

            const BSONObj& lastOp = ops.getDeque().back();
            handleSlaveDelay(lastOp);

            // Set minValid to the last op to be applied in this next batch.
            // This will cause this node to go into RECOVERING state
            // if we should crash and restart before updating the oplog
            OpTime minValid = lastOp["ts"]._opTime();
            setMinValid(&txn, minValid);

            multiApply(ops.getDeque());

            // If we're just testing (no manager), don't keep looping if we exhausted the bgqueue
            // TODO(spencer): Remove repltest.cpp dbtest or make this work with the new replication
            // coordinator
            if (theReplSet && !theReplSet->mgr) {
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
    bool SyncTail::tryPopAndWaitForMore(OperationContext* txn,
                                        SyncTail::OpQueue* ops,
                                        ReplicationCoordinator* replCoord) {
        BSONObj op;
        // Check to see if there are ops waiting in the bgsync queue
        bool peek_success = peek(&op);

        if (!peek_success) {
            // if we don't have anything in the queue, wait a bit for something to appear
            if (ops->empty()) {
                replCoord->signalDrainComplete(txn);
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
        
        if (curVersion != OPLOG_VERSION) {
            severe() << "expected oplog version " << OPLOG_VERSION << " but found version " 
                     << curVersion << " in oplog entry: " << op;
            fassertFailedNoTrace(18820);
        }
    
        // Copy the op to the deque and remove it from the bgsync queue.
        ops->push_back(op);
        _networkQueue->consume();

        // Go back for more ops
        return false;
    }

    OpTime SyncTail::applyOpsToOplog(std::deque<BSONObj>* ops) {
        OpTime lastOpTime;
        {
            OperationContextImpl txn;
            {
                Lock::DBLock lk(txn.lockState(), "local", MODE_X);
                WriteUnitOfWork wunit(&txn);

                while (!ops->empty()) {
                    const BSONObj& op = ops->front();
                    // this updates lastOpTimeApplied
                    lastOpTime = _logOpObjRS(&txn, op);
                    ops->pop_front();
                 }
                wunit.commit();
            }

            // This call may result in us assuming PRIMARY role if we'd been waiting for our sync
            // buffer to drain and it's now empty.  This will acquire a global lock to drop all
            // temp collections, so we must release the above lock on the local database before
            // doing so.
            BackgroundSync::get()->notify(&txn);
        }

        return lastOpTime;
    }

    void SyncTail::handleSlaveDelay(const BSONObj& lastOp) {
        ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
        int slaveDelaySecs = replCoord->getSlaveDelaySecs().total_seconds();

        // ignore slaveDelay if the box is still initializing. once
        // it becomes secondary we can worry about it.
        if( slaveDelaySecs > 0 && replCoord->getCurrentMemberState().secondary() ) {
            const OpTime ts = lastOp["ts"]._opTime();
            long long a = ts.getSecs();
            long long b = time(0);
            long long lag = b - a;
            long long sleeptime = slaveDelaySecs - lag;
            if( sleeptime > 0 ) {
                uassert(12000, "rs slaveDelay differential too big check clocks and systems",
                        sleeptime < 0x40000000);
                if( sleeptime < 60 ) {
                    sleepsecs((int) sleeptime);
                }
                else {
                    warning() << "replSet slavedelay causing a long sleep of " << sleeptime
                              << " seconds" << rsLog;
                    // sleep(hours) would prevent reconfigs from taking effect & such!
                    long long waitUntil = b + sleeptime;
                    while(time(0) < waitUntil) {
                        sleepsecs(6);

                        // Handle reconfigs that changed the slave delay
                        if (replCoord->getSlaveDelaySecs().total_seconds() != slaveDelaySecs)
                            break;
                    }
                }
            }
        } // endif slaveDelay
    }

    static AtomicUInt32 replWriterWorkerId;

    static void initializeWriterThread() {
        // Only do this once per thread
        if (!ClientBasic::getCurrent()) {
            Client::initThreadIfNotAlready();
            replLocalAuth();
        }
    }

    // This free function is used by the writer threads to apply each op
    void multiSyncApply(const std::vector<BSONObj>& ops, SyncTail* st) {
        initializeWriterThread();

        OperationContextImpl txn;

        // allow us to get through the magic barrier
        Lock::ParallelBatchWriterMode::iAmABatchParticipant(txn.lockState());

        bool convertUpdatesToUpserts = true;

        for (std::vector<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            try {
                if (!st->syncApply(&txn, *it, convertUpdatesToUpserts)) {
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

        OperationContextImpl txn;

        // allow us to get through the magic barrier
        Lock::ParallelBatchWriterMode::iAmABatchParticipant(txn.lockState());

        for (std::vector<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            try {
                if (!st->syncApply(&txn, *it)) {
                    bool status;
                    {
                        Lock::GlobalWrite lk(txn.lockState());
                        status = st->shouldRetry(&txn, *it);
                    }

                    if (status) {
                        // retry
                        if (!st->syncApply(&txn, *it)) {
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

} // namespace repl
} // namespace mongo
