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

#include "mongo/db/repl/sync_tail.h"

#include "third_party/murmurhash3/MurmurHash3.h"
#include <boost/functional/hash.hpp>
#include <memory>

#include "mongo/base/counter.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/prefetch.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/repl/applier_helpers.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/session_update_tracker.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session.h"
#include "mongo/db/session_txn_record_gen.h"
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

namespace {

MONGO_FAIL_POINT_DEFINE(pauseBatchApplicationBeforeCompletion);

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

class ApplyBatchFinalizer {
public:
    ApplyBatchFinalizer(ReplicationCoordinator* replCoord) : _replCoord(replCoord) {}
    virtual ~ApplyBatchFinalizer(){};

    virtual void record(const OpTime& newOpTime,
                        ReplicationCoordinator::DataConsistency consistency) {
        _recordApplied(newOpTime, consistency);
    };

protected:
    void _recordApplied(const OpTime& newOpTime,
                        ReplicationCoordinator::DataConsistency consistency) {
        // We have to use setMyLastAppliedOpTimeForward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
        _replCoord->setMyLastAppliedOpTimeForward(newOpTime, consistency);
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

    void record(const OpTime& newOpTime,
                ReplicationCoordinator::DataConsistency consistency) override;

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

void ApplyBatchFinalizerForJournal::record(const OpTime& newOpTime,
                                           ReplicationCoordinator::DataConsistency consistency) {
    _recordApplied(newOpTime, consistency);

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

NamespaceString parseUUIDOrNs(OperationContext* opCtx, const OplogEntry& oplogEntry) {
    auto optionalUuid = oplogEntry.getUuid();
    if (!optionalUuid) {
        return oplogEntry.getNamespace();
    }

    const auto& uuid = optionalUuid.get();
    auto& catalog = UUIDCatalog::get(opCtx);
    auto nss = catalog.lookupNSSByUUID(uuid);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "No namespace with UUID " << uuid.toString(),
            !nss.isEmpty());
    return nss;
}

NamespaceStringOrUUID getNsOrUUID(const NamespaceString& nss, const BSONObj& op) {
    if (auto ui = op["ui"]) {
        return {nss.db().toString(), uassertStatusOK(UUID::parse(ui))};
    }
    return nss;
}

}  // namespace

// static
Status SyncTail::syncApply(OperationContext* opCtx,
                           const BSONObj& op,
                           OplogApplication::Mode oplogApplicationMode) {
    // Count each log op application as a separate operation, for reporting purposes
    CurOp individualOp(opCtx);

    const NamespaceString nss(op.getStringField("ns"));

    auto incrementOpsAppliedStats = [] { opsAppliedStats.increment(1); };

    auto applyOp = [&](Database* db) {
        // For non-initial-sync, we convert updates to upserts
        // to suppress errors when replaying oplog entries.
        UnreplicatedWritesBlock uwb(opCtx);
        DisableDocumentValidation validationDisabler(opCtx);

        // We convert updates to upserts when not in initial sync because after rollback and during
        // startup we may replay an update after a delete and crash since we do not ignore
        // errors. In initial sync we simply ignore these update errors so there is no reason to
        // upsert.
        //
        // TODO (SERVER-21700): Never upsert during oplog application unless an external applyOps
        // wants to. We should ignore these errors intelligently while in RECOVERING and STARTUP
        // mode (similar to initial sync) instead so we do not accidentally ignore real errors.
        bool shouldAlwaysUpsert = (oplogApplicationMode != OplogApplication::Mode::kInitialSync);
        Status status = applyOperation_inlock(
            opCtx, db, op, shouldAlwaysUpsert, oplogApplicationMode, incrementOpsAppliedStats);
        if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
            throw WriteConflictException();
        }
        return status;
    };

    auto opType = OpType_parse(IDLParserErrorContext("syncApply"), op["op"].valuestrsafe());

    if (opType == OpTypeEnum::kNoop) {
        if (nss.db() == "") {
            return Status::OK();
        }
        Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        return applyOp(ctx.db());
    } else if (opType == OpTypeEnum::kInsert && nss.isSystemDotIndexes()) {
        Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        return applyOp(ctx.db());
    } else if (OplogEntry::isCrudOpType(opType)) {
        return writeConflictRetry(opCtx, "syncApply_CRUD", nss.ns(), [&] {
            // Need to throw instead of returning a status for it to be properly ignored.
            try {
                AutoGetCollection autoColl(opCtx, getNsOrUUID(nss, op), MODE_IX);
                auto db = autoColl.getDb();
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream() << "missing database (" << nss.db() << ")",
                        db);
                OldClientContext ctx(opCtx, autoColl.getNss().ns(), db);
                return applyOp(ctx.db());
            } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
                if (oplogApplicationMode == OplogApplication::Mode::kRecovering) {
                    return Status::OK();
                }

                // Delete operations on non-existent namespaces can be treated as successful for
                // idempotency reasons.
                // During RECOVERING mode, we ignore NamespaceNotFound for all CRUD ops since
                // storage does not wait for drops to be checkpointed (SERVER-33161).
                if (opType == OpTypeEnum::kDelete ||
                    oplogApplicationMode == OplogApplication::Mode::kRecovering) {
                    return Status::OK();
                }

                ex.addContext(str::stream() << "Failed to apply operation: " << redact(op));
                throw;
            }
        });
    } else if (opType == OpTypeEnum::kCommand) {
        return writeConflictRetry(opCtx, "syncApply_command", nss.ns(), [&] {
            // a command may need a global write lock. so we will conservatively go
            // ahead and grab one here. suboptimal. :-(
            Lock::GlobalWrite globalWriteLock(opCtx);

            // special case apply for commands to avoid implicit database creation
            Status status = applyCommand_inlock(opCtx, op, oplogApplicationMode);
            incrementOpsAppliedStats();
            return status;
        });
    }

    MONGO_UNREACHABLE;
}

