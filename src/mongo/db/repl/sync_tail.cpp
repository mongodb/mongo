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

#include "third_party/murmurhash3/MurmurHash3.h"
#include <boost/functional/hash.hpp>
#include <memory>

#include "mongo/base/counter.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/prefetch.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;

namespace repl {

AtomicInt32 SyncTail::replBatchLimitOperations{50 * 1000};

/**
 * This variable determines the number of writer threads SyncTail will have. It has a default
 * value, which varies based on architecture and can be overridden using the
 * "replWriterThreadCount" server parameter.
 */
namespace {
#if defined(MONGO_PLATFORM_64)
int replWriterThreadCount = 16;
#elif defined(MONGO_PLATFORM_32)
int replWriterThreadCount = 2;
#else
#error need to include something that defines MONGO_PLATFORM_XX
#endif

class ExportedWriterThreadCountParameter
    : public ExportedServerParameter<int, ServerParameterType::kStartupOnly> {
public:
    ExportedWriterThreadCountParameter()
        : ExportedServerParameter<int, ServerParameterType::kStartupOnly>(
              ServerParameterSet::getGlobal(), "replWriterThreadCount", &replWriterThreadCount) {}

    virtual Status validate(const int& potentialNewValue) {
        if (potentialNewValue < 1 || potentialNewValue > 256) {
            return Status(ErrorCodes::BadValue, "replWriterThreadCount must be between 1 and 256");
        }

        return Status::OK();
    }

} exportedWriterThreadCountParam;

class ExportedBatchLimitOperationsParameter
    : public ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedBatchLimitOperationsParameter()
        : ExportedServerParameter<int, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "replBatchLimitOperations",
              &SyncTail::replBatchLimitOperations) {}

    virtual Status validate(const int& potentialNewValue) {
        if (potentialNewValue < 1 || potentialNewValue > (1000 * 1000)) {
            return Status(ErrorCodes::BadValue,
                          "replBatchLimitOperations must be between 1 and 1 million, inclusive");
        }

        return Status::OK();
    }
} exportedBatchLimitOperationsParam;

// The oplog entries applied
Counter64 opsAppliedStats;
ServerStatusMetricField<Counter64> displayOpsApplied("repl.apply.ops", &opsAppliedStats);

// Number of times we tried to go live as a secondary.
Counter64 attemptsToBecomeSecondary;
ServerStatusMetricField<Counter64> displayAttemptsToBecomeSecondary(
    "repl.apply.attemptsToBecomeSecondary", &attemptsToBecomeSecondary);

// Number and time of each ApplyOps worker pool round
TimerStats applyBatchStats;
ServerStatusMetricField<TimerStats> displayOpBatchesApplied("repl.apply.batches", &applyBatchStats);
void initializePrefetchThread() {
    if (!Client::getCurrent()) {
        Client::initThreadIfNotAlready();
        AuthorizationSession::get(cc())->grantInternalAuthorization();
    }
}

bool isCrudOpType(const char* field) {
    switch (field[0]) {
        case 'd':
        case 'i':
        case 'u':
            return field[1] == 0;
    }
    return false;
}

class ApplyBatchFinalizer {
public:
    ApplyBatchFinalizer(ReplicationCoordinator* replCoord) : _replCoord(replCoord) {}
    virtual ~ApplyBatchFinalizer(){};

    virtual void record(const OpTime& newOpTime) {
        _recordApplied(newOpTime);
    };

protected:
    void _recordApplied(const OpTime& newOpTime) {
        // We have to use setMyLastAppliedOpTimeForward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
        _replCoord->setMyLastAppliedOpTimeForward(newOpTime);
    }

    void _recordDurable(const OpTime& newOpTime) {
        // We have to use setMyLastDurableOpTimeForward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
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

        auto opCtx = cc().makeOperationContext();
        opCtx->recoveryUnit()->waitUntilDurable();
        _recordDurable(latestOpTime);
    }
}
}  // namespace

SyncTail::SyncTail(BackgroundSync* q, MultiSyncApplyFunc func)
    : SyncTail(q, func, makeWriterPool()) {}

SyncTail::SyncTail(BackgroundSync* q,
                   MultiSyncApplyFunc func,
                   std::unique_ptr<OldThreadPool> writerPool)
    : _networkQueue(q), _applyFunc(func), _writerPool(std::move(writerPool)) {}

SyncTail::~SyncTail() {}

std::unique_ptr<OldThreadPool> SyncTail::makeWriterPool() {
    return stdx::make_unique<OldThreadPool>(replWriterThreadCount, "repl writer worker ");
}

bool SyncTail::peek(OperationContext* opCtx, BSONObj* op) {
    return _networkQueue->peek(opCtx, op);
}

