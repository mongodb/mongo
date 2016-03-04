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
#include <memory>
#include "third_party/murmurhash3/MurmurHash3.h"

#include "mongo/base/counter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/prefetch.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;

namespace repl {
#if defined(MONGO_PLATFORM_64)
int SyncTail::replWriterThreadCount = 16;
const int replPrefetcherThreadCount = 16;
#elif defined(MONGO_PLATFORM_32)
int SyncTail::replWriterThreadCount = 2;
const int replPrefetcherThreadCount = 2;
#else
#error need to include something that defines MONGO_PLATFORM_XX
#endif

class ExportedWriterThreadCountParameter
    : public ExportedServerParameter<int, ServerParameterType::kStartupOnly> {
public:
    ExportedWriterThreadCountParameter()
        : ExportedServerParameter<int, ServerParameterType::kStartupOnly>(
              ServerParameterSet::getGlobal(),
              "replWriterThreadCount",
              &SyncTail::replWriterThreadCount) {}

    virtual Status validate(const int& potentialNewValue) {
        if (potentialNewValue < 1 || potentialNewValue > 256) {
            return Status(ErrorCodes::BadValue, "replWriterThreadCount must be between 1 and 256");
        }

        return Status::OK();
    }

} exportedWriterThreadCountParam;


static Counter64 opsAppliedStats;

// The oplog entries applied
static ServerStatusMetricField<Counter64> displayOpsApplied("repl.apply.ops", &opsAppliedStats);

MONGO_FP_DECLARE(rsSyncApplyStop);

// Number and time of each ApplyOps worker pool round
static TimerStats applyBatchStats;
static ServerStatusMetricField<TimerStats> displayOpBatchesApplied("repl.apply.batches",
                                                                   &applyBatchStats);
void initializePrefetchThread() {
    if (!ClientBasic::getCurrent()) {
        Client::initThreadIfNotAlready();
        AuthorizationSession::get(cc())->grantInternalAuthorization();
    }
}
namespace {
bool isCrudOpType(const char* field) {
    switch (field[0]) {
        case 'd':
        case 'i':
        case 'u':
            return field[1] == 0;
    }
    return false;
}

void handleSlaveDelay(const Timestamp& ts) {
    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
    int slaveDelaySecs = durationCount<Seconds>(replCoord->getSlaveDelaySecs());

    // ignore slaveDelay if the box is still initializing. once
    // it becomes secondary we can worry about it.
    if (slaveDelaySecs > 0 && replCoord->getMemberState().secondary()) {
        long long a = ts.getSecs();
        long long b = time(0);
        long long lag = b - a;
        long long sleeptime = slaveDelaySecs - lag;
        if (sleeptime > 0) {
            uassert(12000,
                    "rs slaveDelay differential too big check clocks and systems",
                    sleeptime < 0x40000000);
            if (sleeptime < 60) {
                sleepsecs((int)sleeptime);
            } else {
                warning() << "slavedelay causing a long sleep of " << sleeptime << " seconds";
                // sleep(hours) would prevent reconfigs from taking effect & such!
                long long waitUntil = b + sleeptime;
                while (time(0) < waitUntil) {
                    sleepsecs(6);

                    // Handle reconfigs that changed the slave delay
                    if (durationCount<Seconds>(replCoord->getSlaveDelaySecs()) != slaveDelaySecs)
                        break;
                }
            }
        }
    }  // endif slaveDelay
}
}

namespace {

class ApplyBatchFinalizer {
public:
    ApplyBatchFinalizer(ReplicationCoordinator* replCoord) : _replCoord(replCoord) {}
    virtual ~ApplyBatchFinalizer(){};

    virtual void record(const OpTime& newOpTime) {
        _recordApplied(newOpTime);
    };

protected:
    void _recordApplied(const OpTime& newOpTime) {
        _replCoord->setMyLastAppliedOpTimeForward(newOpTime);
    }

    void _recordDurable(const OpTime& newOpTime) {
        _replCoord->setMyLastDurableOpTimeForward(newOpTime);
    }

private:
    // Used to update the replication system's progress.
    ReplicationCoordinator* _replCoord;
};

class ApplyBatchFinalizerForJournal : public ApplyBatchFinalizer {
public:
    ApplyBatchFinalizerForJournal(ReplicationCoordinator* replCoord)
        : ApplyBatchFinalizer(replCoord),
          _waiterThread{&ApplyBatchFinalizerForJournal::_run, this} {};
    ~ApplyBatchFinalizerForJournal();

    void record(const OpTime& newOpTime) override;

private:
    /**
     * Loops continuously, waiting for writes to be flushed to disk and then calls
     * ReplicationCoordinator::setMyLastOptime with _latestOpTime.
     * Terminates once _shutdownSignaled is set true.
     */
    void _run();

