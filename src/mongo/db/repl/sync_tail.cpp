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
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/insert_group.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_auth.h"
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

// Number and time of each ApplyOps worker pool round
TimerStats applyBatchStats;
ServerStatusMetricField<TimerStats> displayOpBatchesApplied("repl.apply.batches", &applyBatchStats);

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

NamespaceStringOrUUID getNsOrUUID(const NamespaceString& nss, const OplogEntry& op) {
    if (auto ui = op.getUuid()) {
        return {nss.db().toString(), ui.get()};
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
                         const OplogEntryBatch& batch) {

    if (finalStatus.isOK()) {
        auto applyEndTime = clockSource->now();
        auto diffMS = durationCount<Milliseconds>(applyEndTime - applyStartTime);

        // This op was slow to apply, so we should log a report of it.
        if (diffMS > serverGlobalParams.slowMS) {

            StringBuilder s;
            s << "applied op: ";

            if (batch.getOp().getOpType() == OpTypeEnum::kCommand) {
                s << "command ";
            } else {
                s << "CRUD ";
            }

            s << redact(batch.toBSON());
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

/**
 * Updates a CRUD op's hash and isForCappedCollection field if necessary.
 */
void processCrudOp(OperationContext* opCtx,
                   OplogEntry* op,
                   uint32_t* hash,
                   StringMapHashedKey* hashedNs,
                   CachedCollectionProperties* collPropertiesCache) {
    const bool supportsDocLocking =
        opCtx->getServiceContext()->getStorageEngine()->supportsDocLocking();
    auto collProperties = collPropertiesCache->getCollectionProperties(opCtx, *hashedNs);

    // For doc locking engines, include the _id of the document in the hash so we get
    // parallelism even if all writes are to a single collection.
    //
    // For capped collections, this is illegal, since capped collections must preserve
    // insertion order.
    if (supportsDocLocking && !collProperties.isCapped) {
        BSONElement id = op->getIdElement();
        BSONElementComparator elementHasher(BSONElementComparator::FieldNamesMode::kIgnore,
                                            collProperties.collator);
        const size_t idHash = elementHasher.hash(id);
        MurmurHash3_x86_32(&idHash, sizeof(idHash), *hash, hash);
    }

    if (op->getOpType() == OpTypeEnum::kInsert && collProperties.isCapped) {
        // Mark capped collection ops before storing them to ensure we do not attempt to
        // bulk insert them.
        op->isForCappedCollection = true;
    }
}

/**
 * Adds a single oplog entry to the appropriate writer vector.
 */
void addToWriterVector(OplogEntry* op,
                       std::vector<MultiApplier::OperationPtrs>* writerVectors,
                       uint32_t hash) {
    const uint32_t numWriters = writerVectors->size();
    auto& writer = (*writerVectors)[hash % numWriters];
    if (writer.empty()) {
        writer.reserve(8);  // Skip a few growth rounds
    }
    writer.push_back(op);
}

/**
 * Adds a set of derivedOps to writerVectors.
 */
void addDerivedOps(OperationContext* opCtx,
                   MultiApplier::Operations* derivedOps,
                   std::vector<MultiApplier::OperationPtrs>* writerVectors,
                   CachedCollectionProperties* collPropertiesCache) {
    for (auto&& op : *derivedOps) {
        auto hashedNs = StringMapHasher().hashed_key(op.getNss().ns());
        uint32_t hash = static_cast<uint32_t>(hashedNs.hash());
        if (op.isCrudOpType()) {
            processCrudOp(opCtx, &op, &hash, &hashedNs, collPropertiesCache);
        }
        addToWriterVector(&op, writerVectors, hash);
    }
}

}  // namespace

void SyncTail::shutdown() {
    stdx::lock_guard<Latch> lock(_mutex);
    _inShutdown = true;
}

bool SyncTail::inShutdown() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _inShutdown;
}

Status syncApply(OperationContext* opCtx,
                 const OplogEntryBatch& batch,
                 OplogApplication::Mode oplogApplicationMode) {
    // Guarantees that syncApply's context matches that of its calling function, multiSyncApply.
    invariant(!opCtx->writesAreReplicated());
    invariant(documentValidationDisabled(opCtx));

    auto op = batch.getOp();
    // Count each log op application as a separate operation, for reporting purposes
    CurOp individualOp(opCtx);

    const NamespaceString nss(op.getNss());

    auto incrementOpsAppliedStats = [] { opsAppliedStats.increment(1); };

    auto applyOp = [&](Database* db) {
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
            opCtx, db, batch, shouldAlwaysUpsert, oplogApplicationMode, incrementOpsAppliedStats);
        if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
            throw WriteConflictException();
        }
        return status;
    };

    auto clockSource = opCtx->getServiceContext()->getFastClockSource();
    auto applyStartTime = clockSource->now();

    if (MONGO_unlikely(hangAfterRecordingOpApplicationStartTime.shouldFail())) {
        log() << "syncApply - fail point hangAfterRecordingOpApplicationStartTime enabled. "
              << "Blocking until fail point is disabled. ";
        hangAfterRecordingOpApplicationStartTime.pauseWhileSet();
    }

    auto opType = op.getOpType();

    auto finishApply = [&](Status status) {
        return finishAndLogApply(clockSource, status, applyStartTime, batch);
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
                // Delete operations on non-existent namespaces can be treated as successful for
                // idempotency reasons.
                // During RECOVERING mode, we ignore NamespaceNotFound for all CRUD ops since
                // storage does not wait for drops to be checkpointed (SERVER-33161).
                if (opType == OpTypeEnum::kDelete ||
                    oplogApplicationMode == OplogApplication::Mode::kRecovering) {
                    return Status::OK();
                }

                ex.addContext(str::stream()
                              << "Failed to apply operation: " << redact(batch.toBSON()));
                throw;
            }
        }));
    } else if (opType == OpTypeEnum::kCommand) {
        return finishApply(writeConflictRetry(opCtx, "syncApply_command", nss.ns(), [&] {
            // A special case apply for commands to avoid implicit database creation.
            Status status = applyCommand_inlock(opCtx, op, oplogApplicationMode);
            incrementOpsAppliedStats();
            return status;
        }));
    }

    MONGO_UNREACHABLE;
}