// static
Status SyncTail::syncApply(OperationContext* opCtx,
                           const BSONObj& op,
                           bool inSteadyStateReplication,
                           ApplyOperationInLockFn applyOperationInLock,
                           ApplyCommandInLockFn applyCommandInLock,
                           IncrementOpsAppliedStatsFn incrementOpsAppliedStats) {
    // Count each log op application as a separate operation, for reporting purposes
    CurOp individualOp(opCtx);

    const NamespaceString nss(op.getStringField("ns"));

    const char* opType = op["op"].valuestrsafe();

    auto applyOp = [&](Database* db) {
        // For non-initial-sync, we convert updates to upserts
        // to suppress errors when replaying oplog entries.
        UnreplicatedWritesBlock uwb(opCtx);
        DisableDocumentValidation validationDisabler(opCtx);

        Status status =
            applyOperationInLock(opCtx, db, op, inSteadyStateReplication, incrementOpsAppliedStats);
        if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
            throw WriteConflictException();
        }
        return status;
    };

    bool isNoOp = opType[0] == 'n';
    if (isNoOp || (opType[0] == 'i' && nss.isSystemDotIndexes())) {
        auto opStr = isNoOp ? "syncApply_noop" : "syncApply_indexBuild";
        if (isNoOp && nss.db() == "")
            return Status::OK();
        return writeConflictRetry(opCtx, opStr, nss.ns(), [&] {
            Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
            OldClientContext ctx(opCtx, nss.ns());
            return applyOp(ctx.db());
        });
    }

    if (isCrudOpType(opType)) {
        return writeConflictRetry(opCtx, "syncApply_CRUD", nss.ns(), [&] {
            // DB lock always acquires the global lock
            Lock::DBLock dbLock(opCtx, nss.db(), MODE_IX);
            auto ui = op["ui"];
            NamespaceString actualNss = nss;
            if (ui) {
                auto statusWithUUID = UUID::parse(ui);
                if (!statusWithUUID.isOK())
                    return statusWithUUID.getStatus();
                // We may be replaying operations on a collection that was renamed since. If so,
                // it must have been in the same database or it would have gotten a new UUID.
                // Need to throw instead of returning a status for it to be properly ignored.
                actualNss = UUIDCatalog::get(opCtx).lookupNSSByUUID(statusWithUUID.getValue());
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream() << "Failed to apply operation due to missing collection ("
                                      << statusWithUUID.getValue()
                                      << "): "
                                      << redact(op.toString()),
                        !actualNss.isEmpty());
                dassert(actualNss.db() == nss.db());
            }
            Lock::CollectionLock collLock(opCtx->lockState(), actualNss.ns(), MODE_IX);

            Database* db = dbHolder().get(opCtx, actualNss.db());
            if (!db)
                return Status(ErrorCodes::NamespaceNotFound,
                              "cannot apply operation on missing database " + actualNss.db());

            OldClientContext ctx(opCtx, actualNss.ns(), db, /*justCreated*/ false);
            return applyOp(ctx.db());
        });
    }

    if (opType[0] == 'c') {
        return writeConflictRetry(opCtx, "syncApply_command", nss.ns(), [&] {
            // a command may need a global write lock. so we will conservatively go
            // ahead and grab one here. suboptimal. :-(
            Lock::GlobalWrite globalWriteLock(opCtx);

            // special case apply for commands to avoid implicit database creation
            Status status = applyCommandInLock(opCtx, op, inSteadyStateReplication);
            incrementOpsAppliedStats();
            return status;
        });
    }

    // unknown opType
    str::stream ss;
    ss << "bad opType '" << opType << "' in oplog entry: " << redact(op);
    error() << std::string(ss);
    return Status(ErrorCodes::BadValue, ss);
}

Status SyncTail::syncApply(OperationContext* opCtx,
                           const BSONObj& op,
                           bool inSteadyStateReplication) {
    return SyncTail::syncApply(opCtx,
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
            const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
            OperationContext& opCtx = *opCtxPtr;
            AutoGetCollectionForReadCommand ctx(&opCtx, NamespaceString(ns));
            Database* db = ctx.getDb();
            if (db) {
                prefetchPagesForReplicatedOp(&opCtx, db, op);
            }
        } catch (const DBException& e) {
            LOG(2) << "ignoring exception in prefetchOp(): " << redact(e) << endl;
        } catch (const std::exception& e) {
            log() << "Unhandled std::exception in prefetchOp(): " << redact(e.what()) << endl;
            fassertFailed(16397);
        }
    }
}

// Doles out all the work to the reader pool threads and waits for them to complete
void prefetchOps(const MultiApplier::Operations& ops, OldThreadPool* prefetcherPool) {
    invariant(prefetcherPool);
    for (auto&& op : ops) {
        prefetcherPool->schedule(&prefetchOp, op.raw);
    }
    prefetcherPool->join();
}

// Doles out all the work to the writer pool threads.
// Does not modify writerVectors, but passes non-const pointers to inner vectors into func.
void applyOps(std::vector<MultiApplier::OperationPtrs>& writerVectors,
              OldThreadPool* writerPool,
              const MultiApplier::ApplyOperationFn& func,
              std::vector<Status>* statusVector) {
    invariant(writerVectors.size() == statusVector->size());
    TimerHolder timer(&applyBatchStats);
    for (size_t i = 0; i < writerVectors.size(); i++) {
        if (!writerVectors[i].empty()) {
            writerPool->schedule([&func, &writerVectors, statusVector, i] {
                (*statusVector)[i] = func(&writerVectors[i]);
            });
        }
    }
}