    // Protects _cond, _shutdownSignaled, and _latestOpTime.
    stdx::mutex _mutex;
    // Used to alert our thread of a new OpTime.
    stdx::condition_variable _cond;
    // The next OpTime to set as the ReplicationCoordinator's lastOpTime after flushing.
    OpTime _latestOpTime;
    // Once this is set to true the _run method will terminate.
    bool _shutdownSignaled = false;
    // Thread that will _run(). Must be initialized last as it depends on the other variables.
    stdx::thread _waiterThread;
};

ApplyBatchFinalizerForJournal::~ApplyBatchFinalizerForJournal() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _shutdownSignaled = true;
    _cond.notify_all();
    lock.unlock();

    _waiterThread.join();
}

void ApplyBatchFinalizerForJournal::record(const OpTime& newOpTime) {
    // We have to use setMyLastAppliedOpTimeForward since this thread races with
    // logTransitionToPrimaryToOplog.
    _recordApplied(newOpTime);

    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _latestOpTime = newOpTime;
    _cond.notify_all();
}

void ApplyBatchFinalizerForJournal::_run() {
    Client::initThread("ApplyBatchFinalizerForJournal");

    while (true) {
        OpTime latestOpTime;

        {
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            while (_latestOpTime.isNull() && !_shutdownSignaled) {
                _cond.wait(lock);
            }

            if (_shutdownSignaled) {
                return;
            }

            latestOpTime = _latestOpTime;
            _latestOpTime = OpTime();
        }

        auto txn = cc().makeOperationContext();
        txn->recoveryUnit()->goingToWaitUntilDurable();
        txn->recoveryUnit()->waitUntilDurable();
        // We have to use setMyLastDurableOpTimeForward since this thread races with
        // logTransitionToPrimaryToOplog.
        _recordDurable(latestOpTime);
    }
}
}  // anonymous namespace containing ApplyBatchFinalizer definitions.

SyncTail::SyncTail(BackgroundSyncInterface* q, MultiSyncApplyFunc func)
    : _networkQueue(q),
      _applyFunc(func),
      _writerPool(replWriterThreadCount, "repl writer worker "),
      _prefetcherPool(replPrefetcherThreadCount, "repl prefetch worker ") {}

SyncTail::~SyncTail() {}

bool SyncTail::peek(BSONObj* op) {
    return _networkQueue->peek(op);
}

// static
Status SyncTail::syncApply(OperationContext* txn,
                           const BSONObj& op,
                           bool convertUpdateToUpsert,
                           ApplyOperationInLockFn applyOperationInLock,
                           ApplyCommandInLockFn applyCommandInLock,
                           IncrementOpsAppliedStatsFn incrementOpsAppliedStats) {
    if (inShutdown()) {
        return Status::OK();
    }

    // Count each log op application as a separate operation, for reporting purposes
    CurOp individualOp(txn);

    const char* ns = op.getStringField("ns");
    verify(ns);

    const char* opType = op["op"].valuestrsafe();

    bool isCommand(opType[0] == 'c');
    bool isNoOp(opType[0] == 'n');

    if ((*ns == '\0') || (*ns == '.')) {
        // this is ugly
        // this is often a no-op
        // but can't be 100% sure
        if (!isNoOp) {
            error() << "skipping bad op in oplog: " << op.toString();
        }
        return Status::OK();
    }

    if (isCommand) {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            // a command may need a global write lock. so we will conservatively go
            // ahead and grab one here. suboptimal. :-(
            Lock::GlobalWrite globalWriteLock(txn->lockState());

            // special case apply for commands to avoid implicit database creation
            Status status = applyCommandInLock(txn, op);
            incrementOpsAppliedStats();
            return status;
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "syncApply_command", ns);
    }

    auto applyOp = [&](Database* db) {
        // For non-initial-sync, we convert updates to upserts
        // to suppress errors when replaying oplog entries.
        txn->setReplicatedWrites(false);
        DisableDocumentValidation validationDisabler(txn);

        Status status =
            applyOperationInLock(txn, db, op, convertUpdateToUpsert, incrementOpsAppliedStats);
        if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
            throw WriteConflictException();
        }
        return status;
    };

    if (isNoOp || (opType[0] == 'i' && nsToCollectionSubstring(ns) == "system.indexes")) {
        auto opStr = isNoOp ? "syncApply_noop" : "syncApply_indexBuild";
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            Lock::DBLock dbLock(txn->lockState(), nsToDatabaseSubstring(ns), MODE_X);
            OldClientContext ctx(txn, ns);
            return applyOp(ctx.db());
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, opStr, ns);
    }

    if (isCrudOpType(opType)) {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            // DB lock always acquires the global lock
            std::unique_ptr<Lock::DBLock> dbLock;
            std::unique_ptr<Lock::CollectionLock> collectionLock;
            std::unique_ptr<OldClientContext> ctx;

            auto dbName = nsToDatabaseSubstring(ns);

            auto resetLocks = [&](LockMode mode) {
                collectionLock.reset();
                dbLock.reset(new Lock::DBLock(txn->lockState(), dbName, mode));
                collectionLock.reset(new Lock::CollectionLock(txn->lockState(), ns, mode));
            };

            resetLocks(MODE_IX);
            if (!dbHolder().get(txn, dbName)) {
                // need to create database, try again
                resetLocks(MODE_X);
                ctx.reset(new OldClientContext(txn, ns));
            } else {
                ctx.reset(new OldClientContext(txn, ns));
                if (!ctx->db()->getCollection(ns)) {
                    // uh, oh, we need to create collection
                    // try again
                    ctx.reset();
                    resetLocks(MODE_X);
                    ctx.reset(new OldClientContext(txn, ns));
                }
            }

            return applyOp(ctx->db());
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "syncApply_CRUD", ns);
    }

    // unknown opType
    str::stream ss;
    ss << "bad opType '" << opType << "' in oplog entry: " << op.toString();
    error() << std::string(ss);
    return Status(ErrorCodes::BadValue, ss);
}

