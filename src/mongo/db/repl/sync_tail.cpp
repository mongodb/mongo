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
#include "mongo/db/repl/bgsync.h"
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

// Number of times we tried to go live as a secondary.
static Counter64 attemptsToBecomeSecondary;
static ServerStatusMetricField<Counter64> displayAttemptsToBecomeSecondary(
    "repl.apply.attemptsToBecomeSecondary", &attemptsToBecomeSecondary);

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
                           bool inSteadyStateReplication,
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
            Status status = applyCommandInLock(txn, op, inSteadyStateReplication);
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

        Status status = applyOperationInLock(txn, db, op, inSteadyStateReplication);
        if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
            throw WriteConflictException();
        }
        incrementOpsAppliedStats();
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
                // Warning: We must reset the pointer to nullptr first, in order to ensure that we
                // drop the DB lock before acquiring
                // the upgraded one.
                dbLock.reset();
                dbLock.reset(new Lock::DBLock(txn->lockState(), dbName, mode));
                collectionLock.reset(new Lock::CollectionLock(txn->lockState(), ns, mode));
            };

            resetLocks(MODE_IX);
            if (!dbHolder().get(txn, dbName)) {
                // Need to create database, so reset lock to stronger mode.
                resetLocks(MODE_X);
                ctx.reset(new OldClientContext(txn, ns));
            } else {
                ctx.reset(new OldClientContext(txn, ns));
                if (!ctx->db()->getCollection(ns)) {
                    // Need to implicitly create collection.  This occurs for 'u' opTypes,
                    // but not for 'i' nor 'd'.
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

Status SyncTail::syncApply(OperationContext* txn,
                           const BSONObj& op,
                           bool inSteadyStateReplication) {
    return syncApply(txn,
                     op,
                     inSteadyStateReplication,
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
void applyOps(const std::vector<std::vector<BSONObj>>& writerVectors,
              OldThreadPool* writerPool,
              SyncTail::MultiSyncApplyFunc func,
              SyncTail* sync) {
    TimerHolder timer(&applyBatchStats);
    for (std::vector<std::vector<BSONObj>>::const_iterator it = writerVectors.begin();
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
                       std::vector<std::vector<BSONObj>>* writerVectors) {
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

        (*writerVectors)[hash % numWriters].push_back(op.raw);
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

    std::vector<std::vector<BSONObj>> writerVectors(replWriterThreadCount);

    fillWriterVectors(txn, ops.getDeque(), &writerVectors);
    LOG(2) << "replication batch size is " << ops.getDeque().size() << endl;
    // We must grab this because we're going to grab write locks later.
    // We hold this mutex the entire time we're writing; it doesn't matter
    // because all readers are blocked anyway.
    stdx::lock_guard<SimpleMutex> fsynclk(filesLockedFsync);

    // stop all readers until we're done
    Lock::ParallelBatchWriterMode pbwm(txn->lockState());

    if (inShutdownStrict()) {
        log() << "Cannot apply operations due to shutdown in progress";
        return OpTime();
    }

    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
    if (replCoord->getMemberState().primary() && !replCoord->isWaitingForApplierToDrain()) {
        severe() << "attempting to replicate ops while primary";
        fassertFailed(28527);
    }

    // Since we write the oplog from a single thread in-order, we don't need to use the
    // oplogDeleteFromPoint.
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

    setMinValidToAtLeast(txn, lastOpTime);  // Mark us as in the middle of a batch.

    applyOps(writerVectors, &_writerPool, _applyFunc, this);
    _writerPool.join();

    // Due to SERVER-24933 we can't enter inShutdown while holding the PBWM lock.
    invariant(!inShutdownStrict());

    setAppliedThrough(txn, lastOpTime);  // Mark batch as complete.

    return lastOpTime;
}

namespace {
void tryToGoLiveAsASecondary(OperationContext* txn, ReplicationCoordinator* replCoord) {
    if (replCoord->isInPrimaryOrSecondaryState()) {
        return;
    }

    // This needs to happen after the attempt so readers can be sure we've already tried.
    ON_BLOCK_EXIT([] { attemptsToBecomeSecondary.increment(); });

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

    // We can't go to SECONDARY until we reach minvalid.
    if (replCoord->getMyLastAppliedOpTime() < getMinValid(txn)) {
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
    explicit OpQueueBatcher(SyncTail* syncTail, StorageInterface* storageInterface)
        : _syncTail(syncTail), _storageInterface(storageInterface), _thread([&] { run(); }) {}
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

        const auto oplogMaxSize = fassertStatusOK(
            40301, _storageInterface->getOplogMaxSize(&txn, NamespaceString(rsOplogName)));

        // Batches are limited to 10% of the oplog.
        BatchLimits batchLimits;
        batchLimits.ops = replBatchLimitOperations;
        batchLimits.bytes = std::min(oplogMaxSize / 10, size_t(replBatchLimitBytes));
        while (!_inShutdown.load()) {
            const auto slaveDelay = replCoord->getSlaveDelaySecs();
            batchLimits.slaveDelayLatestTimestamp = (slaveDelay > Seconds(0))
                ? (Date_t::now() - slaveDelay)
                : boost::optional<Date_t>();

            OpQueue ops;
            // tryPopAndWaitForMore adds to ops and returns true when we need to end a batch early.
            while (!_inShutdown.load() &&
                   !_syncTail->tryPopAndWaitForMore(&txn, &ops, batchLimits)) {
            }

            // For pausing replication in tests
            while (MONGO_FAIL_POINT(rsSyncApplyStop) && !_inShutdown.load()) {
                sleepmillis(0);
            }

            if (ops.empty()) {
                continue;  // Don't emit empty batches.
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
    StorageInterface* const _storageInterface;

    stdx::mutex _mutex;  // Guards _ops.
    stdx::condition_variable _cv;
    OpQueue _ops;

    stdx::thread _thread;  // Must be last so all other members are initialized before starting.
};

/* tail an oplog.  ok to return, will be re-called. */
void SyncTail::oplogApplication(StorageInterface* storageInterface) {
    OpQueueBatcher batcher(this, storageInterface);

    OperationContextImpl txn;
    auto replCoord = ReplicationCoordinator::get(&txn);
    std::unique_ptr<ApplyBatchFinalizer> finalizer{
        getGlobalServiceContext()->getGlobalStorageEngine()->isDurable()
            ? new ApplyBatchFinalizerForJournal(replCoord)
            : new ApplyBatchFinalizer(replCoord)};

    while (!inShutdown()) {
        OpQueue ops;

        do {
            if (BackgroundSync::get()->getInitialSyncRequestedFlag()) {
                // got a resync command
                return;
            }

            tryToGoLiveAsASecondary(&txn, replCoord);

            // Blocks up to a second waiting for a batch to be ready to apply. If one doesn't become
            // ready in time, we'll loop again so we can do the above checks periodically.
            ops = batcher.getNextBatch(Seconds(1));
        } while (!inShutdown() && ops.empty());

        if (inShutdown())
            return;

        invariant(!ops.empty());

        if (ops.front().raw.isEmpty()) {
            // This means that the network thread has coalesced and we have processed all of its
            // data.
            invariant(ops.getDeque().size() == 1);
            if (replCoord->isWaitingForApplierToDrain()) {
                replCoord->signalDrainComplete(&txn);
            }

            continue;  // This wasn't a real op. Don't try to apply it.
        }

        // Extract some info from ops that we'll need after releasing the batch below.
        const auto firstOpTimeInBatch =
            fassertStatusOK(40299, OpTime::parseFromOplogEntry(ops.front().raw));
        const auto lastOpTimeInBatch =
            fassertStatusOK(28773, OpTime::parseFromOplogEntry(ops.back().raw));

        // Make sure the oplog doesn't go back in time or repeat an entry.
        if (firstOpTimeInBatch <= replCoord->getMyLastAppliedOpTime()) {
            fassert(34361,
                    Status(ErrorCodes::OplogOutOfOrder,
                           str::stream() << "Attempted to apply an oplog entry ("
                                         << firstOpTimeInBatch.toString()
                                         << ") which is not greater than our last applied OpTime ("
                                         << replCoord->getMyLastAppliedOpTime().toString()
                                         << ")."));
        }

        const bool fail = multiApply(&txn, ops).isNull();
        if (fail) {
            // fassert if oplog application failed for any reasons other than shutdown.
            error() << "Failed to apply " << ops.getDeque().size()
                    << " operations - batch start:" << firstOpTimeInBatch
                    << " end:" << lastOpTimeInBatch;
            fassert(34360, inShutdownStrict());
            // Return without setting minvalid in the case of shutdown.
            return;
        }

        // Update various things that care about our last applied optime.
        setNewTimestamp(lastOpTimeInBatch.getTimestamp());
        finalizer->record(lastOpTimeInBatch);
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
        } else if (name == "ts") {
            ts = elem;
        }
    }
}

// Copies ops out of the bgsync queue into the deque passed in as a parameter.
// Returns true if the batch should be ended early.
// Batch should end early if we encounter a command, or if
// there are no further ops in the bgsync queue to read.
// This function also blocks 1 second waiting for new ops to appear in the bgsync
// queue.  We don't block forever so that we can periodically check for things like shutdown or
// reconfigs.
bool SyncTail::tryPopAndWaitForMore(OperationContext* txn,
                                    SyncTail::OpQueue* ops,
                                    const BatchLimits& limits) {
    BSONObj op;
    // Check to see if there are ops waiting in the bgsync queue
    bool peek_success = peek(&op);

    if (!peek_success) {
        // If we don't have anything in the queue, wait a bit for something to appear.
        if (ops->empty()) {
            // Block up to 1 second. We still return true in this case because we want this
            // op to be the first in a new batch with a new start time.
            _networkQueue->waitForMore();
        }

        return true;
    }

    // If this op would put us over the byte limit don't include it unless the batch is empty.
    // We allow single-op batches to exceed the byte limit so that large ops are able to be
    // processed.
    if (!ops->empty() && (ops->getSize() + size_t(op.objsize())) > limits.bytes) {
        return true;  // Return before wasting time parsing the op.
    }

    auto entry = OplogEntry(op);

    if (!entry.raw.isEmpty()) {
        // check for oplog version change
        int curVersion = 0;
        if (entry.version.eoo()) {
            // missing version means version 1
            curVersion = 1;
        } else {
            curVersion = entry.version.Int();
        }

        if (curVersion != OPLOG_VERSION) {
            severe() << "expected oplog version " << OPLOG_VERSION << " but found version "
                     << curVersion << " in oplog entry: " << op;
            fassertFailedNoTrace(18820);
        }
    }

    if (limits.slaveDelayLatestTimestamp &&
        entry.ts.timestampTime() > *limits.slaveDelayLatestTimestamp) {
        if (ops->empty()) {
            // Sleep if we've got nothing to do. Only sleep for 1 second at a time to allow
            // reconfigs and shutdown to occur.
            sleepsecs(1);
        }
        return true;
    }

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

    // Copy the op to the deque and remove it from the bgsync queue.
    ops->push_back(std::move(entry));
    _networkQueue->consume();

    // Go back for more ops, unless we've hit the limit.
    return ops->getDeque().size() >= limits.ops;
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
void multiSyncApply(const std::vector<BSONObj>& ops, SyncTail* st) {
    initializeWriterThread();

    OperationContextImpl txn;
    txn.setReplicatedWrites(false);
    DisableDocumentValidation validationDisabler(&txn);

    // allow us to get through the magic barrier
    txn.lockState()->setIsBatchWriter(true);

    // This function is only called in steady state replication.
    bool inSteadyStateReplication = true;

    for (std::vector<BSONObj>::const_iterator it = ops.begin(); it != ops.end(); ++it) {
        try {
            const Status s = SyncTail::syncApply(&txn, *it, inSteadyStateReplication);
            if (!s.isOK()) {
                severe() << "Error applying operation (" << it->toString() << "): " << s;
                fassertFailedNoTrace(16359);
            }
        } catch (const DBException& e) {
            severe() << "writer worker caught exception: " << causedBy(e)
                     << " on: " << it->toString();

            if (inShutdown()) {
                return;
            }

            fassertFailedNoTrace(16360);
        }
    }
}

// This free function is used by the initial sync writer threads to apply each op
void multiInitialSyncApply(const std::vector<BSONObj>& ops, SyncTail* st) {
    initializeWriterThread();

    OperationContextImpl txn;
    Status status = multiInitialSyncApply_noAbort(&txn, ops, st);
    fassertNoTrace(15915, status);
}

Status multiInitialSyncApply_noAbort(OperationContext* txn,
                                     const std::vector<BSONObj>& ops,
                                     SyncTail* st) {
    txn->setReplicatedWrites(false);
    DisableDocumentValidation validationDisabler(txn);

    // allow us to get through the magic barrier
    txn->lockState()->setIsBatchWriter(true);

    // This function is only called in initial sync, as its name suggests.
    bool inSteadyStateReplication = false;

    for (std::vector<BSONObj>::const_iterator it = ops.begin(); it != ops.end(); ++it) {
        try {
            const Status s = SyncTail::syncApply(txn, *it, inSteadyStateReplication);
            if (!s.isOK()) {
                // Don't retry on commands.
                SyncTail::OplogEntry entry(*it);
                if (entry.opType[0] == 'c') {
                    error() << "Error applying command (" << it->toString() << "): " << s;
                    return s;
                }

                // We might need to fetch the missing docs from the sync source.
                if (st->shouldRetry(txn, *it)) {
                    const Status s2 = SyncTail::syncApply(txn, *it, inSteadyStateReplication);
                    if (!s2.isOK()) {
                        severe() << "Error applying operation (" << it->toString() << "): " << s2;
                        return s2;
                    }
                }

                // If shouldRetry() returns false, fall through.
                // This can happen if the document that was moved and missed by Cloner
                // subsequently got deleted and no longer exists on the Sync Target at all
            }
        } catch (const DBException& e) {
            // SERVER-24927 If we have a NamespaceNotFound exception, then this document will be
            // dropped before initial sync ends anyways and we should ignore it.
            SyncTail::OplogEntry entry(*it);
            if (e.getCode() == ErrorCodes::NamespaceNotFound &&
                isCrudOpType(entry.opType.rawData())) {
                continue;
            }

            severe() << "writer worker caught exception: " << causedBy(e)
                     << " on: " << it->toString();

            if (inShutdown()) {
                return Status::OK();
            }
            return e.toStatus();
        }
    }
    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