void initializeWriterThread() {
    // Only do this once per thread
    if (!Client::getCurrent()) {
        Client::initThreadIfNotAlready();
        AuthorizationSession::get(cc())->grantInternalAuthorization();
    }
}

// Schedules the writes to the oplog for 'ops' into threadPool. The caller must guarantee that 'ops'
// stays valid until all scheduled work in the thread pool completes.
void scheduleWritesToOplog(OperationContext* opCtx,
                           OldThreadPool* threadPool,
                           const MultiApplier::Operations& ops) {

    auto makeOplogWriterForRange = [&ops](size_t begin, size_t end) {
        // The returned function will be run in a separate thread after this returns. Therefore all
        // captures other than 'ops' must be by value since they will not be available. The caller
        // guarantees that 'ops' will stay in scope until the spawned threads complete.
        return [&ops, begin, end] {
            initializeWriterThread();
            const auto opCtxHolder = cc().makeOperationContext();
            const auto opCtx = opCtxHolder.get();
            opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);
            UnreplicatedWritesBlock uwb(opCtx);

            std::vector<InsertStatement> docs;
            docs.reserve(end - begin);
            for (size_t i = begin; i < end; i++) {
                // Add as unowned BSON to avoid unnecessary ref-count bumps.
                // 'ops' will outlive 'docs' so the BSON lifetime will be guaranteed.
                docs.emplace_back(BSONObj(ops[i].raw.objdata()));
            }

            fassertStatusOK(40141,
                            StorageInterface::get(opCtx)->insertDocuments(
                                opCtx, NamespaceString::kRsOplogNamespace, docs));
        };
    };

    // We want to be able to take advantage of bulk inserts so we don't use multiple threads if it
    // would result too little work per thread. This also ensures that we can amortize the
    // setup/teardown overhead across many writes.
    const size_t kMinOplogEntriesPerThread = 16;
    const bool enoughToMultiThread =
        ops.size() >= kMinOplogEntriesPerThread * threadPool->getNumThreads();

    // Only doc-locking engines support parallel writes to the oplog because they are required to
    // ensure that oplog entries are ordered correctly, even if inserted out-of-order. Additionally,
    // there would be no way to take advantage of multiple threads if a storage engine doesn't
    // support document locking.
    if (!enoughToMultiThread ||
        !opCtx->getServiceContext()->getGlobalStorageEngine()->supportsDocLocking()) {

        threadPool->schedule(makeOplogWriterForRange(0, ops.size()));
        return;
    }


    const size_t numOplogThreads = threadPool->getNumThreads();
    const size_t numOpsPerThread = ops.size() / numOplogThreads;
    for (size_t thread = 0; thread < numOplogThreads; thread++) {
        size_t begin = thread * numOpsPerThread;
        size_t end = (thread == numOplogThreads - 1) ? ops.size() : begin + numOpsPerThread;
        threadPool->schedule(makeOplogWriterForRange(begin, end));
    }
}

/**
 * Caches per-collection properties which are relevant for oplog application, so that they don't
 * have to be retrieved repeatedly for each op.
 */
class CachedCollectionProperties {
public:
    struct CollectionProperties {
        bool isCapped = false;
        const CollatorInterface* collator = nullptr;
    };

    CollectionProperties getCollectionProperties(OperationContext* opCtx,
                                                 const StringMapTraits::HashedKey& ns) {
        auto it = _cache.find(ns);
        if (it != _cache.end()) {
            return it->second;
        }

        auto collProperties = getCollectionPropertiesImpl(opCtx, ns.key());
        _cache[ns] = collProperties;
        return collProperties;
    }

private:
    CollectionProperties getCollectionPropertiesImpl(OperationContext* opCtx, StringData ns) {
        CollectionProperties collProperties;

        Lock::DBLock dbLock(opCtx, nsToDatabaseSubstring(ns), MODE_IS);
        auto db = dbHolder().get(opCtx, ns);
        if (!db) {
            return collProperties;
        }

        auto collection = db->getCollection(opCtx, ns);
        if (!collection) {
            return collProperties;
        }

        collProperties.isCapped = collection->isCapped();
        collProperties.collator = collection->getDefaultCollator();
        return collProperties;
    }

    StringMap<CollectionProperties> _cache;
};

// This only modifies the isForCappedCollection field on each op. It does not alter the ops vector
// in any other way.
void fillWriterVectors(OperationContext* opCtx,
                       MultiApplier::Operations* ops,
                       std::vector<MultiApplier::OperationPtrs>* writerVectors) {
    const bool supportsDocLocking =
        getGlobalServiceContext()->getGlobalStorageEngine()->supportsDocLocking();
    const uint32_t numWriters = writerVectors->size();

    CachedCollectionProperties collPropertiesCache;

    for (auto&& op : *ops) {
        StringMapTraits::HashedKey hashedNs(op.getNamespace().ns());
        uint32_t hash = hashedNs.hash();

        if (op.isCrudOpType()) {
            auto collProperties = collPropertiesCache.getCollectionProperties(opCtx, hashedNs);

            // For doc locking engines, include the _id of the document in the hash so we get
            // parallelism even if all writes are to a single collection.
            //
            // For capped collections, this is illegal, since capped collections must preserve
            // insertion order.
            if (supportsDocLocking && !collProperties.isCapped) {
                BSONElement id = op.getIdElement();
                BSONElementComparator elementHasher(BSONElementComparator::FieldNamesMode::kIgnore,
                                                    collProperties.collator);
                const size_t idHash = elementHasher.hash(id);
                MurmurHash3_x86_32(&idHash, sizeof(idHash), hash, &hash);
            }

            if (op.getOpType() == OpTypeEnum::kInsert && collProperties.isCapped) {
                // Mark capped collection ops before storing them to ensure we do not attempt to
                // bulk insert them.
                op.isForCappedCollection = true;
            }
        }

        auto& writer = (*writerVectors)[hash % numWriters];
        if (writer.empty())
            writer.reserve(8);  // skip a few growth rounds.
        writer.push_back(&op);
    }
}

}  // namespace