Status SyncTail::syncApply(OperationContext* txn, const BSONObj& op, bool convertUpdateToUpsert) {
    return SyncTail::syncApply(txn,
                               op,
                               convertUpdateToUpsert,
                               applyOperation_inlock,
                               applyCommand_inlock,
                               stdx::bind(&Counter64::increment, &opsAppliedStats, 1ULL));
}


namespace {

// The pool threads call this to prefetch each op
void prefetchOp(const BSONObj& op) {
    initializePrefetchThread();

    const char* ns = op.getStringField("ns");
    if (ns && (ns[0] != '\0')) {
        try {
            // one possible tweak here would be to stay in the read lock for this database
            // for multiple prefetches if they are for the same database.
            OperationContextImpl txn;
            AutoGetCollectionForRead ctx(&txn, ns);
            Database* db = ctx.getDb();
            if (db) {
                prefetchPagesForReplicatedOp(&txn, db, op);
            }
        } catch (const DBException& e) {
            LOG(2) << "ignoring exception in prefetchOp(): " << e.what() << endl;
        } catch (const std::exception& e) {
            log() << "Unhandled std::exception in prefetchOp(): " << e.what() << endl;
            fassertFailed(16397);
        }
    }
}

// Doles out all the work to the reader pool threads and waits for them to complete
void prefetchOps(const std::deque<SyncTail::OplogEntry>& ops, OldThreadPool* prefetcherPool) {
    invariant(prefetcherPool);
    for (auto&& op : ops) {
        prefetcherPool->schedule(&prefetchOp, op.raw);
    }
    prefetcherPool->join();
}

// Doles out all the work to the writer pool threads and waits for them to complete
void applyOps(const std::vector<std::vector<SyncTail::OplogEntry>>& writerVectors,
              OldThreadPool* writerPool,
              SyncTail::MultiSyncApplyFunc func,
              SyncTail* sync) {
    TimerHolder timer(&applyBatchStats);
    for (std::vector<std::vector<SyncTail::OplogEntry>>::const_iterator it = writerVectors.begin();
         it != writerVectors.end();
         ++it) {
        if (!it->empty()) {
            writerPool->schedule(func, stdx::cref(*it), sync);
        }
    }
}

/**
 * A caching functor that returns true if a namespace refers to a capped collection.
 * Collections that don't exist are implicitly not capped.
 */
class CachingCappedChecker {
public:
    bool operator()(OperationContext* txn, const StringMapTraits::HashedKey& ns) {
        auto it = _cache.find(ns);
        if (it != _cache.end()) {
            return it->second;
        }

        bool isCapped = isCappedImpl(txn, ns.key());
        _cache[ns] = isCapped;
        return isCapped;
    }

private:
    bool isCappedImpl(OperationContext* txn, StringData ns) {
        auto db = dbHolder().get(txn, ns);
        if (!db)
            return false;

        auto collection = db->getCollection(ns);
        return collection && collection->isCapped();
    }