void stableSortByNamespace(MultiApplier::OperationPtrs* oplogEntryPointers) {
    if (oplogEntryPointers->size() < 1U) {
        return;
    }
    auto nssComparator = [](const OplogEntry* l, const OplogEntry* r) {
        return l->getNss() < r->getNss();
    };
    std::stable_sort(oplogEntryPointers->begin(), oplogEntryPointers->end(), nssComparator);
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

    stableSortByNamespace(ops);

    const auto oplogApplicationMode = st->getOptions().mode;

    InsertGroup insertGroup(ops, opCtx, oplogApplicationMode);

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
                const Status status = syncApply(opCtx, &entry, oplogApplicationMode);

                if (!status.isOK()) {
                    // Tried to apply an update operation but the document is missing, there must be
                    // a delete operation for the document later in the oplog.
                    if (status == ErrorCodes::UpdateOperationFailed &&
                        oplogApplicationMode == OplogApplication::Mode::kInitialSync) {
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
void SyncTail::_deriveOpsAndFillWriterVectors(
    OperationContext* opCtx,
    MultiApplier::Operations* ops,
    std::vector<MultiApplier::OperationPtrs>* writerVectors,
    std::vector<MultiApplier::Operations>* derivedOps,
    SessionUpdateTracker* sessionUpdateTracker) noexcept {

    LogicalSessionIdMap<std::vector<OplogEntry*>> partialTxnOps;
    CachedCollectionProperties collPropertiesCache;
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
                addDerivedOps(opCtx, &derivedOps->back(), writerVectors, &collPropertiesCache);
            }
        }


        // If this entry is part of a multi-oplog-entry transaction, ignore it until the commit.
        // We must save it here because we are not guaranteed it has been written to the oplog
        // yet.
        // We also do this for prepare during initial sync.
        if (op.isPartialTransaction() ||
            (op.shouldPrepare() && _options.mode == OplogApplication::Mode::kInitialSync)) {
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

        if (op.isCrudOpType())
            processCrudOp(opCtx, &op, &hash, &hashedNs, &collPropertiesCache);
        // Extract applyOps operations and fill writers with extracted operations using this
        // function.
        if (op.isTerminalApplyOps()) {
            auto logicalSessionId = op.getSessionId();
            // applyOps entries generated by a transaction must have a sessionId and a
            // transaction number.
            if (logicalSessionId && op.getTxnNumber()) {
                // On commit of unprepared transactions, get transactional operations from the
                // oplog and fill writers with those operations.
                // Flush partialTxnList operations for current transaction.
                auto& partialTxnList = partialTxnOps[*logicalSessionId];

                derivedOps->emplace_back(
                    readTransactionOperationsFromOplogChain(opCtx, op, partialTxnList));
                partialTxnList.clear();

                // Transaction entries cannot have different session updates.
                addDerivedOps(opCtx, &derivedOps->back(), writerVectors, &collPropertiesCache);
            } else {
                // The applyOps entry was not generated as part of a transaction.
                invariant(!op.getPrevWriteOpTimeInTransaction());

                derivedOps->emplace_back(ApplyOps::extractOperations(op));

                // Nested entries cannot have different session updates.
                addDerivedOps(opCtx, &derivedOps->back(), writerVectors, &collPropertiesCache);
            }
            continue;
        }

        // If we see a commitTransaction command that is a part of a prepared transaction during
        // initial sync, find the prepare oplog entry, extract applyOps operations, and fill writers
        // with the extracted operations.
        if (op.isPreparedCommit() && (_options.mode == OplogApplication::Mode::kInitialSync)) {
            auto logicalSessionId = op.getSessionId();
            auto& partialTxnList = partialTxnOps[*logicalSessionId];

            derivedOps->emplace_back(
                readTransactionOperationsFromOplogChain(opCtx, op, partialTxnList));
            partialTxnList.clear();

            addDerivedOps(opCtx, &derivedOps->back(), writerVectors, &collPropertiesCache);
            continue;
        }

        addToWriterVector(&op, writerVectors, hash);
    }
}

void SyncTail::fillWriterVectors(OperationContext* opCtx,
                                 MultiApplier::Operations* ops,
                                 std::vector<MultiApplier::OperationPtrs>* writerVectors,
                                 std::vector<MultiApplier::Operations>* derivedOps) noexcept {

    SessionUpdateTracker sessionUpdateTracker;
    _deriveOpsAndFillWriterVectors(opCtx, ops, writerVectors, derivedOps, &sessionUpdateTracker);

    auto newOplogWrites = sessionUpdateTracker.flushAll();
    if (!newOplogWrites.empty()) {
        derivedOps->emplace_back(std::move(newOplogWrites));
        _deriveOpsAndFillWriterVectors(
            opCtx, &derivedOps->back(), writerVectors, derivedOps, nullptr);
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
        if (MONGO_unlikely(pauseBatchApplicationAfterWritingOplogEntries.shouldFail())) {
            log() << "pauseBatchApplicationAfterWritingOplogEntries fail point enabled. Blocking "
                     "until fail point is disabled.";
            pauseBatchApplicationAfterWritingOplogEntries.pauseWhileSet(opCtx);
        }

        // Reset consistency markers in case the node fails while applying ops.
        if (!_options.skipWritesToOplog) {
            _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
            _consistencyMarkers->setMinValidToAtLeast(opCtx, ops.back().getOpTime());
        }

        {
            std::vector<Status> statusVector(_writerPool->getStats().numThreads, Status::OK());

            // Doles out all the work to the writer pool threads. writerVectors is not modified,
            // but  multiSyncApply will modify the vectors that it contains.
            invariant(writerVectors.size() == statusVector.size());
            for (size_t i = 0; i < writerVectors.size(); i++) {
                if (writerVectors[i].empty())
                    continue;

                _writerPool->schedule(
                    [this,
                     &writer = writerVectors.at(i),
                     &status = statusVector.at(i),
                     &multikeyVector = multikeyVector.at(i)](auto scheduleStatus) {
                        invariant(scheduleStatus);

                        auto opCtx = cc().makeOperationContext();

                        // This code path is only executed on secondaries and initial syncing nodes,
                        // so it is safe to exclude any writes from Flow Control.
                        opCtx->setShouldParticipateInFlowControl(false);

                        status = opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
                            return _applyFunc(opCtx.get(), &writer, this, &multikeyVector);
                        });
                    });
            }

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
    if (MONGO_unlikely(pauseBatchApplicationBeforeCompletion.shouldFail())) {
        log() << "pauseBatchApplicationBeforeCompletion fail point enabled. Blocking until fail "
                 "point is disabled.";
        while (MONGO_unlikely(pauseBatchApplicationBeforeCompletion.shouldFail())) {
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

    // Increment the counter for the number of ops applied during catchup if the node is in catchup
    // mode.
    replCoord->incrementNumCatchUpOpsIfCatchingUp(ops.size());

    // We have now written all database writes and updated the oplog to match.
    return ops.back().getOpTime();
}

}  // namespace repl
}  // namespace mongo