// Applies a batch of oplog entries, by writing the oplog entries to the local oplog
// and then using a set of threads to apply the operations.
OpTime SyncTail::multiApply(OperationContext* opCtx, MultiApplier::Operations ops) {
    auto applyOperation = [this](MultiApplier::OperationPtrs* ops) -> Status {
        _applyFunc(ops, this);
        // This function is used by 3.2 initial sync and steady state data replication.
        // _applyFunc() will throw or abort on error, so we return OK here.
        return Status::OK();
    };
    return fassertStatusOK(
        34437, repl::multiApply(opCtx, _writerPool.get(), std::move(ops), applyOperation));
}

namespace {
void tryToGoLiveAsASecondary(OperationContext* opCtx, ReplicationCoordinator* replCoord) {
    if (replCoord->isInPrimaryOrSecondaryState()) {
        return;
    }

    // This needs to happen after the attempt so readers can be sure we've already tried.
    ON_BLOCK_EXIT([] { attemptsToBecomeSecondary.increment(); });

    Lock::GlobalRead readLock(opCtx);

    if (replCoord->getMaintenanceMode()) {
        LOG(1) << "Can't go live (tryToGoLiveAsASecondary) as maintenance mode is active.";
        // we're not actually going live
        return;
    }

    // Only state RECOVERING can transition to SECONDARY.
    MemberState state(replCoord->getMemberState());
    if (!state.recovering()) {
        LOG(2) << "Can't go live (tryToGoLiveAsASecondary) as state != recovering.";
        return;
    }

    // We can't go to SECONDARY until we reach minvalid.
    if (replCoord->getMyLastAppliedOpTime() <
        ReplicationProcess::get(opCtx)->getConsistencyMarkers()->getMinValid(opCtx)) {
        return;
    }

    auto status = replCoord->setFollowerMode(MemberState::RS_SECONDARY);
    if (!status.isOK()) {
        warning() << "Failed to transition into " << MemberState(MemberState::RS_SECONDARY)
                  << ". Current state: " << replCoord->getMemberState() << causedBy(status);
    }
}
}

class SyncTail::OpQueueBatcher {
    MONGO_DISALLOW_COPYING(OpQueueBatcher);

public:
    OpQueueBatcher(SyncTail* syncTail) : _syncTail(syncTail), _thread([this] { run(); }) {}
    ~OpQueueBatcher() {
        invariant(_isDead);
        _thread.join();
    }

    OpQueue getNextBatch(Seconds maxWaitTime) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        if (_ops.empty() && !_ops.mustShutdown()) {
            // We intentionally don't care about whether this returns due to signaling or timeout
            // since we do the same thing either way: return whatever is in _ops.
            (void)_cv.wait_for(lk, maxWaitTime.toSystemDuration());
        }

        OpQueue ops = std::move(_ops);
        _ops = {};
        _cv.notify_all();

        return ops;
    }

private:
    void run() {
        Client::initThread("ReplBatcher");
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        const auto replCoord = ReplicationCoordinator::get(&opCtx);
        const auto fastClockSource = opCtx.getServiceContext()->getFastClockSource();
        const auto oplogMaxSize = fassertStatusOK(40301,
                                                  StorageInterface::get(&opCtx)->getOplogMaxSize(
                                                      &opCtx, NamespaceString::kRsOplogNamespace));

        // Batches are limited to 10% of the oplog.
        BatchLimits batchLimits;
        batchLimits.bytes = std::min(oplogMaxSize / 10, size_t(replBatchLimitBytes));

        while (true) {
            const auto slaveDelay = replCoord->getSlaveDelaySecs();
            batchLimits.slaveDelayLatestTimestamp = (slaveDelay > Seconds(0))
                ? (fastClockSource->now() - slaveDelay)
                : boost::optional<Date_t>();

            // Check this once per batch since users can change it at runtime.
            batchLimits.ops = replBatchLimitOperations.load();

            OpQueue ops;
            // tryPopAndWaitForMore adds to ops and returns true when we need to end a batch early.
            while (!_syncTail->tryPopAndWaitForMore(&opCtx, &ops, batchLimits)) {
            }

            if (ops.empty() && !ops.mustShutdown()) {
                continue;  // Don't emit empty batches.
            }

            stdx::unique_lock<stdx::mutex> lk(_mutex);
            // Block until the previous batch has been taken.
            _cv.wait(lk, [&] { return _ops.empty(); });
            _ops = std::move(ops);
            _cv.notify_all();
            if (_ops.mustShutdown()) {
                _isDead = true;
                return;
            }
        }
    }

    SyncTail* const _syncTail;

    stdx::mutex _mutex;  // Guards _ops.
    stdx::condition_variable _cv;
    OpQueue _ops;

    // This only exists so the destructor invariants rather than deadlocking.
    // TODO remove once we trust noexcept enough to mark oplogApplication() as noexcept.
    bool _isDead = false;

    stdx::thread _thread;  // Must be last so all other members are initialized before starting.
};