    StringMap<bool> _cache;
};

void fillWriterVectors(OperationContext* txn,
                       const std::deque<SyncTail::OplogEntry>& ops,
                       std::vector<std::vector<SyncTail::OplogEntry>>* writerVectors) {
    const bool supportsDocLocking =
        getGlobalServiceContext()->getGlobalStorageEngine()->supportsDocLocking();
    const uint32_t numWriters = writerVectors->size();

    Lock::GlobalRead globalReadLock(txn->lockState());

    CachingCappedChecker isCapped;

    for (auto&& op : ops) {
        StringMapTraits::HashedKey hashedNs(op.ns);
        uint32_t hash = hashedNs.hash();

        const char* opType = op.opType.rawData();

        // For doc locking engines, include the _id of the document in the hash so we get
        // parallelism even if all writes are to a single collection. We can't do this for capped
        // collections because the order of inserts is a guaranteed property, unlike for normal
        // collections.
        if (supportsDocLocking && isCrudOpType(opType) && !isCapped(txn, hashedNs)) {
            BSONElement id;
            switch (opType[0]) {
                case 'u':
                    id = op.o2.Obj()["_id"];
                    break;
                case 'd':
                case 'i':
                    id = op.o.Obj()["_id"];
                    break;
            }

            const size_t idHash = BSONElement::Hasher()(id);
            MurmurHash3_x86_32(&idHash, sizeof(idHash), hash, &hash);
        }

        if (op.opType == "i" && isCapped(txn, hashedNs)) {
            // Mark capped collection ops before storing them to ensure we do not attempt to bulk
            // insert them.
            SyncTail::OplogEntry modifiedOp = op;
            modifiedOp.isForCappedCollection = true;
            (*writerVectors)[hash % numWriters].push_back(modifiedOp);
        } else {
            (*writerVectors)[hash % numWriters].push_back(op);
        }
    }
}

}  // namespace

// Applies a batch of oplog entries, by using a set of threads to apply the operations and then
// writes the oplog entries to the local oplog.
OpTime SyncTail::multiApply(OperationContext* txn, const OpQueue& ops) {
    invariant(_applyFunc);

    if (getGlobalServiceContext()->getGlobalStorageEngine()->isMmapV1()) {
        // Use a ThreadPool to prefetch all the operations in a batch.
        prefetchOps(ops.getDeque(), &_prefetcherPool);
    }

    std::vector<std::vector<SyncTail::OplogEntry>> writerVectors(replWriterThreadCount);

    fillWriterVectors(txn, ops.getDeque(), &writerVectors);
    LOG(2) << "replication batch size is " << ops.getDeque().size() << endl;
    // We must grab this because we're going to grab write locks later.
    // We hold this mutex the entire time we're writing; it doesn't matter
    // because all readers are blocked anyway.
    stdx::lock_guard<SimpleMutex> fsynclk(filesLockedFsync);

    // stop all readers until we're done
    Lock::ParallelBatchWriterMode pbwm(txn->lockState());

    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
    if (replCoord->getMemberState().primary() && !replCoord->isWaitingForApplierToDrain()) {
        severe() << "attempting to replicate ops while primary";
        fassertFailed(28527);
    }

    applyOps(writerVectors, &_writerPool, _applyFunc, this);

    OpTime lastOpTime;
    {
        ON_BLOCK_EXIT([&] { _writerPool.join(); });
        std::vector<BSONObj> raws;
        raws.reserve(ops.getDeque().size());
        for (auto&& op : ops.getDeque()) {
            raws.emplace_back(op.raw);
        }
        lastOpTime = writeOpsToOplog(txn, raws);
    }

    if (inShutdownStrict()) {
        log() << "Cannot apply operations due to shutdown in progress";
        return OpTime();
    }
    // We have now written all database writes and updated the oplog to match.
    return lastOpTime;
}

namespace {
void tryToGoLiveAsASecondary(OperationContext* txn,
                             ReplicationCoordinator* replCoord,
                             const BatchBoundaries& minValidBoundaries,
                             const OpTime& lastWriteOpTime) {
    if (replCoord->isInPrimaryOrSecondaryState()) {
        return;
    }

    ScopedTransaction transaction(txn, MODE_S);
    Lock::GlobalRead readLock(txn->lockState());

    if (replCoord->getMaintenanceMode()) {
        // we're not actually going live
        return;
    }

    // Only state RECOVERING can transition to SECONDARY.
    MemberState state(replCoord->getMemberState());
    if (!state.recovering()) {
        return;
    }

    // If an apply batch is active then we cannot transition.
    if (!minValidBoundaries.start.isNull()) {
        return;
    }

    // Must have applied/written to minvalid, so return if not.
    // -- If 'lastWriteOpTime' is null/uninitialized then we can't transition.
    // -- If 'lastWriteOpTime' is less than the end of the last batch then we can't transition.
    if (lastWriteOpTime.isNull() || minValidBoundaries.end > lastWriteOpTime) {
        return;
    }

    bool worked = replCoord->setFollowerMode(MemberState::RS_SECONDARY);
    if (!worked) {
        warning() << "Failed to transition into " << MemberState(MemberState::RS_SECONDARY)
                  << ". Current state: " << replCoord->getMemberState();
    }
}
}

class SyncTail::OpQueueBatcher {
    MONGO_DISALLOW_COPYING(OpQueueBatcher);

public:
    explicit OpQueueBatcher(SyncTail* syncTail) : _syncTail(syncTail), _thread([&] { run(); }) {}
    ~OpQueueBatcher() {
        _inShutdown.store(true);
        _cv.notify_all();
        _thread.join();
    }

