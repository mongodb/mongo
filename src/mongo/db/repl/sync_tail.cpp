/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/applier_helpers.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/session.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {
namespace {

MONGO_FAIL_POINT_DEFINE(pauseBatchApplicationBeforeCompletion);
MONGO_FAIL_POINT_DEFINE(pauseBatchApplicationAfterWritingOplogEntries);
MONGO_FAIL_POINT_DEFINE(hangAfterRecordingOpApplicationStartTime);

// The oplog entries applied
Counter64 opsAppliedStats;
ServerStatusMetricField<Counter64> displayOpsApplied("repl.apply.ops", &opsAppliedStats);

// Tracks the oplog application batch size.
Counter64 oplogApplicationBatchSize;
ServerStatusMetricField<Counter64> displayOplogApplicationBatchSize("repl.apply.batchSize",
                                                                    &oplogApplicationBatchSize);

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

    virtual void record(const OpTimeAndWallTime& newOpTimeAndWallTime,
                        ReplicationCoordinator::DataConsistency consistency) {
        _recordApplied(newOpTimeAndWallTime, consistency);
    };

protected:
    void _recordApplied(const OpTimeAndWallTime& newOpTimeAndWallTime,
                        ReplicationCoordinator::DataConsistency consistency) {
        // We have to use setMyLastAppliedOpTimeAndWallTimeForward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
        _replCoord->setMyLastAppliedOpTimeAndWallTimeForward(newOpTimeAndWallTime, consistency);
    }

    void _recordDurable(const OpTimeAndWallTime& newOpTimeAndWallTime) {
        // We have to use setMyLastDurableOpTimeForward since this thread races with
        // ReplicationExternalStateImpl::onTransitionToPrimary.
        _replCoord->setMyLastDurableOpTimeAndWallTimeForward(newOpTimeAndWallTime);
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

    void record(const OpTimeAndWallTime& newOpTimeAndWallTime,
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
    OpTimeAndWallTime _latestOpTimeAndWallTime;
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

void ApplyBatchFinalizerForJournal::record(const OpTimeAndWallTime& newOpTimeAndWallTime,
                                           ReplicationCoordinator::DataConsistency consistency) {
    _recordApplied(newOpTimeAndWallTime, consistency);

    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _latestOpTimeAndWallTime = newOpTimeAndWallTime;
    _cond.notify_all();
}

void ApplyBatchFinalizerForJournal::_run() {
    Client::initThread("ApplyBatchFinalizerForJournal");

    while (true) {
        OpTimeAndWallTime latestOpTimeAndWallTime = {OpTime(), Date_t()};

        {
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            while (_latestOpTimeAndWallTime.opTime.isNull() && !_shutdownSignaled) {
                _cond.wait(lock);
            }

            if (_shutdownSignaled) {
                return;
            }

            latestOpTimeAndWallTime = _latestOpTimeAndWallTime;
            _latestOpTimeAndWallTime = {OpTime(), Date_t()};
        }

        auto opCtx = cc().makeOperationContext();
        opCtx->recoveryUnit()->waitUntilDurable();
        _recordDurable(latestOpTimeAndWallTime);
    }
}

NamespaceString parseUUIDOrNs(OperationContext* opCtx, const OplogEntry& oplogEntry) {
    auto optionalUuid = oplogEntry.getUuid();
    if (!optionalUuid) {
        return oplogEntry.getNss();
    }

    const auto& uuid = optionalUuid.get();
    auto& catalog = CollectionCatalog::get(opCtx);
    auto nss = catalog.lookupNSSByUUID(uuid);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "No namespace with UUID " << uuid.toString(),
            nss);
    return *nss;
}

NamespaceStringOrUUID getNsOrUUID(const NamespaceString& nss, const BSONObj& op) {
    if (auto ui = op["ui"]) {
        return {nss.db().toString(), uassertStatusOK(UUID::parse(ui))};
    }
    return nss;
}

/**
 * Used for logging a report of ops that take longer than "slowMS" to apply. This is called
 * right before returning from syncApply, and it returns the same status.
 */
Status finishAndLogApply(ClockSource* clockSource,
                         Status finalStatus,
                         Date_t applyStartTime,
                         OpTypeEnum opType,
                         const BSONObj& op) {

    if (finalStatus.isOK()) {
        auto applyEndTime = clockSource->now();
        auto diffMS = durationCount<Milliseconds>(applyEndTime - applyStartTime);

        // This op was slow to apply, so we should log a report of it.
        if (diffMS > serverGlobalParams.slowMS) {

            StringBuilder s;
            s << "applied op: ";

            if (opType == OpTypeEnum::kCommand) {
                s << "command ";
            } else {
                s << "CRUD ";
            }

            s << redact(op);
            s << ", took " << diffMS << "ms";

            log() << s.str();
        }
    }
    return finalStatus;
}

LockMode fixLockModeForSystemDotViewsChanges(const NamespaceString& nss, LockMode mode) {
    return nss.isSystemDotViews() ? MODE_X : mode;
}

}  // namespace

// static
Status SyncTail::syncApply(OperationContext* opCtx,
                           const BSONObj& op,
                           OplogApplication::Mode oplogApplicationMode,
                           boost::optional<Timestamp> stableTimestampForRecovery) {
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

    auto clockSource = opCtx->getServiceContext()->getFastClockSource();
    auto applyStartTime = clockSource->now();

    if (MONGO_FAIL_POINT(hangAfterRecordingOpApplicationStartTime)) {
        log() << "syncApply - fail point hangAfterRecordingOpApplicationStartTime enabled. "
              << "Blocking until fail point is disabled. ";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterRecordingOpApplicationStartTime);
    }

    auto opType = OpType_parse(IDLParserErrorContext("syncApply"), op["op"].valuestrsafe());

    auto finishApply = [&](Status status) {
        return finishAndLogApply(clockSource, status, applyStartTime, opType, op);
    };

    if (opType == OpTypeEnum::kNoop) {
        incrementOpsAppliedStats();
        return Status::OK();
    } else if (OplogEntry::isCrudOpType(opType)) {
        return finishApply(writeConflictRetry(opCtx, "syncApply_CRUD", nss.ns(), [&] {
            // Need to throw instead of returning a status for it to be properly ignored.
            try {
                AutoGetCollection autoColl(
                    opCtx, getNsOrUUID(nss, op), fixLockModeForSystemDotViewsChanges(nss, MODE_IX));
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
        }));
    } else if (opType == OpTypeEnum::kCommand) {
        return finishApply(writeConflictRetry(opCtx, "syncApply_command", nss.ns(), [&] {
            // TODO SERVER-37180 Remove this double-parsing.
            // The command entry has been parsed before, so it must be valid.
            auto entry = uassertStatusOK(OplogEntry::parse(op));

            // A special case apply for commands to avoid implicit database creation.
            Status status = applyCommand_inlock(
                opCtx, op, entry, oplogApplicationMode, stableTimestampForRecovery);
            incrementOpsAppliedStats();
            return status;
        }));
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

SyncTail::~SyncTail() {}

const OplogApplier::Options& SyncTail::getOptions() const {
    return _options;
}

namespace {

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
        return [storageInterface, &ops, begin, end](auto status) {
            invariant(status);

            auto opCtx = cc().makeOperationContext();

            // This code path is only executed on secondaries and initial syncing nodes, so it is
            // safe to exclude any writes from Flow Control.
            opCtx->setShouldParticipateInFlowControl(false);

            UnreplicatedWritesBlock uwb(opCtx.get());
            ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
                opCtx->lockState());

            std::vector<InsertStatement> docs;
            docs.reserve(end - begin);
            for (size_t i = begin; i < end; i++) {
                // Add as unowned BSON to avoid unnecessary ref-count bumps.
                // 'ops' will outlive 'docs' so the BSON lifetime will be guaranteed.
                docs.emplace_back(InsertStatement{ops[i].getRaw(),
                                                  ops[i].getOpTime().getTimestamp(),
                                                  ops[i].getOpTime().getTerm()});
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

        threadPool->schedule(makeOplogWriterForRange(0, ops.size()));
        return;
    }


    const size_t numOplogThreads = threadPool->getStats().numThreads;
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
                                                 const StringMapHashedKey& ns) {
        auto it = _cache.find(ns);
        if (it != _cache.end()) {
            return it->second;
        }

        auto collProperties = getCollectionPropertiesImpl(opCtx, NamespaceString(ns.key()));
        _cache[ns] = collProperties;
        return collProperties;
    }

private:
    CollectionProperties getCollectionPropertiesImpl(OperationContext* opCtx,
                                                     const NamespaceString& nss) {
        CollectionProperties collProperties;

        Lock::DBLock dbLock(opCtx, nss.db(), MODE_IS);
        auto databaseHolder = DatabaseHolder::get(opCtx);
        auto db = databaseHolder->getDb(opCtx, nss.db());
        if (!db) {
            return collProperties;
        }

        auto collection = db->getCollection(opCtx, nss);
        if (!collection) {
            return collProperties;
        }

        collProperties.isCapped = collection->isCapped();
        collProperties.collator = collection->getDefaultCollator();
        return collProperties;
    }

    StringMap<CollectionProperties> _cache;
};

void tryToGoLiveAsASecondary(OperationContext* opCtx,
                             ReplicationCoordinator* replCoord,
                             OpTime minValid) {
    // Check to see if we can immediately return without taking any locks.
    if (replCoord->isInPrimaryOrSecondaryState_UNSAFE()) {
        return;
    }

    // This needs to happen after the attempt so readers can be sure we've already tried.
    ON_BLOCK_EXIT([] { attemptsToBecomeSecondary.increment(); });

    // Need the RSTL in mode X to transition to SECONDARY
    ReplicationStateTransitionLockGuard transitionGuard(opCtx, MODE_X);

    // Check if we are primary or secondary again now that we have the RSTL in mode X.
    if (replCoord->isInPrimaryOrSecondaryState(opCtx)) {
        return;
    }

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

}  // namespace

class SyncTail::OpQueueBatcher {
    OpQueueBatcher(const OpQueueBatcher&) = delete;
    OpQueueBatcher& operator=(const OpQueueBatcher&) = delete;

public:
    OpQueueBatcher(SyncTail* syncTail,
                   StorageInterface* storageInterface,
                   OplogBuffer* oplogBuffer,
                   OplogApplier::GetNextApplierBatchFn getNextApplierBatchFn)
        : _syncTail(syncTail),
          _storageInterface(storageInterface),
          _oplogBuffer(oplogBuffer),
          _getNextApplierBatchFn(getNextApplierBatchFn),
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

        while (true) {
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(rsSyncApplyStop);

            batchLimits.slaveDelayLatestTimestamp = _calculateSlaveDelayLatestTimestamp();

            // Check the limits once per batch since users can change them at runtime.
            batchLimits.ops = OplogApplier::getBatchLimitOperations();
            batchLimits.bytes = OplogApplier::calculateBatchLimitBytes(
                cc().makeOperationContext().get(), _storageInterface);

            OpQueue ops(batchLimits.ops);
            {
                auto opCtx = cc().makeOperationContext();

                // This use of UninterruptibleLockGuard is intentional. It is undesirable to use an
                // UninterruptibleLockGuard in client operations because stepdown requires the
                // ability to interrupt client operations. However, it is acceptable to use an
                // UninterruptibleLockGuard in batch application because the only cause of
                // interruption would be shutdown, and the ReplBatcher thread has its own shutdown
                // handling.
                UninterruptibleLockGuard noInterrupt(opCtx->lockState());

                auto oplogEntries =
                    fassertNoTrace(31004, _getNextApplierBatchFn(opCtx.get(), batchLimits));
                for (const auto& oplogEntry : oplogEntries) {
                    ops.emplace_back(oplogEntry.getRaw());
                }

                // If we don't have anything in the queue, wait a bit for something to appear.
                if (oplogEntries.empty()) {
                    if (_syncTail->inShutdown()) {
                        ops.setMustShutdownFlag();
                    } else {
                        // Block up to 1 second. We still return true in this case because we want
                        // this op to be the first in a new batch with a new start time.
                        _oplogBuffer->waitForData(Seconds(1));
                    }
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
    OplogApplier::GetNextApplierBatchFn const _getNextApplierBatchFn;

    stdx::mutex _mutex;  // Guards _ops.
    stdx::condition_variable _cv;
    OpQueue _ops;

    // This only exists so the destructor invariants rather than deadlocking.
    // TODO remove once we trust noexcept enough to mark oplogApplication() as noexcept.
    bool _isDead = false;

    stdx::thread _thread;  // Must be last so all other members are initialized before starting.
};

void SyncTail::oplogApplication(OplogBuffer* oplogBuffer,
                                OplogApplier::GetNextApplierBatchFn getNextApplierBatchFn,
                                ReplicationCoordinator* replCoord) {
    // We don't start data replication for arbiters at all and it's not allowed to reconfig
    // arbiterOnly field for any member.
    invariant(!replCoord->getMemberState().arbiter());

    OpQueueBatcher batcher(this, _storageInterface, oplogBuffer, getNextApplierBatchFn);

    _oplogApplication(replCoord, &batcher);
}

void SyncTail::_oplogApplication(ReplicationCoordinator* replCoord,
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

        // This code path gets used during elections, so it should not be subject to Flow Control.
        // It is safe to exclude this operation context from Flow Control here because this code
        // path only gets used on secondaries or on a node transitioning to primary.
        opCtx.setShouldParticipateInFlowControl(false);

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
        const auto lastOpInBatch = ops.back();
        const auto lastOpTimeInBatch = lastOpInBatch.getOpTime();
        const auto lastWallTimeInBatch = lastOpInBatch.getWallClockTime();
        const auto lastAppliedOpTimeAtStartOfBatch = replCoord->getMyLastAppliedOpTime();

        // Make sure the oplog doesn't go back in time or repeat an entry.
        if (firstOpTimeInBatch <= lastAppliedOpTimeAtStartOfBatch) {
            fassert(34361,
                    Status(ErrorCodes::OplogOutOfOrder,
                           str::stream() << "Attempted to apply an oplog entry ("
                                         << firstOpTimeInBatch.toString()
                                         << ") which is not greater than our last applied OpTime ("
                                         << lastAppliedOpTimeAtStartOfBatch.toString() << ")."));
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

        // Update various things that care about our last applied optime. Tests rely on 1 happening
        // before 2 even though it isn't strictly necessary.

        // 1. Persist our "applied through" optime to disk.
        _consistencyMarkers->setAppliedThrough(&opCtx, lastOpTimeInBatch);

        // 2. Ensure that the last applied op time hasn't changed since the start of this batch.
        const auto lastAppliedOpTimeAtEndOfBatch = replCoord->getMyLastAppliedOpTime();
        invariant(lastAppliedOpTimeAtStartOfBatch == lastAppliedOpTimeAtEndOfBatch,
                  str::stream() << "the last known applied OpTime has changed from "
                                << lastAppliedOpTimeAtStartOfBatch.toString() << " to "
                                << lastAppliedOpTimeAtEndOfBatch.toString()
                                << " in the middle of batch application");

        // 3. Update oplog visibility by notifying the storage engine of the new oplog entries.
        const bool orderedCommit = true;
        _storageInterface->oplogDiskLocRegister(
            &opCtx, lastOpTimeInBatch.getTimestamp(), orderedCommit);

        // 4. Finalize this batch. We are at a consistent optime if our current optime is >= the
        // current 'minValid' optime. Note that recording the lastOpTime in the finalizer includes
        // advancing the global timestamp to at least its timestamp.
        auto consistency = (lastOpTimeInBatch >= minValid)
            ? ReplicationCoordinator::DataConsistency::Consistent
            : ReplicationCoordinator::DataConsistency::Inconsistent;
        // Wall clock time is non-optional post 3.6.
        invariant(lastWallTimeInBatch);
        finalizer->record({lastOpTimeInBatch, lastWallTimeInBatch.get()}, consistency);
    }
}

// Returns whether an oplog entry represents an applyOps which is a self-contained atomic operation,
// or the last applyOps of an unprepared transaction, as opposed to part of a prepared transaction
// or a non-final applyOps in an transaction.
inline bool isCommitApplyOps(const OplogEntry& entry) {
    return entry.getCommandType() == OplogEntry::CommandType::kApplyOps && !entry.shouldPrepare() &&
        !entry.isPartialTransaction() && !entry.getObject().getBoolField("prepare");
}

// Returns whether a commitTransaction oplog entry is a part of a prepared transaction.
inline bool isPreparedCommit(const OplogEntry& entry) {
    return entry.getCommandType() == OplogEntry::CommandType::kCommitTransaction;
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
                          << "sync source, attempt " << retryCount << " of " << retryMax;
                continue;  // try again
            }
        } catch (const NetworkException&) {
            warning() << "network problem detected while connecting to the "
                      << "sync source, attempt " << retryCount << " of " << retryMax;
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
        auto nss = oplogEntry.getNss();
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
                      << "sync source, attempt " << retryCount << " of " << retryMax;
            continue;  // try again
        } catch (DBException& e) {
            error() << "assertion fetching missing object: " << redact(e);
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
            auto& catalog = CollectionCatalog::get(opCtx);
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
    // Since we swap the locker in stash / unstash transaction resources,
    // ShouldNotConflictWithSecondaryBatchApplicationBlock will touch the locker that has been
    // destroyed by unstash in its destructor. Thus we set the flag explicitly.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

    // Explicitly start future read transactions without a timestamp.
    opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);

    // When querying indexes, we return the record matching the key if it exists, or an adjacent
    // document. This means that it is possible for us to hit a prepare conflict if we query for an
    // incomplete key and an adjacent key is prepared.
    // We ignore prepare conflicts on secondaries because they may encounter prepare conflicts that
    // did not occur on the primary.
    opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    ApplierHelpers::stableSortByNamespace(ops);

    const auto oplogApplicationMode = st->getOptions().mode;

    ApplierHelpers::InsertGroup insertGroup(ops, opCtx, oplogApplicationMode);

    {  // Ensure that the MultikeyPathTracker stops tracking paths.
        ON_BLOCK_EXIT([opCtx] { MultikeyPathTracker::get(opCtx).stopTrackingMultikeyPathInfo(); });
        MultikeyPathTracker::get(opCtx).startTrackingMultikeyPathInfo();

        for (auto it = ops->cbegin(); it != ops->cend(); ++it) {
            const OplogEntry& entry = **it;

            // If we are successful in grouping and applying inserts, advance the current iterator
            // past the end of the inserted group of entries.
            auto groupResult = insertGroup.groupAndApplyInserts(it);
            if (groupResult.isOK()) {
                it = groupResult.getValue();
                continue;
            }

            // If we didn't create a group, try to apply the op individually.
            try {
                auto stableTimestampForRecovery = st->getOptions().stableTimestampForRecovery;
                const Status status = SyncTail::syncApply(
                    opCtx, entry.getRaw(), oplogApplicationMode, stableTimestampForRecovery);

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

/**
 * ops - This only modifies the isForCappedCollection field on each op. It does not alter the ops
 *      vector in any other way.
 * writerVectors - Set of operations for each worker thread to apply.
 * derivedOps - If provided, this function inserts a decomposition of applyOps operations
 *      and instructions for updating the transactions table.  Required if processing oplogs
 *      with transactions.
 * sessionUpdateTracker - if provided, keeps track of session info from ops.
 */
void SyncTail::_fillWriterVectors(OperationContext* opCtx,
                                  MultiApplier::Operations* ops,
                                  std::vector<MultiApplier::OperationPtrs>* writerVectors,
                                  std::vector<MultiApplier::Operations>* derivedOps,
                                  SessionUpdateTracker* sessionUpdateTracker) {
    const auto serviceContext = opCtx->getServiceContext();
    const auto storageEngine = serviceContext->getStorageEngine();

    const bool supportsDocLocking = storageEngine->supportsDocLocking();
    const uint32_t numWriters = writerVectors->size();

    CachedCollectionProperties collPropertiesCache;
    LogicalSessionIdMap<std::vector<OplogEntry*>> partialTxnOps;

    for (auto&& op : *ops) {
        // If the operation's optime is before or the same as the beginApplyingOpTime we don't want
        // to apply it, so don't include it in writerVectors.
        if (op.getOpTime() <= _options.beginApplyingOpTime) {
            continue;
        }

        auto hashedNs = StringMapHasher().hashed_key(op.getNss().ns());
        // Reduce the hash from 64bit down to 32bit, just to allow combinations with murmur3 later
        // on. Bit depth not important, we end up just doing integer modulo with this in the end.
        // The hash function should provide entropy in the lower bits as it's used in hash tables.
        uint32_t hash = static_cast<uint32_t>(hashedNs.hash());

        // We need to track all types of ops, including type 'n' (these are generated from chunk
        // migrations).
        if (sessionUpdateTracker) {
            if (auto newOplogWrites = sessionUpdateTracker->updateSession(op)) {
                derivedOps->emplace_back(std::move(*newOplogWrites));
                _fillWriterVectors(opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
            }
        }

        // If this entry is part of a multi-oplog-entry transaction, ignore it until the commit.
        // We must save it here because we are not guaranteed it has been written to the oplog
        // yet.
        if (op.isPartialTransaction()) {
            auto& partialTxnList = partialTxnOps[*op.getSessionId()];
            // If this operation belongs to an existing partial transaction, partialTxnList
            // must contain the previous operations of the transaction.
            invariant(partialTxnList.empty() ||
                      partialTxnList.front()->getTxnNumber() == op.getTxnNumber());
            partialTxnList.push_back(&op);
            continue;
        }

        if (op.getCommandType() == OplogEntry::CommandType::kAbortTransaction) {
            auto& partialTxnList = partialTxnOps[*op.getSessionId()];
            partialTxnList.clear();
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
        if (isCommitApplyOps(op)) {
            try {
                auto logicalSessionId = op.getSessionId();
                // applyOps entries generated by a transaction must have a sessionId and a
                // transaction number.
                if (logicalSessionId && op.getTxnNumber()) {
                    // On commit of unprepared transactions, get transactional operations from the
                    // oplog and fill writers with those operations.
                    // Flush partialTxnList operations for current transaction.
                    auto& partialTxnList = partialTxnOps[*logicalSessionId];
                    {
                        // We need to use a ReadSourceScope avoid the reads of the transaction
                        // messing up the state of the opCtx.  In particular we do not want to
                        // set the ReadSource to kLastApplied.
                        ReadSourceScope readSourceScope(opCtx);
                        derivedOps->emplace_back(readTransactionOperationsFromOplogChain(
                            opCtx, op, partialTxnList, boost::none));
                        partialTxnList.clear();
                    }
                    // Transaction entries cannot have different session updates.
                    _fillWriterVectors(
                        opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
                } else {
                    // The applyOps entry was not generated as part of a transaction.
                    invariant(!op.getPrevWriteOpTimeInTransaction());
                    derivedOps->emplace_back(ApplyOps::extractOperations(op));

                    // Nested entries cannot have different session updates.
                    _fillWriterVectors(
                        opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
                }
            } catch (...) {
                fassertFailedWithStatusNoTrace(
                    50711,
                    exceptionToStatus().withContext(str::stream()
                                                    << "Unable to extract operations from applyOps "
                                                    << redact(op.toBSON())));
            }
            continue;
        }

        // If we see a commitTransaction command that is a part of a prepared transaction during
        // initial sync, find the prepare oplog entry, extract applyOps operations, and fill writers
        // with the extracted operations.
        if (isPreparedCommit(op) && (_options.mode == OplogApplication::Mode::kInitialSync)) {
            auto logicalSessionId = op.getSessionId();
            auto& partialTxnList = partialTxnOps[*logicalSessionId];

            {
                // Traverse the oplog chain with its own snapshot and read timestamp.
                ReadSourceScope readSourceScope(opCtx);

                // Get the previous oplog entry, which should be a prepare oplog entry.
                const auto prevOplogEntry = getPreviousOplogEntry(opCtx, op);
                invariant(prevOplogEntry.shouldPrepare());

                // Extract the operations from the applyOps entry.
                auto commitOplogEntryOpTime = op.getOpTime();
                derivedOps->emplace_back(readTransactionOperationsFromOplogChain(
                    opCtx, prevOplogEntry, partialTxnList, commitOplogEntryOpTime.getTimestamp()));
                partialTxnList.clear();
            }

            _fillWriterVectors(opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
            continue;
        }

        auto& writer = (*writerVectors)[hash % numWriters];
        if (writer.empty()) {
            writer.reserve(8);  // Skip a few growth rounds
        }
        writer.push_back(&op);
    }
}

void SyncTail::fillWriterVectors(OperationContext* opCtx,
                                 MultiApplier::Operations* ops,
                                 std::vector<MultiApplier::OperationPtrs>* writerVectors,
                                 std::vector<MultiApplier::Operations>* derivedOps) {
    SessionUpdateTracker sessionUpdateTracker;
    _fillWriterVectors(opCtx, ops, writerVectors, derivedOps, &sessionUpdateTracker);

    auto newOplogWrites = sessionUpdateTracker.flushAll();
    if (!newOplogWrites.empty()) {
        derivedOps->emplace_back(std::move(newOplogWrites));
        _fillWriterVectors(opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
    }
}

void SyncTail::_applyOps(std::vector<MultiApplier::OperationPtrs>& writerVectors,
                         std::vector<Status>* statusVector,
                         std::vector<WorkerMultikeyPathInfo>* workerMultikeyPathInfo) {
    invariant(writerVectors.size() == statusVector->size());
    for (size_t i = 0; i < writerVectors.size(); i++) {
        if (writerVectors[i].empty())
            continue;

        _writerPool->schedule(
            [this,
             &writer = writerVectors.at(i),
             &status = statusVector->at(i),
             &workerMultikeyPathInfo = workerMultikeyPathInfo->at(i)](auto scheduleStatus) {
                invariant(scheduleStatus);

                auto opCtx = cc().makeOperationContext();

                // This code path is only executed on secondaries and initial syncing nodes, so it
                // is safe to exclude any writes from Flow Control.
                opCtx->setShouldParticipateInFlowControl(false);

                status = opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
                    return _applyFunc(opCtx.get(), &writer, this, &workerMultikeyPathInfo);
                });
            });
    }
}

StatusWith<OpTime> SyncTail::multiApply(OperationContext* opCtx, MultiApplier::Operations ops) {
    invariant(!ops.empty());

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

    // Increment the batch size stat.
    oplogApplicationBatchSize.increment(ops.size());

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

        // Use this fail point to hold the PBWM lock after we have written the oplog entries but
        // before we have applied them.
        if (MONGO_FAIL_POINT(pauseBatchApplicationAfterWritingOplogEntries)) {
            log() << "pauseBatchApplicationAfterWritingOplogEntries fail point enabled. Blocking "
                     "until fail point is disabled.";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
                opCtx, pauseBatchApplicationAfterWritingOplogEntries);
        }

        // Reset consistency markers in case the node fails while applying ops.
        if (!_options.skipWritesToOplog) {
            _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
            _consistencyMarkers->setMinValidToAtLeast(opCtx, ops.back().getOpTime());
        }

        {
            std::vector<Status> statusVector(_writerPool->getStats().numThreads, Status::OK());
            _applyOps(writerVectors, &statusVector, &multikeyVector);
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
    }

    // Notify the storage engine that a replication batch has completed. This means that all the
    // writes associated with the oplog entries in the batch are finished and no new writes with
    // timestamps associated with those oplog entries will show up in the future.
    const auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    storageEngine->replicationBatchIsComplete();

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
    // parallel batch writer mode lock.
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