SyncTail::SyncTail(OplogApplier::Observer* observer,
                   ReplicationConsistencyMarkers* consistencyMarkers,
                   StorageInterface* storageInterface,
                   MultiSyncApplyFunc func,
                   ThreadPool* writerPool,
                   const OplogApplier::Options& options)
    : _observer(observer),
      _consistencyMarkers(consistencyMarkers),
      _storageInterface(storageInterface),
      _applyFunc(func),
      _writerPool(writerPool),
      _options(options) {}

SyncTail::SyncTail(OplogApplier::Observer* observer,
                   ReplicationConsistencyMarkers* consistencyMarkers,
                   StorageInterface* storageInterface,
                   MultiSyncApplyFunc func,
                   ThreadPool* writerPool)
    : SyncTail(observer, consistencyMarkers, storageInterface, func, writerPool, {}) {}

SyncTail::~SyncTail() {}

const OplogApplier::Options& SyncTail::getOptions() const {
    return _options;
}

namespace {

// The pool threads call this to prefetch each op
void prefetchOp(const OplogEntry& oplogEntry) {
    const auto& nss = oplogEntry.getNamespace();
    if (!nss.isEmpty()) {
        try {
            // one possible tweak here would be to stay in the read lock for this database
            // for multiple prefetches if they are for the same database.
            const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
            OperationContext& opCtx = *opCtxPtr;
            AutoGetCollectionForReadCommand ctx(&opCtx, nss);
            Database* db = ctx.getDb();
            if (db) {
                prefetchPagesForReplicatedOp(&opCtx, db, oplogEntry);
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
void prefetchOps(const MultiApplier::Operations& ops, ThreadPool* prefetcherPool) {
    invariant(prefetcherPool);
    for (auto&& op : ops) {
        invariant(prefetcherPool->schedule([&] { prefetchOp(op); }));
    }
    prefetcherPool->waitForIdle();
}

// Doles out all the work to the writer pool threads.
// Does not modify writerVectors, but passes non-const pointers to inner vectors into func.
void applyOps(std::vector<MultiApplier::OperationPtrs>& writerVectors,
              ThreadPool* writerPool,
              const SyncTail::MultiSyncApplyFunc& func,
              SyncTail* st,
              std::vector<Status>* statusVector,
              std::vector<WorkerMultikeyPathInfo>* workerMultikeyPathInfo) {
    invariant(writerVectors.size() == statusVector->size());
    for (size_t i = 0; i < writerVectors.size(); i++) {
        if (!writerVectors[i].empty()) {
            invariant(writerPool->schedule([
                &func,
                st,
                &writer = writerVectors.at(i),
                &status = statusVector->at(i),
                &workerMultikeyPathInfo = workerMultikeyPathInfo->at(i)
            ] {
                auto opCtx = cc().makeOperationContext();
                status = func(opCtx.get(), &writer, st, &workerMultikeyPathInfo);
            }));
        }
    }
}

// Schedules the writes to the oplog for 'ops' into threadPool. The caller must guarantee that 'ops'
// stays valid until all scheduled work in the thread pool completes.
void scheduleWritesToOplog(OperationContext* opCtx,
                           StorageInterface* storageInterface,
                           ThreadPool* threadPool,
                           const MultiApplier::Operations& ops) {

    auto makeOplogWriterForRange = [storageInterface, &ops](size_t begin, size_t end) {
        // The returned function will be run in a separate thread after this returns. Therefore all
        // captures other than 'ops' must be by value since they will not be available. The caller
        // guarantees that 'ops' will stay in scope until the spawned threads complete.
        return [storageInterface, &ops, begin, end] {
            auto opCtx = cc().makeOperationContext();
            UnreplicatedWritesBlock uwb(opCtx.get());
            ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
                opCtx->lockState());

            std::vector<InsertStatement> docs;
            docs.reserve(end - begin);
            for (size_t i = begin; i < end; i++) {
                // Add as unowned BSON to avoid unnecessary ref-count bumps.
                // 'ops' will outlive 'docs' so the BSON lifetime will be guaranteed.
                docs.emplace_back(InsertStatement{
                    ops[i].raw, ops[i].getOpTime().getTimestamp(), ops[i].getOpTime().getTerm()});
            }

            fassert(40141,
                    storageInterface->insertDocuments(
                        opCtx.get(), NamespaceString::kRsOplogNamespace, docs));
        };
    };

    // We want to be able to take advantage of bulk inserts so we don't use multiple threads if it
    // would result too little work per thread. This also ensures that we can amortize the
    // setup/teardown overhead across many writes.
    const size_t kMinOplogEntriesPerThread = 16;
    const bool enoughToMultiThread =
        ops.size() >= kMinOplogEntriesPerThread * threadPool->getStats().numThreads;

    // Only doc-locking engines support parallel writes to the oplog because they are required to
    // ensure that oplog entries are ordered correctly, even if inserted out-of-order. Additionally,
    // there would be no way to take advantage of multiple threads if a storage engine doesn't
    // support document locking.
    if (!enoughToMultiThread ||
        !opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking()) {

        invariant(threadPool->schedule(makeOplogWriterForRange(0, ops.size())));
        return;
    }


    const size_t numOplogThreads = threadPool->getStats().numThreads;
    const size_t numOpsPerThread = ops.size() / numOplogThreads;
    for (size_t thread = 0; thread < numOplogThreads; thread++) {
        size_t begin = thread * numOpsPerThread;
        size_t end = (thread == numOplogThreads - 1) ? ops.size() : begin + numOpsPerThread;
        invariant(threadPool->schedule(makeOplogWriterForRange(begin, end)));
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
        auto db = DatabaseHolder::getDatabaseHolder().get(opCtx, ns);
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

/**
 * ops - This only modifies the isForCappedCollection field on each op. It does not alter the ops
 *      vector in any other way.
 * writerVectors - Set of operations for each worker thread to apply.
 * derivedOps - If provided, this function inserts a decomposition of applyOps operations
 *      and instructions for updating the transactions table.
 * sessionUpdateTracker - if provided, keeps track of session info from ops.
 */
void fillWriterVectors(OperationContext* opCtx,
                       MultiApplier::Operations* ops,
                       std::vector<MultiApplier::OperationPtrs>* writerVectors,
                       std::vector<MultiApplier::Operations>* derivedOps,
                       SessionUpdateTracker* sessionUpdateTracker) {
    const auto serviceContext = opCtx->getServiceContext();
    const auto storageEngine = serviceContext->getStorageEngine();

    const bool supportsDocLocking = storageEngine->supportsDocLocking();
    const uint32_t numWriters = writerVectors->size();

    CachedCollectionProperties collPropertiesCache;

    for (auto&& op : *ops) {
        StringMapTraits::HashedKey hashedNs(op.getNamespace().ns());
        uint32_t hash = hashedNs.hash();

        // We need to track all types of ops, including type 'n' (these are generated from chunk
        // migrations).
        if (sessionUpdateTracker) {
            if (auto newOplogWrites = sessionUpdateTracker->updateOrFlush(op)) {
                derivedOps->emplace_back(std::move(*newOplogWrites));
                fillWriterVectors(opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
            }
        }

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

        // Extract applyOps operations and fill writers with extracted operations using this
        // function.
        if (op.isCommand() && op.getCommandType() == OplogEntry::CommandType::kApplyOps) {
            if (op.shouldPrepare()) {
                // TODO (SERVER-35307) mark operations as needing prepare.
                continue;
            }
            try {
                derivedOps->emplace_back(ApplyOps::extractOperations(op));
                fillWriterVectors(
                    opCtx, &derivedOps->back(), writerVectors, derivedOps, sessionUpdateTracker);
            } catch (...) {
                fassertFailedWithStatusNoTrace(
                    50711,
                    exceptionToStatus().withContext(str::stream()
                                                    << "Unable to extract operations from applyOps "
                                                    << redact(op.toBSON())));
            }
            continue;
        }

        auto& writer = (*writerVectors)[hash % numWriters];
        if (writer.empty()) {
            writer.reserve(8);  // Skip a few growth rounds
        }
        writer.push_back(&op);
    }
}

void fillWriterVectors(OperationContext* opCtx,
                       MultiApplier::Operations* ops,
                       std::vector<MultiApplier::OperationPtrs>* writerVectors,
                       std::vector<MultiApplier::Operations>* derivedOps) {
    SessionUpdateTracker sessionUpdateTracker;
    fillWriterVectors(opCtx, ops, writerVectors, derivedOps, &sessionUpdateTracker);

    auto newOplogWrites = sessionUpdateTracker.flushAll();
    if (!newOplogWrites.empty()) {
        derivedOps->emplace_back(std::move(newOplogWrites));
        fillWriterVectors(opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
    }
}

}  // namespace

namespace {
void tryToGoLiveAsASecondary(OperationContext* opCtx,
                             ReplicationCoordinator* replCoord,
                             OpTime minValid) {
    if (replCoord->isInPrimaryOrSecondaryState()) {
        return;
    }

    // This needs to happen after the attempt so readers can be sure we've already tried.
    ON_BLOCK_EXIT([] { attemptsToBecomeSecondary.increment(); });

    // Need global X lock to transition to SECONDARY
    Lock::GlobalWrite writeLock(opCtx);

    // Maintenance mode will force us to remain in RECOVERING state, no matter what.
    if (replCoord->getMaintenanceMode()) {
        LOG(1) << "We cannot transition to SECONDARY state while in maintenance mode.";
        return;
    }

    // We can only transition to SECONDARY from RECOVERING state.
    MemberState state(replCoord->getMemberState());
    if (!state.recovering()) {
        LOG(2) << "We cannot transition to SECONDARY state since we are not currently in "
                  "RECOVERING state. Current state: "
               << state.toString();
        return;
    }

    // We can't go to SECONDARY state until we reach 'minValid', since the database may be in an
    // inconsistent state before this point. If our state is inconsistent, we need to disallow reads
    // from clients, which is why we stay in RECOVERING state.
    auto lastApplied = replCoord->getMyLastAppliedOpTime();
    if (lastApplied < minValid) {
        LOG(2) << "We cannot transition to SECONDARY state because our 'lastApplied' optime is "
                  "less than the 'minValid' optime. minValid optime: "
               << minValid << ", lastApplied optime: " << lastApplied;
        return;
    }

    // Execute the transition to SECONDARY.
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
    OpQueueBatcher(SyncTail* syncTail, StorageInterface* storageInterface, OplogBuffer* oplogBuffer)
        : _syncTail(syncTail),
          _storageInterface(storageInterface),
          _oplogBuffer(oplogBuffer),
          _ops(0),
          _thread([this] { run(); }) {}
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
        _ops = OpQueue(0);
        _cv.notify_all();

        return ops;
    }

private:
    /**
     * If slaveDelay is enabled, this function calculates the most recent timestamp of any oplog
     * entries that can be be returned in a batch.
     */
    boost::optional<Date_t> _calculateSlaveDelayLatestTimestamp() {
        auto service = cc().getServiceContext();
        auto replCoord = ReplicationCoordinator::get(service);
        auto slaveDelay = replCoord->getSlaveDelaySecs();
        if (slaveDelay <= Seconds(0)) {
            return {};
        }
        auto fastClockSource = service->getFastClockSource();
        return fastClockSource->now() - slaveDelay;
    }

    void run() {
        Client::initThread("ReplBatcher");

        BatchLimits batchLimits;
        batchLimits.bytes = OplogApplier::calculateBatchLimitBytes(
            cc().makeOperationContext().get(), _storageInterface);

        while (true) {
            batchLimits.slaveDelayLatestTimestamp = _calculateSlaveDelayLatestTimestamp();

            // Check this once per batch since users can change it at runtime.
            batchLimits.ops = OplogApplier::getBatchLimitOperations();

            OpQueue ops(batchLimits.ops);
            // tryPopAndWaitForMore adds to ops and returns true when we need to end a batch early.
            {
                auto opCtx = cc().makeOperationContext();
                while (!_syncTail->tryPopAndWaitForMore(
                    opCtx.get(), _oplogBuffer, &ops, batchLimits)) {
                }
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
    StorageInterface* const _storageInterface;
    OplogBuffer* const _oplogBuffer;

    stdx::mutex _mutex;  // Guards _ops.
    stdx::condition_variable _cv;
    OpQueue _ops;

    // This only exists so the destructor invariants rather than deadlocking.
    // TODO remove once we trust noexcept enough to mark oplogApplication() as noexcept.
    bool _isDead = false;

    stdx::thread _thread;  // Must be last so all other members are initialized before starting.
};

void SyncTail::oplogApplication(OplogBuffer* oplogBuffer, ReplicationCoordinator* replCoord) {
    if (isMMAPV1()) {
        // Overwrite prefetch index mode if ReplSettings has a mode set.
        auto&& replSettings = replCoord->getSettings();
        if (replSettings.isPrefetchIndexModeSet()) {
            replCoord->setIndexPrefetchConfig(replSettings.getPrefetchIndexMode());
        }
    }

    // We don't start data replication for arbiters at all and it's not allowed to reconfig
    // arbiterOnly field for any member.
    invariant(!replCoord->getMemberState().arbiter());

    OpQueueBatcher batcher(this, _storageInterface, oplogBuffer);

    _oplogApplication(oplogBuffer, replCoord, &batcher);
}

void SyncTail::_oplogApplication(OplogBuffer* oplogBuffer,
                                 ReplicationCoordinator* replCoord,
                                 OpQueueBatcher* batcher) noexcept {
    std::unique_ptr<ApplyBatchFinalizer> finalizer{
        getGlobalServiceContext()->getStorageEngine()->isDurable()
            ? new ApplyBatchFinalizerForJournal(replCoord)
            : new ApplyBatchFinalizer(replCoord)};

    // Get replication consistency markers.
    OpTime minValid;

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
                if (inShutdown()) {
                    severe() << "Turn off rsSyncApplyStop before attempting clean shutdown";
                    fassertFailedNoTrace(40304);
                }
                sleepmillis(10);
            }
        }

        // Get the current value of 'minValid'.
        minValid = _consistencyMarkers->getMinValid(&opCtx);

        // Transition to SECONDARY state, if possible.
        tryToGoLiveAsASecondary(&opCtx, replCoord, minValid);

        long long termWhenBufferIsEmpty = replCoord->getTerm();
        // Blocks up to a second waiting for a batch to be ready to apply. If one doesn't become
        // ready in time, we'll loop again so we can do the above checks periodically.
        OpQueue ops = batcher->getNextBatch(Seconds(1));
        if (ops.empty()) {
            if (ops.mustShutdown()) {
                // Shut down and exit oplog application loop.
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
        const auto firstOpTimeInBatch = ops.front().getOpTime();
        const auto lastOpTimeInBatch = ops.back().getOpTime();
        const auto lastAppliedOpTimeAtStartOfBatch = replCoord->getMyLastAppliedOpTime();

        // Make sure the oplog doesn't go back in time or repeat an entry.
        if (firstOpTimeInBatch <= lastAppliedOpTimeAtStartOfBatch) {
            fassert(34361,
                    Status(ErrorCodes::OplogOutOfOrder,
                           str::stream() << "Attempted to apply an oplog entry ("
                                         << firstOpTimeInBatch.toString()
                                         << ") which is not greater than our last applied OpTime ("
                                         << lastAppliedOpTimeAtStartOfBatch.toString()
                                         << ")."));
        }

        // Don't allow the fsync+lock thread to see intermediate states of batch application.
        stdx::lock_guard<SimpleMutex> fsynclk(filesLockedFsync);

        // Apply the operations in this batch. 'multiApply' returns the optime of the last op that
        // was applied, which should be the last optime in the batch.
        auto lastOpTimeAppliedInBatch =
            fassertNoTrace(34437, multiApply(&opCtx, ops.releaseBatch()));
        invariant(lastOpTimeAppliedInBatch == lastOpTimeInBatch);

        // In order to provide resilience in the event of a crash in the middle of batch
        // application, 'multiApply' will update 'minValid' so that it is at least as great as the
        // last optime that it applied in this batch. If 'minValid' was moved forward, we make sure
        // to update our view of it here.
        if (lastOpTimeInBatch > minValid) {
            minValid = lastOpTimeInBatch;
        }

        // Update various things that care about our last applied optime. Tests rely on 2 happening
        // before 3 even though it isn't strictly necessary. The order of 1 doesn't matter.

        // 1. Update the global timestamp.
        setNewTimestamp(opCtx.getServiceContext(), lastOpTimeInBatch.getTimestamp());

        // 2. Persist our "applied through" optime to disk.
        _consistencyMarkers->setAppliedThrough(&opCtx, lastOpTimeInBatch);

        // 3. Ensure that the last applied op time hasn't changed since the start of this batch.
        const auto lastAppliedOpTimeAtEndOfBatch = replCoord->getMyLastAppliedOpTime();
        invariant(lastAppliedOpTimeAtStartOfBatch == lastAppliedOpTimeAtEndOfBatch,
                  str::stream() << "the last known applied OpTime has changed from "
                                << lastAppliedOpTimeAtStartOfBatch.toString()
                                << " to "
                                << lastAppliedOpTimeAtEndOfBatch.toString()
                                << " in the middle of batch application");

        // 4. Update oplog visibility by notifying the storage engine of the new oplog entries.
        const bool orderedCommit = true;
        _storageInterface->oplogDiskLocRegister(
            &opCtx, lastOpTimeInBatch.getTimestamp(), orderedCommit);

        // 5. Finalize this batch. We are at a consistent optime if our current optime is >= the
        // current 'minValid' optime.
        auto consistency = (lastOpTimeInBatch >= minValid)
            ? ReplicationCoordinator::DataConsistency::Consistent
            : ReplicationCoordinator::DataConsistency::Inconsistent;
        finalizer->record(lastOpTimeInBatch, consistency);
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
                                    OplogBuffer* oplogBuffer,
                                    SyncTail::OpQueue* ops,
                                    const BatchLimits& limits) {
    {
        BSONObj op;
        // Check to see if there are ops waiting in the bgsync queue
        bool peek_success = oplogBuffer->peek(opCtx, &op);
        if (!peek_success) {
            // If we don't have anything in the queue, wait a bit for something to appear.
            if (ops->empty()) {
                if (inShutdown()) {
                    ops->setMustShutdownFlag();
                } else {
                    // Block up to 1 second. We still return true in this case because we want this
                    // op to be the first in a new batch with a new start time.
                    oplogBuffer->waitForData(Seconds(1));
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
                 << curVersion << " in oplog entry: " << redact(entry.toBSON());
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

    // Commands must be processed one at a time. The only exception to this is applyOps because
    // applyOps oplog entries are effectively containers for CRUD operations. Therefore, it is safe
    // to batch applyOps commands with CRUD operations when reading from the oplog buffer.
    // Oplog entries on 'system.views' should also be processed one at a time. View catalog
    // immediately reflects changes for each oplog entry so we can see inconsistent view catalog if
    // multiple oplog entries on 'system.views' are being applied out of the original order.
    if ((entry.isCommand() && entry.getCommandType() != OplogEntry::CommandType::kApplyOps) ||
        entry.getNamespace().isSystemDotViews()) {
        if (ops->getCount() == 1) {
            // apply commands one-at-a-time
            _consume(opCtx, oplogBuffer);
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
    _consume(opCtx, oplogBuffer);

    // Go back for more ops, unless we've hit the limit.
    return ops->getCount() >= limits.ops;
}

void SyncTail::_consume(OperationContext* opCtx, OplogBuffer* oplogBuffer) {
    // This is just to get the op off the queue; it's been peeked at and queued for application
    // already.
    // If we failed to get an op off the queue, this means that shutdown() was called between the
    // consumer's calls to peek() and consume(). shutdown() cleared the buffer so there is nothing
    // for us to consume here. Since our postcondition is already met, it is safe to return
    // successfully.
    BSONObj op;
    invariant(oplogBuffer->tryPop(opCtx, &op) || inShutdown());
}

void SyncTail::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _inShutdown = true;
}

bool SyncTail::inShutdown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _inShutdown;
}

BSONObj SyncTail::getMissingDoc(OperationContext* opCtx, const OplogEntry& oplogEntry) {
    OplogReader missingObjReader;  // why are we using OplogReader to run a non-oplog query?

    if (MONGO_FAIL_POINT(initialSyncHangBeforeGettingMissingDocument)) {
        log() << "initial sync - initialSyncHangBeforeGettingMissingDocument fail point enabled. "
                 "Blocking until fail point is disabled.";
        while (MONGO_FAIL_POINT(initialSyncHangBeforeGettingMissingDocument)) {
            mongo::sleepsecs(1);
        }
    }

    auto source = _options.missingDocumentSourceForInitialSync;
    invariant(source);

    const int retryMax = 3;
    for (int retryCount = 1; retryCount <= retryMax; ++retryCount) {
        if (retryCount != 1) {
            // if we are retrying, sleep a bit to let the network possibly recover
            sleepsecs(retryCount * retryCount);
        }
        try {
            bool ok = missingObjReader.connect(*source);
            if (!ok) {
                warning() << "network problem detected while connecting to the "
                          << "sync source, attempt " << retryCount << " of " << retryMax << endl;
                continue;  // try again
            }
        } catch (const NetworkException&) {
            warning() << "network problem detected while connecting to the "
                      << "sync source, attempt " << retryCount << " of " << retryMax << endl;
            continue;  // try again
        }

        // get _id from oplog entry to create query to fetch document.
        const auto idElem = oplogEntry.getIdElement();

        if (idElem.eoo()) {
            severe() << "cannot fetch missing document without _id field: "
                     << redact(oplogEntry.toBSON());
            fassertFailedNoTrace(28742);
        }

        BSONObj query = BSONObjBuilder().append(idElem).obj();
        BSONObj missingObj;
        auto nss = oplogEntry.getNamespace();
        try {
            auto uuid = oplogEntry.getUuid();
            if (!uuid) {
                missingObj = missingObjReader.findOne(nss.ns().c_str(), query);
            } else {
                auto dbname = nss.db();
                // If a UUID exists for the command object, find the document by UUID.
                missingObj = missingObjReader.findOneByUUID(dbname.toString(), *uuid, query);
            }
        } catch (const NetworkException&) {
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
                str::stream() << "Can no longer connect to initial sync source: "
                              << source->toString());
}

void SyncTail::fetchAndInsertMissingDocument(OperationContext* opCtx,
                                             const OplogEntry& oplogEntry) {
    // Note that using the local UUID/NamespaceString mapping is sufficient for checking
    // whether the collection is capped on the remote because convertToCapped creates a
    // new collection with a different UUID.
    const NamespaceString nss(parseUUIDOrNs(opCtx, oplogEntry));

    {
        // If the document is in a capped collection then it's okay for it to be missing.
        AutoGetCollectionForRead autoColl(opCtx, nss);
        Collection* const collection = autoColl.getCollection();
        if (collection && collection->isCapped()) {
            log() << "Not fetching missing document in capped collection (" << nss << ")";
            return;
        }
    }

    log() << "Fetching missing document: " << redact(oplogEntry.toBSON());
    BSONObj missingObj = getMissingDoc(opCtx, oplogEntry);

    if (missingObj.isEmpty()) {
        BSONObj object2;
        if (auto optionalObject2 = oplogEntry.getObject2()) {
            object2 = *optionalObject2;
        }
        log() << "Missing document not found on source; presumably deleted later in oplog. o first "
                 "field: "
              << redact(oplogEntry.getObject()) << ", o2: " << redact(object2);

        return;
    }

    return writeConflictRetry(opCtx, "fetchAndInsertMissingDocument", nss.ns(), [&] {
        // Take an X lock on the database in order to preclude other modifications.
        AutoGetDb autoDb(opCtx, nss.db(), MODE_X);
        Database* const db = autoDb.getDb();

        WriteUnitOfWork wunit(opCtx);

        Collection* coll = nullptr;
        auto uuid = oplogEntry.getUuid();
        if (!uuid) {
            if (!db) {
                return;
            }
            coll = db->getOrCreateCollection(opCtx, nss);
        } else {
            // If the oplog entry has a UUID, use it to find the collection in which to insert the
            // missing document.
            auto& catalog = UUIDCatalog::get(opCtx);
            coll = catalog.lookupCollectionByUUID(*uuid);
            if (!coll) {
                // TODO(SERVER-30819) insert this UUID into the missing UUIDs set.
                return;
            }
        }

        invariant(coll);

        OpDebug* const nullOpDebug = nullptr;
        Status status = coll->insertDocument(opCtx, InsertStatement(missingObj), nullOpDebug, true);
        uassert(15917,
                str::stream() << "Failed to insert missing document: " << status.toString(),
                status.isOK());

        LOG(1) << "Inserted missing document: " << redact(missingObj);

        wunit.commit();

        if (_observer) {
            const OplogApplier::Observer::FetchInfo fetchInfo(oplogEntry, missingObj);
            _observer->onMissingDocumentsFetchedAndInserted({fetchInfo});
        }
    });
}

// This free function is used by the writer threads to apply each op
Status multiSyncApply(OperationContext* opCtx,
                      MultiApplier::OperationPtrs* ops,
                      SyncTail* st,
                      WorkerMultikeyPathInfo* workerMultikeyPathInfo) {
    invariant(st);

    UnreplicatedWritesBlock uwb(opCtx);
    DisableDocumentValidation validationDisabler(opCtx);
    ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(opCtx->lockState());

    // Explicitly start future read transactions without a timestamp.
    opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);

    ApplierHelpers::stableSortByNamespace(ops);

    // Assume we are recovering if oplog writes are disabled in the options.
    // Assume we are in initial sync if we have a host for fetching missing documents.
    const auto oplogApplicationMode = st->getOptions().skipWritesToOplog
        ? OplogApplication::Mode::kRecovering
        : (st->getOptions().missingDocumentSourceForInitialSync
               ? OplogApplication::Mode::kInitialSync
               : OplogApplication::Mode::kSecondary);

    ApplierHelpers::InsertGroup insertGroup(ops, opCtx, oplogApplicationMode);

    {  // Ensure that the MultikeyPathTracker stops tracking paths.
        ON_BLOCK_EXIT([opCtx] { MultikeyPathTracker::get(opCtx).stopTrackingMultikeyPathInfo(); });
        MultikeyPathTracker::get(opCtx).startTrackingMultikeyPathInfo();

        for (auto it = ops->cbegin(); it != ops->cend(); ++it) {
            const auto& entry = **it;

            // If we are successful in grouping and applying inserts, advance the current iterator
            // past the end of the inserted group of entries.
            auto groupResult = insertGroup.groupAndApplyInserts(it);
            if (groupResult.isOK()) {
                it = groupResult.getValue();
                continue;
            }

            // If we didn't create a group, try to apply the op individually.
            try {
                const Status status = SyncTail::syncApply(opCtx, entry.raw, oplogApplicationMode);

                if (!status.isOK()) {
                    // In initial sync, update operations can cause documents to be missed during
                    // collection cloning. As a result, it is possible that a document that we
                    // need to update is not present locally. In that case we fetch the document
                    // from the sync source.
                    if (status == ErrorCodes::UpdateOperationFailed &&
                        st->getOptions().missingDocumentSourceForInitialSync) {
                        // We might need to fetch the missing docs from the sync source.
                        st->fetchAndInsertMissingDocument(opCtx, entry);
                        continue;
                    }

                    severe() << "Error applying operation (" << redact(entry.toBSON())
                             << "): " << causedBy(redact(status));
                    return status;
                }
            } catch (const DBException& e) {
                // SERVER-24927 If we have a NamespaceNotFound exception, then this document will be
                // dropped before initial sync or recovery ends anyways and we should ignore it.
                if (e.code() == ErrorCodes::NamespaceNotFound && entry.isCrudOpType() &&
                    st->getOptions().allowNamespaceNotFoundErrorsOnCrudOps) {
                    continue;
                }

                severe() << "writer worker caught exception: " << redact(e)
                         << " on: " << redact(entry.toBSON());
                return e.toStatus();
            }
        }
    }

    invariant(!MultikeyPathTracker::get(opCtx).isTrackingMultikeyPathInfo());
    invariant(workerMultikeyPathInfo->empty());
    auto newPaths = MultikeyPathTracker::get(opCtx).getMultikeyPathInfo();
    if (!newPaths.empty()) {
        workerMultikeyPathInfo->swap(newPaths);
    }

    return Status::OK();
}

StatusWith<OpTime> SyncTail::multiApply(OperationContext* opCtx, MultiApplier::Operations ops) {
    invariant(!ops.empty());

    if (isMMAPV1()) {
        // Use a ThreadPool to prefetch all the operations in a batch.
        prefetchOps(ops, _writerPool);
    }

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

    std::vector<WorkerMultikeyPathInfo> multikeyVector(_writerPool->getStats().numThreads);
    {
        // Each node records cumulative batch application stats for itself using this timer.
        TimerHolder timer(&applyBatchStats);

        // We must wait for the all work we've dispatched to complete before leaving this block
        // because the spawned threads refer to objects on the stack
        ON_BLOCK_EXIT([&] { _writerPool->waitForIdle(); });

        // Write batch of ops into oplog.
        if (!_options.skipWritesToOplog) {
            _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, ops.front().getTimestamp());
            scheduleWritesToOplog(opCtx, _storageInterface, _writerPool, ops);
        }

        // Holds 'pseudo operations' generated by secondaries to aid in replication.
        // Keep in scope until all operations in 'ops' and 'derivedOps' have been applied.
        // Pseudo operations include:
        // - applyOps operations expanded to individual ops.
        // - ops to update config.transactions. Normal writes to config.transactions in the
        //   primary don't create an oplog entry, so extract info from writes with transactions
        //   and create a pseudo oplog.
        std::vector<MultiApplier::Operations> derivedOps;

        std::vector<MultiApplier::OperationPtrs> writerVectors(_writerPool->getStats().numThreads);
        fillWriterVectors(opCtx, &ops, &writerVectors, &derivedOps);

        // Wait for writes to finish before applying ops.
        _writerPool->waitForIdle();

        // Reset consistency markers in case the node fails while applying ops.
        if (!_options.skipWritesToOplog) {
            _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
            _consistencyMarkers->setMinValidToAtLeast(opCtx, ops.back().getOpTime());
        }

        {
            std::vector<Status> statusVector(_writerPool->getStats().numThreads, Status::OK());
            applyOps(writerVectors, _writerPool, _applyFunc, this, &statusVector, &multikeyVector);
            _writerPool->waitForIdle();

            // If any of the statuses is not ok, return error.
            for (auto it = statusVector.cbegin(); it != statusVector.cend(); ++it) {
                const auto& status = *it;
                if (!status.isOK()) {
                    severe()
                        << "Failed to apply batch of operations. Number of operations in batch: "
                        << ops.size() << ". First operation: " << redact(ops.front().toBSON())
                        << ". Last operation: " << redact(ops.back().toBSON())
                        << ". Oplog application failed in writer thread "
                        << std::distance(statusVector.cbegin(), it) << ": " << redact(status);
                    return status;
                }
            }
        }

        // Notify the storage engine that a replication batch has completed.
        // This means that all the writes associated with the oplog entries in the batch are
        // finished and no new writes with timestamps associated with those oplog entries will show
        // up in the future.
        const auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        storageEngine->replicationBatchIsComplete();
    }

    // Use this fail point to hold the PBWM lock and prevent the batch from completing.
    if (MONGO_FAIL_POINT(pauseBatchApplicationBeforeCompletion)) {
        log() << "pauseBatchApplicationBeforeCompletion fail point enabled. Blocking until fail "
                 "point is disabled.";
        while (MONGO_FAIL_POINT(pauseBatchApplicationBeforeCompletion)) {
            if (inShutdown()) {
                severe() << "Turn off pauseBatchApplicationBeforeCompletion before attempting "
                            "clean shutdown";
                fassertFailedNoTrace(50798);
            }
            sleepmillis(100);
        }
    }

    Timestamp firstTimeInBatch = ops.front().getTimestamp();
    // Set any indexes to multikey that this batch ignored. This must be done while holding the
    // parallel batch writer mutex.
    for (WorkerMultikeyPathInfo infoVector : multikeyVector) {
        for (MultikeyPathInfo info : infoVector) {
            // We timestamp every multikey write with the first timestamp in the batch. It is always
            // safe to set an index as multikey too early, just not too late. We conservatively pick
            // the first timestamp in the batch since we do not have enough information to find out
            // the timestamp of the first write that set the given multikey path.
            fassert(50686,
                    _storageInterface->setIndexIsMultikey(
                        opCtx, info.nss, info.indexName, info.multikeyPaths, firstTimeInBatch));
        }
    }

    // We have now written all database writes and updated the oplog to match.
    return ops.back().getOpTime();
}

}  // namespace repl
}  // namespace mongo