    OpQueue getNextBatch(Seconds maxWaitTime) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        if (_ops.empty()) {
            // We intentionally don't care about whether this returns due to signaling or timeout
            // since we do the same thing either way: return whatever is in _ops.
            (void)_cv.wait_for(lk, maxWaitTime);
        }

        OpQueue ops = std::move(_ops);
        _ops = {};
        _cv.notify_all();

        return ops;
    }

private:
    void run() {
        Client::initThread("ReplBatcher");
        OperationContextImpl txn;
        auto replCoord = ReplicationCoordinator::get(&txn);

        while (!_inShutdown.load()) {
            Timer batchTimer;

            OpQueue ops;
            // tryPopAndWaitForMore returns true when we need to end a batch early
            while (!_syncTail->tryPopAndWaitForMore(&txn, &ops) &&
                   (ops.getSize() < replBatchLimitBytes) && !_inShutdown.load()) {
                int now = batchTimer.seconds();

                // apply replication batch limits
                if (!ops.empty()) {
                    if (now > replBatchLimitSeconds)
                        break;
                    if (ops.getDeque().size() > replBatchLimitOperations)
                        break;
                }

                const int slaveDelaySecs = durationCount<Seconds>(replCoord->getSlaveDelaySecs());
                if (!ops.empty() && slaveDelaySecs > 0) {
                    const BSONObj lastOp = ops.back().raw;
                    const unsigned int opTimestampSecs = lastOp["ts"].timestamp().getSecs();

                    // Stop the batch as the lastOp is too new to be applied. If we continue
                    // on, we can get ops that are way ahead of the delay and this will
                    // make this thread sleep longer when handleSlaveDelay is called
                    // and apply ops much sooner than we like.
                    if (opTimestampSecs > static_cast<unsigned int>(time(0) - slaveDelaySecs)) {
                        break;
                    }
                }

                if (MONGO_FAIL_POINT(rsSyncApplyStop)) {
                    break;
                }

                // keep fetching more ops as long as we haven't filled up a full batch yet
            }

            // For pausing replication in tests
            while (MONGO_FAIL_POINT(rsSyncApplyStop) && !_inShutdown.load()) {
                sleepmillis(0);
            }

            stdx::unique_lock<stdx::mutex> lk(_mutex);
            while (!_ops.empty()) {
                // Block until the previous batch has been taken.
                if (_inShutdown.load())
                    return;
                _cv.wait(lk);
            }
            _ops = std::move(ops);
            _cv.notify_all();
        }
    }

    AtomicWord<bool> _inShutdown;
    SyncTail* const _syncTail;

    stdx::mutex _mutex;  // Guards _ops.
    stdx::condition_variable _cv;
    OpQueue _ops;

    stdx::thread _thread;  // Must be last so all other members are initialized before starting.
};