void SyncTail::oplogApplication(ReplicationCoordinator* replCoord) {
    OpQueueBatcher batcher(this);

    std::unique_ptr<ApplyBatchFinalizer> finalizer{
        getGlobalServiceContext()->getGlobalStorageEngine()->isDurable()
            ? new ApplyBatchFinalizerForJournal(replCoord)
            : new ApplyBatchFinalizer(replCoord)};

    while (true) {  // Exits on message from OpQueueBatcher.
        // Use a new operation context each iteration, as otherwise we may appear to use a single
        // collection name to refer to collections with different UUIDs.
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;

        // For pausing replication in tests.
        if (MONGO_FAIL_POINT(rsSyncApplyStop)) {
            log() << "sync tail - rsSyncApplyStop fail point enabled. Blocking until fail point is "
                     "disabled.";
            while (MONGO_FAIL_POINT(rsSyncApplyStop)) {
                // Tests should not trigger clean shutdown while that failpoint is active. If we
                // think we need this, we need to think hard about what the behavior should be.
                if (_networkQueue->inShutdown()) {
                    severe() << "Turn off rsSyncApplyStop before attempting clean shutdown";
                    fassertFailedNoTrace(40304);
                }
                sleepmillis(10);
            }
        }

        tryToGoLiveAsASecondary(&opCtx, replCoord);

        long long termWhenBufferIsEmpty = replCoord->getTerm();
        // Blocks up to a second waiting for a batch to be ready to apply. If one doesn't become
        // ready in time, we'll loop again so we can do the above checks periodically.
        OpQueue ops = batcher.getNextBatch(Seconds(1));
        if (ops.empty()) {
            if (ops.mustShutdown()) {
                return;
            }
            if (MONGO_FAIL_POINT(rsSyncApplyStop)) {
                continue;
            }
            // Signal drain complete if we're in Draining state and the buffer is empty.
            replCoord->signalDrainComplete(&opCtx, termWhenBufferIsEmpty);
            continue;  // Try again.
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

        // Don't allow the fsync+lock thread to see intermediate states of batch application.
        stdx::lock_guard<SimpleMutex> fsynclk(filesLockedFsync);

        // Do the work.
        multiApply(&opCtx, ops.releaseBatch());

        // Update various things that care about our last applied optime. Tests rely on 2 happening
        // before 3 even though it isn't strictly necessary. The order of 1 doesn't matter.
        setNewTimestamp(opCtx.getServiceContext(), lastOpTimeInBatch.getTimestamp());  // 1
        ReplicationProcess::get(&opCtx)->getConsistencyMarkers()->setAppliedThrough(
            &opCtx,
            lastOpTimeInBatch);                // 2
        finalizer->record(lastOpTimeInBatch);  // 3
    }
}

// Copies ops out of the bgsync queue into the deque passed in as a parameter.
// Returns true if the batch should be ended early.
// Batch should end early if we encounter a command, or if
// there are no further ops in the bgsync queue to read.
// This function also blocks 1 second waiting for new ops to appear in the bgsync
// queue.  We don't block forever so that we can periodically check for things like shutdown or
// reconfigs.
bool SyncTail::tryPopAndWaitForMore(OperationContext* opCtx,
                                    SyncTail::OpQueue* ops,
                                    const BatchLimits& limits) {
    {
        BSONObj op;
        // Check to see if there are ops waiting in the bgsync queue
        bool peek_success = peek(opCtx, &op);
        if (!peek_success) {
            // If we don't have anything in the queue, wait a bit for something to appear.
            if (ops->empty()) {
                if (_networkQueue->inShutdown()) {
                    ops->setMustShutdownFlag();
                } else {
                    // Block up to 1 second. We still return true in this case because we want this
                    // op to be the first in a new batch with a new start time.
                    _networkQueue->waitForMore();
                }
            }

            return true;
        }

        // If this op would put us over the byte limit don't include it unless the batch is empty.
        // We allow single-op batches to exceed the byte limit so that large ops are able to be
        // processed.
        if (!ops->empty() && (ops->getBytes() + size_t(op.objsize())) > limits.bytes) {
            return true;  // Return before wasting time parsing the op.
        }

        // Don't consume the op if we are told to stop.
        if (MONGO_FAIL_POINT(rsSyncApplyStop)) {
            sleepmillis(10);
            return true;
        }

        ops->emplace_back(std::move(op));  // Parses the op in-place.
    }

    auto& entry = ops->back();

    // check for oplog version change
    int curVersion = entry.getVersion();
    if (curVersion != OplogEntry::kOplogVersion) {
        severe() << "expected oplog version " << OplogEntry::kOplogVersion << " but found version "
                 << curVersion << " in oplog entry: " << redact(entry.raw);
        fassertFailedNoTrace(18820);
    }

    auto entryTime = Date_t::fromDurationSinceEpoch(Seconds(entry.getTimestamp().getSecs()));
    if (limits.slaveDelayLatestTimestamp && entryTime > *limits.slaveDelayLatestTimestamp) {

        ops->pop_back();  // Don't do this op yet.
        if (ops->empty()) {
            // Sleep if we've got nothing to do. Only sleep for 1 second at a time to allow
            // reconfigs and shutdown to occur.
            sleepsecs(1);
        }
        return true;
    }

    // Check for ops that must be processed one at a time.
    if (entry.isCommand() ||  // commands.
        // Index builds are achieved through the use of an insert op, not a command op.
        // The following line is the same as what the insert code uses to detect an index build.
        (!entry.getNamespace().isEmpty() && entry.getNamespace().coll() == "system.indexes")) {
        if (ops->getCount() == 1) {
            // apply commands one-at-a-time
            _networkQueue->consume(opCtx);
        } else {
            // This op must be processed alone, but we already had ops in the queue so we can't
            // include it in this batch. Since we didn't call consume(), we'll see this again next
            // time and process it alone.
            ops->pop_back();
        }

        // Apply what we have so far.
        return true;
    }

    // We are going to apply this Op.
    _networkQueue->consume(opCtx);

    // Go back for more ops, unless we've hit the limit.
    return ops->getCount() >= limits.ops;
}

void SyncTail::setHostname(const std::string& hostname) {
    _hostname = hostname;
}

OldThreadPool* SyncTail::getWriterPool() {
    return _writerPool.get();
}

BSONObj SyncTail::getMissingDoc(OperationContext* opCtx, const BSONObj& o) {
    OplogReader missingObjReader;  // why are we using OplogReader to run a non-oplog query?
    const char* ns = o.getStringField("ns");

    if (MONGO_FAIL_POINT(initialSyncHangBeforeGettingMissingDocument)) {
        log() << "initial sync - initialSyncHangBeforeGettingMissingDocument fail point enabled. "
                 "Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(initialSyncHangBeforeGettingMissingDocument)) {
            mongo::sleepsecs(1);
        }
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
            severe() << "cannot fetch missing document without _id field: " << redact(o);
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
            error() << "assertion fetching missing object: " << redact(e) << endl;
            throw;
        }

        // success!
        return missingObj;
    }
    // retry count exceeded
    msgasserted(15916,
                str::stream() << "Can no longer connect to initial sync source: " << _hostname);
}