/* tail an oplog.  ok to return, will be re-called. */
void SyncTail::oplogApplication() {
    OpQueueBatcher batcher(this);

    OperationContextImpl txn;
    auto replCoord = ReplicationCoordinator::get(&txn);
    std::unique_ptr<ApplyBatchFinalizer> finalizer{
        getGlobalServiceContext()->getGlobalStorageEngine()->isDurable()
            ? new ApplyBatchFinalizerForJournal(replCoord)
            : new ApplyBatchFinalizer(replCoord)};

    auto minValidBoundaries = getMinValid(&txn);
    OpTime originalEndOpTime(minValidBoundaries.end);
    OpTime lastWriteOpTime{replCoord->getMyLastAppliedOpTime()};
    while (!inShutdown()) {
        OpQueue ops;

        do {
            if (BackgroundSync::get()->getInitialSyncRequestedFlag()) {
                // got a resync command
                return;
            }

            tryToGoLiveAsASecondary(&txn, replCoord, minValidBoundaries, lastWriteOpTime);

            // Blocks up to a second waiting for a batch to be ready to apply. If one doesn't become
            // ready in time, we'll loop again so we can do the above checks periodically.
            ops = batcher.getNextBatch(Seconds(1));
        } while (!inShutdown() && ops.empty());

        if (inShutdown())
            return;

        invariant(!ops.empty());

        const BSONObj lastOp = ops.back().raw;

        if (lastOp.isEmpty()) {
            // This means that the network thread has coalesced and we have processed all of its
            // data.
            invariant(ops.getDeque().size() == 1);
            if (replCoord->isWaitingForApplierToDrain()) {
                replCoord->signalDrainComplete(&txn);
            }

            // Reset when triggered in case it was from a rollback, safe to do at any time.
            lastWriteOpTime = replCoord->getMyLastAppliedOpTime();

            continue;  // This wasn't a real op. Don't try to apply it.
        }

        const auto lastOpTime = fassertStatusOK(28773, OpTime::parseFromOplogEntry(lastOp));
        if (lastWriteOpTime >= lastOpTime) {
            // Error for the oplog to go back in time.
            fassert(34361,
                    Status(ErrorCodes::OplogOutOfOrder,
                           str::stream() << "Attempted to apply an oplog entry ("
                                         << lastOpTime.toString()
                                         << ") which is not greater than our lastWrittenOptime ("
                                         << lastWriteOpTime.toString() << ")."));
        }

        handleSlaveDelay(lastOpTime.getTimestamp());

        // Set minValid to the last OpTime that needs to be applied, in this batch or from the
        // (last) failed batch, whichever is larger.
        // This will cause this node to go into RECOVERING state
        // if we should crash and restart before updating finishing.
        const OpTime start(getLastSetTimestamp(), OpTime::kUninitializedTerm);


        // Take the max of the first endOptime (if we recovered) and the end of our batch.

        // Setting end to the max of originalEndOpTime and lastOpTime (the end of the batch)
        // ensures that we keep pushing out the point where we can become consistent
        // and allow reads. If we recover and end up doing smaller batches we must pass the
        // originalEndOpTime before we are good.
        //
        // For example:
        // batch apply, 20-40, end = 40
        // batch failure,
        // restart
        // batch apply, 20-25, end = max(25, 40) = 40
        // batch apply, 25-45, end = 45
        const OpTime end(std::max(originalEndOpTime, lastOpTime));

        // This write will not journal/checkpoint.
        setMinValid(&txn, {start, end});

        lastWriteOpTime = multiApply(&txn, ops);
        if (lastWriteOpTime.isNull()) {
            // fassert if oplog application failed for any reasons other than shutdown.
            error() << "Failed to apply " << ops.getDeque().size()
                    << " operations - batch start:" << start << " end:" << end;
            fassert(34360, inShutdownStrict());
            // Return without setting minvalid in the case of shutdown.
            return;
        }

        setNewTimestamp(lastWriteOpTime.getTimestamp());
        setMinValid(&txn, end, DurableRequirement::None);
        minValidBoundaries.start = {};
        minValidBoundaries.end = end;
        finalizer->record(lastWriteOpTime);
    }
}