bool SyncTail::fetchAndInsertMissingDocument(OperationContext* opCtx, const BSONObj& o) {
    const NamespaceString nss(o.getStringField("ns"));

    {
        // If the document is in a capped collection then it's okay for it to be missing.
        AutoGetCollectionForRead autoColl(opCtx, nss);
        Collection* const collection = autoColl.getCollection();
        if (collection && collection->isCapped()) {
            log() << "Not fetching missing document in capped collection (" << nss << ")";
            return false;
        }
    }

    log() << "Fetching missing document: " << redact(o);
    BSONObj missingObj = getMissingDoc(opCtx, o);

    if (missingObj.isEmpty()) {
        log() << "Missing document not found on source; presumably deleted later in oplog. o first "
                 "field: "
              << o.getObjectField("o").firstElementFieldName()
              << ", o2: " << redact(o.getObjectField("o2"));

        return false;
    }

    return writeConflictRetry(opCtx, "fetchAndInsertMissingDocument", nss.ns(), [&] {
        // Take an X lock on the database in order to preclude other modifications.
        // Also, the database might not exist yet, so create it.
        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_X);
        Database* const db = autoDb.getDb();

        WriteUnitOfWork wunit(opCtx);

        Collection* const coll = db->getOrCreateCollection(opCtx, nss);
        invariant(coll);

        OpDebug* const nullOpDebug = nullptr;
        Status status = coll->insertDocument(opCtx, InsertStatement(missingObj), nullOpDebug, true);
        uassert(15917,
                str::stream() << "Failed to insert missing document: " << status.toString(),
                status.isOK());

        LOG(1) << "Inserted missing document: " << redact(missingObj);

        wunit.commit();
        return true;
    });
}

// This free function is used by the writer threads to apply each op
void multiSyncApply(MultiApplier::OperationPtrs* ops, SyncTail*) {
    initializeWriterThread();
    auto opCtx = cc().makeOperationContext();
    auto syncApply = [](OperationContext* opCtx, const BSONObj& op, bool inSteadyStateReplication) {
        return SyncTail::syncApply(opCtx, op, inSteadyStateReplication);
    };

    fassertNoTrace(16359, multiSyncApply_noAbort(opCtx.get(), ops, syncApply));
}