SyncTail::OplogEntry::OplogEntry(const BSONObj& rawInput) : raw(rawInput.getOwned()) {
    for (auto elem : raw) {
        const auto name = elem.fieldNameStringData();
        if (name == "ns") {
            ns = elem.valuestrsafe();
        } else if (name == "op") {
            opType = elem.valuestrsafe();
        } else if (name == "o2") {
            o2 = elem;
        } else if (name == "v") {
            version = elem;
        } else if (name == "o") {
            o = elem;
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
bool SyncTail::tryPopAndWaitForMore(OperationContext* txn, SyncTail::OpQueue* ops) {
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

    auto entry = OplogEntry(op);

    // Check for ops that must be processed one at a time.
    if (entry.raw.isEmpty() ||       // sentinel that network queue is drained.
        (entry.opType[0] == 'c') ||  // commands.
        // Index builds are acheived through the use of an insert op, not a command op.
        // The following line is the same as what the insert code uses to detect an index build.
        (!entry.ns.empty() && nsToCollectionSubstring(entry.ns) == "system.indexes")) {
        if (ops->empty()) {
            // apply commands one-at-a-time
            ops->push_back(std::move(entry));
            _networkQueue->consume();
        }

        // otherwise, apply what we have so far and come back for the command
        return true;
    }

    // check for oplog version change
    int curVersion = 0;
    if (entry.version.eoo())
        // missing version means version 1
        curVersion = 1;
    else
        curVersion = entry.version.Int();

    if (curVersion != OPLOG_VERSION) {
        severe() << "expected oplog version " << OPLOG_VERSION << " but found version "
                 << curVersion << " in oplog entry: " << op;
        fassertFailedNoTrace(18820);
    }

    // Copy the op to the deque and remove it from the bgsync queue.
    ops->push_back(std::move(entry));
    _networkQueue->consume();

    // Go back for more ops
    return false;
}

void SyncTail::setHostname(const std::string& hostname) {
    _hostname = hostname;
}

BSONObj SyncTail::getMissingDoc(OperationContext* txn, Database* db, const BSONObj& o) {
    OplogReader missingObjReader;  // why are we using OplogReader to run a non-oplog query?
    const char* ns = o.getStringField("ns");

    // capped collections
    Collection* collection = db->getCollection(ns);
    if (collection && collection->isCapped()) {
        log() << "missing doc, but this is okay for a capped collection (" << ns << ")";
        return BSONObj();
    }

    const int retryMax = 3;
    for (int retryCount = 1; retryCount <= retryMax; ++retryCount) {
        if (retryCount != 1) {
            // if we are retrying, sleep a bit to let the network possibly recover
            sleepsecs(retryCount * retryCount);
        }
        try {
            bool ok = missingObjReader.connect(HostAndPort(_hostname));
            if (!ok) {
                warning() << "network problem detected while connecting to the "
                          << "sync source, attempt " << retryCount << " of " << retryMax << endl;
                continue;  // try again
            }
        } catch (const SocketException&) {
            warning() << "network problem detected while connecting to the "
                      << "sync source, attempt " << retryCount << " of " << retryMax << endl;
            continue;  // try again
        }

        // get _id from oplog entry to create query to fetch document.
        const BSONElement opElem = o.getField("op");
        const bool isUpdate = !opElem.eoo() && opElem.str() == "u";
        const BSONElement idElem = o.getObjectField(isUpdate ? "o2" : "o")["_id"];

        if (idElem.eoo()) {
            severe() << "cannot fetch missing document without _id field: " << o.toString();
            fassertFailedNoTrace(28742);
        }

        BSONObj query = BSONObjBuilder().append(idElem).obj();
        BSONObj missingObj;
        try {
            missingObj = missingObjReader.findOne(ns, query);
        } catch (const SocketException&) {
            warning() << "network problem detected while fetching a missing document from the "
                      << "sync source, attempt " << retryCount << " of " << retryMax << endl;
            continue;  // try again
        } catch (DBException& e) {
            error() << "assertion fetching missing object: " << e.what() << endl;
            throw;
        }

        // success!
        return missingObj;
    }
    // retry count exceeded
    msgasserted(15916,
                str::stream() << "Can no longer connect to initial sync source: " << _hostname);
}

bool SyncTail::shouldRetry(OperationContext* txn, const BSONObj& o) {
    const NamespaceString nss(o.getStringField("ns"));
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        // Take an X lock on the database in order to preclude other modifications.
        // Also, the database might not exist yet, so create it.
        AutoGetOrCreateDb autoDb(txn, nss.db(), MODE_X);
        Database* const db = autoDb.getDb();

        // we don't have the object yet, which is possible on initial sync.  get it.
        log() << "adding missing object" << endl;  // rare enough we can log

        BSONObj missingObj = getMissingDoc(txn, db, o);

        if (missingObj.isEmpty()) {
            log() << "missing object not found on source."
                     " presumably deleted later in oplog";
            log() << "o2: " << o.getObjectField("o2").toString();
            log() << "o firstfield: " << o.getObjectField("o").firstElementFieldName();

            return false;
        } else {
            WriteUnitOfWork wunit(txn);

            Collection* const coll = db->getOrCreateCollection(txn, nss.toString());
            invariant(coll);

            Status status = coll->insertDocument(txn, missingObj, true);
            uassert(15917,
                    str::stream() << "failed to insert missing doc: " << status.toString(),
                    status.isOK());

            LOG(1) << "inserted missing doc: " << missingObj.toString() << endl;

            wunit.commit();
            return true;
        }
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "InsertRetry", nss.ns());

    // fixes compile errors on GCC - see SERVER-18219 for details
    MONGO_UNREACHABLE;
}

static void initializeWriterThread() {
    // Only do this once per thread
    if (!ClientBasic::getCurrent()) {
        Client::initThreadIfNotAlready();
        AuthorizationSession::get(cc())->grantInternalAuthorization();
    }
}

// This free function is used by the writer threads to apply each op
void multiSyncApply(const std::vector<SyncTail::OplogEntry>& ops, SyncTail* st) {
    using OplogEntry = SyncTail::OplogEntry;

    std::vector<OplogEntry> oplogEntries(ops.begin(), ops.end());
    std::vector<OplogEntry*> oplogEntryPointers(oplogEntries.size());
    for (size_t i = 0; i < oplogEntries.size(); i++) {
        oplogEntryPointers[i] = &oplogEntries[i];
    }

    if (oplogEntryPointers.size() > 1) {
        std::stable_sort(oplogEntryPointers.begin(),
                         oplogEntryPointers.end(),
                         [](OplogEntry* l, OplogEntry* r) { return l->ns < r->ns; });
    }
    initializeWriterThread();

    OperationContextImpl txn;
    txn.setReplicatedWrites(false);
    DisableDocumentValidation validationDisabler(&txn);

    // allow us to get through the magic barrier
    txn.lockState()->setIsBatchWriter(true);

    bool convertUpdatesToUpserts = true;
    // doNotGroupBeforePoint is used to prevent retrying bad group inserts by marking the final op
    // of a failed group and not allowing further group inserts until that op has been processed.
    std::vector<OplogEntry*>::iterator doNotGroupBeforePoint = oplogEntryPointers.begin();

    for (std::vector<OplogEntry*>::iterator oplogEntriesIterator = oplogEntryPointers.begin();
         oplogEntriesIterator != oplogEntryPointers.end();
         ++oplogEntriesIterator) {
        OplogEntry* entry = *oplogEntriesIterator;
        if (entry->opType[0] == 'i' && !entry->isForCappedCollection &&
            oplogEntriesIterator > doNotGroupBeforePoint) {
            // Attempt to group inserts if possible.
            std::vector<BSONObj> toInsert;
            int batchSize = 0;
            int batchCount = 0;
            auto endOfGroupableOpsIterator = std::find_if(
                oplogEntriesIterator + 1,
                oplogEntryPointers.end(),
                [&](OplogEntry* nextEntry) {
                    return nextEntry->opType[0] != 'i' ||  // Must be an insert.
                        nextEntry->ns != entry->ns ||      // Must be the same namespace.
                        // Must not create too large an object.
                        (batchSize += nextEntry->o.Obj().objsize()) > insertVectorMaxBytes ||
                        ++batchCount >= 64;  // Or have too many entries.
                });

            if (endOfGroupableOpsIterator != oplogEntriesIterator + 1) {
                // Since we found more than one document, create grouped insert of many docs.
                BSONObjBuilder groupedInsertBuilder;
                // Generate an op object of all elements except for "o", since we need to
                // make the "o" field an array of all the o's.
                for (auto elem : entry->raw) {
                    if (elem.fieldNameStringData() != "o") {
                        groupedInsertBuilder.append(elem);
                    }
                }

                // Populate the "o" field with all the groupable inserts.
                BSONArrayBuilder insertArrayBuilder(groupedInsertBuilder.subarrayStart("o"));
                for (std::vector<OplogEntry*>::iterator groupingIterator = oplogEntriesIterator;
                     groupingIterator != endOfGroupableOpsIterator;
                     ++groupingIterator) {
                    insertArrayBuilder.append((*groupingIterator)->o.Obj());
                }
                insertArrayBuilder.done();

                try {
                    // Apply the group of inserts.
                    uassertStatusOK(SyncTail::syncApply(
                        &txn, groupedInsertBuilder.done(), convertUpdatesToUpserts));
                    // It succeeded, advance the oplogEntriesIterator to the end of the
                    // group of inserts.
                    oplogEntriesIterator = endOfGroupableOpsIterator - 1;
                    continue;
                } catch (const DBException& e) {
                    // The group insert failed, log an error and fall through to the
                    // application of an individual op.
                    error() << "Error applying inserts in bulk " << causedBy(e)
                            << " trying first insert as a lone insert";

                    if (inShutdown()) {
                        return;
                    }

                    // Avoid quadratic run time from failed insert by not retrying until we
                    // are beyond this group of ops.
                    doNotGroupBeforePoint = endOfGroupableOpsIterator - 1;
                }
            }
        }

        try {
            // Apply an individual (non-grouped) op.
            const Status s = SyncTail::syncApply(&txn, entry->raw, convertUpdatesToUpserts);

            if (!s.isOK()) {
                severe() << "Error applying operation (" << entry->raw.toString() << "): " << s;
                if (inShutdown()) {
                    return;
                }
                fassertFailedNoTrace(16359);
            }
        } catch (const DBException& e) {
            severe() << "writer worker caught exception: " << causedBy(e)
                     << " on: " << entry->raw.toString();

            if (inShutdown()) {
                return;
            }

            fassertFailedNoTrace(16360);
        }
    }
}

// This free function is used by the initial sync writer threads to apply each op
void multiInitialSyncApply(const std::vector<SyncTail::OplogEntry>& ops, SyncTail* st) {
    initializeWriterThread();

    OperationContextImpl txn;
    txn.setReplicatedWrites(false);
    DisableDocumentValidation validationDisabler(&txn);

    // allow us to get through the magic barrier
    txn.lockState()->setIsBatchWriter(true);

    bool convertUpdatesToUpserts = false;

    for (std::vector<SyncTail::OplogEntry>::const_iterator it = ops.begin(); it != ops.end();
         ++it) {
        try {
            const Status s = SyncTail::syncApply(&txn, it->raw, convertUpdatesToUpserts);
            if (!s.isOK()) {
                if (st->shouldRetry(&txn, it->raw)) {
                    const Status s2 = SyncTail::syncApply(&txn, it->raw, convertUpdatesToUpserts);
                    if (!s2.isOK()) {
                        severe() << "Error applying operation (" << it->raw.toString()
                                 << "): " << s2;
                        fassertFailedNoTrace(15915);
                    }
                }

                // If shouldRetry() returns false, fall through.
                // This can happen if the document that was moved and missed by Cloner
                // subsequently got deleted and no longer exists on the Sync Target at all
            }
        } catch (const DBException& e) {
            severe() << "writer worker caught exception: " << causedBy(e)
                     << " on: " << it->raw.toString();

            if (inShutdown()) {
                return;
            }

            fassertFailedNoTrace(16361);
        }
    }
}

}  // namespace repl
}  // namespace mongo