Status multiSyncApply_noAbort(OperationContext* opCtx,
                              MultiApplier::OperationPtrs* oplogEntryPointers,
                              SyncApplyFn syncApply) {
    UnreplicatedWritesBlock uwb(opCtx);
    DisableDocumentValidation validationDisabler(opCtx);

    // Allow us to get through the magic barrier.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

    // Sort the oplog entries by namespace, so that entries from the same namespace will be next to
    // each other in the list.
    if (oplogEntryPointers->size() > 1) {
        std::stable_sort(oplogEntryPointers->begin(),
                         oplogEntryPointers->end(),
                         [](const OplogEntry* l, const OplogEntry* r) {
                             return l->getNamespace() < r->getNamespace();
                         });
    }

    // This function is only called in steady state replication.
    const bool inSteadyStateReplication = true;

    // doNotGroupBeforePoint is used to prevent retrying bad group inserts by marking the final op
    // of a failed group and not allowing further group inserts until that op has been processed.
    auto doNotGroupBeforePoint = oplogEntryPointers->begin();

    for (auto oplogEntriesIterator = oplogEntryPointers->begin();
         oplogEntriesIterator != oplogEntryPointers->end();
         ++oplogEntriesIterator) {

        auto entry = *oplogEntriesIterator;

        // Attempt to group 'insert' ops if possible.
        if (entry->getOpType() == OpTypeEnum::kInsert && !entry->isForCappedCollection &&
            oplogEntriesIterator > doNotGroupBeforePoint) {

            std::vector<BSONObj> toInsert;

            auto maxBatchSize = insertVectorMaxBytes;
            auto maxBatchCount = 64;

            // Make sure to include the first op in the batch size.
            int batchSize = (*oplogEntriesIterator)->getObject().objsize();
            int batchCount = 1;
            auto batchNamespace = entry->getNamespace();

            /**
             * Search for the op that delimits this insert batch, and save its position
             * in endOfGroupableOpsIterator. For example, given the following list of oplog
             * entries with a sequence of groupable inserts:
             *
             *                S--------------E
             *       u, u, u, i, i, i, i, i, d, d
             *
             *       S: start of insert group
             *       E: end of groupable ops
             *
             * E is the position of endOfGroupableOpsIterator. i.e. endOfGroupableOpsIterator
             * will point to the first op that *can't* be added to the current insert group.
             */
            auto endOfGroupableOpsIterator = std::find_if(
                oplogEntriesIterator + 1,
                oplogEntryPointers->end(),
                [&](const OplogEntry* nextEntry) -> bool {
                    auto opNamespace = nextEntry->getNamespace();
                    batchSize += nextEntry->getObject().objsize();
                    batchCount += 1;

                    // Only add the op to this batch if it passes the criteria.
                    return nextEntry->getOpType() != OpTypeEnum::kInsert  // Must be an insert.
                        || opNamespace != batchNamespace  // Must be in the same namespace.
                        || batchSize > maxBatchSize       // Must not create too large an object.
                        || batchCount > maxBatchCount;    // Limit number of ops in a single group.
                });

            // See if we were able to create a group that contains more than a single op.
            bool isGroup = (endOfGroupableOpsIterator > oplogEntriesIterator + 1);

            if (isGroup) {
                // Since we found more than one document, create grouped insert of many docs.
                BSONObjBuilder groupedInsertBuilder;
                // Generate an op object of all elements except for "o", since we need to
                // make the "o" field an array of all the o's.
                for (auto elem : entry->raw) {
                    if (elem.fieldNameStringData() != "o") {
                        groupedInsertBuilder.append(elem);
                    }
                }

                // Populate the "o" field with an array of all the grouped inserts.
                BSONArrayBuilder insertArrayBuilder(groupedInsertBuilder.subarrayStart("o"));
                for (auto groupingIterator = oplogEntriesIterator;
                     groupingIterator != endOfGroupableOpsIterator;
                     ++groupingIterator) {
                    insertArrayBuilder.append((*groupingIterator)->getObject());
                }
                insertArrayBuilder.done();

                try {
                    // Apply the group of inserts.
                    uassertStatusOK(
                        syncApply(opCtx, groupedInsertBuilder.done(), inSteadyStateReplication));
                    // It succeeded, advance the oplogEntriesIterator to the end of the
                    // group of inserts.
                    oplogEntriesIterator = endOfGroupableOpsIterator - 1;
                    continue;
                } catch (const DBException& e) {
                    // The group insert failed, log an error and fall through to the
                    // application of an individual op.
                    error() << "Error applying inserts in bulk " << causedBy(redact(e))
                            << " trying first insert as a lone insert";

                    // Avoid quadratic run time from failed insert by not retrying until we
                    // are beyond this group of ops.
                    doNotGroupBeforePoint = endOfGroupableOpsIterator - 1;
                }
            }
        }

        // If we didn't create a group, try to apply the op individually.
        try {
            const Status status = syncApply(opCtx, entry->raw, inSteadyStateReplication);

            if (!status.isOK()) {
                severe() << "Error applying operation (" << redact(entry->raw)
                         << "): " << causedBy(redact(status));
                return status;
            }
        } catch (const DBException& e) {
            severe() << "writer worker caught exception: " << redact(e)
                     << " on: " << redact(entry->raw);
            return e.toStatus();
        }
    }

    return Status::OK();
}

// This free function is used by the initial sync writer threads to apply each op
void multiInitialSyncApply_abortOnFailure(MultiApplier::OperationPtrs* ops, SyncTail* st) {
    initializeWriterThread();
    auto opCtx = cc().makeOperationContext();
    AtomicUInt32 fetchCount(0);
    fassertNoTrace(15915, multiInitialSyncApply_noAbort(opCtx.get(), ops, st, &fetchCount));
}

Status multiInitialSyncApply(MultiApplier::OperationPtrs* ops,
                             SyncTail* st,
                             AtomicUInt32* fetchCount) {
    initializeWriterThread();
    auto opCtx = cc().makeOperationContext();
    return multiInitialSyncApply_noAbort(opCtx.get(), ops, st, fetchCount);
}

Status multiInitialSyncApply_noAbort(OperationContext* opCtx,
                                     MultiApplier::OperationPtrs* ops,
                                     SyncTail* st,
                                     AtomicUInt32* fetchCount) {
    UnreplicatedWritesBlock uwb(opCtx);
    DisableDocumentValidation validationDisabler(opCtx);

    // allow us to get through the magic barrier
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

    // This function is only called in initial sync, as its name suggests.
    const bool inSteadyStateReplication = false;

    for (auto it = ops->begin(); it != ops->end(); ++it) {
        auto& entry = **it;
        try {
            const Status s = SyncTail::syncApply(opCtx, entry.raw, inSteadyStateReplication);
            if (!s.isOK()) {
                // In initial sync, update operations can cause documents to be missed during
                // collection cloning. As a result, it is possible that a document that we need to
                // update is not present locally. In that case we fetch the document from the
                // sync source.
                if (s != ErrorCodes::UpdateOperationFailed) {
                    error() << "Error applying operation: " << redact(s) << " ("
                            << redact(entry.raw) << ")";
                    return s;
                }

                // We might need to fetch the missing docs from the sync source.
                fetchCount->fetchAndAdd(1);
                st->fetchAndInsertMissingDocument(opCtx, entry.raw);
            }
        } catch (const DBException& e) {
            // SERVER-24927 If we have a NamespaceNotFound exception, then this document will be
            // dropped before initial sync ends anyways and we should ignore it.
            if (e.getCode() == ErrorCodes::NamespaceNotFound && entry.isCrudOpType()) {
                continue;
            }

            severe() << "writer worker caught exception: " << causedBy(redact(e))
                     << " on: " << redact(entry.raw);
            return e.toStatus();
        }
    }

    return Status::OK();
}

StatusWith<OpTime> multiApply(OperationContext* opCtx,
                              OldThreadPool* workerPool,
                              MultiApplier::Operations ops,
                              MultiApplier::ApplyOperationFn applyOperation) {
    if (!opCtx) {
        return {ErrorCodes::BadValue, "invalid operation context"};
    }

    if (!workerPool) {
        return {ErrorCodes::BadValue, "invalid worker pool"};
    }

    if (ops.empty()) {
        return {ErrorCodes::EmptyArrayOperation, "no operations provided to multiApply"};
    }

    if (!applyOperation) {
        return {ErrorCodes::BadValue, "invalid apply operation function"};
    }

    if (getGlobalServiceContext()->getGlobalStorageEngine()->isMmapV1()) {
        // Use a ThreadPool to prefetch all the operations in a batch.
        prefetchOps(ops, workerPool);
    }

    auto consistencyMarkers = ReplicationProcess::get(opCtx)->getConsistencyMarkers();

    LOG(2) << "replication batch size is " << ops.size();
    // Stop all readers until we're done. This also prevents doc-locking engines from deleting old
    // entries from the oplog until we finish writing.
    Lock::ParallelBatchWriterMode pbwm(opCtx->lockState());

    auto replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->getApplierState() == ReplicationCoordinator::ApplierState::Stopped) {
        severe() << "attempting to replicate ops while primary";
        return {ErrorCodes::CannotApplyOplogWhilePrimary,
                "attempting to replicate ops while primary"};
    }

    std::vector<Status> statusVector(workerPool->getNumThreads(), Status::OK());
    {
        // We must wait for the all work we've dispatched to complete before leaving this block
        // because the spawned threads refer to objects on our stack, including writerVectors.
        std::vector<MultiApplier::OperationPtrs> writerVectors(workerPool->getNumThreads());
        ON_BLOCK_EXIT([&] { workerPool->join(); });

        // Write batch of ops into oplog.
        consistencyMarkers->setOplogTruncateAfterPoint(opCtx, ops.front().getTimestamp());
        scheduleWritesToOplog(opCtx, workerPool, ops);
        fillWriterVectors(opCtx, &ops, &writerVectors);

        // Wait for writes to finish before applying ops.
        workerPool->join();

        // Reset consistency markers in case the node fails while applying ops.
        consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
        consistencyMarkers->setMinValidToAtLeast(opCtx, ops.back().getOpTime());

        applyOps(writerVectors, workerPool, applyOperation, &statusVector);
    }

    // If any of the statuses is not ok, return error.
    for (auto& status : statusVector) {
        if (!status.isOK()) {
            return status;
        }
    }

    // We have now written all database writes and updated the oplog to match.
    return ops.back().getOpTime();
}

}  // namespace repl
}  // namespace mongo
